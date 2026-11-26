#include "BitchatProtocol.h"
#include "BitchatDebugSerial.h"
#include "../../Utils.h"

#if defined(ESP32)
  #include <Arduino.h>
  extern "C" {
  #include "rom/miniz.h"
  }
  #define BITCHAT_HAS_DECOMPRESSION 1
#elif defined(NRF52_PLATFORM)
  #include "BitchatBridge.h"  // For shared static decompression buffers on NRF52
  #include "../miniz/miniz_tinfl.h"
  #define BITCHAT_HAS_DECOMPRESSION 1
#else
  #include "../miniz/miniz_tinfl.h"
  #define BITCHAT_HAS_DECOMPRESSION 1
#endif

#define BITCHAT_MAX_COMPRESSED_WIRE_PAYLOAD 2048

// ============================================================================
// BitchatDuplicateCache
// ============================================================================

BitchatDuplicateCache::BitchatDuplicateCache() : currentIndex(0) {
    clear();
}

void BitchatDuplicateCache::clear() {
    for (size_t i = 0; i < BITCHAT_DUPLICATE_CACHE_SIZE; i++) {
        cache[i].valid = false;
        cache[i].hash = 0;
        cache[i].timestamp = 0;
    }
    currentIndex = 0;
}

uint32_t BitchatDuplicateCache::calculateHash(const BitchatMessage& msg) const {
    uint32_t hash = 2166136261u;

    for (int i = 0; i < BITCHAT_SENDER_ID_SIZE; i++) {
        hash ^= msg.senderId[i];
        hash *= 16777619u;
    }

    uint32_t ts_sec = static_cast<uint32_t>(msg.timestamp / 1000);
    for (int i = 0; i < 4; i++) {
        hash ^= ((ts_sec >> (8 * i)) & 0xFF);
        hash *= 16777619u;
    }

    hash ^= msg.type;
    hash *= 16777619u;

    hash ^= (msg.payloadLength & 0xFF);
    hash *= 16777619u;
    hash ^= ((msg.payloadLength >> 8) & 0xFF);
    hash *= 16777619u;

    // Hash full decoded payload (matches upstream apps and avoids prefix collisions)
    size_t n = msg.payloadLength <= BITCHAT_MAX_PAYLOAD_SIZE ? msg.payloadLength : BITCHAT_MAX_PAYLOAD_SIZE;
    for (size_t i = 0; i < n; i++) {
        hash ^= msg.payload[i];
        hash *= 16777619u;
    }

    return hash;
}

bool BitchatDuplicateCache::isDuplicate(const BitchatMessage& msg) {
    uint32_t hash = calculateHash(msg);
    uint32_t ts_sec = static_cast<uint32_t>(msg.timestamp / 1000);

    for (size_t i = 0; i < BITCHAT_DUPLICATE_CACHE_SIZE; i++) {
        if (!cache[i].valid) continue;
        if (cache[i].hash == hash) {
            int32_t timeDiff = static_cast<int32_t>(ts_sec) - static_cast<int32_t>(cache[i].timestamp);
            if (timeDiff >= -5 && timeDiff <= 5) {
                return true;
            }
        }
    }

    addMessage(msg);
    return false;
}

void BitchatDuplicateCache::addMessage(const BitchatMessage& msg) {
    cache[currentIndex].hash = calculateHash(msg);
    cache[currentIndex].timestamp = static_cast<uint32_t>(msg.timestamp / 1000);
    cache[currentIndex].valid = true;
    currentIndex = (currentIndex + 1) % BITCHAT_DUPLICATE_CACHE_SIZE;
}

// ============================================================================
// Big-endian helpers
// ============================================================================

uint16_t BitchatProtocol::readBE16(const uint8_t* data) {
    return (static_cast<uint16_t>(data[0]) << 8) | data[1];
}

uint32_t BitchatProtocol::readBE32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint64_t BitchatProtocol::readBE64(const uint8_t* data) {
    uint64_t result = 0;
    for (int i = 0; i < 8; i++) {
        result = (result << 8) | data[i];
    }
    return result;
}

