#include "NRF52StaticPoolPacketManager.h"

#if defined(NRF52_PLATFORM)

using namespace nrf52;

NRF52StaticPoolPacketManager::NRF52StaticPoolPacketManager()
    : unused(g_queueTables[0], g_priorityTables[0], g_scheduleTables[0], NRF52_PACKET_POOL_SIZE),
      send_queue(g_queueTables[1], g_priorityTables[1], g_scheduleTables[1], NRF52_PACKET_POOL_SIZE),
      rx_queue(g_queueTables[2], g_priorityTables[2], g_scheduleTables[2], NRF52_PACKET_POOL_SIZE) {
  // Initialize unused queue with static packet pool
  for (int i = 0; i < NRF52_PACKET_POOL_SIZE; i++) {
    unused.add(&g_packetPool[i], 0, 0);
  }
}

// All methods delegate to same logic as StaticPoolPacketManager
mesh::Packet* NRF52StaticPoolPacketManager::allocNew() {
  return unused.removeByIdx(0);
}

void NRF52StaticPoolPacketManager::free(mesh::Packet* packet) {
  unused.add(packet, 0, 0);
}

void NRF52StaticPoolPacketManager::queueOutbound(mesh::Packet* packet, uint8_t priority, uint32_t scheduled_for) {
  send_queue.add(packet, priority, scheduled_for);
}

mesh::Packet* NRF52StaticPoolPacketManager::getNextOutbound(uint32_t now) {
  return send_queue.get(now);
}

int NRF52StaticPoolPacketManager::getOutboundCount(uint32_t now) const {
  return send_queue.countBefore(now);
}

int NRF52StaticPoolPacketManager::getOutboundTotal() const {
  return send_queue.count();
}

int NRF52StaticPoolPacketManager::getFreeCount() const {
  return unused.count();
}

mesh::Packet* NRF52StaticPoolPacketManager::getOutboundByIdx(int i) {
  return send_queue.itemAt(i);
}

mesh::Packet* NRF52StaticPoolPacketManager::removeOutboundByIdx(int i) {
  return send_queue.removeByIdx(i);
}

void NRF52StaticPoolPacketManager::queueInbound(mesh::Packet* packet, uint32_t scheduled_for) {
  rx_queue.add(packet, 0, scheduled_for);
}

mesh::Packet* NRF52StaticPoolPacketManager::getNextInbound(uint32_t now) {
  return rx_queue.get(now);
}

#endif  // NRF52_PLATFORM
