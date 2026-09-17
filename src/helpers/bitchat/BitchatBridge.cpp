#include "BitchatBridge.h"
#include "../TransportKeyStore.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

// Include ed25519 field element operations for Ed25519→Curve25519 conversion
extern "C" {
#include "fe.h"
}

// Platform-specific miniz includes for decompression support
#if defined(ESP32)
  extern "C" {
  #include "rom/miniz.h"
  }
#elif defined(NRF52_PLATFORM)
  #include "../miniz/miniz_tinfl.h"
#endif

// Debug output - Adafruit nRF52 core supports Serial.printf
#if BITCHAT_DEBUG
  #define BITCHAT_DEBUG_PRINTLN(...) do { Serial.printf("BITCHAT_BRIDGE: "); Serial.printf(__VA_ARGS__); Serial.println(); } while(0)
#else
  #define BITCHAT_DEBUG_PRINTLN(...) {}
#endif

// Verbose packet hex dump macro (separate from BITCHAT_DEBUG for optional verbosity)
#if BITCHAT_DEBUG_PACKETDUMP
static void dumpPacketHex(const char* label, const uint8_t* data, size_t len) {
    Serial.printf("PACKETDUMP [%s] (%u bytes):\n", label, (unsigned)len);
    for (size_t i = 0; i < len; i++) {
        Serial.printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) Serial.println();
    }
    if (len % 16 != 0) Serial.println();
}
  #define BITCHAT_PACKETDUMP(label, data, len) dumpPacketHex(label, data, len)
#else
  #define BITCHAT_PACKETDUMP(label, data, len) {}
#endif

// Stack checkpoint helper for debugging excessive stack usage
#if defined(NRF52_PLATFORM) && BITCHAT_DEBUG
  #include <FreeRTOS.h>
  #include <task.h>
  #define STACK_CHECKPOINT(label) do { \
    TaskHandle_t handle = xTaskGetCurrentTaskHandle(); \
    if (handle != NULL) { \
      UBaseType_t watermark = uxTaskGetStackHighWaterMark(handle); \
      uint32_t minFree = watermark * 4; \
      uint32_t stackSize = LOOP_STACK_SZ * 4; \
      uint32_t maxUsed = stackSize - minFree; \
      uint32_t usage = (maxUsed * 100) / stackSize; \
      Serial.printf("STACK_CHECKPOINT [%s]: used=%u/%u (%u%%), free=%u\n", \
        label, (unsigned)maxUsed, (unsigned)stackSize, (unsigned)usage, (unsigned)minFree); \
    } \
  } while(0)
#else
  #define STACK_CHECKPOINT(label) {}
#endif

// Static member definitions to keep large buffers out of heap allocation
BitchatBridge::CachedMessage BitchatBridge::_messageHistory[MESSAGE_HISTORY_SIZE];
BitchatBridge::FragmentBuffer BitchatBridge::_fragmentBuffers[MAX_FRAGMENT_BUFFERS];
BitchatBridge::PendingPart BitchatBridge::_pendingParts[MAX_PENDING_PARTS];
BitchatDuplicateCache BitchatBridge::_duplicateCache;
BitchatMessage BitchatBridge::_reassembledMsg;
BitchatBridge::PeerInfo BitchatBridge::_peerCache[PEER_CACHE_SIZE];

// Decompression buffers (statically allocated at compile time, shared for all decompressions)
// These are allocated in .bss section, not on heap, to avoid malloc failures
#if defined(NRF52_PLATFORM)
tinfl_decompressor BitchatBridge::_decompressorStatic;
uint8_t BitchatBridge::_decompBufferStatic[BITCHAT_MAX_PAYLOAD_SIZE];
#endif

// Module-scope static buffers (moved from function scope for stack safety on NRF52)
// These buffers are truly outside the stack and re-entrant safe
static BitchatMessage g_msgBuffer;           // For announcements/messages
static BitchatMessage g_pongBuffer;          // For PING responses
static BitchatMessage g_reassembledBuffer;   // For fragment reassembly
static uint8_t g_signData[512];              // For message signing
static uint8_t g_reassembledData[2048];      // For fragment data
static char g_senderNick[64];                // For message parsing
static char g_messageContent[2048];          // For message content
static char g_channelName[32];               // For channel name
static char g_meshTxContent[200];            // For mesh→bitchat relay

// PKCS#7 padding for Bitchat protocol signing (must match Android/iOS)
// Block sizes: 256, 512, 1024, 2048 bytes
static size_t applyPKCS7Padding(uint8_t* buffer, size_t dataLen, size_t bufferCapacity) {
    // Find optimal block size
    static const size_t blockSizes[] = {256, 512, 1024, 2048};
    size_t targetSize = dataLen;  // Default to no padding if too large

    for (size_t i = 0; i < 4; i++) {
        if (dataLen + 16 <= blockSizes[i]) {  // +16 for encryption overhead consideration
            targetSize = blockSizes[i];
            break;
        }
    }

    // Don't pad if already at or exceeding target, or if buffer too small
    if (dataLen >= targetSize || targetSize > bufferCapacity) {
        return dataLen;
    }

    size_t paddingNeeded = targetSize - dataLen;
    if (paddingNeeded > 255) {
        return dataLen;  // PKCS#7 can only encode padding length in 1 byte
    }

    // PKCS#7: all padding bytes equal the padding length
    for (size_t i = dataLen; i < targetSize; i++) {
        buffer[i] = static_cast<uint8_t>(paddingNeeded);
    }

    return targetSize;
}

// Static getter implementations for decompression buffers
tinfl_decompressor* BitchatBridge::getDecompressor() {
#if defined(NRF52_PLATFORM)
    return &_decompressorStatic;  // Return address of static array
#else
    return nullptr;  // ESP32 doesn't need this (uses ROM miniz)
#endif
}

uint8_t* BitchatBridge::getDecompBuffer() {
#if defined(NRF52_PLATFORM)
    return _decompBufferStatic;  // Array name decays to pointer
#else
    return nullptr;  // ESP32 doesn't need this (uses ROM miniz)
#endif
}

BitchatBridge::BitchatBridge(mesh::Mesh& mesh, mesh::LocalIdentity& identity, const char* nodeName,
                             const char* regionName)
    : _mesh(mesh)
    , _identity(identity)
    , _nodeName(nodeName)
    , _regionName(regionName)
    , _bitchatPeerId(0)
    , _channelConfigured(false)
    , _lastAnnounceTime(0)
    , _pendingAnnounce(false)
    , _processingMessage(false)
    , _hasReassembledMsg(false)
    , _timeOffset(0)
    , _timeSynced(false)
    , _bootTimestamp(0)
    , _messagesRelayed(0)
    , _duplicatesDropped(0)
    , _meshChannelConfigured(false)
    , _meshChannelIndex(-1)
    , _messageHistoryHead(0)
{
    memset(_defaultChannelName, 0, sizeof(_defaultChannelName));
    strcpy(_defaultChannelName, "mesh");  // Default channel
    memset(&_meshcoreChannel, 0, sizeof(_meshcoreChannel));
    memset(&_meshChannel, 0, sizeof(_meshChannel));
    memset(_noisePublicKey, 0, sizeof(_noisePublicKey));

    // Initialize channel mappings
    for (size_t i = 0; i < MAX_CHANNEL_MAPPINGS; i++) {
        _channelMappings[i].configured = false;
        memset(_channelMappings[i].bitchatName, 0, sizeof(_channelMappings[i].bitchatName));
    }

    // Initialize peer cache
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        _peerCache[i].valid = false;
        _peerCache[i].peerId = 0;
        _peerCache[i].timestamp = 0;
        memset(_peerCache[i].nickname, 0, sizeof(_peerCache[i].nickname));
    }

    // Initialize fragment buffers
    for (size_t i = 0; i < MAX_FRAGMENT_BUFFERS; i++) {
        _fragmentBuffers[i].active = false;
        _fragmentBuffers[i].senderId = 0;
        memset(_fragmentBuffers[i].fragmentId, 0, sizeof(_fragmentBuffers[i].fragmentId));
        _fragmentBuffers[i].totalFragments = 0;
        _fragmentBuffers[i].receivedCount = 0;
        _fragmentBuffers[i].originalType = 0;
        _fragmentBuffers[i].receivedMask = 0;
        _fragmentBuffers[i].dataLen = 0;
        _fragmentBuffers[i].startTime = 0;
    }

    // Initialize message history cache
    for (size_t i = 0; i < MESSAGE_HISTORY_SIZE; i++) {
        _messageHistory[i].valid = false;
        _messageHistory[i].addedTimeMs = 0;
    }

    // Initialize pending parts queue
    for (size_t i = 0; i < MAX_PENDING_PARTS; i++) {
        _pendingParts[i].valid = false;
        _pendingParts[i].originalTimestamp = 0;
    }
    _pendingPartsHead = 0;
    _pendingPartsTail = 0;
    _lastPartSentTime = 0;

    // Initialize pending relays queue (multi-bridge dedup)
    for (size_t i = 0; i < MAX_PENDING_RELAYS; i++) {
        _pendingRelays[i].valid = false;
        _pendingRelays[i].bitchatTimestamp = 0;
        _pendingRelays[i].sendAtMillis = 0;
    }
}

void BitchatBridge::begin() {
    STACK_CHECKPOINT("begin() entry");

    // Derive Noise public key (Curve25519) from Ed25519 identity
    deriveNoisePublicKey(_identity.pub_key, _noisePublicKey);

    // Derive Bitchat peer ID from the Noise public key (the app requires this binding)
    _bitchatPeerId = derivePeerId(_noisePublicKey);

    // Configure the #mesh channel for relaying
    configureMeshChannel();

    // Log that decompression buffers are statically allocated
    // These are allocated at compile time in .bss section (not heap malloc)
#if defined(NRF52_PLATFORM)
    Serial.printf("BITCHAT_BRIDGE: Decompression buffers statically allocated (%u bytes)\n",
                 (unsigned)(sizeof(_decompressorStatic) + sizeof(_decompBufferStatic)));
#endif

    BITCHAT_DEBUG_PRINTLN("Bridge initialized, peer ID: %08lX", (unsigned long)(_bitchatPeerId & 0xFFFFFFFF));

    STACK_CHECKPOINT("begin() exit");
}

