#include "StaticPacketPool.h"

#if defined(NRF52_PLATFORM)

namespace nrf52 {

// Define static storage - this goes in .bss section, not heap
// Total: ~1,140 bytes moved from heap to static allocation
mesh::Packet g_packetPool[NRF52_PACKET_POOL_SIZE];

mesh::Packet* g_queueTables[3][NRF52_PACKET_POOL_SIZE];
uint8_t g_priorityTables[3][NRF52_PACKET_POOL_SIZE];
uint32_t g_scheduleTables[3][NRF52_PACKET_POOL_SIZE];

}  // namespace nrf52

#endif  // NRF52_PLATFORM
