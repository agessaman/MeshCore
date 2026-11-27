# Fault Alerts

An observer can post a one-line message on a LoRa group channel when its network link or
an MQTT broker stays down too long. Alerts go out over the mesh as a flood `GRP_TXT`
packet, not over MQTT, since MQTT is what has failed. Anyone subscribed to the channel in
a companion app sees them as normal channel messages, sent under the node's name.

## Setup

Point alerts at a private channel, test it, then turn them on.

**Hashtag channel** (easiest for an operator group):

```bash
set alert.hashtag ops-alerts   # key = sha256("#ops-alerts")[0..15]
alert test
set alert on
```

**Private key** (`openssl rand -hex 16`, or paste the companion app's "Share Channel" output):

```bash
set alert.psk <32_hex_chars>
alert test
set alert on
```

Subscribers add a channel with the same hashtag or key.

Alerts refuse to use the Public channel, `#test`, or `#bot`, because they would spam every
node in range and trigger auto-responders. The list is `BANNED_ALERT_CHANNELS[]` in
[src/helpers/AlertReporter.cpp](src/helpers/AlertReporter.cpp). It is checked both when you
set the channel and when an alert is sent.

## What triggers an alert

| Fault | Fires after | Setting |
|-------|-------------|---------|
| Network link (Ethernet or WiFi) down | 30 min | `alert.wifi` |
| MQTT slot down, after connecting at least once since boot | 240 min | `alert.mqtt` |

A recovery message goes out once when the link returns. A fault can't fire again until
`alert.interval` has passed (minimum 60 min).

```
MyObserver: WiFi down 47m (reason 201)
MyObserver: WiFi recovered after 1h3m
MyObserver: MQTT slot 1 (analyzer-us) down 4h12m
MyObserver: MQTT slot 1 (analyzer-us) recovered after 4h45m
```

**OTA updates** also post on the alert channel, so a remote operator gets feedback after
the command's reply window closes: `OTA update starting`, or `OTA aborted: <reason>` (the
bridge resumes either way). Success has no message; the node comes back on the new
version. OTA messages are not rate-limited.

## CLI

| Command | Purpose | Default |
|---------|---------|---------|
| `get/set alert on\|off` | Master switch for fault and OTA alerts | off |
| `get/set alert.psk <32 hex>` | Channel key. `get` is serial only. Clears `alert.hashtag` | unset |
| `get/set alert.hashtag <name>` | Derive the key from `#name` (`#` optional) | unset |
| `set alert.psk` / `set alert.hashtag` (no value) | Clear the channel | |
| `get/set alert.region <name>` | Send on this region's scope instead of the default scope; bare `set` clears | unset |
| `get/set alert.wifi <0-1440>` | Minutes before a link alert; `0` disables | 30 |
| `get/set alert.mqtt <0-10080>` | Minutes before a slot alert; `0` disables | 240 |
| `get/set alert.interval <60-10080>` | Minutes between repeats of one fault | 60 |
| `alert test [message]` | Send `[test] <message>` now, even with `alert off` | |

`alert test` warns when `alert` is still off: a successful test doesn't mean automatic
alerts will fire.

## Scope

Alerts flood on the node's default scope (`region default`). `alert.region` overrides it
for alerts only. It must name an existing region (`region put <name>`); an unknown name
falls back to the default scope. With neither set, alerts go out unscoped. Listeners need
the same scope to hear them.

## Notes

- Outage timers live in RAM. A reboot restarts them, so the fault must last the full
  threshold after boot.
- Link alerts need a working LoRa radio; there is no other delivery path.
