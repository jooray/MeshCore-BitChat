#pragma once

#include "BitchatProtocol.h"

#if defined(ESP32)
#include "BitchatBLEService.h"
#elif defined(NRF52_PLATFORM)
#include "../nrf52/BitchatBLEService.h"
#endif

#include <Mesh.h>
#include <Identity.h>

// Forward declaration for decompression support
struct tinfl_decompressor_tag;
typedef struct tinfl_decompressor_tag tinfl_decompressor;

// #mesh channel key: first 16 bytes of SHA256("#mesh")
// This is a "hashtag room" where the key is derived from the channel name
// Calculation: SHA256("#mesh").substring(0, 32) = "5b664cde0b08b220612113db980650f3"
static const uint8_t MESH_CHANNEL_KEY[16] = {
    0x5b, 0x66, 0x4c, 0xde, 0x0b, 0x08, 0xb2, 0x20,
    0x61, 0x21, 0x13, 0xdb, 0x98, 0x06, 0x50, 0xf3
};

// Bitchat channel name for #mesh (includes the # prefix)
#define BITCHAT_MESH_CHANNEL "#mesh"

/**
 * Bitchat Bridge - Translation layer between Bitchat and Meshcore protocols
 *
 * This bridge relays messages between Bitchat #mesh channel and MeshCore #mesh channel.
 * Only #mesh channel messages are relayed - DMs and other channels are ignored.
 * The #mesh channel uses a hashtag-derived key: SHA256("#mesh")[0:16]
 */
class BitchatBridge
#if defined(ESP32) || defined(NRF52_PLATFORM)
    : public BitchatBLECallback
