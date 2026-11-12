# Bitchat Bridge

The Bitchat Bridge enables communication between the [Bitchat](https://bitchat.app) Android app and MeshCore mesh network. Messages sent to the `#mesh` channel in Bitchat are relayed to MeshCore nodes, and vice versa.

## Overview

- **Bridge Direction**: Bidirectional - messages flow both ways between Bitchat and MeshCore
- **Channel**: Only the `#mesh` channel is bridged (hardcoded)
- **Identification**: Messages from Bitchat users appear with a phone emoji prefix on MeshCore nodes
- **Platforms**: ESP32 (full support) and NRF52 (limited - short messages only)

## Supported Boards

### ESP32-based (19 targets) - Full Support

ESP32 boards support long/compressed messages via ROM-based miniz decompression.

| Board | Target | MCU | Tested |
|-------|--------|-----|--------|
| Heltec LoRa32 V2 | `Heltec_v2_companion_radio_usb_bitchat` | ESP32 | |
| Heltec LoRa32 V3 | `Heltec_v3_companion_radio_usb_bitchat` | ESP32-S3 | |
| Heltec WSL3 | `Heltec_WSL3_companion_radio_usb_bitchat` | ESP32-S3 | ✓ |
| Heltec LoRa32 V4 | `heltec_v4_companion_radio_usb_bitchat` | ESP32-S3 | |
| Heltec CT62 | `Heltec_ct62_companion_radio_usb_bitchat` | ESP32-C3 | |
| Heltec Tracker V2 | `heltec_tracker_v2_companion_radio_usb_bitchat` | ESP32 | |
| LilyGo T3-S3 SX1262 | `LilyGo_T3S3_sx1262_companion_radio_usb_bitchat` | ESP32-S3 | |
| LilyGo T3-S3 SX1276 | `LilyGo_T3S3_sx1276_companion_radio_usb_bitchat` | ESP32-S3 | |
| LilyGo T-Deck | `LilyGo_TDeck_companion_radio_usb_bitchat` | ESP32-S3 | |
| LilyGo T-Lora V2.1 | `LilyGo_TLora_V2_1_1_6_companion_radio_usb_bitchat` | ESP32 | ✓ |
| Station G2 | `Station_G2_companion_radio_usb_bitchat` | ESP32 | |
| Seeed Xiao C3 | `Xiao_C3_companion_radio_usb_bitchat` | ESP32-C3 | |
| Seeed Xiao S3 WIO | `Xiao_S3_WIO_companion_radio_usb_bitchat` | ESP32-S3 | |
| Ebyte EoRa-S3 | `Ebyte_EoRa-S3_companion_radio_usb_bitchat` | ESP32-S3 | |
| Meshadventurer SX1262 | `Meshadventurer_sx1262_companion_radio_usb_bitchat` | ESP32 | |
| Meshadventurer SX1268 | `Meshadventurer_sx1268_companion_radio_usb_bitchat` | ESP32 | |
| ThinkNode M2 | `ThinkNode_M2_companion_radio_usb_bitchat` | ESP32-S3 | |
| ThinkNode M5 | `ThinkNode_M5_companion_radio_usb_bitchat` | ESP32-S3 | |

### NRF52-based (18 targets) - Limited Support

NRF52 boards do **not** support long/compressed messages due to heap constraints (SoftDevice BLE reserves significant memory). Short messages (<256 bytes uncompressed) work fine.

| Board | Target | Tested |
|-------|--------|--------|
| Wio Tracker L1 Pro | `WioTrackerL1_companion_radio_usb_bitchat` | ✓ |
| RAK 4631 | `RAK_4631_companion_radio_usb_bitchat` | |
| RAK WisMesh Tag | `RAK_WisMesh_Tag_companion_radio_usb_bitchat` | |
| Seeed Xiao NRF52 | `Xiao_nrf52_companion_radio_usb_bitchat` | |
| LilyGo T-Echo | `LilyGo_T-Echo_companion_radio_usb_bitchat` | |
| Heltec Mesh Solar | `Heltec_mesh_solar_companion_radio_usb_bitchat` | |
| Heltec T114 | `Heltec_t114_companion_radio_usb_bitchat` | |
| Heltec T114 (no display) | `Heltec_t114_without_display_companion_radio_usb_bitchat` | |
| Keepteen LT1 | `KeepteenLT1_companion_radio_usb_bitchat` | |
| Mesh Pocket | `Mesh_pocket_companion_radio_usb_bitchat` | |
| Pro Micro | `ProMicro_companion_radio_usb_bitchat` | |
| SenseCap Solar | `SenseCap_Solar_companion_radio_usb_bitchat` | |
| ThinkNode M1 | `ThinkNode_M1_companion_radio_usb_bitchat` | |
| Nano G2 Ultra | `Nano_G2_Ultra_companion_radio_usb_bitchat` | |
| Ikoka Handheld | `ikoka_handheld_nrf_e22_30dbm_companion_radio_usb_bitchat` | |
| Ikoka Stick | `ikoka_stick_nrf_30dbm_companion_radio_usb_bitchat` | |
| Ikoka Nano | `ikoka_nano_nrf_30dbm_companion_radio_usb_bitchat` | |
| Minewsemi ME25LS01 | `Minewsemi_me25ls01_companion_radio_usb_bitchat` | |
| T1000-E | `t1000e_companion_radio_usb_bitchat` | |

### Long Message Support

| Platform | Long Messages | Reason |
|----------|---------------|--------|
| ESP32/S3/C3 | ✓ Yes | ROM-based miniz (no heap usage) |
| NRF52 | ✗ No | Heap too small for miniz + SoftDevice BLE |

**Recommendation**: Use ESP32-based boards for full Bitchat functionality.

## Build Targets

### USB + Bitchat (`*_companion_radio_usb_bitchat`)

This is the recommended configuration:

- **MeshCore companion app**: Connects via USB serial
- **Bitchat app**: Connects via standalone BLE

This gives the best experience because:
- The MeshCore web app works reliably via USB serial
- Bitchat has dedicated BLE access without sharing with MeshCore

### Building

```bash
# Build for Heltec Wireless Stick Lite V3
pio run -e Heltec_WSL3_companion_radio_usb_bitchat

# Flash
pio run -e Heltec_WSL3_companion_radio_usb_bitchat -t upload
```


## Troubleshooting

### I don't see the peer in my Bitchat app

You probably used the same node with Meshcore Companion BLE node and
your phone still connects to it. Unpair it in bluetooth settings.
Bitchat does not require, nor supports pairing.

If the BLE connection is fine but the peer never appears in the peer list, the app is
most likely rejecting the bridge's ANNOUNCE. Current builds drop an announce when:

- the clock is more than 10 minutes off (2 minutes on iOS) — set the node's time from the
  MeshCore companion app, or keep a phone with Bitchat next to the bridge until it takes the
  time from that phone's announce (see "Timestamps" in the baseline below);
- the signal is below the app's RSSI threshold for its current power mode;
- the announce is unsigned or its sender ID is not derived from the noise key
  (see [Bitchat Protocol Baseline](#bitchat-protocol-baseline)).

## How It Works

### Channel Bridging

1. **Bitchat to MeshCore**: When a Bitchat user sends a message to `#mesh`, the bridge:
   - Receives the message via BLE
   - Decompresses it (Bitchat uses zlib compression)
   - Extracts the sender nickname and message content
   - Sends it to the MeshCore #mesh with a phone emoji prefix

2. **MeshCore to Bitchat**: When a MeshCore node sends a message to the `#mesh` channel, the bridge:
   - Receives the mesh packet
   - Creates a Bitchat MESSAGE packet with the sender name and content
   - Broadcasts it via BLE

### Message Format

Messages from Bitchat appear on MeshCore as:
```
<sender>: message content
```

Messages from MeshCore appear on Bitchat with the MeshCore sender's name in angle brackets:
```
<sender> message content
```

A message that does not fit into one Bitchat payload of `BITCHAT_MAX_PUBLIC_PAYLOAD`
bytes (default 99, see the baseline below for why) is sent as several messages, each with
the sender and a part counter:
```
<sender> [1/2] first part of a longer message
<sender> [2/2] and the rest of it
```
Parts are cut at whitespace where possible and never inside a UTF-8 character.

### Long Messages

MeshCore packets have a ~127-byte payload limit. Long Bitchat messages are automatically split into multiple parts with `[1/N]` indicators.

## Limitations

1. **Only #mesh channel**: Only the `#mesh` hashtag channel is bridged. DMs and other channels are ignored.

2. **Message size**: MeshCore packets are limited to ~127 bytes. Long messages are split into multiple parts.

3. **No end-to-end encryption bridge**: Bitchat's Noise protocol encryption and MeshCore's encryption are separate. Messages are decrypted/re-encrypted at the bridge.

4. **No file/image transfer**: Bitchat file transfers (images, etc.) are not supported on the mesh.

5. **Time synchronization**: The bridge takes its clock from Bitchat announces it hears directly (see "Timestamps" below). If its RTC is wrong and no Bitchat phone is nearby, its announces are rejected. Time needs to be accurate for Bitchat to work.

6. **NRF52 long message limitation**: NRF52 devices cannot decompress long/compressed Bitchat messages due to heap constraints. Messages >256 bytes (before compression) may fail on NRF52.

### Can I run this is a MeshCore repeater?

No, it can only work as companion node, not a repeater. Unfortunately these nodes don't have enough RAM to do multiple jobs (MeshCore repeater + bitchat repeater). So you have to pick either-or. Anyway, it is also because you want it placed differently. MeshCore repeater should be on a hill, or on a roof of a very high building. If you put it somewhere close to people, you are basically jamming the spectrum and not helping the network. 

On the other hand, the bitchat repeater should be where people are (Bluetooth needs to be really close)- in an office building, apartment, public square, close to ground. So you don't want to run both on the same device anyway.

## Debugging

### Serial output is off by default

The USB companion protocol shares the USB Serial port, so any diagnostic text
written there corrupts the companion stream. By default the Bitchat bridge
(including the boot trace, the nRF52 stack usage report every 10 s and the
hardfault dump) writes nothing to Serial. To enable all of it for a debugging
session, build with `-D BITCHAT_DEBUG_SERIAL=1` and open a plain serial
console. Do not use such a build with a companion app over USB.
`BITCHAT_DEBUG=1` and `BITCHAT_DEBUG_PACKETDUMP=1` (below) imply it.

To enable debug output, use one of these methods:

### Method 1: Environment variable (temporary)

```bash
# Build with debug output enabled
PLATFORMIO_BUILD_FLAGS="-D BITCHAT_DEBUG=1" pio run -e Heltec_WSL3_companion_radio_usb_bitchat
```

### Method 2: Edit platformio.ini (persistent)

Add `-D BITCHAT_DEBUG=1` to your environment's build_flags:

```ini
[env:Heltec_WSL3_companion_radio_usb_bitchat]
build_flags =
  ${Heltec_WSL3_companion_radio_usb.build_flags}
  ${bitchat_base.build_flags}
  -D BITCHAT_DEBUG=1   ; Enable debug output
```

This enables detailed logging of:
- Message parsing
- Relay confirmations
- Peer cache updates
- Fragment reassembly

## Technical Details

### BLE Service

- **Service UUID**: `F47B5E2D-4A9E-4C5A-9B3F-8E1D2C3A4B5C`
- **Characteristic**: Single read/write/notify characteristic
- **MTU**: 517 bytes (max BLE MTU for large messages)

### Channel Key

The `#mesh` channel uses a hashtag-derived key:
```
SHA256("#mesh")[0:16] = 5b664cde0b08b220612113db980650f3
```

This matches MeshCore's hashtag room key derivation.

### Protocol Support

- Message decompression (zlib via ESP32 ROM miniz, ESP32 only)
- Ed25519 message signing
- Peer announcement/discovery
- REQUEST_SYNC handling (sends cached messages with TTL 0, like the apps)
- Fragment reassembly for long messages

### Bitchat Protocol Baseline

The bridge is written against the **Bitchat v2.0 binary protocol**. `WHITEPAPER.md` last
changed on 2026-07-26 (#1480, #1485). Both were wording fixes about privacy, metadata and
the Nostr envelope format, with no wire-format change; the protocol content dates from
2026-07-06. Last cross-checked on **2026-11-12** against `permissionlesstech/bitchat-android`
**v2.2.0** (main at `14ab73c`, 2026-11-07) and `permissionlesstech/bitchat` (iOS) **v1.8.0**
(main at `04995f6`, 2026-10-25), including the open iOS PRs #1805, #1817, #1827 and #1846.

What the bridge implements:

| Area | Bridge behaviour |
|------|------------------|
| Packet version | Emits **v1** (14-byte header, 2-byte payload length); parses v1 and v2, skipping the v2 source route (`HAS_ROUTE`). Both apps still accept v1 (`decode()` rejects only versions other than 1 and 2). |
| Peer ID | `senderID` = first 8 bytes of `SHA-256(noise static public key)`, little-endian. **Mandatory** since bitchat-android #730 (2026-07-26): `AnnouncementIdentityValidator` rejects any ANNOUNCE whose senderID is not derived this way. |
| Noise static key | Curve25519 key derived from the node's Ed25519 identity (birational map). No Noise handshake is implemented, so bridge DMs are not possible. |
| Announce TLVs | `0x01` nickname, `0x02` noise pubkey, `0x03` Ed25519 signing key — all three required by the app. Optional `0x04` direct neighbours, `0x05` capabilities and `0x06` bridge geohash are not sent (the bridge supports none of those features; the decoders skip unknown TLVs). |
| Signing | Ed25519 over the **PKCS#7-padded** encoding with TTL forced to `SYNC_TTL_HOPS = 0` and the signature omitted — still what `toBinaryDataForSigning()` produces. |
| iOS signature check | iOS up to 1.8.0 verifies by re-encoding the received packet with `BinaryProtocol.encode()`, which zlib-compresses any payload of **100 bytes or more** with under 90% unique bytes (`compressionThresholdBytes`). The bridge signs what it sends, uncompressed, so iOS dropped every longer MeshCore message as wrongly signed (`BLEPublicMessageHandler`). Android verifies over the received bytes (bitchat-android #863); iOS PR #1805 does the same but is not merged. The bridge therefore splits MeshCore → Bitchat messages into payloads of at most `BITCHAT_MAX_PUBLIC_PAYLOAD` = **99 bytes** (build flag; raise it once iOS ships #1805). Its announces are at most 83 bytes. |
| Padding | Apps switched to *selective* padding (June 2026): only Noise frames are padded on the wire. Receive path is unaffected — decoding tolerates padded and unpadded input, and the signing representation is still padded. |
| TTL | Outgoing packets use TTL 7 (`MESSAGE_TTL_HOPS`). An announce is only registered as a direct-link neighbour when its TTL equals the app's max, so 8 would make the bridge look like a relayed peer. |
| Timestamps | Android drops announces more than **10 minutes** off wall clock; iOS drops any live BLE packet more than **2 minutes** off (`TransportConfig.bleMaxTimestampSkewMs`), and since 1.8.0 also refuses far-future packets in sync replies. iOS PR #1846 (open) keeps only the newest announce per peer by timestamp, so a clock that jumps back makes the bridge's announces look stale. The bridge uses, in order: time synced from Bitchat, the MeshCore RTC if it is not older than the firmware build date, then the build date. It syncs only from an ANNOUNCE heard directly (TTL 7), after duplicate checks, never from relayed packets or sync replies. A clock with no plausible time takes the first such announce; otherwise the announce must be signed by the key it carries, with the sender ID derived from its noise key. Differences up to 60 s are ignored, a later time is taken from one verified peer, an earlier one only when two different verified peers agree within 10 minutes. The MeshCore RTC is set only when it was implausible (1970 or a reset seed) or was set by the bridge. |
| Sender rate (spam filter) | bitchat-android 2.1.0+ hides public messages per sender peer ID beyond a token-bucket budget: burst of 6 then 1 per 10 s, 1200 characters/min, 2 links/min; also near-copies of the sender's own recent text. The bridge is one peer for every MeshCore sender, so MeshCore → Bitchat messages are queued and released under the same buckets with a margin (1 per 11 s, 18 chars/s, 1 link per 33 s). Each part of a split message is one queue entry and one message in the budget; the queue holds 32 parts on ESP32 and 12 on NRF52 (at the default 99-byte limit). The packet timestamp is set at release. If the queue cannot take all parts of a message, the whole (newest) message is dropped. |
| Payload caps | Both apps now cap decoded payloads per type (`PacketPayloadLimits`: MESSAGE 128 KiB, ANNOUNCE 4 KiB, FRAGMENT 1280 B, REQUEST_SYNC 8 KiB). The bridge sends uncompressed, unfragmented messages of at most 99 bytes and small announces, far below the caps. |
| REQUEST_SYNC | Requests addressed to another peer are ignored. Cached packets go back with TTL 0, as the apps' `GossipSyncManager` does, so they are not relayed beyond the requester. The apps send them only to the requesting peer; the bridge's BLE layer has no per-connection send and notifies all connected clients, normally just the requester. |
| Ignored types | `FILE_TRANSFER` (0x22), `VOICE_FRAME` (0x29, live push-to-talk), `0x2A`/`0x2B` (reserved by iOS for courier acks), `ANNOUNCE_V2` (0x2C, not emitted yet) and `VIDEO_FRAME` (0x2D, live video, iOS PR #1827) are never bridged to LoRa. Unknown types are ignored. |

Known gaps against current Bitchat builds:

- MeshCore nodes do not appear as individual Bitchat peers. An announce must be signed by
  the key it announces, which the bridge cannot do on another node's behalf, so proxying
  identities is impossible by design — MeshCore senders are named inside the message text
  instead. Advertising the bridge itself (capability bit + `bridgeGeohash` TLV `0x06`)
  would be the supported way to expose it as a gateway.
- No Noise session support: private messages to/from the bridge peer are not possible,
  only the `#mesh` channel.
- Capability bit `PRIVATE_MEDIA` is not advertised (private media transfer unsupported).
  bitchat-android 2.0.x assigned the capability bits (`BRIDGE` = bit 7, among others); the
  bridge still sends no capabilities TLV.
- Rate pacing only covers the rate buckets. The app's repeat checks (the same sender
  posting near-identical text more than once in 10 minutes) can still hide a MeshCore message
  that repeats an earlier one, for example the same text from two MeshCore users.
- iOS PR #1817 (open) ports the Android spam filter to iOS: the same buckets, plus rules
  for repeated characters (a message of 32+ characters that is mostly one repeated pattern)
  and for text copied from other users (the third near-copy of a 16+ character message
  posted by other senders within 10 minutes is hidden). The bridge's pacing already matches
  the buckets; the content rules can hide MeshCore messages the same way as on Android.

Found in the check of 2026-11-12 and fixed in the bridge: longer MeshCore messages were
dropped by iOS (signature preimage, above); the clock was synced from any received packet,
including relayed and synced ones, before the duplicate check; REQUEST_SYNC replies went out
with TTL 7 and requests for other peers were answered. Otherwise the apps changed nothing the
bridge depends on since 2026-10-28: no new message types are emitted (0x2C and 0x2D are only
in iOS code or open PRs), and the packet format, signing and announce TLVs are unchanged.

Changes in the apps between 2026-08-10 and 2026-10-28 that need nothing from the bridge:
signature preimages now reuse the received wire payload (`WirePayload`, so compressed packets
are no longer re-compressed for verification), duplicate detection hashes type, sender,
timestamp and full payload, fragment reassembly rejects conflicting indexes, and capability
bits got assignments. Packet format, message type values, announce TLVs, the peer ID
rule, the BLE service/characteristic UUIDs and the Noise handshake are unchanged.

## Release Builds

To build all bitchat targets for a release:

```bash
export FIRMWARE_VERSION=v0.8-bitchat

# Build individual targets
sh build.sh build-firmware Heltec_WSL3_companion_radio_usb_bitchat

# Build all Bitchat bridge targets
sh build.sh build-bitchat-firmwares
```

Output files:
- ESP32: `.bin` + `-merged.bin` files
- NRF52: `.uf2` + `.zip` files
