# Observer crash collector

Read-only MQTT subscriber for MeshCore "MQTT observer" firmware. It watches the
per-node status topics and stores only diagnostic events: crash reports,
self-heal reboots, and abnormal reset causes. High-volume packet, raw, and
neighbor topics are never subscribed.

## What it keeps

A status message is kept when at least one of these is true:

- a `crash` object is present (published once after a panic, read back from the
  on-flash core dump);
- `stats.selfheal` is present and `stats.selfheal_n` is greater than zero;
- `stats.reset_reason` is one of `panic`, `int_wdt`, `task_wdt`, `wdt`, or
  `brownout`.

Everything else is discarded without a disk write and without a log line. This
includes normal `sw`, `poweron`, `ext`, `deepsleep`, `sdio`, and `unknown`
resets, status messages with no `stats`, malformed JSON, non-object JSON, and
payloads larger than `OBSERVER_CRASH_MAX_PAYLOAD`.

The default subscription is `meshcore/+/+/status`, so the broker filters out
`meshcore/<IATA>/<pubkey>/packets`, `/raw`, and `/neighbors`.

## Output

One compact JSON object per line in an append-only JSON Lines file. The only
keys that can appear are:

| Key | Meaning |
| --- | --- |
| `received_at` | Collector receive time, UTC ISO 8601 |
| `kinds` | Event kinds: `crash`, `selfheal`, `reset` |
| `iata` | IATA segment from the topic |
| `node_id` | `origin_id` (lowercased when it is 64 hex), else the topic segment |
| `node_name` | `origin` |
| `model` | `model` |
| `firmware_version` | `firmware_version` |
| `crash` | The crash object, unchanged, stored once per unique crash |
| `reset_reason` | `stats.reset_reason` |
| `selfheal` | `stats.selfheal` |
| `selfheal_n` | `stats.selfheal_n` |
| `uptime_secs` | `stats.uptime_secs` |
| `heap_min` | `stats.heap_min` |
| `internal_heap` | `stats.internal_heap` |
| `heap_largest` | `stats.heap_largest` |
| `mqtt_slots_up` | `stats.mqtt_slots_up` |
| `mqtt_slots_total` | `stats.mqtt_slots_total` |

Absent fields are omitted. The crash object is the firmware's own JSON:

```json
{"task":"loopTask","pc":"400d1a2b","exc":28,"va":"00000010","sha":"deadbeef","bt":"400d1a2b 400d5678"}
```

`pc` and `va` are hex strings, `exc` is the Xtensa `EXCCAUSE` or RISC-V
`mcause`, `sha` is the first 8 hex characters of the crashing image's ELF
SHA-256, and `bt` is a space-separated list of backtrace words. A trailing `!`
inside `bt` means the firmware flagged the backtrace as corrupt. These keys come
from `src/helpers/CrashReportFormat.h` (`formatJson()`, lines 110-141).

Example line:

```json
{"received_at":"2026-11-11T15:44:48.123456+00:00","kinds":["crash","reset"],"iata":"LAX","node_id":"<64 hex>","node_name":"Some Node","model":"Heltec V4 OLED","firmware_version":"v1.18.0.12-observer-beta-dev-3476485","crash":{"task":"loopTask","pc":"400d1a2b","exc":28,"va":"00000010","sha":"deadbeef","bt":"400d1a2b 400d5678"},"reset_reason":"panic","uptime_secs":70,"heap_min":126160,"internal_heap":128548,"heap_largest":118772,"mqtt_slots_up":5,"mqtt_slots_total":5}
```

### Disk writes

The file is opened, one line is appended, flushed, and closed only when an event
is kept. There is no per-message write, no heartbeat file, no database, and no
periodic snapshot. Disk size is checked only after a kept write.

