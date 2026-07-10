# Provisioning File Support — Implementation Plan

Goal: let a region (or a custom-build maintainer) supply a "defaults package" that
jump-starts a freshly flashed node — radio settings, `path.hash.mode`, MQTT slots,
alert config — without forking the firmware or baking private brokers into source.

Design decision (settled): the package is a **text file of CLI commands**, not a
packed binary. The CLI interpreter (`CommonCLI::handleCommand`) is already the
canonical, validated, self-persisting way to set every relevant setting. A binary
blob would couple third-party-authored files to the `MQTTPrefs`/`NodePrefs` struct
layouts, which are the fork's most fragile, fleet-critical surface (see the frozen
legacy layouts and static_asserts in `src/helpers/CommonCLI.h`). **This plan makes
zero changes to any persisted struct layout.**

---

## File format: `/provision`

Stored in the node's filesystem as `/provision`. Plain ASCII text:

```
#meshcore-provision v1
# Optional comment lines; blank lines ignored.
set path.hash.mode 2
set freq 906.875
set mqtt1.preset analyzer-us
set mqtt3.preset custom
set mqtt3.host mqtt.example.org
set mqtt3.port 8883
set alert.wifi 30
set alert.mqtt 240
```

Rules:
- First non-blank line MUST be `#meshcore-provision v1` (exact prefix match on
  `#meshcore-provision v`, integer version follows). Unknown major version →
  refuse to apply, keep the file.
- Lines starting with `#` and blank lines are skipped.
- Max line length: **159 chars** (matches the serial console command buffer,
  `static char command[160]` in `examples/simple_repeater/main.cpp`). Longer
  lines are reported as errors and skipped.
- Max file size: **4 KB** (enforced by fetch and by apply).
- Each remaining line is fed verbatim to `CommonCLI::handleCommand()`.
  Unknown/invalid commands produce an error reply and are skipped — the format
  is forward-compatible by construction.

Applied-marker: `/provision_done` (empty file). Its presence suppresses boot-time
auto-apply. `provision apply` (manual) ignores the marker and re-runs.

---

## Phase 1 — core runner + CLI commands (all platforms)

**Files: `src/helpers/CommonCLI.h`, `src/helpers/CommonCLI.cpp`**

New private method `CommonCLI::runProvisionFile(FILESYSTEM* fs, uint32_t sender_timestamp, char* reply)`:
1. Open `/provision`; validate size cap and header line.
2. If `_mqtt_prefs_hold` is set (prefs written by newer firmware — see
   `saveMQTTPrefs()`), refuse with an error reply. Applying would write v1-format
   MQTT prefs over newer config.
3. For each command line:
   - Reject blocklisted commands (skip + count as error): `erase`, `start ota`,
     `password `, `set prv.key`, `provision` (no recursion), `reboot`/`restart`
     if such commands exist at app level. Everything else passes through,
     including `set freq` — privilege is enforced by the existing
     `sender_timestamp == 0` guards in `handleCommand` itself.
   - Call `handleCommand(sender_timestamp, line, line_reply)` with a local
     160-byte reply buffer. **Reentrancy check required**: verify the
     `handleCommand` → `handleObserverCommand` path uses no static/shared
     buffers that the outer `provision` command invocation is also using. If it
     does, copy the provision subcommand args before iterating.
   - Log each line + reply via `MESH_DEBUG_PRINTLN`; count replies beginning
     with `Err`/`ERR` as failures.
4. Write summary into `reply`: `Provision: N applied, M failed, K skipped`.

New CLI command family in `CommonCLI::handleCommand` (available on all builds,
not just MQTT ones — plain repeaters benefit from `path.hash.mode` etc.):

| Command | Behavior |
|---|---|
| `provision` | Status: file present? size? header version? marker present? |
| `provision show` | Print the file contents (paged into the reply buffer; for serial use, print full file to Serial and a summary in `reply`) |
| `provision apply` | Run the file with the **invoker's** `sender_timestamp`. Remote invocations therefore cannot change radio params / prv.key — same policy as typing the commands. Does not touch the marker. |
| `provision remove` | Delete `/provision` and `/provision_done`. |
| `provision begin` / `provision end` | (Optional but recommended; serial-only, `sender_timestamp == 0`.) Paste mode: between `begin` and `end`, console lines are written to `/provision` instead of executed (header line validated first, same size/line caps). `provision end` closes the file without applying. This is the only file-creation path for non-WiFi builds (nRF52/RP2040), where fetch (no WiFi) and the Phase 4 trailer (DFU signing) are unavailable — it needs nothing but a terminal emulator. |

Privilege: `provision apply`/`remove` require the same admin context as other
config commands (they arrive through the normal authenticated CLI path; no extra
gating needed). Do NOT restrict apply to serial-only — a remotely applied region
file that only touches alert/MQTT settings is a supported use case.

**Boot auto-apply — files: `examples/simple_repeater/main.cpp` (or `MyMesh.cpp`
near the end of `begin()`), `examples/simple_room_server/` equivalent:**

At the **end of setup/begin** (after radio, filesystem, CLI, and bridge are fully
initialized — set-command callbacks like `updateAdvertTimer` and bridge restarts
must be safe to fire):
1. If `/provision` exists and `/provision_done` does not:
   - Call `runProvisionFile(fs, /*sender_timestamp=*/0, reply)` — full serial
     privileges, so region radio defaults apply. This is the jump-start path.
   - Write `/provision_done` **before** rebooting (prevents a reboot loop).
   - Reboot (`_board->reboot()`) so all settings take effect from a clean start.
