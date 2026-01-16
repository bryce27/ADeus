#include "ble_server.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>

// For GATT we'll use a simple approach with HCI advertising
// and L2CAP CoC (Connection-oriented Channels) for data transfer

BLEServer::BLEServer() {}

BLEServer::~BLEServer() {
    stop();
}

// Helper to run shell commands
static int runCommand(const std::string& cmd) {
    return system(cmd.c_str());
}

bool BLEServer::start(const std::string& deviceName) {
    if (m_running) {
        std::cerr << "BLE Server already running" << std::endl;
        return false;
    }
    
    std::cout << "Starting BLE Server as '" << deviceName << "'..." << std::endl;
    
    // Stop bluetoothd to get direct HCI access (we'll manage it ourselves)
    std::cout << "Configuring Bluetooth adapter..." << std::endl;
    
    // Reset and configure the adapter
    runCommand("sudo hciconfig hci0 down 2>/dev/null");
    usleep(100000);
    runCommand("sudo hciconfig hci0 up 2>/dev/null");
    usleep(100000);
    
    // Set device name
    std::string nameCmd = "sudo hciconfig hci0 name '" + deviceName + "' 2>/dev/null";
    runCommand(nameCmd.c_str());
    
    // Make discoverable and enable LE advertising
    runCommand("sudo hciconfig hci0 piscan 2>/dev/null");
    runCommand("sudo hciconfig hci0 leadv 0 2>/dev/null");
    
    // Set up LE advertising data using hcitool
    // Advertisement data format: flags, complete local name, service UUID
    std::string advData = buildAdvertisingData(deviceName);
    std::string advCmd = "sudo hcitool -i hci0 cmd 0x08 0x0008 " + advData + " 2>/dev/null";
    runCommand(advCmd.c_str());
    
    // Set scan response data (device name)
    std::string scanRsp = buildScanResponseData(deviceName);
    std::string scanCmd = "sudo hcitool -i hci0 cmd 0x08 0x0009 " + scanRsp + " 2>/dev/null";
    runCommand(scanCmd.c_str());
    
    // Enable advertising
    runCommand("sudo hcitool -i hci0 cmd 0x08 0x000A 01 2>/dev/null");
    
    // Create L2CAP socket for BLE CoC (Connection-oriented Channel)
    m_serverSocket = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (m_serverSocket < 0) {
        std::cerr << "Failed to create Bluetooth socket: " << strerror(errno) << std::endl;
        return false;
    }
    
    // Set socket options for BLE
    int opt = 1;
    setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct bt_security sec = {0};
    sec.level = BT_SECURITY_LOW;
    setsockopt(m_serverSocket, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof(sec));
    
    // Bind to a dynamic PSM for L2CAP CoC
    struct sockaddr_l2 loc_addr = {0};
    loc_addr.l2_family = AF_BLUETOOTH;
    loc_addr.l2_bdaddr = {{0, 0, 0, 0, 0, 0}};  // BDADDR_ANY
    loc_addr.l2_psm = htobs(0x80);               // Dynamic PSM (128+)
    loc_addr.l2_cid = 0;
    loc_addr.l2_bdaddr_type = BDADDR_LE_PUBLIC;
    
    if (bind(m_serverSocket, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) {
        std::cerr << "Failed to bind socket: " << strerror(errno) << std::endl;
        std::cerr << "Trying alternative approach..." << std::endl;
        
        // Try without LE-specific options
        close(m_serverSocket);
        m_serverSocket = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
        
        memset(&loc_addr, 0, sizeof(loc_addr));
        loc_addr.l2_family = AF_BLUETOOTH;
        loc_addr.l2_psm = htobs(0x1001);  // Higher dynamic PSM
        
        if (bind(m_serverSocket, (struct sockaddr *)&loc_addr, sizeof(loc_addr)) < 0) {
            std::cerr << "Failed to bind socket (retry): " << strerror(errno) << std::endl;
            close(m_serverSocket);
            m_serverSocket = -1;
            return false;
        }
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
    
    std::cout << "BLE Server started successfully!" << std::endl;
    std::cout << "Device is advertising as: " << deviceName << std::endl;
    std::cout << "Service UUID: " << BLEProtocol::SERVICE_UUID << std::endl;
    std::cout << "\nWaiting for connections..." << std::endl;
    std::cout << "(Make sure to pair the device first using 'bluetoothctl')" << std::endl;
    return true;
}

std::string BLEServer::buildAdvertisingData(const std::string& deviceName) {
    // Build LE advertising data
    // Format: length, type, data...
    std::vector<uint8_t> data;
    
    // Flags: LE General Discoverable, BR/EDR not supported
    data.push_back(0x02);  // Length
    data.push_back(0x01);  // Type: Flags
    data.push_back(0x06);  // LE General Discoverable + BR/EDR Not Supported
    
    // Complete 128-bit Service UUID (reversed)
    // 4fafc201-1fb5-459e-8fcc-c5c9c331914b
    data.push_back(0x11);  // Length (17 bytes: 1 type + 16 UUID)
    data.push_back(0x07);  // Type: Complete 128-bit Service UUIDs
    // UUID in little-endian
    uint8_t uuid[] = {0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f,
                      0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f};
    for (int i = 0; i < 16; i++) {
        data.push_back(uuid[i]);
    }
    
    // Build hex string for hcitool command
    std::string result;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X", (int)data.size());
    result = buf;
    
    for (uint8_t b : data) {
        snprintf(buf, sizeof(buf), " %02X", b);
        result += buf;
    }
    
    return result;
}

std::string BLEServer::buildScanResponseData(const std::string& deviceName) {
    std::vector<uint8_t> data;
    
    // Complete Local Name
    size_t nameLen = std::min(deviceName.length(), (size_t)29);
    data.push_back(static_cast<uint8_t>(nameLen + 1));  // Length
    data.push_back(0x09);  // Type: Complete Local Name
    for (size_t i = 0; i < nameLen; i++) {
        data.push_back(static_cast<uint8_t>(deviceName[i]));
    }
    
    // Build hex string
    std::string result;
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X", (int)data.size());
    result = buf;
    
    for (uint8_t b : data) {
        snprintf(buf, sizeof(buf), " %02X", b);
        result += buf;
    }
    
    return result;
}

void BLEServer::stop() {
    m_running = false;
    
    // Disable advertising
    runCommand("sudo hcitool -i hci0 cmd 0x08 0x000A 00 2>/dev/null");
    
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
        uint8_t buf[512];
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
    header.packetSize = static_cast<uint16_t>(std::min(length, (size_t)65535));
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
