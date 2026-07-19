# Stability, Testability, and Upstream-Merge Handoff

## Purpose

This document is the plan of record for refining the fork-owned WebConfig and
MQTT observer code after the initial policy extraction and host-test work. The
goal is to improve runtime stability, long-uptime confidence, and serviceability
without broad rewrites of upstream-heavy files or creating unnecessary merge
conflicts.

The order is deliberate: cheap CI and persistence guardrails land first, then
tests and ownership boundaries needed to make lifecycle work safe, and only
then the cooperative-shutdown refactor. Hardware soak testing validates the
result; it is not the first line of defense for the riskiest change.

## Roadmap Status

The guardrail phases have already landed on this branch; the remaining work is
the lifecycle refactor and its safety net. Phases are intentionally not
renumbered so cross-references and the completed acceptance criteria stay stable;
each phase below carries an explicit status line, and this table is the quick
index.

| Phase | Scope | Status |
|-------|-------|--------|
| 1 | PR CI smoke builds + ArduinoJson pin enforcement | Done (build-size gate and ASan/UBSan still pending) |
| 2 | PSRAM restart resource symmetry | Done |
| 3 | MQTT preference migration fixtures | Done (filesystem adapter still lives in `CommonCLI`) |
| 0 | Pre-change lifecycle characterization | Hardware run 2026-07-19 (see "Hardware Characterization Results"): teardown timing measured on V3+V4. Finding: flat `MQTT_STOP_TIMEOUT_MS=8000` too small → **FIXED** with slot-scaled timeout (`5s + 8s×slots`), hardware-verified (2-slot stop now clean) |
| 4 | Ownership and teardown test seams | Seams + ownership doc + teardown tests done; production rewiring deferred to Phase 5 |
| 5 | Cooperative MQTT shutdown | Minimal cooperative `end()` + `begin()` guard + OTA barrier implemented on branch `phase5/cooperative-mqtt-shutdown` (native green, firmware smoke build green); NOT hardware-validated. Volatile-handshake replacement + snapshot-consumer repointing deferred |
| — | OTA teardown barrier | Implemented — flash gated on a clean MQTT stop in `simple_repeater`; not hardware-validated |
| 6 | Request/queue/connection/publication integration tests | Partial: WiFi-backoff + publish-outcome + enum-alignment gaps extracted and host-tested on branch `phase6/integration-tests`; WebConfig batch/reboot/stop state machine has a pure host-tested spec (`WebConfigBatch.h`, not yet wired); queue-orchestration coverage still open |
| 7 | Uptime, memory, and fault-injection gates | Representative HW matrix run 2026-07-19: V3 non-PSRAM + V4 PSRAM done (no leak/crash; forced-path OTA-withhold + ~15–27 s loop stall observed). Multi-day soak + stack-HWM build pending |

Forward plan, in execution order: **Phase 0 → Phase 4 → Phase 5 (with the OTA
teardown barrier) → Phase 6 → Phase 7.** Do not reopen a "Done" phase without a
deliberate reason (see "Change-Control Discipline").

## Hardware Characterization Results (Phase 0 & Phase 7) — 2026-07-19

Hardware run of the outstanding Phase 0 (pre-change/cooperative teardown timing)
and Phase 7 (uptime/memory/fault-injection) items against two live observer
nodes. **Not yet committed as a plan change — this section records measured
results and a release-gating recommendation for review.**

### Setup

- **V3** — Heltec WiFi LoRa 32 **V3**, non-PSRAM (max 2 active slots), env
  `Heltec_v3_repeater_observer_mqtt`, 1 configured slot (`meshmapper`, wss).
- **V4** — Heltec WiFi LoRa 32 **V4**, **2 MB PSRAM** (max 5 active slots), env
  `heltec_v4_repeater_observer_mqtt`, 3 configured slots (`analyzer-us`,
  `cascadiamesh`, `waev`, all wss). mbedTLS + JSON/raw buffers allocate in PSRAM.
- Both flashed with this branch (`phase5/cooperative-mqtt-shutdown`) via
  `pio run -t upload` (app-slot flash only; `nvs`+`spiffs` preserved, no erase).
  Note: `FIRMWARE_VERSION`/`FIRMWARE_BUILD_DATE` are hardcoded in `MyMesh.h` and
  were not bumped, so `ver` reads "v1.16.0 (6 Jun 2026)" on both old and new
  builds — confirm the flash by behavior (the `(clean)` / cooperative-stop log
  lines), not by `ver`.
- Driven over the serial CLI (`set bridge.enabled off/on` = `end()`/`begin()`;
  `set mqttN.*` = slot reconfigure; `get mqtt.stats` = `Free`/`Max`(largest
  block)/queue/outbox/per-slot counters). Host-side timestamped log parsing.

### PRIMARY FINDING — `MQTT_STOP_TIMEOUT_MS = 8000` is too small (release-gating)

Per-wss-slot teardown costs **~5–6 s**, applied **sequentially**
(`destroySlotClients()`: `disconnect()` → 50 ms → `esp_mqtt_client_destroy()`;
the ~5 s is the esp-mqtt task/network close, not the 50 ms settle). This is
unchanged from the pre-change path (same teardown code), so the cooperative
`end()` ack time scales with the number of connected slots:

