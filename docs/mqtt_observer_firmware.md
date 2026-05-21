# MQTT Observer Firmware

This page documents the MQTT observer firmware in this repository. The observer builds add WiFi, MQTT uplinking, MQTT presets, optional SNMP monitoring, and runtime CLI configuration to repeater and room server firmware.

## Quick Setup

After flashing an observer MQTT build, connect to the serial console at 115200 baud or use repeater login.

1. Configure radio settings to match your mesh:

```bash
set radio 910.525,62.5,7,5
set tx 22
```

2. Configure identity and MQTT location code:

```bash
set name MyObserver
set mqtt.iata SEA
```

To migrate an existing node identity, restore its private key:

```bash
set prv.key <your_64_hex_char_private_key>
```

3. Configure WiFi and reboot:

```bash
set wifi.ssid YourWiFiNetwork
set wifi.pwd YourWiFiPassword
reboot
```

4. Optional: make the observer receive-only:

```bash
set repeat off
```

5. Verify MQTT and WiFi state:

```bash
get bridge.enabled
get mqtt.rx
get mqtt.tx
get mqtt.origin
get mqtt.iata
get mqtt1.preset
get mqtt2.preset
get mqtt.status
get wifi.status
```

## DutchMeshCore Defaults

Fresh MQTT preferences in this firmware default to:

| Setting | Default |
|---|---|
| Slot 1 | `dutchmeshcore-1` |
| Slot 2 | `dutchmeshcore-2` |
| Slots 3-6 | `none` |
| RX packet uplinking | `on` |
| TX packet uplinking | `advert` |
| Status messages | `on` |
| Packet messages | `on` |
| Raw messages | `off` |
| Status interval | 5 minutes |
| WiFi power save | `none` |
| WiFi SSID/password | blank |
| Timezone | blank, UTC until configured |
| Repeat/forwarding | on |

The built-in DutchMeshCore presets are:

| Preset | Server | Authentication | Transport |
|---|---|---|---|
| `dutchmeshcore-1` | `wss://collector1.dutchmeshcore.nl:443/mqtt` | JWT (Ed25519) | WSS |
| `dutchmeshcore-2` | `wss://collector2.dutchmeshcore.nl:443/mqtt` | JWT (Ed25519) | WSS |

Both use the MeshCore topic style: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/{type}`.

## Supported Presets

The firmware includes these preset names:

```text
analyzer-us, analyzer-eu, meshmapper, meshrank, waev, meshomatic,
cascadiamesh, tennmesh, nashmesh, chimesh, meshat.se,
eastidahomesh, dutchmeshcore-1, dutchmeshcore-2, coloradomesh,
custom, none
```

Use `get mqtt.presets` to list presets from the device. If the reply ends with `next:<idx>`, continue with `get mqtt.presets <idx>`.

## Build Targets

MQTT observer targets are PlatformIO environments whose names end in `_observer_mqtt`. Examples present in this repository include:

```bash
pio run -e Heltec_v3_repeater_observer_mqtt
pio run -e heltec_v4_repeater_observer_mqtt
pio run -e Station_G2_repeater_observer_mqtt
pio run -e LilyGo_TLora_V2_1_1_6_repeater_observer_mqtt
pio run -e LilyGo_TLora_V2_1_1_6_room_server_observer_mqtt
```

The MQTT bridge is enabled with `WITH_MQTT_BRIDGE=1`. SNMP support is enabled on supported observer builds with `WITH_SNMP=1`.

## Partition And Flashing Notes

Some MQTT observer builds use larger app partitions for MQTT, TLS, and certificate bundle support. When a board's partition table changes, flash the merged firmware (`*-merged.bin`) the first time so the bootloader and partition table are written together.

| Environment | Partition table | Flash size | App slot size | Notes |
|---|---|---:|---:|---|
| `LilyGo_T3S3_sx1262_repeater_observer_mqtt` | `min_spiffs.csv` | 4 MB | 1.875 MB | Changed from default |
| `LilyGo_T3S3_sx1262_room_server_observer_mqtt` | `min_spiffs.csv` | 4 MB | 1.875 MB | Changed from default |
| `LilyGo_TLora_V2_1_1_6_repeater_observer_mqtt` | `min_spiffs.csv` | 4 MB | 1.875 MB | TTGO LoRa32 V1.0; observer env omits `sensor_base` |
| `LilyGo_TLora_V2_1_1_6_room_server_observer_mqtt` | `min_spiffs.csv` | 4 MB | 1.875 MB | Same as repeater observer |
| `Station_G2_repeater_observer_mqtt` | `default_16MB.csv` | 16 MB | 6.25 MB | 16 MB flash board |
| `Station_G2_room_server_observer_mqtt` | `default_16MB.csv` | 16 MB | 6.25 MB | 16 MB flash board |

Build and flash a merged binary:

```bash
pio run -t mergebin -e LilyGo_T3S3_sx1262_repeater_observer_mqtt
esptool.py write_flash 0x0 .pio/build/LilyGo_T3S3_sx1262_repeater_observer_mqtt/firmware-merged.bin
```

If the partition layout changes, stored settings in NVS are typically wiped or invalidated. Expect to reconfigure admin preferences, WiFi, MQTT slots, device name, and related settings.

## MQTT Topics

The bridge publishes status, packet, and raw messages:

| Type | Topic |
|---|---|
| Status | `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/status` |
| Packets | `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/packets` |
| Raw | `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/raw` |

`{DEVICE_PUBLIC_KEY}` is the device public key as 64 hexadecimal characters.

## SNMP Monitoring

Observer nodes can include an optional SNMP v2c agent for monitoring radio stats, MQTT connectivity, memory usage, and WiFi RSSI.

```bash
set snmp on
reboot
snmpwalk -v2c -c public <device-ip> 1.3.6.1.4.1.99999
```

SNMP is read-only, listens on UDP port 161, and is disabled by default. The community string defaults to `public` and can be changed with `set snmp.community <string>`.

## Troubleshooting

For WiFi issues:

```bash
get wifi.ssid
get wifi.pwd
get wifi.status
set wifi.powersave none
reboot
```

For missing MQTT messages:

```bash
get bridge.enabled
set bridge.enabled on
get mqtt.rx
set mqtt.rx on
get mqtt.tx
get mqtt.status
get mqtt1.preset
get mqtt2.preset
get mqtt.iata
```

For timezone issues:

```bash
get timezone
set timezone Europe/Amsterdam
set timezone.offset 1
```