void BitchatBridge::configureMeshChannel() {
    // Build the #mesh GroupChannel from the pre-calculated key
    memset(&_meshChannel, 0, sizeof(_meshChannel));
    memcpy(_meshChannel.secret, MESH_CHANNEL_KEY, 16);

    // Compute the 1-byte hash used for lookup
    mesh::Utils::sha256(_meshChannel.hash, sizeof(_meshChannel.hash),
                        MESH_CHANNEL_KEY, 16);

    _meshChannelConfigured = true;
    _meshChannelIndex = -1;  // Not stored in mesh's channel array yet

    // Register the mapping for Bitchat <-> MeshCore
    registerChannelMapping("mesh", _meshChannel);

    BITCHAT_DEBUG_PRINTLN("#mesh channel configured for bridging");
}

bool BitchatBridge::isMeshChannel(const mesh::GroupChannel& channel) const {
    // Compare the channel secret (key) - first 16 bytes
    return memcmp(channel.secret, MESH_CHANNEL_KEY, 16) == 0;
}

void BitchatBridge::addToMessageHistory(const BitchatMessage& msg) {
    _messageHistory[_messageHistoryHead].msg = msg;
    _messageHistory[_messageHistoryHead].addedTimeMs = millis();
    _messageHistory[_messageHistoryHead].valid = true;
    _messageHistoryHead = (_messageHistoryHead + 1) % MESSAGE_HISTORY_SIZE;
}

// ============================================================================
// GCS Filter Implementation for REQUEST_SYNC
// ============================================================================

// GCS TLV types (from Android RequestSyncPacket.kt)
#define GCS_TLV_P      0x01  // Golomb-Rice parameter (1 byte)
#define GCS_TLV_N      0x02  // Number of elements (4 bytes BE)
#define GCS_TLV_DATA   0x03  // Encoded bitstream

bool BitchatBridge::parseGCSFilter(const uint8_t* payload, size_t len, GCSFilter& outFilter) {
    // Initialize filter with defaults
    outFilter.p = 0;
    outFilter.n = 0;
    outFilter.m = 0;
    outFilter.data = nullptr;
    outFilter.dataLen = 0;

    if (payload == nullptr || len < 3) {
        return false;
    }

    // Parse TLV structure
    size_t offset = 0;
    bool hasP = false, hasN = false, hasData = false;

    while (offset + 2 <= len) {
        uint8_t type = payload[offset++];
        uint8_t length = payload[offset++];

        if (offset + length > len) {
            break;  // Truncated TLV
        }

        switch (type) {
            case GCS_TLV_P:
                if (length >= 1) {
                    outFilter.p = payload[offset];
                    hasP = true;
                }
                break;

            case GCS_TLV_N:
                if (length >= 4) {
                    // Big-endian 4-byte integer
                    outFilter.n = (static_cast<uint32_t>(payload[offset]) << 24) |
                                  (static_cast<uint32_t>(payload[offset + 1]) << 16) |
                                  (static_cast<uint32_t>(payload[offset + 2]) << 8) |
                                  static_cast<uint32_t>(payload[offset + 3]);
                    hasN = true;
                }
                break;

            case GCS_TLV_DATA:
                outFilter.data = &payload[offset];
                outFilter.dataLen = length;
                hasData = true;
                break;

            default:
                // Unknown TLV type - skip
                break;
        }
        offset += length;
    }

    // Calculate M = N * 2^P
    if (hasP && hasN) {
        outFilter.m = outFilter.n << outFilter.p;
    }

    BITCHAT_DEBUG_PRINTLN("GCS filter: P=%d, N=%u, M=%u, dataLen=%u",
                          outFilter.p, outFilter.n, outFilter.m, (unsigned)outFilter.dataLen);

    // Filter is valid if we have all required fields
    return hasP && hasN && hasData && outFilter.n > 0;
}

bool BitchatBridge::GCSFilter::mightContain(const uint8_t* packetId16) const {
    // Check if a packet ID might be in the GCS filter.
    //
    // GCS works by hashing items to a value in range [0, M), then Golomb-Rice
    // encoding the sorted differences. To check membership, we:
    // 1. Hash the packet ID to get value h in [0, M)
    // 2. Decode the filter to find all stored values
    // 3. Check if h is among the stored values
    //
    // For efficiency, we decode on-the-fly and stop early if we find or pass h.

    if (data == nullptr || dataLen == 0 || n == 0 || m == 0) {
        return false;  // Empty filter - nothing matches
    }

    // Hash packet ID to range [0, M) using SipHash-like reduction
    // Use first 8 bytes of packet ID as uint64, then reduce to M
    uint64_t h64 = 0;
    for (int i = 0; i < 8; i++) {
        h64 |= (static_cast<uint64_t>(packetId16[i]) << (i * 8));
    }

    // Reduce to range [0, M) using multiplication and shift (fast modulo)
    // h = (h64 * M) >> 64, but since M fits in 32 bits, we use simpler approach
    uint32_t h = static_cast<uint32_t>(h64 % m);

    // Golomb-Rice decode the filter to check membership
    // Each value is encoded as: unary(quotient) + binary(remainder, P bits)
    // Values are delta-encoded (differences from previous value)

    uint32_t current = 0;  // Running sum of deltas
    size_t bitPos = 0;     // Bit position in data

    // Helper to read a single bit
    auto readBit = [this, &bitPos]() -> int {
        if (bitPos / 8 >= dataLen) return -1;  // Past end
        int bit = (data[bitPos / 8] >> (7 - (bitPos % 8))) & 1;
        bitPos++;
        return bit;
    };

    // Helper to read P bits as binary value
    auto readBinary = [this, &bitPos](uint8_t bits) -> int32_t {
        if (bits == 0) return 0;
        if (bitPos / 8 + (bits + 7) / 8 > dataLen) return -1;

        int32_t value = 0;
        for (uint8_t i = 0; i < bits; i++) {
            int bit = (data[bitPos / 8] >> (7 - (bitPos % 8))) & 1;
            value = (value << 1) | bit;
            bitPos++;
        }
        return value;
    };

    for (uint32_t i = 0; i < n; i++) {
        // Decode quotient (unary: count 1s until 0)
        uint32_t quotient = 0;
        int bit;
        while ((bit = readBit()) == 1) {
            quotient++;
            if (quotient > m) return false;  // Malformed filter
        }
        if (bit < 0) return false;  // Truncated

        // Decode remainder (P bits, binary)
        int32_t remainder = readBinary(p);
        if (remainder < 0) return false;  // Truncated

        // Reconstruct delta and add to current
        uint32_t delta = (quotient << p) | static_cast<uint32_t>(remainder);
        current += delta;

        // Check if we found or passed the target
        if (current == h) {
            return true;  // Found - requester has this message
        }
        if (current > h) {
            return false;  // Passed it - requester doesn't have this message
        }
    }

    return false;  // Not found in filter
}

void BitchatBridge::handleRequestSync(const BitchatMessage& msg) {
    STACK_CHECKPOINT("handleRequestSync() entry");

    BITCHAT_DEBUG_PRINTLN("REQUEST_SYNC from %08lX", (unsigned long)(msg.getSenderId64() & 0xFFFFFFFF));

    // Parse the GCS filter from the REQUEST_SYNC payload
    // The filter tells us which messages the requester already has
    GCSFilter filter;
    bool hasFilter = parseGCSFilter(msg.payload, msg.payloadLength, filter);

    if (hasFilter) {
        BITCHAT_DEBUG_PRINTLN("REQUEST_SYNC has GCS filter (N=%u elements)", filter.n);
        BITCHAT_PACKETDUMP("GCS_FILTER", msg.payload, msg.payloadLength);
    } else {
        BITCHAT_DEBUG_PRINTLN("REQUEST_SYNC has no GCS filter - sending all");
    }

    // Expire old messages before responding
    uint32_t now = millis();
    for (size_t i = 0; i < MESSAGE_HISTORY_SIZE; i++) {
        if (_messageHistory[i].valid &&
            (now - _messageHistory[i].addedTimeMs) > MESSAGE_EXPIRY_MS) {
            _messageHistory[i].valid = false;
            BITCHAT_DEBUG_PRINTLN("Expired old message at index %u", (unsigned)i);
        }
    }

    // Send cached messages that the requester doesn't have
    int sent = 0;
    int skipped = 0;
    for (size_t i = 0; i < MESSAGE_HISTORY_SIZE; i++) {
        if (!_messageHistory[i].valid) continue;

        // If we have a filter, check if requester already has this message
        if (hasFilter) {
            uint8_t packetId[16];
            BitchatProtocol::computePacketId(_messageHistory[i].msg, packetId);
            BITCHAT_PACKETDUMP("SYNC_CHECK_PACKET_ID", packetId, 16);

            if (filter.mightContain(packetId)) {
                // Requester likely already has this message - skip it
                skipped++;
                BITCHAT_DEBUG_PRINTLN("Skipping msg %u - already in filter", (unsigned)i);
                continue;
            }
        }

#if defined(ESP32) || defined(NRF52_PLATFORM)
        _bleService.broadcastMessage(_messageHistory[i].msg);
        sent++;
#endif
    }

    // Always send our announcement - deferred to avoid deep call stack during BLE callback
    // This prevents stack overflow by moving Ed25519 signing out of the deep call chain
    _pendingAnnounce = true;

    BITCHAT_DEBUG_PRINTLN("REQUEST_SYNC response: sent %d messages, skipped %d (filter=%s)",
                          sent, skipped, hasFilter ? "yes" : "no");
}

uint64_t BitchatBridge::derivePeerId(const uint8_t* noisePublicKey) {
    // Bitchat derives the peer ID from the Noise static (Curve25519) key:
    // peerID = first 8 bytes of SHA-256(noise public key), sent little-endian in senderID.
    // The app rejects any ANNOUNCE whose senderID doesn't match this derivation.
    uint8_t hash[32];
    mesh::Utils::sha256(hash, sizeof(hash), noisePublicKey, 32);

    uint64_t id = 0;
    for (int i = 0; i < 8; i++) {
        id |= (static_cast<uint64_t>(hash[i]) << (i * 8));
    }
    return id;
}

void BitchatBridge::deriveNoisePublicKey(const uint8_t* ed25519PubKey, uint8_t* curve25519PubKey) {
    // Convert Ed25519 public key (edwards Y) to Curve25519 public key (montgomery X)
    // Formula: montgomeryX = (edwardsY + 1) * inverse(1 - edwardsY) mod p
    // This is the standard Ed25519→Curve25519 conversion from RFC 7748
    fe x1, tmp0, tmp1;

    fe_frombytes(x1, ed25519PubKey);
    fe_1(tmp1);
    fe_add(tmp0, x1, tmp1);      // tmp0 = edwardsY + 1
    fe_sub(tmp1, tmp1, x1);      // tmp1 = 1 - edwardsY
    fe_invert(tmp1, tmp1);       // tmp1 = inverse(1 - edwardsY)
    fe_mul(x1, tmp0, tmp1);      // x1 = (edwardsY + 1) * inverse(1 - edwardsY)
    fe_tobytes(curve25519PubKey, x1);
}