| Config | Slots | Measured teardown | vs 8 s timeout | Result |
|--------|-------|-------------------|----------------|--------|
| V3 non-PSRAM | 1 wss | ack ~5.3–6.2 s (avg 5.8) | under | **clean** ✅ |
| V3 non-PSRAM (design max) | 2 wss | ~11–12 s (cut off at 8 s) | **over** | **forced/dirty → OTA withheld** ❌ |
| V4 PSRAM (normal config) | 3 wss | ~16 s (cut off at 8 s; per-slot disc @2.2/7.8/10.6 s) | **over** | **forced/dirty → OTA withheld** ❌ |
| V4 PSRAM (design max) | 5 wss | ~27–30 s projected | **far over** | forced/dirty → OTA withheld ❌ |

Pre-change (v1.16.0) reference teardown (same nodes, blocking `end()`): 1 slot
~6.1 s (V3), 3 slots ~16.4 s (V4) — consistent with the above.

**Consequence:** at the non-PSRAM board's *designed maximum* of 2 wss slots, and
on a *normal healthy* 3-slot PSRAM node, an ordinary cooperative shutdown exceeds
8 s → the coordinator fires `StopTimedOut` → sets the dirty latch → force-kills
the MQTT task (the exact mbedTLS-mid-teardown path Phase 5 exists to avoid) and
`canFlashAfterStop()` returns false. Because the OTA barrier only flashes after a
**clean** stop, this means **any multi-slot device would have `ota update`
permanently withheld** with the current timeout — the barrier misclassifies
healthy stops as dirty. This inverts the barrier's intent and must be fixed
before Phase 5 ships.

**FIX APPLIED (slot-count-aware timeout) — verified on hardware.**

The flat `MQTT_STOP_TIMEOUT_MS = 8000` is replaced by a slot-scaled budget
computed per stop in `MQTTBridge::end()`:

```
timeout = MQTT_STOP_TIMEOUT_BASE_MS (5000) + MQTT_STOP_TIMEOUT_PER_SLOT_MS (8000) × enabled_slots
```

→ 1 slot 13 s, 2 slots 21 s, 3 slots 29 s, 5 slots 45 s — ~1.5–2× headroom over
the measured ~5–6 s/slot teardown. `end()` counts enabled slots and calls
`Coordinator::setStopTimeoutMs()` before `requestStop()`. Headroom is nearly free
because `end()` returns as soon as the task acks (`_stop_acked` is checked before
the timeout ticks), so a larger bound does not slow a healthy stop — it only
lengthens the wait before force-killing a genuinely wedged task. (Files:
`MQTTBridge.cpp` constants + `end()`; `MQTTLifecycle.h` `setStopTimeoutMs()`.)

**Hardware verification (both memory paths):** the same configs that force-timed-
out at 8 s pre-fix now ack cleanly — V3 non-PSRAM 2-slot logs `timeout 21000 ms`
and acks in ~11.7 s; V4 PSRAM 3-slot logs `timeout 29000 ms` and acks in ~16.4 s
→ **`MQTT Bridge stopped (clean)`** (OTA no longer withheld). Native suite + both
firmware builds green. Complementary future option (not done): shorten the
per-slot `esp_mqtt_client_destroy()` wait to reduce absolute teardown time.

### Phase 0 — cooperative lifecycle (V3, non-PSRAM, this branch)

- 6× and 10× `begin→connect→end→restart` cycles: **all clean**, ack 5.3–6.2 s,
  end() loop-task block ≤6.9 s, restart→connect ~5.4 s.
- **No leak across full stop/start cycles:** free heap flat (~137–142 k, varies
  only with the meshmapper outbox), largest free block stable (~115–129 k). This
  satisfies Phase 5's "no downward heap/largest-block trend across start/stop
  cycles" acceptance criterion on the non-PSRAM path.

### OTA teardown barrier

- Barrier **input** validated on hardware in both states: clean stops →
  `canFlashAfterStop()` true; forced/timeout stops log `OTA blocked` /
  `OTA flashing withheld` (= `canFlashAfterStop()` false). Confirmed
  `mayBeginFlash() == (Stopped && !_stop_timed_out)`; a fresh start clears the
  latch; a dirty stop still permits restart.
- Barrier **action** (`simple_repeater` aborts+resumes vs. proceeds) is a
  one-line gate on that latch (`MyMesh.cpp` deferred-OTA site) — validated by
  code + the Phase 4 host lifecycle tests.
- **Not exercised end-to-end on hardware:** the live `ota update` deferred-flash
  path. A plain `pio run` build reports `ERR: OTA not configured (build via
  build.sh)`, so `otaFromManifest` bails before scheduling; driving it needs a
  `build.sh` firmware + a controlled manifest, and was not improvised on working
  fleet nodes (risk of flashing a real fleet build). Recommend a dedicated
  bench run for this.

### Phase 7 — fault-injection matrix (V3 non-PSRAM; representative, not the soak)

