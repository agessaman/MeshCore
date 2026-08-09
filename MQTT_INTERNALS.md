# MQTT Bridge Internals

Developer-facing notes on how the MQTT observer feature is structured in the codebase: source files, the seams that keep it isolated from upstream MeshCore code, and how on-device settings are migrated across firmware versions. For user-facing setup and CLI reference, see [MQTT_IMPLEMENTATION.md](MQTT_IMPLEMENTATION.md).

## Files

### Core Implementation
- `src/helpers/bridges/MQTTBridge.h` - MQTT bridge class definition
- `src/helpers/bridges/MQTTBridge.cpp` - MQTT bridge implementation
- `src/helpers/MQTTPresets.h` - Preset definitions, CA certificates, and lookup functions
- `src/helpers/MQTTDefaults.h` - Compile-time defaults for fresh `/mqtt.json`
- `src/helpers/MQTTPrefsSerializer.h` - Versioned, semantic observer JSON schema
- `src/helpers/MQTTMessageBuilder.h` - JSON message formatting utilities
- `src/helpers/MQTTMessageBuilder.cpp` - JSON message formatting implementation
- `src/helpers/JWTHelper.h` - JWT token generation for Ed25519-based authentication
- `src/helpers/CommonCLI_Observer.cpp` - All observer CLI command handling (MQTT, WiFi,
  timezone, NTP, OTA, SNMP, alerts)

### Integration seams with upstream code

The observer feature is kept out of upstream-tracked files through three mechanisms:

- **CLI hook methods** — upstream `CommonCLI.cpp` delegates to three `CommonCLI`
  methods defined in the fork-owned `CommonCLI_Observer.cpp`: `handleObserverCommand()`,
  `handleObserverSetCmd()`, and `handleObserverGetCmd()`. Each returns `true` if it
  consumed the command, otherwise the upstream parser runs. Only these three call
  sites touch upstream CLI code.
- **Callback virtuals** — observer behaviour needed from the application is exposed
  as default-no-op virtuals on `CommonCLICallbacks` (e.g. `restartBridgeSlot`,
  `isMqttBridgeRunning`, `syncMqttNtp`, `onAlertConfigChanged`, `sendAlertText`,
  `resolveAlertScope`, `beginDeferredOtaUpdate`). The example apps override them
  behind `#ifdef WITH_MQTT_BRIDGE`.
- **Separate settings file** — observer settings (MQTT slots, WiFi, timezone, SNMP,
  radio watchdog, fault alerts) live in the runtime `MQTTPrefs` object and are
  field-serialized to `/mqtt.json`, keeping `NodePrefs` / `/prefs.json` aligned with
  upstream. The two files are independent transactions, not one atomic snapshot.

Remaining integration points in upstream files:
- `examples/simple_repeater/MyMesh.{h,cpp}`, `examples/simple_room_server/MyMesh.{h,cpp}` -
  bridge/alerter/SNMP wiring and packet-feed hooks, guarded by `#ifdef WITH_MQTT_BRIDGE`;
  plus the `createObserverPacketManager()` call in each constructor (see below)
- `src/helpers/CommonCLI.{h,cpp}` - the three CLI hooks, `MQTTPrefs` load/save/migration
- `src/Dispatcher.{h,cpp}` - radio watchdog block, guarded by `#ifdef WITH_MQTT_BRIDGE`

### Capture vs. duty-cycle throttling

RX processing needs a free packet from the static pool before `logRx()` (and thus the
MQTT uplink) can run — `Dispatcher::checkRecv()` silently discards received data when
the pool is empty. Because the outbound queue holds pool packets with no expiry,
duty-cycle throttling can park the entire pool waiting on TX budget, capping capture at
the TX rate — and the parked repeats absorb every budget refill, starving the node's
own CLI responses and making it un-administrable over the mesh. Observer builds
therefore use `RxReservePacketManager` (fork-owned,
`src/helpers/RxReservePacketManager.h`): below the RX reserve (a quarter of the pool)
it sheds only low-priority outbound (multi-hop flood repeats, adverts, trace), keeping
the node's own responses/ACKs queueable; below a smaller emergency floor it sheds
everything to keep capture alive. Queued packets still untransmitted 30 s past their
scheduled time are expired at dequeue, so under throttle the queue holds only fresh
traffic and admin responses reach the trickle of TX budget. Non-observer builds keep
the upstream pool behavior.

### Neighbors publication path (PSRAM only)

Periodic neighbors publishing is gated on `WITH_MQTT_NEIGHBORS`
(`defined(BOARD_HAS_PSRAM) && defined(MAX_NEIGHBOURS) && MAX_NEIGHBOURS > 0`,
defined in `MQTTBridge.h`). It spans two subsystems and two cores:

