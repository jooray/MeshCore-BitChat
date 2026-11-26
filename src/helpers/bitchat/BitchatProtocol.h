#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Bitchat Protocol Constants
#define BITCHAT_HEADER_SIZE 14     // v1: version(1) + type(1) + ttl(1) + timestamp(8) + flags(1) + payloadLength(2)
#define BITCHAT_HEADER_SIZE_V2 16  // v2: same as v1 but payloadLength is 4 bytes
#define BITCHAT_SIGNATURE_SIZE 64  // Ed25519 signature
#define BITCHAT_MAX_WIRE_PAYLOAD_SIZE 245  // Legacy reference max wire payload
// Max decompressed payload size - reduced on NRF52 to save RAM (1KB vs 2KB)
#if defined(NRF52_PLATFORM)
  #define BITCHAT_MAX_PAYLOAD_SIZE 1024  // 1KB for NRF52 (saves 1KB RAM)
#else
  #define BITCHAT_MAX_PAYLOAD_SIZE 2048  // 2KB for other platforms
#endif
#define BITCHAT_VERSION 1        // version we emit
#define BITCHAT_VERSION_2 2      // v2 = 4-byte payloadLen + optional source route
#define BITCHAT_VERSION_MAX 2    // highest version we can parse
#define BITCHAT_SENDER_ID_SIZE 8
#define BITCHAT_RECIPIENT_ID_SIZE 8

#define BITCHAT_MAX_MESSAGE_SIZE 512

// BLE Service UUIDs
#define BITCHAT_SERVICE_UUID "F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C"
#define BITCHAT_CHARACTERISTIC_UUID "A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D"

// Duplicate cache configuration
#define BITCHAT_DUPLICATE_CACHE_SIZE 100
#define BITCHAT_DUPLICATE_TIME_WINDOW_MS 300000  // 5 minutes

// Bitchat Message Types
enum BitchatMessageType : uint8_t {
    BITCHAT_MSG_ANNOUNCE = 0x01,
    BITCHAT_MSG_MESSAGE = 0x02,
    BITCHAT_MSG_LEAVE = 0x03,
    BITCHAT_MSG_IDENTITY = 0x04,
    BITCHAT_MSG_CHANNEL = 0x05,
    BITCHAT_MSG_PING = 0x06,
    BITCHAT_MSG_PONG = 0x07,
    BITCHAT_MSG_NOISE_HANDSHAKE = 0x10,
    BITCHAT_MSG_NOISE_ENCRYPTED = 0x11,
    BITCHAT_MSG_FRAGMENT_NEW = 0x20,
    BITCHAT_MSG_REQUEST_SYNC = 0x21,
    BITCHAT_MSG_FILE_TRANSFER = 0x22,
    BITCHAT_MSG_VOICE_FRAME = 0x29,   // ephemeral push-to-talk frame, never gossiped
    BITCHAT_MSG_COURIER_ACK_A = 0x2A, // reserved by iOS for courier spray acks
    BITCHAT_MSG_COURIER_ACK_B = 0x2B, // reserved by iOS for courier spray acks
    BITCHAT_MSG_ANNOUNCE_V2 = 0x2C,   // iOS identity-free presence, not emitted yet
    BITCHAT_MSG_VIDEO_FRAME = 0x2D,   // live video frame (iOS PR #1827), ephemeral
    BITCHAT_MSG_FRAGMENT = 0xFF
};

// Bitchat Protocol Flags
#define BITCHAT_FLAG_HAS_RECIPIENT 0x01
#define BITCHAT_FLAG_HAS_SIGNATURE 0x02
#define BITCHAT_FLAG_IS_COMPRESSED 0x04
#define BITCHAT_FLAG_HAS_ROUTE     0x08  // v2+ only: optional source route follows recipientID
#define BITCHAT_FLAG_IS_RSR        0x10  // iOS; cleared in the signing preimage

// Announce payload TLV types
#define BITCHAT_TLV_NICKNAME 0x01
#define BITCHAT_TLV_NOISE_PUBKEY 0x02
#define BITCHAT_TLV_ED25519_PUBKEY 0x03

