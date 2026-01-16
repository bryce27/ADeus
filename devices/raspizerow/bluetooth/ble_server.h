#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include <cstdint>
#include <cstddef>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include "ble_protocol.h"

class BLEServer {
public:
    using CommandCallback = std::function<void(BLEProtocol::Command, const uint8_t*, size_t)>;
    using ConnectionCallback = std::function<void(bool connected)>;
    
    BLEServer();
    ~BLEServer();
    
    // Initialize and start advertising
    bool start(const std::string& deviceName = "ADeus-Pi");
    
    // Stop the server
    void stop();
    
    // Send audio data to connected client
    bool sendAudioData(const uint8_t* data, size_t length, uint16_t seqNum, uint16_t totalPackets);
    
    // Send status update
    bool sendStatus(const BLEProtocol::StatusPacket& status);
    
    // Check if a client is connected
    bool isConnected() const;
    
    // Set callbacks
    void setConnectionCallback(ConnectionCallback callback);
    void setCommandCallback(CommandCallback callback);
    
    // Get MTU size
    size_t getMTU() const;

private:
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};
    ConnectionCallback m_connectionCallback;
    CommandCallback m_commandCallback;
    std::thread m_serverThread;
    std::mutex m_sendMutex;
    int m_serverSocket{-1};
    int m_clientSocket{-1};
    size_t m_mtu{185};
    
    void serverLoop();
    void handleClientData(const uint8_t* data, size_t len);
    std::string buildAdvertisingData(const std::string& deviceName);
    std::string buildScanResponseData(const std::string& deviceName);
};

#endif // BLE_SERVER_H