- **Mesh side (Core 1), `MyMesh`**: the `loop()` runs a two-stage refresh driven by
  `mqtt_neighbors_interval`. Stage 1 sends a zero-hop `sendNodeDiscoverReq()` and waits
  out its 60 s collection window to refresh `neighbours[]`. Stage 2
  (`startNeighborDiscover`) fires one anon-regions scope query per heard neighbor,
  overlaying them onto the peer-index space at `NEIGHBOR_DISCOVER_PEER_BASE` so their
  `PAYLOAD_TYPE_RESPONSE` packets decrypt via `searchPeersByHash`/`getPeerSharedSecret`/
  `onPeerDataRecv` even when the neighbor is not an ACL client. A reply is zero-hop by
  request, so `handleNeighborDiscoverResponse` also re-stamps `heard_timestamp` in both
  the snapshot and `neighbours[]` — proof of reception, and the only thing that heals a
  stamp taken before the clock was set. After all responses land or a 30 s window
  expires, `finishNeighborDiscover()` builds the JSON with
  `MQTTMessageBuilder::buildNeighborsMessage` into a transient buffer (PSRAM where
  available, internal DRAM otherwise) and hands it to the bridge. The entry table and its
  hex strings share one heap block sized to the pass — at `MAX_NEIGHBOURS` they reach
  ~4.5 KB, which does not fit the mesh loop task's 8 KB stack. Ages that still span a clock epoch publish as `null` rather than a
  fabricated delta (`neighborHeardAgeUsable`, see `UPSTREAM_BUGS.md` #1).
- **Buffer sizing**: `NEIGHBORS_JSON_BUFFER_SIZE` is 10 KB with PSRAM and 4 KB without,
  since a non-PSRAM board pays for the persistent buffer, the transient build buffer and
  the ArduinoJson pool out of the same internal DRAM each TLS slot needs ~40 KB of
  (~13 KB peak instead of ~35 KB). The pool has its own budget
  (`NEIGHBORS_DOC_POOL_BUDGET`) because ArduinoJson v7 hands out pool blocks in fixed
  4096-byte chunks, so a table that just fits the text buffer can still need well over
  it in pool — and a starved pool sets `doc.overflowed()`, which drops the entire publish
  instead of truncating it. `NEIGHBORS_MAX_PUBLISH_ENTRIES` (20 without PSRAM) keeps the
  pool inside a single block.
- **Bridge side, handoff**: `requestPublishNeighbors(json, len)` (Core 1) memcpys into the
  persistent buffer (`NEIGHBORS_JSON_BUFFER_SIZE`, PSRAM where available) and sets
  `_neighbors_publish_pending` with a release store; the MQTT task (`mqttTaskLoop`, Core 0)
  consumes it with an acquire load, calls `publishNeighbors()`, and clears the flag. A
  second snapshot is dropped while one is in flight. `publishNeighbors()` sends QoS 1,
  retain = `preset->allow_retain` (custom slots non-retained). MeshRank slots are included,
  publishing to `meshrank/uplink/{token}/{device}/neighbors` (non-retained, since the
  preset sets `allow_retain = false`).
- **Status reporting**: `MyMesh` reports the schedule each loop via
  `setNeighborsSchedule(phase, secs)`; `formatMqttStatusReply` renders it as the trailing
  `nbr: <when>/<last>` field in `get mqtt.status` while the feature is enabled.

The JSON builder lives in the pure, host-tested `MQTTPayloadBuilder`
(`test/test_mqtt_payload_builder`); the topic type in `MQTTTopicRouter`
(`test/test_mqtt_topic_router`). The mesh↔bridge orchestration above is on-target only.

### Runtime construction and slot memory

- **Deferred construction** — `MQTTBridge` is heap-allocated in each app's `begin()`
  (`bridge = new MQTTBridge(...)` in `MyMesh.cpp`) rather than held as a static member,
  because constructing it at static-init time crashes on ESP32 classic.
- **Runtime slot array** — `RUNTIME_MQTT_SLOTS` (`MQTTPresets.h`) is 6 with PSRAM and 3
  without, saving ~1.2 KB of heap on non-PSRAM boards. `MAX_MQTT_SLOTS` stays 6 on every
  build because it fixes the persisted `MQTTPrefs` layout, so slot config survives moving
  firmware between board classes. Three runtime slots suffice without PSRAM:
  `_max_active_slots` caps those boards at 2 live connections, leaving one spare for
  reconfiguration. Configured slots past the cap report `(inactive)`.
- **Buffers** — the 768-byte JWT `auth_token` is inline in every `MQTTSlot`, not allocated
  per JWT-auth slot. What varies is the MQTT client's TX/RX buffer: 896 bytes (the minimum
  that fits a CONNECT plus a 768-byte JWT) uniformly on PSRAM boards to limit
  fragmentation from mixed allocations, and 896 or 512 per slot on non-PSRAM boards so
  non-JWT slots leave smaller holes across teardown/recreate cycles. The large
  JSON/raw-packet buffers go through `psram_malloc()`, which prefers PSRAM and falls back
  to internal heap.

