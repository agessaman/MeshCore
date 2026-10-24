# MQTT Observer Firmware

Observer firmware turns a MeshCore repeater or room server into an uplink that publishes
the packets it hears to up to six MQTT brokers.

## Quick Start Guide

You need the device, WiFi credentials, and a console: USB serial at 115200 baud, or a
repeater admin login from the companion app.

**1. Flash.** Use the [MeshCore Observer Flasher](https://observer.gessaman.com/): pick
**MQTT Observer Firmware** and your device, then flash from Chrome or Edge. To build it
yourself, see [Build Configuration](#build-configuration).

**2. Set the radio** on a fresh flash. These must match the rest of your mesh:

```bash
set radio 910.525,62.5,7,5    # <freq_MHz>,<bw_kHz>,<sf>,<cr>
set tx 22
```

**3. Set the identity:**

```bash
set name MyObserver
set mqtt.iata SEA
set prv.key <64_hex_chars>    # optional: keep the identity of a node you are replacing
```

**4. Set WiFi.** The value is the rest of the line; do not add quotes.

```bash
set wifi.ssid YourWiFiNetwork
set wifi.pwd YourWiFiPassword
```

**5. Optional settings:**

```bash
set mqtt3.preset meshmapper   # add a broker (slots 1 and 2 default to Let's Mesh US and EU)
set timezone America/New_York # published timestamps stay UTC either way
set repeat off                # receive-only observer
```

**6. Reboot and verify:**

```bash
reboot
get link.status
get mqtt.status
```

The node joins WiFi, connects to each configured broker, and starts publishing. Its
MQTT origin is the node name.

## Broker Presets

Each of the six slots holds one preset:

```bash
set mqtt3.preset meshmapper
set mqtt3.preset none         # disable the slot
```

Broker address, transport, and credentials ship in the firmware. The **Extra setup**
column lists the presets that need more. Every preset except `meshrank` uses the
`meshcore/{iata}/…` topic layout, so `mqtt.iata` must be set.

`get mqtt.presets` lists what the running firmware ships. The table matches the current build.

| Preset | Broker | Auth | Extra setup |
|--------|--------|------|-------------|
| `analyzer-us` | `wss://mqtt-us-v1.letsmesh.net:443/mqtt` | JWT | default slot 1 |
| `analyzer-eu` | `wss://mqtt-eu-v1.letsmesh.net:443/mqtt` | JWT | default slot 2 |
| `nz-analyzer` | `wss://meshcore-mqtt-1.baird.io:443` | JWT | |
| `meshmapper` | `wss://mqtt.meshmapper.net:443/mqtt` | JWT | |
| `meshrank` | `mqtts://meshrank.net:8883` | token in topic | `set mqttN.token <token>` |
| `waev` | `wss://mqtt.waev.app:443/mqtt` | JWT | |
| `meshomatic` | `wss://us-east.meshomatic.net:443/mqtt` | JWT | |
| `cascadiamesh` | `wss://mqtt-v1.cascadiamesh.org:443/mqtt` | JWT | |
| `tennmesh` | `mqtt://mqtt.tennmesh.com:1883` | user/pass (built in) | |
| `nashmesh` | `mqtt://mqtt.nashme.sh:1883` | user/pass (built in) | |
| `ctmesh` | `mqtt://mqtt.ctmesh.org:1883` | user/pass (built in) | |
| `chimesh` | `wss://mqtt.chimesh.org:443` | JWT | |
| `meshat.se` | `wss://meshcore-mqtt.meshat.se:443` | JWT | |
| `eastidahomesh` | `mqtt://live.eastidahomesh.com:1883` | none | |
| `coloradomesh` | `wss://mqtt.meshcore.coloradomesh.org:443` | JWT | |
| `dutchmeshcore-1` | `wss://collector1.dutchmeshcore.nl:443/mqtt` | JWT | |
| `dutchmeshcore-2` | `wss://collector2.dutchmeshcore.nl:443/mqtt` | JWT | |
| `meshcore-ca-1` | `wss://mqtt1.meshcore.ca:443/mqtt` | JWT | |
| `meshcore-ca-2` | `wss://mqtt2.meshcore.ca:443/mqtt` | JWT | |
| `meshcore-fi` | `wss://mc-mqtt.meshcore.fi:443/` | JWT | |
| `okimesh-1` | `wss://mqtt1.okimesh.org:9002/mqtt` | JWT | |
| `okimesh-2` | `wss://mqtt2.okimesh.org:9002/mqtt` | JWT | |
| `inwmesh` | `mqtts://scope.inwmesh.org:8883` | user/pass (per slot) | `set mqttN.username`, `set mqttN.password` |
| `bostonmesh` | `wss://mqttmc01.bostonme.sh:443/mqtt` | JWT | |
| `rflab` | `wss://mqtt.rflab.io:443` | JWT | |
| `ipnt.uk` | `wss://mqtt.ipnt.uk:443` | JWT | |
| `flmesh` | `wss://mcmqtt.jntconnections.com:443` | JWT | |
| `corecomms` | `wss://mqtt.corecomms.net:443/mqtt` | JWT | |
| `meshtexas` | `wss://mqtt.meshtexas.org:443/mqtt` | JWT | |
| `mesh-chaun14` | `mqtt://mqtt.mesh.chaun14.fr:1884` | user/pass (username is the public key) | `set mqttN.password` |
| `wcmesh` | `wss://mqtt.wcmesh.com:443` | JWT | |
| `atvirastinklas` | `wss://mqtt-mc.atvirastinklas.lt:443` | JWT | |
| `gomesh` | `wss://mqtt.gomesh.dev:443` | JWT | |
| `idahomesh` | `wss://mqtt.idahomesh.org:443/mqtt` | JWT | |
| `ntxmesh` | `wss://ntxmesh.dhovin.me:8883` | JWT | |
| `bsmesh` | `wss://mqtt.bsmesh.de:8885` | JWT | |
| `marwoj` | `mqtts://mqtt.marwoj.net:8883` | user/pass (built in) | |
| `sacmesh` | `wss://mqtt.sacmesh.org:443/` | JWT | |
| `custom` | your broker | user/pass, or JWT when `mqttN.audience` is set | see [Custom Brokers](#custom-brokers) |
| `none` | slot disabled | | |

`wss://` is WebSocket over TLS, `mqtts://` is MQTT over TLS, and `mqtt://` is unencrypted.

### Slots and Memory Limits

Fresh installs use `analyzer-us` in slot 1, `analyzer-eu` in slot 2, and `none` in 3-6.

- **With PSRAM:** all six slots can be active.
- **Without PSRAM:** at most two TLS slots (`wss://` or `mqtts://`); each needs about 40 KB
  of internal heap. Extra slots show as `(inactive)` in `get mqtt.status` and stay saved,
  so they activate if the settings move to a PSRAM board.

**Neighbors publication** (`WITH_MQTT_NEIGHBORS`) is on for every PSRAM board, and for the
ESP32-S3 boards without PSRAM that set `-D MQTT_NEIGHBORS_WITHOUT_PSRAM=1` (Heltec V3/WSL3,
RAK3112, Heltec Tracker v1.1/v2). Those builds cap the table at 20 entries and spend about
7.4 KB of static DRAM (9.6 KB on room servers) plus up to 13 KB per publish, from the same
heap TLS uses. Classic ESP32 builds leave it off.

## CLI Commands

Commands work over serial and over a remote admin login unless marked *serial only*.

### MQTT Slots

`N` is the slot number, 1-6.

| Command | Purpose |
|---------|---------|
| `get/set mqttN.preset <name>` | Broker preset, `custom`, or `none` |
| `get/set mqttN.server <host or URL>` | Custom broker address |
| `get/set mqttN.port <1-65535>` | Custom broker port (optional when the URL has a scheme) |
| `get/set mqttN.username <user>` | For `custom` and per-slot presets such as `inwmesh` |
| `get/set mqttN.password <pass>` | Same; `get` shows the value on serial only |
| `get/set mqttN.token <token>` | Per-slot token, e.g. MeshRank; `get` shows the value on serial only |
| `get/set mqttN.topic <template>` | Topic template, `custom` slots only |
| `get/set mqttN.audience <aud>` | Switches a `custom` slot to JWT auth; bare `set` clears it |
| `get/set mqttN.filter <all\|none\|list>` | Packet types sent to this slot |
| `get mqttN.diag` | Last connection error and timing for the slot |

Server and port apply only to `custom` slots. The username and password apply to
`custom` and to presets that take per-device credentials (`inwmesh`, `mesh-chaun14`).

#### Packet filters

Each slot has its own allowlist of payload types, by name or number:

```bash
set mqtt1.filter txt_msg,advert   # same as: set mqtt1.filter 2,4
set mqtt1.filter none             # stay connected for status/neighbors, send no packets
set mqtt1.filter                  # back to all
```

| Type | Name | Type | Name |
|------|------|------|------|
| 0 | `req` | 8 | `path` |
| 1 | `response` | 9 | `trace` |
| 2 | `txt_msg` | 10 | `multipart` |
| 3 | `ack` | 11 | `control` |
| 4 | `advert` | 12-14 | reserved (number only) |
| 5 | `grp_txt` | 15 | `raw_custom` |
| 6 | `grp_data` | | |
| 7 | `anon_req` | | |

- The filter covers `packets` and `raw` publishes for RX and TX. It does not affect
  forwarding, status, or neighbors. Changes apply live.
- `get mqttN.filter` answers in numeric form (`all`, `none`, or an ascending list).
- Filtered packets are dropped before they reach the publish queue. `get mqtt.stats`
  counts them as `filt=<n>`, and `get mqttN.diag` shows a slot's filter when it is not
  `all`. A long diag reply shortens it to `filter:<n>/16`.
- WebConfig shows the filter as a checkbox per type.

**Rolling back:** builds from the release that added filters onward ignore filters they
don't know about. Older builds reject the longer settings file and fall back to defaults,
losing WiFi and broker settings. Set every slot back to `all` before rolling back that far.

#### MeshRank

```bash
set mqtt3.preset meshrank
set mqtt3.token <token from the MeshRank website>
```

MeshRank receives status, packets, and neighbors under `meshrank/uplink/{token}/{device}/`.
It never receives `raw`, and its publishes are not retained.

### Custom Brokers

```bash
set mqtt3.preset custom
set mqtt3.server your-broker.example.com
set mqtt3.port 1883
set mqtt3.username your-username
set mqtt3.password your-password
```

**JWT:** setting `audience` switches the slot to the same Ed25519 JWT scheme the built-in
presets use. The node connects as `v1_{PUBLIC_KEY}`, renews the token before it expires
(24 h lifetime), and includes `mqtt.owner` / `mqtt.email` in the token when set.

```bash
set mqtt3.server wss://my-broker.example.com:443/mqtt
set mqtt3.audience my-broker.example.com
```

**LAN broker without TLS:** `set mqtt3.server ws://192.168.1.50:9001/mqtt`. Non-TLS URLs
skip certificate checks.

With a full URL (`mqtt://`, `mqtts://`, `ws://`, `wss://`), the port comes from the URL
or the scheme's default.

#### Topic templates

`set mqttN.topic` applies to `custom` slots only. The default is
`meshcore/{iata}/{device}/{type}`.

| Placeholder | Value |
|-------------|-------|
| `{iata}` | `mqtt.iata` |
| `{device}` | public key, 64 hex chars |
| `{token}` | `mqttN.token` |
| `{type}` | `status`, `packets`, `raw`, or `neighbors` |

### MQTT Shared Settings

| Command | Purpose | Default |
|---------|---------|---------|
| `get/set mqtt.origin <name>` | Origin name in payloads | node name |
| `get/set mqtt.iata <code>` | 3-character location code, uppercased; bare `set` clears | blank |
| `set mqtt.status on\|off` | Status messages (periodic and on connect) | on |
| `get/set mqtt.packets on\|off` | `packets` topic | on |
| `get/set mqtt.raw on\|off` | `raw` topic | off |
| `get/set mqtt.rx on\|off` | Publish received packets | on |
| `get/set mqtt.tx on\|off\|advert` | Publish transmitted packets; `advert` sends only this node's own adverts | advert |
| `get/set mqtt.advert.verify on\|off` | Drop received adverts with bad signatures (counted as `badsig=` in `mqtt.stats`) | off |
| `get/set mqtt.interval <1-60>` | Status interval, minutes | 5 |
| `get/set mqtt.neighbors on\|off` | Periodic neighbors publish (neighbors builds) | off |
| `get/set mqtt.neighbors.interval <12-336>` | Neighbors interval, hours | 24 |
| `get/set mqtt.ntp <host\|none>` | Primary NTP server; `set` syncs once to test it | `pool.ntp.org` |
| `get mqtt.ntp.diag` | Probe every NTP server without changing the clock | |
| `get/set mqtt.owner <64 hex>` | Owner public key for JWT (`get` is serial only) | |
| `get/set mqtt.email <email>` | Owner email for JWT (`get` is serial only) | |
| `get mqtt.presets [start]` | List presets, paged; continue from `next:<idx>` | |
| `get mqtt.status` | Per-slot connection state | |

RX/TX, filter, and neighbors changes apply live. `get mqtt.analyzer.us` and
`get mqtt.analyzer.eu` remain for old scripts and report whether slot 1 or 2 holds that preset.

### Network and WiFi

| Command | Purpose |
|---------|---------|
| `get/set wifi.ssid <ssid>` | Up to 31 characters |
| `get/set wifi.pwd <password>` | Up to 63 characters; `get` shows it on serial only. `set wifi.pwd ` (trailing space) for an open network |
| `get/set wifi.powersave none\|min\|max` | Default `none` |
| `get link.status` | Active medium, IP, signal, uptime, or the last disconnect reason |
| `get link.diag` | Why Ethernet or WiFi was chosen |
| `get link.dns` | Live DNS servers and what each medium's DHCP lease supplied |
| `get wifi.status` | Older WiFi-only form of `link.status` |

The SSID and password take everything after the first space, so spaces are fine and
quotes are stored literally. New credentials are used on the next reconnect, no reboot needed.

### Timezone

| Command | Purpose |
|---------|---------|
| `get/set timezone <tz>` | IANA name (`America/Los_Angeles`), abbreviation (`PST`, `CET`, `BST`, ...), or offset (`UTC-8`, `+5`) |
| `get/set timezone.offset <-12..14>` | Fallback hours when `timezone` is blank |

The timezone affects local display only. Published timestamps are always UTC.

### Firmware Updates (OTA)

Observer builds update themselves from the release manifest on observer.gessaman.com.

| Command | Purpose |
|---------|---------|
| `ota check` | Report whether a newer build exists. Does not flash |
| `ota update` | Download and flash the newer build, then reboot (about 30 s offline) |
| `ota branch` | Show the selected channel, the channel this build came from, and the manifest URL |
| `ota branch prod` | Follow production (alias `stable`) |
| `ota branch beta` | Follow beta (alias `dev`) |
| `ota branch default` | Follow the channel this build came from |
| `start ota` | Serve a manual upload page (ElegantOTA) on the node's IP, or a `MeshCore-OTA` hotspot when offline. Stops after 15 minutes idle |
| `start ota ap` | Always use the `MeshCore-OTA` hotspot, e.g. on networks with client isolation |
| `stop ota` | Stop the manual upload page |

`ota branch` only saves the choice. Run `ota update` afterward to switch:

```bash
ota branch beta
ota check     # update available: v1.18.0.3 (...) -> v1.18.0.7 (...) (channel switch)
ota update
```

`ota check` replies:

- `up to date: <version>`
- `update available: <current> -> <new> (<n> behind | new base | channel switch)`
- `... [partition change: cable flash]`: the new build changes the partition table,
  which OTA can't write. Flash it by cable (see [Partition Table Changes](#partition-table-changes))

`ota update` refuses a channel switch to a build that can't read this node's saved
settings or lacks a transport the node uses (Ethernet). The reply says
`channel switch refused ... cable flash`. If [fault alerts](ALERTS.md) are on, the node
also announces the start of an update and any abort on the alert channel.

### Web Configuration Portal

The portal configures WiFi, radio, MQTT slots, and filters from a browser. It is off by
default on a configured node.

| Command | Purpose |
|---------|---------|
| `start webconfig` | Serve the portal on the node's LAN IP (admin password required). With no WiFi configured it raises the setup AP instead |
| `start webconfig ap` | Force the setup AP. Run `set bridge.enabled off` first when the bridge is on WiFi |
| `stop webconfig` | Stop the portal |

**Setup AP.** A node with no WiFi configured starts an open AP named
`MeshCore-Setup-XXXX` with a captive portal at `http://192.168.4.1/`. The display shows
both. The wizard steps through WiFi, radio, and MQTT, then saves and reboots. The AP stops
after 10 minutes with no client. Build with `-D WEBCONFIG_AP_PASSWORD='"..."'` to add WPA2.

**Security.** The setup AP has no login and sends credentials over plain HTTP, so
provision somewhere private and use LAN mode afterward. LAN mode uses the admin password,
a 20-minute idle session, and a 30-second lockout after five failed logins.

**ThinkNode M7 on Ethernet** with no WiFi configured opens the portal on the LAN at first
boot. The serial log and display show a one-time login code, and you must set a new
admin password.

**Applying changes.** MQTT settings apply live. Radio settings apply at reboot. Changing
WiFi saves and reboots; find the node's new IP on your router. If the portal is
unreachable, the serial CLI always works.

### Display

Observer builds with a screen:

| Command | Purpose | Default |
|---------|---------|---------|
| `get/set display.timeout <0-3600>` | Seconds before the screen blanks; `0` keeps it on | 60 |
| `get/set display.flip on\|off` | Rotate 180 degrees. Survives updates, so check it if a screen looks upside down | off |

Tap the panel or press USER to wake or blank the screen.

### Radio Watchdog

`get/set radio.watchdog <0-120>` (minutes, default 5, `0` disables). If the radio sees no
packet, interrupt, or successful TX for that long, the firmware restarts receive mode.
This recovers radios that stop reporting interrupts without a reboot. On meshes that are
silent for long stretches, raise the interval or set `0`.

### Bridge

| Command | Purpose |
|---------|---------|
| `get/set bridge.enabled on\|off` | Master switch for the bridge |
| `get/set bridge.source rx\|tx` | For RS232 and ESP-NOW bridges. For MQTT, use `mqtt.rx` / `mqtt.tx` |

### SNMP

`set snmp on|off` and `set snmp.community <string>` (default `public`) take effect after a
restart. See [MQTT_SNMP.md](MQTT_SNMP.md).

### Diagnostics

| Command | Shows |
|---------|-------|
| `get mqtt.status` | Each slot's state; with neighbors on, `nbr: <next>/<last>` |
| `get mqttN.diag` | Last error for slot N (TLS, socket, clock) and its filter |
| `get mqtt.stats` | Heap, queue depth, outbox, drops, `filt=`, `badsig=` |
| `get mqtt.config.valid` | `valid` when the saved MQTT settings pass validation |
| `memory` | Heap, internal RAM, largest free block, PSRAM |
| `tls.bundletest <host[:port]>` | Test a TLS handshake against the built-in certificate bundle |

## MQTT Topics

| Topic | Contents |
|-------|----------|
| `meshcore/{IATA}/{PUBLIC_KEY}/status` | Node status and stats. QoS 1, retained except on `meshrank` and `waev` |
| `meshcore/{IATA}/{PUBLIC_KEY}/packets` | Decoded packet with RF data |
| `meshcore/{IATA}/{PUBLIC_KEY}/raw` | Raw packet hex |
| `meshcore/{IATA}/{PUBLIC_KEY}/neighbors` | Zero-hop repeater neighbors with SNR, age, and scopes. QoS 0 |

`{PUBLIC_KEY}` is 64 hex characters. MeshRank uses `meshrank/uplink/{token}/{PUBLIC_KEY}/<type>`.

**Neighbors** publish after `discover.scopes`, or every `mqtt.neighbors.interval` hours
when `mqtt.neighbors` is on. A periodic run first refreshes the neighbor table for 60 s
(like `discover.neighbors`), then queries each neighbor's scopes. A manual
`discover.scopes` during a refresh waits for it and says so:
`OK - scopes queued (47s discovery remaining)`.

## JSON Message Formats

All timestamps are UTC with an explicit `+00:00` offset.

### Status

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

`stats` and its fields are omitted when no value is available. Packet counts are totals
since boot.

### Packet

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

- Numeric fields are JSON strings. `time` and `date` are UTC.
- `SNR`, `RSSI`, and `score` appear on RX packets only. `score` is the rebroadcast score
  ×1000 (0-1000), the same number the serial RX log prints.
- `path` appears on direct-route packets: one lowercase hex token per hop, matching
  [meshcore-packet-capture](https://github.com/agessaman/meshcore-packet-capture).

### Raw

```json
{
  "origin": "MeshCore-HOWL",
  "origin_id": "A1B2C3D4E5F67890...",
  "timestamp": "2024-01-01T12:00:00.000000+00:00",
  "type": "RAW",
  "data": "F5930103807E5F1E..."
}
```

### Neighbors

```json
{
  "timestamp": "2024-01-01T12:00:00.000000+00:00",
  "origin": "MeshCore-HOWL",
  "origin_id": "A1B2C3D4E5F67890...",
  "total_neighbors": 2,
  "queried_neighbors": 2,
  "truncated": false,
  "self": { "scopes": "DEN,APRS", "default_scope": "*" },
  "neighbors": [
    { "pubkey": "0011223344556677...", "snr": 9.75, "heard_secs_ago": 42,
      "scopes": "DEN,APRS", "status": "responded" },
    { "pubkey": "8899AABBCCDDEEFF...", "snr": 12.5, "heard_secs_ago": null,
      "scopes": "DEN", "status": "responded" }
  ]
}
```

- Entries are sorted by usefulness. The list stops early when the 10 KB buffer fills or
  the 20-entry cap applies; compare its length with `total_neighbors`.
- `queried_neighbors` counts scope requests actually sent. `status` is `responded`,
  `timeout`, or `send_failed`.
- `heard_secs_ago: null` means unknown, never zero: the neighbor was last heard before
  the clock was set. It usually clears on the next cycle.
- `self.default_scope` is the node's `region default`, or `*` when unset.

## Behavior

**Connections.** Each slot reconnects with exponential backoff, then drops to a slow
periodic retry. Packets queue while a slot is down. When the network route changes
(Ethernet to WiFi or back), every slot disconnects cleanly and retries at once on the new
route. Each medium keeps its own DHCP DNS servers.

**Ethernet (ThinkNode M7).** The observer uses Ethernet when it has a DHCP lease at boot,
otherwise WiFi, and switches at runtime. Without a cable, boot waits about 1 s for
Ethernet. The DHCP hostname is `meshcore-<node name>` on both.

**NTP.** JWT auth needs the correct time. The node syncs hourly from `mqtt.ntp`, then
`pool.ntp.org`, `time.google.com`, `time.cloudflare.com`, `time.aws.com`, and
`time.nist.gov`. Replies from unsynced servers, the wrong address, or with a mismatched
request stamp are ignored.

**Authentication** is fixed per preset: Ed25519 JWT (username `v1_{PUBLIC_KEY}`, renewed
before expiry), username/password (built in, or per slot), or none.

## Build Configuration

Observer environments are named `<board>_repeater_observer_mqtt` and
`<board>_room_server_observer_mqtt`. List them all with:

```bash
grep -rhoE '^\[env:[^]]*observer_mqtt\]' variants
pio run -e heltec_v4_repeater_observer_mqtt
```

The LilyGo T-LoRa V2.1-1.6 observer envs are parked (trailing `_` in the env name)
because the image no longer fits its 4 MB flash.

### Partition Table Changes

Some observer builds use a different partition table than the board's other MeshCore
firmware. **The first time you move to one of these, flash the merged image
(`*-merged.bin`)** so the bootloader and partition table are written too. OTA works
normally after that.

| Environment (repeater and room server) | Partition table | App slot |
|-------------|----------------|----------|
| `LilyGo_T3S3_sx1262_*_observer_mqtt` | `min_spiffs.csv` (4 MB) | 1.875 MB |
| `LilyGo_T3S3_sx1276_*_observer_mqtt` | `min_spiffs.csv` (4 MB) | 1.875 MB |
| `Ebyte_EoRa-S3_*_observer_mqtt` | `min_spiffs.csv` (4 MB) | 1.875 MB |
| `Station_G2_*_observer_mqtt` | `default_16MB.csv` | 6.25 MB |
| `Station_G3_ESP32_*_observer_mqtt` (both RF slots) | `default_16MB.csv` | 6.25 MB |
| `LilyGo_TBeam_1W_*_observer_mqtt` | `default_16MB.csv` | 6.25 MB |

**A partition change usually erases settings** (WiFi, MQTT, name, admin password), so plan
to reconfigure. A device coming from non-MeshCore firmware is always erased.

To flash: use the [web flasher](https://observer.gessaman.com/), whose Download menu
also has the merged, erase, and bootloader files, or:

```bash
pio run -t mergebin -e LilyGo_T3S3_sx1262_repeater_observer_mqtt
esptool.py write_flash 0x0 .pio/build/LilyGo_T3S3_sx1262_repeater_observer_mqtt/firmware-merged.bin
```

### Build Flags

| Flag | Purpose |
|------|---------|
| `WITH_MQTT_BRIDGE=1` | MQTT bridge (required) |
| `WITH_SNMP=1` | SNMP agent |
| `MQTT_DEBUG=1` | Debug logging |
| `MQTT_WIFI_TX_POWER` | WiFi TX power (default `WIFI_POWER_11dBm`) |

These set fresh-install defaults (see `src/helpers/MQTTDefaults.h`). They don't change a
node that already has `/mqtt.json`.

| Macro | Default |
|-------|---------|
| `MQTT_DEFAULT_SLOT1_PRESET` … `SLOT6_PRESET` | `analyzer-us`, `analyzer-eu`, then `none` |
| `MQTT_DEFAULT_IATA` | blank |
| `MQTT_DEFAULT_TIMEZONE` | blank |
| `MQTT_DEFAULT_TIMEZONE_OFFSET` | `0` |

```ini
build_flags =
  -D MQTT_DEFAULT_SLOT1_PRESET='"meshcore-ca-1"'
  -D MQTT_DEFAULT_SLOT2_PRESET='"meshcore-ca-2"'
  -D MQTT_DEFAULT_IATA='"YYZ"'
  -D MQTT_DEFAULT_TIMEZONE='"America/Toronto"'
```

WiFi credentials can't be set at build time.

## Upgrading Settings

The first boot after an update converts older settings formats, including the pre-slot
`mqtt.analyzer.us` / `mqtt.server` layout, and keeps brokers, origin, IATA, WiFi, and
timezone. Firmware old enough to predate the separate observer settings file is the
exception: re-enter MQTT and WiFi settings after that upgrade. Details are in
[MQTT_INTERNALS.md](MQTT_INTERNALS.md#settings-upgrade--migration).

## Troubleshooting

**No WiFi:**

```bash
get wifi.ssid
get link.status            # shows the disconnect reason
set wifi.powersave none
```

**No MQTT messages:**

```bash
get bridge.enabled         # must be on
get mqtt.rx                # must be on
get mqtt.iata              # must be set for meshcore/{iata}/... presets
get mqtt.status
get mqtt1.diag             # TLS, socket, or clock error for slot 1
get mqtt1.filter           # none sends no packets
```

**JWT brokers reject the login:** check the clock with `get mqtt.ntp.diag`.

## Fault Alerts

The node can post a message on a LoRa group channel when WiFi or a broker stays down. See
[ALERTS.md](ALERTS.md).

## Developer Documentation

Source layout, upstream integration points, settings file format, and local testing:
[MQTT_INTERNALS.md](MQTT_INTERNALS.md).
