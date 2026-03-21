# MQTT Bridge Implementation for MeshCore

This document describes the MQTT bridge implementation that allows MeshCore repeaters to uplink packet data to multiple MQTT brokers.

## Quick Start Guide

### Essential Commands to Get MQTT Repeater Running

**1. Connect to device console via repeater login or serial console (115200 baud)**

**2. Configure WiFi Credentials**
```bash
set wifi.ssid YourWiFiNetwork
set wifi.pwd YourWiFiPassword
```

If you wish to upload to the MeshCore Analyzer, also `set mqtt.iata XXX` to a valid IATA airport code.

**3. Reboot to Connect to WiFi**
```bash
reboot
```

**4. Toggle bridge.source to rx**
```bash
set bridge.source rx
```

**5. Verify Configuration**
```bash
get wifi.ssid
get bridge.enabled
get bridge.source
get mqtt.origin
get mqtt.iata
get mqtt1.preset
get mqtt2.preset
get mqtt3.preset
```

**6. Restart Bridge (if needed)**
```bash
# Option A: Toggle bridge off then on
set bridge.enabled off
set bridge.enabled on

# Option B: Full device reboot
reboot
```

**That's it!** The device will now:
- Connect to WiFi automatically
- Start uplinking mesh packets to configured MQTT brokers
- By default, publish to Let's Mesh Analyzer US (slot 1) and EU (slot 2)
- Use device name as MQTT origin (set automatically)

---

## Overview

The MQTT bridge implementation provides:
- Up to 3 concurrent MQTT connection slots with built-in presets
- Built-in presets for LetsMesh Analyzer (US/EU) and MeshMapper
- Custom broker support with username/password authentication
- JWT (Ed25519 device signing) authentication for preset brokers
- WSS (WebSocket Secure) and direct MQTT transport
- Automatic reconnection with exponential backoff
- JSON message formatting for status, packets, and raw data
- Packet queuing during connection issues
- Automatic migration from old configuration format

## Architecture

### Slot-Based Preset System

The MQTT bridge uses a slot-based architecture with up to 3 concurrent connections. Each slot can be configured with a built-in preset or custom broker settings.

**Built-in Presets:**

| Preset | Server | Auth | Transport |
|--------|--------|------|-----------|
| `analyzer-us` | mqtt-us-v1.letsmesh.net:443 | JWT (Ed25519) | WSS |
| `analyzer-eu` | mqtt-eu-v1.letsmesh.net:443 | JWT (Ed25519) | WSS |
| `meshmapper` | mqtt.meshmapper.cc:443 | JWT (Ed25519) | WSS |
| `custom` | User-configured | Username/Password | MQTT or WSS |
| `none` | (disabled) | — | — |

**Default Configuration:**
- Slot 1: `analyzer-us`
- Slot 2: `analyzer-eu`
- Slot 3: `none`

**Memory Limits:**
- With PSRAM: All 3 slots can be active simultaneously
- Without PSRAM: Maximum 2 active slots (each WSS/TLS connection requires ~40KB internal heap)
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
```

### Build Flags
- `WITH_MQTT_BRIDGE=1` - Enable MQTT bridge (required)
- `MQTT_DEBUG=1` - Enable debug logging (optional)
- `MQTT_WIFI_TX_POWER` - WiFi TX power level (default: `WIFI_POWER_11dBm`)
- `MQTT_WIFI_POWER_SAVE_DEFAULT` - Default WiFi power save mode (0=min, 1=none, 2=max)

## Default Configuration

The MQTT bridge comes with the following defaults:
- **Origin**: Device name (set automatically)
- **IATA**: (must be configured)
- **Status Messages**: Enabled
- **Packet Messages**: Enabled
- **Raw Messages**: Disabled
- **TX Messages**: Disabled (RX only by default)
- **Status Interval**: 5 minutes (300000 ms)
- **Slot 1**: `analyzer-us` (mqtt-us-v1.letsmesh.net:443)
- **Slot 2**: `analyzer-eu` (mqtt-eu-v1.letsmesh.net:443)
- **Slot 3**: `none` (disabled)
- **WiFi SSID**: "ssid_here" (must be configured)
- **WiFi Password**: "password_here" (must be configured)
- **WiFi Power Save**: "min" (minimum power saving, balanced performance and power)
- **Timezone**: "America/Los_Angeles" (Pacific Time with DST support)
- **Timezone Offset**: -8 hours (fallback)

## CLI Commands

### MQTT Slot Commands

Each slot (1-3) supports the following commands:

#### Get Commands
- `get mqtt1.preset` - Get slot 1 preset name
- `get mqtt2.preset` - Get slot 2 preset name
- `get mqtt3.preset` - Get slot 3 preset name
- `get mqttN.server` - Get custom server hostname for slot N
- `get mqttN.port` - Get custom server port for slot N
- `get mqttN.username` - Get custom username for slot N
- `get mqttN.password` - Get custom password for slot N

#### Set Commands
- `set mqtt1.preset analyzer-us` - Set slot 1 to LetsMesh Analyzer US
- `set mqtt1.preset analyzer-eu` - Set slot 1 to LetsMesh Analyzer EU
- `set mqtt1.preset meshmapper` - Set slot 1 to MeshMapper
- `set mqtt1.preset custom` - Set slot 1 to custom broker (configure server/port/username/password)
- `set mqtt1.preset none` - Disable slot 1
- `set mqttN.server <hostname>` - Set custom server hostname for slot N
- `set mqttN.port <port>` - Set custom server port for slot N (1-65535)
- `set mqttN.username <username>` - Set custom username for slot N
- `set mqttN.password <password>` - Set custom password for slot N

**Note:** Custom server/port/username/password settings only apply when the slot's preset is `custom`.

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

### MQTT Shared Commands

These settings apply across all MQTT slots:

#### Get Commands
- `get mqtt.origin` - Get device origin name
- `get mqtt.iata` - Get IATA code
- `get mqtt.status` - Get MQTT status summary (connection info per slot)
- `get mqtt.packets` - Get packet message setting (on/off)
- `get mqtt.raw` - Get raw message setting (on/off)
- `get mqtt.tx` - Get TX message setting (on/off)
- `get mqtt.interval` - Get status publish interval
- `get mqtt.owner` - Get owner public key (serial console only)
- `get mqtt.email` - Get owner email address (serial console only)

#### Set Commands
- `set mqtt.origin <name>` - Set device origin name
- `set mqtt.iata <code>` - Set IATA code (auto-uppercased)
- `set mqtt.status on|off` - Enable/disable status messages
- `set mqtt.packets on|off` - Enable/disable packet messages
- `set mqtt.raw on|off` - Enable/disable raw messages
- `set mqtt.tx on|off` - Enable/disable TX packet messages
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
  - `none` - No power saving (best performance, highest power consumption)
  - `min` - Minimum power saving (default, balanced performance and power)
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

### Bridge Commands

#### Get Commands
- `get bridge.source` - Get packet source (rx/tx)
- `get bridge.enabled` - Get bridge enabled status (on/off)

#### Set Commands
- `set bridge.source rx|tx` - Set packet source (rx for received, tx for transmitted)
- `set bridge.enabled on|off` - Enable/disable bridge

## Command Architecture

The CLI commands are organized into two levels:

### Bridge Commands (`bridge.*`)
**Low-level bridge control** - These settings apply to all bridge types (MQTT, RS232, ESP-NOW, etc.):
- `bridge.enabled` - Master switch for the entire bridge system
- `bridge.source` - Controls which packet events to capture (RX vs TX)

### Bridge-Specific Commands (`mqtt.*`, `mqttN.*`, `wifi.*`, `timezone.*`)
**Implementation-specific settings** - These only apply to the MQTT bridge:
- `mqttN.*` - Per-slot MQTT broker configuration (N = 1, 2, or 3)
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
  "timestamp": "2024-01-01T12:00:00.000000",
  "origin": "Device Name",
  "origin_id": "DEVICE_PUBLIC_KEY",
  "model": "device_model",
  "firmware_version": "firmware_version",
  "radio": "radio_info",
  "client_version": "meshcore-custom-repeater/{build_date}"
}
```