2. Keep this hook outside `#ifdef WITH_MQTT_BRIDGE` — it is useful on plain
   repeater builds too (file can be seeded by Phase 4 or a LittleFS image).

---

## Phase 2 — `provision fetch <url>` (ESP32 / `WITH_MQTT_BRIDGE` builds only)

Every variant that defines `WITH_MQTT_BRIDGE` is ESP32-family (heltec_v3/v4,
lilygo t-beam/t3s3/tlora, rak3112, station_g2, xiao_s3, …), so Arduino-ESP32
`HTTPClient` + `WiFiClientSecure` are available.

**File: `src/helpers/CommonCLI_Observer.cpp`** (alongside the other
`WITH_MQTT_BRIDGE` CLI handling):

- `provision fetch <url>` — requires `WiFi.status() == WL_CONNECTED` (WiFi is
  managed by `MQTTBridge`; if not connected, reply with an actionable error:
  "set wifi.ssid/wifi.password first").
- HTTPS: validate against the CA roots already bundled in
  `src/helpers/MQTTPresets.h` (`GTS_ROOT_R4`, `ISRG_ROOT_X1`) — try each. Plain
  `http://` and `provision fetch <url> insecure` (skip cert validation) are
  allowed; the file is inspectable via `provision show` before apply, so the
  trust decision happens at apply time.
- Enforce the 4 KB cap while streaming; validate the `#meshcore-provision v1`
  header **before** writing `/provision`; reject otherwise (don't clobber an
  existing good file with garbage).
- Fetch only stores the file. It never auto-applies and never touches the
  marker. Reply: byte count + line count + "run 'provision apply' to apply".

Expected operator flow (matches the Discord thread's target UX):
flash stock bin → USB console → `set wifi.ssid` / `set wifi.password` →
`provision fetch https://region.example/defaults.txt` → `provision show` →
`provision apply`.

---

## Phase 3 — compile-time defaults for rebuilders (small, independent)

Extend the existing `build_flags` override pattern from
`src/helpers/MQTTDefaults.h` to the non-MQTT settings custom builders bake in:

- `-D NODE_DEFAULT_PATH_HASH_MODE=2` — consumed where `NodePrefs` defaults are
  established (the `path_hash_mode` default; clamp 0–2 as at
  `CommonCLI.cpp:291`).
- Optionally `NODE_DEFAULT_FREQ` / `NODE_DEFAULT_SF` etc. if trivially wireable
  into the existing prefs-defaults path — do not restructure anything for this.

This covers the "rebuild-from-source" audience without them patching code, and
is independent of Phases 1–2.

---

## Phase 4 (optional, later) — `.bin`-appended provision trailer (ESP32 only)

Only if zero-interaction first-flash provisioning proves necessary. Do not block
Phases 1–3 on this.

- Trailer format appended to the app `.bin`: magic `MCPV1\0` + `uint16` payload
  length + payload (the text file) + CRC32.
- On boot (ESP32, before the Phase 1 auto-apply check): if `/provision` absent
  and `/provision_done` absent, locate the end of the running app image
  (`esp_ota_get_running_partition()` + image length from the image header /
  `esp_image` APIs), read the trailer, verify magic + CRC, write payload to
  `/provision`. Phase 1's auto-apply then handles the rest.
- Host tool `tools/append_provision.py` (append trailer to a built `.bin`).
- Explicitly ESP32-only: nRF52 DFU zips are signed against the exact image and
  RP2040 UF2 has no equivalent channel. Document this limitation.

---

## Edge cases / guardrails checklist

- [ ] `_mqtt_prefs_hold` set → `provision apply` refuses (whole file, not per-line).
- [ ] Reboot loop impossible: marker written before the post-auto-apply reboot.
- [ ] Blocklist enforced on every apply path (boot and manual).
- [ ] Oversized file / oversized line / missing header / future version → clear
      error, file preserved, nothing partially applied before the header check.
- [ ] Fetch failure modes (no WiFi, TLS failure, HTTP ≠ 200, size cap) each give
      distinct error replies.
- [ ] Reentrant `handleCommand` verified safe (no shared static buffers).
- [ ] `provision` commands excluded from any command paths that log/replay in
      ways that could recurse.

## Verification (per repo workflow)

- No persisted struct layouts change → the host migration harness needs no new
  cases, but run it to confirm it still passes.
- Build at least: one ESP32 observer target (e.g. `heltec_v3` repeater_observer_mqtt)
  and one non-MQTT/non-ESP32 target (nRF52 repeater) to prove the
  `#ifdef` seams hold. Note the known pre-existing build failures in the repo
  memory — don't chase those.
- On-target smoke (ESP32 dev board):
  1. Fresh FS + `/provision` seeded via serial or fetch → reboot → confirm
     auto-apply ran once, marker present, settings persisted, second reboot does
     not re-apply.
  2. `provision fetch` against a real HTTPS URL (S3/GitHub raw) and against a
     bad URL.
  3. Remote CLI `provision apply` of a file containing `set freq` → confirm freq
     line is rejected (serial-only guard) while other lines apply.

## Docs

- New `PROVISIONING.md` (user-facing): file format, example region file, the
  fetch/show/apply flow, blocklist, ESP32-only notes for fetch.
- One-paragraph note for custom-build maintainers: **a private broker should be
  a `custom` slot (host/port/user/pass/token/audience persist in `/mqtt_prefs`),
  not a source-baked named preset** — named presets die on OTA to stock firmware
  when `findMQTTPreset()` misses; custom slots survive any firmware. This advice
  applies today, before any of the above ships.
