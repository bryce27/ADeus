#ifndef BLE_PROTOCOL_H
#define BLE_PROTOCOL_H

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

/*
 * ADeus BLE Protocol
 * 
 * The Pi acts as a BLE peripheral with these characteristics:
 * 
 * Service UUID: 4fafc201-1fb5-459e-8fcc-c5c9c331914b
 * 
 * Characteristics:
 *   - Audio Data (notify):   beb5483e-36e1-4688-b7f5-ea07361b26a8
 *   - Control (write):       beb5483e-36e1-4688-b7f5-ea07361b26a9
 *   - Status (read/notify):  beb5483e-36e1-4688-b7f5-ea07361b26aa
 */

namespace BLEProtocol {

// Service and Characteristic UUIDs
constexpr const char* SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
constexpr const char* AUDIO_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
constexpr const char* CONTROL_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a9";
constexpr const char* STATUS_CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26aa";

// Control Commands (phone -> Pi)
enum class Command : uint8_t {
    START_RECORDING = 0x01,
    STOP_RECORDING = 0x02,
    PAUSE_RECORDING = 0x03,
    RESUME_RECORDING = 0x04,
    SET_GAIN = 0x10,           // Followed by float32 gain value
    SET_SAMPLE_RATE = 0x11,    // Followed by uint32 sample rate
    SET_CHUNK_DURATION = 0x12, // Followed by uint16 seconds
    GET_STATUS = 0x20,
    PING = 0xFF,
};

// Status Response (Pi -> phone)
enum class RecordingState : uint8_t {
    STOPPED = 0x00,
    RECORDING = 0x01,
    PAUSED = 0x02,
    ERROR = 0xFF,
};

// Audio packet header (prepended to audio data)
struct AudioPacketHeader {
    uint8_t magic[2] = {'A', 'D'};  // "AD" for ADeus
    uint16_t sequenceNumber;        // For detecting dropped packets
    uint16_t packetSize;            // Size of audio data following header
    uint16_t totalPackets;          // Total packets in this chunk
    uint32_t timestamp;             // Unix timestamp
} __attribute__((packed));

// Status packet structure
struct StatusPacket {
    uint8_t magic[2] = {'A', 'S'};  // "AS" for ADeus Status
    RecordingState state;
    uint8_t batteryPercent;         // 0-100, or 255 if unknown
    float currentGain;
    uint32_t sampleRate;
    uint16_t chunkDuration;
    uint32_t bytesRecorded;         // Bytes recorded this session
    uint32_t uptime;                // Seconds since start
} __attribute__((packed));

// Helper to serialize status
inline std::vector<uint8_t> serializeStatus(const StatusPacket& status) {
    std::vector<uint8_t> data(sizeof(StatusPacket));
    memcpy(data.data(), &status, sizeof(StatusPacket));
    return data;
}

// Helper to parse command
inline Command parseCommand(const uint8_t* data, size_t len) {
    if (len < 1) return Command::PING;
    return static_cast<Command>(data[0]);
}

// Helper to parse float parameter from command
inline float parseFloatParam(const uint8_t* data, size_t len) {
    if (len < 5) return 0.0f;
    float value;
    memcpy(&value, data + 1, sizeof(float));
    return value;
}

// Helper to parse uint32 parameter from command
inline uint32_t parseUint32Param(const uint8_t* data, size_t len) {
    if (len < 5) return 0;
    uint32_t value;
    memcpy(&value, data + 1, sizeof(uint32_t));
    return value;
}

// Helper to parse uint16 parameter from command
inline uint16_t parseUint16Param(const uint8_t* data, size_t len) {
    if (len < 3) return 0;
    uint16_t value;
    memcpy(&value, data + 1, sizeof(uint16_t));
    return value;
}

} // namespace BLEProtocol

#endif // BLE_PROTOCOL_H