void BitchatBridge::loop() {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    static bool firstLoopAfterConnect = false;
    bool hadClient = _bleService.hasConnectedClient();

    _bleService.loop();

    // Track if we just processed a connect
    if (_bleService.hasConnectedClient() && !hadClient) {
        firstLoopAfterConnect = true;
    }

    if (firstLoopAfterConnect) {
        Serial.println("DEBUG_CRASH: BitchatBridge::loop after _bleService.loop()");
        Serial.flush();
    }

    // Process pending relays (multi-bridge collision avoidance with random delay)
    processPendingRelays();

    if (firstLoopAfterConnect) {
        Serial.println("DEBUG_CRASH: BitchatBridge::loop after processPendingRelays()");
        Serial.flush();
    }

    // Process pending multi-part messages
    processPendingParts();

    // Process queued reassembled fragments (deferred to avoid re-entrant call chain)
    if (_hasReassembledMsg) {
        processBitchatMessage(_reassembledMsg);
        _hasReassembledMsg = false;
    }

    if (firstLoopAfterConnect) {
        Serial.println("DEBUG_CRASH: BitchatBridge::loop after processPendingParts()");
        Serial.flush();
        firstLoopAfterConnect = false;
    }

    uint32_t now = millis();

    // Handle deferred announcement (from BLE callback - limited stack)
    // Defer announcements if message processing is active to prevent deep call stack overlap
    if (_pendingAnnounce && !_processingMessage) {
        STACK_CHECKPOINT("loop() before sendPeerAnnouncement");
        _pendingAnnounce = false;
        sendPeerAnnouncement();
        _lastAnnounceTime = now;
        STACK_CHECKPOINT("loop() after sendPeerAnnouncement");
    }

    // Periodically expire old messages from cache (every 30 seconds)
    static uint32_t lastExpiryCheck = 0;
    if (now - lastExpiryCheck >= 30000) {
        lastExpiryCheck = now;
        for (size_t i = 0; i < MESSAGE_HISTORY_SIZE; i++) {
            if (_messageHistory[i].valid &&
                (now - _messageHistory[i].addedTimeMs) > MESSAGE_EXPIRY_MS) {
                _messageHistory[i].valid = false;
            }
        }
    }

    // Always send periodic announcements - don't check if service is active
    // This ensures announcements resume after MeshCore app disconnects
    // BLE notification will go out whether or not anyone is listening
    // Use shorter interval when we know a client has interacted
    bool hasClient = _bleService.hasConnectedClient();
    uint32_t interval = hasClient
        ? ANNOUNCE_INTERVAL_CONNECTED_MS
        : ANNOUNCE_INTERVAL_MS;

    uint32_t elapsed = now - _lastAnnounceTime;

    if (elapsed >= interval) {
        BITCHAT_DEBUG_PRINTLN("Sending periodic announcement (interval=%lu, elapsed=%lu)",
            interval, elapsed);
        sendPeerAnnouncement();
        _lastAnnounceTime = now;
    }
#endif
}

#if defined(ESP32)

// BLE Server callbacks for standalone mode
// Forwards connect/disconnect events to BitchatBLEService so advertising is restarted
class StandaloneBLEServerCallbacks : public BLEServerCallbacks {
    BitchatBLEService* _bleService;
public:
    StandaloneBLEServerCallbacks(BitchatBLEService* service) : _bleService(service) {}
    void onConnect(BLEServer* pServer) override {
        _bleService->onServerConnect();
    }
    void onDisconnect(BLEServer* pServer) override {
        _bleService->onServerDisconnect();
    }
};

bool BitchatBridge::attachBLEService(BLEServer* server) {
    if (!_bleService.attachToServer(server, this)) {
        BITCHAT_DEBUG_PRINTLN("Failed to attach BLE service");
        return false;
    }
    _bleService.start();

    // Send first announcement immediately so connecting clients see us right away
    sendPeerAnnouncement();
    _lastAnnounceTime = millis();

    return true;
}

bool BitchatBridge::beginStandalone(const char* deviceName) {
    // Initialize BLE independently (no SerialBLEInterface)
    BLEDevice::init(deviceName);
    BLEDevice::setMTU(185);

    // Create BLE server
    BLEServer* server = BLEDevice::createServer();
    if (server == nullptr) {
        return false;
    }

    // Register server callbacks to restart advertising on connect/disconnect
    // Without this, ESP32 stops advertising after first connection and never restarts
    server->setCallbacks(new StandaloneBLEServerCallbacks(&_bleService));

    // Attach Bitchat service to the server
    if (!_bleService.attachToServer(server, this)) {
        return false;
    }

    // Set device name and start service (without touching advertising)
    _bleService.setDeviceName(deviceName);
    _bleService.startServiceOnly();

    // Start advertising with Bitchat UUID in main advertisement
    _bleService.startAdvertising();

    // Send first announcement
    sendPeerAnnouncement();
    _lastAnnounceTime = millis();

    BITCHAT_DEBUG_PRINTLN("Standalone mode initialized: %s", deviceName);
    return true;
}

bool BitchatBridge::isBLEActive() const {
    return _bleService.isActive();
}

bool BitchatBridge::hasBitchatClient() const {
    return _bleService.hasConnectedClient();
}
#elif defined(NRF52_PLATFORM)
bool BitchatBridge::beginStandalone(const char* deviceName) {
    Serial.println("BITCHAT_BRIDGE: beginStandalone (NRF52) entry");
    Serial.print("BITCHAT_BRIDGE: deviceName=");
    Serial.println(deviceName);
    Serial.print("BITCHAT_BRIDGE: this=");
    Serial.println((unsigned long)this, HEX);
    Serial.flush();

    // Initialize Bluefruit BLE with Bitchat service
    Serial.println("BITCHAT_BRIDGE: Calling _bleService.beginStandalone()");
    Serial.flush();
    bool result = _bleService.beginStandalone(deviceName, this);
    Serial.print("BITCHAT_BRIDGE: _bleService.beginStandalone() returned: ");
    Serial.println(result);
    Serial.flush();

    if (!result) {
        BITCHAT_DEBUG_PRINTLN("Failed to start BLE service");
        return false;
    }

    // Start advertising with Bitchat UUID in main advertisement
    _bleService.startAdvertising();

    // Send first announcement
    sendPeerAnnouncement();
    _lastAnnounceTime = millis();

    BITCHAT_DEBUG_PRINTLN("Standalone mode initialized: %s", deviceName);
    return true;
}

bool BitchatBridge::isBLEActive() const {
    return _bleService.isActive();
}

bool BitchatBridge::hasBitchatClient() const {
    return _bleService.hasConnectedClient();
}
#else
bool BitchatBridge::isBLEActive() const {
    return false;
}

bool BitchatBridge::hasBitchatClient() const {
    return false;
}
#endif

void BitchatBridge::setDefaultChannel(const char* channelName) {
    strncpy(_defaultChannelName, channelName, sizeof(_defaultChannelName) - 1);
    _defaultChannelName[sizeof(_defaultChannelName) - 1] = '\0';
}

void BitchatBridge::setMeshcoreChannel(const mesh::GroupChannel& channel) {
    _meshcoreChannel = channel;
    _channelConfigured = true;
}

bool BitchatBridge::registerChannelMapping(const char* bitchatChannelName, const mesh::GroupChannel& meshChannel) {
    // Skip # prefix if present
    const char* name = bitchatChannelName;
    if (name[0] == '#') name++;

    // Check if mapping already exists (update it)
    for (size_t i = 0; i < MAX_CHANNEL_MAPPINGS; i++) {
        if (_channelMappings[i].configured &&
            strcmp(_channelMappings[i].bitchatName, name) == 0) {
            _channelMappings[i].meshChannel = meshChannel;
            return true;
        }
    }

    // Find empty slot
    for (size_t i = 0; i < MAX_CHANNEL_MAPPINGS; i++) {
        if (!_channelMappings[i].configured) {
            strncpy(_channelMappings[i].bitchatName, name, sizeof(_channelMappings[i].bitchatName) - 1);
            _channelMappings[i].bitchatName[sizeof(_channelMappings[i].bitchatName) - 1] = '\0';
            _channelMappings[i].meshChannel = meshChannel;
            _channelMappings[i].configured = true;
            BITCHAT_DEBUG_PRINTLN("Registered channel mapping: %s", name);
            return true;
        }
    }

    BITCHAT_DEBUG_PRINTLN("Channel mapping registry full");
    return false;
}

bool BitchatBridge::findMeshChannel(const char* channelName, mesh::GroupChannel& outChannel) {
    // Skip # prefix if present
    const char* name = channelName;
    if (name[0] == '#') name++;

    // Search in registry
    for (size_t i = 0; i < MAX_CHANNEL_MAPPINGS; i++) {
        if (_channelMappings[i].configured &&
            strcmp(_channelMappings[i].bitchatName, name) == 0) {
            outChannel = _channelMappings[i].meshChannel;
            return true;
        }
    }

    // Fall back to default channel if configured
    if (_channelConfigured) {
        outChannel = _meshcoreChannel;
        return true;
    }

    return false;
}

const char* BitchatBridge::getChannelName(const mesh::GroupChannel& channel) {
    // Search in registry
    for (size_t i = 0; i < MAX_CHANNEL_MAPPINGS; i++) {
        if (_channelMappings[i].configured &&
            memcmp(&_channelMappings[i].meshChannel, &channel, sizeof(mesh::GroupChannel)) == 0) {
            return _channelMappings[i].bitchatName;
        }
    }

    // Return default channel name
    return _defaultChannelName;
}

