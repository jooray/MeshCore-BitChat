#pragma once

#include "BitchatProtocol.h"

#ifdef ESP32
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEAdvertising.h>

#define BITCHAT_BLE_RX_MAX 514

class BitchatBLECallback {
public:
    virtual ~BitchatBLECallback() {}
    virtual void onBitchatRawPacket(const uint8_t* data, size_t len) = 0;
    virtual void onBitchatClientConnect() {}
    virtual void onBitchatClientDisconnect() {}
    virtual void onBitchatClientSubscribed() {}
};

class BitchatBLEService : public BLECharacteristicCallbacks {
public:
    BitchatBLEService();

    bool attachToServer(BLEServer* server, BitchatBLECallback* callback);
    void start();
    void startServiceOnly();
    void setDeviceName(const char* name);
    void startAdvertising();

    bool isActive() const { return _serviceActive; }
    bool hasConnectedClient() const {
        return __atomic_load_n(&_bitchatClientCount, __ATOMIC_ACQUIRE) > 0;
    }

    bool broadcastMessage(const BitchatMessage& msg);
    bool broadcastRaw(const uint8_t* data, size_t len);
    void loop();

    void onServerDisconnect();
    void onServerConnect();
    void onCccdWrite(bool subscribed);

protected:
    void onWrite(BLECharacteristic* pCharacteristic) override;
    void onRead(BLECharacteristic* pCharacteristic) override;
    void onStatus(BLECharacteristic* pCharacteristic, Status s, uint32_t code) override;

private:
    class CccdCallbacks : public BLEDescriptorCallbacks {
    public:
        explicit CccdCallbacks(BitchatBLEService* svc) : _svc(svc) {}
        void onWrite(BLEDescriptor* pDescriptor) override;
    private:
        BitchatBLEService* _svc;
    };

    BLEServer* _server;
    BLEService* _service;
    BLECharacteristic* _characteristic;
    BitchatBLECallback* _callback;
    CccdCallbacks _cccdCallbacks;
    char _deviceName[48];

    bool _serviceActive;
    uint8_t _bitchatClientCount;
    uint8_t _lastKnownServerCount;
    bool _clientSubscribed;
    bool _pendingConnect;
    bool _pendingDisconnect;
    bool _pendingSubscribe;

    // Lock-free SPSC raw packet ring: BLE task produces, loop() consumes
    static const size_t RX_RING_DEPTH = 4;
    struct RxSlot {
        uint16_t len;
        uint8_t data[BITCHAT_BLE_RX_MAX];
    };
    RxSlot _rxRing[RX_RING_DEPTH];
    uint32_t _rxHead;
    uint32_t _rxTail;

    void checkForDisconnects();
};

#endif // ESP32
