# Provisioning File Support

A **provision file** is a "defaults package" that jump-starts a freshly flashed
node — radio settings, `path.hash.mode`, MQTT slots, alert config — without
forking the firmware or baking private brokers into source. It is a plain text
file of ordinary CLI commands, stored on the node as `/provision`. Each line is
run through the same CLI interpreter as the serial console, so validation,
privilege rules, and persistence are identical to typing the commands yourself.

## File format

```
#meshcore-provision v1
# Region defaults for Example Mesh. Comments and blank lines are ignored.
set path.hash.mode 2
set freq 906.875
set mqtt1.preset analyzer-us
set mqtt3.preset custom
set mqtt3.server mqtt.example.org
set mqtt3.port 8883
set alert.wifi 30
set alert.mqtt 240
```

Rules:

- The **first non-blank line must be** `#meshcore-provision v1`. A file with a
  newer version number is refused (and kept on flash untouched).
- Lines starting with `#` and blank lines are skipped.
- Max line length: **159 characters** (the serial command buffer size). Longer
  lines are counted as errors and skipped.
- Max file size: **4 KB** — enforced by both `provision fetch` and apply.
- Every other line is fed verbatim to the CLI. Unknown or invalid commands
  produce an error and are skipped, so the format is forward-compatible: a file
  written for newer firmware still applies everything the older firmware
  understands.

## CLI commands

| Command | Behavior |
|---|---|
| `provision` | Status: file present, size, header version, applied-marker present |
| `provision show` | Serial: prints the whole file to the console. Remote: paged — `provision show <start-line>` for the next page |
| `provision apply` | Runs the file with **your** privileges (see below). Ignores the applied-marker |
| `provision remove` | Deletes `/provision` and the `/provision_done` marker |
| `provision begin` / `provision end` | Serial-only paste capture (see below) |
| `provision fetch <url> [insecure]` | Downloads a file over HTTPS/HTTP (observer/MQTT builds only) |

### Privileges

`provision apply` runs each line with the invoker's privilege level. Applying
from the **serial console** can change everything, including radio parameters.
Applying **remotely** (over the mesh) is allowed — a region file that only
touches alert/MQTT settings is a supported use case — but serial-only commands
such as `set freq` are rejected on those lines, exactly as if you had typed
them remotely.

Some commands are never run from a provision file, on any path:
`erase`, `start ota`, `ota check/update`, `password`, `set prv.key`,
`reboot`/`clkreboot`/`poweroff`/`shutdown`, `region load` (interactive mode),
and `provision` itself. Blocked lines are counted as "skipped" in the summary.

### Boot auto-apply

At the end of boot, if `/provision` exists and the `/provision_done` marker
does not, the node runs the file with full serial privileges (this is the
first-flash jump-start path), writes the marker, and reboots so all settings —
including radio parameters — take effect from a clean start. The marker is
written *before* the file runs, so a bad file can never cause a reboot loop.
Manual `provision apply` ignores and never touches the marker; use
`provision remove` to clear both file and marker.

### Getting a file onto a node

**With WiFi (ESP32 observer/MQTT builds)** — the typical flow after flashing a
stock binary:

```
set wifi.ssid MyNetwork
set wifi.pwd MyPassword
provision fetch https://region.example/defaults.txt
provision show
provision apply
```

`fetch` validates HTTPS against the CA roots already bundled with the firmware
(Google Trust Services, ISRG/Let's Encrypt). Plain `http://` URLs and
`provision fetch <url> insecure` (skip certificate validation) are allowed —
the file is only *stored*, never auto-applied, so you can inspect it with
`provision show` before trusting it. Fetch refuses to overwrite `/provision`
with anything that is oversized or missing the header.

**Without WiFi (nRF52 / RP2040 / any build)** — paste over the serial console:

```
provision begin
<paste the file>
provision end
provision apply        (or just reboot for auto-apply... marker permitting)
```

Between `begin` and `end`, console lines are written to `/provision` instead
of executed. The header line is validated first and the same size/line caps
apply. `provision end` closes the file **without** applying it.

### Interaction with newer-firmware config

If `/mqtt_prefs` on the node was written by a newer firmware version than the
one running (a downgrade), `provision apply` refuses to run the whole file:
applying would rewrite the newer config in this firmware's older format.

## Note for custom-build maintainers

A private broker should be configured as a **`custom` slot**
(`set mqttN.preset custom` plus `server`/`port`/`username`/`password`/`token`/
`audience` — all persisted in `/mqtt_prefs`), **not** a source-baked named
preset. Named presets die on OTA to stock firmware when `findMQTTPreset()`
misses; custom slots survive any firmware. A provision file is the easy way to
distribute exactly that configuration.

For rebuilders, the existing `MQTT_DEFAULT_*` build flags (see
`src/helpers/MQTTDefaults.h`) and the standard `LORA_FREQ`/`LORA_SF`/etc.
flags remain available for baking defaults in at compile time.
