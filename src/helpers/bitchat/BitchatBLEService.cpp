#include "BitchatBLEService.h"
#include "BitchatDebugSerial.h"

#ifdef ESP32

#include <Arduino.h>
#include <esp_bt.h>

#if BITCHAT_DEBUG
  #define BITCHAT_DEBUG_PRINTLN(F, ...) BITCHAT_SERIAL.printf("BITCHAT: " F "\n", ##__VA_ARGS__)
#else
  #define BITCHAT_DEBUG_PRINTLN(...) {}
#endif

#if BITCHAT_DEBUG_PACKETDUMP
static void dumpPacketHex(const char* label, const uint8_t* data, size_t len) {
    BITCHAT_SERIAL.printf("PACKETDUMP [%s] (%zu bytes):\n", label, len);
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

void BitchatBLEService::CccdCallbacks::onWrite(BLEDescriptor* pDescriptor) {
    if (_svc == nullptr || pDescriptor == nullptr) return;
    uint8_t* val = pDescriptor->getValue();
    size_t len = pDescriptor->getLength();
    bool sub = (val != nullptr && len >= 1 && (val[0] & 0x03) != 0);
    _svc->onCccdWrite(sub);
}

BitchatBLEService::BitchatBLEService()
    : _server(nullptr)
    , _service(nullptr)
    , _characteristic(nullptr)
    , _callback(nullptr)
    , _cccdCallbacks(this)
    , _serviceActive(false)
    , _bitchatClientCount(0)
    , _lastKnownServerCount(0)
    , _clientSubscribed(false)
    , _pendingConnect(false)
    , _pendingDisconnect(false)
    , _pendingSubscribe(false)
    , _rxHead(0)
    , _rxTail(0)
{
    memset(_deviceName, 0, sizeof(_deviceName));
    strcpy(_deviceName, "Bitchat");
    memset(_rxRing, 0, sizeof(_rxRing));
}

bool BitchatBLEService::attachToServer(BLEServer* server, BitchatBLECallback* callback) {
    if (server == nullptr || callback == nullptr) {
        BITCHAT_DEBUG_PRINTLN("attachToServer: null server or callback");
        return false;
    }

    _server = server;
    _callback = callback;

    _service = _server->createService(BITCHAT_SERVICE_UUID);
    if (_service == nullptr) {
        BITCHAT_DEBUG_PRINTLN("Failed to create Bitchat service");
        return false;
    }

    _characteristic = _service->createCharacteristic(
        BITCHAT_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ |
        BLECharacteristic::PROPERTY_WRITE |
        BLECharacteristic::PROPERTY_WRITE_NR |
        BLECharacteristic::PROPERTY_NOTIFY |
        BLECharacteristic::PROPERTY_INDICATE
    );

    if (_characteristic == nullptr) {
        BITCHAT_DEBUG_PRINTLN("Failed to create Bitchat characteristic");
        return false;
    }

    _characteristic->setAccessPermissions(ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE);
    BLE2902* cccd = new BLE2902();
    cccd->setCallbacks(&_cccdCallbacks);
    _characteristic->addDescriptor(cccd);
    _characteristic->setCallbacks(this);

    BITCHAT_DEBUG_PRINTLN("Bitchat BLE service attached to server");
    return true;
}

void BitchatBLEService::start() {
    if (_service == nullptr) return;

    BLEDevice::setMTU(517);
    _service->start();
    _serviceActive = true;

    if (_server != nullptr) {
        BLEAdvertising* advertising = _server->getAdvertising();
        BLEAdvertisementData scanResponse;
        scanResponse.setCompleteServices(BLEUUID(BITCHAT_SERVICE_UUID));
        advertising->setScanResponseData(scanResponse);
        BITCHAT_DEBUG_PRINTLN("Bitchat BLE service started (shared mode)");
    }
}

void BitchatBLEService::startServiceOnly() {
    if (_service == nullptr) return;

    BLEDevice::setMTU(517);
    _service->start();
    _serviceActive = true;
    BITCHAT_DEBUG_PRINTLN("Bitchat BLE service started (standalone)");
}

void BitchatBLEService::setDeviceName(const char* name) {
    strncpy(_deviceName, name, sizeof(_deviceName) - 1);
    _deviceName[sizeof(_deviceName) - 1] = '\0';
}

void BitchatBLEService::startAdvertising() {
    if (_server == nullptr) return;

    BLEAdvertising* advertising = _server->getAdvertising();

    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL0, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL1, ESP_PWR_LVL_P9);
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_CONN_HDL2, ESP_PWR_LVL_P9);

    advertising->setMinInterval(0x140);  // 200ms
    advertising->setMaxInterval(0x190);  // 250ms

    BLEAdvertisementData advData;
    advData.setFlags(ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT);
    advData.setCompleteServices(BLEUUID(BITCHAT_SERVICE_UUID));
    advertising->setAdvertisementData(advData);

    char safeName[20];
    size_t j = 0;
    for (size_t i = 0; _deviceName[i] != '\0' && j < sizeof(safeName) - 1; i++) {
        if (_deviceName[i] >= 0x20 && _deviceName[i] <= 0x7E) {
            safeName[j++] = _deviceName[i];
        }
    }
    safeName[j] = '\0';
    if (j == 0) strcpy(safeName, "Bitchat");

    BLEAdvertisementData scanResponse;
    scanResponse.setName(safeName);
    advertising->setScanResponseData(scanResponse);

    advertising->start();
    BITCHAT_DEBUG_PRINTLN("BLE advertising started: %s", safeName);
}