void BitchatProtocol::writeBE16(uint8_t* data, uint16_t value) {
    data[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<uint8_t>(value & 0xFF);
}

void BitchatProtocol::writeBE64(uint8_t* data, uint64_t value) {
    for (int i = 7; i >= 0; i--) {
        data[7 - i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

// ============================================================================
// Inflate
// ============================================================================

int BitchatProtocol::inflatePayload(const uint8_t* in, size_t inLen, uint8_t* out, size_t outCap,
                                    uint8_t version) {
#if BITCHAT_HAS_DECOMPRESSION
    size_t prefixLen = (version >= BITCHAT_VERSION_2) ? 4 : 2;
    if (inLen <= prefixLen) return -1;

    uint32_t originalSize = (version >= BITCHAT_VERSION_2) ? readBE32(in) : readBE16(in);
    if (originalSize == 0 || originalSize > outCap) return -1;

    const uint8_t* src = in + prefixLen;
    size_t srcLen = inLen - prefixLen;

#if defined(NRF52_PLATFORM)
    tinfl_decompressor* decomp = BitchatBridge::getDecompressor();
    if (decomp == nullptr) return -1;
#else
    static tinfl_decompressor s_decomp;
    tinfl_decompressor* decomp = &s_decomp;
#endif

    static const int attempts[2] = {
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF,
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF | TINFL_FLAG_PARSE_ZLIB_HEADER
    };

    int result = -1;
    for (int a = 0; a < 2; a++) {
        size_t inBytes = srcLen;
        size_t outBytes = outCap;
        tinfl_init(decomp);
        tinfl_status status = tinfl_decompress(decomp, src, &inBytes, out, out, &outBytes, attempts[a]);
        if (status == TINFL_STATUS_DONE && outBytes == originalSize) {
            result = static_cast<int>(outBytes);
            break;
        }
    }

    return result;
#else
    (void)in; (void)inLen; (void)out; (void)outCap; (void)version;
    return -1;
#endif
}

bool BitchatProtocol::decompressPayload(BitchatMessage& msg) {
    if (!msg.isCompressed()) return true;
#if defined(NRF52_PLATFORM)
    uint8_t* tmp = BitchatBridge::getDecompBuffer();
    if (tmp == nullptr) return false;
#else
    static uint8_t s_tmp[BITCHAT_MAX_PAYLOAD_SIZE];
    uint8_t* tmp = s_tmp;
#endif
    int n = inflatePayload(msg.payload, msg.payloadLength, tmp, BITCHAT_MAX_PAYLOAD_SIZE, msg.version);
    if (n >= 0) {
        memcpy(msg.payload, tmp, static_cast<size_t>(n));
        msg.payloadLength = static_cast<uint16_t>(n);
        msg.flags &= ~BITCHAT_FLAG_IS_COMPRESSED;
    }
    return n >= 0;
}

// ============================================================================
// Parsing and Serialization
// ============================================================================

int BitchatProtocol::frameLength(const uint8_t* data, size_t length) {
    if (length < BITCHAT_HEADER_SIZE) return 0;
    uint8_t version = data[0];
    if (version < 1 || version > BITCHAT_VERSION_MAX) return -1;
    uint8_t flags = data[11];
    size_t header = BITCHAT_HEADER_SIZE;
    uint32_t payloadLen;
    if (version >= BITCHAT_VERSION_2) {
        if (length < BITCHAT_HEADER_SIZE_V2) return 0;
        payloadLen = readBE32(&data[12]);
        header = BITCHAT_HEADER_SIZE_V2;
    } else {
        payloadLen = readBE16(&data[12]);
    }
    if (payloadLen > BITCHAT_MAX_COMPRESSED_WIRE_PAYLOAD) return -1;
    size_t need = header + BITCHAT_SENDER_ID_SIZE + payloadLen;
    if (flags & BITCHAT_FLAG_HAS_RECIPIENT) need += BITCHAT_RECIPIENT_ID_SIZE;
    if (flags & BITCHAT_FLAG_HAS_SIGNATURE) need += BITCHAT_SIGNATURE_SIZE;
    if (version >= BITCHAT_VERSION_2 && (flags & BITCHAT_FLAG_HAS_ROUTE)) {
        size_t routeAt = header + BITCHAT_SENDER_ID_SIZE +
                         ((flags & BITCHAT_FLAG_HAS_RECIPIENT) ? BITCHAT_RECIPIENT_ID_SIZE : 0);
        if (length <= routeAt) return static_cast<int>(routeAt + 1);
        need += 1 + static_cast<size_t>(data[routeAt]) * BITCHAT_SENDER_ID_SIZE;
    }
    return static_cast<int>(need);
}

bool BitchatProtocol::parseMessage(const uint8_t* data, size_t length, BitchatMessage& msg) {
    if (length < BITCHAT_HEADER_SIZE) return false;

    size_t offset = 0;
    msg.version = data[offset++];
    msg.type = data[offset++];
    msg.ttl = data[offset++];
    msg.timestamp = readBE64(&data[offset]);
    offset += 8;
    msg.flags = data[offset++];

    if (msg.version < 1 || msg.version > BITCHAT_VERSION_MAX) return false;

    uint32_t payloadLen;
    if (msg.version >= BITCHAT_VERSION_2) {
        if (length < BITCHAT_HEADER_SIZE_V2) return false;
        payloadLen = readBE32(&data[offset]);
        offset += 4;
    } else {
        payloadLen = readBE16(&data[offset]);
        offset += 2;
    }

    size_t maxPayload = msg.isCompressed() ? BITCHAT_MAX_COMPRESSED_WIRE_PAYLOAD : BITCHAT_MAX_PAYLOAD_SIZE;
    if (payloadLen > maxPayload) return false;
    msg.payloadLength = static_cast<uint16_t>(payloadLen);
    msg.wirePayloadLength = msg.payloadLength;

    size_t expectedSize = offset + BITCHAT_SENDER_ID_SIZE + payloadLen;
    if (msg.hasRecipient()) expectedSize += BITCHAT_RECIPIENT_ID_SIZE;
    if (msg.hasSignature()) expectedSize += BITCHAT_SIGNATURE_SIZE;
    if (length < expectedSize) return false;

    memcpy(msg.senderId, &data[offset], BITCHAT_SENDER_ID_SIZE);
    offset += BITCHAT_SENDER_ID_SIZE;

    memset(msg.recipientId, 0, BITCHAT_RECIPIENT_ID_SIZE);
    if (msg.hasRecipient()) {
        memcpy(msg.recipientId, &data[offset], BITCHAT_RECIPIENT_ID_SIZE);
        offset += BITCHAT_RECIPIENT_ID_SIZE;
    }

    if (msg.version >= BITCHAT_VERSION_2 && (msg.flags & BITCHAT_FLAG_HAS_ROUTE)) {
        if (offset >= length) return false;
        size_t routeBytes = 1 + static_cast<size_t>(data[offset]) * BITCHAT_SENDER_ID_SIZE;
        if (length < expectedSize + routeBytes) return false;
        offset += routeBytes;
    }

    memset(msg.payload, 0, sizeof(msg.payload));
    if (payloadLen > 0) {
        bool isFragmentType = (msg.type == BITCHAT_MSG_FRAGMENT || msg.type == BITCHAT_MSG_FRAGMENT_NEW);
        if (msg.isCompressed() && !isFragmentType) {
            int n = inflatePayload(&data[offset], payloadLen, msg.payload, sizeof(msg.payload), msg.version);
            if (n < 0) return false;
            msg.payloadLength = static_cast<uint16_t>(n);
            msg.flags &= ~BITCHAT_FLAG_IS_COMPRESSED;
        } else {
            if (payloadLen > sizeof(msg.payload)) return false;
            memcpy(msg.payload, &data[offset], payloadLen);
        }
        offset += payloadLen;
    }

    memset(msg.signature, 0, BITCHAT_SIGNATURE_SIZE);
    if (msg.hasSignature()) {
        memcpy(msg.signature, &data[offset], BITCHAT_SIGNATURE_SIZE);
    }
    return true;
}

size_t BitchatProtocol::serializeMessage(const BitchatMessage& msg, uint8_t* buffer, size_t maxLength) {
    size_t requiredSize = BITCHAT_HEADER_SIZE + BITCHAT_SENDER_ID_SIZE
                        + (msg.hasRecipient() ? BITCHAT_RECIPIENT_ID_SIZE : 0)
                        + msg.payloadLength
                        + (msg.hasSignature() ? BITCHAT_SIGNATURE_SIZE : 0);
    if (maxLength < requiredSize) return 0;

    size_t offset = 0;
    buffer[offset++] = BITCHAT_VERSION;  // always emit v1
    buffer[offset++] = msg.type;
    buffer[offset++] = msg.ttl;
    writeBE64(&buffer[offset], msg.timestamp);
    offset += 8;
    buffer[offset++] = msg.flags & ~BITCHAT_FLAG_HAS_ROUTE;
    writeBE16(&buffer[offset], msg.payloadLength);
    offset += 2;

    memcpy(&buffer[offset], msg.senderId, BITCHAT_SENDER_ID_SIZE);
    offset += BITCHAT_SENDER_ID_SIZE;

    if (msg.hasRecipient()) {
        memcpy(&buffer[offset], msg.recipientId, BITCHAT_RECIPIENT_ID_SIZE);
        offset += BITCHAT_RECIPIENT_ID_SIZE;
    }

    if (msg.payloadLength > 0) {
        memcpy(&buffer[offset], msg.payload, msg.payloadLength);
        offset += msg.payloadLength;
    }

    if (msg.hasSignature()) {
        memcpy(&buffer[offset], msg.signature, BITCHAT_SIGNATURE_SIZE);
        offset += BITCHAT_SIGNATURE_SIZE;
    }

    return offset;
}

bool BitchatProtocol::validateMessage(const BitchatMessage& msg) {
    if (msg.version < 1 || msg.version > BITCHAT_VERSION_MAX) return false;

    switch (msg.type) {
        case BITCHAT_MSG_ANNOUNCE:
        case BITCHAT_MSG_MESSAGE:
        case BITCHAT_MSG_LEAVE:
        case BITCHAT_MSG_IDENTITY:
        case BITCHAT_MSG_CHANNEL:
        case BITCHAT_MSG_PING:
        case BITCHAT_MSG_PONG:
        case BITCHAT_MSG_NOISE_HANDSHAKE:
        case BITCHAT_MSG_NOISE_ENCRYPTED:
        case BITCHAT_MSG_FRAGMENT_NEW:
        case BITCHAT_MSG_REQUEST_SYNC:
        case BITCHAT_MSG_FILE_TRANSFER:
        case BITCHAT_MSG_FRAGMENT:
            break;
        default:
            return false;
    }

    if (msg.payloadLength > BITCHAT_MAX_PAYLOAD_SIZE) return false;

    for (int i = 0; i < BITCHAT_SENDER_ID_SIZE; i++) {
        if (msg.senderId[i] != 0) return true;
    }
    return false;
}

size_t BitchatProtocol::getMessageSize(const BitchatMessage& msg) {
    size_t headerSize = (msg.version >= BITCHAT_VERSION_2) ? BITCHAT_HEADER_SIZE_V2 : BITCHAT_HEADER_SIZE;
    size_t size = headerSize + BITCHAT_SENDER_ID_SIZE;
    if (msg.hasRecipient()) size += BITCHAT_RECIPIENT_ID_SIZE;
    size += (msg.wirePayloadLength > 0) ? msg.wirePayloadLength : msg.payloadLength;
    if (msg.hasSignature()) size += BITCHAT_SIGNATURE_SIZE;
    return size;
}

void BitchatProtocol::computePacketId(const BitchatMessage& msg, uint8_t* outId16) {
    static uint8_t hashInput[1 + BITCHAT_SENDER_ID_SIZE + 8 + BITCHAT_MAX_PAYLOAD_SIZE];
    size_t offset = 0;

    hashInput[offset++] = msg.type;
    memcpy(&hashInput[offset], msg.senderId, BITCHAT_SENDER_ID_SIZE);
    offset += BITCHAT_SENDER_ID_SIZE;
    writeBE64(&hashInput[offset], msg.timestamp);
    offset += 8;

    size_t payLen = msg.payloadLength <= BITCHAT_MAX_PAYLOAD_SIZE ? msg.payloadLength : BITCHAT_MAX_PAYLOAD_SIZE;
    if (payLen > 0) {
        memcpy(&hashInput[offset], msg.payload, payLen);
        offset += payLen;
    }

    uint8_t fullHash[32];
    mesh::Utils::sha256(fullHash, 32, hashInput, static_cast<int>(offset));
    memcpy(outId16, fullHash, 16);
}

size_t BitchatProtocol::applyPKCS7Padding(uint8_t* buffer, size_t dataLen, size_t capacity) {
    static const size_t blockSizes[] = {256, 512, 1024, 2048};
    size_t targetSize = dataLen;
    for (size_t i = 0; i < 4; i++) {
        if (dataLen + 16 <= blockSizes[i]) {
            targetSize = blockSizes[i];
            break;
        }
    }
    if (dataLen >= targetSize || targetSize > capacity) return dataLen;
    size_t paddingNeeded = targetSize - dataLen;
    if (paddingNeeded > 255) return dataLen;
    memset(buffer + dataLen, static_cast<int>(paddingNeeded), paddingNeeded);
    return targetSize;
}

size_t BitchatProtocol::signingPreimage(const BitchatMessage& msg, uint8_t* buffer, size_t maxLength) {
    size_t len = serializeMessage(msg, buffer, maxLength);
    if (len == 0) return 0;
    if (msg.hasSignature()) len -= BITCHAT_SIGNATURE_SIZE;
    buffer[2] = 0;  // TTL = SYNC_TTL_HOPS
    buffer[11] &= ~(BITCHAT_FLAG_HAS_SIGNATURE | BITCHAT_FLAG_IS_RSR);
    return applyPKCS7Padding(buffer, len, maxLength);
}

void BitchatProtocol::createAnnounce(BitchatMessage& msg, uint64_t senderId, const char* nickname,
                                     const uint8_t* noisePublicKey, const uint8_t* signingPublicKey,
                                     uint64_t timestamp, uint8_t ttl) {
    msg.version = BITCHAT_VERSION;
    msg.type = BITCHAT_MSG_ANNOUNCE;
    msg.ttl = ttl;
    msg.timestamp = timestamp;
    msg.flags = 0;
    msg.setSenderId64(senderId);
    memset(msg.recipientId, 0, sizeof(msg.recipientId));

    size_t offset = 0;
    if (nickname != nullptr && nickname[0] != '\0') {
        size_t nickLen = strlen(nickname);
        if (nickLen > 13) nickLen = 13;
        msg.payload[offset++] = BITCHAT_TLV_NICKNAME;
        msg.payload[offset++] = static_cast<uint8_t>(nickLen);
        memcpy(&msg.payload[offset], nickname, nickLen);
        offset += nickLen;
    }
    if (noisePublicKey != nullptr) {
        msg.payload[offset++] = BITCHAT_TLV_NOISE_PUBKEY;
        msg.payload[offset++] = 32;
        memcpy(&msg.payload[offset], noisePublicKey, 32);
        offset += 32;
    }
    if (signingPublicKey != nullptr) {
        msg.payload[offset++] = BITCHAT_TLV_ED25519_PUBKEY;
        msg.payload[offset++] = 32;
        memcpy(&msg.payload[offset], signingPublicKey, 32);
        offset += 32;
    }
    msg.payloadLength = static_cast<uint16_t>(offset);
    msg.wirePayloadLength = msg.payloadLength;
}

const uint8_t* BitchatProtocol::findTLV(const uint8_t* payload, size_t len, uint8_t type, uint8_t* outLen) {
    size_t off = 0;
    while (off + 2 <= len) {
        uint8_t t = payload[off];
        uint8_t l = payload[off + 1];
        off += 2;
        if (off + l > len) return nullptr;
        if (t == type) {
            *outLen = l;
            return &payload[off];
        }
        off += l;
    }
    return nullptr;
}
