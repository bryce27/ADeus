#ifndef VOICE_ACTIVITY_DETECTOR_H
#define VOICE_ACTIVITY_DETECTOR_H

#include <vector>
#include <cstdint>
#include <deque>
#include <cmath>

/**
 * Voice Activity Detector (VAD)
 * 
 * Uses energy-based detection with adaptive thresholding to determine
 * if audio contains speech or is just silence/background noise.
 * 
 * Features:
 * - Adaptive noise floor estimation
 * - Hangover to prevent cutting off speech ends
 * - Smoothing to avoid rapid on/off switching
 */
class VoiceActivityDetector {
public:
    struct Config {
        float energyThreshold = 0.02f;    // Minimum energy above noise floor
        float noiseFloorAlpha = 0.995f;   // Noise floor adaptation rate (higher = slower)
        int hangoverFrames = 30;          // Frames to keep active after speech ends
        int minSpeechFrames = 5;          // Minimum frames to consider as speech
        int frameSize = 256;              // Samples per analysis frame
        bool enabled = true;
    };

    VoiceActivityDetector(const Config& config = Config{})
        : m_config(config)
        , m_noiseFloor(0.0f)
        , m_speechFrameCount(0)
        , m_silenceFrameCount(0)
        , m_isActive(false)
        , m_initialized(false)
    {}

    /**
     * Process audio samples and determine if speech is present
     * @param samples Audio samples (32-bit signed)
     * @param numSamples Number of samples
     * @return true if voice activity detected
     */
    bool process(const int32_t* samples, size_t numSamples) {
        if (!m_config.enabled) {
            return true; // If disabled, always return active
        }

        // Calculate RMS energy
        double sumSquares = 0.0;
        for (size_t i = 0; i < numSamples; i++) {
            // Normalize to [-1, 1] range
            double sample = static_cast<double>(samples[i]) / 2147483648.0;
            sumSquares += sample * sample;
        }
        float energy = static_cast<float>(std::sqrt(sumSquares / numSamples));

        // Initialize noise floor on first call
        if (!m_initialized) {
            m_noiseFloor = energy;
            m_initialized = true;
        }

        // Update noise floor (only when not speaking)
        if (!m_isActive) {
            m_noiseFloor = m_config.noiseFloorAlpha * m_noiseFloor + 
                          (1.0f - m_config.noiseFloorAlpha) * std::min(energy, m_noiseFloor * 2.0f);
        }

        // Clamp noise floor to reasonable range
        m_noiseFloor = std::max(m_noiseFloor, 0.0001f);

        // Detect if this frame has speech
        float threshold = m_noiseFloor + m_config.energyThreshold;
        bool frameHasSpeech = energy > threshold;

        // State machine with hangover
        if (frameHasSpeech) {
            m_speechFrameCount++;
            m_silenceFrameCount = 0;
            
            // Require minimum speech frames before activating
            if (m_speechFrameCount >= m_config.minSpeechFrames) {
                m_isActive = true;
            }
        } else {
            if (m_isActive) {
                m_silenceFrameCount++;
                
                // Apply hangover before deactivating
                if (m_silenceFrameCount >= m_config.hangoverFrames) {
                    m_isActive = false;
                    m_speechFrameCount = 0;
                }
            } else {
                m_speechFrameCount = 0;
            }
        }

        // Store recent energy for statistics
        m_recentEnergies.push_back(energy);
        if (m_recentEnergies.size() > 100) {
            m_recentEnergies.pop_front();
        }

        return m_isActive;
    }

    // Overload for char buffer (raw audio bytes)
    bool process(const char* buffer, size_t bufferSize, int bytesPerSample) {
        if (bytesPerSample == 4) {
            return process(reinterpret_cast<const int32_t*>(buffer), 
                          bufferSize / bytesPerSample);
        } else if (bytesPerSample == 2) {
            // Convert 16-bit to normalized energy
            const int16_t* samples = reinterpret_cast<const int16_t*>(buffer);
            size_t numSamples = bufferSize / 2;
            
            double sumSquares = 0.0;
            for (size_t i = 0; i < numSamples; i++) {
                double sample = static_cast<double>(samples[i]) / 32768.0;
                sumSquares += sample * sample;
            }
            float energy = static_cast<float>(std::sqrt(sumSquares / numSamples));
            
            // Use simplified detection for 16-bit
            return energy > m_noiseFloor + m_config.energyThreshold;
        }
        return true; // Unknown format, assume active
    }

    // Check if currently detecting voice
    bool isActive() const { return m_isActive; }

    // Get current noise floor estimate
    float getNoiseFloor() const { return m_noiseFloor; }

    // Get current energy threshold
    float getThreshold() const { 
        return m_noiseFloor + m_config.energyThreshold; 
    }

    // Get statistics
    float getAverageEnergy() const {
        if (m_recentEnergies.empty()) return 0.0f;
        float sum = 0.0f;
        for (float e : m_recentEnergies) sum += e;
        return sum / m_recentEnergies.size();
    }

    // Configuration
    void setEnabled(bool enabled) { m_config.enabled = enabled; }
    bool isEnabled() const { return m_config.enabled; }
    
    void setThreshold(float threshold) { 
        m_config.energyThreshold = threshold; 
    }
    float getConfiguredThreshold() const { 
        return m_config.energyThreshold; 
    }

    void setHangover(int frames) { m_config.hangoverFrames = frames; }
    
    void reset() {
        m_noiseFloor = 0.0f;
        m_speechFrameCount = 0;
        m_silenceFrameCount = 0;
        m_isActive = false;
        m_initialized = false;
        m_recentEnergies.clear();
    }

    Config& config() { return m_config; }
    const Config& config() const { return m_config; }

private:
    Config m_config;
    float m_noiseFloor;
    int m_speechFrameCount;
    int m_silenceFrameCount;
    bool m_isActive;
    bool m_initialized;
    std::deque<float> m_recentEnergies;
};

#endif // VOICE_ACTIVITY_DETECTOR_H
