#pragma once

// Debug output for the Bitchat bridge and its crash diagnostics.
//
// On boards where the companion protocol runs over the USB Serial port, any
// extra text written to Serial corrupts the companion stream. All Bitchat
// diagnostics therefore go through BITCHAT_SERIAL, which is a silent sink
// unless the firmware is built with -D BITCHAT_DEBUG_SERIAL=1 (or one of the
// more specific -D BITCHAT_DEBUG=1 / -D BITCHAT_DEBUG_PACKETDUMP=1 flags).
//
// Use only for debugging with a serial console, never in release builds that
// are used with a companion app over USB.

#include <stddef.h>
#include <stdint.h>

#if !defined(BITCHAT_DEBUG_SERIAL) && (defined(BITCHAT_DEBUG) || defined(BITCHAT_DEBUG_PACKETDUMP))
  #if (BITCHAT_DEBUG + 0) || (BITCHAT_DEBUG_PACKETDUMP + 0)
    #define BITCHAT_DEBUG_SERIAL 1
  #endif
#endif

#if defined(BITCHAT_DEBUG_SERIAL) && (BITCHAT_DEBUG_SERIAL + 0)
  #include <Arduino.h>
  #define BITCHAT_SERIAL Serial
  #define BITCHAT_DEBUG_SERIAL_ENABLED 1
#else
  struct BitchatNullSerial {
    template <typename... Args> size_t print(Args&&...) { return 0; }
    template <typename... Args> size_t println(Args&&...) { return 0; }
    template <typename... Args> size_t printf(const char*, Args&&...) { return 0; }
    void flush() {}
    explicit operator bool() const { return false; }
  };
  static BitchatNullSerial bitchatNullSerial;
  #define BITCHAT_SERIAL bitchatNullSerial
  #define BITCHAT_DEBUG_SERIAL_ENABLED 0
#endif
