#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <atomic>
#include <thread>

// BLE Service and Characteristic UUIDs (same as ESP32 for app compatibility)
#define ADEUS_SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define ADEUS_AUDIO_CHAR_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

class BLEServer {
public:
    BLEServer();
    ~BLEServer();
    
    // Initialize and start advertising
    bool start(const std::string& deviceName = "ADeus-Pi");
    
    // Stop the server
    void stop();
    
    // Send audio data to connected client
    bool sendAudioData(const uint8_t* data, size_t length);
    
    // Check if a client is connected
    bool isConnected() const;
    
    // Set connection callback
    void setConnectionCallback(std::function<void(bool)> callback);
    
    // Get MTU size
    size_t getMTU() const;

private:
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};
    std::function<void(bool)> m_connectionCallback;
    std::thread m_serverThread;
    int m_serverSocket{-1};
    int m_clientSocket{-1};
    size_t m_mtu{185}; // Default MTU matching ESP32
    
    void serverLoop();
};

#endif // BLE_SERVER_H
