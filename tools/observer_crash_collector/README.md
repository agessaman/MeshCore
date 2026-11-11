# Observer crash collector

Read-only paho-mqtt 2.x subscriber for ESP32 observer status topics. The default
filter, `meshcore/+/+/status`, excludes packets, raw traffic, and neighbors.
No publishing, last will, or offline queue is configured; delivery is QoS 0 with
a clean session. Only diagnostic events go into the JSON Lines event file.

## Events and boot evidence

Each node can contribute one occurrence of each kind per observed boot:

- `crash`: a valid firmware crash object. Firmware repeats it throughout the
  boot, even after erasing the flash dump. An identical crash on a later boot
  counts again and keeps its diagnostics.
- `reset`: `panic`, `int_wdt`, `task_wdt`, `wdt`, or `brownout`.
- `selfheal`: a software (`sw`) reset with a recognized `selfheal` cause and
  positive `selfheal_n`. Inherited self-heal fields on other reset types are
  context only. Firmware can clear these fields during healthy operation.

Kinds may arrive in separate statuses; a late crash does not recount the reset.
Node identity comes exclusively from the topic's 64-hex public key. A supplied
`origin_id` must match it. Keys and hex fields are normalized to lowercase.

Boot evidence, strongest first:

1. When both observations contain `stats.boot_id` (1–32 hex characters), equal
   IDs mean the same boot and different IDs mean a new boot, regardless of uptime.
   Current firmware does not provide this optional field yet.
2. Otherwise compare uptime with the previous uptime plus elapsed collector
   receive time, allowing 30 seconds of slack. Falling below that prediction
   means a new boot. An expected 32-bit millisecond wrap (4,294,967.296 seconds)
   within the same slack is accepted without inventing a reboot. Byte-identical
   redelivery of the last payload never starts a boot or advances the baseline.
3. Device timestamps are context only. NTP steps and 1970 timestamps do not
   affect detection. Reset/self-heal tuple changes alone never prove a reboot.

### Limits

The collector requires a synchronized host clock, including across restarts;
the unit orders startup after `time-sync.target`. Without boot IDs, missing
uptime, reordered statuses, uptime regressions from other causes, and wrap-boundary reboots are
ambiguous. A reboot during downtime is detectable only if its uptime regresses
against the receive-time prediction; a higher-than-predicted uptime is not
proof. A new boot with byte-identical payload is indistinguishable from replay.
Cloned keys cannot be separated. Evicted node history can count replay again.
Whole boots missed while disconnected cannot be recovered from QoS 0 or from
retention of only the latest status.

A hard kill loses the last snapshot interval's non-event observations, but saved
receive time and uptime remain a consistent pair and newer available event records
restore reported kinds. A newer retained payload can falsely imply a reboot after
a long gap, so that decision is deferred, preserving the old anchor until a fresh
status arrives, unless boot IDs resolve it. This can delay counting or miss a
retained-only boot; unflagged stale traffic and byte-identical boots remain
ambiguous.

## Storage and bounds

Events include UTC `received_at`, `kinds`, topic `iata`, `node_id`, and available
bounded context: `node_name`, `model`, `firmware_version`, `device_timestamp`,
`reset_reason`, `selfheal`, `selfheal_n`, and `uptime_secs`. Integer diagnostics
are `heap_min`, `internal_heap`, `heap_largest`, `mqtt_slots_up`,
`mqtt_slots_total`, `mqtt_outage_secs`, `mqtt_connect_failures`,
`mqtt_slots_breaker`, and `battery_mv` (unsigned 32-bit values, excluding bools).

`crash` follows `src/helpers/CrashReportFormat.h`: a printable task of at most
15 characters, eight-digit hex `pc` and `va`, unsigned 32-bit `exc`, optional
eight-digit ELF `sha`, and up to eight space-separated hex backtrace words.
`bt` may end with `!`, marking corruption. Invalid crash fragments and unknown
reset/self-heal tokens are dropped. Nonfinite JSON numbers reject the message.
Other text is capped at 128 characters and nonprintable/non-ASCII characters
become `?`; output JSON escapes text. Logs are separately escaped and capped.

Each event append is flushed and fsynced before dedup acknowledgement. Rotation
occurs before an append would exceed the threshold; a single record can exceed
a very small configured threshold. Only the current file and `.1` are kept.
The parent directory is fsynced after rename and creation. Startup truncates
an unfinished trailing line before accepting new events. Missing files are
normal; unreadable history, append, fsync, state, or rotation failures produce
one bounded storage error and exit 1 for systemd to restart.

`<output>.state` is an atomically replaced JSON snapshot, version 1, containing
a global sequence and an LRU list of `[node_key, evidence]` pairs. Evidence is
last receive time, uptime, optional boot ID, last payload SHA-256, and reported
kinds. Changed accepted statuses advance memory without copying the node map.
Non-event statuses cause no disk writes between snapshots, which limits wear on
SD cards. A dirty snapshot is written before each event-file rotation, on
SIGTERM, SIGINT, and stdin EOF, and otherwise at most once per
`OBSERVER_CRASH_SNAPSHOT_INTERVAL` (default 86400 seconds, monotonic clock).
Each event carries `state_seq` and `boot_evidence`, so startup replays events
newer than the snapshot and a hard kill loses only non-event uptime baselines.
A node whose baseline is lost or stale is re-anchored by its next status; a
reboot without a reportable kind is not an event either way. Events and state are not a single
filesystem transaction; an event is acknowledged after its successful append,
independently of snapshots. Older collector events can seed weaker evidence until
new statuses arrive.

