# DutchMeshCore Observer Firmware — Changes vs Main

> **⚠ HIGHLY EXPERIMENTAL** — This branch is based on `mqtt-bridge-implementation-flex-lwt`, which is itself an active development branch not yet merged into main. The MQTT observer feature, and the extended device support added here, should be considered experimental. Expect bugs, breaking changes, and incomplete testing across hardware. Use in production at your own risk.

This branch customises the default MQTT configuration for the [dutchmeshcore.nl](https://dutchmeshcore.nl) observer network. Observers only need to configure WiFi credentials and their IATA code after flashing.

## Changes

### 1. Default MQTT broker presets replaced (`src/helpers/MQTTPresets.h`)

The first two built-in preset slots previously pointed to the LetsMesh Analyzer (US and EU). They have been replaced with the DutchMeshCore collectors:

| Slot | Old preset | New preset |
|------|-----------|------------|
| 1 | `analyzer-us` — `wss://mqtt-us-v1.letsmesh.net:443/mqtt` | `dutchmeshcore-1` — `wss://collector1.dutchmeshcore.nl:443` |
| 2 | `analyzer-eu` — `wss://mqtt-eu-v1.letsmesh.net:443/mqtt` | `dutchmeshcore-2` — `wss://collector2.dutchmeshcore.nl:443` |

Both use JWT authentication (Ed25519 device identity) and the ISRG Root X1 (Let's Encrypt) CA certificate.

**Why slots 1 and 2 specifically:** non-PSRAM devices are limited to 2 active MQTT connections at runtime. By placing the DutchMeshCore collectors in the first two slots they are guaranteed to be active on all supported hardware, including lower-cost boards without PSRAM.

### 2. Default TX mode changed to `on` (`examples/simple_repeater/MyMesh.cpp`, `src/helpers/CommonCLI.cpp`)

`mqtt_tx_enabled` default changed from `2` (own adverts only) to `1` (all TX packets).

| Setting | Old default | New default |
|---------|------------|-------------|
| `mqtt.tx` | `advert` | `on` |
| `mqtt.rx` | `on` | `on` (unchanged) |
| `bridge.enabled` | `on` | `on` (unchanged) |

**Why:** observers should forward all traffic — both received (RX) and transmitted (TX) packets — to give the collectors a complete view of mesh activity.

### 3. MQTT observer envs added to all eligible ESP32 devices

The original upstream branch had MQTT observer builds for 10 devices. This branch extends that to 30 devices by adding `*_observer_mqtt` environments to every remaining ESP32 and ESP32-C6 based variant. NRF52, RP2040, and STM32 boards were intentionally excluded — they have no WiFi and cannot connect to an MQTT broker.

All new observer envs follow the same pattern as the existing V3/V4 reference builds:

| Feature | Value |
|---------|-------|
| `MQTT_DEBUG` | enabled |
| `MQTT_MEMORY_DEBUG` | enabled (repeater envs) |
| `MQTT_WIFI_TX_POWER` | `WIFI_POWER_11dBm` — conservative TX power to avoid LoRa interference |
| `WITH_SNMP` | enabled (repeater envs) |
| `board_ssl_cert_source` | `adafruit` |
| WiFi credential block | commented-out `#  -D WIFI_SSID/WIFI_PWD` lines in build_flags |

New variants added (20 devices, both repeater and room-server observer envs where applicable):

`ebyte_eora_s3`, `generic-e22` (sx1262 + sx1268), `heltec_ct62`, `heltec_tracker`, `heltec_tracker_v2`, `heltec_v2`, `heltec_wireless_paper`, `lilygo_t3s3_sx1276`, `lilygo_tbeam_1w`, `lilygo_tdeck`, `lilygo_tlora_c6`, `lilygo_tlora_v2_1`, `m5stack_unit_c6l`, `meshadventurer`, `nibble_screen_connect`, `tenstar_c3`, `thinknode_m2`, `thinknode_m5`, `xiao_c3`, `xiao_c6`

> **These additional device builds are highly experimental and untested.** They follow the same pattern as the existing observer envs but have not been verified on real hardware. ESP32-C6 and ESP32-C3 variants (`lilygo_tlora_c6`, `m5stack_unit_c6l`, `xiao_c6`, `tenstar_c3`, `xiao_c3`) carry extra risk as the C6/C3 platform support is itself marked experimental upstream.

## What observers need to configure after flashing

1. `set wifi.ssid <your-ssid>`
2. `set wifi.password <your-password>`
3. `set mqtt.iata <your-3-letter-airport-code>`
Only when updating a repeater/roomserver that's already set up you do step 4&5:
4. `set mqtt1.preset dutchmeshcore-1`
5. `set mqtt2.preset dutchmeshcore-2`

Everything else is pre-configured by this firmware.
