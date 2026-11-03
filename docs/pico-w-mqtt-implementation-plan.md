# Pico W MQTT observer implementation plan

Status: experimental software candidate implemented; physical board acceptance pending.
Date: 2026-10-26.
Branch: `feat/pico-w-mqtt`.
Base: `observer-firmware-dev` at `ca8004187065a7b5419494cd2495bac0b0153cb1`.

## 1. Target and assumptions

Build a standalone Wi-Fi MQTT observer for a **Raspberry Pi Pico W (RP2040)
with the Waveshare Pico-LoRa-SX1262 add-on**. The user identified the flasher
entry as **RPI Pico 2040 + WaveShare SX1262** and confirmed that the requesting
user has a Pico W using Wi-Fi. Treat that combination as the development target.
The flasher label alone does not establish that a board has Wi-Fi.

Use `variants/rpi_picow`, `board = rpipicow`, and the Earle Philhower Arduino-Pico
core. Do not use `variants/waveshare_rp2040_lora`: that describes the integrated
Waveshare RP2040-LoRa-HF/LF board, has a different pin map, and selects `board = pico`.
Pico 2 W/RP2350, bare Pico without Wi-Fi, and other SX1262 modules are outside this
initial target.

The existing Pico W definitions match the manufacturer's Pico-LoRa-SX1262 pin
table. All pin numbers below are GPIO numbers, not physical header positions:

| Signal | GPIO / setting | Basis |
| --- | --- | --- |
| SPI clock | GP10, SPI1 | Existing variant and Waveshare pin table |
| MOSI / MISO | GP11 / GP12 | Existing variant and Waveshare pin table |
| CS / NSS | GP3 | Existing variant and Waveshare pin table |
| DIO1 | GP20 | Existing variant and Waveshare pin table |
| Reset | GP15 | Existing variant and Waveshare pin table |
| BUSY | GP2 | Existing variant and Waveshare pin table |
| Battery ADC | GP26 | Existing board helper and Waveshare pin table |
| RF switch | SX1262 DIO2 | Inherited variant assumption; verify on board |
| TCXO voltage | 1.8 V via DIO3 | Inherited variant assumption; verify on board |
| Current limit / maximum configured TX power | 130 mA / 22 dBm | Inherited variant; retain existing regional/operator settings |

No further wiring information is required to implement against this assumption.
Record the actual module revision, frequency band, and any wiring differences
when the remote tester first tries the build. Keep pin and oscillator overrides
in the variant so a correction does not require redesigning the observer.

Neither the project owner nor the implementer has this hardware locally. Separate
host-tested, cross-compiled, and remote-board-tested results throughout the work.

## 2. Release scope

The first supported release is a repeater-role observer with a constrained network
profile. It must be useful with the existing observer services, including their
TLS and authentication requirements.

| Capability | First release | Later work |
| --- | --- | --- |
| SX1262 capture and existing repeater/admin behavior | Required | Additional board variants |
| Wi-Fi station, DHCP, hostname, DNS, reconnect | Required | AP provisioning and other network media |
| MQTT 3.1.1 over TCP, TLS, WS, WSS | Required transport coverage | Broader broker certification |
| LetsMesh US/EU and MeshMapper presets | Required WSS/JWT validation | Remaining presets verified individually |
| Slot 1 connection | Required, one live connection | Additional simultaneous brokers after memory qualification |
| Slots 2–6 settings | Preserve; report inactive on Pico | Runtime activation when supported |
| Packet/status/raw messages, topics, filters, origin/IATA | Required | Neighbor-table publication |
| Device identity and existing signed authentication format | Required | None needed for basic compatibility |
| USB serial configuration and diagnostics | Required | Browser configuration |
| LittleFS settings and restart recovery | Required | Optional export/import tools |
| Radio watchdog and existing mesh controls | Retain; verify | Broader RF qualification |
| SNMP, neighbor discovery/publication, fault-alert delivery | Explicitly unavailable in initial Pico profile | Separate implementation/qualification stages |
| HTTP OTA, automatic updates, channel switching | Explicitly unavailable | Separate RP2040 update design |
| Room-server observer, BLE, display, external sensors | Outside first release | Separate targets after repeater qualification |

Preserve the existing six-slot settings schema. Only slot 1 owns a client on Pico;
inactive slots must never consume a TLS context or accidentally become active
after reconfiguration. Fresh Pico settings should set slot 2 through slot 6 to
`none`, while loading an existing file preserves all stored values. Do not
silently translate one preset into another or downgrade a secure endpoint.

