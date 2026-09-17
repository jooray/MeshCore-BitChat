#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include "MyMesh.h"

#ifdef NRF52_SERIES
extern "C" void print_stack_usage(void);
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
#include <helpers/MultiSerialInterface.h>
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
  #else
    #error "SerialBLEInterface is not defined for this platform"
  #endif
#endif

// include wifi interface
#ifdef WIFI_SSID
  #ifndef TCP_PORT
    #define TCP_PORT 5000
  #endif
  #ifdef ESP32
    // include esp32 wifi interface
    #include <helpers/esp32/SerialWifiInterface.h>
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
MyMesh the_mesh(radio_driver, fast_rng, rtc_clock, tables, store
   #ifdef DISPLAY_CLASS
      , &ui_task
   #endif
);

#if defined(ENABLE_BITCHAT) && (defined(ESP32) || defined(NRF52_PLATFORM))
  BitchatBridge* bitchat_bridge = nullptr;
#endif

/* END GLOBAL OBJECTS */

void halt() {
  while (1) ;
}

/* WIFI RECONNECT TRACKERS */
#if defined(ESP32) && defined(WIFI_SSID)
  bool wifi_needs_reconnect = false;
  unsigned long last_wifi_reconnect_attempt = 0;
#endif

void setup() {
  Serial.begin(115200);

  // Early debug - wait for USB serial on NRF52
  #if defined(NRF52_PLATFORM)
  // Wait for USB serial to be ready (with timeout)
  uint32_t serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart < 3000)) {
    delay(10);
  }
  // Serial port ready - skip boot wait since crashes happen much later
  #endif
  Serial.println("\n\n=== BOOT START ===");
  Serial.flush();

  board.begin();
  Serial.println("board.begin() done");
  Serial.flush();

#ifdef NRF52_SERIES
  // Paint ISR stack with canary pattern for high-water marking
  extern unsigned long __StackLimit;
  extern unsigned long __StackTop;
  uint32_t* isr_stack_ptr = (uint32_t*)&__StackLimit;
  uint32_t isr_stack_words = ((uint32_t)&__StackTop - (uint32_t)&__StackLimit) / 4;
  for (uint32_t i = 0; i < isr_stack_words; i++) {
      isr_stack_ptr[i] = 0xa5a5a5a5;  // FreeRTOS canary pattern
  }
  Serial.println("ISR stack painted with canary pattern");
  Serial.flush();
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
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#elif defined(ESP32)
  SPIFFS.begin(true);
  store.begin();
  the_mesh.begin(
    #ifdef DISPLAY_CLASS
        disp != NULL
    #else
        false
    #endif
  );
#else
  #error "need to define filesystem"
#endif

// add bluetooth interface
#if defined(BLE_PIN_CODE)
  bluetooth_interface.begin(BLE_NAME_PREFIX, the_mesh.getNodePrefs()->node_name, the_mesh.getBLEPin());
  interface_manager.addInterface(InterfaceType::Bluetooth, &bluetooth_interface);
#endif

// add wifi interface
#ifdef WIFI_SSID
  board.setInhibitSleep(true);   // prevent sleep when WiFi is active
  WiFi.setAutoReconnect(true);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
      if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
          WIFI_DEBUG_PRINTLN("WiFi disconnected. Flagging for reconnect...");
          wifi_needs_reconnect = true;
      } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
          WIFI_DEBUG_PRINTLN("WiFi connected successfully!");
          wifi_needs_reconnect = false;
      }
  });

  WiFi.begin(WIFI_SSID, WIFI_PWD);
  wifi_interface.begin(TCP_PORT);
  interface_manager.addInterface(InterfaceType::WiFi, &wifi_interface);
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
                                    the_mesh.getNodePrefs()->bitchat_region);
  if (bitchat_bridge == nullptr) {
    Serial.println("ERROR: BitchatBridge allocation failed!");
  } else {
    bitchat_bridge->begin();
  #if defined(BLE_PIN_CODE) && defined(ESP32)
    // share the BLE server with the MeshCore companion interface
    if (bluetooth_interface.getBLEServer() != nullptr
          && bitchat_bridge->attachBLEService(bluetooth_interface.getBLEServer())) {
      bluetooth_interface.setBitchatService(&bitchat_bridge->getBLEService());
      Serial.println("Bitchat BLE service attached");
    } else {
      Serial.println("ERROR: Failed to attach Bitchat BLE service!");
    }
  #else
    // no companion BLE interface, Bitchat owns the BLE stack
    if (bitchat_bridge->beginStandalone(the_mesh.getNodeName())) {
      Serial.println("Bitchat BLE service started (standalone mode)");
    } else {
      Serial.println("ERROR: Failed to start Bitchat BLE service!");
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
#endif

  board.onBootComplete();
}

void loop() {
  the_mesh.loop();
  interface_manager.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
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

#ifdef NRF52_SERIES
  // Print stack usage every 10 seconds for diagnostics
  static uint32_t last_stack_print = 0;
  if (millis() - last_stack_print >= 10000) {
    print_stack_usage();
    last_stack_print = millis();
  }
#endif

#if defined(ESP32) && defined(WIFI_SSID)
  // Safely attempt to reconnect every 10 seconds if flagged
  if (wifi_needs_reconnect && (millis() - last_wifi_reconnect_attempt > 10000)) {
    WIFI_DEBUG_PRINTLN("Attempting manual WiFi reconnect...");
    WiFi.disconnect();
    WiFi.reconnect();
    last_wifi_reconnect_attempt = millis();
  }
#endif
}
