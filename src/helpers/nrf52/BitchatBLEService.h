#pragma once

#ifdef NRF52_PLATFORM

#include "../bitchat/BitchatProtocol.h"
#include <bluefruit.h>

/**
 * Callback interface for Bitchat BLE events
 */
class BitchatBLECallback {
public:
    virtual ~BitchatBLECallback() {}

    /**
     * Called when a Bitchat message is received via BLE
     * @param msg The received message
     */
    virtual void onBitchatMessageReceived(const BitchatMessage& msg) = 0;

    /**
     * Called when a Bitchat BLE client connects
     */
    virtual void onBitchatClientConnect() {}

    /**
     * Called when a Bitchat BLE client disconnects
     */
    virtual void onBitchatClientDisconnect() {}
};

/**
 * Bitchat BLE Service for NRF52 (using Bluefruit)
 * Provides a GATT service for Bitchat protocol communication
 *
 * Note: This uses standalone BLE advertising. For NRF52, when Bitchat is enabled,
 * the MeshCore companion uses USB serial while Bitchat uses BLE independently.
 */
class BitchatBLEService {
public:
    BitchatBLEService();

    /**
     * Initialize BLE and start the Bitchat service in standalone mode
     * Creates BLE server with Bitchat service only.
     * @param deviceName BLE device name for advertising
     * @param callback Callback for Bitchat events
     * @return true if successful
     */
    bool beginStandalone(const char* deviceName, BitchatBLECallback* callback);

    /**
     * Start BLE advertising
     * Call after beginStandalone() to start advertising
     */
    void startAdvertising();

    /**
     * Check if the service is active
     */
    bool isActive() const { return _serviceActive; }

    /**
     * Check if a Bitchat client is connected
     */
    bool hasConnectedClient() const { return _bitchatClientCount > 0; }

    /**
     * Broadcast a message to connected Bitchat clients
     * @param msg Message to send
     * @return true if message was sent
     */
    bool broadcastMessage(const BitchatMessage& msg);

    /**
     * Process loop - call from main loop
     * Handles deferred message processing
     */
    void loop();

    /**
     * Called when a client disconnects
     */
    void onServerDisconnect();

private:
    // Bluefruit service and characteristic
    BLEService _service;
    BLECharacteristic _characteristic;
    BitchatBLECallback* _callback;
    char _deviceName[48];

    bool _serviceActive;
    uint8_t _bitchatClientCount;
    bool _clientSubscribed;

    // Flags for deferred processing
    volatile bool _pendingConnect;
    volatile bool _pendingData;

    // Write buffer for reassembling fragmented BLE writes
    // 1024 bytes to handle long messages that compress to ~615 bytes (2 fragments)
    // Made static to keep 1KB out of heap allocation
    static uint8_t _writeBuffer[1024];
    size_t _writeBufferOffset;
    uint32_t _lastWriteTime;
    static const uint32_t WRITE_TIMEOUT_MS = 5000;

    // Message queue for deferred processing (incoming)
    // Reduced from 8 to 2 to save ~13KB heap (each BitchatMessage is ~2KB)
    static const size_t MESSAGE_QUEUE_SIZE = 2;
    struct QueuedMessage {
        BitchatMessage msg;
        bool valid;
    };
    // Static to keep out of heap allocation
    static QueuedMessage _messageQueue[MESSAGE_QUEUE_SIZE];
    volatile size_t _queueHead;
    volatile size_t _queueTail;

    // Pending outgoing message (when client not yet subscribed)
    // Made static to keep ~2KB out of heap allocation
    static BitchatMessage _pendingOutgoing;
    bool _hasPendingOutgoing;

    void clearWriteBuffer();
    bool queueMessage(const BitchatMessage& msg);
    void processQueue();
    void sendPendingOutgoing();

    // Static callbacks for Bluefruit (will call instance methods)
    static void onConnect(uint16_t conn_handle);
    static void onDisconnect(uint16_t conn_handle, uint8_t reason);
    static void onCharacteristicWrite(uint16_t conn_handle, BLECharacteristic* chr, uint8_t* data, uint16_t len);
    static void onCharacteristicCccdWrite(uint16_t conn_handle, BLECharacteristic* chr, uint16_t cccd_value);

    // Singleton for static callback access
    static BitchatBLEService* _instance;
};

#endif // NRF52_PLATFORM