The LRU has at most 20,000 fixed-size node keys and bounded evidence values.
State and temporary snapshots are bounded independently of event rotation.
Stdin lines and MQTT packets are bounded before accumulating payload bodies.
The paho 2.x packet guard uses its private receive hook; tests exercise the
installed implementation. WebSocket receive-buffer growth is also capped.
The unit adds `MemoryMax=256M`. One collector per output path is supported.

## Configuration

All settings use environment variables:

| `OBSERVER_CRASH_` suffix | Default | Meaning |
| --- | --- | --- |
| `HOST` | required | Broker hostname/IP, without scheme |
| `PORT` | 1883 / 8883 with TLS | Broker port |
| `TLS` | false | Enable certificate/hostname-verified TLS |
| `WS` / `WS_PATH` | false / `/mqtt` | MQTT over WebSockets |
| `USERNAME` | unset | MQTT username |
| `PASSWORD` | unset | Password; nonempty value overrides file |
| `PASSWORD_FILE` | unset | UTF-8 file; trailing CR/LF removed |
| `CLIENT_ID` | random | MQTT client ID |
| `TOPIC` | `meshcore/+/+/status` | Subscription filter |
| `KEEPALIVE` | 60 | MQTT keepalive seconds |
| `CA_FILE` | system trust | TLS CA bundle |
| `TLS_INSECURE` | false | Explicitly disable verification |
| `OUTPUT` | `crash-events.jsonl` | Event path, also used for state |
| `MAX_BYTES` | 20971520 | Event-file rotation threshold |
| `MAX_PAYLOAD` | 65536 | Payload byte limit, capped at 65536 |
| `NODE_CACHE` | 20000 | Node capacity, capped at 20000 |
| `BACKOFF_MAX` | 60 | Reconnect delay cap in seconds |
| `SNAPSHOT_INTERVAL` | 86400 | Minimum seconds between periodic dirty snapshots |

Enable TLS when sending credentials over an untrusted network. Credentials do
not appear in logs, records, or command arguments. `collector.env.example`
provides a deployment template. Booleans accept true/false, yes/no, on/off, 1/0.

## Running and decoding

```sh
python3 -m venv .venv
.venv/bin/python -m pip install 'paho-mqtt>=2,<3'
OBSERVER_CRASH_HOST=mqtt.example.net .venv/bin/python collector.py
OBSERVER_CRASH_OUTPUT=/tmp/events.jsonl .venv/bin/python collector.py --stdin < samples.txt
OBSERVER_CRASH_OUTPUT=/tmp/events.jsonl .venv/bin/python collector.py --summary
```

Stdin lines are `topic<TAB>payload`; `--dry-run` is an alias and still writes
output. The tests include `tests/stdin-sample.txt`. Stdin prints final counters
to stderr; normal MQTT operation logs lifecycle changes, kept events, and
pending counters at most hourly.

Summary counts only categories named in `kinds`. It groups canonical crash
content and reporting firmware version, showing counts, distinct nodes, and
first/last receive times. Memory is capped at 200 crash groups, 200 firmware
versions per other category, and 1,024 distinct nodes per crash group; capped
node counts show `>=`, and omitted crash occurrences are reported.

The summary's shell-quoted addr2line hint contains only validated addresses.
Replace `firmware-matching-<sha>.elf` with the exact crashing ELF identified by
its SHA prefix. The reporting firmware version may describe a later image.
For RISC-V use `riscv32-esp-elf-addr2line`; for Xtensa choose the board's toolchain.
Backtrace return addresses, especially after a corruption marker, need care.

Readiness is logged only after SUBACK grants the subscription. Rejected
subscriptions, pre-CONNACK disconnects, and loop errors retry with capped,
jittered backoff. Setup runs in one daemon thread with an eight-second deadline;
a stalled attempt exits 1 instead of accumulating threads. SIGTERM/SIGINT
interrupt the main wait or network pump and normally exit 0, including while
DNS or WebSocket setup is blocked. This is tested with fake clients and a
subprocess, not a live systemd installation or a physical power cut.

## systemd installation

Run from this directory on Linux (install Python's venv package if needed):

```sh
sudo groupadd --system meshcore-observer
sudo useradd --system --gid meshcore-observer --home-dir /var/lib/observer-crash-collector \
  --shell /usr/sbin/nologin meshcore-observer
sudo install -d /opt/observer-crash-collector /etc/observer-crash-collector
sudo install -m 0755 collector.py /opt/observer-crash-collector/collector.py
sudo python3 -m venv /opt/observer-crash-collector/venv
sudo /opt/observer-crash-collector/venv/bin/python -m pip install 'paho-mqtt>=2,<3'
sudo install -m 0644 observer-crash-collector.service /etc/systemd/system/
sudo install -m 0640 -o root -g meshcore-observer \
  collector.env.example /etc/observer-crash-collector/collector.env
# Edit the environment file with your broker settings before starting.
sudo systemctl daemon-reload
sudo systemctl enable --now observer-crash-collector
```

The unit uses that venv's interpreter. `StateDirectory` owns the output directory;
`UMask=0077`, strict system protection, protected home, and empty capabilities
limit access. Keep any password file outside `/home`, readable by the service
group. Logs go to the journal. Linux/systemd hardening requires deployment checks.

## Tests

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools/observer_crash_collector/tests -v
```

Tests need no broker. Two receive-bound tests use installed paho 2.x and skip
explicitly if it is absent; the other tests use fake clients or stdin.
