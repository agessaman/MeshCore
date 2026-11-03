# Experimental Pico W MQTT observer

This target is for a **Raspberry Pi Pico W (RP2040) with the Waveshare
Pico-LoRa-SX1262 add-on**, using Wi-Fi. It is a software-tested, cross-compiled
candidate. It has not been booted on a physical board or connected to the
public broker services on this hardware.

The assumed wiring is the existing Pico W variant: SPI1 clock GP10, MOSI GP11,
MISO GP12, CS GP3, DIO1 GP20, reset GP15, BUSY GP2. It inherits DIO2 RF switching
and the 1.8 V DIO3 TCXO setting. Confirm the user's actual module against these
assumptions during first bring-up. The integrated Waveshare RP2040-LoRa board
is a different target.

## Build and install

```sh
pio run -e PicoW_repeater_observer_mqtt_experimental
```

The installable artifact is
`.pio/build/PicoW_repeater_observer_mqtt_experimental/firmware.uf2`.
Use the Pico's BOOTSEL USB drive installation procedure. Keep the user's
known-good firmware and settings backup available for recovery.

The target uses a 2 MiB flash layout with 0.5 MiB LittleFS, matching this
repository's Pico W variant. An existing image from another source may have a
different layout. The UF2 contains the application; it is not an ESP merged
binary or an OTA update.

The platform is pinned to `a1df30f0ad7cd1ac389d01701ac36ac917770be8`, Arduino-Pico
to `0affea4db5dbcd4a43568784acb4858c4786f1a9` (package `1.50401.0`), MQTT to
2.5.3, WebSockets to 2.7.2, and ArduinoJson to 7.4.3. The observed compiler/tool
package is `5.140200.250530` (GCC 14.2.0). The firmware identifies itself as
`v1.18.0-pico-dhcp2`.

## Configure over USB serial

Open a serial console at 115200 baud. Use the user's established frequency,
bandwidth, spreading factor, coding rate and TX-power settings, as on their
working Pico firmware. Factory radio defaults are not a substitute for those
settings.

Replace these placeholders with the user's values:

```text
set wifi.ssid <network-name>
set wifi.pwd <network-password>
set mqtt.iata <airport-code>
set mqtt1.preset analyzer-us
set bridge.enabled on
get wifi.status
get mqtt.status
get mqtt1.diag
get mqtt.stats
```

Use `analyzer-eu` or `meshmapper` instead when appropriate. Those presets use
WSS, the existing device identity, and the existing Ed25519 token format.
The transport supports them; real board/broker interoperability is still a
tester acceptance item.

`set repeat off` disables packet forwarding. Adverts and administrative replies
have their own controls, so this is not a promise of zero RF transmissions.

A local custom broker can be configured with:

```text
set mqtt1.preset custom
set mqtt1.server <broker-host>
set mqtt1.port 1883
```

Add `mqtt1.username` and `mqtt1.password` if required. Port 8883 selects TLS for a
plain hostname. Explicit `mqtt://`, `mqtts://`, `ws://` and `wss://` URLs are also
accepted in `mqtt1.server`, within the existing 63-character field limit. An
explicit port in the URL takes precedence; otherwise a nonzero `mqtt1.port`
overrides the scheme default. For a custom WS endpoint, include its exact path.

Secure presets use their configured CA. Custom secure endpoints and presets
without a dedicated CA use ISRG Root X1 plus GTS Root R4. Other private/custom
certificate authorities are not currently configurable. A chain outside this
trust set fails verification; the client never falls back to insecure TLS.
IPv6 literal custom endpoints are not supported by the initial URI parser.

Settings and identity use the existing LittleFS files and schema.
Only slot 1 connects. Slots 2–6 remain saved and are reported inactive.

## Wi-Fi troubleshooting

On the October 28 `v1.18.0-pico-dhcp2` candidate, `get wifi.status` and `get link.diag`
include a connection stage and raw Wi-Fi/driver codes. The previous output
`wifi down ip:(IP unset) rssi:0` did not establish whether association or DHCP
failed; it also appeared when the observer had never started.

Collect these replies after boot and again after about 90 seconds:

```text
get bridge.enabled
get wifi.ssid
get wifi.status
get mqtt.status
get mqtt.stats
```

- `not started`: the network worker has not initialized Wi-Fi. Check
  `bridge.enabled` and `mqtt.status`.
- `joining` / `join timeout`: Wi-Fi association or the WPA handshake has not
  completed. Check the 2.4 GHz network, SSID and WPA/WPA2 settings.
- `SSID not found` / `auth failed`: the driver reported a network lookup or
  authentication failure. Do not include the password in a tester report.
- `waiting DHCP` / `DHCP timeout`: association completed but no IPv4 lease has
  arrived. Check the router's DHCP service, address pool and access controls.
- `connected`: association and an IPv4 address are available; MQTT or NTP
  failures are reported separately by `mqtt.status`.

