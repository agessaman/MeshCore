# Introduction

Welcome to the MeshCore documentation.

Below are a few quick start guides.

- [Frequently Asked Questions](./faq.md)
- [CLI Commands](./cli_commands.md)
- [Companion Protocol](./companion_protocol.md)
- [Packet Format](./packet_format.md)
- [QR Codes](./qr_codes.md)

## MQTT Observer Firmware

This repository includes DutchMeshCore MQTT observer firmware builds. The observer builds add WiFi, MQTT uplinking, MQTT presets, optional SNMP monitoring, and runtime CLI configuration to repeater and room server firmware.

Quick setup after flashing an observer MQTT build:

```bash
set radio 910.525,62.5,7,5
set tx 22
set name MyObserver
set mqtt.iata SEA
set wifi.ssid YourWiFiNetwork
set wifi.pwd YourWiFiPassword
reboot
```

Fresh MQTT preferences in this firmware default to slot 1 `dutchmeshcore-1`, slot 2 `dutchmeshcore-2`, and slots 3-6 `none`. The DutchMeshCore presets use:

| Preset | Server | Authentication | Transport |
|---|---|---|---|
| `dutchmeshcore-1` | `wss://collector1.dutchmeshcore.nl:443/mqtt` | JWT (Ed25519) | WSS |
| `dutchmeshcore-2` | `wss://collector2.dutchmeshcore.nl:443/mqtt` | JWT (Ed25519) | WSS |

See [CLI Commands](./cli_commands.md#mqtt-observer-firmware-when-mqtt-bridge-support-is-compiled-in) for MQTT, WiFi, timezone, custom broker, and SNMP command details.

If you find a mistake in any of our documentation, or find something is missing, please feel free to open a pull request for us to review.

- [Documentation Source](https://github.com/Dutch-MeshCore/DutchMeshCore.nl-MQTT/tree/main/docs)