Matrix: 10 clean stop/start + 5 forced 2-slot teardowns + 8 slot-reconfigure +
4 down-broker (`wss://192.0.2.1`) flaps. Time series of free/largest-block/
queue/outbox recorded per step; reboot/crash detection on.

- **No crash across the entire matrix** (incl. 5 repeated force-kill fallbacks) —
  the reviewed dirty-stop fallback is robust on non-PSRAM.
- **Forced teardown reproducible:** every 2-slot stop timed out (acks 7.4–8.2 s),
  reinforcing the primary finding.
- **Bounded fragmentation, fully recoverable:** slot-level reconfigure/flap
  churn (which keeps a disabled slot's persistent client until a *full* bridge
  restart) dropped the largest free block from ~125 k to ~72 k and held ~17 k of
  free heap, but it **plateaued** (stable across 12 churn iterations, not an
  unbounded leak) and a reboot fully restored ~141 k free / ~125 k largest block.
  This is pre-existing bridge behavior (TLS-context lifecycle), not a Phase 5
  regression.

### Phase 7 — forced-teardown stress (V4 PSRAM, 3 wss slots, this branch)

8× `end()`/`begin()` cycles at the node's normal 3-slot config (every stop
exceeds 8 s → forced/dirty):

- **All 8 forced, no leak, no crash.** Largest free block **rock-stable at
  139 252 across all 8 cycles** (mbedTLS is PSRAM-allocated, so internal-heap
  fragmentation is absent on the PSRAM path); free internal heap flat. The
  force-kill fallback is robust on PSRAM too.
- **Loop-task stall is severe on the forced path:** `end()` blocked the loop
  task **~15–27 s** per stop, because the forced path waits the full 8 s timeout
  and *then* runs a complete redundant teardown on Core 1 (≈ 8 s + ~16 s). During
  that window the repeater's mesh/CLI/radio servicing is stalled. Sizing the
  timeout so the cooperative (Core-0) teardown completes cleanly removes both the
  OTA-withhold and this double-teardown stall.
- Both nodes left restored to their original config, bridge on, heap healthy.

### Limitations / not covered

- Task stack high-water mark and explicit task/client counts are not exposed by
  the CLI; a dedicated `MQTT_MEMORY_DEBUG` build is needed for those (heap
  stability was used as a proxy leak signal, and it held).
- The 72 h / 7-day soaks were not run (bounded representative cycles per agreed
  scope); WiFi loss/recovery, TLS-handshake failure, queue saturation, JWT/NTP
  failure, and `millis()`-rollover scenarios need external network control /
  time injection and were not driven over serial.
- Both devices left restored to their original slot config with the bridge on.

## Current Baseline

The current branch has:

- Native tests (GoogleTest, `[env:native]`) for MQTT presets, validation, topic
  templates and routing, connection policy, packet-queue policy, payload
  construction, WebConfig keys, the `/mqtt_prefs` codec, the atomic prefs store,
  the runtime-buffer lifecycle, and upstream `Utils::toHex` behavior.
- ArduinoJson pinned to 7.4.3 across the native and all firmware environments,
  enforced in CI by `scripts/check_arduinojson_pin.py`.
- PR CI (`.github/workflows/`) that runs the native suite and compiles both
  representative MQTT observer smoke builds:
  `Heltec_v3_repeater_observer_mqtt` (non-PSRAM) and
  `T_Beam_S3_Supreme_SX1262_repeater_observer_mqtt` (PSRAM).
- Symmetric PSRAM runtime buffers: `begin()` allocates through
  `allocateRuntimeBuffers()` and `end()` frees and nulls through
  `releaseRuntimeBuffers()`, so raw-data caching and the PSRAM JSON buffers are
  restored after a restart (`MQTTRuntimeBufferLifecycle.h`).
- A versioned `/mqtt_prefs` loader extracted into fork-owned, host-tested seams
  (`MQTTPrefsCodec.h`, `MQTTPrefsStorage.h`, `MQTTPrefsAtomicStore.h`) that
  classifies every deployed layout, preserves an unknown or newer file without
  overwriting it, and rejects corrupt input without out-of-bounds reads.
- WebConfig request/result correlation, failed-batch reboot gating, and a
  process-lifetime HTTP listener that avoids deleting an object still referenced
  by an asynchronous request.
- Pure MQTT policy helpers that reduce decision duplication while leaving the
  production bridge as the integration point.

This is a solid guardrail and unit-test foundation, but it does not yet validate
the MQTT task/client lifecycle, cross-core state ownership, the OTA teardown
barrier, or long-running heap behavior. Those are the remaining phases.

## Constraints

1. MQTT preference-file layouts are fleet-critical. Unknown newer formats must
   not be overwritten, and every supported old layout must remain recoverable.
2. Upstream MeshCore changes are merged regularly. Prefer additive fork-owned
   helpers and small adapters over reorganizing upstream-heavy files.
3. The non-PSRAM profile is the memory-pressure baseline; the PSRAM profile is
   a separate allocation/lifecycle path and must also be tested.
4. Risky lifecycle changes require fast deterministic tests before hardware
   soak testing.
5. Preserve observable behavior unless a behavior change is explicitly named,
   reviewed, and tested.

