## MeshCore-BitChat v0.4-bitchat

This release rebases the BitChat bridge onto the upstream **MeshCore v1.15.0** release.

### What's changed

- **Merged upstream MeshCore `v1.15.0`** (from MeshCore 1.14.1), bringing in ~145 upstream commits, including:
  - New **RegionMap / default-scope** support and refactored `region` CLI commands (repeater, room server, companion)
  - Companion `FIRMWARE_VER_CODE` bumped to 11
  - BLE/OTA improvements: DFU over BLE stack, GATT cache fix for Android re-pairing, SDK 3.x OTA fixes
  - Button polarity defaulted to active-LOW across firmware types (plus T1000E and GAT562 mesh-watch button fixes)
  - Power-saving improvements and auto-shutdown handling for e-ink / powered devices
  - Several new boards and sensor additions, plus assorted board/variant fixes
- BitChat bridge functionality unchanged; all 37 BitChat targets rebuilt against the new base.

### Firmware

Binaries are attached for all 37 BitChat-enabled targets:
- **ESP32 boards** (full long/compressed message support): `.bin` (app only) and `-merged.bin` (flash at `0x0`)
- **NRF52 boards** (short messages only): `.uf2` (drag-and-drop) and `.zip` (DFU)

See `docs/bitchat-bridge.md` for the full board list and flashing instructions.
