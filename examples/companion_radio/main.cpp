#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"
#include <helpers/bitchat/BitchatDebugSerial.h>  // BITCHAT_SERIAL: silent unless BITCHAT_DEBUG_SERIAL=1

#if defined(NRF52_SERIES) && defined(ENABLE_BITCHAT)  // stack diagnostics live in the bitchat sources
extern "C" void print_stack_usage(void);
#endif

#if defined(ENABLE_WIFI_INTERFACE) && defined(ENABLE_ESP_MDNS)
#define HAS_ESP_MDNS
#include <ESPmDNS.h>
#endif

// Believe it or not, this std C function is busted on some platforms!
static uint32_t _atoi(const char* sp) {
  uint32_t n = 0;
  while (*sp && *sp >= '0' && *sp <= '9') {
    n *= 10;
    n += (*sp++ - '0');
  }
  return n;
}

// interface manager
// The meshsolar library (Heltec_mesh_solar target.h) does "#define NONE 1", which
// breaks InterfaceType::NONE; hide the macro while the header is parsed.
#pragma push_macro("NONE")
#undef NONE
#include <helpers/MultiSerialInterface.h>
#pragma pop_macro("NONE")
MultiSerialInterface interface_manager;

// include bluetooth interface
#if defined(BLE_PIN_CODE)
  #ifdef ESP32
    // include esp32 bluetooth interface
    #include <helpers/esp32/SerialBLEInterface.h>
    SerialBLEInterface bluetooth_interface;
  #elif defined(NRF52_PLATFORM)
    // include nrf52 bluetooth interface
    #include <helpers/nrf52/SerialBLEInterface.h>
    SerialBLEInterface bluetooth_interface;
  #elif defined(RP2040_PLATFORM)
    // include rp2040 (Pico W / CYW43) bluetooth interface
    #include <helpers/rp2040/SerialBLEInterface.h>
    SerialBLEInterface bluetooth_interface;
  #else
    #error "SerialBLEInterface is not defined for this platform"
  #endif
#endif

// include wifi interface
#ifdef ENABLE_WIFI_INTERFACE
  #ifndef WIFI_SSID
    #define WIFI_SSID ""
  #endif
  #ifndef WIFI_PWD
    #define WIFI_PWD ""
  #endif
  #ifndef TCP_PORT
    #define TCP_PORT 5000
  #endif
  #ifndef WIFI_RETRY_INTERVAL
    #if defined(RP2040_PLATFORM)
      #define WIFI_RETRY_INTERVAL 30000   // each attempt blocks loop(), so retry less often
    #else
      #define WIFI_RETRY_INTERVAL 10000   // millis between reconnect attempts
    #endif
  #endif
  #ifndef WIFI_RETRY_TIMEOUT
    #define WIFI_RETRY_TIMEOUT 5000     // RP2040: cap on how long one join may block loop()
  #endif
  #if defined(ESP32) || defined(RP2040_PLATFORM)
    #include <helpers/wifi/SerialWifiInterface.h>
    SerialWifiInterface wifi_interface;
  #else
    #error "SerialWifiInterface is not defined for this platform"
  #endif
#endif

// include usb interface
#if defined(ENABLE_USB_INTERFACE)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface usb_serial_interface;
#endif

// include ethernet interface
#if defined(ETHERNET_ENABLED)
  #include <helpers/ethernet/EthernetInterface.h>
  ETHERNET_CLASS ethernet_interface;
#endif

// include hardware serial interface
#if defined(SERIAL_RX)
  #include <helpers/ArduinoSerialInterface.h>
  ArduinoSerialInterface hardware_serial_interface;
  HardwareSerial companion_serial(1);
#endif

// platform file system
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  #include <InternalFileSystem.h>
  #if defined(QSPIFLASH)
    #include <CustomLFS_QSPIFlash.h>
    DataStore store(InternalFS, QSPIFlash, rtc_clock);
  #else
    #if defined(EXTRAFS)
      #include <CustomLFS.h>
      CustomLFS ExtraFS(0xD4000, 0x19000, 128);
      DataStore store(InternalFS, ExtraFS, rtc_clock);
    #else
      DataStore store(InternalFS, rtc_clock);
    #endif
  #endif
#elif defined(RP2040_PLATFORM)
  #include <LittleFS.h>
  DataStore store(LittleFS, rtc_clock);
#elif defined(ESP32)
  #include <SPIFFS.h>
  DataStore store(SPIFFS, rtc_clock);
#endif

/* GLOBAL OBJECTS */
#ifdef DISPLAY_CLASS
  #include "UITask.h"
  UITask ui_task(&board, &interface_manager);
#endif