Keep the existing repeater role and RF configuration semantics. Document
`set repeat off` for operators who want forwarding disabled; do not describe this
as eliminating every possible transmission, since adverts/admin replies have
their own controls. First bring-up should use the tester's established RF settings.

## 3. Current implementation and porting boundaries

The source audit found these concrete dependencies:

| Owning source | Current dependency | Planned change |
| --- | --- | --- |
| `platformio.ini`, `variants/rpi_picow/*` | Existing RP2040 radio/core/LittleFS support | Add isolated experimental observer environment and pin its toolchain |
| `src/helpers/bridges/MQTTBridge.{h,cpp}` | Direct `PsychicMqttClient`, `esp_err_t`, ESP MQTT config/errors, ESP task/heap APIs | Introduce portable client and worker seams; preserve shared observer policy |
| `lib/PsychicMqttClient/*` | ESP-IDF MQTT, TLS and event tasks | Keep as the ESP backend; exclude from Pico discovery/build |
| `src/helpers/NetworkLink.{h,cpp}` | Interface and implementation gated by `ESP_PLATFORM` | Expose a platform-neutral interface; add Pico Wi-Fi implementation |
| `src/helpers/JWTHelper.cpp` | Portable Ed25519 identity/signing plus `mbedtls/base64.h` | Replace only base64 dependency with bounded portable encoding |
| `src/helpers/CommonCLI_Observer.cpp` | ESP Wi-Fi power, bundle diagnostics and OTA operations | Route supported commands through capabilities and platform services |
| `examples/simple_repeater/MyMesh.{h,cpp}`, `main.cpp` | Bridge wiring and live mesh/radio/RTC reads from network work | Keep mesh ownership; publish snapshots and queue work |
| `src/helpers/AlertReporter.*`, `SNMPAgent.*`, `esp32/WebConfigServer.*` | Feature coupling, network/status access and platform code | Make optional services explicit and absent from initial Pico runtime |
| `MQTTPrefs*`, `MQTTDefaults.h`, `CommonCLI.*` | Six-slot schema, recovery/migration, ESP defaults | Preserve schema/recovery; apply Pico defaults/capabilities separately |
| `build.sh`, observer workflows and flasher metadata | Mixed artifact generation; observer publishing gathers `.bin` files | Add build-only UF2 artifacts before considering release publication |

The non-ESP circular queue and `MQTTBridge::loop()` are scaffolding, not evidence
of a working portable observer. The header still exposes ESP types and the network
interface is absent on Pico. Do not fake `ESP_PLATFORM` to reach those code paths.
Existing internal design documents contain historical sections; resolve conflicts
against the code at the base revision.

## 4. Architecture decisions

### 4.1 Shared observer policy with two MQTT backends

Keep packet formatting, topic routing, preset lookup, effective configuration,
backoff, JWT renewal, filtering and lifecycle decisions shared. Do not duplicate
the 5,000-line bridge for Pico or redesign the ESP backend in this port.

Introduce a narrow `ObserverMqttClient` interface with owned configuration, connect,
service/poll, publish, disconnect/stop, connected state, normalized errors, and
capabilities. Represent completion semantics explicitly: accepted locally,
acknowledged by the broker, failed, or unknown after transport loss. An ESP enqueue
result and a synchronous Pico PUBACK must not be treated as identical guarantees.
Use generation IDs to reject events/results from previous configurations.

An `EspObserverMqttClient` adapter retains the vendored Psychic client and its
current ownership, stop/quarantine, outbox and TLS behavior. A
`PicoObserverMqttClient` implements the same external contract without including
ESP headers. Move SDK configuration and diagnostics into the ESP adapter; avoid
exposing an ESP config struct through the common interface.

Preferred Pico stack: **256dpi/MQTT (`arduino-mqtt`/lwmqtt)** over Arduino `Client`,
with Pico `WiFiClient`/BearSSL `WiFiClientSecure` for TCP/TLS and a bounded
WebSocket-to-`Client` adapter for WS/WSS using **Links2004/arduinoWebSockets**.
The upstream sources expose QoS/retain support, custom `Client` transport, and
RP2040/BearSSL WebSockets. These are implementation candidates, not yet a proven
combination. Pin exact revisions only after milestone 0 compiles the combination.
Keep dependencies confined to the Pico observer environment.

The transport spike must prove:

- MQTT HTTP upgrade uses the URL's exact host/port/path/query, SNI and `mqtt`
  WebSocket subprotocol. Validate the upgrade and reject unsupported schemes.
- Arbitrary MQTT read/write boundaries work across binary WebSocket messages and
  continuation frames; several MQTT packets in one incoming message also work.
  Pump WS control traffic while MQTT waits for CONNACK/PUBACK/PINGRESP.