void BitchatBridge::syncTimeFromPacket(uint64_t packetTimestamp) {
#ifdef ARDUINO
    // Only sync if the timestamp looks reasonable (after year 2024, before year 2100)
    // Unix timestamp for 2024-01-01 00:00:00 UTC = 1704067200000 ms
    // Unix timestamp for 2100-01-01 00:00:00 UTC = 4102444800000 ms
    const uint64_t MIN_VALID_TIMESTAMP = 1704067200000ULL;  // 2024-01-01
    const uint64_t MAX_VALID_TIMESTAMP = 4102444800000ULL;  // 2100-01-01

    // Threshold for RTC sync - if RTC differs by more than this, update it
    // This allows Bitchat to act as an NTP-like time source for the mesh
    const uint32_t RTC_SYNC_THRESHOLD_SECS = 30;  // 30 seconds

    if (packetTimestamp >= MIN_VALID_TIMESTAMP && packetTimestamp <= MAX_VALID_TIMESTAMP) {
        uint64_t localMs = millis();
        int64_t newOffset = static_cast<int64_t>(packetTimestamp) - static_cast<int64_t>(localMs);

        // If this is first sync, or if the offset changed significantly (device was rebooted), update it
        if (!_timeSynced || abs(newOffset - _timeOffset) > 60000) {  // > 1 minute drift
            _timeOffset = newOffset;
            bool wasFirstSync = !_timeSynced;
            _timeSynced = true;

            // Record boot timestamp on first sync (used to filter old synced messages)
            if (wasFirstSync) {
                _bootTimestamp = packetTimestamp;
                BITCHAT_DEBUG_PRINTLN("Boot timestamp recorded: %lu sec", (unsigned long)(packetTimestamp / 1000ULL));
            }
            BITCHAT_DEBUG_PRINTLN("Time synced from Bitchat: offset=%ld ms", (long)(_timeOffset / 1000));
        }

        // Also sync the RTC if the difference is significant
        // This allows other MeshCore components to benefit from Bitchat time sync
        mesh::RTCClock* rtc = _mesh.getRTCClock();
        if (rtc != nullptr) {
            uint32_t bitchatTimeSecs = static_cast<uint32_t>(packetTimestamp / 1000ULL);
            uint32_t rtcTime = rtc->getCurrentTime();
            int32_t timeDiff = static_cast<int32_t>(bitchatTimeSecs) - static_cast<int32_t>(rtcTime);

            if (abs(timeDiff) > RTC_SYNC_THRESHOLD_SECS) {
                rtc->setCurrentTime(bitchatTimeSecs);
                BITCHAT_DEBUG_PRINTLN("RTC synced from Bitchat: %u (was off by %d secs)",
                                      bitchatTimeSecs, timeDiff);
            }
        }
    }
#endif
}

bool BitchatBridge::parseAnnounceTLV(const uint8_t* payload, size_t len, char* nickname, size_t nickLen) {
    // ANNOUNCE payload is TLV encoded: [type:1][length:1][value:N]...
    size_t offset = 0;
    while (offset + 2 <= len) {
        uint8_t type = payload[offset++];
        uint8_t length = payload[offset++];
        if (offset + length > len) break;

        if (type == BITCHAT_TLV_NICKNAME && length > 0) {
            size_t toCopy = (length < nickLen - 1) ? length : nickLen - 1;
            memcpy(nickname, &payload[offset], toCopy);
            nickname[toCopy] = '\0';
            return true;
        }
        offset += length;
    }
    return false;
}

void BitchatBridge::cachePeer(uint64_t peerId, const char* nickname) {
    uint32_t now = millis();

    // First, check if peer already exists and update it
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        if (_peerCache[i].valid && _peerCache[i].peerId == peerId) {
            strncpy(_peerCache[i].nickname, nickname, sizeof(_peerCache[i].nickname) - 1);
            _peerCache[i].nickname[sizeof(_peerCache[i].nickname) - 1] = '\0';
            _peerCache[i].timestamp = now;
            BITCHAT_DEBUG_PRINTLN("Updated peer cache: %s -> %08lX", nickname, (unsigned long)(peerId & 0xFFFFFFFF));
            return;
        }
    }

    // Find an empty slot or the oldest entry
    size_t targetIdx = 0;
    uint32_t oldestTime = UINT32_MAX;
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        if (!_peerCache[i].valid) {
            targetIdx = i;
            break;
        }
        if (_peerCache[i].timestamp < oldestTime) {
            oldestTime = _peerCache[i].timestamp;
            targetIdx = i;
        }
    }

    // Store the new peer
    _peerCache[targetIdx].peerId = peerId;
    strncpy(_peerCache[targetIdx].nickname, nickname, sizeof(_peerCache[targetIdx].nickname) - 1);
    _peerCache[targetIdx].nickname[sizeof(_peerCache[targetIdx].nickname) - 1] = '\0';
    _peerCache[targetIdx].timestamp = now;
    _peerCache[targetIdx].valid = true;
    BITCHAT_DEBUG_PRINTLN("Cached new peer: %s -> %08lX", nickname, (unsigned long)(peerId & 0xFFFFFFFF));
}

const char* BitchatBridge::lookupPeerNickname(uint64_t peerId) {
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        if (_peerCache[i].valid && _peerCache[i].peerId == peerId) {
            return _peerCache[i].nickname;
        }
    }
    return nullptr;
}