#if defined(ENABLE_BITCHAT) && (defined(ESP32) || defined(NRF52_PLATFORM))
  #include <helpers/bitchat/BitchatBridge.h>
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store);

#if defined(ENABLE_BITCHAT) && (defined(ESP32) || defined(NRF52_PLATFORM))
  BitchatBridge* bitchat_bridge = nullptr;
#endif

/* END GLOBAL OBJECTS */

void halt() {
  while (1) ;
}

/* WIFI RECONNECT TRACKERS */
#ifdef ENABLE_WIFI_INTERFACE
  bool wifi_needs_reconnect = false;
  unsigned long last_wifi_reconnect_attempt = 0;
  char wifi_ssid[33] = WIFI_SSID;   // replaced by stored prefs at boot, if set
  char wifi_pwd[64] = WIFI_PWD;
  bool wifi_was_connected = false;
  bool wifi_enabled = false;   // set at boot from prefs; false also when the effective SSID is blank
#endif

#if defined(DISPLAY_CLASS) && defined(EINK_REFRESH_LOOP)
static void loopDuringDisplayRefresh();
#endif

void setup() {
  Serial.begin(115200);

  // Debug builds only (-D BITCHAT_DEBUG_SERIAL=1): wait for the USB serial console on NRF52
  #if defined(NRF52_PLATFORM) && BITCHAT_DEBUG_SERIAL_ENABLED
  // Wait for USB serial to be ready (with timeout)
  uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart < 3000)) {
    delay(10);
  }
  // Serial port ready - skip boot wait since crashes happen much later
  #endif
  BITCHAT_SERIAL.println("\n\n=== BOOT START ===");
  BITCHAT_SERIAL.flush();

  board.begin();
  BITCHAT_SERIAL.println("board.begin() done");
  BITCHAT_SERIAL.flush();

#if defined(NRF52_SERIES) && defined(ENABLE_BITCHAT)  // stack diagnostics live in the bitchat sources
  // Paint ISR stack with canary pattern for high-water marking
  extern unsigned long __StackLimit;
  extern unsigned long __StackTop;
  uint32_t* isr_stack_ptr = (uint32_t*)&__StackLimit;
  uint32_t isr_stack_words = ((uint32_t)&__StackTop - (uint32_t)&__StackLimit) / 4;
  for (uint32_t i = 0; i < isr_stack_words; i++) {
      isr_stack_ptr[i] = 0xa5a5a5a5;  // FreeRTOS canary pattern
  }
  BITCHAT_SERIAL.println("ISR stack painted with canary pattern");
  BITCHAT_SERIAL.flush();
#endif

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#ifdef DISPLAY_CLASS
  DisplayDriver* disp = NULL;
  if (display.begin()) {
    disp = &display;
    disp->startFrame();
  #ifdef ST7789
    disp->setTextSize(2);
  #endif
    disp->drawTextCentered(disp->width() / 2, 28, "Loading...");
    disp->endFrame();
  }
#endif

  if (!radio_init()) { halt(); }

  fast_rng.begin(radio_driver.getRngSeed());

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  #if defined(QSPIFLASH)
    if (!QSPIFlash.begin()) {
      // debug output might not be available at this point, might be too early. maybe should fall back to InternalFS here?
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: failed to initialize");
    } else {
      MESH_DEBUG_PRINTLN("CustomLFS_QSPIFlash: initialized successfully");
    }
  #else
  #if defined(EXTRAFS)
      ExtraFS.begin();
  #endif
  #endif
  store.begin();
  the_mesh.begin();
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin();
#elif defined(ESP32)
  SPIFFS.begin(true);
  store.begin();
  the_mesh.begin();
#else
  #error "need to define filesystem"
#endif

#ifdef BLE_PIN_CODE // 123456 by default
  if (the_mesh.getNodePrefs()->ble_pin == 0) {
#ifdef DISPLAY_CLASS
    if (disp != NULL && BLE_PIN_CODE == 123456) {
      StdRNG rng;
      the_mesh.setBLEPin(rng.nextInt(100000, 999999)); // random pin each session
    } else {
      the_mesh.setBLEPin(BLE_PIN_CODE); // otherwise static pin
    }
#else
    the_mesh.setBLEPin(BLE_PIN_CODE); // otherwise static pin
#endif
  } else {
    the_mesh.setBLEPin(the_mesh.getNodePrefs()->ble_pin);
  }
#else
  the_mesh.setBLEPin(0);
#endif

// add bluetooth interface
#if defined(BLE_PIN_CODE)
  bluetooth_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
  interface_manager.addInterface(InterfaceType::Bluetooth, &bluetooth_interface);
#endif

