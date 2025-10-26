# MQTT Bridge Implementation for MeshCore

This document describes the MQTT bridge implementation that allows MeshCore repeaters to uplink packet data to multiple MQTT brokers.

## Overview

The MQTT bridge implementation provides:
- Multiple MQTT broker support (up to 3 brokers)
- Automatic reconnection with exponential backoff
- JSON message formatting for status, packets, and raw data
- Configurable topics and QoS levels
- Packet queuing during connection issues

## Files Added

### Core Implementation
- `src/helpers/bridges/MQTTBridge.h` - MQTT bridge class definition
- `src/helpers/bridges/MQTTBridge.cpp` - MQTT bridge implementation
- `src/helpers/MQTTMessageBuilder.h` - JSON message formatting utilities
- `src/helpers/MQTTMessageBuilder.cpp` - JSON message formatting implementation

### Integration
- Updated `examples/simple_repeater/MyMesh.h` - Added MQTT bridge support
- Updated `examples/simple_repeater/MyMesh.cpp` - Added MQTT bridge integration and raw radio data capture
- Updated `src/helpers/CommonCLI.h` - Added MQTT, WiFi, and timezone configuration fields
- Updated `src/helpers/CommonCLI.cpp` - Added MQTT, WiFi, and timezone CLI commands
- Updated `variants/heltec_v3/platformio.ini` - Added MQTT build configuration
- Updated `variants/station_g2/platformio.ini` - Added MQTT build configuration for Station G2

## Build Configuration

To build the MQTT bridge firmware:

### Heltec V3
```bash
pio run -e Heltec_v3_repeater_bridge_mqtt
```

### Station G2
```bash
pio run -e Station_G2_repeater_bridge_mqtt
```

## Default Configuration

The MQTT bridge comes with the following defaults:
- **Origin**: "MeshCore-Repeater"
- **IATA**: "SEA"
- **Status Messages**: Enabled
- **Packet Messages**: Enabled
- **Raw Messages**: Disabled
- **TX Messages**: Disabled (RX only by default)
- **Status Interval**: 5 minutes (300000 ms)
- **Default Broker**: meshtastic.pugetmesh.org:1883 (username: meshdev, password: large4cats)
- **WiFi SSID**: "ssid_here" (must be configured)
- **WiFi Password**: "password_here" (must be configured)
- **Timezone**: "America/Los_Angeles" (Pacific Time with DST support)
- **Timezone Offset**: -8 hours (fallback)
- **Let's Mesh Analyzer US**: Enabled (mqtt-us-v1.letsmesh.net:443)
- **Let's Mesh Analyzer EU**: Enabled (mqtt-eu-v1.letsmesh.net:443)

## CLI Commands

### MQTT Commands

#### Get Commands
- `get mqtt.origin` - Get device origin name
- `get mqtt.iata` - Get IATA code
- `get mqtt.status` - Get status message setting (on/off)
- `get mqtt.packets` - Get packet message setting (on/off)
- `get mqtt.raw` - Get raw message setting (on/off)
- `get mqtt.tx` - Get TX message setting (on/off)
- `get mqtt.interval` - Get status publish interval (ms)
- `get mqtt.analyzer.us` - Get US Let's Mesh Analyzer server setting (on/off)
- `get mqtt.analyzer.eu` - Get EU Let's Mesh Analyzer server setting (on/off)

#### Set Commands
- `set mqtt.origin <name>` - Set device origin name
- `set mqtt.iata <code>` - Set IATA code
- `set mqtt.status on|off` - Enable/disable status messages
- `set mqtt.packets on|off` - Enable/disable packet messages
- `set mqtt.raw on|off` - Enable/disable raw messages
- `set mqtt.tx on|off` - Enable/disable TX packet messages
- `set mqtt.interval <ms>` - Set status publish interval (1000-3600000 ms)
- `set mqtt.analyzer.us on|off` - Enable/disable US Let's Mesh Analyzer server
- `set mqtt.analyzer.eu on|off` - Enable/disable EU Let's Mesh Analyzer server

### WiFi Commands

#### Get Commands
- `get wifi.ssid` - Get WiFi SSID
- `get wifi.pwd` - Get WiFi password

#### Set Commands
- `set wifi.ssid <ssid>` - Set WiFi SSID
- `set wifi.pwd <password>` - Set WiFi password

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

### Bridge-Specific Commands (`mqtt.*`, `wifi.*`, `timezone.*`)
**Implementation-specific settings** - These only apply to the MQTT bridge:
- `mqtt.*` - MQTT broker configuration, message types, and formatting
- `wifi.*` - WiFi connection settings for MQTT connectivity
- `timezone.*` - Timezone configuration for accurate timestamps

This design allows MeshCore to support multiple bridge types simultaneously while keeping configuration clean and logical.

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

### Let's Mesh Analyzer Integration
- **JWT Authentication**: Ed25519-signed tokens for secure MQTT authentication
- **WebSocket MQTT**: Support for MQTT over WebSocket connections (TLS/SSL)
- **Dual Server Support**: Both US and EU servers enabled by default
- **Automatic Token Generation**: Creates authentication tokens using device's Ed25519 keys
- **Username Format**: `v1_{UPPERCASE_PUBLIC_KEY}` (e.g., `v1_7E7662676F7F0850A8A355BAAFBFC1EB7B4174C340442D7D7161C9474A2C9400`)
- **Server Configuration**:
  - US Server: `mqtt-us-v1.letsmesh.net:443` (WebSocket with TLS)
  - EU Server: `mqtt-eu-v1.letsmesh.net:443` (WebSocket with TLS)

## Testing

1. Flash the MQTT bridge firmware to your device
2. Connect to the device via serial console
3. Configure WiFi connection: `set wifi.ssid "YourSSID"` and `set wifi.pwd "YourPassword"`
4. Configure timezone: `set timezone "America/Los_Angeles"`
5. Check MQTT settings: `get mqtt.origin`
6. Monitor MQTT broker for incoming messages

## Dependencies

- **PubSubClient**: MQTT client library
- **ArduinoJson**: JSON message formatting (v6.17.3)
- **NTPClient**: Network time protocol client
- **Timezone**: Timezone conversion library (JChristensen/Timezone)
- **WiFi**: ESP32 WiFi functionality
- **Ed25519**: Cryptographic library for JWT token signing
- **JWTHelper**: Custom JWT token generation for Let's Mesh Analyzer authentication

## Future Enhancements

- Full WebSocket MQTT implementation (currently JWT tokens are generated but WebSocket publishing is pending)
- Multiple broker configuration via CLI
- Advanced packet filtering
- Custom topic templates
- TLS/SSL support for secure connections
- Real-time WebSocket MQTT publishing to Let's Mesh Analyzer servers