## Change-Control Discipline (Stop-and-Ask)

This roadmap is executed incrementally, often by an agent working one phase at a
time. Each unit of work is scoped to its assigned phase. When work uncovers
something outside that scope, the correct action is to **stop and ask, not to fix
forward.** Silent scope expansion is the primary way a targeted stability change
turns into an unreviewed refactor or a needless merge-conflict surface. This
section is binding on anyone — human or agent — executing the plan.

### Stop and ask before writing code when any of these is true

- A refactor larger than the current phase authorizes appears necessary — for
  example touching task lifecycle, client lifetime, cross-core ownership, or the
  queue backends when the phase did not name them.
- A bug is discovered that is not described in the plan, especially one
  affecting persistence, teardown, OTA, or heap behavior.
- A phase premise turns out to be false or already implemented (for example, a
  described defect that is already fixed). Report the discrepancy and get the
  plan re-baselined before writing code against a stale assumption.
- A change would alter observable behavior the phase did not explicitly name,
  review, and test (Constraint 5).
- Work would touch upstream-heavy files (`MQTTBridge.cpp`, `CommonCLI`, the
  role-specific `MyMesh` files) beyond a small, additive adapter (Constraint 2).
- A change touches fleet-critical persistence layouts, or could overwrite or
  discard an unknown or newer `/mqtt_prefs` file (Constraint 1).
- Two phases would be combined in one change, or a "Explicitly Deferred Debt"
  item would be pulled forward.
- A fix spans multiple files, changes a public seam, or would surprise a
  reviewer expecting only the phase's stated work.

### What "stop and ask" means in practice

- Do not implement the out-of-scope change in the same pass. Record it.
- Report it with evidence: file and line references, a concrete reproduction or
  failure scenario, and the specific invariant or constraint at risk.
- Present options with tradeoffs and a recommendation, then wait for an explicit
  decision on scope before proceeding.
- File a newly found bug as its own item. Do not fold an opportunistic fix into
  an unrelated phase's commit — single-purpose commits are required, and an
  unexpected fix deserves its own review.

### What may proceed without asking

- Work squarely inside the assigned phase and its acceptance criteria.
- Small, behavior-preserving fixes fully contained in fork-owned helper files
  with matching host tests, where a reviewer would expect them as part of the
  phase.

When uncertain whether a change is in scope, treat it as out of scope and ask.
The cost of a question is a round trip; the cost of an unreviewed lifecycle or
persistence change across a fleet of thousands of devices is not.

## Sequenced Work

### Phase 0: Record the pre-change lifecycle characterization

**Status: Not started — the next actionable step, and prerequisite for Phases 4
and 5.**

Capture current behavior before changing shutdown mechanics. This provides a
reference for the lifecycle fakes and lets the later state-machine refactor
prove equivalence rather than relying on memory or a multi-hour soak.

Characterize at least:

- Normal `begin() -> connect -> end()` ordering.
- `end()` while disconnected, connecting, connected, publishing, retrying, and
  applying a slot reconfiguration.
- Which callbacks can arrive during and after `end()`.
- Queue disposition and connection/status counters across stop and restart.
- Heap, largest allocatable internal block, task stack high-water mark, and
  client/task counts before start, after start, after stop, and after restart.
- Partial initialization failures: queue allocation, task creation, client
  allocation, and PSRAM-buffer allocation.

Use instrumented hardware logging where necessary, but encode every behavior
that can be represented deterministically into the lifecycle fake tests in
Phase 4. Store representative logs as CI or test artifacts rather than enabling
permanent high-volume production logging.

### Phase 1: Put MQTT/WebConfig firmware smoke builds in PR CI

**Status: Complete on this branch.** PR CI already compiles both smoke builds and
runs the native suite and the ArduinoJson pin check. Still pending against the
acceptance criteria: build-size artifact/threshold reporting (criterion 4) and
the optional ASan/UBSan native job. The description below is retained as the
record of intent.

The PR build matrix historically did not compile an MQTT observer target; it now
compiles two required smoke builds for changes touching firmware, variants,
PlatformIO configuration, MQTT/WebConfig helpers, or their tests:

- `Heltec_v3_repeater_observer_mqtt` for the constrained non-PSRAM path.
- `T_Beam_S3_Supreme_SX1262_repeater_observer_mqtt` for the PSRAM path.

Keep the native suite required. Add ASan/UBSan to a separate native job if the
PlatformIO native toolchain supports it reliably. Record static RAM and flash
usage and fail only on reviewed limits with enough headroom to avoid noisy
one-byte regressions.

Acceptance criteria:

- Pull requests cannot merge when native tests or either representative MQTT
  build fails.
- The build log shows ArduinoJson 7.4.3 for both firmware profiles.
- CI checks that every ArduinoJson declaration remains pinned to 7.4.3.
- Build-size output is retained as an artifact or job summary.

### Phase 2: Fix PSRAM restart resource symmetry

**Status: Complete on this branch.** The asymmetry described in the original plan
has been fixed; this section now documents the intended design and the tests that
guard it, not outstanding work.