// add wifi interface
#ifdef ENABLE_WIFI_INTERFACE
  // use wifi ssid and password from prefs if ssid is not empty, otherwise use the build flag defaults
  if (the_mesh.getNodePrefs()->wifi_ssid[0]) {
    strcpy(wifi_ssid, the_mesh.getNodePrefs()->wifi_ssid);
    strcpy(wifi_pwd, the_mesh.getNodePrefs()->wifi_pwd);
  }
  // only start wifi if enabled and ssid is not empty
  wifi_enabled = the_mesh.getNodePrefs()->wifi_enabled;
  if (wifi_enabled && wifi_ssid[0]) {
#if defined(ESP32)
    board.setInhibitSleep(true);   // prevent sleep when WiFi is active
    WiFi.setAutoReconnect(true);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            WIFI_DEBUG_PRINTLN("WiFi disconnected (reason=%s). Flagging for reconnect...",
              WiFi.disconnectReasonName((wifi_err_reason_t)info.wifi_sta_disconnected.reason));
            wifi_needs_reconnect = true;
        } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
            WIFI_DEBUG_PRINTLN("WiFi connected successfully!");
            wifi_needs_reconnect = false;
        }
    });
#endif

    WIFI_DEBUG_PRINTLN("connecting to %s", wifi_ssid);

#if defined(RP2040_PLATFORM)
    // the join itself blocks inside the core (CYW43::begin busy-waits for the
    // association), so every attempt stalls the mesh loop. beginNoBlock() only skips the
    // extra DHCP wait. Give the first connect a full window, then bound the retries below.
    WiFi.beginNoBlock(wifi_ssid, wifi_pwd);
    last_wifi_reconnect_attempt = millis();   // let DHCP finish before the poll can retry
#else
    WiFi.begin(wifi_ssid, wifi_pwd);
#endif
    wifi_interface.begin(TCP_PORT);
    interface_manager.addInterface(InterfaceType::WiFi, &wifi_interface);
  } else {
    WIFI_DEBUG_PRINTLN("wifi disabled");
  }
#endif

// add usb interface
#if defined(ENABLE_USB_INTERFACE)
  usb_serial_interface.begin(Serial);
  interface_manager.addInterface(InterfaceType::USB, &usb_serial_interface);
#endif

// add ethernet interface
#if defined(ETHERNET_ENABLED)
  ethernet_interface.begin();
  interface_manager.addInterface(InterfaceType::Ethernet, &ethernet_interface);
#endif

#ifdef HAS_ESP_MDNS
  const char* nodeName = the_mesh.getNodeName();

  // "meshcore_" prefix + sanitized node name, capped at 63 chars (max DNS label length)
  char mdnsName[64];
  const size_t prefixLen = snprintf(mdnsName, sizeof(mdnsName), "meshcore_");
  size_t pos = prefixLen;
  for (size_t i = 0; nodeName[i] != '\0' && pos < sizeof(mdnsName) - 1; ++i) {
    const char c = nodeName[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_') {
      mdnsName[pos++] = c;
    }
  }
  mdnsName[pos] = '\0';

  if (!MDNS.begin(mdnsName)) {
    WIFI_DEBUG_PRINTLN("mDNS failed to start!");
  } else {
    MDNS.addService("meshcore", "tcp", TCP_PORT);
    MDNS.addServiceTxt("meshcore", "tcp", "node-name", nodeName);
    MDNS.addServiceTxt("meshcore", "tcp", "firmware-version", FIRMWARE_VERSION);
    char protocol[12];
    snprintf(protocol, sizeof(protocol), "%d", (int)FIRMWARE_VER_CODE);
    MDNS.addServiceTxt("meshcore", "tcp", "protocol-version", (const char*)protocol);
    MDNS.addServiceTxt("meshcore", "tcp", "build-date", __DATE__);
    MDNS.addServiceTxt("meshcore", "tcp", "build-time", __TIME__);
  }
#endif

// add hardware serial interface
#if defined(SERIAL_RX)
  companion_serial.setPins(SERIAL_RX, SERIAL_TX);
  companion_serial.begin(115200);
  hardware_serial_interface.begin(companion_serial);
  interface_manager.addInterface(InterfaceType::HardwareSerial, &hardware_serial_interface);
#endif