### Reconnection, backoff, and circuit breaker

The client's own auto-reconnect is disabled (`setAutoReconnect(false)`); the bridge drives
reconnection per slot.

- Backoff ladder: 10 s → 30 s → 60 s → 120 s → 300 s, staggered by 3 s × slot index so
  slots don't all handshake at once.
- The ladder resets only after a connection has held for 2 minutes
  (`BACKOFF_STABLE_RESET_MS`), which is longer than the 75 s keepalive — a link that can't
  survive one keepalive round-trip keeps its earned rung instead of hammering TLS
  handshakes at the 10 s rung. CONNACK alone does not reset it.
- After 3 more failures at the top rung (~15 min) the slot's circuit breaker trips and
  routine reconnects stop. A tripped slot is probed once every 30 minutes (with a fresh
  JWT where applicable); a successful connect clears the breaker, as does reconfiguring
  the slot.
- Message retransmit timeout is 15 s — one retry inside esp-mqtt's 30 s outbox expiry,
  preserving at-least-once delivery while capping duplicates at one.

### Message building

- The `hash` field in `packets` messages is MeshCore's own packet hash,
  `Packet::calculatePacketHash()` — SHA256 over the payload type and payload (plus
  `path_len` for TRACE), truncated to `MAX_HASH_SIZE`. It is the same value the dispatcher
  uses, so uplinked hashes match the mesh.
- `score` is recomputed at publish time from the packet's SNR and length via the radio's
  `packetScore()`, so it matches the value the firmware used on receive.
- Timezone: the JChristensen/Timezone object (`_timezone_storage`, inline since the
  memory-defrag work) is kept current from `timezone_string` via `setRules()`, but
  `formatIsoTimestampForMqtt()` explicitly ignores it — every published timestamp, time,
  and date field is UTC off `gmtime()`, matching Python's
  `datetime.now(timezone.utc).isoformat()`. The timezone prefs therefore do not affect
  MQTT message content.

### Command namespacing

CLI commands sit at two levels. `bridge.*` is low-level and shared by all bridge types
(MQTT, RS232, ESP-NOW): `bridge.enabled` is the master switch, and `bridge.source` selects
which packet events non-MQTT bridges capture. The MQTT bridge ignores `bridge.source` in
favour of independent `mqtt.rx` / `mqtt.tx` controls. Everything MQTT-specific lives under
`mqtt.*` (shared settings), `mqttN.*` (per-slot broker config), `wifi.*`, and `timezone.*`.

### `/mqtt.json` file format

Observer preferences use the same `ConfigSerializer` object notation as upstream
`/prefs.json`: semantic unquoted keys, quoted strings, decimal numbers, and nested
objects. Schema version 1 requires the root `version:1` field. The main shape is:

```text
{version:1,
 wifi:{ssid:"...",password:"...",power_save:1},
 time:{timezone:"...",utc_offset:0,ntp_server:"..."},
 mqtt:{origin:"...",iata:"SEA",packets_enabled:1,raw_enabled:0,
       tx_enabled:2,rx_enabled:1,
       status:{enabled:1,interval_ms:300000},
       neighbors:{enabled:0,interval_ms:86400000},
       owner:{public_key:"...",email:"..."},
       slot1:{preset:"analyzer-us",host:"",port:0,username:"",password:"",
              token:"",topic:"",audience:"",packet_filter:65535},
       ... slot2 through slot6 ...},
 snmp:{enabled:0,community:"public"},
 radio:{watchdog_min:5},
 alert:{enabled:0,psk_hex:"",wifi_minutes:30,mqtt_minutes:240,
        rate_limit_min:60,hashtag:"",region:""}}
```

Keys may contain digits after their first character, which permits the readable
`slot1` ... `slot6` names. Every schema key fits `ConfigSerializer`'s 15-character
visible-key limit. Known strings and numbers use strict parsing: duplicates, overlong
strings, malformed decimals, and overflow reject the complete file. A supported file
with a semantically out-of-range value is repaired to that field's safe default and
rewritten atomically. Unknown fields are ignored only within the serializer's general
limits (15-character keys, 127-byte decoded values, and six nested object levels below the root).

Loading always starts with defaults and parses into a separate heap scratch object.
The live preferences change only after the complete file parses, has an explicit
supported version, and passes validation. A missing/future version, syntax error,
overlength value, or allocation/read failure leaves `/mqtt.json` untouched, runs this
boot on defaults, and holds observer saves so a later CLI/WebConfig write cannot erase
the opaque source. If an observer setter cannot commit its JSON transaction, it restores
the pre-command in-memory preferences and does not restart or apply a bridge change as
though the setting were durable.