PSRAM-backed raw-data and JSON buffers are allocated in `begin()` through
`allocateRuntimeBuffers()` (idempotent, allocate-if-missing), freed and nulled by
`end()` through `releaseRuntimeBuffers()`, and reallocated by a later `begin()`.
Raw-data caching and the PSRAM JSON buffers therefore survive a restart; the
task-stack buffer is used only when a live allocation actually fails. The pure
helpers live in `MQTTRuntimeBufferLifecycle.h`, covered by
`test_mqtt_runtime_buffer_lifecycle`.

The design keeps symmetric runtime-resource operations:

- `allocateRuntimeBuffers()` called by `begin()` or a shared initialization
  path.
- `releaseRuntimeBuffers()` called by `end()` and initialization rollback.

The operations must be idempotent, handle partial allocation, and preserve the
existing graceful fallback when PSRAM allocation fails. Avoid changing client
or task shutdown behavior in this phase.

Tests and validation:

- Allocation success, partial failure, repeated allocation, and repeated
  release using a narrow allocator fake.
- Multiple `begin()/end()` cycles do not lose the raw-data buffer or permanently
  move JSON serialization onto the task stack.
- Initialization failure releases only resources owned by that attempt.
- Representative PSRAM and non-PSRAM firmware builds pass.
- A short hardware restart loop shows stable free heap and largest free block.

### Phase 3: Add binary MQTT preference migration fixtures

**Status: Complete on this branch.** The versioned loader, the frozen-layout
`static_assert`s, and the migration/fixture coverage described below are
implemented in `MQTTPrefsCodec.h`, `MQTTPrefsStorage.h`, and
`MQTTPrefsAtomicStore.h`, and host-tested by `test_mqtt_prefs_codec` and
`test_mqtt_prefs_atomic_store`. Unknown-newer files are preserved and corrupt
input is rejected without overwrite. Residual: the concrete filesystem adapter
and the `load/saveMQTTPrefs()` orchestration still live in `CommonCLI.cpp`
(platform-coupled by design); keep that adapter small. The description below is
retained as the record of intent.

Build deterministic tests around the versioned `/mqtt_prefs` loader before
upstream merges change `CommonCLI`, filesystem behavior, or preference structs.
Use checked-in, non-secret binary fixtures or byte arrays representing each
deployed layout.

Required cases:

- Pre-slot layout.
- Three-slot layout.
- Legacy headerless six-slot layout.
- Shorter version-1 payload with newer fields defaulted.
- Current version round trip.
- Truncated header and truncated payload.
- Invalid magic and implausible payload length.
- Unsupported newer version: run with safe defaults and preserve the original
  file without overwriting it.
- Migration failure or interrupted save leaves a recoverable source file.
- Existing credentials, slot ordering, and publish flags survive migration.

Prefer extracting a fork-owned serializer/decoder seam over host-compiling all
of `CommonCLI`. Keep the production adapter small and retain the frozen-layout
`static_assert`s.

Acceptance criteria:

- All known deployed layouts have a fixture and field-by-field expected result.
- Corrupt input cannot cause an out-of-bounds read or silent overwrite.
- A future `MQTTPrefs` layout change fails tests until its migration and fixture
  are added deliberately.

### Phase 4: Establish ownership and teardown test seams

**Status: Seams, ownership doc, and teardown tests landed; production rewiring
deferred to Phase 5 by explicit decision (scope: "seams + tests only").** The
fork-owned pure lifecycle state machine and narrow dependency seam
(`src/helpers/MQTTLifecycle.h`), the teardown-focused test matrix
(`test/test_mqtt_lifecycle/`), and the ownership model (`MQTT_OWNERSHIP.md`) are
in place. `MQTTBridge.cpp` was intentionally left untouched to keep the
merge-sensitive file free of churn until the Phase 5 change lands as one
reviewable unit. The invasive production work — publishing the plain-data
snapshot and repointing consumers at it, replacing the `volatile` handshakes
with a command queue / task notifications, and the cooperative-shutdown `end()`
rewrite with a `begin()` double-call guard — is carried into Phase 5.

Verified premise (with Phase 4 refinements): the loop task, WebConfig, CLI, and
`AlertReporter` currently read the MQTT task's live, mutable slot objects and
client counters cross-core without a lock or a published snapshot.
`getSlotStatusSnapshot()` is built on demand from live state despite its name —
the refactor must actually publish a plain-data snapshot, not assume one exists.
Refinements found while mapping the code (see `MQTT_OWNERSHIP.md` for
file:line references):

- The snapshot's `name`/`state` `const char*`s point at static rodata, so they
  are not dangling; the real hazards are reading the mutable `slot.preset`
  pointer value (Core 0 can null/reassign it) and `slot.client->getPublishOk()`
  (a client pointer Core 0 can `delete` during teardown).
- The live-bridge web reads are in the `MyMesh.cpp` app layer (`buildStatsJson`),
  not `WebConfigServer.cpp`, which reads only a compile-time constant.
- `end()` clears only the `s_mqtt_bridge_instance` singleton; the app's `bridge`
  pointer and `AlertReporter::_bridge` are not cleared, so instance reads can
  touch a torn-down bridge.