#endif
{
public:
    /**
     * Constructor
     * @param mesh Reference to the Mesh instance
     * @param identity Reference to this node's LocalIdentity
     * @param nodeName This node's display name
     */
    BitchatBridge(mesh::Mesh& mesh, mesh::LocalIdentity& identity, const char* nodeName,
                  const uint8_t& pathHashMode);

    /**
     * Initialize the bridge
     * Call after mesh.begin()
     */
    void begin();

    /**
     * Main loop - call from main loop
     */
    void loop();

#if defined(ESP32)
    /**
     * Attach BLE service to existing server (shared BLE mode)
     * @param server BLE server to attach to
     * @return true if successful
     */
    bool attachBLEService(BLEServer* server);

    /**
     * Initialize BLE independently (standalone mode, no SerialBLEInterface)
     * Creates own BLE server with Bitchat service only.
     * Use this when MeshCore companion uses USB serial instead of BLE.
     * @param deviceName BLE device name for advertising
     * @return true if successful
     */
    bool beginStandalone(const char* deviceName);

    /**
     * Get the BLE service for disconnect callback registration
     */
    BitchatBLEService& getBLEService() { return _bleService; }
#elif defined(NRF52_PLATFORM)
    /**
     * Initialize BLE independently (standalone mode)
     * For NRF52, this uses Bluefruit BLE stack.
     * @param deviceName BLE device name for advertising
     * @return true if successful
     */
    bool beginStandalone(const char* deviceName);

    /**
     * Get the BLE service for disconnect callback registration
     */
    BitchatBLEService& getBLEService() { return _bleService; }
#endif

    /**
     * Handle incoming Meshcore GROUP message
     * Call this from onGroupDataRecv() or onChannelMessageRecv()
     * @param channel The Meshcore channel
     * @param timestamp Message timestamp
     * @param senderName Sender's display name (from message)
     * @param text Message text
     */
    void onMeshcoreGroupMessage(const mesh::GroupChannel& channel, uint32_t timestamp,
                                 const char* senderName, const char* text);

    /**
     * Set the default channel name for Bitchat
     * @param channelName Channel name without # prefix
     */
    void setDefaultChannel(const char* channelName);

    /**
     * Set the channel for outgoing Bitchat messages
     * @param channel Meshcore GroupChannel to use
     */
    void setMeshcoreChannel(const mesh::GroupChannel& channel);

    /**
     * Get this node's Bitchat peer ID (derived from identity)
     */
    uint64_t getBitchatPeerId() const { return _bitchatPeerId; }

    /**
     * Register a channel mapping between Bitchat channel name and MeshCore GroupChannel
     * @param bitchatChannelName Channel name without # prefix (e.g., "general")
     * @param meshChannel MeshCore GroupChannel
     * @return true if mapping was added (false if registry full)
     */
    bool registerChannelMapping(const char* bitchatChannelName, const mesh::GroupChannel& meshChannel);

    /**
     * Find MeshCore channel for a Bitchat channel name
     * @param channelName Channel name (with or without # prefix)
     * @param outChannel OUT: MeshCore GroupChannel if found
     * @return true if mapping found
     */
    bool findMeshChannel(const char* channelName, mesh::GroupChannel& outChannel);

    /**
     * Get Bitchat channel name for a MeshCore channel
     * @param channel MeshCore GroupChannel
     * @return Channel name (without #) or nullptr if not found
     */
    const char* getChannelName(const mesh::GroupChannel& channel);

    /**
     * Check if BLE service is active
     */
    bool isBLEActive() const;

    /**
     * Check if a Bitchat client is connected
     */
    bool hasBitchatClient() const;

    /**
     * Get statistics
     */
    uint32_t getMessagesRelayed() const { return _messagesRelayed; }
    uint32_t getDuplicatesDropped() const { return _duplicatesDropped; }

    /**
     * Get shared decompression buffers for BitchatProtocol
     * These are allocated once during begin() and reused for all decompressions
     * to avoid malloc failures when heap is fragmented.
     * @return Decompressor context or nullptr if not allocated
     */
    static tinfl_decompressor* getDecompressor();

    /**
     * Get shared decompression buffer
     * @return Buffer pointer or nullptr if not allocated
     */
    static uint8_t* getDecompBuffer();

protected:
#if defined(ESP32) || defined(NRF52_PLATFORM)
    // BitchatBLECallback implementation
    void onBitchatMessageReceived(const BitchatMessage& msg) override;
    void onBitchatClientConnect() override;
    void onBitchatClientDisconnect() override;
#endif

private:
    mesh::Mesh& _mesh;
    mesh::LocalIdentity& _identity;
    const char* _nodeName;
    const uint8_t& _pathHashMode;

#if defined(ESP32) || defined(NRF52_PLATFORM)
    BitchatBLEService _bleService;
#endif

    // Static to keep ~1.2KB out of heap allocation
    static BitchatDuplicateCache _duplicateCache;

    // Bitchat peer identity (derived from Meshcore identity)
    uint64_t _bitchatPeerId;

    // Noise public key (Curve25519, derived from Ed25519 identity)
    uint8_t _noisePublicKey[32];

    // Default channel for Bitchat messages
    char _defaultChannelName[32];
    mesh::GroupChannel _meshcoreChannel;
    bool _channelConfigured;

    // Channel registry for bidirectional mapping
    struct ChannelMapping {
        char bitchatName[32];      // Channel name without # prefix
        mesh::GroupChannel meshChannel;
        bool configured;
    };
    static const size_t MAX_CHANNEL_MAPPINGS = 4;
    ChannelMapping _channelMappings[MAX_CHANNEL_MAPPINGS];

    // Announcement timing
    uint32_t _lastAnnounceTime;
    volatile bool _pendingAnnounce;  // Flag to defer announcement to main loop (BLE callback has limited stack)
    volatile bool _processingMessage;  // Guard against re-entrant message processing (prevents stack explosion)
    // Matches the app's own policy: announce often while undiscovered, then back off to
    // keepalive once a client is connected and already knows us.
    static const uint32_t ANNOUNCE_INTERVAL_MS = 4000;  // 4 seconds when no client connected
    static const uint32_t ANNOUNCE_INTERVAL_CONNECTED_MS = 15000;  // 15 seconds when client connected

    // Fragment reassembly deferred processing (to avoid re-entrant call chains)
    // Static to keep ~1.1KB out of heap allocation
    static BitchatMessage _reassembledMsg;  // Buffer for reassembled message awaiting processing
    bool _hasReassembledMsg;         // True when _reassembledMsg contains a message to process

    // Time synchronization (calibrated from received Bitchat packets)
    // Android sends Unix timestamps; we sync from them since ESP32 may not have valid RTC
    int64_t _timeOffset;      // Offset to add to millis() to get Unix time (ms)
    bool _timeSynced;         // True after receiving at least one valid timestamp from Android
    uint64_t _bootTimestamp;  // Unix timestamp (ms) when time was first synced (used to filter old messages)

    // Statistics
    uint32_t _messagesRelayed;
    uint32_t _duplicatesDropped;

    // TTL for outgoing messages. Must match the app's MESSAGE_TTL_HOPS (7): the app only
    // treats an ANNOUNCE as a direct-link observation when ttl == its own max TTL.
    static const uint8_t DEFAULT_TTL = 7;

    // #mesh channel configuration
    mesh::GroupChannel _meshChannel;        // The MeshCore #mesh channel
    bool _meshChannelConfigured;            // True if #mesh channel is found/configured
    int _meshChannelIndex;                  // Index in the mesh's channel array (-1 if not found)

    // Message history cache for REQUEST_SYNC
    // Stores recent messages so we can respond to sync requests
    struct CachedMessage {
        BitchatMessage msg;
        uint32_t addedTimeMs;  // millis() when message was cached (for expiration)
        bool valid;
    };
    static const size_t MESSAGE_HISTORY_SIZE = 8;
    static const uint32_t MESSAGE_EXPIRY_MS = 300000;  // 5 minutes
    // Static to keep ~17KB out of heap allocation (8 * ~2KB BitchatMessage)
    static CachedMessage _messageHistory[MESSAGE_HISTORY_SIZE];
    size_t _messageHistoryHead;

    /**
     * Golomb-Coded Set (GCS) filter for REQUEST_SYNC
     * Used to determine which messages the requester already has.
     * See Android RequestSyncPacket.kt for format details.
     */
    struct GCSFilter {
        uint8_t p;           // Golomb-Rice parameter (bits for remainder)
        uint32_t n;          // Number of elements in filter
        uint32_t m;          // Range M = N * 2^P
        const uint8_t* data; // Pointer to encoded bitstream
        size_t dataLen;      // Length of encoded data

        /**
         * Check if a packet ID might be in the filter (probabilistic)
         * @param packetId16 16-byte packet ID
         * @return true if the ID might be in the filter (requester may have it)
         */
        bool mightContain(const uint8_t* packetId16) const;
    };

    /**
     * Parse GCS filter from REQUEST_SYNC payload
     * @param payload REQUEST_SYNC payload (TLV encoded)
     * @param len Payload length
     * @param outFilter Output filter structure
     * @return true if filter was successfully parsed
     */
    bool parseGCSFilter(const uint8_t* payload, size_t len, GCSFilter& outFilter);

    /**
     * Find or configure the #mesh channel
     * Called during begin() to set up the channel for relaying
     */
    void configureMeshChannel();

    /**
     * Check if a MeshCore channel matches the #mesh channel by key
     * @param channel Channel to check
     * @return true if it's the #mesh channel
     */
    bool isMeshChannel(const mesh::GroupChannel& channel) const;

    /**
     * Add a message to the history cache
     * @param msg Message to cache
     */
    void addToMessageHistory(const BitchatMessage& msg);

    /**
     * Handle REQUEST_SYNC by sending cached messages
     * @param msg The REQUEST_SYNC message
     */
    void handleRequestSync(const BitchatMessage& msg);

    // Peer nickname cache (populated from ANNOUNCE messages)
    struct PeerInfo {
        uint64_t peerId;
        char nickname[16];    // 13 chars + null + padding
        uint32_t timestamp;   // millis() when last seen
        bool valid;
    };
    static const size_t PEER_CACHE_SIZE = 32;
    // Static to keep ~1KB out of heap allocation
    static PeerInfo _peerCache[PEER_CACHE_SIZE];

    // Fragment reassembly buffers for long messages
    // Bitchat fragments messages >245 bytes into multiple FRAGMENT messages
    // Android fragment header format (13 bytes):
    //   - 8 bytes: Fragment ID (random)
    //   - 2 bytes: Index (big-endian UInt16)
    //   - 2 bytes: Total count (big-endian UInt16)
    //   - 1 byte:  Original message type
    struct FragmentBuffer {
        uint64_t senderId;
        uint8_t fragmentId[8];      // 8-byte fragment ID (matches Android)
        uint16_t totalFragments;    // Changed from uint8_t
        uint16_t receivedCount;     // Track received count
        uint8_t originalType;       // Store original message type
        uint8_t receivedMask;       // Bitmask of received fragments (up to 16 fragments with extension)
        uint8_t data[2048];         // Reassembly buffer
        size_t dataLen;
        uint32_t startTime;         // millis() when first fragment received
        bool active;
    };
    static const size_t MAX_FRAGMENT_BUFFERS = 4;  // Reduced from 16 to save memory
    static const uint32_t FRAGMENT_TIMEOUT_MS = 10000;  // 10 second timeout
    // Static to keep ~8KB out of heap (4 * ~2KB each)
    static FragmentBuffer _fragmentBuffers[MAX_FRAGMENT_BUFFERS];

    /**
     * Handle incoming fragment message
     * Reassembles multi-fragment messages and processes when complete
     */
    void handleFragment(const BitchatMessage& msg);

    /**
     * Derive Bitchat peer ID from Meshcore identity
     * Uses first 8 bytes of public key
     */
    // Bitchat binds the peer ID to the Noise static key: first 8 bytes of SHA-256(curve25519 pubkey).
    // Announces whose senderID doesn't match are rejected by the app (AnnouncementIdentityValidator).
    uint64_t derivePeerId(const uint8_t* noisePublicKey);

    /**
     * Derive Noise public key (Curve25519) from Ed25519 public key
     * Uses standard Edwards→Montgomery conversion
     */
    void deriveNoisePublicKey(const uint8_t* ed25519PubKey, uint8_t* curve25519PubKey);

    /**
     * Send peer announcement to connected Bitchat clients
     */
    void sendPeerAnnouncement();

    /**
     * Sign a Bitchat message with the companion's Ed25519 identity
     * Uses Bitchat protocol signing rules (TTL=0, PKCS#7 padding)
     */
    void signMessage(BitchatMessage& msg);

    /**
     * Process incoming Bitchat message from BLE
     */
    void processBitchatMessage(const BitchatMessage& msg);

    /**
     * Relay Bitchat channel message to Meshcore mesh (with random delay for multi-bridge dedup)
     * @param msg Original Bitchat message
     * @param channelName Channel name (e.g., "#mesh")
     * @param senderNick Sender's nickname from Bitchat
     * @param text Message content
     */
    void relayChannelMessageToMesh(const BitchatMessage& msg, const char* channelName,
                                   const char* senderNick, const char* text);

    /**
     * Internal relay function - actually sends message to mesh after delay
     * @param senderNick Sender's nickname from Bitchat
     * @param text Message content
     * @param originalTimestamp Original Bitchat timestamp in seconds
     */
    void relayChannelMessageToMesh_Internal(const char* senderNick, const char* text, uint32_t originalTimestamp);

    /**
     * Relay Bitchat DM to Meshcore mesh
     */
    void relayDirectMessageToMesh(const BitchatMessage& msg, const char* text);

    /**
     * Broadcast Bitchat message to connected BLE clients
     */
    void broadcastToBitchat(const BitchatMessage& msg);

    /**
     * Get current time in milliseconds (for Bitchat timestamps)
     * Returns synchronized time if available, otherwise falls back to RTC or millis()
     */
    uint64_t getCurrentTimeMs();

    /**
     * Synchronize local time from a received Bitchat packet timestamp
     * This is critical for proper operation - Android rejects announces with stale timestamps
     */
    void syncTimeFromPacket(uint64_t packetTimestamp);

    /**
     * Parse ANNOUNCE message TLV to extract nickname
     * @param payload TLV-encoded payload
     * @param len Payload length
     * @param nickname OUT: Nickname if found
     * @param nickLen Capacity of nickname buffer
     * @return true if nickname was found
     */
    bool parseAnnounceTLV(const uint8_t* payload, size_t len, char* nickname, size_t nickLen);

    /**
     * Cache or update a peer's nickname
     * @param peerId Peer's 64-bit ID
     * @param nickname Peer's nickname
     */
    void cachePeer(uint64_t peerId, const char* nickname);

    /**
     * Look up a cached peer nickname
     * @param peerId Peer's 64-bit ID
     * @return Nickname or nullptr if not found
     */
    const char* lookupPeerNickname(uint64_t peerId);

    /**
     * Send a single message part to the mesh (immediate, no delay)
     * @param senderNick Sender nickname with emoji prefix
     * @param text Message text (may include part indicator)
     * @param originalTimestamp Original Bitchat timestamp in seconds (for deterministic packet hash)
     */
    void sendSingleMessageToMesh(const char* senderNick, const char* text, uint32_t originalTimestamp);

    // Pending message parts queue for reliable multi-part message delivery
    // Instead of using mesh's delayed transmission (which can fail silently when pool is exhausted),
    // we queue parts here and send them one at a time with timer-based delays in loop()
    struct PendingPart {
        char senderNick[68];    // Includes emoji prefix
        char text[180];         // Part text with "[X/Y] " indicator
        uint32_t originalTimestamp;  // Original Bitchat timestamp (seconds) for deterministic hashing
        bool valid;
    };
    static const size_t MAX_PENDING_PARTS = 8;  // Queue size (parts 2-8 queued, part 1 sent immediately)
    static const size_t MAX_MESSAGE_PARTS = 8;  // Max parts per message (~1KB) - more is unreliable over LoRa
    static const uint32_t PART_SEND_DELAY_MS = 15000;  // Delay between parts (15s for LoRa reliability)
    // Static to keep ~2KB out of heap
    static PendingPart _pendingParts[MAX_PENDING_PARTS];
    size_t _pendingPartsHead;        // Next part to send
    size_t _pendingPartsTail;        // Next slot to queue into
    uint32_t _lastPartSentTime;      // millis() when last part was sent

    // Multi-bridge duplicate relay prevention
    // When multiple bridges are on the same Bitchat network, they all receive the same BLE message
    // and try to relay it. This causes: (1) RF collision if both transmit simultaneously,
    // (2) duplicate messages if both succeed with different timestamps.
    //
    // Solution: Random delay (0-3s) + use original Bitchat timestamp for deterministic packet hash
    // - Random delay spreads out transmission attempts, avoiding RF collision
    // - Original timestamp ensures all bridges produce identical packets → MeshCore dedup catches duplicates
    // Per-entry content buffer size - 256 bytes covers most messages
    // Longer messages will be truncated in the queue (still sent, just may lose data)
    static const size_t MAX_RELAY_CONTENT_SIZE = 256;

    struct PendingRelay {
        char senderNick[68];                      // Sender nickname from Bitchat
        char content[MAX_RELAY_CONTENT_SIZE];     // Message content (inline buffer)
        uint32_t bitchatTimestamp;                // Original Bitchat timestamp in seconds
        uint32_t sendAtMillis;                    // When to send (millis() + random delay)
        bool valid;
    };
    // 4 slots to handle message bursts - drops only occur if 4+ messages arrive within the 0-3s delay window
    static const size_t MAX_PENDING_RELAYS = 4;
    static const uint32_t MAX_RELAY_DELAY_MS = 3000;  // 0-3 second random delay
    PendingRelay _pendingRelays[MAX_PENDING_RELAYS];

    /**
     * Queue a message part for delayed sending
     * @param senderNick Sender nickname with emoji prefix
     * @param text Message text (may include part indicator)
     * @param originalTimestamp Original Bitchat timestamp in seconds (for deterministic packet hash)
     * @return true if queued successfully
     */
    bool queueMessagePart(const char* senderNick, const char* text, uint32_t originalTimestamp);

    /**
     * Queue a channel message relay with random delay (multi-bridge collision avoidance)
     * @param senderNick Sender nickname from Bitchat
     * @param content Message content
     * @param bitchatTimestamp Original Bitchat timestamp in seconds
     * @return true if queued successfully
     */
    bool queuePendingRelay(const char* senderNick, const char* content, uint32_t bitchatTimestamp);

    /**
     * Process pending relay queue - called from loop()
     * Sends queued messages when their delay has elapsed
     */
    void processPendingRelays();

    /**
     * Process pending message parts queue (called from loop())
     */
    void processPendingParts();

    /**
     * Parse BitchatMessage TLV payload structure
     *
     * The payload contains:
     * [flags:1][timestamp:8][idLen:1][id:N][senderLen:1][sender:N]
     * [contentLen:2][content:N]...[channelLen:1][channel:N if hasChannel]
     *
     * @param payload Message payload (TLV encoded)
     * @param payloadLen Payload length
     * @param senderNick OUT: Sender's nickname
     * @param senderNickLen Capacity of senderNick buffer
     * @param content OUT: Message content (text)
     * @param contentLen Capacity of content buffer
     * @param channelName OUT: Channel name (with #), empty if DM
     * @param channelNameLen Capacity of channelName buffer
     * @return true if successfully parsed
     */
    bool parseBitchatMessageTLV(const uint8_t* payload, size_t payloadLen,
                                char* senderNick, size_t senderNickLen,
                                char* content, size_t contentLen,
                                char* channelName, size_t channelNameLen);

    // ============================================================================
    // Decompression Buffers (Static, Shared)
    // ============================================================================
    // These are statically allocated at compile time (.bss section) to avoid
    // malloc failures when heap is fragmented during message processing.
    // Total allocation: 10,412 bytes (8364 + 2048) reserved before program runs
#if defined(NRF52_PLATFORM)
    static tinfl_decompressor _decompressorStatic;  // Decompressor context (~8.3KB)
    static uint8_t _decompBufferStatic[BITCHAT_MAX_PAYLOAD_SIZE];  // Decompression buffer (2KB)
#endif
};