- Ping/pong, close, bounded RX buffering, partial writes and disconnects cannot
  recurse into MQTT callbacks or cause an unbounded allocation. Reject unexpected
  text frames, oversized input and buffer overflow cleanly.
- Maximum JWT CONNECT, maximum topic, 2 KiB JSON payloads, QoS 0 and QoS 1, retain,
  keepalive, will configuration where used, and broker refusal codes are supported.
- Disable library-owned reconnect loops: the bridge remains the single reconnect
  owner. If the WS library cannot expose a clean stopped state, use a small pinned
  patch/adapter and record it; do not introduce a second independent retry loop.
- Trust anchors have explicit lifetimes. Use owned BearSSL objects and the pointer
  overload rather than repeatedly allocating certificates with a PEM convenience
  overload whose ownership has not been established.

If this combination cannot meet bounded shutdown, memory or verified TLS
requirements, stop integration at the spike and document/select a replacement
transport. TCP-only success does not satisfy the first-release preset requirement.

Proposed new source boundaries (final names can follow local conventions):

| File / directory | Responsibility |
| --- | --- |
| `src/helpers/ObserverCapabilities.h` | Platform feature and runtime-slot limits |
| `src/helpers/mqtt/ObserverMqttClient.h` | Client contract and portable results/errors |
| `src/helpers/mqtt/EspObserverMqttClient.{h,cpp}` | Psychic/ESP-IDF adapter |
| `src/helpers/mqtt/PicoObserverMqttClient.{h,cpp}` | Pico MQTT and transport selection |
| `src/helpers/mqtt/WebSocketStreamClient.{h,cpp}` | Bounded binary WS stream adapter |
| `src/helpers/ObserverWorker.{h,cpp}` | Worker/queue/stop platform boundary |
| `src/helpers/ObserverSnapshots.h` | Owned config, capture, health and command/result data |
| `src/helpers/rp2040/PicoWifiNetworkLink.cpp` | Pico implementation of `NetworkLink` |
| `test/test_observer_mqtt_backend/`, `test/test_websocket_stream/` | Backend and transport contract coverage |

Add these directories explicitly to the affected PlatformIO source filters;
`helpers/*.cpp` does not include nested implementation directories automatically.

### 4.2 Task and resource ownership

Enable Arduino-Pico FreeRTOS SMP **only for the new observer environment** using
`PIO_FRAMEWORK_ARDUINO_ENABLE_FREERTOS`. Leave existing Pico targets unchanged.
Start with the existing mesh application/USB CLI in the Arduino task on core 0,
and a single observer worker on core 1. Establish affinity using the pinned core's
actual FreeRTOS API. This assignment differs from ESP's existing task placement;
express ownership by task role, not assumptions that “Core 0 means MQTT.”

| Owner | Exclusive resources | Communication |
| --- | --- | --- |
| Mesh/application task | Radio/SPI1, dispatcher, packet pool, mutable identity, hardware RTC/I2C, ADC, settings persistence | Copies captured packets and sampled status; receives commands/results |
| Observer worker | Wi-Fi control, DNS, UDP/NTP socket, MQTT/WS/TLS client, serialization scratch, immutable applied config | Bounded command/capture queues; copied status/results |
| SDK/lwIP tasks | Core-managed driver resources | Only through documented driver APIs |

Use zero-wait capture enqueue from mesh callbacks. Store a full packet/raw-byte
copy plus RSSI/SNR, capture monotonic time, and score/other needed radio metadata;
never retain pool pointers. The network worker must not query SPI, I2C, ADC,
`_mgr`, or mutable dispatcher/radio state to construct a publish. Reuse the
observer's RX-reserve and queue-age/retry policies.

Apply configuration as an immutable snapshot delivered by value/owned storage.
The worker must not read `_obs`, `NodePrefs`, node name, IATA arrays or mutable
identity fields while the CLI is changing them. Copy identity once after load;
if identity import changes it, stop/join, install a new owned copy and restart.
Never log private keys, Wi-Fi passwords, JWTs or broker credentials.

Return status through bounded copies with a short mutex/critical section or a
proven snapshot protocol. Do not use `volatile` for inter-task ownership, assume
aligned unsynchronized reads are sufficient, or rely on lock-free 64-bit atomics
on Cortex-M0+. Audit existing 64-bit outage snapshots and atomics at compile/link.
Keep locks outside DNS/TLS/socket/file operations and never hold a scheduler lock
around a blocking connection attempt.

All LittleFS operations stay on the application task. NTP sets the worker's
validated system-time state and sends the application task an RTC update request;
the worker does not call an autodiscovered hardware RTC over Wire. Preserve the
existing source/nonce/epoch validation and use monotonic deadlines for retries.

