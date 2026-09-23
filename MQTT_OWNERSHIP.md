# Observer task ownership

Current implementation: `feat/observer-reliability`, based on `5c0da89117cdddcafbe1c9c5803fa92082ba7e2f`, September 2026. The framework remains Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7 on PlatformIO espressif32 6.11.0.

## Owners and publication boundaries

| Domain | Owner | Handoff |
|---|---|---|
| SDK handles, slot transitions, config application, MQTT publication and NTP probes | MQTT bridge worker | SDK callbacks enqueue copied `MqttSlotEvent` records; the worker applies them |
| SDK connection/stop acknowledgment flags | SDK MQTT task and wrapper caller | `std::atomic<bool>`; lifecycle effects never run inside a callback |
| Durable preferences, CLI, portal batch execution, lifecycle coordinator | Arduino loop task | Candidate edit, verified save, then durable RAM commit; revisioned config mailbox to worker |
| RF packet staging and score, radio/board telemetry | Arduino loop task | Value-copy FreeRTOS packet queue; one-second telemetry snapshot |
| Diagnostic slot state and counters | MQTT bridge worker | Pointer-free runtime snapshot; readers copy under a short lock, then format |
| Portal configuration responses | AsyncTCP task | Reads a portal-owned view published by `tick()` between loop commands; mutex also protects batch state |
| SNMP OID storage | MQTT bridge worker | Mesh task publishes one radio/name/version record; worker copies it before servicing a request |
| Network event observations | Wi-Fi event task | Atomic outage/status publication; credentials remain with network owner |
| OTA manifest check | Dedicated task | Cached asynchronous result with request ID and completion time |

Core affinity is not ownership: the SDK MQTT task and bridge worker may both run on Core 0 and still require synchronization. The queue and snapshot publishers above run in task context, not from an ISR.

`ObserverMailbox`, `ObserverConfigMailbox`, `MqttEventChannel` and `ObserverAsyncJob` share `ObserverLock`: a statically allocated FreeRTOS mutex on ESP32 and `std::mutex` in host tests. Every holder is a task, never an ISR, so the lock does not disable interrupts; priority inheritance covers a preempted holder. No allocation, formatting, flash write, SDK call or network operation runs under it. The worker's runtime snapshot publishes every pass, but its heap fields come from a once-per-second sample so the heap walk is not paid per iteration. `MQTTPrefs` is 2,880 bytes in the current layout; the bridge retains two copies (pending and applied). The portal retains another copy while its object exists. These costs need to be included in non-PSRAM heap measurements.

## Event and configuration ordering

Callbacks capture an incarnation when a client is allocated and copy their error fields before the SDK event expires. They never mutate slots or retain SDK event pointers. Retired-incarnation and stopped/quarantined-client events are ignored. A start attempt enters `Starting` before calling the driver. A failed call restores the prior state without claiming the credential was applied.

The event channel holds 32 records. Overflow records a count and affected-slot mask. The worker clears the affected slot's connected state and stops its client before another publication cycle. A proven stop can be retried normally; an unproven stop retains its resources. Queue loss never means an assumed connected session.

Configuration and the accumulated slot-reconfigure mask travel together. The worker consumes one complete revision at a safe point. Other runtime settings and radio metadata are sampled between loop commands, at most every 100 ms. Failed and indeterminate saves never publish a candidate to the running worker; indeterminate flash outcomes still require reboot/recovery to determine which image is durable on storage.

`get mqtt.runtime` reports runtime sample sequence/age, desired/applied configuration revisions and event-overflow count. Revision equality means the worker consumed the configuration, not that a broker accepted it. The whole report is a sampled value: if the worker is blocked in the SDK, its age grows and a newly queued revision may not yet appear.

The packet capture task reads loop-owned durable settings and published slot admission state. It never reads a worker-owned client or mutable slot. QoS0 publish acceptance retains its existing meaning; no delivery guarantee or public broker schema has changed.

## Stop and restart

The loop task requests stop through `MQTTLifecycle::Coordinator`. The worker tears down clients and acknowledges only after their stops are proven. The task trampoline publishes its final acknowledgment immediately before self-deletion. Resource cleanup follows that acknowledgment.

A timeout enters `StopUnproven`. There is **no forced worker deletion** and no freeing of reachable clients, tokens, packet buffers or task state. OTA flashing is withheld. A late acknowledgment permits cleanup and a later restart; an unexpected failed SDK stop quarantines the client for the boot. Do not turn a timeout into permission to free resources.

Client handles can be reused across compatible configuration changes, but TLS session buffers are allocated and released as connections open and close. Retaining a wrapper does not guarantee persistent mbedTLS session allocations. The existing OTA settle delay is retained; native/build results do not establish idle-task reclamation or device heap behavior.

## Diagnostics

`get mqtt.ntp.diag` queues a probe and returns immediately. Repeat it to retrieve the completed result, identified by request ID and age. Results are cached for 30 seconds; after expiry, the next call starts a new probe. The probe never sets the clock. Forced primary NTP synchronization also returns immediately, and shares the completion-based retry scheduler with ordinary synchronization.

`ota check` follows the same request/result pattern, caching results for 60 seconds. A queued or running check cannot authorize flashing. If `ota update` has to start a check, the app loop arms `OtaUpdateFollowUp`, polls `MainBoard::otaCheckInProgress()`, and schedules the ordinary deferred flash itself once the cached dry run reports an applicable build; a refusal or a check that does not settle within two minutes is reported on the console and alert channel instead. Actual flashing still uses the existing deferred stop/barrier path and re-fetches the manifest with certificate verification. The check remains advisory.

The portal terminal uses the same commands and replies; repeat a queued diagnostic command to poll its job. A completed portal command batch means its CLI commands returned, not that an asynchronous probe finished.

## Validation and release gate

Host tests exercise the production snapshot/config/event/job helpers, transition effects, checked client initialization, NTP scheduling, and existing persistence/lifecycle policies. ASan/UBSan run in `native_sanitized`; those sanitizers do not detect all data races or validate the precompiled SDK. Concurrent host tests check coherent copies and ordering, while firmware builds check the FreeRTOS adapter and framework integration.

Observer PR smoke coverage includes repeater, room server, Ethernet, PSRAM and a non-observer target. Both observer release workflows require the reusable host verification workflow and preset parity before publication. Parity compares the candidate SHA with the sibling channel. These local workflow checks are not evidence of a live GitHub run.

Before fleet deployment, bench-test portal polling during reconfiguration, fast callbacks, broken brokers/DNS/NTP, queue saturation, stop during connect/probe, repeated start/stop, and OTA abort/resume on non-PSRAM, PSRAM and Ethernet boards. Record loop gaps, heap/largest block, stack high-water marks and task/client counts. Run power-cut migration tests separately.

## History and upstream seams

- [Earlier ownership phase plan](docs/observer-ownership-history.md): historical hazards and proposed phases.
- [Stability/testability handoff](STABILITY_TESTABILITY_HANDOFF.md): historical device evidence; use its recorded board/date/revision, not as proof of this branch.
- [Observer implementation](MQTT_IMPLEMENTATION.md): operating/configuration behavior.
- [Upstream restoration history](RESTORE_UPSTREAM_NOTES.md): earlier restoration notes, with current-source correction.

Keep observer preference layouts and shared helpers outside upstream `NodePrefs`. CommonCLI's serializer/candidate hooks remain narrow; application integration retains role-specific mesh behavior. Do not replace the framework, remove stop guards, or expand the private WebSocket ABI shim without revalidating the pinned SDK.
