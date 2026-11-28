# Repeater Polling

Repeater observers can poll nearby repeaters for battery voltage, temperature and status, and
upload the results over MQTT. The observer logs in to each target as a **guest** under a
separate polling key, asks for telemetry, and never floods: only direct neighbors are polled.

Polling is in testing. It ships only in the poll test channel (`observer-firmware-poll`) and
is not on the beta or production channels yet.

## Three ways to run it

Where readings go depends on whether a polling server (analyzer) is configured, and which kind.

| Mode | Set up with | Who decodes the reading | What leaves the node |
|------|-------------|-------------------------|----------------------|
| **Local** | `poll.url` blank, `poll.slot` set | You: the node publishes decoded readings to your broker | Readings on your MQTT slot. No HTTP, no claims, no key upload |
| **Beacon** (JSON) | `poll.url` = a Beacon analyzer | The node, which publishes decoded readings | Claims and readings over MQTT, HTTP policy/owners calls. The polling key never leaves the node |
| **CoreScope** (encrypted) | `poll.url` = a CoreScope instance, `poll.consent yes` | CoreScope, from the overheard encrypted reply | Claims over MQTT, and the polling **private key** registered with CoreScope (renewed every 6 h) |

The node doesn't pick Beacon or CoreScope itself: the analyzer's policy answer does
(`reading_format` set means Beacon/JSON, absent or a 404 means CoreScope/encrypted). Until the
first policy answer of a boot, nothing is registered or polled. Local mode makes no HTTP calls
at all.

## Local mode: poll your own repeaters, no analyzer

Use this to poll a few repeaters and read the data yourself on a broker you control.

```
set mqtt2.preset custom          # a slot pointed at your own broker
set mqtt2.server mqtt://broker.example.net:1883
set mqtt2.username <user>        # and mqtt2.password, if your broker needs them
set poll.slot 2                  # the slot readings are published on
poll add <64-hex key | neighbor prefix>
poll pw <key> hello              # or blank, or `set <password>`; default `auto` tries blank then hello
set poll on
```

- `set poll on` with a blank `poll.url` needs `poll.slot`; it creates the polling key on the
  spot. No consent is needed because the key never leaves the node.
- Readings are published on the slot's telemetry topic: `meshcore/<IATA>/<device>/telemetry`
  for the MeshCore topic layout, or your custom template with `{type}` = `telemetry`. MeshRank
  slots don't carry polling topics.
- Nothing else changes: same targets, intervals, safety rules and status commands as below.
- Clearing `poll.slot` turns polling off. Setting a `poll.url` switches to that analyzer (the
  node then waits for its policy before polling again).
- `get poll` shows `mode local`.

## Readings

One reading per message, published as JSON:

```json
{"v":2,"origin":"My Observer","origin_id":"AB12…","timestamp":"2026-11-22T18:04:11.000000+00:00",
 "plugin":"meshcore.observer_fw/v1.18.0.9",
 "readings":[{"node_id":"c8c8c858…","battery_mv":4226,"source":"poll","reply_hops":0,
   "method":"telemetry","sampled_at":"2026-11-22T18:04:09.000000+00:00","snr":9.25,"temperature_c":21.5,
   "lpp":"0174107e…","tag":1763834649,"packet_hash":"9F3C…"}]}
```

- `node_id` is the target repeater's public key; `origin`/`origin_id` are the observer's name
  and public key.
- `method` is `telemetry` (voltage, and MCU temperature on some boards; `lpp` holds the raw
  Cayenne LPP bytes) or `status` (every `poll.status` hours; `stats` carries the repeater's
  status fields instead of `lpp`).
- A repeater answering a guest sends only base telemetry. Other sensors appear only if the
  repeater's owner gives the polling key a higher role (`setperm <polling key> 1`).
- Local and Beacon (`reading_format:"telemetry"`) use this envelope, `v:2`. Beacon's
  `reading_format:"json"` publishes the older `v:1` envelope on `…/battery`.

## What is shared with whom

| Data | Local | Beacon | CoreScope |
|------|-------|--------|-----------|
| Node's own private key | never | never | never |
| Polling private key (a separate key made for polling only) | never | never | uploaded with `poll.consent yes`, signed by the node key |
| Node public key and name | your broker | analyzer | analyzer |
| Target public keys, SNR, poll state (claims) | not sent | analyzer, on change and every 30 min | analyzer, same |
| Decoded readings | your broker | analyzer's broker | not sent (CoreScope decodes the packet feed) |
| Guest passwords | stay on the node | stay on the node; with `poll.requests on`, the analyzer may send one for a target it asks you to poll, encrypted to your node key | stay on the node |

Notes:

- The polling key's private half is what decrypts a target's reply. In CoreScope mode it is
  uploaded so CoreScope can read the replies the observer network overhears; anyone holding it
  can read those replies, and could log in to targets as that guest. It never grants more than
  guest access. `poll.consent no` stops further uploads.
- If CoreScope takes no per-observer keys (404 on registration), the node uses a shared key
  CoreScope hands out instead, and uploads nothing.
- The encrypted replies themselves travel in the normal packet feed in every mode, like any
  other packet the observer hears.
- Requested targets (`poll.requests on`, Beacon over HTTPS only) are held in RAM only, with
  their passwords. A password an analyzer sends you is readable by your node: treat it as
  shared with volunteer observers, which is why only guest passwords are accepted.
- In JSON reading mode the claim also reports `accepts_requests`, `request_slots` (free
  seats), `max_slots` (`poll.limit`) and `poll_interval` (`poll.interval`, in minutes).

## Safety rules (all modes)

- Only direct neighbors are polled: the target must be in the neighbor table and heard in
  the last day. Packets are sent direct, behind forwarded traffic; nothing is flooded. A
  target out of reach shows `not_direct` and gets no packet.
- Passwords tried: blank and `hello`, or the one set with `poll pw <key> set`, which is then
  the only one tried.
- If a target answers the polling key as **admin**, polling of it stops, a password of your own
  is discarded, the node sends `setperm <polling key> 0` and checks the grant is gone. The
  target stays stopped until `poll enable <key>`, across reboots and even after removal.
- At most twice per hour-long window per target (the analyzer's policy can change the window
  and cap; local mode uses these defaults), one poll at a time, at least 60 s between
  scheduled polls, never during neighbor discovery.
- No polling until NTP has set the clock this boot.

## Commands

| Command | Purpose | Default |
|---------|---------|---------|
| `set poll on\|off` | Master switch. `on` needs `poll.url`, or `poll.slot` for local mode | off |
| `set poll.url <https://...>\|none` | Analyzer base URL; blank = local mode. Clearing it turns polling off | blank |
| `set poll.slot <1-6>\|0` | MQTT slot for claims and readings. With an analyzer, 0 = the slot whose broker the policy names. Local mode requires it | 0 |
| `set poll.consent yes\|no` | Allow uploading the polling private key (CoreScope mode only) | no |
| `set poll.interval <30-1440>` | Minutes between polls of one target | 60 |
| `set poll.status <1\|3\|6\|12\|24>` | Hours between status requests | 6 |
| `set poll.requests on\|off` | Accept analyzer-requested targets (Beacon over HTTPS) | off |
| `set poll.limit <1-8>` | Most targets polled. Your own targets count first and are never dropped; requested targets fill the rest | 8 |
| `set poll.community on\|off` | Share polling with other observers of the same analyzer | on |
| `poll add <key \| prefix>` | Add a target (up to 8). A prefix must match one heard neighbor | |
| `poll remove <key>`, `poll clear`, `poll list` | Edit or show targets | |
| `poll pw <key> auto\|blank\|hello\|set <password>` | Guest password to try (1-15 printable characters, never echoed) | auto |
| `poll now <key \| prefix>` | One poll now. Needs the polling key, a synced clock, a direct route, 60 s since the last | |
| `poll enable <key \| prefix>` | Clear an admin stop | |
| `get poll` | Switch, mode (`local`, `encrypted`, `json`, `telemetry`, `unknown`), slot, requests, targets, key, gate, clock | |
| `get poll.status [n]` | One line per target, or details of target n (battery, temperature, route, SNR, next poll) | |
| `get poll.key` | Polling public key | |

`get mqtt.status` ends with `, poll: <ok>/<targets>` while polling is on.

## Known issues

- `set poll on` doesn't check that `poll.slot` points at a configured, enabled MQTT slot. If it
  doesn't, every reading is refused by the bridge and retried indefinitely, and `get poll` still
  shows `slot <n> (set)`. Check the slot with `get mqtt.status` before turning polling on.
- Switching from local mode to an analyzer URL after the node has been polling for 21 minutes
  skips the startup hold, which normally waits for a claim and an owners answer before the
  first scheduled poll. The node still waits for the analyzer's first policy answer. Changing
  from one analyzer URL to another behaves the same way. Rebooting after the switch restores
  the hold.

## Builds

Polling is built into repeater observers with PSRAM (`WITH_REPEATER_POLL`). Boards without
PSRAM opt in with `REPEATER_POLL_WITHOUT_PSRAM`; `REPEATER_POLL_DISABLE` turns it off. Room
servers aren't supported. Debug builds (`MQTT_DEBUG`) log each step with a `POLL:` prefix.