// Compile-time timestamp calculation (approximate)
// __DATE__ format: "Jan 16 2025"
// __TIME__ format: "10:30:45"
static uint64_t getCompileTimeMs() {
    // Parse __DATE__ and __TIME__ to get approximate compile timestamp
    // This is a rough estimate but good enough for our purposes
    const char* date = __DATE__;  // "Mmm DD YYYY"
    const char* time = __TIME__;  // "HH:MM:SS"

    // Month lookup
    int month = 0;
    if (date[0] == 'J' && date[1] == 'a') month = 1;       // Jan
    else if (date[0] == 'F') month = 2;                     // Feb
    else if (date[0] == 'M' && date[2] == 'r') month = 3;  // Mar
    else if (date[0] == 'A' && date[1] == 'p') month = 4;  // Apr
    else if (date[0] == 'M' && date[2] == 'y') month = 5;  // May
    else if (date[0] == 'J' && date[2] == 'n') month = 6;  // Jun
    else if (date[0] == 'J' && date[2] == 'l') month = 7;  // Jul
    else if (date[0] == 'A' && date[1] == 'u') month = 8;  // Aug
    else if (date[0] == 'S') month = 9;                     // Sep
    else if (date[0] == 'O') month = 10;                    // Oct
    else if (date[0] == 'N') month = 11;                    // Nov
    else if (date[0] == 'D') month = 12;                    // Dec

    int day = (date[4] == ' ' ? 0 : (date[4] - '0') * 10) + (date[5] - '0');
    int year = (date[7] - '0') * 1000 + (date[8] - '0') * 100 +
               (date[9] - '0') * 10 + (date[10] - '0');

    int hour = (time[0] - '0') * 10 + (time[1] - '0');
    int minute = (time[3] - '0') * 10 + (time[4] - '0');
    int second = (time[6] - '0') * 10 + (time[7] - '0');

    // Calculate Unix timestamp (simplified, ignoring leap years for rough estimate)
    // Days since Unix epoch (Jan 1, 1970)
    int daysPerMonth[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int64_t days = (int64_t)(year - 1970) * 365 + (year - 1969) / 4;  // Leap year approximation
    for (int m = 1; m < month; m++) {
        days += daysPerMonth[m];
    }
    days += day - 1;

    uint64_t seconds = (uint64_t)days * 86400ULL + hour * 3600 + minute * 60 + second;
    return seconds * 1000ULL;
}

uint64_t BitchatBridge::getCurrentTimeMs() {
#ifdef ARDUINO
    // If we have synced time from a Bitchat client, use that (most reliable)
    if (_timeSynced) {
        return static_cast<uint64_t>(static_cast<int64_t>(millis()) + _timeOffset);
    }

    // Next best source: the Meshcore RTC (set by the companion app / GPS / RTC chip).
    // Bitchat rejects announces more than 10 minutes off wall clock, so a plausible
    // RTC value is much better than a hardcoded bootstrap date.
    const uint32_t MIN_PLAUSIBLE_EPOCH = 1767225600UL;  // Jan 1, 2026
    mesh::RTCClock* rtc = _mesh.getRTCClock();
    if (rtc != NULL) {
        uint32_t now = rtc->getCurrentTime();
        if (now > MIN_PLAUSIBLE_EPOCH) {
            return static_cast<uint64_t>(now) * 1000ULL;
        }
    }

    // Fallback: hardcoded date + millis. Announces will be rejected by current Bitchat
    // builds until we hear a peer packet and sync, but the bridge still receives.
    const uint64_t BOOTSTRAP_TIME_MS = (uint64_t)MIN_PLAUSIBLE_EPOCH * 1000ULL;
    return BOOTSTRAP_TIME_MS + millis();
#else
    return 0;
#endif
}

void BitchatBridge::sendPeerAnnouncement() {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    STACK_CHECKPOINT("sendPeerAnnouncement() entry");

    uint64_t timestamp = getCurrentTimeMs();

#if BITCHAT_DEBUG
    // Print timestamp in seconds (fits in 32-bit for valid times through 2106)
    uint32_t timestampSec = (uint32_t)(timestamp / 1000ULL);
    Serial.print("BITCHAT_BRIDGE: Announce: timestamp_sec=");
    Serial.print(timestampSec);
    Serial.print(" (expected ~1767000000 for Jan 2026), synced=");
    Serial.println(_timeSynced ? 1 : 0);
#endif

    // Use global buffer to avoid stack overflow on NRF52
    BitchatProtocol::createAnnounce(
        g_msgBuffer,
        _bitchatPeerId,
        _nodeName,
        _noisePublicKey,      // Curve25519 for Noise protocol
        _identity.pub_key,    // Ed25519 for signatures
        timestamp,
        DEFAULT_TTL
    );

    STACK_CHECKPOINT("sendPeerAnnouncement() before signMessage");

    // Sign the announce - Android requires signatures
    signMessage(g_msgBuffer);

    STACK_CHECKPOINT("sendPeerAnnouncement() after signMessage");

    if (_bleService.broadcastMessage(g_msgBuffer)) {
        BITCHAT_DEBUG_PRINTLN("Sent peer announcement");
    } else {
        BITCHAT_DEBUG_PRINTLN("FAILED to send peer announcement");
    }
    Serial.println("DEBUG_CRASH: sendPeerAnnouncement about to return");
    Serial.flush();

    STACK_CHECKPOINT("sendPeerAnnouncement() exit");
#endif
}

void BitchatBridge::signMessage(BitchatMessage& msg) {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    STACK_CHECKPOINT("signMessage() entry");

    // IMPORTANT: Bitchat protocol signs with TTL=0 and signature flag cleared
    // AND applies PKCS#7 padding to match Android/iOS toBinaryDataForSigning() behavior
    uint8_t originalTtl = msg.ttl;
    msg.ttl = 0;  // Fixed TTL for signing (matches SYNC_TTL_HOPS)
    msg.setHasSignature(false);  // Clear signature flag for signing

    // Use global buffer to avoid stack overflow on NRF52
    size_t signLen = BitchatProtocol::serializeMessage(msg, g_signData, sizeof(g_signData));
    if (signLen > 0) {
        // Apply PKCS#7 padding to match Android/iOS block sizes
        size_t paddedLen = applyPKCS7Padding(g_signData, signLen, sizeof(g_signData));

        BITCHAT_DEBUG_PRINTLN("Signing message: %u bytes (padded from %u)", (unsigned)paddedLen, (unsigned)signLen);

        STACK_CHECKPOINT("signMessage() before Ed25519 sign");
        _identity.sign(msg.signature, g_signData, paddedLen);
        STACK_CHECKPOINT("signMessage() after Ed25519 sign");
    }

    // Restore actual TTL and set signature flag for transmission
    msg.ttl = originalTtl;
    msg.setHasSignature(true);

    STACK_CHECKPOINT("signMessage() exit");
#endif
}

// ============================================================================
// Bitchat → Meshcore
// ============================================================================

#if defined(ESP32) || defined(NRF52_PLATFORM)
void BitchatBridge::onBitchatMessageReceived(const BitchatMessage& msg) {
    processBitchatMessage(msg);
}

void BitchatBridge::onBitchatClientConnect() {
    STACK_CHECKPOINT("onBitchatClientConnect()");
    BITCHAT_DEBUG_PRINTLN("Client connected");
    // Defer announcement to loop() to avoid deep call stack in callback
    _pendingAnnounce = true;
}

void BitchatBridge::onBitchatClientDisconnect() {
    BITCHAT_DEBUG_PRINTLN("Bitchat client disconnected");
}
#endif

void BitchatBridge::processBitchatMessage(const BitchatMessage& msg) {
    STACK_CHECKPOINT("processBitchatMessage() entry");

    _processingMessage = true;  // Set guard to prevent announcement during processing

    uint64_t msgSender = msg.getSenderId64();
    Serial.printf("PROCESS_MSG: type=0x%02X payloadLen=%u sender=%08lX ts=%lu\n",
                  msg.type, msg.payloadLength,
                  (unsigned long)(msgSender & 0xFFFFFFFF),
                  (unsigned long)(msg.timestamp / 1000ULL));
    Serial.flush();

    // Full payload hex dump for MESSAGE types
    if (msg.type == BITCHAT_MSG_MESSAGE) {
        Serial.println("REDUNDANT_DEBUG: ====== MESSAGE PAYLOAD HEX ======");
        for (uint16_t i = 0; i < msg.payloadLength && i < 256; i++) {
            Serial.printf("%02X", msg.payload[i]);
        }
        Serial.println();
        Serial.println("REDUNDANT_DEBUG: ====== END MESSAGE PAYLOAD ======");
        Serial.flush();
    }

    BITCHAT_PACKETDUMP("BLE_RX", msg.payload, msg.payloadLength);

    // Sync time from incoming Bitchat packets (Android sends valid Unix timestamps)
    // This is critical: our announces will be rejected as stale without valid time
    if (msg.timestamp > 0) {
        syncTimeFromPacket(msg.timestamp);
    }

    // Check for duplicates - log the hash inputs for debugging
#if BITCHAT_DEBUG_PACKETDUMP
    {
        uint64_t senderId = msg.getSenderId64();
        uint32_t timestampSecs = static_cast<uint32_t>(msg.timestamp / 1000ULL);
        Serial.printf("DEDUP_CHECK: sender=%08lX ts_sec=%lu type=%02X payloadLen=%u\n",
                      (unsigned long)(senderId & 0xFFFFFFFF), (unsigned long)timestampSecs, msg.type, msg.payloadLength);
        if (msg.payloadLength > 0) {
            size_t hashBytes = msg.payloadLength < 16 ? msg.payloadLength : 16;
            BITCHAT_PACKETDUMP("DEDUP_PAYLOAD_PREFIX", msg.payload, hashBytes);
        }
    }
#endif

    // Check for duplicates
    if (_duplicateCache.isDuplicate(msg)) {
        _duplicatesDropped++;
        BITCHAT_DEBUG_PRINTLN("Duplicate message dropped");
        _processingMessage = false;
        return;
    }

    // Drop messages older than boot time (prevents reprocessing old synced messages after reboot)
    // Only apply to MESSAGE types - ANNOUNCE and SYNC can be older
    if (_timeSynced && _bootTimestamp > 0 && msg.timestamp > 0 &&
        msg.type == BITCHAT_MSG_MESSAGE && msg.timestamp < _bootTimestamp) {
        BITCHAT_DEBUG_PRINTLN("Dropping message older than boot time (%lu < %lu)",
                              (unsigned long)(msg.timestamp / 1000ULL),
                              (unsigned long)(_bootTimestamp / 1000ULL));
        _processingMessage = false;
        return;
    }

    // Handle based on message type
    switch (msg.type) {
        case BITCHAT_MSG_MESSAGE: {
            // Use global buffers to avoid stack overflow on NRF52
            bool parsedAsTlv = false;

            // First try TLV parsing (some messages might use it)
            bool parsed = parseBitchatMessageTLV(msg.payload, msg.payloadLength,
                                                  g_senderNick, sizeof(g_senderNick),
                                                  g_messageContent, sizeof(g_messageContent),
                                                  g_channelName, sizeof(g_channelName));
            if (parsed) {
                parsedAsTlv = true;
            }

            if (!parsed && msg.payloadLength > 0 && msg.payloadLength < sizeof(g_messageContent)) {
                // TLV parsing failed - treat payload as plain text
                // This is the simple format Bitchat uses for channel messages
                memcpy(g_messageContent, msg.payload, msg.payloadLength);
                g_messageContent[msg.payloadLength] = '\0';

                // Try to look up cached nickname from previous ANNOUNCE
                uint64_t senderId = msg.getSenderId64();
                const char* cachedNick = lookupPeerNickname(senderId);
                if (cachedNick != nullptr) {
                    strncpy(g_senderNick, cachedNick, sizeof(g_senderNick) - 1);
                    g_senderNick[sizeof(g_senderNick) - 1] = '\0';
                } else {
                    // Fall back to ID-based nickname
                    snprintf(g_senderNick, sizeof(g_senderNick), "%04X",
                             (unsigned)(senderId & 0xFFFF));
                }

                // Plain text messages are assumed to be #mesh channel messages
                // The outer HAS_RECIPIENT flag doesn't indicate DM for plain text
                strcpy(g_channelName, BITCHAT_MESH_CHANNEL);

                BITCHAT_DEBUG_PRINTLN("Plain text message from %s: %s", g_senderNick, g_messageContent);
                parsed = true;
            }

            if (parsed) {
                // IMPORTANT: Only relay #mesh channel messages, ignore everything else
                // Check if channel matches #mesh (with or without # prefix)
                const char* chanToCheck = g_channelName;
                if (chanToCheck[0] == '#') chanToCheck++;

                if (strcmp(chanToCheck, "mesh") != 0) {
                    BITCHAT_DEBUG_PRINTLN("Ignoring message to channel '%s' (only #mesh)", g_channelName);
                    break;
                }

                // Ignore DMs - only check for TLV-parsed messages
                // Plain text messages use outer HAS_RECIPIENT for signing, not for DM indication
                if (parsedAsTlv && msg.hasRecipient()) {
                    BITCHAT_DEBUG_PRINTLN("Ignoring DM (only #mesh channel is bridged)");
                    break;
                }

                // Multi-bridge loop prevention: Check if message appears to be a MeshCore
                // relay from another bridge. Format: "<senderName> message" indicates this
                // message originated from MeshCore and was relayed to Bitchat by another bridge.
                if (g_messageContent[0] == '<') {
                    const char* closeBracket = strchr(g_messageContent, '>');
                    if (closeBracket != nullptr && closeBracket[1] == ' ') {
                        BITCHAT_DEBUG_PRINTLN("Skipping relay - appears to be MeshCore echo from another bridge");
                        break;
                    }
                }

                // Add to message history for REQUEST_SYNC responses
                addToMessageHistory(msg);
                BITCHAT_DEBUG_PRINTLN("Added message to history cache");

                // Relay to MeshCore #mesh channel
                BITCHAT_DEBUG_PRINTLN("Relaying message from %s to #mesh", g_senderNick);
                relayChannelMessageToMesh(msg, g_channelName, g_senderNick, g_messageContent);
            } else {
                BITCHAT_DEBUG_PRINTLN("Failed to parse MESSAGE payload (len=%u)", msg.payloadLength);
            }
            break;
        }

        case BITCHAT_MSG_ANNOUNCE: {
            // Parse announce to extract and cache peer's nickname
            char nickname[16];
            if (parseAnnounceTLV(msg.payload, msg.payloadLength, nickname, sizeof(nickname))) {
                uint64_t peerId = msg.getSenderId64();
                cachePeer(peerId, nickname);
                BITCHAT_DEBUG_PRINTLN("Cached peer: %s (%08lX)", nickname, (unsigned long)(peerId & 0xFFFFFFFF));
            }
            break;
        }

        case BITCHAT_MSG_PING:
            // Respond with PONG
            BITCHAT_DEBUG_PRINTLN("Received ping, sending pong");
            {
                // Use global buffer to avoid stack overflow on NRF52
                g_pongBuffer.version = BITCHAT_VERSION;
                g_pongBuffer.type = BITCHAT_MSG_PONG;
                g_pongBuffer.ttl = 1;
                g_pongBuffer.timestamp = getCurrentTimeMs();
                g_pongBuffer.flags = BITCHAT_FLAG_HAS_RECIPIENT;
                g_pongBuffer.setSenderId64(_bitchatPeerId);
                g_pongBuffer.setRecipientId64(msg.getSenderId64());
                g_pongBuffer.payloadLength = 0;
#if defined(ESP32) || defined(NRF52_PLATFORM)
                _bleService.broadcastMessage(g_pongBuffer);
#endif
            }
            break;

        case BITCHAT_MSG_FILE_TRANSFER:
            // File transfers (images, etc.) are not supported on mesh
            BITCHAT_DEBUG_PRINTLN("Skipping file transfer (not supported)");
            break;

        case BITCHAT_MSG_VOICE_FRAME:
            // Live push-to-talk audio: ephemeral and far too large for LoRa
            BITCHAT_DEBUG_PRINTLN("Skipping voice frame (not supported)");
            break;

        case BITCHAT_MSG_FRAGMENT_NEW:
        case BITCHAT_MSG_FRAGMENT:
            // Fragment messages are used for long text messages (>245 bytes)
            // Reassemble and process when complete
            handleFragment(msg);
            break;

        case BITCHAT_MSG_REQUEST_SYNC:
            handleRequestSync(msg);
            Serial.println("BITCHAT_BRIDGE: handleRequestSync() returned OK");
            break;

        default:
            BITCHAT_DEBUG_PRINTLN("Unhandled message type: 0x%02X", msg.type);
            break;
    }
    Serial.println("BITCHAT_BRIDGE: processBitchatMessage() COMPLETE");

    _processingMessage = false;  // Clear guard after processing complete
}

bool BitchatBridge::parseBitchatMessageTLV(const uint8_t* payload, size_t payloadLen,
                                            char* senderNick, size_t senderNickLen,
                                            char* content, size_t contentLen,
                                            char* channelName, size_t channelNameLen) {
    // Minimum size: flags(1) + timestamp(8) + idLen(1) + senderLen(1) + contentLen(2) = 13 bytes
    if (payloadLen < 13 || senderNickLen == 0 || contentLen == 0 || channelNameLen == 0) {
        return false;
    }

    senderNick[0] = '\0';
    content[0] = '\0';
    channelName[0] = '\0';

    size_t offset = 0;

    // Read flags byte
    uint8_t flags = payload[offset++];
    bool hasOriginalSender = (flags & 0x04) != 0;
    bool hasRecipientNickname = (flags & 0x08) != 0;
    bool hasSenderPeerID = (flags & 0x10) != 0;
    bool hasMentions = (flags & 0x20) != 0;
    bool hasChannel = (flags & 0x40) != 0;
    bool isEncrypted = (flags & 0x80) != 0;

    // Skip timestamp (8 bytes big-endian)
    if (offset + 8 > payloadLen) {
        return false;
    }
    offset += 8;

    // Read ID length and skip ID
    if (offset >= payloadLen) {
        return false;
    }
    uint8_t idLen = payload[offset++];
    if (offset + idLen > payloadLen) {
        return false;
    }
    offset += idLen;

    // Read sender nickname
    if (offset >= payloadLen) {
        return false;
    }
    uint8_t senderLen = payload[offset++];
    if (offset + senderLen > payloadLen) {
        return false;
    }

    size_t toCopy = senderLen;
    if (toCopy >= senderNickLen) toCopy = senderNickLen - 1;
    memcpy(senderNick, &payload[offset], toCopy);
    senderNick[toCopy] = '\0';
    offset += senderLen;

    // Read content length (2 bytes big-endian)
    if (offset + 2 > payloadLen) {
        return false;
    }
    uint16_t contentLength = (static_cast<uint16_t>(payload[offset]) << 8) | payload[offset + 1];
    offset += 2;

    // Read content
    if (offset + contentLength > payloadLen) {
        return false;
    }
    if (!isEncrypted) {
        toCopy = contentLength;
        if (toCopy >= contentLen) toCopy = contentLen - 1;
        memcpy(content, &payload[offset], toCopy);
        content[toCopy] = '\0';
    }
    offset += contentLength;

    // Skip optional fields to get to channel
    // Order: originalSender, recipientNickname, senderPeerID, mentions, channel

    if (hasOriginalSender && offset < payloadLen) {
        uint8_t len = payload[offset++];
        if (offset + len > payloadLen) return false;
        offset += len;
    }

    if (hasRecipientNickname && offset < payloadLen) {
        uint8_t len = payload[offset++];
        if (offset + len > payloadLen) return false;
        offset += len;
    }

    if (hasSenderPeerID && offset < payloadLen) {
        uint8_t len = payload[offset++];
        if (offset + len > payloadLen) return false;
        offset += len;
    }

    if (hasMentions && offset < payloadLen) {
        uint8_t mentionCount = payload[offset++];
        for (uint8_t i = 0; i < mentionCount && offset < payloadLen; i++) {
            uint8_t len = payload[offset++];
            if (offset + len > payloadLen) return false;
            offset += len;
        }
    }

    // Read channel if present
    if (hasChannel && offset < payloadLen) {
        uint8_t chanLen = payload[offset++];
        if (offset + chanLen > payloadLen) return false;

        toCopy = chanLen;
        if (toCopy >= channelNameLen) toCopy = channelNameLen - 1;
        memcpy(channelName, &payload[offset], toCopy);
        channelName[toCopy] = '\0';
    }

    BITCHAT_DEBUG_PRINTLN("TLV parsed: sender='%s' content='%s' channel='%s'", senderNick, content, channelName);

    return senderNick[0] != '\0';  // At minimum we need a sender
}

void BitchatBridge::sendSingleMessageToMesh(const char* senderNick, const char* text, uint32_t originalTimestamp) {
    // This is the internal function that sends a single message chunk to the mesh.
    // The caller is responsible for message splitting if needed.

    // Must have #mesh channel configured
    if (!_meshChannelConfigured) {
        BITCHAT_DEBUG_PRINTLN("#mesh channel not configured, cannot send to mesh");
        return;
    }

    // Use the #mesh channel for all bridged messages
    mesh::GroupChannel targetChannel = _meshChannel;

    // Use original Bitchat timestamp for deterministic packet hashing (multi-bridge dedup)
    // This ensures all bridges produce identical packets → MeshCore dedup catches duplicates
    uint32_t timestamp = originalTimestamp;

    // Sanity check: if timestamp is invalid, fall back to current time
    uint32_t now = _timeSynced ? static_cast<uint32_t>(getCurrentTimeMs() / 1000ULL)
                               : (_mesh.getRTCClock() ? _mesh.getRTCClock()->getCurrentTime() : 0);
    if (timestamp == 0 || timestamp > now + 60 || timestamp < now - 3600) {
        // Timestamp invalid (0, >1min in future, or >1hr in past) - use current time
        BITCHAT_DEBUG_PRINTLN("Invalid original timestamp %u, using current %u", timestamp, now);
        timestamp = now;
    }

    // Build Meshcore group message payload
    // Format: timestamp(4) + txt_type(1) + "📱 sender: text"
    uint8_t payload[MAX_PACKET_PAYLOAD];
    size_t offset = 0;

    // Timestamp (4 bytes)
    memcpy(&payload[offset], &timestamp, 4);
    offset += 4;

    // Text type (0 = plain text)
    payload[offset++] = 0;

    // Add 📱 prefix to sender name (identifies Bitchat origin)
    // 📱 = UTF-8: F0 9F 93 B1 (4 bytes)
    char prefixedSender[68];  // 4 bytes emoji + 1 space + 63 chars max
    snprintf(prefixedSender, sizeof(prefixedSender), "\xF0\x9F\x93\xB1 %s", senderNick);

    size_t senderLen = strlen(prefixedSender);
    size_t textLen = strlen(text);

    // Copy "📱 sender: "
    size_t available = MAX_PACKET_PAYLOAD - offset - 1;
    size_t toCopy = senderLen;
    if (toCopy > available) toCopy = available;
    memcpy(&payload[offset], prefixedSender, toCopy);
    offset += toCopy;

    if (offset < MAX_PACKET_PAYLOAD - 2) {
        payload[offset++] = ':';
        payload[offset++] = ' ';
    }

    // Copy text
    available = MAX_PACKET_PAYLOAD - offset - 1;
    toCopy = textLen;
    if (toCopy > available) toCopy = available;
    memcpy(&payload[offset], text, toCopy);
    offset += toCopy;

    // Null terminate
    payload[offset] = '\0';

    BITCHAT_PACKETDUMP("MESH_TX", payload, offset);

    // Create and send packet immediately (no delay - use pending parts queue for multi-part)
    mesh::Packet* pkt = _mesh.createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, targetChannel, payload, offset);
    if (pkt != nullptr) {
        char regionName[32];
        if (_regionName[0] == '#') snprintf(regionName, sizeof(regionName), "%s", _regionName);
        else snprintf(regionName, sizeof(regionName), "#%s", _regionName);
        TransportKey regionKey;
        mesh::Utils::sha256(regionKey.key, sizeof(regionKey.key),
                            reinterpret_cast<const uint8_t*>(regionName), strlen(regionName));
        uint16_t transportCodes[2] = {regionKey.calcTransportCode(pkt), 0};
        _mesh.sendFlood(pkt, transportCodes);
        _messagesRelayed++;
        BITCHAT_DEBUG_PRINTLN("Sent to mesh: %s: %s", prefixedSender, text);
    } else {
        BITCHAT_DEBUG_PRINTLN("Failed to create mesh packet (pool may be full)");
    }
}