When the file reaches `OBSERVER_CRASH_MAX_BYTES` (default 20 MB) it is renamed
to `<name>.1`, replacing any older `.1`; the next event starts a new file. At
startup the dedup state is rebuilt by reading `<name>.1` and then `<name>`, so a
restart does not store the same crash again. Only the current file and its one
rotation are kept.

### Deduplication

A `(reset_reason, selfheal, selfheal_n)` report is stored again only when the
node rebooted since the last stored report. Because `reset_reason` and the
self-heal record are fixed for a boot, a change in that tuple is itself proof of
a reboot. For an identical tuple, a reboot is detected when:

- the boot-time estimate moves by more than 30 seconds. The estimate is the
  status `timestamp` minus `uptime_secs`. The first status after boot is
  published at connect, about 70 seconds of uptime in the firmware example, so
  a real reboot moves the estimate by at least that much. Thirty seconds
  absorbs the one-second uptime resolution, scheduler jitter, and small NTP
  steps without swallowing a real reboot; or
- `uptime_secs` goes down, when a boot-time estimate is unavailable.

If `uptime_secs` is missing on both sides, only the report tuple is compared:
the same tuple is a duplicate, a different tuple is a new boot. After a process
restart, boot times are not reconstructed from collector receive times, so the
first post-restart message is compared by tuple and uptime only. A reboot that
happened while the collector was down and produced the same tuple with a higher
uptime cannot be detected; the next distinct tuple is.

A crash is identified by node id plus the canonical crash object, which covers
the ELF SHA prefix, `pc`, `bt`, and `task` (and also `exc` and `va`). The same
crash redelivered by the broker, after a reconnect, or after a collector restart
is not stored again. A crash object is written only on the record that first
reports it.

Both dedup structures are bounded: an LRU of `OBSERVER_CRASH_NODE_CACHE` nodes
(default 20,000), and an LRU of four times that many crash signatures in total.
A broker with tens of thousands of nodes therefore cannot grow memory without
limit.

## Configuration

All settings are environment variables. There is no configuration file parser.

| Variable | Default | Description |
| --- | --- | --- |
| `OBSERVER_CRASH_HOST` | required | Broker hostname or IP address (no scheme) |
| `OBSERVER_CRASH_PORT` | `1883`, or `8883` when TLS is on | Broker port |
| `OBSERVER_CRASH_TLS` | `false` | Enable TLS. For WebSockets this means `wss` |
| `OBSERVER_CRASH_WS` | `false` | Use MQTT over WebSockets instead of plain TCP |
| `OBSERVER_CRASH_WS_PATH` | `/mqtt` | WebSocket path (for example `/mqtt` or `/ws`) |
| `OBSERVER_CRASH_USERNAME` | unset | MQTT username |
| `OBSERVER_CRASH_PASSWORD` | unset | MQTT password. Takes precedence over the password file |
| `OBSERVER_CRASH_PASSWORD_FILE` | unset | File containing the MQTT password. Only trailing CR/LF is stripped |
| `OBSERVER_CRASH_CLIENT_ID` | random | MQTT client id. Default is `observer-crash-collector-<8 hex>` |
| `OBSERVER_CRASH_TOPIC` | `meshcore/+/+/status` | Subscription filter |
| `OBSERVER_CRASH_KEEPALIVE` | `60` | MQTT keepalive seconds |
| `OBSERVER_CRASH_CA_FILE` | system trust store | CA bundle for TLS |
| `OBSERVER_CRASH_TLS_INSECURE` | `false` | Skip certificate verification. Do not use in production |
| `OBSERVER_CRASH_OUTPUT` | `crash-events.jsonl` | Output JSON Lines path |
| `OBSERVER_CRASH_MAX_BYTES` | `20971520` (20 MB) | Rotation threshold |
| `OBSERVER_CRASH_MAX_PAYLOAD` | `65536` | Maximum accepted payload in bytes |
| `OBSERVER_CRASH_NODE_CACHE` | `20000` | Maximum nodes kept in dedup memory |
| `OBSERVER_CRASH_BACKOFF_MAX` | `60` | Reconnect backoff cap in seconds |