void BitchatBLEService::onServerDisconnect() {
    // Runs on Bluedroid BTC task: defer bridge callbacks to loop()
    __atomic_store_n(&_pendingDisconnect, true, __ATOMIC_RELEASE);
    if (_server != nullptr) {
        _server->getAdvertising()->start();
    }
}

void BitchatBLEService::onServerConnect() {
    // Runs on Bluedroid BTC task: keep advertising for additional phones and notify loop()
    __atomic_store_n(&_pendingConnect, true, __ATOMIC_RELEASE);
    if (_server != nullptr) {
        _server->getAdvertising()->start();
    }
}

void BitchatBLEService::onCccdWrite(bool subscribed) {
    bool prev = __atomic_exchange_n(&_clientSubscribed, subscribed, __ATOMIC_ACQ_REL);
    if (subscribed && !prev) {
        __atomic_store_n(&_pendingSubscribe, true, __ATOMIC_RELEASE);
    }
}

void BitchatBLEService::checkForDisconnects() {
    if (_server == nullptr) return;

    uint32_t currentServerCount = _server->getConnectedCount();
    if (currentServerCount < _lastKnownServerCount) {
        uint8_t disconnected = static_cast<uint8_t>(_lastKnownServerCount - currentServerCount);
        uint8_t count = __atomic_load_n(&_bitchatClientCount, __ATOMIC_RELAXED);
        count = (disconnected >= count) ? 0 : static_cast<uint8_t>(count - disconnected);
        __atomic_store_n(&_bitchatClientCount, count, __ATOMIC_RELEASE);
        _lastKnownServerCount = static_cast<uint8_t>(currentServerCount);

        if (count == 0) {
            __atomic_store_n(&_clientSubscribed, false, __ATOMIC_RELEASE);
            if (_callback != nullptr) {
                _callback->onBitchatClientDisconnect();
            }
        }
    } else if (currentServerCount > _lastKnownServerCount) {
        uint8_t newClients = static_cast<uint8_t>(currentServerCount - _lastKnownServerCount);
        uint8_t count = __atomic_load_n(&_bitchatClientCount, __ATOMIC_RELAXED);
        __atomic_store_n(&_bitchatClientCount, static_cast<uint8_t>(count + newClients), __ATOMIC_RELEASE);
        _lastKnownServerCount = static_cast<uint8_t>(currentServerCount);
    }
}

