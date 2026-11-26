#include "BitchatBLEService.h"
#include "../bitchat/BitchatDebugSerial.h"

#if defined(NRF52_PLATFORM) && defined(ENABLE_BITCHAT)

#include <Arduino.h>

#if BITCHAT_DEBUG
  #define BITCHAT_DEBUG_PRINTLN(...) do { BITCHAT_SERIAL.printf("BITCHAT_BLE: "); BITCHAT_SERIAL.printf(__VA_ARGS__); BITCHAT_SERIAL.println(); } while(0)
#else
  #define BITCHAT_DEBUG_PRINTLN(...) {}
#endif

// Bitchat service UUID: F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C (little-endian)
static const uint8_t BITCHAT_SERVICE_UUID_BYTES[] = {
    0x5C, 0x4B, 0x3A, 0x2C, 0x1D, 0x8E, 0x3F, 0x9B,
    0x5A, 0x4C, 0x9E, 0x4A, 0x2D, 0x5E, 0x7B, 0xF4
};

// Bitchat characteristic UUID: A1B2C3D4-E5F6-4A5B-8C9D-0E1F2A3B4C5D (little-endian)
static const uint8_t BITCHAT_CHARACTERISTIC_UUID_BYTES[] = {
    0x5D, 0x4C, 0x3B, 0x2A, 0x1F, 0x0E, 0x9D, 0x8C,
    0x5B, 0x4A, 0xF6, 0xE5, 0xD4, 0xC3, 0xB2, 0xA1
};

BitchatBLEService* BitchatBLEService::_instance = nullptr;
BitchatBLEService::RxSlot BitchatBLEService::_rxRing[RX_RING_DEPTH];
BitchatBLEService::TxSlot BitchatBLEService::_txRing[TX_RING_DEPTH];

BitchatBLEService::BitchatBLEService()
    : _service(BITCHAT_SERVICE_UUID_BYTES)
    , _characteristic(BITCHAT_CHARACTERISTIC_UUID_BYTES)
    , _callback(nullptr)
    , _serviceActive(false)
    , _bitchatClientCount(0)
    , _clientSubscribed(false)
    , _pendingConnect(false)
    , _pendingDisconnect(false)
    , _pendingSubscribe(false)
    , _rxHead(0)
    , _rxTail(0)
    , _txHead(0)
    , _txCount(0)
    , _txStartedMs(0)
    , _txInFlight(false)
{
    memset(_deviceName, 0, sizeof(_deviceName));
    strcpy(_deviceName, "Bitchat");
    memset(_rxRing, 0, sizeof(_rxRing));
    memset(_txRing, 0, sizeof(_txRing));
    _instance = this;
}

bool BitchatBLEService::beginStandalone(const char* deviceName, BitchatBLECallback* callback) {
    if (callback == nullptr) return false;

    _callback = callback;
    strncpy(_deviceName, deviceName, sizeof(_deviceName) - 1);
    _deviceName[sizeof(_deviceName) - 1] = '\0';

    Bluefruit.configPrphConn(517, BLE_GAP_EVENT_LENGTH_DEFAULT, BLE_GATTS_HVN_TX_QUEUE_SIZE_DEFAULT, BLE_GATTC_WRITE_CMD_TX_QUEUE_SIZE_DEFAULT);
    Bluefruit.begin();
    Bluefruit.setTxPower(8);

    Bluefruit.Periph.setConnectCallback(onConnect);
    Bluefruit.Periph.setDisconnectCallback(onDisconnect);

    Bluefruit.Security.setMITM(false);
    Bluefruit.Security.setIOCaps(false, false, false);

    char safeName[32];
    size_t j = 0;
    for (size_t i = 0; deviceName[i] != '\0' && j < sizeof(safeName) - 1; i++) {
        if (deviceName[i] >= 0x20 && deviceName[i] <= 0x7E) {
            safeName[j++] = deviceName[i];
        }
    }
    safeName[j] = '\0';
    if (j == 0) strcpy(safeName, "Bitchat");

    Bluefruit.setName(safeName);

    _service.begin();
    _characteristic.setProperties(CHR_PROPS_READ | CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP | CHR_PROPS_NOTIFY | CHR_PROPS_INDICATE);
    _characteristic.setPermission(SECMODE_OPEN, SECMODE_OPEN);
    _characteristic.setMaxLen(512);
    _characteristic.setWriteCallback(onCharacteristicWrite);
    _characteristic.setCccdWriteCallback(onCharacteristicCccdWrite);
    _characteristic.begin();

    _serviceActive = true;
    BITCHAT_DEBUG_PRINTLN("Bitchat BLE service initialized: %s", safeName);
    return true;
}

void BitchatBLEService::startAdvertising() {
    Bluefruit.Advertising.clearData();
    Bluefruit.ScanResponse.clearData();

    Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
    Bluefruit.Advertising.addService(_service);
    Bluefruit.ScanResponse.addName();

    Bluefruit.Advertising.restartOnDisconnect(true);
    Bluefruit.Advertising.setInterval(32, 244);
    Bluefruit.Advertising.setFastTimeout(30);
    Bluefruit.Advertising.start(0);
    BITCHAT_DEBUG_PRINTLN("BLE advertising started");
}

void BitchatBLEService::onServerDisconnect() {
    uint8_t prev = __atomic_load_n(&_bitchatClientCount, __ATOMIC_RELAXED);
    if (prev > 0) {
        __atomic_store_n(&_bitchatClientCount, static_cast<uint8_t>(prev - 1), __ATOMIC_RELEASE);
    }
    if (prev <= 1) {
        __atomic_store_n(&_clientSubscribed, false, __ATOMIC_RELEASE);
        __atomic_store_n(&_pendingDisconnect, true, __ATOMIC_RELEASE);
    }
}

