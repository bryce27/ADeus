#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <iostream>
#include <alsa/asoundlib.h>
#include <curl/curl.h>
#include <fstream>
#include "cxxopts.hpp"
#include <chrono>
#include <filesystem>
#include <atomic>
#include <csignal>

// Optional Bluetooth support (compile with -DENABLE_BLUETOOTH and link libbluetooth)
#ifdef ENABLE_BLUETOOTH
#include "bluetooth/ble_server.h"
#include "bluetooth/ble_protocol.h"
#endif

// Voice Activity Detection
#include "vad/voice_activity_detector.h"

#define DO_NOT_APPLY_GAIN 1.0

// Configuration
struct Config {
    std::string audioDevice = "plughw:1,0";  // Default to USB mic
    int bytesPerSample = 4;
    short channels = 1;
    unsigned int sampleRate = 44100;
    int durationInSeconds = 60;
    float audioGain = DO_NOT_APPLY_GAIN;
    bool saveToLocalFile = false;
    bool useBluetooth = false;
    bool verbose = false;
    int retentionDays = 7;  // Keep audio files for N days
    
    // Voice Activity Detection
    bool vadEnabled = true;
    float vadThreshold = 0.02f;
    int vadHangoverMs = 1500;  // Keep recording for 1.5s after speech ends
};

Config config;
std::atomic<bool> running{true};
std::atomic<bool> recordingPaused{false};
int rc;

// Voice Activity Detector instance
VoiceActivityDetector vad;
std::atomic<int> vadSilenceCount{0};
std::atomic<int> vadSpeechCount{0};

// Signal handler for graceful shutdown
void signalHandler(int signum) {
    std::cout << "\nShutting down..." << std::endl;
    running = false;
}

template <typename T>
class SafeQueue
{
private:
    std::queue<T> queue;
    std::mutex mutex;
    std::condition_variable cond;

public:
    void push(T value)
    {
        std::lock_guard<std::mutex> lock(mutex);
        queue.push(value);
        cond.notify_one();
    }

    T pop()
    {
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait(lock, [this] { return !queue.empty() || !running; });
        if (!running && queue.empty()) {
            return T();  // Return empty on shutdown
        }
        T value = queue.front();
        queue.pop();
        return value;
    }

    bool empty()
    {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.empty();
    }
    
    void notify_all()
    {
        cond.notify_all();
    }
};

SafeQueue<std::vector<char>> audioQueue;

#ifdef ENABLE_BLUETOOTH
BLEServer* bleServer = nullptr;
#endif

