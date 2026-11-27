# SNMP Agent for MeshCore Observer Firmware

Observer nodes can run a read-only SNMP v2c agent so tools such as Zabbix, LibreNMS, or
PRTG can poll radio, MQTT, memory, and network stats.

- **Protocol**: SNMPv2c (read-only, no SET support)
- **Port**: UDP 161 (standard SNMP)
- **Library**: [0neblock/SNMP_Agent](https://github.com/0neblock/Arduino_SNMP)
- **Off by default**

## Quick Start

```bash
# Enable SNMP
set snmp on
reboot

# Verify from a host on the same network
snmpwalk -v2c -c public <device-ip> 1.3.6.1.4.1.99999
```

## CLI Commands

| Command | Description |
|---|---|
| `get snmp` | Show SNMP agent status (`on` / `off`) |
| `set snmp on` | Enable SNMP agent (restart required) |
| `set snmp off` | Disable SNMP agent (restart required) |
| `get snmp.community` | Show SNMP community string |
| `set snmp.community <string>` | Set community string (restart required, default: `public`) |

## OID Reference

All OIDs are under the private enterprise subtree `1.3.6.1.4.1.99999` (`.iso.org.dod.internet.private.enterprises.99999`).

### System (.1.x.0)

| OID | Type | Description |
|---|---|---|
| `.1.1.0` | INTEGER | System uptime (seconds) |
| `.1.2.0` | STRING | Firmware version |
| `.1.3.0` | STRING | Node name |

### Radio (.2.x.0)

| OID | Type | Description |
|---|---|---|
| `.2.1.0` | INTEGER | Total packets received |
| `.2.2.0` | INTEGER | Total packets sent |
| `.2.3.0` | INTEGER | Receive errors (CRC failures, etc.) |
| `.2.4.0` | INTEGER | Noise floor (dBm) |
| `.2.5.0` | INTEGER | Last RSSI (dBm) |
| `.2.6.0` | INTEGER | Last SNR (dB x 4) |
| `.2.7.0` | INTEGER | Flood packets sent |
| `.2.8.0` | INTEGER | Direct packets sent |
| `.2.9.0` | INTEGER | Flood packets received |
| `.2.10.0` | INTEGER | Direct packets received |
| `.2.11.0` | INTEGER | Total air time (seconds) |

### MQTT (.3.x.0)

| OID | Type | Description |
|---|---|---|
| `.3.1.0` | INTEGER | Connected MQTT slot count |
| `.3.2.0` | INTEGER | Packet queue depth |
| `.3.3.0` | INTEGER | Skipped publishes (memory pressure) |

### Memory (.4.x.0)

| OID | Type | Description |
|---|---|---|
| `.4.1.0` | INTEGER | Free heap (bytes) |
| `.4.2.0` | INTEGER | Max allocatable block (bytes) |
| `.4.3.0` | INTEGER | Free internal RAM (bytes) |
| `.4.4.0` | INTEGER | Free PSRAM (bytes, 0 if no PSRAM) |

### Network (.5.x.0)

| OID | Type | Description |
|---|---|---|
| `.5.1.0` | INTEGER | WiFi RSSI (dBm) |

## Example Output

```
$ snmpwalk -v2c -c public 192.168.50.95 1.3.6.1.4.1.99999

SNMPv2-SMI::enterprises.99999.1.1.0 = INTEGER: 18        # uptime 18s
SNMPv2-SMI::enterprises.99999.1.2.0 = STRING: "v1.14.1"  # firmware
SNMPv2-SMI::enterprises.99999.1.3.0 = STRING: "Heltec Repeater"
SNMPv2-SMI::enterprises.99999.2.1.0 = INTEGER: 3         # 3 packets received
SNMPv2-SMI::enterprises.99999.2.4.0 = INTEGER: -96       # noise floor -96 dBm
SNMPv2-SMI::enterprises.99999.2.5.0 = INTEGER: -25       # last RSSI -25 dBm
SNMPv2-SMI::enterprises.99999.4.1.0 = INTEGER: 199612    # ~195 KB free heap
SNMPv2-SMI::enterprises.99999.4.2.0 = INTEGER: 188404    # ~184 KB max alloc
SNMPv2-SMI::enterprises.99999.5.1.0 = INTEGER: -22       # WiFi RSSI -22 dBm
```

## Architecture

The SNMP agent runs inside the existing MQTT FreeRTOS task on Core 0 (the WiFi/network core). No additional task is created.

- **Radio stats** are pushed from Core 1 (mesh task) every 2 seconds via `updateRadioStats()`
- **MQTT stats** are updated from Core 0 each MQTT task loop iteration
- **Memory and WiFi stats** are read directly in the SNMP loop on Core 0

The agent starts once the network connects with SNMP enabled, and uses about 2-3 KB of RAM.

## Build Configuration

`WITH_SNMP=1` is set on most ESP32 observer environments. Check a board with
`grep -n WITH_SNMP variants/<board>/platformio.ini`. To add it to another observer env:

```ini
build_flags =
  ...
  -D WITH_SNMP=1

build_src_filter =
  ...
  +<helpers/SNMPAgent.cpp>

lib_deps =
  ...
  0neblock/SNMP_Agent
```

## Files

| File | Description |
|---|---|
| `src/helpers/SNMPAgent.h` | SNMP agent wrapper class declaration |
| `src/helpers/SNMPAgent.cpp` | OID registration and stats update implementation |
| `src/helpers/CommonCLI.h` | `MQTTPrefs` (`snmp_enabled`, `snmp_community`) |
| `src/helpers/CommonCLI_Observer.cpp` | CLI commands |
| `src/helpers/bridges/MQTTBridge.cpp` | SNMP loop integration in `mqttTaskLoop()` |
| `examples/simple_repeater/MyMesh.cpp` | Radio stats push and agent initialization |

## Private Enterprise Number

The OIDs use `99999`, a placeholder. A real Private Enterprise Number is free from
[IANA](https://pen.iana.org/pen/PenApplication.page).