bool BitchatBridge::queueMessagePart(const char* senderNick, const char* text, uint32_t originalTimestamp) {
    // Find next available slot in circular queue
    size_t nextTail = (_pendingPartsTail + 1) % MAX_PENDING_PARTS;
    if (nextTail == _pendingPartsHead && _pendingParts[_pendingPartsTail].valid) {
        // Queue is full
        BITCHAT_DEBUG_PRINTLN("Pending parts queue full, dropping part");
        return false;
    }

    strncpy(_pendingParts[_pendingPartsTail].senderNick, senderNick,
            sizeof(_pendingParts[_pendingPartsTail].senderNick) - 1);
    _pendingParts[_pendingPartsTail].senderNick[sizeof(_pendingParts[_pendingPartsTail].senderNick) - 1] = '\0';

    strncpy(_pendingParts[_pendingPartsTail].text, text,
            sizeof(_pendingParts[_pendingPartsTail].text) - 1);
    _pendingParts[_pendingPartsTail].text[sizeof(_pendingParts[_pendingPartsTail].text) - 1] = '\0';

    _pendingParts[_pendingPartsTail].originalTimestamp = originalTimestamp;
    _pendingParts[_pendingPartsTail].valid = true;
    _pendingPartsTail = nextTail;

    BITCHAT_DEBUG_PRINTLN("Queued message part for delayed sending");
    return true;
}

