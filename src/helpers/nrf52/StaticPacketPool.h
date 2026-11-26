#pragma once

#if defined(NRF52_PLATFORM)

#include <Packet.h>

// Static packet pool for NRF52 - avoids heap fragmentation
// Pool size fixed at compile time to enable static allocation
// This moves ~1.1KB from heap to .bss section, leaving heap unfragmented for SoftDevice BLE

#define NRF52_PACKET_POOL_SIZE 4

namespace nrf52 {

// Static storage - allocated in .bss section at compile time
extern mesh::Packet g_packetPool[NRF52_PACKET_POOL_SIZE];

// Queue table storage (3 queues: unused, send, rx)
extern mesh::Packet* g_queueTables[3][NRF52_PACKET_POOL_SIZE];
extern uint8_t g_priorityTables[3][NRF52_PACKET_POOL_SIZE];
extern uint32_t g_scheduleTables[3][NRF52_PACKET_POOL_SIZE];

}  // namespace nrf52

#endif  // NRF52_PLATFORM