void BitchatBLEService::loop() {
    bool hadDisconnect = __atomic_exchange_n(&_pendingDisconnect, false, __ATOMIC_ACQ_REL);
    checkForDisconnects();
    if (hadDisconnect && __atomic_load_n(&_bitchatClientCount, __ATOMIC_RELAXED) == 0 &&
        _server != nullptr && _server->getConnectedCount() == 0) {
        __atomic_store_n(&_clientSubscribed, false, __ATOMIC_RELEASE);
    }

    if (__atomic_exchange_n(&_pendingConnect, false, __ATOMIC_ACQ_REL)) {
        if (_callback != nullptr) {
            _callback->onBitchatClientConnect();
        }
    }

    if (__atomic_exchange_n(&_pendingSubscribe, false, __ATOMIC_ACQ_REL)) {
        if (_callback != nullptr) {
            _callback->onBitchatClientSubscribed();
        }
    }

    uint32_t head = __atomic_load_n(&_rxHead, __ATOMIC_RELAXED);
    while (head != __atomic_load_n(&_rxTail, __ATOMIC_ACQUIRE)) {
        const RxSlot& slot = _rxRing[head % RX_RING_DEPTH];
        BITCHAT_PACKETDUMP("BLE_SERVICE_RX", slot.data, slot.len);
        if (_callback != nullptr) {
            _callback->onBitchatRawPacket(slot.data, slot.len);
        }
        head++;
        __atomic_store_n(&_rxHead, head, __ATOMIC_RELEASE);
    }

    // Periodically restart advertising if no clients connected
    if (_server != nullptr && __atomic_load_n(&_bitchatClientCount, __ATOMIC_RELAXED) == 0 &&
        _server->getConnectedCount() == 0) {
        static uint32_t lastAdvRestart = 0;
        uint32_t now = millis();
        if (now - lastAdvRestart > 30000) {
            _server->getAdvertising()->start();
            lastAdvRestart = now;
        }
    }
}

void BitchatBLEService::onWrite(BLECharacteristic* pCharacteristic) {
    // Zero-copy read directly from BLECharacteristic buffer (no std::string heap alloc)
    const uint8_t* data = pCharacteristic->getData();
    size_t len = pCharacteristic->getLength();
    if (data == nullptr || len == 0 || len > BITCHAT_BLE_RX_MAX) {
        return;
    }

    uint32_t tail = __atomic_load_n(&_rxTail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&_rxHead, __ATOMIC_ACQUIRE);
    if (tail - head >= RX_RING_DEPTH) {
        return;  // Ring full, drop packet
    }

    RxSlot& slot = _rxRing[tail % RX_RING_DEPTH];
    slot.len = static_cast<uint16_t>(len);
    memcpy(slot.data, data, len);
    __atomic_store_n(&_rxTail, tail + 1, __ATOMIC_RELEASE);
}

void BitchatBLEService::onRead(BLECharacteristic* pCharacteristic) {
    (void)pCharacteristic;
}

void BitchatBLEService::onStatus(BLECharacteristic* pCharacteristic, Status s, uint32_t code) {
    (void)pCharacteristic; (void)code;
    if (s == Status::ERROR_NOTIFY_DISABLED) {
        __atomic_store_n(&_clientSubscribed, false, __ATOMIC_RELEASE);
    }
}

bool BitchatBLEService::broadcastRaw(const uint8_t* data, size_t len) {
    if (!_serviceActive || _characteristic == nullptr || data == nullptr ||
        len == 0 || len > BITCHAT_MAX_MESSAGE_SIZE) {
        return false;
    }
    BITCHAT_PACKETDUMP("BLE_SERVICE_TX", data, len);
    _characteristic->setValue(const_cast<uint8_t*>(data), len);
    if (_server != nullptr && _server->getConnectedCount() > 0) {
        _characteristic->notify(true);
    }
    return true;
}

bool BitchatBLEService::broadcastMessage(const BitchatMessage& msg) {
    static uint8_t buffer[BITCHAT_MAX_MESSAGE_SIZE];
    size_t len = BitchatProtocol::serializeMessage(msg, buffer, sizeof(buffer));
    if (len == 0) return false;
    return broadcastRaw(buffer, len);
}

#endif // ESP32