The candidate serializes Wi-Fi setup, observation and RSSI on the core's lwIP
task. The application initializes lwIP before starting the network worker; each
dispatch also ensures initialization before queueing work. The first October 28
DHCP candidate omitted this initialization and was reported to lose serial and
stop responding after flashing. Do not use that candidate. BOOTSEL can reload
the previous image without a filesystem erase.

An environment-local CYW43 patch issues one asynchronous join instead of
waiting inside `beginNoBlock()`. The worker allows 30 seconds for association
and, once associated, 60 seconds for DHCP before retrying with backoff. RSSI is
sampled every five seconds. These changes have host/build validation; physical
Pico W DHCP and broker acceptance remain tester checks.

Regression checks:

```sh
pio test -e native_pico_wifi
pio run -e PicoW_repeater_observer_mqtt_experimental
python3 scripts/test_pico_observer_cyw43.py
```

The Wi-Fi suite exercises the actual network-link implementation with SDK
doubles. The last command compiles the generated CYW43 association method with
sanitizers and a stalled-handshake double, and checks open authentication and
immediate join rejection. Neither check substitutes for a physical connection.

## Operation and limits

- The application task on core 0 owns the SX1262/SPI bus, packet pool, ADC,
  hardware RTC and LittleFS. It sends copies to the network worker on core 1.
- The worker manages Wi-Fi/DNS/NTP, MQTT/TLS/WS, token generation and JSON
  serialization. Wi-Fi control and observations execute through the core's
  lwIP task on core 0. No network operation runs in the radio capture callback.
- The capture queue holds six copied items, plus at most one item being
  published/retried. Overflow evicts the oldest queued item and increments
  `drop`. Failed best-effort sends use the existing bounded retry policy.
- RF continues before first validated NTP time; these events increment
  `time_drop` instead of being published with a fabricated wall-clock timestamp.
  Queue expiry uses monotonic time; messages retain the existing UTC publish-time
  format.
- `get mqtt.stats` reports successful library publishes, errors, drops, filtered
  events, time drops, sampled minimum heap and worker stack high-water. QoS 0
  success means the transport accepted the bytes; it does not prove broker
  receipt. Status uses QoS 1 and the preset's retain policy.
- Only Wi-Fi, slot 1 endpoint/credential/token/topic, IATA, owner and email
  changes reconnect MQTT and discard queued packets. Other observer settings
  (publish toggles, filters, status interval, radio parameters) apply in place.
- `get mqtt.ntp.diag` reports cached time state and configured primary; active
  probe-only diagnostics are unavailable. Changing `mqtt.ntp` schedules
  synchronization on the worker.
- Start/stop is cooperative. The CLI/radio keeps running while a slow operation
  finishes. A requested stop becomes unproven after 60 seconds; resources are
  retained until the worker acknowledges. A subsequent start resumes only after
  that acknowledgment.
- The application and worker each have 16 KiB stacks in this profile. The
  application stack is raised from the pinned core's 4 KiB default to accommodate
  observer settings and snapshots; actual headroom still needs physical measurement.
- Pico W TX LED blinks are disabled because the built-in LED uses the Wi-Fi chip.
  Ordinary Pico targets retain their previous LED behavior.
- Web configuration/AP setup, BLE, SNMP, automatic fault alerts, neighbor-table
  publication, room-server observer and OTA/channel switching are unavailable.
  Basic mesh neighbor/admin behavior retains a ten-entry cache.

The build applies environment-local copies of pinned sources through
`scripts/pico_observer_transport.py`. The changes initialize the upstream WS
client before first stop, enforce TLS 1.2, require an echoed MQTT subprotocol,
bound HTTP header lines/deadlines and frame read time, increase the Pico
application stack, and make CYW43 association asynchronous. Shared framework
packages are not rewritten.

## Validation and tester handoff

The new backend/stream tests use deterministic socket and SDK doubles. They cover
credential clearing, CA ownership/failure, TLS selection, QoS/retain forwarding,
fragmentation/coalescing, overflow, bad subprotocol/text frames and suppressed
automatic reconnect. Sanitizers exercise the actual adapters with those doubles;
they do not emulate the radio, TLS engine or Wi-Fi driver.

The build-only workflow uploads an experimental UF2 and checksum. The environment
name is excluded from existing observer release enumeration. No stable/beta flasher
entry or automatic-update manifest is added.

The remote tester should record:

1. Pico W and module revision/band, pin differences, UF2 checksum and boot output.
2. RF reception versus the known-good image on identical settings: at least 1,000
   numbered packets at a controlled low rate, targeting 99% capture versus the
   reference. Record uplink queue drops separately.
3. Successful LetsMesh/MeshMapper connection, accepted identity, topics and
   packet/status/raw output.
4. Wrong credentials, unavailable DNS/NTP, AP loss/recovery and certificate
   rejection, including serial/admin and radio responsiveness.
5. At least 100 reconnect/reconfigure cycles and a 48-hour soak with natural token
   renewal. Record memory/stack/queue trends and unexplained resets.
6. Settings persistence, RF interruptions during saves, and BOOTSEL recovery.

The detailed gates are in [the implementation plan](pico-w-mqtt-implementation-plan.md).
No on-air, runtime-memory or endurance claim is established by compilation.