The client subscribes with QoS 0, uses a clean session, sets no last will, and
never publishes. Reconnects use capped exponential backoff with jitter and
resubscribe on every successful connect. Credentials are never written to
stdout, stderr, the output file, or exception messages.

## Running

```sh
python3 -m pip install 'paho-mqtt>=2,<3'
export OBSERVER_CRASH_HOST=mqtt.example.net
export OBSERVER_CRASH_OUTPUT=/var/lib/observer-crash-collector/events.jsonl
python3 collector.py
```

Offline processing of captured lines, without a broker:

```sh
OBSERVER_CRASH_OUTPUT=/tmp/events.jsonl \
  python3 collector.py --stdin < samples.txt
```

Each input line is `topic<TAB>payload`. `--dry-run` is an alias for `--stdin`.
Both run through the same filter, dedup, and append path; output still goes to
`OBSERVER_CRASH_OUTPUT`.

Summarize stored events:

```sh
OBSERVER_CRASH_OUTPUT=/var/lib/observer-crash-collector/events.jsonl \
  python3 collector.py --summary
```

`--summary` groups crash signatures by firmware version, ELF SHA prefix, task,
and `pc`, with event counts, distinct node counts, and first/last receive times.
It also prints self-heal causes and abnormal reset reasons per firmware version.

Stop the process with SIGTERM or SIGINT; it disconnects and exits with status 0.

## Decoding a crash

`bt` holds call-return addresses, not exact source line PCs. The ELF must be the
exact build whose SHA-256 starts with the 8-character `sha` prefix; another
build can map the same address to the wrong function. The summary prints a
ready-to-paste hint:

```sh
xtensa-esp32s3-elf-addr2line -pfiaC -e <firmware.elf matching sha deadbeef> 400d1a2b 400d1a2b 400d5678
```

Replace the placeholder with the matching `.elf`. The first address is `pc`;
the rest come from `bt`. If `bt` ends with `!`, the firmware marked the
backtrace corrupt, so treat the frames below the first bad one as unreliable.
For RISC-V boards, use the matching `riscv32-esp-elf-addr2line` instead.

## systemd

Install the script under `/opt/observer-crash-collector`, the unit under
`/etc/systemd/system`, the environment file under
`/etc/observer-crash-collector/collector.env`, and create the service user:

```sh
sudo groupadd --system meshcore-observer
sudo useradd --system --gid meshcore-observer \
  --home-dir /var/lib/observer-crash-collector \
  --shell /usr/sbin/nologin meshcore-observer
sudo install -d -o meshcore-observer -g meshcore-observer \
  /var/lib/observer-crash-collector
sudo install -d /etc/observer-crash-collector
sudo install -m 0640 -o root -g meshcore-observer \
  collector.env.example /etc/observer-crash-collector/collector.env
sudo systemctl daemon-reload
sudo systemctl enable --now observer-crash-collector
```

`StateDirectory=` creates and owns the output directory. The unit sets
`ProtectSystem=strict`, `ProtectHome=true`, an empty capability set, and a
system-call filter. Keep the password file outside `/home` (for example under
`/etc/observer-crash-collector`) and readable only by the service group, or use
`OBSERVER_CRASH_PASSWORD` in a root-owned `0600` environment file.

Logs go to stderr (the journal) only on start, connect, disconnect, each kept
event, and a counters line at most once per hour. Counters are never written to
the output file.

## Tests

```sh
python3 -m unittest discover -s tools/observer_crash_collector/tests -v
```

The tests cover every event kind, discarded non-events, repeated five-minute
statuses, a reboot producing a second record, crash redelivery after a
reconnect and after a restart with state rebuilt from the file, malformed and
oversized payloads, a missing `stats` object, rotation plus rebuild, summary
grouping, and password non-disclosure. They do not need a broker or paho-mqtt.