Audit board LED access: Pico W's built-in LED belongs to the CYW43 chip. Existing
radio TX callbacks call `digitalWrite(LED_BUILTIN)`. Disable those TX blinks in the
initial observer profile or marshal LED requests to the network owner; do not let
an apparently local LED operation violate the driver ownership design.

### 4.3 Blocking, deadlines and lifecycle

TLS can block for seconds on Pico. It runs only on the observer worker and yields
to the SDK. Configure separate bounded DNS, TCP, TLS, upgrade, MQTT ACK and close
deadlines; document the actual library limits. Proposed starting limits are
5 s DNS/TCP, 15 s TLS, 5 s upgrade, and 2 s MQTT ACK. Confirm each bound in the
pinned libraries and measure the combined worst case. If DNS lacks a bounded API,
implement a cancellable resolver path before claiming bounded stop.

Use a platform worker adapter for creation, byte-to-`StackType_t` stack sizing,
queue operations and stop acknowledgment. ESP stack-depth units differ from
standard FreeRTOS words: converting the existing `8192` blindly could allocate
32 KiB. Start Pico's worker at 12 KiB, measure JWT/TLS stack usage, and adjust.

Retain the shared `MQTTLifecycle` state machine. Stop prevents new work, finishes
or times out the active transport operation, closes its client on the owning
worker, and acknowledges before release/restart. Set the stop budget from the
verified operation bounds with margin. A timeout leaves resources quarantined;
never force-delete a task inside TLS and then free its state. Serialize CLI
reconfiguration and NTP requests with request IDs; the application keeps servicing
radio while results are pending. Define bounded queue-full/busy replies.

The Pico watchdog must distinguish a failed radio from a slow network handshake.
Test startup, missing credentials, disabled bridge, allocation failure,
stop/start, reconfigure during connection, loss of AP, and late completion.

### 4.4 TLS, authentication and time

Preserve the existing broker-specific token format, including uppercase device
username and hex Ed25519 signature. This firmware's token format must not be
silently replaced with a generic JWT library's base64 signature. Add fixed-vector
and maximum-field-size tests before replacing the mbedTLS base64 call.

Require validated time before authenticated/secure broker connections. Reuse
`NtpValidation`/connection policy and the existing NTP server list, nonce and
source checks. Keep RF capture/admin available when NTP or DNS is unavailable;
report why MQTT is waiting. Initial Pico policy: continue RF processing before
first validated time, but count/drop those captures from the uplink rather than
publishing fabricated wall-clock values. Once time is valid, preserve the existing
builders' publish-time UTC semantics; use monotonic capture age for queue expiry.
Keep retry/age calculations independent of wall-clock corrections. Test initial
sync, invalidation, clock jumps, and millis rollover with an explicit time-drop
counter; do not claim lossless capture-to-broker delivery during startup.

Use CA-chain, hostname and date verification on TLS/WSS. Select the preset's
existing CA or a curated Pico trust store for secure presets without a dedicated
CA and custom secure brokers. If trust is unavailable, report unsupported/missing
trust and refuse that secure connection. Do not inherit the WS library's insecure
fallback when passed no CA. Preserve hostname/SNI through DNS resolution, and
set TLS 1.2 minimum. Certificate rotation is an operational consideration for the
trust store; do not pin a short-lived leaf certificate.

Keep default TLS receive capacity unless the peer's smaller-fragment behavior is
proven/negotiated. An MQTT packet cap does not make a smaller TLS record buffer
safe. The ESP certificate bundle format and mbedTLS footprint settings do not
apply to BearSSL.

### 4.5 Memory, persistence and capabilities

Use explicit Pico capabilities instead of treating every non-PSRAM board as an
ESP with two active connections. Preserve all persisted slots and schema version;
represent slot 2–6 as inactive due to platform capability and never index a
one-slot runtime array using a settings-slot index.

Start with six capture items, one 2 KiB publish scratch buffer, a bounded JSON
allocator, one transport and one auth token. Size MQTT buffers from actual CONNECT
and topic maxima; 896 bytes is an existing ESP allocation, not a universal Pico
limit. Bound WS receive storage independently of the JSON publish limit. Prefer
stable allocations and reuse over reallocating scratch on every publish.

Preserve separate `/prefs.json`, `/mqtt.json`, and identity files, atomic-store
recovery, unknown-version retention, and rollback after failed saves. Apply
worker configuration only after the application has accepted/persisted a change,
or keep an explicit staged/committed transaction if the existing CLI needs it.
Do not write flash from the observer worker. Audit Arduino-Pico flash programming
and multicore/RTOS coordination: even correctly owned LittleFS writes may pause
the other core. Measure save-time RF gaps on hardware and avoid routine flash
writes in the capture path.