// add bitchat bridge, must be after the bluetooth interface has been started
#if defined(ENABLE_BITCHAT) && (defined(ESP32) || defined(NRF52_PLATFORM))
  bitchat_bridge = new BitchatBridge(the_mesh, the_mesh.self_id, the_mesh.getNodeName(),
                                    the_mesh.getNodePrefs()->path_hash_mode);
  if (bitchat_bridge == nullptr) {
    BITCHAT_SERIAL.println("ERROR: BitchatBridge allocation failed!");
  } else {
    bitchat_bridge->begin();
  #if defined(BLE_PIN_CODE) && defined(ESP32)
    // share the BLE server with the MeshCore companion interface
    if (bluetooth_interface.getBLEServer() != nullptr
          && bitchat_bridge->attachBLEService(bluetooth_interface.getBLEServer())) {
      bluetooth_interface.setBitchatService(&bitchat_bridge->getBLEService());
      BITCHAT_SERIAL.println("Bitchat BLE service attached");
    } else {
      BITCHAT_SERIAL.println("ERROR: Failed to attach Bitchat BLE service!");
    }
  #else
    // no companion BLE interface, Bitchat owns the BLE stack
    if (bitchat_bridge->beginStandalone(the_mesh.getNodeName())) {
      BITCHAT_SERIAL.println("Bitchat BLE service started (standalone mode)");
    } else {
      BITCHAT_SERIAL.println("ERROR: Failed to start Bitchat BLE service!");
    }
  #endif
    the_mesh.initBitchat(bitchat_bridge);
  }
#endif

  the_mesh.startInterface(interface_manager);
  sensors.begin();

#if ENV_INCLUDE_GPS == 1
  the_mesh.applyGpsPrefs();
#endif

#ifdef DISPLAY_CLASS
  ui_task.begin(disp, &sensors, the_mesh.getNodePrefs());  // still want to pass this in as dependency, as prefs might be moved
  the_mesh.setListener(&ui_task);
#ifdef EINK_REFRESH_LOOP
  if (disp != NULL) disp->setBusyHook(loopDuringDisplayRefresh);
#endif
#endif

  board.onBootComplete();
}

// Work that must keep running even while the UI is blocked (e.g. by an e-ink refresh).
// Called from loop() and from the display busy hook, so both always run the same set.
static void loopCore() {
  the_mesh.loop();
  interface_manager.loop();
  sensors.loop();
}

#if defined(DISPLAY_CLASS) && defined(EINK_REFRESH_LOOP)
// Runs from inside the e-ink busy wait, so radio/BLE/sensors keep going during a panel refresh.
// Must not touch the display: UI listener callbacks only update state and schedule the next frame.
static void loopDuringDisplayRefresh() {
  static bool active = false;
  if (active) return;
  active = true;
  loopCore();
  active = false;
}
#endif

void loop() {
  loopCore();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
  board.loop();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif

#if defined(ENABLE_BITCHAT) && (defined(ESP32) || defined(NRF52_PLATFORM))
  if (bitchat_bridge != nullptr) {
    bitchat_bridge->loop();
  }
#endif

  if (!the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#endif
  }

#if defined(NRF52_SERIES) && defined(ENABLE_BITCHAT) && BITCHAT_DEBUG_SERIAL_ENABLED  // stack diagnostics, debug builds only
  // Print stack usage every 10 seconds for diagnostics
  static uint32_t last_stack_print = 0;
  if (millis() - last_stack_print >= 10000) {
    print_stack_usage();
    last_stack_print = millis();
  }
#endif

#ifdef ENABLE_WIFI_INTERFACE
  if (wifi_enabled) {
    // RP2040 has no WiFi event callbacks, so poll the link state instead
  #if defined(RP2040_PLATFORM)
      wifi_needs_reconnect = (WiFi.status() != WL_CONNECTED);
      if (wifi_was_connected == wifi_needs_reconnect) {   // link state changed
        wifi_was_connected = !wifi_needs_reconnect;
        if (wifi_was_connected) {
          WIFI_DEBUG_PRINTLN("connected, listening on %s:%d", WiFi.localIP().toString().c_str(), TCP_PORT);
        } else {
          WIFI_DEBUG_PRINTLN("link lost");
        }
      }
  #endif

    // Safely attempt to reconnect if flagged. On RP2040 each attempt blocks the mesh loop
    // for up to WIFI_RETRY_TIMEOUT, so retry less often and cap how long a join may stall.
    if (wifi_needs_reconnect && (millis() - last_wifi_reconnect_attempt > WIFI_RETRY_INTERVAL)) {
      WIFI_DEBUG_PRINTLN("Attempting manual WiFi reconnect to %s (status %d)...", wifi_ssid, WiFi.status());
    #if defined(RP2040_PLATFORM)
        WiFi.setTimeout(WIFI_RETRY_TIMEOUT);
        WiFi.beginNoBlock(wifi_ssid, wifi_pwd);   // no reconnect() on this platform
    #else
        WiFi.disconnect();
        WiFi.reconnect();
    #endif
      last_wifi_reconnect_attempt = millis();
    }
  }
#endif
}
