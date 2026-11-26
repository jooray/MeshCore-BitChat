#pragma once

#if defined(NRF52_PLATFORM) && defined(ENABLE_BITCHAT)

#include "../bitchat/BitchatProtocol.h"
#include <bluefruit.h>

#define BITCHAT_BLE_RX_MAX 514

class BitchatBLECallback {
public:
    virtual ~BitchatBLECallback() {}
    virtual void onBitchatRawPacket(const uint8_t* data, size_t len) = 0;
    virtual void onBitchatClientConnect() {}
    virtual void onBitchatClientDisconnect() {}
    virtual void onBitchatClientSubscribed() {}
};

class BitchatBLEService {
public:
    BitchatBLEService();

    bool beginStandalone(const char* deviceName, BitchatBLECallback* callback);
    void startAdvertising();

    bool isActive() const { return _serviceActive; }
    bool hasConnectedClient() const {
        return __atomic_load_n(&_bitchatClientCount, __ATOMIC_ACQUIRE) > 0;
    }

    bool broadcastMessage(const BitchatMessage& msg);
    bool broadcastRaw(const uint8_t* data, size_t len);
    void loop();
    void onServerDisconnect();

private:
    BLEService _service;
    BLECharacteristic _characteristic;
    BitchatBLECallback* _callback;
    char _deviceName[48];

    bool _serviceActive;
    uint8_t _bitchatClientCount;
    bool _clientSubscribed;
    bool _pendingConnect;
    bool _pendingDisconnect;
    bool _pendingSubscribe;

    // Static SPSC raw packet ring: SoftDevice callback produces, loop() consumes
    static const size_t RX_RING_DEPTH = 4;
    struct RxSlot {
        uint16_t len;
        uint8_t data[BITCHAT_BLE_RX_MAX];
    };
    static RxSlot _rxRing[RX_RING_DEPTH];
    uint32_t _rxHead;
    uint32_t _rxTail;

    // Static TX queue so REQUEST_SYNC bursts (up to 8 packets + ANNOUNCE) survive
    // SoftDevice HVN buffer exhaustion (NRF_ERROR_RESOURCES) with retry in loop()
    static const size_t TX_RING_DEPTH = 10;
    static const size_t TX_SLOT_SIZE = 256;
    static const uint32_t TX_GIVE_UP_MS = 2000;
    struct TxSlot {
        uint16_t len;
        uint8_t data[TX_SLOT_SIZE];
    };
    static TxSlot _txRing[TX_RING_DEPTH];
    size_t _txHead;
    size_t _txCount;
    uint32_t _txStartedMs;
    bool _txInFlight;

    void drainTxQueue();

    static void onConnect(uint16_t conn_handle);
    static void onDisconnect(uint16_t conn_handle, uint8_t reason);
    static void onCharacteristicWrite(uint16_t conn_handle, BLECharacteristic* chr, uint8_t* data, uint16_t len);
    static void onCharacteristicCccdWrite(uint16_t conn_handle, BLECharacteristic* chr, uint16_t cccd_value);

    static BitchatBLEService* _instance;
};

#endif // NRF52_PLATFORM && ENABLE_BITCHAT