This phase is the safety net for cooperative shutdown. It must land before the
shutdown state machine.

#### Ownership model

Document one owner for each mutable runtime domain:

- MQTT task: clients, slot connection state, NTP client operations, publish
  counters, and the packet drain path.
- Loop task: CLI execution, preference persistence, WebConfig batch draining,
  and bridge lifecycle requests.
- Producer/radio context: packet staging before queue handoff.
- Async TCP context: request parsing and immutable response handoff only.

Replace cross-core `volatile` handshakes with an appropriate primitive:

- Task notifications or a command queue for one-way lifecycle/reconfigure/NTP
  requests.
- Atomics only for truly independent scalar state.
- Immutable published snapshots for WebConfig, CLI diagnostics, and alerting.
- A mutex only where ownership transfer or snapshot publication cannot express
  the operation cleanly.

In particular, loop/WebConfig code must not directly inspect mutable MQTT slot
objects or client counters. The MQTT task should publish a plain-data status
snapshot.

#### Narrow lifecycle fakes

Introduce interfaces or callbacks for only the dependencies needed to drive the
lifecycle deterministically:

- Clock/timer.
- Task start, stop request, acknowledgment, and timeout.
- MQTT client connect/disconnect and delayed callbacks.
- Queue depth/pop/requeue behavior.
- Runtime allocator and heap measurements.
- OTA coordinator/barrier.

Required teardown-focused tests:

- Stop while connecting, connected, publishing, retrying, renewing a token,
  running NTP, and applying a slot change.
- Callback delivered before stop, during stop, after disconnect, and after the
  stop acknowledgment.
- Duplicate stop, stop before full initialization, and restart after stop.
- Timeout/fallback behavior when the MQTT task or client does not acknowledge.
- No client, queue, buffer, or task access after its owner releases it.
- Current queue and diagnostic behavior is preserved unless a change is
  explicitly approved.

### OTA Teardown Barrier: release-critical scenario

**Status: Not started. This is the fix for a known shipping crash, not a
hypothetical hardening target.** With a broker down over `wss`, the abrupt
`vTaskDelete` in `end()` can kill the MQTT task inside mbedTLS; `destroySlotClients()`
then frees client buffers on a possibly-corrupted heap and OTA begins flashing
with no barrier — the observed teardown heap panic. There is no coordination
today between MQTT shutdown and flash writing beyond straight-line ordering on
the loop task.

Treat OTA teardown as a first-class test target throughout Phases 0, 2, 4, 5,
and 7. It is not merely another restart case.

The required invariant is: firmware erase/write must not begin until MQTT
shutdown has reached a safe acknowledgment point, and the bridge must not be
restarted while flash writing is active.

Required scenarios:

- OTA requested while MQTT is connecting, publishing, draining retries, or
  handling a callback.
- MQTT stop succeeds and OTA begins only after the teardown barrier.
- MQTT stop times out: OTA aborts safely rather than writing under uncertain
  ownership.
- OTA preflight or download aborts before flashing: the bridge restarts once and
  returns to the prior configured behavior.
- Successful OTA: no bridge restart is attempted before the device reboots.
- Power loss/reset at the platform-supported OTA boundaries retains a bootable
  partition; this portion requires hardware/platform validation.
- Repeated failed OTA attempts do not leak heap, duplicate WiFi callbacks, or
  leave the bridge permanently stopped.

Retain the prior teardown/heap-panic reproduction as a regression artifact if
available. OTA-related lifecycle tests are release gates for any future change
to bridge teardown, OTA sequencing, MQTT client lifetime, or task ownership.

### Phase 5: Implement cooperative MQTT shutdown

