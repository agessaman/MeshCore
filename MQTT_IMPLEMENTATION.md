# MQTT Bridge Implementation for MeshCore

This document describes the MQTT bridge implementation that allows MeshCore repeaters to uplink packet data to multiple MQTT brokers.

## Quick Start Guide

### Essential Commands to Get MQTT Observer Running

**1. Flash the observer firmware to your device**

Use one of the observer build targets (e.g., `heltec_v4_repeater_observer_mqtt`). After flashing, connect to the device console via serial (115200 baud) or repeater login.

**2. Configure radio settings**

If this is a fresh flash or full erase, configure your radio parameters first. These must match other nodes in your mesh:

```bash
set radio 910.525,62.5,7,5
set tx 22
```

Format: `set radio <freq_MHz>,<bw_kHz>,<sf>,<cr>`

**3. Configure device identity**

```bash
set name MyObserver
set mqtt.iata SEA
```

If migrating from an existing node (e.g., a Raspberry Pi gateway), restore the private key to keep the same identity:
```bash
set prv.key <your_64_hex_char_private_key>
```

**4. Configure WiFi credentials** (value is the rest of the line; do not use quotes — see [WiFi Commands](#wifi-commands))
```bash
set wifi.ssid YourWiFiNetwork
set wifi.pwd YourWiFiPassword
```

**5. (Optional) Disable packet repeating**

If this observer is receive-only (e.g., using a PCB antenna in a location where repeating would be harmful), disable forwarding:
```bash
set repeat off
```

**6. Reboot to connect**
```bash
reboot
```

**7. Verify configuration**
```bash
get wifi.ssid
get bridge.enabled
get mqtt.rx
get mqtt.tx
get mqtt.origin
get mqtt.iata
get mqtt1.preset
get mqtt2.preset
get mqtt3.preset
get mqtt.status
```

**That's it!** The device will now:
- Connect to WiFi automatically
- Start uplinking mesh packets to configured MQTT brokers
- By default, publish to Let's Mesh Analyzer US (slot 1) and EU (slot 2)
- Use device name as MQTT origin (set automatically)

---

## Overview

The MQTT bridge implementation provides:
- Up to 6 MQTT connection slots with built-in presets
- Built-in presets for many community brokers (see the [preset table](#slot-based-preset-system) for the full list)
- Custom broker support with username/password authentication
- JWT (Ed25519 device signing) authentication for most preset brokers; TennMesh uses a fixed username/password (plain MQTT)
- WSS (WebSocket Secure), direct MQTT/TLS, and plain MQTT (TennMesh) transport
- Automatic reconnection with exponential backoff
- JSON message formatting for status, packets, and raw data
- Packet queuing during connection issues
- Automatic migration from old configuration format

## Architecture

### Slot-Based Preset System

The MQTT bridge uses a slot-based architecture with up to 6 concurrent connections. Each slot can be configured with a built-in preset or custom broker settings.

**Built-in Presets:**

| Preset | Server | Auth | Transport |
|--------|--------|------|-----------|
| `analyzer-us` | mqtt-us-v1.letsmesh.net:443 | JWT (Ed25519) | WSS |
| `analyzer-eu` | mqtt-eu-v1.letsmesh.net:443 | JWT (Ed25519) | WSS |
| `nz-analyzer` | meshcore-mqtt-1.baird.io:443 | JWT (Ed25519) | WSS |
| `meshmapper` | mqtt.meshmapper.net:443 | JWT (Ed25519) | WSS |
| `meshrank` | meshrank.net:8883 | None (token in topic) | MQTT over TLS |
| `waev` | mqtt.waev.app:443 | JWT (Ed25519) | WSS |
| `meshomatic` | us-east.meshomatic.net:443 | JWT (Ed25519) | WSS |
| `cascadiamesh` | mqtt-v1.cascadiamesh.org:443 | JWT (Ed25519) | WSS |
| `tennmesh` | mqtt.tennmesh.com:1883 | Username/password (fixed in firmware) | Plain MQTT |
| `nashmesh` | mqtt://mqtt.nashme.sh:1883 | Username/password (fixed in firmware) | Plain MQTT |
| `ctmesh` | mqtt.ctmesh.org:1883 | Username/password (fixed in firmware) | Plain MQTT |
| `chimesh` | wss://mqtt.chimesh.org:443 | JWT (Ed25519) | WSS |
| `meshat.se` | meshcore-mqtt.meshat.se:443 | JWT (Ed25519) | WSS |
| `eastidahomesh` | wss://broker.eastidahomesh.net:443 | None | WSS |
| `coloradomesh` | wss://mqtt.meshcore.coloradomesh.org:1883 | JWT (Ed25519) | WSS |
| `dutchmeshcore-1` | collector1.dutchmeshcore.nl:443 | JWT (Ed25519) | WSS |
| `dutchmeshcore-2` | collector2.dutchmeshcore.nl:443 | JWT (Ed25519) | WSS |
| `meshcore-ca-1` | mqtt1.meshcore.ca:443 | JWT (Ed25519) | WSS |
| `meshcore-ca-2` | mqtt2.meshcore.ca:443 | JWT (Ed25519) | WSS |
| `bostonmesh` | mqttmc01.bostonme.sh:443 | JWT (Ed25519) | WSS |
| `flmesh.us` | mcmqtt.jntconnections.com:443 | JWT (Ed25519) | WSS |
| `inwmesh` | scope.inwmesh.org:8883 | Username/password (per slot via `mqttN.username` / `mqttN.password`) | MQTT over TLS |
| `custom` | User-configured | Username/Password | MQTT or WSS |
| `none` | (disabled) | — | — |

**Default Configuration:**
- Slot 1: `analyzer-us`
- Slot 2: `analyzer-eu`
- Slots 3-6: `none`

**Memory Limits:**
- With PSRAM: All slots can be active simultaneously
- Without PSRAM: Maximum 2 active TLS/WSS slots (each WSS/TLS connection requires ~40KB internal heap)
- If more slots are configured than the device supports, excess slots show as `(inactive)` in `get mqtt.status`
- Slot configurations are preserved in preferences — moving firmware to a PSRAM device activates all slots

### Files

#### Core Implementation
- `src/helpers/bridges/MQTTBridge.h` - MQTT bridge class definition
- `src/helpers/bridges/MQTTBridge.cpp` - MQTT bridge implementation
- `src/helpers/MQTTPresets.h` - Preset definitions, CA certificates, and lookup functions
- `src/helpers/MQTTMessageBuilder.h` - JSON message formatting utilities
- `src/helpers/MQTTMessageBuilder.cpp` - JSON message formatting implementation
- `src/helpers/JWTHelper.h` - JWT token generation for Ed25519-based authentication

#### Integration
- Updated `examples/simple_repeater/MyMesh.h` - Added MQTT bridge support
- Updated `examples/simple_repeater/MyMesh.cpp` - Added MQTT bridge integration and raw radio data capture
- Updated `src/helpers/CommonCLI.h` - MQTT slot preferences, WiFi, and timezone fields
- Updated `src/helpers/CommonCLI.cpp` - MQTT slot CLI commands, migration logic

## Build Configuration

To build the MQTT bridge firmware:

```bash
# Heltec V3
pio run -e Heltec_v3_repeater_observer_mqtt

# Heltec V4
pio run -e heltec_v4_repeater_observer_mqtt

# Station G2
pio run -e Station_G2_repeater_observer_mqtt

# LilyGo T-LoRa V2.1-1.6 (TTGO LoRa32 V1.0)
pio run -e LilyGo_TLora_V2_1_1_6_repeater_observer_mqtt
pio run -e LilyGo_TLora_V2_1_1_6_room_server_observer_mqtt
```

**TLora naming:** The env prefix `LilyGo_TLora_V2_1_1_6` is LilyGo’s **T-LoRa V2.1–1.6** board (SX1276); PlatformIO selects **`ttgo-lora32-v1`** (TTGO LoRa32 V1.0). **MQTT observer** envs extend a slim base **without** `sensor_base` so the image fits `min_spiffs`; **all other** `LilyGo_TLora_V2_1_1_6_*` targets still use optional I2C environmental sensors as before. The **`lilygo_tlora_c6`** variant is separate hardware (ESP32-C6).

**T-LoRa V2.1–1.6 MQTT observer — one WSS broker:** This hardware is **classic ESP32 without PSRAM**. Each WSS preset uses a full TLS stack and large contiguous heap allocations; **two active broker presets at once** typically fails the second connection (`mbedtls_ssl_setup` / `esp-tls` `0x8017`, low `IntMax` in `memory`). **Treat these observer builds as supporting one active cloud preset:** configure the broker you need in `mqtt1` or `mqtt2`, and set the other slot to `none` (e.g. `set mqtt2.preset none`). Use PSRAM-capable boards if you need multiple simultaneous MQTT uplinks.

### Partition Table Changes — Merged Firmware Required

Some MQTT observer builds use a non-default partition table to accommodate the larger firmware size (MQTT libraries, TLS, cert bundle, etc.). **When a board's partition table changes, you must flash the merged firmware (`*-merged.bin`) the first time** so the new partition layout and bootloader are written together. After that initial flash, standard OTA or non-merged updates will work normally.

| Environment | Partition Table | Flash Size | App Slot Size | Notes |
|-------------|----------------|------------|---------------|-------|
| `LilyGo_T3S3_sx1262_repeater_observer_mqtt` | `min_spiffs.csv` | 4 MB | 1.875 MB | Changed from default (1.25 MB) |
| `LilyGo_T3S3_sx1262_room_server_observer_mqtt` | `min_spiffs.csv` | 4 MB | 1.875 MB | Changed from default (1.25 MB) |
| `LilyGo_TLora_V2_1_1_6_repeater_observer_mqtt` | `min_spiffs.csv` | 4 MB | 1.875 MB | TTGO LoRa32 V1.0; observer omits `sensor_base`. **One active WSS broker** recommended (no PSRAM; dual TLS usually fails on the second slot). |
| `LilyGo_TLora_V2_1_1_6_room_server_observer_mqtt` | `min_spiffs.csv` | 4 MB | 1.875 MB | same |
| `Station_G2_repeater_observer_mqtt` | `default_16MB.csv` | 16 MB | 6.25 MB | 16 MB flash board |
| `Station_G2_room_server_observer_mqtt` | `default_16MB.csv` | 16 MB | 6.25 MB | 16 MB flash board |
| `LilyGo_TBeam_1W_repeater_observer_mqtt` | `default_16MB.csv` | 16 MB | 6.25 MB | Set in `boards/t_beam_1w.json`; required vs implicit `default.csv` |
| `LilyGo_TBeam_1W_room_server_observer_mqtt` | `default_16MB.csv` | 16 MB | 6.25 MB | same |

**NVS / settings when the partition layout changes**

Flashing a **full merged image** (`*-merged.bin` at offset `0x0`) writes a new bootloader **and** partition table. If that layout **differs** from what is already on the device, **NVS is typically wiped or invalidated** — expect to lose stored configuration (admin preferences, WiFi, MQTT slots, name, etc.) and reconfigure from scratch.

- **`LilyGo_TLora_V2_1_1_6_*_observer_mqtt`:** These use the **same** `min_spiffs.csv` layout as other MeshCore TLora builds, so moving between repeater / room server / MQTT observer does **not** require a different partition table for normal upgrades. **If you previously installed an older TLora MQTT observer that used `huge_app.csv`,** flashing this firmware switches back to `min_spiffs` — treat that as a **partition layout change** (merged flash; NVS may be reset). **If you install MeshCore on a device that used a non-MeshCore partition map,** the first merged flash can still **wipe** settings.
- **`Station_G2_*_observer_mqtt`** and **`LilyGo_TBeam_1W_*_observer_mqtt`**: These use `default_16MB.csv` to accomodate the larger size of the MQTT observer firmware. Installing MQTT observer firmware on these devices requires a **merged** flash the first time. The same applies if you move **from** firmware that was built with a **different** partition table—the first merged flash that installs this layout will **wipe** stored settings. 

**How to flash the merged firmware:**

You can flash the merged firmware using either the web flasher or the command line:

- **Web flasher (recommended):** Use the [MeshCore Web Flasher](https://meshcore.io/flasher) to flash the `*-merged.bin` file directly from your browser — no tools to install.
- **Command line:**
  ```bash
  # Build the merged binary
  pio run -t mergebin -e LilyGo_T3S3_sx1262_repeater_observer_mqtt

  # Flash at offset 0x0 (overwrites bootloader + partition table)
  esptool.py write_flash 0x0 .pio/build/LilyGo_T3S3_sx1262_repeater_observer_mqtt/firmware-merged.bin
  ```

> **Note:** If the **partition layout is unchanged** (e.g. updating the MQTT observer build in place), device configuration in NVS is usually retained; Bluetooth pairings may still be cleared on some upgrade paths. If the **partition table is new to the device**, see **NVS / settings when the partition layout changes** above — stored settings are typically lost. After the first merged flash **for a given layout**, subsequent updates on that board can use OTA or the standard non-merged binary when applicable.

### Build Flags
- `WITH_MQTT_BRIDGE=1` - Enable MQTT bridge (required)
- `WITH_SNMP=1` - Enable SNMP agent (optional, see [MQTT_SNMP.md](MQTT_SNMP.md))
- `MQTT_DEBUG=1` - Enable debug logging (optional)
- `MQTT_WIFI_TX_POWER` - WiFi TX power level (default: `WIFI_POWER_11dBm`)
- ~~`MQTT_WIFI_POWER_SAVE_DEFAULT`~~ - Removed; all builds now default to `none` (no power save)

#### Compile-time fresh-install defaults (`src/helpers/MQTTDefaults.h`)

Optional PlatformIO `build_flags` override defaults written when `/mqtt_prefs` is first created. They do **not** change existing saved prefs on upgrade or reflash (unless `/mqtt_prefs` is erased).

| Macro | Default | Notes |
|-------|---------|-------|
| `MQTT_DEFAULT_SLOT1_PRESET` … `MQTT_DEFAULT_SLOT6_PRESET` | slots 1–2: `analyzer-us` / `analyzer-eu`; slots 3–6: `none` | Must be a built-in preset name, `none`, or `custom` |
| `MQTT_DEFAULT_IATA` | (empty) | e.g. `'"YYZ"'` |
| `MQTT_DEFAULT_TIMEZONE` | (empty) | e.g. `'"America/Toronto"'` |
| `MQTT_DEFAULT_TIMEZONE_OFFSET` | `0` | Fallback hours when TZ string is empty |

Example community build:

```ini
build_flags =
  -D MQTT_DEFAULT_SLOT1_PRESET='"meshcore-ca-1"'
  -D MQTT_DEFAULT_SLOT2_PRESET='"meshcore-ca-2"'
  -D MQTT_DEFAULT_IATA='"YYZ"'
  -D MQTT_DEFAULT_TIMEZONE='"America/Toronto"'
  -D MQTT_DEFAULT_TIMEZONE_OFFSET=-5
```

WiFi SSID/password are not compile-time configurable (operators set them per device via CLI).

Legacy `get mqtt.analyzer_us` / `set mqtt.analyzer_us` still refer to the preset name `analyzer-us`, not “whatever slot 1 default is”.

## Default Configuration

The MQTT bridge comes with the following defaults for fresh installs (unless overridden by the macros above):
- **Origin**: Device name (set automatically from `set name`)
- **IATA**: (blank — must be configured for MeshCore-style topic presets such as Analyzer and TennMesh, unless `MQTT_DEFAULT_IATA` is set at build time)
- **Status Messages**: Enabled
- **Packet Messages**: Enabled
- **Raw Messages**: Disabled
- **RX Packets**: Enabled (uplink received packets)
- **TX Packets**: `advert` by default (uplink this node's own adverts; set to `on` for all TX or `off` to disable)
- **Status Interval**: 5 minutes (300000 ms)
- **Slot 1**: `analyzer-us`
- **Slot 2**: `analyzer-eu`
- **Slots 3-6**: `none` (disabled)
- **WiFi SSID**: (blank — must be configured)
- **WiFi Password**: (blank — optional for open networks)
- **WiFi Power Save**: `none` (no power save)
- **Timezone**: (blank — uses UTC until configured, unless `MQTT_DEFAULT_TIMEZONE` is set at build time)
- **Timezone Offset**: 0 (fallback, no offset, unless `MQTT_DEFAULT_TIMEZONE_OFFSET` is set)
- **Repeat (forwarding)**: On (set `repeat off` for receive-only observers)

## CLI Commands

### MQTT Slot Commands

Each slot (1-6) supports the following commands:

#### Get Commands
- `get mqtt1.preset` - Get slot 1 preset name
- `get mqtt2.preset` - Get slot 2 preset name
- `get mqttN.preset` - Get slot N preset name (N = 1-6)
- `get mqttN.server` - Get custom server hostname for slot N
- `get mqttN.port` - Get custom server port for slot N
- `get mqttN.username` - Get custom username for slot N
- `get mqttN.password` - Get custom password for slot N
- `get mqttN.token` - Get per-slot token (e.g., MeshRank account token)
- `get mqttN.topic` - Get custom topic template for slot N
- `get mqttN.audience` - Get JWT audience for slot N (custom slots only)

#### Set Commands
- `set mqttN.preset <name>` - Set slot N to a built-in preset. Use any `name` from the [preset table](#slot-based-preset-system) (run `get mqtt.presets` on-device for the full list). Most presets need no further configuration; the exceptions are:
  - `meshrank` - requires a per-slot token (`set mqttN.token <token>`)
  - `inwmesh` - requires per-slot credentials (`set mqttN.username` / `set mqttN.password`)
- `set mqttN.preset custom` - Set slot N to custom broker (configure server/port/username/password)
- `set mqttN.preset none` - Disable slot N
- `set mqttN.server <hostname>` - Set custom server hostname for slot N
- `set mqttN.port <port>` - Set custom server port for slot N (1-65535)
- `set mqttN.username <username>` - Set username for slot N (`custom` preset, or presets like `inwmesh` that require per-device credentials)
- `set mqttN.password <password>` - Set password for slot N (`custom` preset, or presets like `inwmesh` that require per-device credentials)
- `set mqttN.token <token>` - Set per-slot token (required for MeshRank preset)
- `set mqttN.topic <template>` - Set custom topic template (custom preset only, see below)
- `set mqttN.audience <audience>` - Set JWT audience for custom slot (enables Ed25519 JWT auth)
- `set mqttN.audience` - Clear JWT audience (reverts to username/password auth)

**Note:** Custom server/port settings only apply when the slot's preset is `custom`. Username/password also apply to built-in presets that use per-slot credentials (e.g. `inwmesh`); other userpass presets (`tennmesh`, `nashmesh`, `ctmesh`) ship fixed credentials in firmware.

#### Example: Configure MeshRank on Slot 3
```bash
set mqtt3.preset meshrank
set mqtt3.token FE1B34242C5938C39225310081FD6718
```

The token is generated on the MeshRank website and is tied to your account. MeshRank only receives packet data (no status or raw messages).

#### Example: Configure MeshMapper on Slot 3
```bash
set mqtt3.preset meshmapper
```

#### Example: Configure Custom Broker on Slot 3
```bash
set mqtt3.preset custom
set mqtt3.server your-broker.example.com
set mqtt3.port 1883
set mqtt3.username your-username
set mqtt3.password your-password
```

#### Example: Custom Broker with JWT Authentication (Ed25519)

For community brokers that support the MeshCore JWT auth protocol (same as the built-in presets), set the `audience` field to enable Ed25519-signed JWT authentication:
```bash
set mqtt3.preset custom
set mqtt3.server wss://my-broker.example.com:443/mqtt
set mqtt3.port 443
set mqtt3.audience my-broker.example.com
```

When `audience` is set, the device will:
- Connect with username `v1_{PUBLIC_KEY}` and an Ed25519-signed JWT as the password
- Automatically renew tokens before expiry (default 24h lifetime)
- Include owner public key and email in the JWT payload (if configured via `set mqtt.owner` / `set mqtt.email`)

To revert a slot back to username/password auth, clear the audience:
```bash
set mqtt3.audience
```

#### Example: Custom Broker with Custom Topic Template
```bash
set mqtt3.preset custom
set mqtt3.server my-broker.local
set mqtt3.port 1883
set mqtt3.topic mynetwork/{device}/{type}
```

### Custom Topic Templates

When a slot's preset is `custom`, you can define a custom topic template using placeholders:

| Placeholder | Value | Example |
|-------------|-------|---------|
| `{iata}` | IATA airport code | `SEA` |
| `{device}` | Device public key (64 hex chars) | `CC5D3CFD...` |
| `{token}` | Per-slot token from `mqttN.token` | `FE1B3424...` |
| `{type}` | Message type | `status`, `packets`, or `raw` |

If no custom topic is set, custom slots default to: `meshcore/{iata}/{device}/{type}`

**Note:** Topic templates only apply to `custom` preset slots. Built-in presets (analyzer-us, analyzer-eu, meshmapper, meshrank, eastidahomesh, coloradomesh, tennmesh, etc.) always use their hardcoded topic format.

### MQTT Shared Commands

These settings apply across all MQTT slots:

#### Get Commands
- `get mqtt.origin` - Get device origin name
- `get mqtt.iata` - Get IATA code
- `get mqtt.presets` - List available MQTT presets (paginated, comma-separated)
- `get mqtt.presets <start>` - Continue list from index shown in `... next:<idx>`
- `get mqtt.status` - Get MQTT status summary (connection info per slot)
- `get mqtt.packets` - Get packet message setting (on/off)
- `get mqtt.raw` - Get raw message setting (on/off)
- `get mqtt.rx` - Get RX packet uplinking setting (on/off)
- `get mqtt.tx` - Get TX packet uplinking setting (on/off/advert)
- `get mqtt.interval` - Get status publish interval
- `get mqtt.owner` - Get owner public key (serial console only)
- `get mqtt.email` - Get owner email address (serial console only)

#### Set Commands
- `set mqtt.origin <name>` - Set device origin name
- `set mqtt.iata <code>` - Set IATA code (auto-uppercased)
- `set mqtt.status on|off` - Enable/disable status messages
- `set mqtt.packets on|off` - Enable/disable packet messages
- `set mqtt.raw on|off` - Enable/disable raw messages
- `set mqtt.rx on|off` - Enable/disable RX (received) packet uplinking
- `set mqtt.tx on|off|advert` - Set TX packet uplinking mode:
  - `on` - Uplink all transmitted packets
  - `advert` - Uplink only this node's own advert packets (self-originated)
  - `off` - Disable TX packet uplinking
- `set mqtt.interval <minutes>` - Set status publish interval (1-60 minutes)
- `set mqtt.owner <64-hex-char-public-key>` - Set owner public key
- `set mqtt.email <email>` - Set owner email address

### WiFi Commands

#### Get Commands
- `get wifi.ssid` - Get WiFi SSID
- `get wifi.pwd` - Get WiFi password
- `get wifi.status` - Get WiFi connection status, IP, RSSI, and uptime
- `get wifi.powersave` - Get WiFi power save mode (none/min/max)

#### Set Commands
- `set wifi.ssid <ssid>` - Set WiFi SSID
- `set wifi.pwd <password>` - Set WiFi password
- `set wifi.powersave none|min|max` - Set WiFi power save mode

> **Note:** The value is everything after the first space (spaces in SSID/password are fine). Do not wrap in quotes — they are stored literally. Max length: 31 characters (SSID), 63 (password). For open networks, use `set wifi.pwd ` with nothing after the space.
  - `none` - No power saving (best performance, highest power consumption)
  - `min` - Minimum power saving (balanced performance and power)
  - `max` - Maximum power saving (lowest power consumption, may affect performance)

### Timezone Commands

#### Get Commands
- `get timezone` - Get timezone string (e.g., "America/Los_Angeles")
- `get timezone.offset` - Get timezone offset in hours (-12 to +14)

#### Set Commands
- `set timezone <string>` - Set timezone string (IANA format or abbreviation)
- `set timezone.offset <offset>` - Set timezone offset in hours (-12 to +14)

#### Supported Timezone Formats
- **IANA strings**: `America/Los_Angeles`, `Europe/London`, `Asia/Tokyo`, etc.
- **Common abbreviations**: `PDT`, `PST`, `MDT`, `MST`, `CDT`, `CST`, `EDT`, `EST`, `BST`, `GMT`, `CEST`, `CET`
- **UTC offsets**: `UTC-8`, `UTC+5`, `+5`, `-8`, etc.

### Device & Radio Commands

These are standard MeshCore commands, not MQTT-specific, but important for observer setup:

#### Get Commands
- `get name` - Get device name
- `get repeat` - Get repeat (forwarding) status (on/off)
- `get freq` - Get radio frequency
- `get public.key` - Get device public key (for migration)

#### Set Commands
- `set name <name>` - Set device name (also sets MQTT origin)
- `set repeat on|off` - Enable/disable packet forwarding (use `off` for receive-only observers)
- `set prv.key <64-hex-char-key>` - Restore private key (for migrating identity from another device)
- `set tx <dBm>` - Set transmit power

### Bridge Commands

#### Get Commands
- `get bridge.source` - Get packet source (rx/tx)
- `get bridge.enabled` - Get bridge enabled status (on/off)

#### Set Commands
- `set bridge.source rx|tx` - Set packet source (rx for received, tx for transmitted)
- `set bridge.enabled on|off` - Enable/disable bridge

### SNMP Commands

#### Get Commands
- `get snmp` - Get SNMP agent status (on/off)
- `get snmp.community` - Get SNMP community string

#### Set Commands
- `set snmp on|off` - Enable/disable SNMP agent (restart required)
- `set snmp.community <string>` - Set SNMP community string (restart required, default: `public`)

See [MQTT_SNMP.md](MQTT_SNMP.md) for full SNMP documentation.

## Command Architecture

The CLI commands are organized into two levels:

### Bridge Commands (`bridge.*`)
**Low-level bridge control** - These settings apply to all bridge types (MQTT, RS232, ESP-NOW, etc.):
- `bridge.enabled` - Master switch for the entire bridge system
- `bridge.source` - Controls which packet events to capture for non-MQTT bridges (RS232, ESP-NOW). For MQTT, use `mqtt.rx` and `mqtt.tx` instead.

### Bridge-Specific Commands (`mqtt.*`, `mqttN.*`, `wifi.*`, `timezone.*`)
**Implementation-specific settings** - These only apply to the MQTT bridge:
- `mqtt.rx` / `mqtt.tx` - Independent per-direction packet uplinking control
- `mqttN.*` - Per-slot MQTT broker configuration (N = 1-6)
- `mqtt.*` - Shared MQTT settings (message types, origin, IATA, etc.)
- `wifi.*` - WiFi connection settings for MQTT connectivity
- `timezone.*` - Timezone configuration for accurate timestamps

## MQTT Topics

The bridge publishes to three main topics with the following structure:

### Status Topic: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/status`
Device connection status and metadata (retained messages).

### Packets Topic: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/packets`
Full packet data with RF characteristics and metadata.

### Raw Topic: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/raw`
Minimal raw packet data for map integration.

**Note**: `{DEVICE_PUBLIC_KEY}` is the device's public key in hexadecimal format (64 characters).

## JSON Message Formats

### Status Message
```json
{
  "status": "online|offline",
  "timestamp": "2024-01-01T12:00:00.000000+00:00",
  "origin": "Device Name",
  "origin_id": "DEVICE_PUBLIC_KEY",
  "model": "device_model",
  "firmware_version": "firmware_version",
  "radio": "radio_info",
  "client_version": "meshcore/{firmware_version}",
  "repeat": "on|off",
  "stats": {
    "battery_mv": 4100,
    "uptime_secs": 3600,
    "packets_sent": 42,
    "packets_received": 128,
    "errors": 0,
    "queue_len": 0,
    "noise_floor": -110,
    "tx_air_secs": 12,
    "rx_air_secs": 340,
    "recv_errors": 2,
    "internal_heap": 102400
  }
}
```

**Notes:**
- Timestamps are always emitted in UTC with an explicit `+00:00` offset.
- The `stats` object is only included when at least one stat value is available; individual fields are omitted when their value is unavailable.
- `packets_sent` / `packets_received` are cumulative totals since boot (flood + direct), sourced from the dispatcher counters.

### Packet Message
```json
{
  "origin": "MeshCore-HOWL",
  "origin_id": "A1B2C3D4E5F67890...",
  "timestamp": "2024-01-01T12:00:00.000000+00:00",
  "type": "PACKET",
  "direction": "rx|tx",
  "time": "12:00:00",
  "date": "01/01/2024",
  "len": "45",
  "packet_type": "4",
  "route": "F|D|T|U",
  "payload_len": "32",
  "raw": "F5930103807E5F1E...",
  "SNR": "12.5",
  "RSSI": "-65",
  "score": "234",
  "hash": "A1B2C3D4E5F67890",
  "path": ["aa", "bb", "cc"]
}
```

**Notes:**
- All numeric fields (`len`, `packet_type`, `payload_len`, `SNR`, `RSSI`, `score`) are formatted as JSON strings.
- `time` and `date` are always UTC (`HH:MM:SS` and `DD/MM/YYYY`); `timestamp` is UTC with an explicit `+00:00` offset.
- `SNR`, `RSSI`, and `score` are only present for RX packets (received from radio). TX packets omit these fields since the packet originates from this node.
- `score` is the firmware's rebroadcast score for the received packet (the same value used to compute flood-rebroadcast delay), scaled ×1000 to match the integer printed in the serial RX log — e.g. a score of `0.234` is emitted as `"234"` (range `0`–`1000`). It is recomputed at publish time from the packet's SNR and length via the radio's `packetScore()`, so it matches what the firmware used on receive. Omitted when unavailable (e.g. the non-PSRAM reconstruction-less fallback path).
- `path` is only present for direct-route packets that carry path data. It is a JSON array of lowercase hex hop tokens, one element per hop — e.g. `["aa","bb","cc"]` for single-byte hashes, or `["aaaa","bbbb"]` for multi-byte hashes. This matches the `path` representation emitted by [meshcore-packet-capture](https://github.com/agessaman/meshcore-packet-capture).

### Raw Message
```json
{
  "origin": "MeshCore-HOWL",
  "origin_id": "A1B2C3D4E5F67890...",
  "timestamp": "2024-01-01T12:00:00.000000+00:00",
  "type": "RAW",
  "data": "F5930103807E5F1E..."
}
```

## Key Features

### Slot-Based Preset System
- Up to 6 concurrent MQTT connections (with PSRAM), 2 without PSRAM
- Built-in presets for many community brokers (see the [preset table](#slot-based-preset-system))
- Custom broker support with username/password auth and custom topic templates
- JWT (Ed25519) for most preset brokers; MeshRank uses token-in-topic; TennMesh uses fixed username/password over plain MQTT
- WSS (WebSocket Secure), direct MQTT over TLS, and plain MQTT (TennMesh)
- Automatic reconnection with exponential backoff per slot
- Circuit breaker pattern with periodic probes for recovery from prolonged outages
- JWT token buffers only allocated for JWT-auth slots (memory efficient)
- Deferred construction: MQTTBridge is heap-allocated in `begin()` to avoid ESP32 static init crashes

### Raw Radio Data Capture
- Captures actual raw radio transmission data (including radio headers)
- Uses proper MeshCore packet hashing (SHA256-based)
- Provides accurate SNR/RSSI values from actual radio reception (RX packets only)
- Independent RX and TX packet uplinking — both can be active simultaneously
- TX advert mode: selectively uplink only this node's own advert packets

### Timezone Support
- Full timezone support with automatic DST handling
- Supports IANA timezone strings, common abbreviations, and UTC offsets
- Separates local time (for timestamps) and UTC time (for time/date fields)
- Uses JChristensen/Timezone library for accurate timezone conversions

### WiFi Configuration
- Runtime WiFi credential management via CLI
- Persistent storage across reboots
- Automatic reconnection with exponential backoff

### NTP Time Synchronization
- Automatic time synchronization with NTP servers
- Periodic time updates (every hour)
- Proper UTC system time handling

### Authentication
The auth mode is fixed per preset (see the [preset table](#slot-based-preset-system)). Three modes are used:
- **JWT Authentication**: Ed25519-signed tokens for brokers that expect JWT (most WSS presets). For `custom` slots, JWT is used when `audience` is set.
- **Username/Password**: Some presets ship fixed credentials embedded in firmware (`tennmesh`, `nashmesh`, `ctmesh` — plain MQTT, no TLS); others (`inwmesh`, `custom`) take per-slot credentials via `mqttN.username` / `mqttN.password`.
- **None**: `meshrank` (account token carried in the topic) and `eastidahomesh` connect without broker auth.
- **Username Format** (JWT): `v1_{UPPERCASE_PUBLIC_KEY}`
- **Automatic Token Renewal**: Tokens are renewed before expiration

## Migration from Old Configuration

When upgrading from a firmware version that used the old MQTT configuration format (`mqtt.analyzer.us`, `mqtt.analyzer.eu`, `mqtt.server`, `mqtt.port`, `mqtt.username`, `mqtt.password`), the device automatically migrates settings:

- `mqtt.analyzer.us = on` → Slot 1 preset: `analyzer-us`
- `mqtt.analyzer.eu = on` → Slot 2 preset: `analyzer-eu`
- Custom server configured → Slot 3 preset: `custom` with host/port/username/password preserved
- All other settings (origin, IATA, message types, WiFi, timezone) are preserved as-is

The migration happens automatically on first boot after firmware update. No manual intervention is needed.

## First-Time Setup

### Prerequisites
- MeshCore device with observer MQTT firmware flashed
- WiFi network credentials
- Serial console access (115200 baud) or repeater login via companion app

### Step 1: Configure Radio (after fresh flash/full erase)

If this is a fresh flash, radio parameters must be set to match your mesh network:
```
set radio 910.525,62.5,7,5
set tx 22
```

### Step 2: Configure Device Identity
```
set name MyObserver
set mqtt.iata SEA
```

If migrating from an existing device, restore the private key to keep the same identity:
```
set prv.key <your_64_hex_char_private_key>
```

### Step 3: Configure WiFi

Use the rest of the line as the value (spaces allowed; no quotes). See [WiFi Commands](#wifi-commands).

```
set wifi.ssid YourWiFiNetwork
set wifi.pwd YourWiFiPassword
reboot
```

### Step 4: Configure Timezone (optional)
```
set timezone America/New_York
```

Or use an offset as a fallback:
```
set timezone.offset -5
```

### Step 5: (Optional) Disable Repeating

For receive-only observers (e.g., using a PCB antenna or in a location where repeating is not desired):
```
set repeat off
```

### Step 6: Verify Slot Configuration
```
get mqtt1.preset    # Should show: analyzer-us
get mqtt2.preset    # Should show: analyzer-eu
get mqtt3.preset    # Should show: none
```

### Step 7: (Optional) Add Additional Presets
```
set mqtt3.preset meshmapper
```

### Step 8: Verify Connection
```
get bridge.enabled
get mqtt.rx
get mqtt.tx
get mqtt.status
get wifi.status
```

### Troubleshooting

#### Device Won't Connect to WiFi
```
get wifi.ssid
get wifi.pwd
set wifi.powersave none    # Try disabling power saving
reboot
```

#### No MQTT Messages Appearing
```
get bridge.enabled
set bridge.enabled on
get mqtt.rx                # Should be "on"
set mqtt.rx on
get mqtt.status            # Check per-slot connection status
get mqtt1.diag             # Last slot error details (TLS/sock/time)
get mqtt2.diag
get mqtt3.diag
get mqtt1.preset           # Verify slots are configured
get mqtt.iata              # IATA must be set for MeshCore-topic presets (e.g. Analyzer, ColoradoMesh, TennMesh)
```

#### Timezone Issues
```
get timezone
set timezone America/New_York    # IANA format
set timezone EST                 # Abbreviation
set timezone UTC-5               # UTC offset
```

## SNMP Monitoring

Observer nodes include an optional SNMP v2c agent that exposes radio stats, MQTT connectivity, memory usage, and network information to standard monitoring tools. See [MQTT_SNMP.md](MQTT_SNMP.md) for setup and OID reference.


## Fault Alerts

Fault alerts broadcast LoRa group-channel notifications when WiFi or configured MQTT links stay down past configured thresholds, with optional recovery notices and rate limiting to avoid spam.
For configuration, CLI commands, examples, and operational notes, see [ALERTS.md](ALERTS.md).

## Radio Watchdog

The radio watchdog detects a LoRa radio that appears stuck in RX mode but has stopped seeing any activity (valid packets, radio interrupts, or successful TX). When the configured silence interval is exceeded, the firmware idles the radio and restarts receive mode. This helps long-running MQTT observers recover from conditions such as PSRAM starvation that can cause missed radio interrupts without a full reboot.

Activity is tracked from the most recent of: a valid RX, any radio ISR (including CRC errors), or a successful TX. That composite timestamp reduces false recoveries on quiet meshes where legitimate packet gaps can exceed the watchdog interval.

#### Get Commands
- `get radio.watchdog` - Get watchdog interval in minutes (`0` = disabled)

#### Set Commands
- `set radio.watchdog <minutes>` - Set watchdog interval (`0` to disable, or `1-120`)

**Default:** `5` minutes

**Examples:**
```bash
get radio.watchdog
set radio.watchdog 10    # 10-minute silence before recovery
set radio.watchdog 0     # disable watchdog
```

On very quiet meshes where no traffic is expected for long periods, increase the interval or set `0` to disable the watchdog and avoid unnecessary radio recoveries.