### Packet Message
```json
{
  "origin": "MeshCore-HOWL",
  "origin_id": "A1B2C3D4E5F67890...",
  "timestamp": "2024-01-01T12:00:00.000000",
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
  "hash": "A1B2C3D4E5F67890",
  "path": "node1,node2,node3"
}
```

### Raw Message
```json
{
  "origin": "MeshCore-HOWL",
  "origin_id": "A1B2C3D4E5F67890...",
  "timestamp": "2024-01-01T12:00:00.000000",
  "type": "RAW",
  "data": "F5930103807E5F1E..."
}
```

## Key Features

### Slot-Based Preset System
- Up to 3 concurrent MQTT connections (with PSRAM), 2 without PSRAM
- Built-in presets for LetsMesh Analyzer (US/EU) and MeshMapper
- Custom broker support with username/password auth
- JWT (Ed25519) authentication for preset brokers
- Automatic reconnection with exponential backoff per slot
- JWT token buffers only allocated for JWT-auth slots (memory efficient)
- Deferred construction: MQTTBridge is heap-allocated in `begin()` to avoid ESP32 static init crashes

### Raw Radio Data Capture
- Captures actual raw radio transmission data (including radio headers)
- Uses proper MeshCore packet hashing (SHA256-based)
- Provides accurate SNR/RSSI values from actual radio reception
- Supports both RX and TX packet uplinking (configurable)

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
- **JWT Authentication**: Ed25519-signed tokens for secure MQTT authentication (used by all built-in presets)
- **Username/Password**: Standard MQTT authentication for custom brokers
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
- MeshCore device with MQTT bridge firmware flashed
- WiFi network credentials
- LoRa-capable device for configuration (repeater console)

### Step 1: Configure WiFi
```
set wifi.ssid YourWiFiNetwork
set wifi.pwd YourWiFiPassword
reboot
```

### Step 2: Configure Device Identity
```
set mqtt.iata SEA
get mqtt.origin
```

### Step 3: Verify Slot Configuration
```
get mqtt1.preset    # Should show: analyzer-us
get mqtt2.preset    # Should show: analyzer-eu
get mqtt3.preset    # Should show: none
```

### Step 4: (Optional) Add MeshMapper
```
set mqtt3.preset meshmapper
```

### Step 5: (Optional) Configure Custom Broker
```
set mqtt3.preset custom
set mqtt3.server your-broker.example.com
set mqtt3.port 1883
set mqtt3.username your-username
set mqtt3.password your-password
```

### Step 6: Verify Connection
```
set bridge.source rx
get bridge.enabled
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
get mqtt.status            # Check per-slot connection status
get mqtt1.preset           # Verify slots are configured
get mqtt.iata              # IATA must be set for Analyzer presets
```

#### Timezone Issues
```
get timezone
set timezone America/New_York    # IANA format
set timezone EST                 # Abbreviation
set timezone UTC-5               # UTC offset
```

## Dependencies

- **PsychicMqttClient**: MQTT client library (supports WSS and direct MQTT)
- **ArduinoJson**: JSON message formatting
- **NTPClient**: Network time protocol client
- **Timezone**: Timezone conversion library (JChristensen/Timezone)
- **WiFi**: ESP32 WiFi functionality
- **Ed25519**: Cryptographic library for JWT token signing
- **JWTHelper**: Custom JWT token generation for device authentication