**Status: Minimal cooperative shutdown implemented on branch
`phase5/cooperative-mqtt-shutdown` (scope: "the smallest change that fixes the
OTA teardown panic as one reviewable unit"). Native suite green; the non-PSRAM
observer firmware smoke build compiles. NOT yet hardware-validated — that is the
Phase 7 gate — and the stop timeout is a Phase-0 placeholder (see below).**

What landed (wiring the Phase 4 `MQTTLifecycle` state machine into the bridge):

- `MQTTBridge` owns a `MQTTLifecycle::Coordinator` driven **only** by the loop
  task (Core 1) from `begin()`/`end()`. A nested `LifecycleOps` binds the pure,
  host-tested `Ops` spec to FreeRTOS/PsychicMqttClient.
- `end()` no longer blind-`vTaskDelete`s. It requests a cooperative stop; the
  MQTT task (Core 0) sees a new `volatile _stop_requested`, tears down its own
  clients **on Core 0 where the mbedTLS contexts live**, sets `_stop_acked`
  last, and self-terminates. `end()` waits (bounded) for the ack, then frees the
  queue/buffers. This removes the "kill the task mid-mbedTLS, then free client
  buffers on a corrupted heap" teardown path.
- Bounded stop timeout → reviewed fallback: on timeout the task is force-killed
  and torn down on Core 1 (the old behavior), but a **dirty latch** is set so
  `canFlashAfterStop()` is false and OTA flashing is withheld.
- `begin()` has a double-call guard and syncs the Coordinator to `Running`.
- OTA teardown barrier: `simple_repeater`'s deferred-OTA fire site aborts and
  resumes the bridge unless the preceding `end()` reported a clean stop.

Deferred (kept out of this reviewable unit; carried to a Phase 5b / Phase 6):
replacing the `volatile` NTP/reconfigure handshakes with a command channel, and
publishing a plain-data status snapshot to repoint the §1/§2 consumers in
`MQTT_OWNERSHIP.md` (the `AlertReporter`/`buildStatsJson` instance-pointer reads
that can still touch a torn-down bridge). Those are not required to fix the OTA
panic and would enlarge the merge-sensitive diff.

**Phase 0 dependency — CHARACTERIZED + FIXED 2026-07-19:** the flat
`MQTT_STOP_TIMEOUT_MS` = 8 s placeholder was **too small** (measured per-wss-slot
teardown ~5–6 s sequential → ~11–12 s at 2 slots, ~16 s at 3, ~27–30 s at 5; at
8 s healthy multi-slot stops tripped the dirty fallback and the OTA barrier
withheld flashing). Replaced with a **slot-scaled timeout** set per stop in
`end()`: `5 s + 8 s × enabled_slots` (`MQTT_STOP_TIMEOUT_BASE_MS` /
`MQTT_STOP_TIMEOUT_PER_SLOT_MS`, applied via `Coordinator::setStopTimeoutMs()`).
Hardware-verified: a 2-slot stop that force-timed-out pre-fix now acks clean in
~11.7 s within the 21 s budget. See "Hardware Characterization Results (Phase 0 &
Phase 7)".

The original plan of record follows. Replace direct task deletion with an
explicit lifecycle such as:

`Stopped -> Starting -> Running -> StopRequested -> Stopping -> Stopped`

The exact representation can differ, but it must provide:

- Idempotent start and stop requests.
- A stop request delivered through the established ownership channel.
- Cessation of new connects, publishes, retries, and reconfigurations.
- Ordered client/service shutdown on the MQTT task.
- A completion acknowledgment before the loop task releases queues, buffers, or
  other shared resources.
- A bounded timeout with clear diagnostics and a deliberately reviewed fallback.
- Safe restart after a completed stop.
- An OTA barrier that consumes the same completion acknowledgment.

Do not combine this phase with queue-loop deduplication, broad bridge cleanup,
or unrelated feature changes.

Acceptance criteria:

- All Phase 4 lifecycle tests pass, including delayed/stale callbacks.
- Characterized behavior from Phase 0 is preserved or differences are explicitly
  documented and approved.
- OTA teardown-barrier tests pass.
- Repeated hardware start/stop cycles show no downward heap or largest-block
  trend and no task/client-count growth.

### Phase 6: Expand request, queue, connection, and publication integration tests

**Status: Partial — branch `phase6/integration-tests` (draft PR, base `phase5`).**
The remaining *inline* decision points that were host-testable have been extracted
into the pure policy seams and covered:

- WiFi STA reconnect backoff moved out of `handleWiFiConnection()` into
  `MQTTConnectionPolicy::{wifiReconnectBackoffMs,wifiReconnectDue,
  nextWifiBackoffAttempt}` (behavior-preserving; adversarially reviewed for
  rollover/boundary equivalence) with `test_mqtt_connection_policy` cases.
- The (packet, raw) publication-outcome pairing named as
  `MQTTPacketQueuePolicy::queuedPacketPublished()` and wired at both queue-drain
  sites, with `test_mqtt_packet_queue_policy` cases (partial success = completed).
- `MQTTPublicationType` values frozen in `test_mqtt_topic_router`; the
  bridge-side `MQTTMessageType` alignment was already a compile-time `static_assert`.

The **WebConfig POST/result/reboot/stop state machine** (the largest gap) now
has a pure, host-tested SPEC — `src/helpers/WebConfigBatch.h` +
`test/test_webconfig_batch/` (merged to `webconfig`) — that faithfully
characterizes the accept/drain/result/reboot/stop decisions currently inline in
`WebConfigServer.cpp`. A line-by-line faithfulness review confirmed it; the
follow-up nits it raised are now resolved: the one stale line reference was
corrected, and the batch constants are literal-pinned by a freeze test
(`ConstantsMatchTheWebConfigServerSource`), so a spec/source constant drift trips
the native suite rather than going silent.

Like Phase 4's `MQTTLifecycle.h`, it is **spec-first and NOT yet wired**:
rewiring the hardware-tuned server to consume it (so it becomes load-bearing and
non-drifting) is a deliberately separate, hardware-validated follow-up.
**Porting constraint (fleet behavior) — keep-stale reboot deadline:** production
never clears `_reboot_at` on a new POST-accept (`WebConfigServer.cpp:699-710`) —
it clears it only on a full server stop/reset (`.cpp:235`) — so a prior
fully-OK reboot batch's armed deadline *survives into a later batch and still
fires*. `WebConfigBatch::finishRebootAt()` returns `0` for the no-reboot case, so
a naive `_reboot_at = finishRebootAt(...)` during wiring would silently **cancel**
that surviving reboot and change fleet behavior. The port must assign only when
the result is non-zero (preserving keep-stale semantics), or change the behavior
deliberately and cover it with a test. The hazard is documented at the
`finishRebootAt()` definition in the header.

