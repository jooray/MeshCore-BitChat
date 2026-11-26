#pragma once

#if defined(NRF52_PLATFORM)

#include <Dispatcher.h>
#include "StaticPacketPool.h"
#include "../StaticPoolPacketManager.h"

// NRF52-specific packet manager using truly static allocation
// Moves ~1.1KB from heap to .bss section, leaving heap unfragmented for SoftDevice BLE
class NRF52StaticPoolPacketManager : public mesh::PacketManager {
  PacketQueue unused, send_queue, rx_queue;

public:
  NRF52StaticPoolPacketManager();

  mesh::Packet* allocNew() override;
  void free(mesh::Packet* packet) override;
  void queueOutbound(mesh::Packet* packet, uint8_t priority, uint32_t scheduled_for) override;
  mesh::Packet* getNextOutbound(uint32_t now) override;
  int getOutboundCount(uint32_t now) const override;
  int getOutboundTotal() const override;
  int getFreeCount() const override;
  mesh::Packet* getOutboundByIdx(int i) override;
  mesh::Packet* removeOutboundByIdx(int i) override;
  void queueInbound(mesh::Packet* packet, uint32_t scheduled_for) override;
  mesh::Packet* getNextInbound(uint32_t now) override;
};

#endif  // NRF52_PLATFORM
