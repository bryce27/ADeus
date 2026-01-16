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
    std::string cmd = "hciconfig hci0 name '" + deviceName + "' 2>/dev/null";
    system(cmd.c_str());
    
    // Reset adapter
    system("hciconfig hci0 reset 2>/dev/null");
    usleep(100000);
    
    // Power on and make discoverable
    system("hciconfig hci0 up 2>/dev/null");
    system("hciconfig hci0 piscan 2>/dev/null");
    system("hciconfig hci0 leadv 3 2>/dev/null");
    
    // Create L2CAP socket for BLE
    m_serverSocket = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (m_serverSocket < 0) {
        std::cerr << "Failed to create Bluetooth socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options for BLE
    struct bt_security sec = {0};
    sec.level = BT_SECURITY_LOW;
    if (setsockopt(m_serverSocket, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec)) < 0) {
        std::cerr << "Warning: Failed to set security level: " << strerror(errno) << std::endl;
    }
    
    // Bind to BLE ATT CID (PSM must be 0 when using fixed CID)
    struct sockaddr_l2 loc_addr = {0};
    loc_addr.l2_family = AF_BLUETOOTH;
    loc_addr.l2_bdaddr = {{0, 0, 0, 0, 0, 0}};  // BDADDR_ANY
    loc_addr.l2_psm = 0;                         // Must be 0 for fixed CID
    loc_addr.l2_cid = htobs(4);                  // ATT CID for BLE
    loc_addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
    
    if (bind(m_serverSocket, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) {
        std::cerr << "Failed to bind socket: " << strerror(errno) << std::endl;
        std::cerr << "This may require running as root or adding capabilities" << std::endl;
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }
    
    // Listen for connections
    if (listen(m_serverSocket, 1) < 0) {
        std::cerr << "Failed to listen: " << strerror(errno) << std::endl;
        close(m_serverSocket);
        m_serverSocket = -1;
        return false;
    }
    
    m_running = true;
    m_serverThread = std::thread(&BLEServer::serverLoop, this);
    
    std::cout << "BLE Server started, waiting for connections..." << std::endl;
    std::cout << "Device is discoverable as: " << deviceName << std::endl;
    return true;
}

void BLEServer::stop() {
    m_running = false;
    
    if (m_clientSocket >= 0) {
        shutdown(m_clientSocket, SHUT_RDWR);
        close(m_clientSocket);
        m_clientSocket = -1;
    }
    
    if (m_serverSocket >= 0) {
        shutdown(m_serverSocket, SHUT_RDWR);
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
            if (m_running && errno != EINTR) {
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
        
        // Handle client communication
        uint8_t buf[256];
        while (m_running && m_connected) {
            ssize_t bytes = recv(m_clientSocket, buf, sizeof(buf), MSG_DONTWAIT);
            
            if (bytes > 0) {
                handleClientData(buf, bytes);
            } else if (bytes == 0) {
                // Client disconnected
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                // Error
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

void BLEServer::handleClientData(const uint8_t* data, size_t len) {
    if (len < 1) return;
    
    auto cmd = BLEProtocol::parseCommand(data, len);
    
    std::cout << "Received command: 0x" << std::hex << static_cast<int>(cmd) << std::dec << std::endl;
    
    if (m_commandCallback) {
        m_commandCallback(cmd, data, len);
    }
}

bool BLEServer::sendAudioData(const uint8_t* data, size_t length, uint16_t seqNum, uint16_t totalPackets) {
    if (!m_connected || m_clientSocket < 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(m_sendMutex);
    
    // Create packet with header
    BLEProtocol::AudioPacketHeader header;
    header.sequenceNumber = seqNum;
    header.packetSize = static_cast<uint16_t>(length);
    header.totalPackets = totalPackets;
    header.timestamp = static_cast<uint32_t>(time(nullptr));
    
    // Send header
    ssize_t sent = send(m_clientSocket, &header, sizeof(header), 0);
    if (sent < 0) {
        std::cerr << "Failed to send header: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Send audio data in MTU-sized chunks
    size_t offset = 0;
    while (offset < length) {
        size_t chunkSize = std::min(m_mtu, length - offset);
        sent = send(m_clientSocket, data + offset, chunkSize, 0);
        
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);
                continue;
            }
            std::cerr << "Send failed: " << strerror(errno) << std::endl;
            return false;
        }
        
        offset += sent;
        usleep(4000); // 4ms delay between chunks
    }
    
    return true;
}

bool BLEServer::sendStatus(const BLEProtocol::StatusPacket& status) {
    if (!m_connected || m_clientSocket < 0) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(m_sendMutex);
    
    auto data = BLEProtocol::serializeStatus(status);
    ssize_t sent = send(m_clientSocket, data.data(), data.size(), 0);
    
    return sent == static_cast<ssize_t>(data.size());
}

bool BLEServer::isConnected() const {
    return m_connected;
}

void BLEServer::setConnectionCallback(ConnectionCallback callback) {
    m_connectionCallback = callback;
}

void BLEServer::setCommandCallback(CommandCallback callback) {
    m_commandCallback = callback;
}

size_t BLEServer::getMTU() const {
    return m_mtu;
}
