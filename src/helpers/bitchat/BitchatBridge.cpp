#include "BitchatBridge.h"
#include "BitchatDebugSerial.h"
#include "BitchatTextSplit.h"
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
  #define BITCHAT_DEBUG_PRINTLN(...) do { BITCHAT_SERIAL.printf("BITCHAT_BRIDGE: "); BITCHAT_SERIAL.printf(__VA_ARGS__); BITCHAT_SERIAL.println(); } while(0)
#else
  #define BITCHAT_DEBUG_PRINTLN(...) {}
#endif

// Verbose packet hex dump macro (separate from BITCHAT_DEBUG for optional verbosity)
#if BITCHAT_DEBUG_PACKETDUMP
static void dumpPacketHex(const char* label, const uint8_t* data, size_t len) {
    BITCHAT_SERIAL.printf("PACKETDUMP [%s] (%u bytes):\n", label, (unsigned)len);
    for (size_t i = 0; i < len; i++) {
        BITCHAT_SERIAL.printf("%02X ", data[i]);
        if ((i + 1) % 16 == 0) BITCHAT_SERIAL.println();
    }
    if (len % 16 != 0) BITCHAT_SERIAL.println();
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
      BITCHAT_SERIAL.printf("STACK_CHECKPOINT [%s]: used=%u/%u (%u%%), free=%u\n", \
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
uint8_t BitchatBridge::_reassembledRaw[BitchatBridge::REASSEMBLY_BUFFER_SIZE];
BitchatBridge::PeerInfo BitchatBridge::_peerCache[PEER_CACHE_SIZE];

// Decompression buffers (statically allocated at compile time, shared for all decompressions)
#if defined(NRF52_PLATFORM)
tinfl_decompressor BitchatBridge::_decompressorStatic;
uint8_t BitchatBridge::_decompBufferStatic[BITCHAT_MAX_PAYLOAD_SIZE];
#endif

// Module-scope static buffers (outside the stack, safe on NRF52)
static BitchatMessage g_rxBuffer;            // For incoming packet parsing
static BitchatMessage g_msgBuffer;           // For announcements/messages
static BitchatMessage g_pongBuffer;          // For PING responses
static uint8_t g_wireBuffer[512];            // For serialization / sync replay
static uint8_t g_signData[512];              // For message signing
static char g_senderNick[64];                // For message parsing
static char g_messageContent[BITCHAT_MAX_PAYLOAD_SIZE + 1];  // For message content
static char g_channelName[32];               // For channel name
static uint8_t g_meshPayload[MAX_PACKET_PAYLOAD];  // LoRa TX scratch (off nRF52 loop stack)
static char g_prefixedSender[68];
static char g_splitChunk[180];

// Outbound (MeshCore -> Bitchat) pacing queue, see BitchatBridge.h "Outbound pacing".
// One slot holds one chunk of at most BITCHAT_MAX_PUBLIC_PAYLOAD bytes (BitchatTextSplit.h).
#if defined(NRF52_PLATFORM)
  #define BITCHAT_TX_QUEUE_BYTES 1200
#else
  #define BITCHAT_TX_QUEUE_BYTES 3200
#endif
#define BITCHAT_TX_SLOT_SIZE (BITCHAT_MAX_PUBLIC_PAYLOAD + 1)
#define BITCHAT_TX_QUEUE_SIZE ((BITCHAT_TX_QUEUE_BYTES / BITCHAT_TX_SLOT_SIZE) < 4 ? 4 : (BITCHAT_TX_QUEUE_BYTES / BITCHAT_TX_SLOT_SIZE))
static char g_txQueue[BITCHAT_TX_QUEUE_SIZE][BITCHAT_TX_SLOT_SIZE];
static size_t g_txQueueHead = 0;   // oldest entry
static size_t g_txQueueCount = 0;

// Directly received ANNOUNCE waiting for signature verification in loop(). Ed25519 verify
// needs ~3 KB of stack, too much for the BLE receive call chain on NRF52.
static struct {
    bool pending;
    uint64_t peerId;
    uint64_t timestamp;     // announce timestamp (ms)
    uint32_t rxMillis;      // millis() at receive, to age the timestamp until verified
    uint8_t signingKey[32];
    uint8_t signature[BITCHAT_SIGNATURE_SIZE];
    uint16_t dataLen;       // signing preimage length (padded)
} g_pendingTimeSync;
static uint8_t g_timeSyncData[512];         // signing preimage of the pending announce
static uint32_t g_lastTimeSyncVerifyMs = 0;  // millis() of the last verification attempt
static const uint32_t TIME_SYNC_VERIFY_INTERVAL_MS = 10000;

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
                             const uint8_t& pathHashMode, const char* regionName,
                             const uint8_t* defaultScopeKey)
    : _mesh(mesh)
    , _identity(identity)
    , _nodeName(nodeName)
    , _pathHashMode(pathHashMode)
    , _regionName(regionName)
    , _defaultScopeKey(defaultScopeKey)
    , _bitchatPeerId(0)
    , _channelConfigured(false)
    , _lastAnnounceTime(0)
    , _pendingAnnounce(false)
    , _pendingSyncAnnounce(false)
    , _processingMessage(false)
    , _reassembledRawLen(0)
    , _hasReassembledMsg(false)
    , _timeOffset(0)
    , _lastMillis32(0)
    , _millisHigh(0)
    , _timeSynced(false)
    , _timeSyncVerified(false)
    , _rtcSetByBridge(false)
    , _bootTimestamp(0)
    , _lastTxTimestamp(0)
    , _messagesRelayed(0)
    , _duplicatesDropped(0)
    , _meshChannelConfigured(false)
    , _meshChannelIndex(-1)
    , _messageHistoryHead(0)
    , _txMsgTokens(TX_MSG_CAPACITY)
    , _txCharTokens(TX_CHAR_CAPACITY)
    , _txLinkTokens(TX_LINK_CAPACITY)
    , _txLastRefillMs(0)
    , _txDropped(0)
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
        _messageHistory[i].len = 0;
        _messageHistory[i].timestamp = 0;
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
    BITCHAT_SERIAL.printf("BITCHAT_BRIDGE: Decompression buffers statically allocated (%u bytes)\n",
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

void BitchatBridge::addToMessageHistory(const BitchatMessage& msg, const uint8_t* raw, size_t rawLen) {
    if (raw == nullptr || rawLen == 0 || rawLen > HISTORY_SLOT_SIZE) {
        return;
    }
    CachedMessage& c = _messageHistory[_messageHistoryHead];
    BitchatProtocol::computePacketId(msg, c.packetId);
    c.timestamp = msg.timestamp;
    memcpy(c.wire, raw, rawLen);
    c.len = static_cast<uint16_t>(rawLen);
    c.addedTimeMs = millis();
    _messageHistoryHead = (_messageHistoryHead + 1) % MESSAGE_HISTORY_SIZE;
}

// ============================================================================
// GCS Filter Implementation for REQUEST_SYNC
// Matches Android RequestSyncPacket.kt / GCSFilter.kt and iOS RequestSyncPacket.swift / GCSFilter.swift
// ============================================================================

#define GCS_TLV_P     0x01  // Golomb-Rice parameter (1 byte, 1..32)
#define GCS_TLV_M     0x02  // Hash range M = N * 2^P (4 bytes BE, > 1)
#define GCS_TLV_DATA  0x03  // Encoded Golomb-Rice bitstream
#define GCS_TLV_SINCE 0x05  // Optional sinceTimestamp cursor (8 bytes BE, ms)
#define GCS_MAX_P     32
#define GCS_MAX_DATA  1024

bool BitchatBridge::parseGCSFilter(const uint8_t* payload, size_t len, GCSFilter& outFilter) {
    outFilter.p = 0;
    outFilter.m = 0;
    outFilter.data = nullptr;
    outFilter.dataLen = 0;
    outFilter.sinceTimestamp = 0;
    outFilter.hasSince = false;

    if (payload == nullptr || len < 3) {
        return false;
    }

    size_t offset = 0;
    bool hasP = false, hasM = false, hasData = false;

    // RequestSyncPacket TLV: [type:1][length:2 BE][value]
    while (offset + 3 <= len) {
        uint8_t type = payload[offset++];
        uint16_t length = BitchatProtocol::readBE16(&payload[offset]);
        offset += 2;

        if (offset + length > len) {
            return false;
        }

        if (type == GCS_TLV_P && length == 1) {
            outFilter.p = payload[offset];
            hasP = true;
        } else if (type == GCS_TLV_M && length == 4) {
            outFilter.m = BitchatProtocol::readBE32(&payload[offset]);
            hasM = true;
        } else if (type == GCS_TLV_DATA) {
            if (length > GCS_MAX_DATA) return false;
            outFilter.data = &payload[offset];
            outFilter.dataLen = length;
            hasData = true;
        } else if (type == GCS_TLV_SINCE && length == 8) {
            outFilter.sinceTimestamp = BitchatProtocol::readBE64(&payload[offset]);
            outFilter.hasSince = true;
        }
        offset += length;
    }

    return hasP && hasM && hasData && outFilter.p >= 1 && outFilter.p <= GCS_MAX_P && outFilter.m > 1;
}

bool BitchatBridge::GCSFilter::mightContain(const uint8_t* packetId16) const {
    if (data == nullptr || p < 1 || p > GCS_MAX_P || m <= 1) {
        return false;
    }

    // h64(id16) = first 8 bytes of SHA-256(id16) as big-endian unsigned 63-bit integer
    uint8_t digest[8];
    mesh::Utils::sha256(digest, sizeof(digest), packetId16, 16);
    uint64_t h64 = BitchatProtocol::readBE64(digest) & 0x7FFFFFFFFFFFFFFFULL;
    uint64_t target = h64 % static_cast<uint64_t>(m);
    if (target == 0) target = 1;

    size_t bit = 0;
    size_t totalBits = dataLen * 8;
    uint64_t acc = 0;

    while (bit < totalBits) {
        uint64_t q = 0;
        for (;;) {
            if (bit >= totalBits) return false;
            int b = (data[bit / 8] >> (7 - (bit % 8))) & 1;
            bit++;
            if (b == 0) break;
            if (++q > m) return false;
        }
        if (bit + p > totalBits) return false;
        uint64_t r = 0;
        for (uint8_t k = 0; k < p; k++) {
            r = (r << 1) | (static_cast<uint64_t>(data[bit / 8] >> (7 - (bit % 8))) & 1ULL);
            bit++;
        }
        uint64_t x = (q << p) + r + 1;
        acc += x;
        if (acc >= static_cast<uint64_t>(m)) return false;
        if (acc == target) return true;
        if (acc > target) return false;
    }

    return false;
}

void BitchatBridge::handleRequestSync(const BitchatMessage& msg) {
    STACK_CHECKPOINT("handleRequestSync() entry");

    BITCHAT_DEBUG_PRINTLN("REQUEST_SYNC from %08lX", (unsigned long)(msg.getSenderId64() & 0xFFFFFFFF));

    if (msg.hasRecipient()) {
        uint64_t to = msg.getRecipientId64();
        if (to != _bitchatPeerId && to != 0xFFFFFFFFFFFFFFFFULL) {
            BITCHAT_DEBUG_PRINTLN("REQUEST_SYNC for another peer, ignored");
            return;
        }
    }

    GCSFilter filter;
    bool hasFilter = parseGCSFilter(msg.payload, msg.payloadLength, filter);

    uint32_t now = millis();
    for (size_t i = 0; i < MESSAGE_HISTORY_SIZE; i++) {
        if (_messageHistory[i].len > 0 &&
            (now - _messageHistory[i].addedTimeMs) > MESSAGE_EXPIRY_MS) {
            _messageHistory[i].len = 0;
        }
    }

    int sent = 0;
    int skipped = 0;
    for (size_t i = 0; i < MESSAGE_HISTORY_SIZE; i++) {
        CachedMessage& c = _messageHistory[i];
        if (c.len == 0) continue;

        if (hasFilter) {
            if (filter.hasSince && c.timestamp < filter.sinceTimestamp) {
                skipped++;
                continue;
            }
            if (filter.mightContain(c.packetId)) {
                skipped++;
                continue;
            }
        }

#if defined(ESP32) || defined(NRF52_PLATFORM)
        // Replay exact wire bytes with TTL 0 so compressed/v2 signatures stay valid
        memcpy(g_wireBuffer, c.wire, c.len);
        g_wireBuffer[2] = 0;
        _bleService.broadcastRaw(g_wireBuffer, c.len);
        sent++;
#endif
    }

    // Reply with TTL 0 (SYNC_TTL_HOPS) so Android's DirectLinkAnnouncementPolicy
    // does not re-trigger scheduleInitialSyncToPeer() in a loop.
    _pendingSyncAnnounce = true;

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
        BITCHAT_SERIAL.println("DEBUG_CRASH: BitchatBridge::loop after _bleService.loop()");
        BITCHAT_SERIAL.flush();
    }

    // Process pending relays (multi-bridge collision avoidance with random delay)
    processPendingRelays();

    if (firstLoopAfterConnect) {
        BITCHAT_SERIAL.println("DEBUG_CRASH: BitchatBridge::loop after processPendingRelays()");
        BITCHAT_SERIAL.flush();
    }

    // Process pending multi-part messages
    processPendingParts();

    // Verify a deferred announce and maybe take the time from it (needs a shallow stack)
    processPendingTimeSync();

    // Release paced MeshCore -> Bitchat messages
    processOutboundQueue();

    // Process queued reassembled fragments (deferred to avoid re-entrant call chain)
    processReassembled();

    if (firstLoopAfterConnect) {
        BITCHAT_SERIAL.println("DEBUG_CRASH: BitchatBridge::loop after processPendingParts()");
        BITCHAT_SERIAL.flush();
        firstLoopAfterConnect = false;
    }

    uint32_t now = millis();

    // Handle deferred announcement (from BLE callback - limited stack)
    // Defer announcements if message processing is active to prevent deep call stack overlap
    if (_pendingAnnounce && !_processingMessage) {
        STACK_CHECKPOINT("loop() before sendPeerAnnouncement");
        _pendingAnnounce = false;
        _pendingSyncAnnounce = false;
        sendPeerAnnouncement(DEFAULT_TTL);
        _lastAnnounceTime = now;
        STACK_CHECKPOINT("loop() after sendPeerAnnouncement");
    } else if (_pendingSyncAnnounce && !_processingMessage) {
        _pendingSyncAnnounce = false;
        sendPeerAnnouncement(0);
    }

    // Periodically expire old messages from cache (every 30 seconds)
    static uint32_t lastExpiryCheck = 0;
    if (now - lastExpiryCheck >= 30000) {
        lastExpiryCheck = now;
        for (size_t i = 0; i < MESSAGE_HISTORY_SIZE; i++) {
            if (_messageHistory[i].len > 0 &&
                (now - _messageHistory[i].addedTimeMs) > MESSAGE_EXPIRY_MS) {
                _messageHistory[i].len = 0;
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
    // Initialize BLE independently (no SerialBLEInterface).
    // MTU 517 is requested inside BitchatBLEService::attachToServer().
    BLEDevice::init(deviceName);

    // Create BLE server
    BLEServer* server = BLEDevice::createServer();
    if (server == nullptr) {
        return false;
    }

    // Register server callbacks to restart advertising on connect/disconnect
    // Static instance avoids heap allocation
    static StandaloneBLEServerCallbacks s_standaloneCallbacks(&_bleService);
    server->setCallbacks(&s_standaloneCallbacks);

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
    BITCHAT_SERIAL.println("BITCHAT_BRIDGE: beginStandalone (NRF52) entry");
    BITCHAT_SERIAL.print("BITCHAT_BRIDGE: deviceName=");
    BITCHAT_SERIAL.println(deviceName);
    BITCHAT_SERIAL.print("BITCHAT_BRIDGE: this=");
    BITCHAT_SERIAL.println((unsigned long)this, HEX);
    BITCHAT_SERIAL.flush();

    // Initialize Bluefruit BLE with Bitchat service
    BITCHAT_SERIAL.println("BITCHAT_BRIDGE: Calling _bleService.beginStandalone()");
    BITCHAT_SERIAL.flush();
    bool result = _bleService.beginStandalone(deviceName, this);
    BITCHAT_SERIAL.print("BITCHAT_BRIDGE: _bleService.beginStandalone() returned: ");
    BITCHAT_SERIAL.println(result);
    BITCHAT_SERIAL.flush();

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

    // Find an empty slot or the oldest entry (wraparound-safe age comparison)
    size_t targetIdx = 0;
    uint32_t maxAge = 0;
    bool hasCandidate = false;
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        if (!_peerCache[i].valid) {
            targetIdx = i;
            break;
        }
        uint32_t age = now - _peerCache[i].timestamp;
        if (!hasCandidate || age > maxAge) {
            maxAge = age;
            targetIdx = i;
            hasCandidate = true;
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

void BitchatBridge::evictPeer(uint64_t peerId) {
    for (size_t i = 0; i < PEER_CACHE_SIZE; i++) {
        if (_peerCache[i].valid && _peerCache[i].peerId == peerId) {
            _peerCache[i].valid = false;
            return;
        }
    }
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

// A clock earlier than this firmware's build date is wrong. This also rejects the seeds
// boards fall back to after losing time: 1970, VolatileRTCClock's May 2024 and
// ESP32Board's RTC_TIME_MIN (1 Mar 2026). Two days of slack cover the rough
// getCompileTimeMs() calendar maths.
static uint32_t minPlausibleEpoch() {
    static uint32_t minEpoch = 0;
    if (minEpoch == 0) {
        minEpoch = static_cast<uint32_t>(getCompileTimeMs() / 1000ULL) - 2 * 86400UL;
    }
    return minEpoch;
}

static bool isPlausibleEpoch(uint32_t secs) {
    const uint32_t MAX_PLAUSIBLE_EPOCH = 4102444800UL;  // 2100-01-01
    return secs >= minPlausibleEpoch() && secs < MAX_PLAUSIBLE_EPOCH;
}

BitchatTimeSyncPolicy::Trust BitchatBridge::clockTrust() {
    if (_timeSynced) {
        return _timeSyncVerified ? BitchatTimeSyncPolicy::CLOCK_TRUSTED : BitchatTimeSyncPolicy::CLOCK_WEAK;
    }
    mesh::RTCClock* rtc = _mesh.getRTCClock();
    if (rtc != NULL && isPlausibleEpoch(rtc->getCurrentTime())) {
        return BitchatTimeSyncPolicy::CLOCK_TRUSTED;
    }
    return BitchatTimeSyncPolicy::CLOCK_UNTRUSTED;
}

uint64_t BitchatBridge::uptimeMs() {
#ifdef ARDUINO
    uint32_t now = millis();
    if (now < _lastMillis32) {
        _millisHigh += 0x100000000ULL;
    }
    _lastMillis32 = now;
    return _millisHigh | now;
#else
    return 0;
#endif
}

void BitchatBridge::adoptTime(uint64_t packetTimestamp, bool verified) {
#ifdef ARDUINO
    int64_t before = static_cast<int64_t>(getCurrentTimeMs());
    _timeOffset = static_cast<int64_t>(packetTimestamp) - static_cast<int64_t>(uptimeMs());
    if (!_timeSynced) {
        // Used to drop messages older than our first sync (stale history after reboot)
        _bootTimestamp = packetTimestamp;
    }
    _timeSynced = true;
    _timeSyncVerified = verified;
    BITCHAT_DEBUG_PRINTLN("Time synced from %s announce: moved %ld s",
                          verified ? "verified" : "unverified",
                          (long)((static_cast<int64_t>(packetTimestamp) - before) / 1000));

    // Set the MeshCore RTC only when it could not be trusted (1970 / seed date after a
    // reset), or when it was the bridge that set it. An RTC set by the companion app, GPS
    // or an RTC chip is left alone.
    mesh::RTCClock* rtc = _mesh.getRTCClock();
    if (rtc != nullptr) {
        uint32_t secs = static_cast<uint32_t>(packetTimestamp / 1000ULL);
        uint32_t rtcNow = rtc->getCurrentTime();
        if (!isPlausibleEpoch(rtcNow) || _rtcSetByBridge) {
            int64_t diff = static_cast<int64_t>(secs) - static_cast<int64_t>(rtcNow);
            if (diff > 30 || diff < -30) {
                rtc->setCurrentTime(secs);
                _rtcSetByBridge = true;
                BITCHAT_DEBUG_PRINTLN("RTC set from Bitchat: %lu", (unsigned long)secs);
            }
        }
    }
#endif
}

void BitchatBridge::onDirectAnnounceTime(const BitchatMessage& msg) {
#ifdef ARDUINO
    const uint64_t MIN_VALID_TIMESTAMP = 1704067200000ULL;  // 2024-01-01
    const uint64_t MAX_VALID_TIMESTAMP = 4102444800000ULL;  // 2100-01-01
    if (msg.timestamp < MIN_VALID_TIMESTAMP || msg.timestamp > MAX_VALID_TIMESTAMP) return;

    uint64_t peerId = msg.getSenderId64();
    BitchatTimeSyncPolicy::Trust trust = clockTrust();
    int64_t delta = static_cast<int64_t>(msg.timestamp) - static_cast<int64_t>(getCurrentTimeMs());

    // An untrustworthy clock takes any direct announce right away: no app accepts our
    // announces with a 1970 or seed clock anyway. Verification below may upgrade it.
    if (_timePolicy.shouldAdopt(trust, false, delta, peerId, millis())) {
        adoptTime(msg.timestamp, false);
        trust = clockTrust();
        delta = 0;
    }
    if (!BitchatTimeSyncPolicy::needsVerification(trust, delta)) return;
    // One at a time and at most every 10 s; announces repeat every few seconds anyway
    if (g_pendingTimeSync.pending) return;
    if (g_lastTimeSyncVerifyMs != 0 && millis() - g_lastTimeSyncVerifyMs < TIME_SYNC_VERIFY_INTERVAL_MS) return;

    // Signature check is only possible when we can rebuild the signed bytes exactly:
    // an uncompressed v1/v2 packet.
    if (!msg.hasSignature() || msg.isCompressed() || msg.wirePayloadLength != msg.payloadLength) {
        return;
    }

    // The announce must carry its keys, and the sender ID must be derived from the noise key
    uint8_t keyLen = 0;
    const uint8_t* noiseKey = BitchatProtocol::findTLV(msg.payload, msg.payloadLength, BITCHAT_TLV_NOISE_PUBKEY, &keyLen);
    if (noiseKey == nullptr || keyLen != 32) return;
    const uint8_t* signingKey = BitchatProtocol::findTLV(msg.payload, msg.payloadLength, BITCHAT_TLV_ED25519_PUBKEY, &keyLen);
    if (signingKey == nullptr || keyLen != 32) return;
    if (derivePeerId(noiseKey) != peerId) return;

    size_t len = BitchatProtocol::signingPreimage(msg, g_timeSyncData, sizeof(g_timeSyncData));
    if (len == 0) return;

    g_pendingTimeSync.peerId = peerId;
    g_pendingTimeSync.timestamp = msg.timestamp;
    g_pendingTimeSync.rxMillis = millis();
    memcpy(g_pendingTimeSync.signingKey, signingKey, 32);
    memcpy(g_pendingTimeSync.signature, msg.signature, BITCHAT_SIGNATURE_SIZE);
    g_pendingTimeSync.dataLen = static_cast<uint16_t>(len);
    g_pendingTimeSync.pending = true;
#endif
}

void BitchatBridge::processPendingTimeSync() {
#ifdef ARDUINO
    if (!g_pendingTimeSync.pending) return;
    g_pendingTimeSync.pending = false;
    g_lastTimeSyncVerifyMs = millis() | 1;  // never 0

    mesh::Identity signer(g_pendingTimeSync.signingKey);
    if (!signer.verify(g_pendingTimeSync.signature, g_timeSyncData, g_pendingTimeSync.dataLen)) {
        BITCHAT_DEBUG_PRINTLN("Announce signature invalid, not used for time");
        return;
    }

    // Age the timestamp by the time it waited here
    uint32_t now = millis();
    uint64_t ts = g_pendingTimeSync.timestamp + (now - g_pendingTimeSync.rxMillis);
    BitchatTimeSyncPolicy::Trust trust = clockTrust();
    int64_t delta = static_cast<int64_t>(ts) - static_cast<int64_t>(getCurrentTimeMs());
    if (_timePolicy.shouldAdopt(trust, true, delta, g_pendingTimeSync.peerId, now)) {
        adoptTime(ts, true);
    }
#endif
}

uint64_t BitchatBridge::getCurrentTimeMs() {
#ifdef ARDUINO
    // If we have synced time from a Bitchat client, use that (most reliable)
    if (_timeSynced) {
        return static_cast<uint64_t>(static_cast<int64_t>(uptimeMs()) + _timeOffset);
    }

    // Next best source: the Meshcore RTC (set by the companion app / GPS / RTC chip).
    // Bitchat rejects announces more than 10 minutes off wall clock, so a plausible
    // RTC value is much better than a hardcoded bootstrap date.
    mesh::RTCClock* rtc = _mesh.getRTCClock();
    if (rtc != NULL) {
        uint32_t now = rtc->getCurrentTime();
        if (isPlausibleEpoch(now)) {
            return static_cast<uint64_t>(now) * 1000ULL;
        }
    }

    // Fallback: build date + uptime. Announces will be rejected by current Bitchat
    // builds until we hear a direct announce and sync, but the bridge still receives.
    return static_cast<uint64_t>(minPlausibleEpoch()) * 1000ULL + uptimeMs();
#else
    return 0;
#endif
}

void BitchatBridge::sendPeerAnnouncement(uint8_t ttl) {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    STACK_CHECKPOINT("sendPeerAnnouncement() entry");

    uint64_t timestamp = getCurrentTimeMs();

#if BITCHAT_DEBUG
    // Print timestamp in seconds (fits in 32-bit for valid times through 2106)
    uint32_t timestampSec = (uint32_t)(timestamp / 1000ULL);
    BITCHAT_SERIAL.print("BITCHAT_BRIDGE: Announce: timestamp_sec=");
    BITCHAT_SERIAL.print(timestampSec);
    BITCHAT_SERIAL.print(" (expected ~1767000000 for Jan 2026), synced=");
    BITCHAT_SERIAL.println(_timeSynced ? 1 : 0);
#endif

    // Use global buffer to avoid stack overflow on NRF52
    BitchatProtocol::createAnnounce(
        g_msgBuffer,
        _bitchatPeerId,
        _nodeName,
        _noisePublicKey,      // Curve25519 for Noise protocol
        _identity.pub_key,    // Ed25519 for signatures
        timestamp,
        ttl
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
    BITCHAT_SERIAL.println("DEBUG_CRASH: sendPeerAnnouncement about to return");
    BITCHAT_SERIAL.flush();

    STACK_CHECKPOINT("sendPeerAnnouncement() exit");
#endif
}

void BitchatBridge::signMessage(BitchatMessage& msg) {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    STACK_CHECKPOINT("signMessage() entry");

    size_t paddedLen = BitchatProtocol::signingPreimage(msg, g_signData, sizeof(g_signData));
    if (paddedLen > 0) {
        BITCHAT_DEBUG_PRINTLN("Signing message: %u bytes", (unsigned)paddedLen);
        STACK_CHECKPOINT("signMessage() before Ed25519 sign");
        _identity.sign(msg.signature, g_signData, paddedLen);
        STACK_CHECKPOINT("signMessage() after Ed25519 sign");
        msg.setHasSignature(true);
    }

    STACK_CHECKPOINT("signMessage() exit");
#endif
}

// ============================================================================
// Bitchat → Meshcore
// ============================================================================

#if defined(ESP32) || defined(NRF52_PLATFORM)
void BitchatBridge::onBitchatRawPacket(const uint8_t* data, size_t len) {
    if (!BitchatProtocol::parseMessage(data, len, g_rxBuffer)) {
        return;
    }
    if (!BitchatProtocol::validateMessage(g_rxBuffer)) {
        return;
    }
    size_t rawLen = BitchatProtocol::frameLength(data, len);
    if (rawLen == 0) rawLen = len;
    processBitchatMessage(g_rxBuffer, data, rawLen);
    processReassembled();
}

void BitchatBridge::onBitchatClientConnect() {
    STACK_CHECKPOINT("onBitchatClientConnect()");
    BITCHAT_DEBUG_PRINTLN("Client connected");
    // Defer announcement to loop() to avoid deep call stack in callback
    if ((uint32_t)(millis() - _lastAnnounceTime) >= 2000) {
        _pendingAnnounce = true;
    }
}

void BitchatBridge::onBitchatClientDisconnect() {
    BITCHAT_DEBUG_PRINTLN("Bitchat client disconnected");
}

void BitchatBridge::onBitchatClientSubscribed() {
    BITCHAT_DEBUG_PRINTLN("Bitchat client subscribed to notifications");
    _pendingAnnounce = true;
}
#endif

void BitchatBridge::processBitchatMessage(const BitchatMessage& msg, const uint8_t* raw, size_t rawLen) {
    STACK_CHECKPOINT("processBitchatMessage() entry");

    _processingMessage = true;  // Set guard to prevent announcement during processing

    uint64_t msgSender = msg.getSenderId64();
    BITCHAT_SERIAL.printf("PROCESS_MSG: type=0x%02X payloadLen=%u sender=%08lX ts=%lu\n",
                  msg.type, msg.payloadLength,
                  (unsigned long)(msgSender & 0xFFFFFFFF),
                  (unsigned long)(msg.timestamp / 1000ULL));
    BITCHAT_SERIAL.flush();

    // Full payload hex dump for MESSAGE types
    if (msg.type == BITCHAT_MSG_MESSAGE) {
        BITCHAT_SERIAL.println("REDUNDANT_DEBUG: ====== MESSAGE PAYLOAD HEX ======");
        for (uint16_t i = 0; i < msg.payloadLength && i < 256; i++) {
            BITCHAT_SERIAL.printf("%02X", msg.payload[i]);
        }
        BITCHAT_SERIAL.println();
        BITCHAT_SERIAL.println("REDUNDANT_DEBUG: ====== END MESSAGE PAYLOAD ======");
        BITCHAT_SERIAL.flush();
    }

    BITCHAT_PACKETDUMP("BLE_RX", msg.payload, msg.payloadLength);

    // Check for duplicates - log the hash inputs for debugging
#if BITCHAT_DEBUG_PACKETDUMP
    {
        uint64_t senderId = msg.getSenderId64();
        uint32_t timestampSecs = static_cast<uint32_t>(msg.timestamp / 1000ULL);
        BITCHAT_SERIAL.printf("DEDUP_CHECK: sender=%08lX ts_sec=%lu type=%02X payloadLen=%u\n",
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

                // Ignore DMs and encrypted/empty TLV payloads
                if (parsedAsTlv && (msg.hasRecipient() || g_messageContent[0] == '\0')) {
                    BITCHAT_DEBUG_PRINTLN("Ignoring DM or encrypted/empty TLV message");
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

                // Add to message history for REQUEST_SYNC responses (raw wire bytes preserve signature)
                addToMessageHistory(msg, raw, rawLen);
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
            // Only an announce heard directly (TTL still at the app's maximum) is fresh enough
            // to set the clock from; relayed and synced packets carry older timestamps.
            if (msg.ttl == DEFAULT_TTL) {
                onDirectAnnounceTime(msg);
            }
            break;
        }

        case BITCHAT_MSG_LEAVE:
            evictPeer(msg.getSenderId64());
            break;

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
        case BITCHAT_MSG_COURIER_ACK_A:
        case BITCHAT_MSG_COURIER_ACK_B:
        case BITCHAT_MSG_ANNOUNCE_V2:
        case BITCHAT_MSG_VIDEO_FRAME:
            // Live push-to-talk audio / live video (ephemeral, far too large for LoRa), and
            // types the apps reserved but do not emit yet. validateMessage() already drops
            // these at parse time; listed here so they are never bridged.
            BITCHAT_DEBUG_PRINTLN("Skipping message type 0x%02X (not bridged)", msg.type);
            break;

        case BITCHAT_MSG_FRAGMENT_NEW:
        case BITCHAT_MSG_FRAGMENT:
            // Fragment messages are used for long text messages (>245 bytes)
            // Reassemble and process when complete
            handleFragment(msg);
            break;

        case BITCHAT_MSG_REQUEST_SYNC:
            handleRequestSync(msg);
            BITCHAT_SERIAL.println("BITCHAT_BRIDGE: handleRequestSync() returned OK");
            break;

        default:
            BITCHAT_DEBUG_PRINTLN("Unhandled message type: 0x%02X", msg.type);
            break;
    }
    BITCHAT_SERIAL.println("BITCHAT_BRIDGE: processBitchatMessage() COMPLETE");

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

    if (hasOriginalSender) {
        if (offset >= payloadLen) return false;
        uint8_t len = payload[offset++];
        if (offset + len > payloadLen) return false;
        offset += len;
    }

    if (hasRecipientNickname) {
        if (offset >= payloadLen) return false;
        uint8_t len = payload[offset++];
        if (offset + len > payloadLen) return false;
        offset += len;
    }

    if (hasSenderPeerID) {
        if (offset >= payloadLen) return false;
        uint8_t len = payload[offset++];
        if (offset + len > payloadLen) return false;
        offset += len;
    }

    if (hasMentions) {
        if (offset >= payloadLen) return false;
        uint8_t mentionCount = payload[offset++];
        for (uint8_t i = 0; i < mentionCount; i++) {
            if (offset >= payloadLen) return false;
            uint8_t len = payload[offset++];
            if (offset + len > payloadLen) return false;
            offset += len;
        }
    }

    // Read channel if present
    if (hasChannel) {
        if (offset >= payloadLen) return false;
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

    // Sanity check: only clamp against local clock when our clock is plausible; avoid
    // unsigned underflow if RTC is near 0 on cold boot.
    uint32_t now = _timeSynced ? static_cast<uint32_t>(getCurrentTimeMs() / 1000ULL)
                               : (_mesh.getRTCClock() ? _mesh.getRTCClock()->getCurrentTime() : 0);
    bool clockOk = _timeSynced || isPlausibleEpoch(now);
    bool tsOk = (timestamp != 0) &&
                (!clockOk || (timestamp <= now + 60 && now <= timestamp + 3600));
    if (!tsOk) {
        if (clockOk || !isPlausibleEpoch(timestamp)) {
            BITCHAT_DEBUG_PRINTLN("Invalid original timestamp %u, using current %u", timestamp, now);
            timestamp = now;
        }
    }

    // Build Meshcore group message payload (module-scope buffers keep nRF52 loop stack lean)
    // Format: timestamp(4) + txt_type(1) + "📱 sender: text"
    uint8_t* payload = g_meshPayload;
    size_t offset = 0;

    // Timestamp (4 bytes)
    memcpy(&payload[offset], &timestamp, 4);
    offset += 4;

    // Text type (0 = plain text)
    payload[offset++] = 0;

    // Add 📱 prefix to sender name (identifies Bitchat origin)
    // 📱 = UTF-8: F0 9F 93 B1 (4 bytes)
    snprintf(g_prefixedSender, sizeof(g_prefixedSender), "\xF0\x9F\x93\xB1 %s", senderNick);

    size_t senderLen = strlen(g_prefixedSender);
    size_t textLen = strlen(text);

    // Copy "📱 sender: "
    size_t available = MAX_PACKET_PAYLOAD - offset - 1;
    size_t toCopy = senderLen;
    if (toCopy > available) toCopy = available;
    memcpy(&payload[offset], g_prefixedSender, toCopy);
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
        const uint8_t pathHashSize = _pathHashMode + 1;
        TransportKey scopeKey;
        memset(scopeKey.key, 0, sizeof(scopeKey.key));
        if (_regionName != nullptr && _regionName[0] != '\0') {
            // Dedicated Bitchat region scope. Leading '#' is optional.
            char regionName[32];
            if (_regionName[0] == '#') snprintf(regionName, sizeof(regionName), "%s", _regionName);
            else snprintf(regionName, sizeof(regionName), "#%s", _regionName);
            mesh::Utils::sha256(scopeKey.key, sizeof(scopeKey.key),
                                reinterpret_cast<const uint8_t*>(regionName), strlen(regionName));
        } else if (_defaultScopeKey != nullptr) {
            // Fall back to companion's configured default flood scope (if non-null)
            memcpy(scopeKey.key, _defaultScopeKey, sizeof(scopeKey.key));
        }

        if (!scopeKey.isNull()) {
            uint16_t transportCodes[2] = {scopeKey.calcTransportCode(pkt), 0};
            _mesh.sendFlood(pkt, transportCodes, uint32_t{0}, pathHashSize);
        } else {
            _mesh.sendFlood(pkt, uint32_t{0}, pathHashSize);
        }
        _messagesRelayed++;
        BITCHAT_DEBUG_PRINTLN("Sent to mesh: %s: %s", g_prefixedSender, text);
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
        if (_pendingRelays[i].valid && (int32_t)(now - _pendingRelays[i].sendAtMillis) >= 0) {
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

    // Two-pass split so the [i/n] header matches the real number of parts after
    // word/punctuation/UTF-8 boundaries and never silently truncates a short tail.
    size_t cuts[MAX_MESSAGE_PARTS + 1];
    cuts[0] = 0;
    int numParts = 0;
    size_t offset = 0;
    while (offset < contentLen && numParts < (int)MAX_MESSAGE_PARTS) {
        size_t remaining = contentLen - offset;
        size_t chunkLen = (remaining > MAX_CHUNK_SIZE) ? MAX_CHUNK_SIZE : remaining;

        if (chunkLen < remaining) {
            size_t bestSplit = 0;

            // First pass: look for last space within chunk (word boundary)
            for (size_t i = chunkLen; i > chunkLen / 2; i--) {
                if (text[offset + i - 1] == ' ' && ((uint8_t)text[offset + i] & 0xC0) != 0x80) {
                    bestSplit = i;
                    break;
                }
            }

            // Second pass: if no space, look for punctuation
            if (bestSplit == 0) {
                for (size_t i = chunkLen; i > chunkLen / 2; i--) {
                    char c = text[offset + i - 1];
                    if ((c == '.' || c == ',' || c == '!' || c == '?' ||
                         c == ';' || c == ':' || c == '-' || c == ')' || c == ']') &&
                        ((uint8_t)text[offset + i] & 0xC0) != 0x80) {
                        bestSplit = i;
                        break;
                    }
                }
            }

            if (bestSplit > 0) {
                chunkLen = bestSplit;
            } else {
                chunkLen = BitchatTextSplitter::utf8Floor(text + offset, remaining, chunkLen);
            }
        }

        if (chunkLen == 0) {
            BITCHAT_DEBUG_PRINTLN("Error: Could not find safe split point");
            break;
        }

        offset += chunkLen;
        cuts[++numParts] = offset;
    }

    bool truncated = (offset < contentLen);
    if (truncated) {
        BITCHAT_SERIAL.printf("BITCHAT_BRIDGE: WARNING - Message too long (%d bytes), capped at %d parts\n",
                      (int)contentLen, numParts);
    }

    BITCHAT_DEBUG_PRINTLN("Splitting message from %s into %d parts (len=%d%s)",
                          senderNick, numParts, (int)contentLen, truncated ? ", TRUNCATED" : "");

    // Send part 1 immediately, queue remaining parts for delayed sending
    // This avoids overwhelming the mesh packet pool which can silently drop delayed packets
    for (int part = 0; part < numParts; part++) {
        size_t partStart = cuts[part];
        size_t chunkLen = cuts[part + 1] - partStart;

        snprintf(g_splitChunk, sizeof(g_splitChunk), "[%d/%d] %.*s",
                 part + 1, numParts, (int)chunkLen, text + partStart);

        BITCHAT_DEBUG_PRINTLN("Part %d/%d: offset=%d, len=%d",
                              part + 1, numParts, (int)partStart, (int)chunkLen);
        BITCHAT_PACKETDUMP("SPLIT_PART", (const uint8_t*)g_splitChunk, strlen(g_splitChunk));

        if (part == 0) {
            sendSingleMessageToMesh(senderNick, g_splitChunk, originalTimestamp);
            _lastPartSentTime = millis();
        } else {
            queueMessagePart(senderNick, g_splitChunk, originalTimestamp);
        }
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

    // Ignore duplicate fragment index
    if ((buf->receivedMask & (1u << fragmentIndex)) != 0) {
        return;
    }

    // Sequential reassembly requires fragments in order
    if (fragmentIndex != buf->receivedCount) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Out-of-order idx=%u (expected %u), dropping",
                              fragmentIndex, buf->receivedCount);
        buf->active = false;
        return;
    }

    // Copy fragment data (starts after 13-byte header)
    size_t dataOffset = 13;
    size_t dataLen = msg.payloadLength - dataOffset;

    if (dataLen == 0 || dataLen > 500) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Invalid dataLen=%u", (unsigned)dataLen);
        buf->active = false;
        return;
    }

    if (buf->dataLen + dataLen > sizeof(buf->data)) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Overflow (%u + %u > %u)",
                              (unsigned)buf->dataLen, (unsigned)dataLen, (unsigned)sizeof(buf->data));
        buf->active = false;
        return;
    }
    memcpy(&buf->data[buf->dataLen], &msg.payload[dataOffset], dataLen);
    buf->dataLen += dataLen;

    buf->receivedMask |= static_cast<uint16_t>(1u << fragmentIndex);
    buf->receivedCount++;

    // Check if complete
    if (buf->receivedCount == buf->totalFragments) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Reassembly complete (%u bytes)", (unsigned)buf->dataLen);

        if (buf->dataLen <= sizeof(_reassembledRaw)) {
            memcpy(_reassembledRaw, buf->data, buf->dataLen);
            _reassembledRawLen = buf->dataLen;
            _hasReassembledMsg = true;
        }
        buf->active = false;
    }
}

void BitchatBridge::processReassembled() {
    if (!_hasReassembledMsg) return;
    _hasReassembledMsg = false;  // Clear before parsing to avoid stuck state on error

    if (!BitchatProtocol::parseMessage(_reassembledRaw, _reassembledRawLen, g_rxBuffer)) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Parse failed");
        return;
    }
    if (!BitchatProtocol::validateMessage(g_rxBuffer)) {
        BITCHAT_DEBUG_PRINTLN("FRAGMENT: Validation failed");
        return;
    }
    // Reject nested fragments
    if (g_rxBuffer.type == BITCHAT_MSG_FRAGMENT || g_rxBuffer.type == BITCHAT_MSG_FRAGMENT_NEW) {
        return;
    }
    size_t rawLen = BitchatProtocol::frameLength(_reassembledRaw, _reassembledRawLen);
    if (rawLen == 0) rawLen = _reassembledRawLen;
    processBitchatMessage(g_rxBuffer, _reassembledRaw, rawLen);
}

// ============================================================================
// Meshcore → Bitchat
// ============================================================================

void BitchatBridge::onMeshcoreGroupMessage(const mesh::GroupChannel& channel, uint32_t timestamp,
                                            const char* senderName, const char* text) {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    BITCHAT_SERIAL.println("BITCHAT_BRIDGE: >>> onMeshcoreGroupMessage() ENTRY <<<");
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

    // Check if this message originated from Bitchat (has phone emoji prefix 📱 = F0 9F 93 B1)
    // to prevent multi-bridge rebroadcast loops. MyMesh splits "📱 nick: body" into
    // senderName="📱 nick" and text="body", so check both.
    auto hasPhonePrefix = [](const char* s) -> bool {
        return s != nullptr &&
               (uint8_t)s[0] == 0xF0 && (uint8_t)s[1] == 0x9F &&
               (uint8_t)s[2] == 0x93 && (uint8_t)s[3] == 0xB1;
    };
    if (hasPhonePrefix(senderName) || hasPhonePrefix(text)) {
        BITCHAT_DEBUG_PRINTLN("Skipping relay - message originated from Bitchat");
        return;
    }

    // Bitchat shows the MESSAGE payload as plain text: "<senderName> text", split into
    // chunks iOS can verify, and paced because the apps hide a sender beyond its rate budget
    enqueueBitchatOutbound(senderName, text);
    BITCHAT_SERIAL.println("BITCHAT_BRIDGE: <<< onMeshcoreGroupMessage() EXIT <<<");
#endif
}

// ============================================================================
// Outbound pacing (MeshCore -> Bitchat)
// ============================================================================

size_t BitchatBridge::countCodePoints(const char* text) {
    size_t n = 0;
    for (const uint8_t* p = (const uint8_t*)text; *p; p++) {
        if ((*p & 0xC0) != 0x80) n++;  // count lead bytes only
    }
    return n;
}

static bool isLinkLabelChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}

static bool isAsciiAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// True if s[0..len) is a bare domain: label(.label)*.tld[/...], tld >= 2 letters
static bool looksLikeBareDomain(const char* s, size_t len) {
    size_t hostEnd = 0;
    while (hostEnd < len && s[hostEnd] != '/') hostEnd++;
    size_t lastDot = hostEnd;
    size_t dots = 0;
    size_t labelLen = 0;
    for (size_t i = 0; i < hostEnd; i++) {
        if (s[i] == '.') {
            if (labelLen == 0) return false;
            lastDot = i;
            dots++;
            labelLen = 0;
        } else if (isLinkLabelChar(s[i])) {
            labelLen++;
        } else {
            return false;
        }
    }
    if (dots == 0 || labelLen == 0) return false;
    size_t tldLen = hostEnd - lastDot - 1;
    if (tldLen < 2) return false;
    for (size_t i = lastDot + 1; i < hostEnd; i++) {
        if (!isAsciiAlpha(s[i])) return false;
    }
    return true;
}

// Mirrors bitchat-android spam/LinkCounter closely enough for pacing; over-counting
// only delays a message, under-counting could get it hidden.
size_t BitchatBridge::countLinks(const char* text) {
    size_t links = 0;
    const char* p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) break;
        const char* tok = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') p++;
        size_t len = p - tok;
        // trim surrounding punctuation the regex would not consume
        while (len > 0 && strchr("(<[\"'", *tok)) { tok++; len--; }
        while (len > 0 && strchr(".,;:!?)>]\"'", tok[len - 1])) len--;
        if (len == 0) continue;
        if ((len > 7 && strncasecmp(tok, "http://", 7) == 0) ||
            (len > 8 && strncasecmp(tok, "https://", 8) == 0) ||
            (len > 4 && strncasecmp(tok, "www.", 4) == 0)) {
            links++;
            continue;
        }
        if (memchr(tok, '@', len) != nullptr) continue;  // e-mail addresses are not links
        if (looksLikeBareDomain(tok, len)) links++;
    }
    return links;
}

void BitchatBridge::enqueueBitchatOutbound(const char* senderName, const char* text) {
    BitchatTextSplitter splitter(senderName, text, BITCHAT_MAX_PUBLIC_PAYLOAD);
    size_t chunks = splitter.count();
    // All chunks or none: a message missing its middle is worse than a dropped one
    if (chunks > BITCHAT_TX_QUEUE_SIZE - g_txQueueCount) {
        _txDropped++;
        BITCHAT_DEBUG_PRINTLN("TX queue full (%u/%u), dropping MeshCore message of %u chunks (dropped=%u)",
                              (unsigned)g_txQueueCount, (unsigned)BITCHAT_TX_QUEUE_SIZE,
                              (unsigned)chunks, (unsigned)_txDropped);
        return;
    }
    for (size_t i = 0; i < chunks; i++) {
        size_t slot = (g_txQueueHead + g_txQueueCount) % BITCHAT_TX_QUEUE_SIZE;
        splitter.next(g_txQueue[slot]);
        g_txQueueCount++;
    }
    BITCHAT_DEBUG_PRINTLN("TX queued %u chunk(s) for Bitchat (%u waiting%s)", (unsigned)chunks,
                          (unsigned)g_txQueueCount, splitter.truncated() ? ", truncated" : "");
    processOutboundQueue();  // send right away if the budget allows
}

void BitchatBridge::processOutboundQueue() {
    uint32_t now = millis();
    uint32_t elapsed = now - _txLastRefillMs;
    _txLastRefillMs = now;
    if (elapsed > 120000) elapsed = 120000;  // every bucket is full after 2 minutes

    uint32_t add = elapsed;  // msg and link buckets refill 1 unit per ms
    _txMsgTokens = (_txMsgTokens + add > TX_MSG_CAPACITY) ? TX_MSG_CAPACITY : _txMsgTokens + add;
    _txLinkTokens = (_txLinkTokens + add > TX_LINK_CAPACITY) ? TX_LINK_CAPACITY : _txLinkTokens + add;
    uint32_t addChars = elapsed * TX_CHAR_REFILL_PER_MS;
    _txCharTokens = (_txCharTokens + addChars > TX_CHAR_CAPACITY) ? TX_CHAR_CAPACITY : _txCharTokens + addChars;

    if (g_txQueueCount == 0) return;

    const char* text = g_txQueue[g_txQueueHead];
    uint32_t charCost = (uint32_t)countCodePoints(text) * TX_CHAR_COST;
    uint32_t linkCost = (uint32_t)countLinks(text) * TX_LINK_COST;
    if (charCost > TX_CHAR_CAPACITY) charCost = TX_CHAR_CAPACITY;
    if (linkCost > TX_LINK_CAPACITY) linkCost = TX_LINK_CAPACITY;  // can never fit otherwise

    if (_txMsgTokens < TX_MSG_COST || _txCharTokens < charCost || _txLinkTokens < linkCost) {
        return;  // wait for the buckets to refill
    }
    _txMsgTokens -= TX_MSG_COST;
    _txCharTokens -= charCost;
    _txLinkTokens -= linkCost;

    sendTextToBitchat(text);
    g_txQueueHead = (g_txQueueHead + 1) % BITCHAT_TX_QUEUE_SIZE;
    g_txQueueCount--;
}

void BitchatBridge::sendTextToBitchat(const char* text) {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    // Strictly monotonic timestamp keeps multi-part chunks ordered within one RTC second
    uint64_t ts = getCurrentTimeMs();
    if (ts <= _lastTxTimestamp) ts = _lastTxTimestamp + 1;
    _lastTxTimestamp = ts;

    // Create Bitchat message - use global buffer to avoid stack overflow
    g_msgBuffer.version = BITCHAT_VERSION;
    g_msgBuffer.type = BITCHAT_MSG_MESSAGE;
    g_msgBuffer.ttl = DEFAULT_TTL;
    g_msgBuffer.timestamp = ts;
    g_msgBuffer.flags = 0;  // No special flags - simple channel message
    g_msgBuffer.setSenderId64(_bitchatPeerId);

    // Simple payload format - just copy the text content directly. Chunks are already at
    // most BITCHAT_MAX_PUBLIC_PAYLOAD bytes (iOS signature limit, see BitchatTextSplit.h).
    size_t contentLen = BitchatTextSplitter::utf8Floor(text, strlen(text), BITCHAT_MAX_PUBLIC_PAYLOAD);
    memcpy(g_msgBuffer.payload, text, contentLen);
    g_msgBuffer.payloadLength = static_cast<uint16_t>(contentLen);

    // Sign the message
    signMessage(g_msgBuffer);

    size_t wireLen = BitchatProtocol::serializeMessage(g_msgBuffer, g_wireBuffer, sizeof(g_wireBuffer));
    if (wireLen == 0) {
        BITCHAT_DEBUG_PRINTLN("TX to Bitchat serialize failed");
        return;
    }

    // Add exact wire bytes to message history for REQUEST_SYNC responses
    addToMessageHistory(g_msgBuffer, g_wireBuffer, wireLen);

    // CRITICAL: Add to dedup cache BEFORE broadcasting to prevent re-relay loop
    // When this message comes back via sync from other Bitchat peers, we'll recognize
    // it as a duplicate and not relay it again to MeshCore
    _duplicateCache.addMessage(g_msgBuffer);

    BITCHAT_PACKETDUMP("BLE_TX_FROM_MESH", g_msgBuffer.payload, g_msgBuffer.payloadLength);
    bool sent = _bleService.broadcastRaw(g_wireBuffer, wireLen);
    BITCHAT_DEBUG_PRINTLN("TX to Bitchat (result=%d, added to dedup)", sent ? 1 : 0);
#endif
}

void BitchatBridge::broadcastToBitchat(const BitchatMessage& msg) {
#if defined(ESP32) || defined(NRF52_PLATFORM)
    _bleService.broadcastMessage(msg);
#endif
}
