# Host unit tests

Fast, hardware-free unit tests for the fork's pure logic, run on the host with
GoogleTest via PlatformIO's `native` environment. They cover the extractable
observer/WebConfig logic (validation, preset table, topic templates, key
parsing) — the parts that don't depend on the ESP32, radio, or network stack.
Integration behavior (AsyncTCP transport, WiFi/MQTT, SoftAP) is exercised
separately; see "Local testing without hardware" in `MQTT_IMPLEMENTATION.md`.

## Running

```sh
pio test -e native                      # all suites
pio test -e native -f test_webconfig_keys   # a single suite
```

A green `[PASSED]` per suite means GoogleTest returned 0 (all assertions
passed). PlatformIO's "0 test cases" line is just its Unity-style counter and
does not reflect the GoogleTest count — run the built binary directly
(`.pio/build/native/program`) to see the per-assertion breakdown.

## Suites

| Suite | Source under test | Covers |
|-------|-------------------|--------|
| `test_mqtt_presets` | `src/helpers/MQTTPresets.h` | preset lookup; table integrity (unique names, non-empty URLs, JWT-audience invariant, names fit the slot buffer); `mqttPresetNeedsSlotCredentials`; slot-count constants |
| `test_observer_validation` | `src/helpers/MQTTObserverValidation.h` | IATA (exactly 3 alphanumerics), owner key (64 hex), NTP hostname, and the buffer-fit check behind the #17 length validation — including boundaries and nulls |
| `test_webconfig_keys` | `src/helpers/WebConfigKeys.h` | POST-key allowlist, secret detection, admin-password classification/validation, slot-index bounds, and the short-key out-of-bounds guard (attacker-supplied keys) |
| `test_topic_template` | `src/helpers/MQTTTopicTemplate.h` | `{iata}/{device}/{token}/{type}` expansion, overflow/NUL-termination, and a buffer-size fuzz |
| `test_mqtt_topic_router` | `src/helpers/MQTTTopicRouter.h` | complete preset/custom topic-routing contract; MeshRank all types except raw; required identifiers; invalid inputs/slots; exact buffer boundaries |
| `test_mqtt_connection_policy` | `src/helpers/MQTTConnectionPolicy.h` | reconnect guard/backoff/stagger and breaker transitions; stable reset; JWT lifetime/renewal policy; exact timing boundaries and 32-bit `millis()` rollover; WiFi current-outage start sticky across STA reconnect attempts |
| `test_network_policy` | `src/helpers/NetworkPolicy.h` | transport-neutral MQTT link-transition actions and `start ota` selected-LAN versus forced/fallback SoftAP choice |
| `test_network_hostname` | `src/helpers/NetworkHostname.h` | DHCP-safe `meshcore-` hostname generation; invalid-character collapse; empty fallback; exact length boundary; stable identity suffix on truncation |
| `test_alert_fault_policy` | `src/helpers/AlertFaultPolicy.h` | WiFi/MQTT fault edge detector; `OutageSnapshot` (down / started_ms / initiating reason) fed to tick and `formatWifiAlert`; reason-8 reconnects change neither duration nor initiating reason; flap between status polls; down at `millis()==0`; packed 64-bit cross-task word; rate-limit floor and first-fire; 5 s poll cadence and `millis()` rollover |
| `test_display_viewport` | `src/helpers/ui/DisplayViewport.h`, `src/helpers/ui/DisplayFrameSignature.h` | logical-to-physical portrait mapping; fractional span coverage; fitted-width conversion; preferred/fallback text scaling; stable visible-frame change detection |
| `test_mqtt_packet_queue_policy` | `src/helpers/MQTTPacketQueuePolicy.h` | queue-full eviction; stale-disconnect flush; adaptive drain limits; bounded QoS0 retries; exact timing boundaries and 32-bit `millis()` rollover |
| `test_mqtt_packet_filter` | `src/helpers/MQTTPacketFilter.h` | per-slot 0-15 allowlist parsing/formatting, numeric and named spellings; exact bounds; membership; candidate/eligible split and retry-completion policy; pre-queue union gate; default-mask detection |
| `test_mqtt_runtime_buffer_lifecycle` | `src/helpers/MQTTRuntimeBufferLifecycle.h` | idempotent allocation/release; partial-allocation degradation; retry of only missing buffers |
| `test_mqtt_prefs_codec` | `src/helpers/MQTTPrefsStorage.h`, `src/helpers/MQTTPrefsCodec.h` | binary pre-slot/3-slot/6-slot migration fixtures; v1 header integrity; downgrade preservation; shortest-payload write policy (default filters stay downgrade-readable) |
| `test_mqtt_prefs_serializer` | `src/helpers/MQTTPrefsSerializer.h`, `src/helpers/ConfigSerializer.*` | semantic nested `/mqtt.json` round trips; numeric slot keys; required/future version handling; strict length/overflow/duplicate rejection; safe semantic repair; scratch-before-live loading |
| `test_mqtt_prefs_atomic_store` | `src/helpers/MQTTPrefsAtomicStore.h`, `src/helpers/MQTTPrefsRecovery.h` | production JSON begin/write/checksum-finish/schema-verify/commit orchestration; first-migration and rename-boundary recovery; legacy `/node_prefs` handoff; failure cleanup and original-file preservation |
| `test_mqtt_payload_builder` | `src/helpers/MQTTPayloadBuilder.cpp` | status/packet/raw JSON contracts; optional fields; escaping; RX metrics and path; score handling; exact buffer bounds; maximum representative payloads |
| `test_radio_activity_window` | `src/helpers/RadioActivityWindow.h` | 20-minute minute-bucketed RX window: totals and derived rates; bucket rotation and oldest-to-newest ordering; expiry at the boundary; ring clear after 20 minutes of silence; warm-up versus steady-state denominators; peak minute; last-packet age and staleness; counter saturation; `millis()` rollover, including the minute boundary a `now_ms / 60000` quotient would corrupt |
| `test_observer_dashboard` | `src/helpers/ui/ObserverDashboard.h` | R8 TFT observer dashboard against a recording `DisplayDriver` in both orientation profiles: compact number/byte/age formatting and the 5 s age quantisation; per-row character budgets; on-panel and inside-the-margin bounds; no silent portrait scale fallback; non-overlapping row rectangles and each row's repaint covering everything it draws; 20-bar graph scaling, ordering and empty/spike cases; per-row signatures and the partial-repaint policy |
| `test_touch_tap_detector` | `src/helpers/ui/TouchTapDetector.h` | debounced rising-edge detection for the polled Expansion Kit touch panel: idle quiet; one tap per touch; long presses do not repeat; sub-debounce blips ignored; contact bounce still counts once; minimum gap between accepted taps; `millis()` rollover; reset semantics |
| `test_utils` | `src/Utils.cpp` | `Utils::toHex` (upstream) |

## Conventions (and how to add a suite)

- Each `test/test_<name>/` directory builds into its **own** GoogleTest program
  and must define its own `main()` (`::testing::InitGoogleTest` + `RUN_ALL_TESTS`).
- Tests are **host-only**: include only pure headers. Arduino/crypto stubs live
  in `test/mocks/` (on the include path via `-I test/mocks`).
- Firmware headers are included from `src` (via `-I src`, e.g.
  `#include "helpers/MQTTPresets.h"`). Some are guarded or ESP-flavored, so a
  suite may need shims **before** the include — e.g. `test_mqtt_presets` does
  `#define WITH_MQTT_BRIDGE 1` (the preset table is behind that flag) and
  `#define PROGMEM` (the embedded CA-cert strings are PROGMEM-qualified).
- To add a suite: create `test/test_<name>/test_<name>.cpp` with a `main()`, and
  add any host-only source it links to the `native` env's `build_src_filter` in
  `platformio.ini` (header-only code needs no source entry). No other wiring.
- Keep logic testable by extracting pure functions into headers (as
  `MQTTObserverValidation.h` / `WebConfigKeys.h` / `MQTTTopicTemplate.h` do) and
  having the firmware call the same functions.