void recordAudio(snd_pcm_t *capture_handle, snd_pcm_uframes_t period_size)
{
    int targetBytes = config.sampleRate * config.durationInSeconds * config.bytesPerSample * config.channels;
    std::vector<char> buffer(period_size * config.bytesPerSample);
    std::vector<char> accumulatedBuffer;
    std::vector<char> vadBuffer;  // Buffer audio during VAD detection
    
    // Configure VAD based on settings
    vad.setEnabled(config.vadEnabled);
    vad.setThreshold(config.vadThreshold);
    // Convert hangover from ms to frames
    int hangoverFrames = (config.vadHangoverMs * config.sampleRate) / (1000 * period_size);
    vad.setHangover(hangoverFrames);
    
    bool wasActive = false;
    int silentChunks = 0;
    const int maxSilentChunks = 3;  // Send after this many silent chunks if we have data

    while (running)
    {
        // Check if paused (from Bluetooth command)
        if (recordingPaused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        
        rc = snd_pcm_readi(capture_handle, buffer.data(), period_size);
        if (rc == -EPIPE)
        {
            std::cerr << "Overrun occurred" << std::endl;
            snd_pcm_prepare(capture_handle);
        }
        else if (rc < 0)
        {
            std::cerr << "Error from read: " << snd_strerror(rc) << std::endl;
            break;
        }
        else if (rc != (int)period_size)
        {
            if (config.verbose) {
                std::cerr << "Short read, read " << rc << " frames" << std::endl;
            }
        }
        else
        {
            size_t bytesRead = rc * config.bytesPerSample * config.channels;
            
            // Run Voice Activity Detection
            bool isActive = vad.process(buffer.data(), bytesRead, config.bytesPerSample);
            
            if (config.vadEnabled) {
                if (isActive) {
                    vadSpeechCount++;
                    silentChunks = 0;
                    
                    // Add to accumulated buffer
                    accumulatedBuffer.insert(accumulatedBuffer.end(), 
                                            buffer.begin(), buffer.begin() + bytesRead);
                    
                    if (!wasActive && config.verbose) {
                        std::cout << "VAD: Speech detected (noise floor: " 
                                  << vad.getNoiseFloor() << ")" << std::endl;
                    }
                    wasActive = true;
                } else {
                    vadSilenceCount++;
                    silentChunks++;
                    
                    // If we were active and now silent, might still want to send
                    if (wasActive) {
                        // Include a bit of trailing silence
                        if (silentChunks <= 2) {
                            accumulatedBuffer.insert(accumulatedBuffer.end(),
                                                    buffer.begin(), buffer.begin() + bytesRead);
                        }
                        
                        // After enough silence, send what we have
                        if (silentChunks >= maxSilentChunks && !accumulatedBuffer.empty()) {
                            if (config.verbose) {
                                std::cout << "VAD: Sending " << accumulatedBuffer.size() 
                                          << " bytes after speech ended" << std::endl;
                            }
                            audioQueue.push(accumulatedBuffer);
                            accumulatedBuffer.clear();
                            wasActive = false;
                        }
                    }
                }
            } else {
                // VAD disabled, accumulate everything
                accumulatedBuffer.insert(accumulatedBuffer.end(), 
                                        buffer.begin(), buffer.begin() + bytesRead);
            }

            // Send when we hit target size
            if (accumulatedBuffer.size() >= static_cast<size_t>(targetBytes))
            {
                audioQueue.push(accumulatedBuffer);
                accumulatedBuffer.clear();
                wasActive = false;
                if (config.verbose) {
                    std::cout << "Audio chunk ready for sending (" 
                              << targetBytes << " bytes)" << std::endl;
                }
            }
        }
    }
    
    // Send any remaining audio on shutdown
    if (!accumulatedBuffer.empty()) {
        audioQueue.push(accumulatedBuffer);
    }
}

void createWavHeader(std::vector<char> &header, int bitsPerSample, int dataSize)
{
    header.insert(header.end(), {'R', 'I', 'F', 'F'});
    int chunkSize = 36 + dataSize;
    auto chunkSizeBytes = reinterpret_cast<const char *>(&chunkSize);
    header.insert(header.end(), chunkSizeBytes, chunkSizeBytes + 4);
    header.insert(header.end(), {'W', 'A', 'V', 'E'});
    header.insert(header.end(), {'f', 'm', 't', ' '});
    int subchunk1Size = 16;
    auto subchunk1SizeBytes = reinterpret_cast<const char *>(&subchunk1Size);
    header.insert(header.end(), subchunk1SizeBytes, subchunk1SizeBytes + 4);
    short audioFormat = 1;
    auto audioFormatBytes = reinterpret_cast<const char *>(&audioFormat);
    header.insert(header.end(), audioFormatBytes, audioFormatBytes + 2);
    auto channelsBytes = reinterpret_cast<const char *>(&config.channels);
    header.insert(header.end(), channelsBytes, channelsBytes + 2);
    auto sampleRateBytes = reinterpret_cast<const char *>(&config.sampleRate);
    header.insert(header.end(), sampleRateBytes, sampleRateBytes + 4);
    int byteRate = config.sampleRate * config.channels * bitsPerSample / 8;
    auto byteRateBytes = reinterpret_cast<const char *>(&byteRate);
    header.insert(header.end(), byteRateBytes, byteRateBytes + 4);
    short blockAlign = config.channels * bitsPerSample / 8;
    auto blockAlignBytes = reinterpret_cast<const char *>(&blockAlign);
    header.insert(header.end(), blockAlignBytes, blockAlignBytes + 2);
    auto bitsPerSampleBytes = reinterpret_cast<const char *>(&bitsPerSample);
    header.insert(header.end(), bitsPerSampleBytes, bitsPerSampleBytes + 2);
    header.insert(header.end(), {'d', 'a', 't', 'a'});
    auto dataSizeBytes = reinterpret_cast<const char *>(&dataSize);
    header.insert(header.end(), dataSizeBytes, dataSizeBytes + 4);
}

void saveWavToFile(const std::vector<char> &buffer)
{
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::system_clock::to_time_t(now);
    std::string timestampStr = std::to_string(timestamp);
    std::filesystem::create_directory("data");
    std::string filename = "data/" + timestampStr + "_audio.wav";
    std::ofstream outfile(filename, std::ios::binary);
    if (outfile.is_open())
    {
        outfile.write(buffer.data(), buffer.size());
        outfile.close();
        std::cout << "Saved: " << filename << std::endl;
    }
}

// Delete audio files older than retentionDays
void cleanupOldAudioFiles()
{
    if (!std::filesystem::exists("data")) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto cutoffTime = now - std::chrono::hours(24 * config.retentionDays);
    int deletedCount = 0;
    
    for (const auto& entry : std::filesystem::directory_iterator("data")) {
        if (entry.is_regular_file() && entry.path().extension() == ".wav") {
            auto fileTime = std::filesystem::last_write_time(entry);
            // Convert file_time_type to system_clock time_point
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now()
            );
            
            if (sctp < cutoffTime) {
                try {
                    std::filesystem::remove(entry.path());
                    deletedCount++;
                    if (config.verbose) {
                        std::cout << "Deleted old file: " << entry.path() << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Failed to delete " << entry.path() << ": " << e.what() << std::endl;
                }
            }
        }
    }
    
    if (deletedCount > 0) {
        std::cout << "Cleanup: deleted " << deletedCount << " audio file(s) older than " 
                  << config.retentionDays << " days" << std::endl;
    }
}

// Background thread for periodic cleanup
void cleanupThread()
{
    while (running) {
        cleanupOldAudioFiles();
        // Run cleanup every hour
        for (int i = 0; i < 3600 && running; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void sendWavBufferHTTP(const std::vector<char> &buffer)
{
    const char *supabaseUrlEnv = getenv("SUPABASE_URL");
    if (!supabaseUrlEnv)
    {
        std::cerr << "Environment variable SUPABASE_URL is not set." << std::endl;
        return;
    }

    const char *authTokenEnv = getenv("AUTH_TOKEN");
    if (!authTokenEnv)
    {
        std::cerr << "Environment variable AUTH_TOKEN is not set." << std::endl;
        return;
    }

    std::string url = std::string(supabaseUrlEnv) + "/functions/v1/process-audio";
    std::string authToken = authTokenEnv;

    CURL *curl;
    CURLcode res;
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();
    
    if (curl)
    {
        struct curl_slist *headers = NULL;
        headers = curl_slist_append(headers, ("Authorization: Bearer " + authToken).c_str());
        headers = curl_slist_append(headers, "Content-Type: audio/wav");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(buffer.size()));
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buffer.data());
        
        if (config.verbose) {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }

        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        } else {
            std::cout << "Audio sent successfully" << std::endl;
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
}

#ifdef ENABLE_BLUETOOTH
void sendWavBufferBLE(const std::vector<char> &buffer)
{
    if (!bleServer || !bleServer->isConnected()) {
        std::cerr << "BLE not connected, waiting..." << std::endl;
        return;
    }
    
    // Calculate packet info for BLE transmission
    size_t mtu = bleServer->getMTU();
    size_t headerSize = sizeof(BLEProtocol::AudioPacketHeader);
    size_t dataPerPacket = mtu - headerSize;
    uint16_t totalPackets = static_cast<uint16_t>((buffer.size() + dataPerPacket - 1) / dataPerPacket);
    
    // Send raw audio data over BLE (phone app will handle WAV creation)
    if (bleServer->sendAudioData(reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size(), 0, totalPackets)) {
        std::cout << "Audio sent via BLE (" << buffer.size() << " bytes)" << std::endl;
    } else {
        std::cerr << "Failed to send audio via BLE" << std::endl;
    }
}
#endif

void sendWavBuffer(const std::vector<char> &buffer)
{
    // Always save audio files locally on the Pi
    saveWavToFile(buffer);

#ifdef ENABLE_BLUETOOTH
    if (config.useBluetooth) {
        sendWavBufferBLE(buffer);
        return;
    }
#endif
    
    sendWavBufferHTTP(buffer);
}

void handleAudioBuffer()
{
    int targetBytes = config.sampleRate * config.durationInSeconds * config.bytesPerSample * config.channels;
    
    while (running)
    {
        std::vector<char> dataChunk;
        
        while (dataChunk.size() < static_cast<size_t>(targetBytes) && running)
        {
            std::vector<char> buffer = audioQueue.pop();
            if (buffer.empty() && !running) break;
            dataChunk.insert(dataChunk.end(), buffer.begin(), buffer.end());
        }

        if (!running) break;

        if (config.audioGain != DO_NOT_APPLY_GAIN)
        {
            for (size_t i = 0; i < dataChunk.size(); i += 2)
            {
                short sample = static_cast<short>((dataChunk[i + 1] << 8) | dataChunk[i]);
                sample = static_cast<short>(std::min(std::max(-32768, static_cast<int>(config.audioGain * sample)), 32767));
                dataChunk[i] = sample & 0xFF;
                dataChunk[i + 1] = (sample >> 8) & 0xFF;
            }
        }

        if (!dataChunk.empty())
        {
            std::vector<char> wavHeader;
            int bitsPerSample = 32;
            int dataSize = dataChunk.size();
            createWavHeader(wavHeader, bitsPerSample, dataSize);

            std::vector<char> wavBuffer;
            wavBuffer.reserve(wavHeader.size() + dataSize);
            wavBuffer.insert(wavBuffer.end(), wavHeader.begin(), wavHeader.end());
            wavBuffer.insert(wavBuffer.end(), dataChunk.begin(), dataChunk.end());

            sendWavBuffer(wavBuffer);
        }
    }
}

void printBanner()
{
    std::cout << R"(
    _    ____                  
   / \  |  _ \  ___ _   _ ___ 
  / _ \ | | | |/ _ \ | | / __|
 / ___ \| |_| |  __/ |_| \__ \
/_/   \_\____/ \___|\__,_|___/
                              
)" << std::endl;
    std::cout << "ADeus Raspberry Pi Zero Audio Recorder" << std::endl;
    std::cout << "=======================================" << std::endl;
}

Config process_args(int argc, char *argv[])
{
    Config cfg;
    cxxopts::Options options("adeus", "ADeus Raspberry Pi Zero Audio Recorder");

    options.add_options()
        ("h,help", "Print help")
        ("s,save", "Save audio to local file")
        ("g,gain", "Microphone gain (volume multiplier)", cxxopts::value<float>())
        ("d,device", "ALSA audio device (default: plughw:1,0)", cxxopts::value<std::string>())
        ("r,rate", "Sample rate in Hz (default: 44100)", cxxopts::value<unsigned int>())
        ("t,duration", "Recording duration per chunk in seconds (default: 60)", cxxopts::value<int>())
        ("retention", "Days to keep audio files before auto-delete (default: 7)", cxxopts::value<int>())
        ("vad", "Enable Voice Activity Detection (default: on)", cxxopts::value<bool>()->default_value("true"))
        ("vad-threshold", "VAD sensitivity threshold 0.0-1.0 (default: 0.02)", cxxopts::value<float>())
        ("vad-hangover", "Keep recording for N ms after speech ends (default: 1500)", cxxopts::value<int>())
#ifdef ENABLE_BLUETOOTH
        ("b,bluetooth", "Use Bluetooth LE instead of WiFi")
#endif
        ("v,verbose", "Enable verbose output");

    auto result = options.parse(argc, argv);

    if (result.count("help"))
    {
        std::cout << options.help() << std::endl;
        std::cout << "\nEnvironment variables:" << std::endl;
        std::cout << "  SUPABASE_URL  - Your Supabase project URL" << std::endl;
        std::cout << "  AUTH_TOKEN    - Your Supabase anon key" << std::endl;
        std::cout << "\nExamples:" << std::endl;
        std::cout << "  ./main                           # Use defaults (USB mic, WiFi)" << std::endl;
        std::cout << "  ./main -d plughw:0,0             # Use built-in audio" << std::endl;
        std::cout << "  ./main -d plughw:1,0 -g 2.0      # USB mic with 2x gain" << std::endl;
        std::cout << "  ./main --save                    # Also save audio locally" << std::endl;
#ifdef ENABLE_BLUETOOTH
        std::cout << "  ./main --bluetooth               # Use BLE instead of WiFi" << std::endl;
#endif
        exit(0);
    }

    if (result.count("save")) {
        cfg.saveToLocalFile = true;
    }

    if (result.count("gain")) {
        cfg.audioGain = result["gain"].as<float>();
    }

    if (result.count("device")) {
        cfg.audioDevice = result["device"].as<std::string>();
    }

    if (result.count("rate")) {
        cfg.sampleRate = result["rate"].as<unsigned int>();
    }

    if (result.count("duration")) {
        cfg.durationInSeconds = result["duration"].as<int>();
    }

    if (result.count("retention")) {
        cfg.retentionDays = result["retention"].as<int>();
    }

    // VAD options
    if (result.count("vad")) {
        cfg.vadEnabled = result["vad"].as<bool>();
    }
    if (result.count("vad-threshold")) {
        cfg.vadThreshold = result["vad-threshold"].as<float>();
    }
    if (result.count("vad-hangover")) {
        cfg.vadHangoverMs = result["vad-hangover"].as<int>();
    }

#ifdef ENABLE_BLUETOOTH
    if (result.count("bluetooth")) {
        cfg.useBluetooth = true;
    }
#endif

    if (result.count("verbose")) {
        cfg.verbose = true;
    }

    return cfg;
}

int main(int argc, char *argv[])
{
    // Set up signal handlers
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    printBanner();
    config = process_args(argc, argv);
    
    // Print configuration
    std::cout << "\nConfiguration:" << std::endl;
    std::cout << "  Audio device:    " << config.audioDevice << std::endl;
    std::cout << "  Sample rate:     " << config.sampleRate << " Hz" << std::endl;
    std::cout << "  Chunk duration:  " << config.durationInSeconds << " seconds" << std::endl;
    std::cout << "  Audio gain:      " << config.audioGain << "x" << std::endl;
    std::cout << "  Save locally:    yes (in data/ directory)" << std::endl;
    std::cout << "  Retention:       " << config.retentionDays << " days" << std::endl;
    std::cout << "  VAD:             " << (config.vadEnabled ? "enabled" : "disabled");
    if (config.vadEnabled) {
        std::cout << " (threshold: " << config.vadThreshold 
                  << ", hangover: " << config.vadHangoverMs << "ms)";
    }
    std::cout << std::endl;
#ifdef ENABLE_BLUETOOTH
    std::cout << "  Mode:            " << (config.useBluetooth ? "Bluetooth LE" : "WiFi/HTTP") << std::endl;
#else
    std::cout << "  Mode:            WiFi/HTTP" << std::endl;
#endif
    std::cout << std::endl;

#ifdef ENABLE_BLUETOOTH
    // Start BLE server if bluetooth mode
    if (config.useBluetooth) {
        bleServer = new BLEServer();
        if (!bleServer->start("ADeus-Pi")) {
            std::cerr << "Failed to start BLE server" << std::endl;
            return 1;
        }
        std::cout << "Waiting for BLE connection from phone app..." << std::endl;
    }
#endif

    // Open PCM device for recording
    snd_pcm_t *capture_handle;
    snd_pcm_format_t format = SND_PCM_FORMAT_S32_LE;

    rc = snd_pcm_open(&capture_handle, config.audioDevice.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (rc < 0)
    {
        std::cerr << "Unable to open PCM device '" << config.audioDevice << "': " << snd_strerror(rc) << std::endl;
        std::cerr << "\nTip: Run 'arecord -l' to list available capture devices" << std::endl;
        std::cerr << "     Common devices: plughw:0,0 (built-in), plughw:1,0 (USB)" << std::endl;
        return 1;
    }

    std::cout << "Opened audio device: " << config.audioDevice << std::endl;

    // Set PCM parameters
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;

    rc = snd_pcm_set_params(capture_handle,
                            format,
                            SND_PCM_ACCESS_RW_INTERLEAVED,
                            config.channels,
                            config.sampleRate,
                            1,       // allow software resampling
                            500000); // desired latency

    if (rc < 0)
    {
        std::cerr << "Setting PCM parameters failed: " << snd_strerror(rc) << std::endl;
        return 1;
    }

    snd_pcm_get_params(capture_handle, &buffer_size, &period_size);
    snd_pcm_prepare(capture_handle);

    std::cout << "Recording started! Press Ctrl+C to stop." << std::endl;
    std::cout << std::endl;

    // Run initial cleanup of old files
    cleanupOldAudioFiles();

    // Start threads
    std::thread recordingThread(recordAudio, capture_handle, period_size);
    std::thread sendingThread(handleAudioBuffer);
    std::thread cleanupThreadHandle(cleanupThread);

    // Wait for threads
    recordingThread.join();
    
    // Wake up the sending thread
    audioQueue.notify_all();
    sendingThread.join();
    cleanupThreadHandle.join();

    // Cleanup
    snd_pcm_drop(capture_handle);
    snd_pcm_close(capture_handle);

#ifdef ENABLE_BLUETOOTH
    if (bleServer) {
        bleServer->stop();
        delete bleServer;
    }
#endif

    std::cout << "Goodbye!" << std::endl;
    return 0;
}