Saves stream to `/mqtt.json.tmp` through a sticky short-write detector while computing
size and checksum. The firmware closes and rereads the temp, verifies size/checksum,
parses it into another scratch object, then publishes with
`/mqtt.json` -> `/mqtt.json.bak` and temp -> primary. Boot recovery selects the usable
primary/temp/backup without overwriting an opaque future or corrupt primary. A valid
future-version temp that reached the rename phase wins over the stale backup and is held
for newer firmware. If a temp claims a future version but uses grammar this firmware
cannot parse, or cannot be classified because scratch allocation fails, recovery may run
the last usable backup but retains the uncertain temp and holds all observer writes. A
definitively corrupt/incomplete current-version temp may be discarded for that backup.
If power fails during the very first migration, there is no JSON primary or backup yet;
recovery discards a definitively invalid temp so the intact `/mqtt_prefs` source can be
loaded and the migration retried. An uncertain/future temp is still preserved and held.

Unknown slot preset names are repaired to `none`, never to a build-specific default
broker. Duplicate known presets from historical firmware are preserved on load; current
CLI and WebConfig setters prevent creating new duplicates without silently changing an
existing deployment during migration.

Schema changes that reinterpret existing names or values must increment `version`.
Additive fields may remain in version 1 only when old readers can safely ignore them
within the limits above. A future version is treated as opaque rather than partially
loading its known-looking fields. Literal schema keys are compile-time checked against
the 15-character visible-key limit so a new version-1 field cannot accidentally violate
that downgrade contract.

#### Downgrade and rollback

The old `/mqtt_prefs` binary is read only for one-time migration and is deliberately
not updated or deleted. When it exists and is usable, it is the exact pre-migration
rollback snapshot. Firmware older than this JSON change will therefore see stale
observer settings after a downgrade. A fresh JSON-only install has no binary observer
configuration to recover on downgrade.

The two files are intentionally not reconciled. If an operator changes observer settings
while running old firmware, those changes update only `/mqtt_prefs`. Rolling forward to
this firmware makes the existing `/mqtt.json` authoritative again, so rollback-era edits
are ignored unless the operator exports and reapplies them. Conversely, JSON-only settings
cannot be recovered by old firmware. This is a rollback snapshot, not bidirectional sync.

`LegacyV1MQTTPrefs` and the older pre-slot/3-slot/6-slot structs are frozen migration
ABIs with size/offset assertions. The runtime `MQTTPrefs` layout is not an on-flash ABI.

### Settings upgrade / migration

`loadPrefs()` handles every historical on-device format one-time at boot:
- **`/mqtt_prefs` -> `/mqtt.json`** — if the legacy file has the version header its
  frozen v1 layout is field-copied. Otherwise
  it is a legacy headerless file and its layout is detected by size: pre-slot
  (`OldMQTTPrefs`), 3-slot (`ThreeSlotMQTTPrefs`), or the 6-slot layout shipped on
  `observer-firmware` back when it was named `mqtt-bridge-implementation-flex`
  (`Legacy6SlotMQTTPrefs`). Each is field-copied into
  the runtime `MQTTPrefs` and saved as schema-versioned JSON — which also
  drops the vestigial `_legacy_*` fields the flex layout carried mid-struct. This is a
  one-time rewrite; every deployed device performs it on its first boot of versioned
  firmware, after which `/mqtt.json` is authoritative. The binary source remains as
  a rollback snapshot and is never dual-written.
  The pre-slot (`OldMQTTPrefs`) copy maps the old single-broker keys onto slots:
  `mqtt.analyzer.us = on` → slot 1 `analyzer-us`, `mqtt.analyzer.eu = on` → slot 2
  `analyzer-eu`, and a configured `mqtt.server` / `mqtt.port` / `mqtt.username` /
  `mqtt.password` → slot 3 `custom` with those values preserved. Origin, IATA, message
  types, WiFi, and timezone carry over as-is.
- **`/com_prefs`** — a file written by fork firmware that predates the `MQTTPrefs` split
  (a zero-filled MQTT gap plus a trailing observer block) is detected by size; the
  trailing SNMP / radio-watchdog / fault-alert settings and the `rx_boosted_gain` /
  `flood_max_*` fields are recovered, carried into `/mqtt.json`, and the active files are
  rewritten in the current formats.
- Settings the pre-split firmware stored *inside* the `/com_prefs` MQTT gap (the MQTT
  slot/WiFi config itself) are **not** recovered — users upgrading from firmware that
  old must re-enter their MQTT and WiFi configuration.