void BitchatBLEService::drainTxQueue() {
    if (_txCount == 0) return;

    if (!__atomic_load_n(&_clientSubscribed, __ATOMIC_ACQUIRE)) {
        _txCount = 0;
        _txInFlight = false;
        return;
    }

    uint32_t now = millis();
    while (_txCount > 0) {
        const TxSlot& slot = _txRing[_txHead];
        if (!_txInFlight) {
            _txStartedMs = now;
            _txInFlight = true;
        }
        uint16_t sent = _characteristic.notify(slot.data, slot.len);
        if (sent > 0) {
            _txHead = (_txHead + 1) % TX_RING_DEPTH;
            _txCount--;
            _txInFlight = false;
            continue;
        }
        // SoftDevice HVN queue full: retry on next loop() up to TX_GIVE_UP_MS
        if (now - _txStartedMs >= TX_GIVE_UP_MS) {
            BITCHAT_DEBUG_PRINTLN("TX notify timed out, dropping packet");
            _txHead = (_txHead + 1) % TX_RING_DEPTH;
            _txCount--;
            _txInFlight = false;
            continue;
        }
        break;
    }
}

void BitchatBLEService::loop() {
    if (__atomic_exchange_n(&_pendingConnect, false, __ATOMIC_ACQ_REL)) {
        if (_callback != nullptr) _callback->onBitchatClientConnect();
    }

    if (__atomic_exchange_n(&_pendingSubscribe, false, __ATOMIC_ACQ_REL)) {
        if (_callback != nullptr) _callback->onBitchatClientSubscribed();
    }

    if (__atomic_exchange_n(&_pendingDisconnect, false, __ATOMIC_ACQ_REL)) {
        _txCount = 0;
        _txInFlight = false;
        if (_callback != nullptr) _callback->onBitchatClientDisconnect();
    }

    drainTxQueue();

    uint32_t head = __atomic_load_n(&_rxHead, __ATOMIC_RELAXED);
    while (head != __atomic_load_n(&_rxTail, __ATOMIC_ACQUIRE)) {
        const RxSlot& slot = _rxRing[head % RX_RING_DEPTH];
        if (_callback != nullptr) {
            _callback->onBitchatRawPacket(slot.data, slot.len);
        }
        head++;
        __atomic_store_n(&_rxHead, head, __ATOMIC_RELEASE);
    }

    drainTxQueue();
}

bool BitchatBLEService::broadcastRaw(const uint8_t* data, size_t len) {
    if (!_serviceActive || data == nullptr || len == 0 || len > TX_SLOT_SIZE) {
        return false;
    }

    _characteristic.write(data, static_cast<uint16_t>(len));
    if (!__atomic_load_n(&_clientSubscribed, __ATOMIC_ACQUIRE)) {
        return true;
    }
    if (_txCount >= TX_RING_DEPTH) {
        BITCHAT_DEBUG_PRINTLN("TX queue full, packet dropped");
        return false;
    }
    size_t idx = (_txHead + _txCount) % TX_RING_DEPTH;
    _txRing[idx].len = static_cast<uint16_t>(len);
    memcpy(_txRing[idx].data, data, len);
    _txCount++;
    drainTxQueue();
    return true;
}

bool BitchatBLEService::broadcastMessage(const BitchatMessage& msg) {
    static uint8_t buffer[TX_SLOT_SIZE];
    size_t len = BitchatProtocol::serializeMessage(msg, buffer, sizeof(buffer));
    if (len == 0) return false;
    return broadcastRaw(buffer, len);
}

void BitchatBLEService::onConnect(uint16_t conn_handle) {
    (void)conn_handle;
    if (_instance != nullptr) {
        uint8_t c = __atomic_load_n(&_instance->_bitchatClientCount, __ATOMIC_RELAXED);
        __atomic_store_n(&_instance->_bitchatClientCount, static_cast<uint8_t>(c + 1), __ATOMIC_RELEASE);
        __atomic_store_n(&_instance->_pendingConnect, true, __ATOMIC_RELEASE);
    }
}

void BitchatBLEService::onDisconnect(uint16_t conn_handle, uint8_t reason) {
    (void)conn_handle; (void)reason;
    if (_instance != nullptr) {
        _instance->onServerDisconnect();
    }
}

void BitchatBLEService::onCharacteristicWrite(uint16_t conn_handle, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
    (void)conn_handle; (void)chr;
    if (_instance == nullptr || len == 0 || len > BITCHAT_BLE_RX_MAX) return;

    uint32_t tail = __atomic_load_n(&_instance->_rxTail, __ATOMIC_RELAXED);
    uint32_t head = __atomic_load_n(&_instance->_rxHead, __ATOMIC_ACQUIRE);
    if (tail - head >= RX_RING_DEPTH) return;

    RxSlot& slot = _rxRing[tail % RX_RING_DEPTH];
    slot.len = len;
    memcpy(slot.data, data, len);
    __atomic_store_n(&_instance->_rxTail, tail + 1, __ATOMIC_RELEASE);
}

void BitchatBLEService::onCharacteristicCccdWrite(uint16_t conn_handle, BLECharacteristic* chr, uint16_t cccd_value) {
    (void)conn_handle; (void)chr;
    if (_instance == nullptr) return;
    bool sub = (cccd_value & BLE_GATT_HVX_NOTIFICATION) != 0;
    __atomic_store_n(&_instance->_clientSubscribed, sub, __ATOMIC_RELEASE);
    if (sub) {
        __atomic_store_n(&_instance->_pendingSubscribe, true, __ATOMIC_RELEASE);
    }
}

#endif // NRF52_PLATFORM && ENABLE_BITCHAT