struct BitchatMessage {
    uint8_t version;
    uint8_t type;
    uint8_t ttl;
    uint64_t timestamp;  // milliseconds since epoch
    uint8_t flags;
    uint16_t payloadLength;
    uint16_t wirePayloadLength;  // Original payload length on wire (before decompression)
    uint8_t senderId[BITCHAT_SENDER_ID_SIZE];
    uint8_t recipientId[BITCHAT_RECIPIENT_ID_SIZE];
    uint8_t payload[BITCHAT_MAX_PAYLOAD_SIZE];
    uint8_t signature[BITCHAT_SIGNATURE_SIZE];

    BitchatMessage() { clear(); }

    void clear() {
        memset(this, 0, sizeof(*this));
        version = BITCHAT_VERSION;
    }

    bool hasRecipient() const { return (flags & BITCHAT_FLAG_HAS_RECIPIENT) != 0; }
    bool hasSignature() const { return (flags & BITCHAT_FLAG_HAS_SIGNATURE) != 0; }
    bool isCompressed() const { return (flags & BITCHAT_FLAG_IS_COMPRESSED) != 0; }

    void setHasRecipient(bool val) { if (val) flags |= BITCHAT_FLAG_HAS_RECIPIENT; else flags &= ~BITCHAT_FLAG_HAS_RECIPIENT; }
    void setHasSignature(bool val) { if (val) flags |= BITCHAT_FLAG_HAS_SIGNATURE; else flags &= ~BITCHAT_FLAG_HAS_SIGNATURE; }

    uint64_t getSenderId64() const { return readLE64(senderId); }
    void setSenderId64(uint64_t id) { writeLE64(senderId, id); }
    uint64_t getRecipientId64() const { return readLE64(recipientId); }
    void setRecipientId64(uint64_t id) { writeLE64(recipientId, id); }

    static uint64_t readLE64(const uint8_t* p) {
        uint64_t id = 0;
        for (int i = 0; i < 8; i++) {
            id |= (static_cast<uint64_t>(p[i]) << (i * 8));
        }
        return id;
    }

    static void writeLE64(uint8_t* p, uint64_t id) {
        for (int i = 0; i < 8; i++) {
            p[i] = static_cast<uint8_t>((id >> (i * 8)) & 0xFF);
        }
    }
};

/**
 * Duplicate Message Cache (FNV-1a over sender, second, type, length and full payload)
 */
class BitchatDuplicateCache {
public:
    BitchatDuplicateCache();

    bool isDuplicate(const BitchatMessage& msg);
    void addMessage(const BitchatMessage& msg);
    void clear();

private:
    struct CacheEntry {
        uint32_t hash;
        uint32_t timestamp;  // seconds
        bool valid;

        CacheEntry() : hash(0), timestamp(0), valid(false) {}
    };

    CacheEntry cache[BITCHAT_DUPLICATE_CACHE_SIZE];
    size_t currentIndex;

    uint32_t calculateHash(const BitchatMessage& msg) const;
};

class BitchatProtocol {
public:
    static bool parseMessage(const uint8_t* data, size_t length, BitchatMessage& msg);
    static size_t serializeMessage(const BitchatMessage& msg, uint8_t* buffer, size_t maxLength);
    static bool validateMessage(const BitchatMessage& msg);
    static int frameLength(const uint8_t* data, size_t length);
    static size_t getMessageSize(const BitchatMessage& msg);
    static void computePacketId(const BitchatMessage& msg, uint8_t* outId16);
    static size_t applyPKCS7Padding(uint8_t* buffer, size_t dataLen, size_t capacity);
    static size_t signingPreimage(const BitchatMessage& msg, uint8_t* buffer, size_t maxLength);

    static void createAnnounce(BitchatMessage& msg, uint64_t senderId, const char* nickname,
                               const uint8_t* noisePublicKey, const uint8_t* signingPublicKey,
                               uint64_t timestamp, uint8_t ttl);
    static const uint8_t* findTLV(const uint8_t* payload, size_t len, uint8_t type, uint8_t* outLen);

    static int inflatePayload(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap,
                              uint8_t version = BITCHAT_VERSION);
    static bool decompressPayload(BitchatMessage& msg);

    static uint16_t readBE16(const uint8_t* data);
    static uint32_t readBE32(const uint8_t* data);
    static uint64_t readBE64(const uint8_t* data);
    static void writeBE16(uint8_t* data, uint16_t value);
    static void writeBE64(uint8_t* data, uint64_t value);
};
