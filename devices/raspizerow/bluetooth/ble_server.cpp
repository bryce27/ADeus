#include "ble_server.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

BLEServer::BLEServer() {}

BLEServer::~BLEServer() {
    stop();
}

bool BLEServer::start(const std::string& deviceName) {
    if (m_running) {
        std::cerr << "BLE Server already running" << std::endl;
        return false;
    }
    
    std::cout << "Starting BLE Server as '" << deviceName << "'..." << std::endl;
    
    // Set device name using hciconfig
    std::string cmd = "hciconfig hci0 name '" + deviceName + "'";
    system(cmd.c_str());
    
    // Make discoverable
    system("hciconfig hci0 piscan");
    system("hciconfig hci0 leadv 3");
    
    // Create L2CAP socket for BLE
    m_serverSocket = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (m_serverSocket < 0) {
        std::cerr << "Failed to create Bluetooth socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Bind to BLE PSM (Protocol Service Multiplexer)
    struct sockaddr_l2 loc_addr = {0};
    loc_addr.l2_family = AF_BLUETOOTH;
    loc_addr.l2_bdaddr = {{0, 0, 0, 0, 0, 0}};  // BDADDR_ANY
    loc_addr.l2_psm = htobs(0x25);  // Dynamic PSM for BLE
    loc_addr.l2_cid = htobs(4);     // ATT CID
    
    if (bind(m_serverSocket, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) {
        std::cerr << "Failed to bind socket: " << strerror(errno) << std::endl;
        close(m_serverSocket);
        return false;
    }
    
    // Listen for connections
    if (listen(m_serverSocket, 1) < 0) {
        std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
        close(m_serverSocket);
        return false;
    }
    
    m_running = true;
    m_serverThread = std::thread(&BLEServer::serverLoop, this);
    
    std::cout << "BLE Server started, waiting for connections..." << std::endl;
    return true;
}

void BLEServer::stop() {
    m_running = false;
    
    if (m_clientSocket >= 0) {
        close(m_clientSocket);
        m_clientSocket = -1;
    }
    
    if (m_serverSocket >= 0) {
        close(m_serverSocket);
        m_serverSocket = -1;
    }
    
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
    
    m_connected = false;
}

void BLEServer::serverLoop() {
    while (m_running) {
        struct sockaddr_l2 rem_addr = {0};
        socklen_t opt = sizeof(rem_addr);
        
        // Accept incoming connection
        m_clientSocket = accept(m_serverSocket, (struct sockaddr *)&rem_addr, &opt);
        
        if (m_clientSocket < 0) {
            if (m_running) {
                std::cerr << "Accept failed: " << strerror(errno) << std::endl;
            }
            continue;
        }
        
        // Get client address
        char addr[18];
        ba2str(&rem_addr.l2_bdaddr, addr);
        std::cout << "BLE client connected: " << addr << std::endl;
        
        m_connected = true;
        if (m_connectionCallback) {
            m_connectionCallback(true);
        }
        
        // Keep connection alive until disconnected
        char buf[256];
        while (m_running && m_connected) {
            ssize_t bytes = recv(m_clientSocket, buf, sizeof(buf), MSG_DONTWAIT);
            if (bytes == 0) {
                // Client disconnected
                break;
            } else if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;
            }
            usleep(10000); // 10ms sleep
        }
        
        std::cout << "BLE client disconnected" << std::endl;
        m_connected = false;
        if (m_connectionCallback) {
            m_connectionCallback(false);
        }
        
        close(m_clientSocket);
        m_clientSocket = -1;
    }
}

bool BLEServer::sendAudioData(const uint8_t* data, size_t length) {
    if (!m_connected || m_clientSocket < 0) {
        return false;
    }
    
    // Send in chunks matching MTU
    size_t offset = 0;
    while (offset < length) {
        size_t chunkSize = std::min(m_mtu, length - offset);
        ssize_t sent = send(m_clientSocket, data + offset, chunkSize, 0);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000); // Wait 1ms and retry
                continue;
            }
            std::cerr << "Send failed: " << strerror(errno) << std::endl;
            return false;
        }
        
        offset += sent;
        usleep(4000); // 4ms delay between chunks (like ESP32)
    }
    
    return true;
}

bool BLEServer::isConnected() const {
    return m_connected;
}

void BLEServer::setConnectionCallback(std::function<void(bool)> callback) {
    m_connectionCallback = callback;
}

size_t BLEServer::getMTU() const {
    return m_mtu;
}