Still open (each a good follow-up PR): wiring `WebConfigServer.cpp` to the
`WebConfigBatch` spec above, and the **queue-orchestration** behaviors (FIFO
ordering, evict/requeue-failure interplay, the two adapters' drop-vs-keep-head
divergence), which need a fake-queue harness. The original scope list follows.

After lifecycle ownership is stable, broaden deterministic integration coverage:

- WebConfig POST/result/reboot/stop behavior, including lost responses,
  duplicate request IDs, concurrent clients, partial command failure, and stop
  with an active handler.
- MQTT queue behavior for FreeRTOS and circular-buffer adapters: overflow,
  delayed retry, requeue failure, stale flush, queue ordering, and
  `millis()` rollover.
- Packet succeeds/raw fails and raw succeeds/packet fails.
- Connection backoff, stable reset, breaker probe, WiFi recovery, token renewal,
  and slot reconfiguration callback ordering.
- Topic and payload contracts at the bridge boundary, so enum/cast/adaptation
  mistakes are covered in addition to the pure helper tests.

Once both queue backends are covered by the same behavioral contract, consider
extracting a shared `processQueuedPacket()` that returns an outcome while each
backend retains pop/requeue/dequeue ownership. Do not make queue deduplication a
prerequisite for the stability work.

### Phase 7: Establish uptime, memory, and fault-injection gates

**Status: Not started.** The final validation gate; runs after the lifecycle and
OTA-barrier work is in place.

Use hardware soak tests to validate the already-tested design, not to discover
basic lifecycle errors for the first time.

Run at least one constrained non-PSRAM board and one PSRAM board through:

- Stable broker operation.
- Broker unavailable, rejecting authentication, and flapping.
- WiFi loss/recovery and credential changes.
- TLS handshake failures and repeated reconnect backoff.
- Queue saturation and slow broker behavior.
- Repeated slot reconfiguration and full bridge stop/start.
- WebConfig start/save/stop cycles.
- JWT renewal and NTP failure/recovery.
- OTA success, preflight abort, download failure, teardown timeout, and resume.
- At least one test crossing the 32-bit `millis()` rollover boundary, accelerated
  where hardware time injection is available.

Record and threshold:

- Minimum free internal heap.
- Largest free internal block.
- MQTT task stack high-water mark.
- PSRAM free/largest block where available.
- Queue and outbox high-water marks.
- Connect/disconnect/retry/publish counters.
- Watchdog and reset reason.
- Task/client counts across restart cycles.

Recommended gates are a 72-hour fault-injection run followed by a seven-day
stable run. Store machine-readable time series and a concise summary artifact.
Avoid enabling high-volume diagnostic logging in production builds.

## Continuous Upstream-Merge Discipline

Apply these practices throughout every phase:

- Keep fork-owned logic in additive helper files and keep adapters in
  upstream-heavy files small.
- Avoid drive-by formatting, renames, and unrelated cleanup in
  `MQTTBridge.cpp`, `CommonCLI`, and the role-specific `MyMesh` files.
- Use single-purpose commits with messages that describe their full scope.
- Merge upstream frequently enough that conflicts remain attributable.
- Before each upstream merge, record the native-test, representative-build,
  firmware-size, and persistence-fixture baseline.
- After the merge, run native tests, both MQTT smoke builds, preference fixtures,
  and the relevant lifecycle/OTA tests before resolving the merge as complete.
- Review semantic behavior at adapters even when Git reports no textual
  conflict; upstream signature, lifetime, and task-context changes can invalidate
  fork assumptions silently.
- Reuse resolutions only after revalidating them against the new upstream code.
- Extract repeater/room-server WebConfig ownership into a shared fork helper when
  that integration next requires material change; do not perform a standalone
  broad move solely for aesthetics.

## Explicitly Deferred Debt

These items are recognized but are not prerequisites for the sequenced
stability work:

- Shared packet-drain processing between FreeRTOS and circular-buffer backends.
- Repeater/room-server WebConfig integration duplication.
- Moving the WebConfig HTML generator out of the common ESP32 build path.
- The secret-placeholder edge case for a credential exactly equal to the UI
  sentinel.
- Secure setup-AP authentication beyond the currently documented open/optional
  PSK threat model.

Revisit an item when its code is already being changed, when operational data
raises its priority, or when tests make the refactor substantially safer.

## Completion Definition

This roadmap is complete when:

- PR CI protects native logic and both representative firmware memory paths.
- Every deployed MQTT preference layout has a passing migration fixture.
- Runtime resources survive repeated start/stop cycles symmetrically.
- Cross-core ownership is explicit and diagnostics consume immutable snapshots.
- Cooperative shutdown passes deterministic lifecycle and OTA-barrier tests.
- Hardware fault-injection and stable soaks meet reviewed memory, stack, task,
  and reliability thresholds.
- Upstream merges use the same automated gate and do not require broad rewrites
  of fork-owned behavior.