Initial engineering budgets, to be validated rather than claimed as measured:

| Item | Starting budget / acceptance target |
| --- | --- |
| Physical SRAM | 264 KiB total |
| Wi-Fi/lwIP | Core documentation estimates about 40 KB; measure pinned build |
| Observer worker stack | 12 KiB initially; at least 25% unused at observed worst case |
| Capture queue | Six complete items; report actual `sizeof` and high-water |
| JSON serialization | 2 KiB output plus a separately bounded document pool |
| TLS/WS/MQTT | Measure resident and handshake peak separately, one connection |
| Dynamic headroom | At least 32 KiB free at measured peak after full startup/config load |
| Flash | Fit code, CYW43 firmware, trust anchors and 0.5 MiB LittleFS within 2 MiB |

Record `.data`/`.bss`, link-map totals, actual allocator pools, minimum free heap,
allocation failures, task stack high-water and queue drops. Audit whether the
pinned RTOS uses the C heap before summing diagnostics; a config heap constant is
not proof of a separate reserved pool. Do not label total free heap as largest
contiguous allocation. If the core has no valid largest-block metric, expose that
as unavailable and test relevant allocation failures instead.

Unsupported services must have accurate CLI/help/status replies. Do not report
that SNMP, web configuration, alerts, OTA or ESP-specific Wi-Fi power controls
have taken effect when absent. Keep saved cross-platform values intact. Map
`wifi.powersave`/`wifi.txpower` only if Pico semantics and units are verified;
otherwise report unsupported when setting them. Clear error categories should
separate Wi-Fi, DNS, time, TLS, HTTP upgrade, MQTT refusal and resource limits.

## 5. Implementation sequence and completion gates

### Milestone 0: reproducible target and transport spike

1. Capture base/Pico core/platform/compiler/library revisions. The repository's
   RP2040 platform URL is unpinned; the local installed Arduino-Pico package is
   `1.50401.0`. Current online documentation describes newer behavior, so verify
   APIs against the chosen pinned source. Pin the Pico observer independently;
   avoid changing every upstream RP2040 environment as a side effect.
2. Cross-compile the existing `PicoW_repeater` baseline and retain size/map output.
3. Add `PicoW_repeater_observer_mqtt_experimental`, extending `rpi_picow` with
   observer sources/dependencies, FreeRTOS, one broker and optional services off.
   Add `PsychicMqttClient` to `lib_ignore` for Pico and verify library discovery.
   Do not carry ESP linker wrappers, certificate scripts or OTA dependencies into it.
4. Use an isolated diagnostic environment to compile the selected MQTT/WS/TLS
   stack, CA lifetime handling and worker adapter. Exercise protocol code on host
   with a local broker/WS proxy where possible; leave Pico TLS timing explicitly
   pending. Pin dependency commits, licenses and any minimal patches.

Gate: baseline compiles, experimental dependency/link graph is ESP-free, UF2
generation is identified, and the transport choice has no unresolved architectural
blocker. Record failures before widening integration.

### Milestone 1: portable seams with ESP regression coverage

1. Add the client/result/error/capability and worker abstractions; wrap the current
   ESP backend without changing its retry/teardown behavior.
2. Make `NetworkLink`'s public interface portable and isolate the ESP implementation.
3. Extract a shared observer service step that the appropriate worker invokes.
   Keep one scheduling path per platform; retire the misleading synchronous Pico
   fallback once the worker is wired.
4. Add portable JWT base64 and byte-for-byte vectors. Gate optional services and
   move ESP diagnostics/config details behind their owning adapters.

Gate: existing host suites pass; representative ESP32 non-PSRAM, PSRAM and
Ethernet observer builds compile; ordinary Pico/non-observer builds still compile.
No change to persisted format, broker payloads, or ESP shutdown guarantees.

### Milestone 2: Pico runtime and Wi-Fi integration

1. Implement Pico `NetworkLink`, worker, queues, copied configuration/status, and
   single-owner storage/RTC/LED behavior described above.
2. Implement Wi-Fi station/DHCP/hostname/reconnect with nonblocking application
   commands; missing credentials leave an operational serial CLI.
3. Wire NTP/time validation, single runtime slot, UART/USB diagnostics and
   capability-aware CLI. Preserve six-slot storage and load/save recovery.
4. Add allocation-failure handling, bounded queue-full behavior and stop/restart.

Gate: experimental firmware links within flash/static RAM budgets; host contract
tests cover queue/config/lifecycle boundaries; all Pico observer CLI operations
are either implemented or accurately marked unavailable.