void BitchatBridge::processPendingParts() {
    // Check if we have pending parts to send
    if (_pendingPartsHead == _pendingPartsTail && !_pendingParts[_pendingPartsHead].valid) {
        return;  // Queue is empty
    }

    // Check if enough time has passed since last part was sent
    uint32_t now = millis();
    if (now - _lastPartSentTime < PART_SEND_DELAY_MS) {
        return;  // Not time yet
    }

    // Send the next part
    if (_pendingParts[_pendingPartsHead].valid) {
        BITCHAT_DEBUG_PRINTLN("Sending queued part: %s", _pendingParts[_pendingPartsHead].text);
        sendSingleMessageToMesh(_pendingParts[_pendingPartsHead].senderNick,
                                 _pendingParts[_pendingPartsHead].text,
                                 _pendingParts[_pendingPartsHead].originalTimestamp);
        _pendingParts[_pendingPartsHead].valid = false;
        _pendingPartsHead = (_pendingPartsHead + 1) % MAX_PENDING_PARTS;
        _lastPartSentTime = now;
    }
}

bool BitchatBridge::queuePendingRelay(const char* senderNick, const char* content, uint32_t bitchatTimestamp) {
    // Multi-bridge collision avoidance: queue message with random delay before relaying
    // This spreads out transmission attempts across bridges, avoiding RF collision

    size_t contentLen = strlen(content);

    // Long messages: send immediately (bypass delay queue)
    // Rationale: long messages are rare and distinctive, MeshCore dedup handles them fine
    // The splitter in relayChannelMessageToMesh_Internal() will handle them correctly
    if (contentLen >= MAX_RELAY_CONTENT_SIZE) {
        BITCHAT_DEBUG_PRINTLN("Long message (%u bytes), sending immediately (bypassing delay queue)", (unsigned)contentLen);
        relayChannelMessageToMesh_Internal(senderNick, content, bitchatTimestamp);
        return true;
    }

    // Short messages: queue with random delay for multi-bridge collision avoidance

    // Find first empty slot
    size_t slotIdx = MAX_PENDING_RELAYS;
    for (size_t i = 0; i < MAX_PENDING_RELAYS; i++) {
        if (!_pendingRelays[i].valid) {
            slotIdx = i;
            break;
        }
    }

    if (slotIdx >= MAX_PENDING_RELAYS) {
        BITCHAT_DEBUG_PRINTLN("All %u relay slots busy, dropping new message", (unsigned)MAX_PENDING_RELAYS);
        return false;
    }

    PendingRelay& slot = _pendingRelays[slotIdx];

    // Store sender nick
    strncpy(slot.senderNick, senderNick, sizeof(slot.senderNick) - 1);
    slot.senderNick[sizeof(slot.senderNick) - 1] = '\0';

    // Store content in inline buffer (guaranteed to fit due to check above)
    memcpy(slot.content, content, contentLen);
    slot.content[contentLen] = '\0';

    slot.bitchatTimestamp = bitchatTimestamp;

    // Random delay between 0 and MAX_RELAY_DELAY_MS milliseconds
    uint32_t randomDelay = random(0, MAX_RELAY_DELAY_MS);
    slot.sendAtMillis = millis() + randomDelay;
    slot.valid = true;

    BITCHAT_DEBUG_PRINTLN("Queued relay in slot %u with %ums delay (multi-bridge dedup)", (unsigned)slotIdx, randomDelay);
    return true;
}

void BitchatBridge::processPendingRelays() {
    // Process pending relay queue - called from loop()
    // Sends queued messages when their delay has elapsed

    uint32_t now = millis();

    for (size_t i = 0; i < MAX_PENDING_RELAYS; i++) {
        if (_pendingRelays[i].valid && now >= _pendingRelays[i].sendAtMillis) {
            // Time to send this relay
            BITCHAT_DEBUG_PRINTLN("Processing pending relay slot %u (ts=%u)", (unsigned)i, _pendingRelays[i].bitchatTimestamp);

            // Call the internal relay function that handles message splitting
            relayChannelMessageToMesh_Internal(
                _pendingRelays[i].senderNick,
                _pendingRelays[i].content,
                _pendingRelays[i].bitchatTimestamp
            );

            // Mark as processed
            _pendingRelays[i].valid = false;
        }
    }
}

void BitchatBridge::relayChannelMessageToMesh(const BitchatMessage& msg, const char* channelName,
                                               const char* senderNick, const char* text) {
    // Multi-bridge duplicate relay prevention:
    // Queue the message with a random delay instead of sending immediately.
    // This spreads out transmission attempts across bridges, avoiding RF collision.
    // Using the original Bitchat timestamp ensures all bridges produce identical packets
    // so MeshCore dedup can catch any duplicates that do make it through.

    // Extract original Bitchat timestamp (convert from ms to seconds)
    uint32_t bitchatTimestamp = static_cast<uint32_t>(msg.timestamp / 1000ULL);

    // Queue for delayed relay
    queuePendingRelay(senderNick, text, bitchatTimestamp);
}