### Milestone 3: observer protocol compatibility

1. Integrate TCP/TLS and WS/WSS transports with normalized success/error behavior,
   broker-policy backoff, circuit breaking, keepalive and token renewal.
2. Reuse existing packet/status/raw builders and topic/filter policies; snapshot
   radio/board statistics on the application task. Verify max-sized output is
   complete and overflow is reported/dropped, never truncated into valid-looking JSON.
3. Exercise local broker/WS-proxy scenarios and fixed packet/token fixtures.
   Keep external broker testing to a dedicated test identity/account.
4. Validate no client/token/task/certificate growth over repeated reconfiguration.

Gate: protocol/contract tests pass, transport buffering and QoS behavior are
documented, and a tester-ready UF2 has reproducible provenance. Mark it
cross-compiled/experimental until board tests are complete.

### Milestone 4: remote board acceptance

Provide a versioned UF2, SHA-256, exact assumed pin map, serial setup instructions,
known limitations and a results template. Preserve a known-good Pico build for
recovery and tell the tester whether any flash-layout change requires backup.
Use BOOTSEL/UF2 installation; do not promise ESP Web Serial flashing or Pico OTA.

Collect the following evidence from the remote tester:

| Test | Required evidence / acceptance |
| --- | --- |
| Boot and recovery | Boots with/without an open serial terminal; BOOTSEL restores known-good build |
| RF baseline | Radio initializes; comparison with established Pico firmware on identical RF settings |
| Wi-Fi | Correct/wrong/missing credentials; AP loss/recovery, DHCP renewal and DNS failure are diagnosed |
| Time | NTP startup/failure/recovery; invalid replies cannot set clock or mint a supposedly valid token |
| Broker interoperability | LetsMesh and MeshMapper WSS plus controlled TCP/TLS; accepted identity/topics/payloads |
| TLS failures | Wrong CA/hostname/expired certificate rejected; no insecure fallback |
| Capture under reconnect | Controlled sequence-numbered RF traffic during TLS reconnect; radio loop remains responsive |
| Packet queue | Bounded RAM and explicit overflow/stale-drop counts; no crash under overload |
| Serial/admin | Commands remain responsive during slow network operations; settings survive reboot |
| Lifecycle | At least 100 connect/disconnect/reconfigure cycles with no accumulating tasks/clients or heap loss |
| Token renewal | Natural renewal observed during soak; isolated shortened-lifetime test covers failure/retry |
| Endurance | At least 48 h with controlled outages, periodic heap/stack/queue samples and no unexplained reset |
| Flash saves | Settings-save timing/RF interruption measured; identity and files remain readable after restart |

For a controlled low-rate RF test, target at least 99% capture versus the known-good
reference over at least 1,000 numbered packets. Distinguish radio capture loss
from intentional bounded uplink queue drops during outages; network downtime
does not imply lossless MQTT buffering. Agree any changed acceptance threshold
from measured evidence rather than hiding loss. Hardware remains a release gate,
not a prerequisite for completing milestones 0–3.

### Milestone 5: release packaging and documentation

The experimental environment name deliberately does **not** end in
`observer_mqtt`; current observer workflows enumerate only that suffix and their
publishing step copies `.bin` files. Add a build-only CI job for the experimental
UF2, with no publish credentials or deployment. Do not let a new environment
silently enter the existing ESP publishing path.

After acceptance, add RP2040-aware release handling for UF2 family IDs, filenames,
checksums and compatible filesystem layout. Inspect `build.sh`'s optional merged
binary and nRF HEX conversion paths; neither is an RP2040 install procedure.
Update flasher/download metadata to show **Pico W + Waveshare Pico-LoRa-SX1262**,
UF2 instructions and capability limits. Review beta/stable channel workflows and
their artifact parsers together. Promote the canonical
`PicoW_repeater_observer_mqtt` name only when this packaging is ready.

Update `MQTT_IMPLEMENTATION.md`, `MQTT_INTERNALS.md`, Pico variant comments and
the supported-board table. Include fresh-device serial setup, supported commands,
one-slot behavior, broker examples, backup/recovery and known validation limits.
Keep OTA capabilities false until a separate updater exists. Publishing release
artifacts/flasher changes is a distinct deployment step after review.

## 6. Test plan and regression matrix

Reuse existing native suites for MQTT lifecycle/client state/effective config,
connection policy, runtime buffers, payloads/topics/filtering, queue policy,
presets/replies, NTP/network policy, observer validation, and settings codecs,
serialization, atomic-store and recovery. Add tests for actual new boundaries:

- Backend contract tests for configuration clearing, generation rejection,
  publish completion, stop failure and restart eligibility.
- WS stream fragmentation/coalescing, control frames, binary-only validation,
  timeout/overflow, read/write boundaries and callback reentrancy.
- Pico capability-to-CLI behavior and all six persisted-slot indices against a
  single runtime client, including restoration from existing observer settings.
- Immutable configuration/status handoff and full capture value-copy lifetime;
  host sanitizer runs where the actual shared handoff code can be exercised.
- Exact authentication vectors, JSON maxima, valid/invalid NTP and capture-time
  correction behavior, certificate/trust selection, and allocation failure.
- Worker stack-size conversion, queue saturation, cancellation/stop acknowledgment
  and settings save/reconfigure transaction failures.

Build matrix: existing `PicoW_repeater`, new experimental Pico observer,
`PicoW_companion_radio_wifi`, `waveshare_rp2040_lora_repeater`,
`Heltec_v3_repeater_observer_mqtt` (non-PSRAM),
`heltec_v4_repeater_observer_mqtt` (PSRAM),
`ThinkNode_M7_repeater_observer_mqtt` (Ethernet), and `RAK_4631_repeater` (nRF52).
Include `Heltec_v3_room_server_observer_mqtt` for shared room-server integration.
These case-sensitive names were checked against the base variant definitions.

Expected implementation validation commands (new environment does not exist yet):

```sh
pio test -e native
pio test -e native_kiss_modem
pio test -e native_ota_channel
pio run -e PicoW_repeater
pio run -e PicoW_repeater_observer_mqtt_experimental
pio run -e PicoW_companion_radio_wifi
pio run -e waveshare_rp2040_lora_repeater
pio run -e Heltec_v3_repeater_observer_mqtt
pio run -e heltec_v4_repeater_observer_mqtt
pio run -e ThinkNode_M7_repeater_observer_mqtt
pio run -e RAK_4631_repeater
pio run -e Heltec_v3_room_server_observer_mqtt
```

Run targeted suites per implementation commit, then the full native suites and
cross-platform matrix before handoff. Host protocol tests cannot certify the Pico
Wi-Fi/BearSSL driver, RF reception, flash timing or physical power stability.

## 7. Suggested commit sequence

1. `build(pico): pin experimental observer toolchain and transport spike`
2. `refactor(mqtt): isolate client backend and normalized transport results`
3. `refactor(observer): isolate worker and platform capabilities`
4. `feat(pico): add Wi-Fi worker and owned configuration/status exchange`
5. `feat(pico): add verified MQTT TLS and WebSocket transports`
6. `feat(pico): wire observer capture, authentication and serial settings`
7. `test(pico): cover transport failures, lifecycle and cross-platform builds`
8. `docs(pico): add remote acceptance evidence and operating instructions`
9. `build(observer): publish qualified Pico UF2 artifacts` (after board acceptance)

Each commit must build its affected targets; adjust boundaries if an abstraction
requires an adapter and call-site change together. Keep exploratory transport
sketches outside production source filters and remove dead spike code before review.

## 8. Later feature stages

These stages have explicit scope so first-release omissions are visible:

- **More brokers:** qualify a second connection at simultaneous-handshake peak;
  extend runtime selection without changing the six-slot on-disk schema. Repeat
  outage/token-renewal and 48 h memory tests before raising the cap.
- **SNMP and alerts:** portable memory/health snapshots, a worker-owned UDP agent,
  and application-owned mesh alert sends. Retest congestion, rate limits and
  startup/fault behavior; retain current alert policy and secret handling.
- **Neighbors:** fit an explicit discovery/publish memory budget, keep all radio
  discovery on the application task, and test truncated large tables.
- **Browser setup:** implement a Pico server with the existing configuration
  validation/transaction model. Arduino-Pico currently does not support combined
  STA/AP mode; design an explicit setup-mode transition with network teardown,
  credential save and restart. Do not transplant the ESP async server unchanged.
- **OTA:** design RP2040-specific image validation, update staging space, filesystem
  compatibility, interrupted-update recovery and task/flash coordination. Prove
  physical recovery before enabling automatic updates or channel commands.
- **Room server/other Pico boards:** separate environment and resource qualification,
  with board-specific pins, frequency-band/module assumptions and protocol tests.

No estimate for feature parity should hide these stages inside the initial port.
Milestone 0 resolves dependency risk; remote tester availability determines when
hardware qualification can finish. Expect implementation across several focused
changes and at least the 48 h board soak after a working candidate is available.

## 9. Completion criteria and current evidence