void BitchatBridge::relayChannelMessageToMesh_Internal(const char* senderNick, const char* text, uint32_t originalTimestamp) {
    // Internal function that actually relays the message to MeshCore
    // Called from processPendingRelays() after the random delay has elapsed

    // We only relay #mesh channel, so use it directly
    if (!_meshChannelConfigured) {
        BITCHAT_DEBUG_PRINTLN("#mesh channel not configured, cannot relay");
        return;
    }

    // Calculate available space for message text
    // MeshCore MAX_TEXT_LEN is 160 bytes total for: "📱nick: text"
    // Overhead: 📱(4) + space(1) + nick(up to 13) + ": "(2) = ~20 bytes
    // With part indicator "[X/Y] "(7 bytes), we have ~133 bytes for text
    // Be conservative and use 120 bytes per chunk
    const size_t MAX_CHUNK_SIZE = 120;

    size_t contentLen = strlen(text);

    if (contentLen <= MAX_CHUNK_SIZE) {
        // Single message - no splitting needed
        sendSingleMessageToMesh(senderNick, text, originalTimestamp);
        return;
    }

    // Calculate number of parts needed
    int numParts = (contentLen + MAX_CHUNK_SIZE - 1) / MAX_CHUNK_SIZE;
    bool truncated = false;
    if (numParts > (int)MAX_MESSAGE_PARTS) {
        Serial.printf("BITCHAT_BRIDGE: WARNING - Message too long (%d bytes), truncating from %d to %d parts\n",
                      (int)contentLen, numParts, (int)MAX_MESSAGE_PARTS);
        numParts = MAX_MESSAGE_PARTS;  // Cap for reliability over LoRa
        truncated = true;
    }

    BITCHAT_DEBUG_PRINTLN("Splitting message from %s into %d parts (len=%d%s)",
                          senderNick, numParts, (int)contentLen, truncated ? ", TRUNCATED" : "");

    // Send part 1 immediately, queue remaining parts for delayed sending
    // This avoids overwhelming the mesh packet pool which can silently drop delayed packets
    size_t offset = 0;
    for (int part = 0; part < numParts && offset < contentLen; part++) {
        size_t remaining = contentLen - offset;
        size_t chunkLen = (remaining > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : remaining;

        // Smart split: try word boundaries first, then punctuation, then fallback to UTF-8 safe
        if (chunkLen < remaining) {
            // We need to split - find the best split point
            size_t bestSplit = 0;

            // First pass: look for last space within chunk (word boundary)
            for (size_t i = chunkLen; i > chunkLen / 2; i--) {
                if (text[offset + i - 1] == ' ') {
                    // Found a space - check it's not mid UTF-8
                    uint8_t nextByte = (uint8_t)text[offset + i];
                    if ((nextByte & 0xC0) != 0x80) {
                        bestSplit = i;
                        break;
                    }
                }
            }

            // Second pass: if no space, look for punctuation
            if (bestSplit == 0) {
                for (size_t i = chunkLen; i > chunkLen / 2; i--) {
                    char c = text[offset + i - 1];
                    // Common punctuation that's safe to split after
                    if (c == '.' || c == ',' || c == '!' || c == '?' ||
                        c == ';' || c == ':' || c == '-' || c == ')' || c == ']') {
                        // Check next byte isn't UTF-8 continuation
                        uint8_t nextByte = (uint8_t)text[offset + i];
                        if ((nextByte & 0xC0) != 0x80) {
                            bestSplit = i;
                            break;
                        }
                    }
                }
            }

            // Use best split point if found, otherwise fallback to UTF-8 safe split
            if (bestSplit > 0) {
                chunkLen = bestSplit;
            } else {
                // Fallback: just ensure we don't split mid UTF-8 character
                while (chunkLen > 0) {
                    uint8_t nextByte = (uint8_t)text[offset + chunkLen];
                    if ((nextByte & 0xC0) != 0x80) {
                        // Not a continuation byte, safe to split here
                        break;
                    }
                    chunkLen--;
                }
            }
        }

        if (chunkLen == 0) {
            BITCHAT_DEBUG_PRINTLN("Error: Could not find safe split point");
            break;
        }

        // Build chunk with part indicator
        char chunk[180];  // Room for part indicator + text
        snprintf(chunk, sizeof(chunk), "[%d/%d] %.*s",
                 part + 1, numParts, (int)chunkLen, text + offset);

        BITCHAT_DEBUG_PRINTLN("Part %d/%d: offset=%d, len=%d",
                              part + 1, numParts, (int)offset, (int)chunkLen);
        BITCHAT_PACKETDUMP("SPLIT_PART", (const uint8_t*)chunk, strlen(chunk));

        if (part == 0) {
            // Send first part immediately (with original timestamp for multi-bridge dedup)
            sendSingleMessageToMesh(senderNick, chunk, originalTimestamp);
            _lastPartSentTime = millis();  // Start the timer for subsequent parts
        } else {
            // Queue remaining parts for delayed sending via processPendingParts()
            // Pass original timestamp for deterministic packet hashing
            queueMessagePart(senderNick, chunk, originalTimestamp);
        }

        offset += chunkLen;
    }
}

void BitchatBridge::relayDirectMessageToMesh(const BitchatMessage& msg, const char* text) {
    // For DMs, we need to find the recipient in Meshcore contacts
    // This requires integration with the contact database
    // For now, log and skip
    BITCHAT_DEBUG_PRINTLN("DM relay not yet implemented - need contact lookup");

    // TODO: Implement DM relay
    // 1. Map Bitchat recipient ID to Meshcore Identity
    // 2. Look up shared secret
    // 3. Create encrypted TXT_MSG packet
    // 4. Send via appropriate routing
}

// ============================================================================
// Fragment Reassembly
// ============================================================================

void BitchatBridge::handleFragment(const BitchatMessage& msg) {
    // Android fragment header format (13 bytes):
    //   - 8 bytes: Fragment ID (random identifier for this fragment sequence)
    //   - 2 bytes: Index (big-endian UInt16)
    //   - 2 bytes: Total count (big-endian UInt16)
    //   - 1 byte:  Original message type
    //   - Variable: Fragment data
    if (msg.payloadLength < 13) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Payload too short (%u < 13)", msg.payloadLength);
        return;
    }

    // Parse 8-byte fragment ID
    uint8_t fragmentIdBytes[8];
    memcpy(fragmentIdBytes, &msg.payload[0], 8);

    // Parse 2-byte index (big-endian)
    uint16_t fragmentIndex = (static_cast<uint16_t>(msg.payload[8]) << 8) | msg.payload[9];

    // Parse 2-byte total count (big-endian)
    uint16_t totalFragments = (static_cast<uint16_t>(msg.payload[10]) << 8) | msg.payload[11];

    // Parse 1-byte original message type
    uint8_t originalType = msg.payload[12];

    uint64_t senderId = msg.getSenderId64();

    // Skip fragments from ourselves (rebroadcasted back to us)
    if (senderId == _bitchatPeerId) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Ignoring our own fragment");
        return;
    }

    BITCHAT_DEBUG_PRINTLN("FRAGMENT: idx=%u/%u type=0x%02X", fragmentIndex, totalFragments, originalType);

    // Validate fragment parameters
    if (totalFragments == 0 || totalFragments > 16 || fragmentIndex >= totalFragments) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Invalid params (total=%u, idx=%u)", totalFragments, fragmentIndex);
        return;
    }

    uint32_t now = millis();

    // Clean up expired fragment buffers
    for (size_t i = 0; i < MAX_FRAGMENT_BUFFERS; i++) {
        if (_fragmentBuffers[i].active &&
            (now - _fragmentBuffers[i].startTime) > FRAGMENT_TIMEOUT_MS) {
            _fragmentBuffers[i].active = false;
        }
    }

    // Find existing buffer for this sender/fragmentId
    FragmentBuffer* buf = nullptr;
    for (size_t i = 0; i < MAX_FRAGMENT_BUFFERS; i++) {
        if (_fragmentBuffers[i].active &&
            _fragmentBuffers[i].senderId == senderId &&
            memcmp(_fragmentBuffers[i].fragmentId, fragmentIdBytes, 8) == 0) {
            buf = &_fragmentBuffers[i];
            break;
        }
    }

    // New fragment sequence - find empty buffer
    if (buf == nullptr) {
        if (msg.type != BITCHAT_MSG_FRAGMENT_NEW && fragmentIndex != 0) {
            // Missed the first fragment - can't reassemble
            BITCHAT_DEBUG_PRINTLN("FRAGMENT: Missed first fragment (idx=%u)", fragmentIndex);
            return;
        }

        for (size_t i = 0; i < MAX_FRAGMENT_BUFFERS; i++) {
            if (!_fragmentBuffers[i].active) {
                buf = &_fragmentBuffers[i];
                buf->active = true;
                buf->senderId = senderId;
                memcpy(buf->fragmentId, fragmentIdBytes, 8);
                buf->totalFragments = totalFragments;
                buf->receivedCount = 0;
                buf->originalType = originalType;
                buf->receivedMask = 0;
                buf->dataLen = 0;
                buf->startTime = now;
                memset(buf->data, 0, sizeof(buf->data));
                break;
            }
        }
    }

    if (buf == nullptr) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Buffer pool full");
        // Clear all buffers if pool is full (likely corrupted state)
        for (size_t i = 0; i < MAX_FRAGMENT_BUFFERS; i++) {
            _fragmentBuffers[i].active = false;
        }
        return;
    }

    // Copy fragment data (starts after 13-byte header)
    size_t dataOffset = 13;
    size_t dataLen = msg.payloadLength - dataOffset;

    // Validate dataLen - allow up to 500 bytes per fragment (compressed chunks can be large)
    if (dataLen == 0 || dataLen > 500) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Invalid dataLen=%u", (unsigned)dataLen);
        buf->active = false;
        return;
    }

    // For now, use sequential assembly - append data for each fragment
    // This works for ordered delivery; for out-of-order we'd need offset tracking
    if (fragmentIndex == 0) {
        // First fragment - copy directly
        if (dataLen > sizeof(buf->data)) {
            BITCHAT_DEBUG_PRINTLN("FRAGMENT: First fragment too large (%u > %u)",
                                  (unsigned)dataLen, (unsigned)sizeof(buf->data));
            buf->active = false;
            return;
        }
        memcpy(buf->data, &msg.payload[dataOffset], dataLen);
        buf->dataLen = dataLen;
    } else {
        // Subsequent fragments - append to existing data
        if (buf->dataLen + dataLen > sizeof(buf->data)) {
            BITCHAT_DEBUG_PRINTLN("FRAGMENT: Overflow (%u + %u > %u)",
                                  (unsigned)buf->dataLen, (unsigned)dataLen, (unsigned)sizeof(buf->data));
            buf->active = false;
            return;
        }
        memcpy(&buf->data[buf->dataLen], &msg.payload[dataOffset], dataLen);
        buf->dataLen += dataLen;
    }

    buf->receivedMask |= (1 << fragmentIndex);
    buf->receivedCount++;

    // Check if complete
    if (buf->receivedCount == buf->totalFragments) {
        // Reassembly complete!
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Reassembly complete (%u bytes)", (unsigned)buf->dataLen);

        // Copy buffer data before releasing (parseMessage may be slow)
        // Use global buffer to avoid stack overflow on NRF52
        size_t dataLen = buf->dataLen;
        if (dataLen > sizeof(g_reassembledData)) {
            BITCHAT_DEBUG_PRINTLN("FRAGMENT: Data too large (%u > %u)",
                                  (unsigned)dataLen, (unsigned)sizeof(g_reassembledData));
            buf->active = false;
            return;
        }
        memcpy(g_reassembledData, buf->data, dataLen);

        // Release buffer before processing
        buf->active = false;

        // Use global buffer to avoid stack overflow on NRF52
        if (!BitchatProtocol::parseMessage(g_reassembledData, dataLen, g_reassembledBuffer)) {
            BITCHAT_DEBUG_PRINTLN("FRAGMENT: Parse failed");
            return;
        }

        if (!BitchatProtocol::validateMessage(g_reassembledBuffer)) {
            BITCHAT_DEBUG_PRINTLN("FRAGMENT: Validation failed");
            return;
        }

        // Queue the reassembled message for processing in loop() to avoid re-entrant call chain
        _reassembledMsg = g_reassembledBuffer;
        _hasReassembledMsg = true;
    }
}

// ============================================================================
// Meshcore → Bitchat
// ============================================================================

void BitchatBridge::onMeshcoreGroupMessage(const mesh::GroupChannel& channel, uint32_t timestamp,
                                            const char* senderName, const char* text) {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    Serial.println("BITCHAT_BRIDGE: >>> onMeshcoreGroupMessage() ENTRY <<<");
    BITCHAT_DEBUG_PRINTLN("MESH_RX: sender=%s text=%s", senderName, text ? text : "(null)");
    BITCHAT_DEBUG_PRINTLN("MESH_RX: hasClient=%d, BLEactive=%d",
                          _bleService.hasConnectedClient() ? 1 : 0,
                          _bleService.isActive() ? 1 : 0);
    if (text != nullptr) {
        BITCHAT_PACKETDUMP("MESH_RX", (const uint8_t*)text, strlen(text));
    }

    // IMPORTANT: Only relay #mesh channel messages to Bitchat
    if (!isMeshChannel(channel)) {
        // Not the #mesh channel - don't relay
        BITCHAT_DEBUG_PRINTLN("Filtering non-#mesh MeshCore group message");
        return;
    }

    // Check if this message originated from Bitchat (has phone emoji prefix)
    // to prevent rebroadcast loops. UTF-8 phone emoji (📱) is 4 bytes: 0xF0 0x9F 0x93 0xB1
    if (text != nullptr && strlen(text) >= 4) {
        if ((uint8_t)text[0] == 0xF0 && (uint8_t)text[1] == 0x9F &&
            (uint8_t)text[2] == 0x93 && (uint8_t)text[3] == 0xB1) {
            BITCHAT_DEBUG_PRINTLN("Skipping relay - message originated from Bitchat");
            return;
        }
    }

    // Build simple message content: "<senderName> text"
    // Bitchat displays MESSAGE payload as plain text
    // Use global buffer to avoid stack overflow on NRF52
    snprintf(g_meshTxContent, sizeof(g_meshTxContent), "<%s> %s", senderName, text);

    // Create Bitchat message - use global buffer to avoid stack overflow
    g_msgBuffer.version = BITCHAT_VERSION;
    g_msgBuffer.type = BITCHAT_MSG_MESSAGE;
    g_msgBuffer.ttl = DEFAULT_TTL;
    g_msgBuffer.timestamp = getCurrentTimeMs();
    g_msgBuffer.flags = 0;  // No special flags - simple channel message
    g_msgBuffer.setSenderId64(_bitchatPeerId);

    // Simple payload format - just copy the text content directly
    size_t contentLen = strlen(g_meshTxContent);
    if (contentLen > BITCHAT_MAX_PAYLOAD_SIZE) {
        contentLen = BITCHAT_MAX_PAYLOAD_SIZE;
    }
    memcpy(g_msgBuffer.payload, g_meshTxContent, contentLen);
    g_msgBuffer.payloadLength = static_cast<uint16_t>(contentLen);

    // Sign the message
    signMessage(g_msgBuffer);

    // Add to message history for REQUEST_SYNC responses
    addToMessageHistory(g_msgBuffer);

    // CRITICAL: Add to dedup cache BEFORE broadcasting to prevent re-relay loop
    // When this message comes back via sync from other Bitchat peers, we'll recognize
    // it as a duplicate and not relay it again to MeshCore
    _duplicateCache.addMessage(g_msgBuffer);

    BITCHAT_PACKETDUMP("BLE_TX_FROM_MESH", g_msgBuffer.payload, g_msgBuffer.payloadLength);
    bool sent = _bleService.broadcastMessage(g_msgBuffer);
    BITCHAT_DEBUG_PRINTLN("TX to Bitchat: %s (result=%d, added to dedup)", senderName, sent ? 1 : 0);
    Serial.println("BITCHAT_BRIDGE: <<< onMeshcoreGroupMessage() EXIT <<<");
#endif
}

void BitchatBridge::broadcastToBitchat(const BitchatMessage& msg) {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    _bleService.broadcastMessage(msg);
#endif
}