First-release completion requires one repeatable UF2 build, compatible packet and
authentication output, passing host/regression builds, bounded networking and
memory behavior, recoverable persistent settings, accurate capability reporting,
and the remote acceptance evidence above. A compile-only candidate remains
experimental. Broader parity is complete only after the applicable later stages.

At plan creation: the branch was created from the verified current remote base;
the relevant implementation, local toolchain sources, workflow artifact handling,
manufacturer pin table and upstream transport APIs were inspected. No new
environment, firmware implementation, compile, broker session or hardware test
has been performed as part of writing this plan.

## Implementation record (2026-10-26)

The initial candidate is implemented in
`PicoW_repeater_observer_mqtt_experimental`. Operating instructions and the remote
tester handoff are in [pico-w-mqtt.md](pico-w-mqtt.md).

Implementation decisions refined after inspecting and compiling the dependencies:

- The common `MQTTBridge` facade selects a small Pico worker implementation.
  The ESP bridge implementation remains intact. Pico shares the existing payload,
  topic, authentication, filter, retry, backoff, lifecycle-state and persistence
  logic rather than copying the ESP bridge or performing a broad SDK abstraction
  rewrite. The proposed `EspObserverMqttClient` adapter is consequently unnecessary
  for this initial port.
- Application and network worker stacks are each 16 KiB. The pinned core actually
  creates a 4 KiB application stack; an environment-local source copy adjusts
  that without rewriting shared framework packages.
- The pinned WebSocket implementation needed environment-local fixes for
  first-use initialization, TLS 1.2 minimum, echoed MQTT subprotocol, bounded HTTP
  lines/upgrade deadlines and frame-read deadlines.
- The profile retains a ten-entry basic mesh neighbor cache because upstream
  repeater code does not compile with a zero-sized cache. MQTT neighbor publication
  remains unavailable.
- Active NTP probe-only diagnostics are deferred; the Pico diagnostic reports
  cached state accurately. A primary-server edit requests worker synchronization.
- The existing ESP-only vendored MQTT library was being discovered by ordinary
  Pico builds. The RP2040 exclusion and Pico W companion ignore overrides now
  preserve that build boundary.

Validation completed locally:

| Check | Result |
| --- | --- |
| Full native, KISS, OTA-channel and Pico transport suites | 605 tests passed |
| Pico adapter/backend ASan and UBSan run | 11 tests passed |
| Experimental Pico observer cross-compile | Passed; UF2 generated |
| Isolated pinned MQTT/TLS/WebSocket probe | Passed |
| Ordinary Pico W repeater and Wi-Fi companion | Passed |
| Integrated Waveshare RP2040-LoRa repeater | Passed |
| RAK4631 repeater | Passed |
| Heltec V3/V4 and V3 room-server observers | Passed |
| ThinkNode M7 Ethernet observer | Passed after clearing stale SCons metadata |
| ArduinoJson pin checks and workflow YAML parsing | Passed |
| UF2 structure, RP2040 family and application address range | Verified; no LittleFS overlap |

The observer build reports 97,748 bytes static RAM and 768,864 bytes application
flash. Dynamic heap, worst-case stack headroom, actual TLS timing, physical RF
performance, broker acceptance and the 48-hour soak remain unmeasured on hardware.
The candidate has not been published, installed or added to the observer flasher.
Milestones requiring those measurements remain acceptance gates.

## References

- [Base revision](https://github.com/agessaman/MeshCore/tree/ca8004187065a7b5419494cd2495bac0b0153cb1)
- [MeshCore flasher](https://flasher.meshcore.io/)
- [Waveshare Pico-LoRa-SX1262 pin table and product information](https://www.waveshare.com/wiki/Pico-LoRa-SX1262)
- [Arduino-Pico Wi-Fi constraints](https://arduino-pico.readthedocs.io/en/stable/wifi.html)
- [Arduino-Pico FreeRTOS and filesystem threading](https://arduino-pico.readthedocs.io/en/stable/freertos.html)
- [Arduino-Pico BearSSL verification, buffers and handshake timing](https://arduino-pico.readthedocs.io/en/stable/bearssl-client-secure-class.html)
- [256dpi MQTT API and transport requirements](https://github.com/256dpi/arduino-mqtt)
- [arduinoWebSockets RP2040 network selection](https://github.com/Links2004/arduinoWebSockets/blob/master/src/WebSockets.h)
- [arduinoWebSockets CA handling and reconnect behavior](https://github.com/Links2004/arduinoWebSockets/blob/master/src/WebSocketsClient.cpp)

Online dependency documentation describes moving upstream revisions. Milestone 0
must replace library/API assumptions with exact pinned revisions and compile
evidence; the inherited radio assumptions remain subject to remote board testing.
