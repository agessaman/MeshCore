# CLI Commands

This document provides an overview of CLI commands that can be sent to MeshCore Repeaters, Room Servers and Sensors.

## Navigation

- [Operational](#operational)
- [Neighbors](#neighbors-repeater-only)
- [Statistics](#statistics)
- [Logging](#logging)
- [Information](#info)
- [Configuration](#configuration)
  - [Radio](#radio)
  - [System](#system)
  - [Routing](#routing)
  - [ACL](#acl)
  - [Region Management](#region-management-v110)
    - [Region Examples](#region-examples)
  - [GPS](#gps-when-gps-support-is-compiled-in)
  - [Sensors](#sensors-when-sensor-support-is-compiled-in)
  - [Bridge](#bridge-when-bridge-support-is-compiled-in)

---

## Operational

### Reboot the node
**Usage:** 
- `reboot`

**Note:** No reply is sent.

---

### Power-off the node
**Usage:**
- `poweroff`, or
- `shutdown`

**Note:** No reply is sent.

---

### Enter the UF2 bootloader (nRF52 only)
**Usage:**
- `uf2reset`

**Serial Only:** Yes

**Note:** Reboots directly into the UF2 bootloader on supported nRF52 boards.

---

### Reset the clock and reboot
**Usage:**
- `clkreboot`

**Note:** No reply is sent.

---

### Sync the clock with the remote device
**Usage:** 
- `clock sync`

---

### Display current time in UTC
**Usage:**
- `clock`

---

### Set the time to a specific timestamp
**Usage:** 
- `time <epoch_seconds>`

**Parameters:**
- `epoch_seconds`: Unix epoch time

---

### Send a flood advert
**Usage:** 
- `advert`

---

### Send a zero-hop advert
**Usage:**
- `advert.zerohop`

---

### Start or stop an Over-The-Air (OTA) firmware update
**Usage:**
- `start ota`
- `stop ota`

---

### Erase/Factory Reset
**Usage:**
- `erase`

**Serial Only:** Yes

**Warning:** _**This is destructive!**_

---

## Neighbors (Repeater Only)

### List nearby neighbors
**Usage:** 
- `neighbors`

**Note:** The output of this command is limited to the 8 most recent adverts.

**Note:** Each line is encoded as `{pubkey-prefix}:{timestamp}:{snr*4}`

---

### Remove a neighbor
**Usage:** 
- `neighbor.remove <pubkey_prefix>`

**Parameters:** 
- `pubkey_prefix`: The public key of the node to remove from the neighbors list. This can be a short prefix or the full key. All neighbors matching the provided prefix will be removed.

**Note:** You can remove all neighbors by sending a space character as the prefix. The space indicates an empty prefix, which matches all existing neighbors.

---

### Discover zero hop neighbors

**Usage:** 
- `discover.neighbors`

---

## Statistics

### Clear Stats
**Usage:** `clear stats`

---

### System Stats - Battery, Uptime, Queue Length and Debug Flags
**Usage:** 
- `stats-core`

**Serial Only:** Yes

---

### Radio Stats - Noise floor, Last RSSI/SNR, Airtime, Receive errors
**Usage:** `stats-radio`

**Serial Only:** Yes

---

### Packet stats - Packet counters: Received, Sent
**Usage:** `stats-packets`

**Serial Only:** Yes

---

## Logging

### Begin capture of rx log to node storage
**Usage:** `log start`

---

### End capture of rx log to node storage
**Usage:** `log stop`

---

### Erase captured log
**Usage:** `log erase`

---

### Print the captured log to the serial terminal
**Usage:** `log`

**Serial Only:** Yes

---

## Info

### Get the Version
**Usage:** `ver`

---

### Show the hardware name
**Usage:** `board`

---

## Configuration

### Radio

#### View or change this node's radio parameters
**Usage:**
- `get radio`
- `set radio <freq>,<bw>,<sf>,<cr>`

**Parameters:**
- `freq`: Frequency in MHz
- `bw`: Bandwidth in kHz. Most targets allow `7.8`, `10.4`, `15.6`, `20.8`, `31.25`, `41.7`, `62.5`, `125`, `250`, `500`. LR1110 targets allow `62.5`, `125`, `250`, `500`.
- `sf`: Spreading factor (5-12)
- `cr`: Coding rate (5-8)

**Set by build flag:** `LORA_FREQ`, `LORA_BW`, `LORA_SF`, `LORA_CR`

**Default:** `869.525,250,11,5`

**Note:** Requires reboot to apply

---

#### View or change this node's transmit power
**Usage:**
- `get tx`
- `set tx <dbm>`

**Parameters:**
- `dbm`: Power level in dBm (1-22)

**Set by build flag:** `LORA_TX_POWER`

**Default:** Varies by board

**Notes:** This setting only controls the power level of the LoRa chip. Some nodes have an additional power amplifier stage which increases the total output. Refer to the node's manual for the correct setting to use. **Setting a value too high may violate the laws in your country.**

---

#### Change the radio parameters for a set duration
**Usage:** 
- `tempradio <freq>,<bw>,<sf>,<cr>,<timeout_mins>`

**Parameters:**
- `freq`: Frequency in MHz (150-2500)
- `bw`: Bandwidth in kHz (same allowed values as `set radio`)
- `sf`: Spreading factor (5-12)
- `cr`: Coding rate (5-8)
- `timeout_mins`: Duration in minutes (must be > 0)

**Note:** This is not saved to preferences and will clear on reboot

---

#### Schedule radio parameter changes
**Usage:**
- `set radioat <freq>,<bw>,<sf>,<cr>,<start_time>`
- `get radioat [n|all]`
- `del radioat [n|all]`
- `set tempradioat <freq>,<bw>,<sf>,<cr>,<start_time>,<end_time>`
- `get tempradioat [n|all]`
- `del tempradioat [n|all]`

**Parameters:**
- `freq`: Frequency in MHz (150-2500)
- `bw`: Bandwidth in kHz (same allowed values as `set radio`)
- `sf`: Spreading factor (5-12)
- `cr`: Coding rate (5-8)
- `start_time`: Unix epoch time when the setting starts
- `end_time`: Unix epoch time when a temporary setting reverts
- `n`: Scheduled entry number from `get radioat` or `get tempradioat`

**Notes:**
- `get radioat` and `get tempradioat` list all entries when `n` is omitted.
- `del radioat` and `del tempradioat` delete all entries when `n` is omitted.
- Each queue supports 3 entries. Scheduled entries are not saved across reboot.
- `radioat` saves the new radio preferences when it fires. `tempradioat` applies temporarily, then reverts to the saved radio preferences.

---

#### View or change this node's frequency
**Usage:**
- `get freq`
- `set freq <frequency>`

**Parameters:**
- `frequency`: Frequency in MHz

**Default:** `869.525`

**Note:** Requires reboot to apply
**Serial Only:** `set freq <frequency>`

---

#### View or change this node's rx boosted gain mode (SX12xx and LR1110, v1.14.1+)
**Usage:**
- `get radio.rxgain`
- `set radio.rxgain <state>`

**Parameters:**
  - `state`: `on`|`off`

**Default:** `on`

**Temporary Note:** If you upgraded from an older version to 1.14.1 without erasing flash, this setting is `off` because of [#2118](https://github.com/meshcore-dev/MeshCore/issues/2118)

---

#### View or change the LoRa FEM receive-path gain state on supported boards
**Usage:**
- `get radio.fem.rxgain`
- `set radio.fem.rxgain <state>`

**Parameters:**
- `state`: `on`|`off`

**Notes:**
- This controls the external LoRa FEM receive-path LNA where the board supports it.
- This is separate from `radio.rxgain`, which controls the radio chip receive gain mode.

---

### System

#### View or change this node's name
**Usage:**
- `get name`
- `set name <name>`

**Parameters:**
- `name`: Node name

**Set by build flag:** `ADVERT_NAME`

**Default:** Varies by board

**Note:** Max length varies. If a location is set, the max length is 24 bytes; 32 otherwise. Emoji and unicode characters may take more than one byte.

---

#### View or change this node's latitude
**Usage:**
- `get lat`
- `set lat <degrees>`

**Set by build flag:** `ADVERT_LAT`

**Default:** `0`

**Parameters:**
- `degrees`: Latitude in degrees

---

#### View or change this node's longitude
**Usage:**
- `get lon`
- `set lon <degrees>`

**Set by build flag:** `ADVERT_LON`

**Default:** `0`

**Parameters:**
- `degrees`: Longitude in degrees

---

#### View or change this node's identity (Private Key)
**Usage:**
- `get prv.key`
- `set prv.key <private_key>`

**Parameters:**
- `private_key`: Private key in hex format (64 hex characters)

**Serial Only:**
- `get prv.key`: Yes
- `set prv.key`: No

**Note:** Requires reboot to take effect after setting

---

#### Change this node's admin password
**Usage:**
- `password <new_password>`

**Parameters:**
- `new_password`: New admin password

**Set by build flag:** `ADMIN_PASSWORD`

**Default:** `password`

**Note:** Command reply echoes the updated password for confirmation.

**Note:** Any node using this password will be added to the admin ACL list.

---

#### View or change this node's guest password
**Usage:**
- `get guest.password`
- `set guest.password <password>`

**Parameters:**
- `password`: Guest password

**Set by build flag:** `ROOM_PASSWORD` (Room Server only)

**Default:** `<blank>`

---

#### View or change this node's owner info
**Usage:**
- `get owner.info`
- `set owner.info <text>`

**Parameters:**
- `text`: Owner information text

**Default:** `<blank>`

**Note:** `|` characters are translated to newlines

**Note:** Requires firmware 1.12+

---

#### Fine-tune the battery reading
**Usage:**
- `get adc.multiplier`
- `set adc.multiplier <value>`

**Parameters:**
- `value`: ADC multiplier (0.0-10.0)

**Default:** `0.0` (value defined by board)

**Note:** Returns "Error: unsupported by this board" if hardware doesn't support it

---

#### Send a repeater flood text
**Usage:**
- `send text.flood <message>`

**Parameters:**
- `message`: Text to send to the shared `#repeaters` flood channel, prefixed with this node's name. Any `:` in the node name is sent as `;` so the prefix delimiter stays unambiguous.

**Example:**
```
send text.flood checking ridge link
```

---

#### View or change battery alert state
**Usage:**
- `get battery.alert`
- `set battery.alert <on|off>`

**Default:** `off`

**Note:** When enabled, the repeater checks battery level once per minute and sends low-battery warnings to the `#repeaters` flood channel.

---

#### View or change battery alert thresholds
**Usage:**
- `get battery.alert.low`
- `set battery.alert.low <1-100>`
- `get battery.alert.critical`
- `set battery.alert.critical <0-99>`

**Defaults:**
- `battery.alert.low`: `20`
- `battery.alert.critical`: `10`

**Note:** The low threshold must be greater than the critical threshold.

---

#### View this node's public key
**Usage:** `get public.key`

---

#### View this node's firmware version
**Usage:** `ver`

---

#### View this node's configured role
**Usage:** `get role`

---

#### View or change this node's power saving flag (Repeater Only)
**Usage:**
- `powersaving`
- `powersaving on`
- `powersaving off`

**Parameters:** 
- `on`: enable power saving
- `off`: disable power saving

**Default:** `off`

**Note:** When enabled, device enters sleep mode between radio transmissions. Enabling is refused from the local serial console or while an active USB serial data connection is detected; USB power alone does not block power saving.

---

### Routing

#### View or change this node's repeat flag
**Usage:**
- `get repeat`
- `set repeat <state>`

**Parameters:**
  - `state`: `on`|`off`

**Default:** `on`

---

#### View or change this node's advert path hash size
**Usage:**
- `get path.hash.mode`
- `set path.hash.mode <value>`

**Parameters:**
- `value`: Path hash size (0-2)
  - `0`: 1 Byte hash size (256 unique ids)[64 max flood]
  - `1`: 2 Byte hash size (65,536 unique ids)[32 max flood]
  - `2`: 3 Byte hash size (16,777,216 unique ids)[21 max flood]
  - `3`: DO NOT USE (Reserved) 

**Default:** `0`

**Note:** the 'path.hash.mode' sets the low-level ID/hash encoding size used when the repeater adverts. This setting has no impact on what packet ID/hash size this repeater forwards, all sizes should be forwarded on firmware >= 1.14. This feature was added in firmware 1.14

**Temporary Note:** adverts with ID/hash sizes of 2 or 3 bytes may have limited flood propagation in your network while this feature is new as v1.13.0 firmware and older will drop packets with multibyte path ID/hashes as only 1-byte hashes are supported. Consider your install base of firmware >=1.14 has reached a criticality for effective network flooding before implementing higher ID/hash sizes. 

---

#### View or change this node's loop detection
**Usage:**
- `get loop.detect`
- `set loop.detect <state>`

**Parameters:**
- `state`: 
  - `off`: no loop detection is performed
  - `minimal`: packets are dropped if repeater's ID/hash appears 4 or more times (1-byte), 2 or more (2-byte), 1 or more (3-byte)
  - `moderate`: packets are dropped if repeater's ID/hash appears 2 or more times (1-byte), 1 or more (2-byte), 1 or more (3-byte)
  - `strict`: packets are dropped if repeater's ID/hash appears 1 or more times (1-byte), 1 or more (2-byte), 1 or more (3-byte)
  
**Default:** `off`

**Note:** When it is enabled, repeaters will now reject flood packets which look like they are in a loop. This has been happening recently in some meshes when there is just a single 'bad' repeater firmware out there (probably some forked or custom firmware). If the payload is messed with, then forwarded, the same packet ends up causing a packet storm, repeated up to the max 64 hops. This feature was added in firmware 1.14

**Example:** If preference is `loop.detect minimal`, and a 1-byte path size packet is received, the repeater will see if its own ID/hash is already in the path. If it's already encoded 4 times, it will reject the packet.  If the packet uses 2-byte path size, and repeater's own ID/hash is already encoded 2 times, it rejects. If the packet uses 3-byte path size, and the repeater's own ID/hash is already encoded 1 time, it rejects. 

---

#### View or change the retransmit delay factor for flood traffic
**Usage:**
- `get txdelay`
- `set txdelay <value>`

**Parameters:**
- `value`: Transmit delay factor (0-2)

**Default:** `0.5`

**Note:** When multiple nearby repeaters all hear the same flood packet, each waits a random amount of time before retransmitting to avoid simultaneous collisions. This factor scales the size of that random window. Higher values reduce collision risk at the cost of added latency. `0` disables the window entirely.

---

#### View or change the retransmit delay factor for direct traffic
**Usage:**
- `get direct.txdelay`
- `set direct.txdelay <value>`

**Parameters:**
- `value`: Direct transmit delay factor (0-2)

**Default:** `0.2`

**Note:** Same collision-avoidance random window as `txdelay`, but applied to direct (non-flood, routed) traffic. The default is lower because direct packets are addressed to a specific next hop, so far fewer nodes compete to retransmit them.

---

#### [Experimental] View or change the processing delay for received traffic
**Usage:**
- `get rxdelay`
- `set rxdelay <value>`

**Parameters:**
- `value`: Receive delay base (0-20)

**Default:** `0.0`

**Note:** When enabled, repeaters that received a flood packet with a weak signal are held in a delay queue before processing, while those that received it with a strong signal process it immediately. This gives strong-signal paths forwarding priority. By the time weak-signal nodes process their copy, the packet may have already propagated and will be suppressed as a duplicate, reducing redundant retransmissions.

---

#### View or change the duty cycle limit
**Usage:**
- `get dutycycle`
- `set dutycycle <value>`

**Parameters:**
- `value`: Duty cycle percentage (1-100)

**Default:** `50%` (equivalent to airtime factor 1.0)

**Examples:**
- `set dutycycle 100` — no duty cycle limit
- `set dutycycle 50` — 50% duty cycle (default)
- `set dutycycle 10` — 10% duty cycle
- `set dutycycle 1` — 1% duty cycle (strictest EU requirement)

> **Note:** Added in firmware v1.15.0

---

#### View or change the airtime factor (duty cycle limit)
> **Deprecated** as of firmware v1.15.0. Use [`get/set dutycycle`](#view-or-change-the-duty-cycle-limit) instead.

**Usage:**
- `get af`
- `set af <value>`

**Parameters:**
- `value`: Airtime factor (0-9). After each transmission, the repeater enforces a silent period of approximately the on-air transmission time multiplied by the value. This results in a long-term duty cycle of roughly 1 divided by (1 plus the value). For example:
  - `af = 1` → ~50% duty
  - `af = 2` → ~33% duty
  - `af = 3` → ~25% duty
  - `af = 9` → ~10% duty
  You are responsible for choosing a value that is appropriate for your jurisdiction and channel plan (for example EU 868 Mhz 10% duty cycle regulation).

**Default:** `1.0`

---

#### View or change the local interference threshold
**Usage:**
- `get int.thresh`
- `set int.thresh <value>`

**Parameters:**
- `value`: Interference threshold value

**Default:** `0.0`

---

#### Enable or disable hardware Channel Activity Detection (CAD)
**Usage:**
- `get cad`
- `set cad <on|off>`

**Description:** When enabled, the radio performs a hardware Channel Activity Detection scan before transmitting and defers if the channel is busy. Runs independently of `int.thresh` — either, both, or none may be active.

**Parameters:**
- `on|off`: Enable or disable hardware CAD

**Default:** `off`

---

#### View or change the AGC Reset Interval
**Usage:**
- `get agc.reset.interval`
- `set agc.reset.interval <value>`

**Parameters:**
- `value`: Interval in seconds rounded down to a multiple of 4 (17 becomes 16). 0 to disable.

**Default:** `0.0`

---

#### Enable or disable Multi-Acks support
**Usage:**
- `get multi.acks`
- `set multi.acks <state>`

**Parameters:**
- `state`: `0` (disable) or `1` (enable)

**Default:** `0`

---

#### View or change the flood advert interval
**Usage:**
- `get flood.advert.interval`
- `set flood.advert.interval <hours>`

**Parameters:**
- `hours`: Interval in hours (3-168)

**Default:** `12` (Repeater) - `0` (Sensor)

---

#### View or change the zero-hop advert interval
**Usage:**
- `get advert.interval`
- `set advert.interval <minutes>`

**Parameters:**
- `minutes`: Interval in minutes rounded down to the nearest multiple of 2 (61 becomes 60) (60-240)

**Default:** `0`

---

#### Limit the number of hops for a flood message
**Usage:**
- `get flood.max`
- `set flood.max <value>`

**Parameters:**
- `value`: Maximum flood hop count (0-64)

**Default:** `64`

---

#### Limit the number of hops for an unscoped flood message
**Usage:**
- `get flood.max.unscoped`
- `set flood.max.unscoped <value>`

**Parameters:**
- `value`: Maximum flood hop count (0-64) for a packet without a scope (no region set)

**Default:** `0xFF` - indicates it hasn't been set, will track flood.max until it is.

**Note:** An alternative to `region denyf *`, setting `flood.max.unscoped` to a lower value such as `3` would allow for local unscoped messages to propagate, while preventing noisy neighbors from flooding a local region.

---

#### Limit the number of hops for an advert flood message
**Usage:**
- `get flood.max.advert`
- `set flood.max.advert <value>`

**Parameters:**
- `value`: Maximum flood hop count (0-64) for an advert packet

**Default:** `8`

---

#### Forward flood group data packets on repeaters
**Usage:**
- `get flood.channel.data`
- `set flood.channel.data <on|off>`

**Parameters:**
- `on`: Retransmit received flood `GRP_DATA` channel packets.
- `off`: Do not retransmit received flood `GRP_DATA` channel packets.

**Default:** `on`

**Forwarding behavior:** Repeater firmware only. The repeater still receives and
logs the packet when logging is enabled; this only blocks retransmission.
This is checked before `flood.channel.block` and applies to flood `GRP_DATA`
packets regardless of channel key, subject to `flood.channel.block.hops`.
Flood group text (`GRP_TXT`) is unaffected by this setting.

`get flood.channel.data` includes the active hop gate as `h=all` or `h>N`.

---

#### Block selected flood channel packets on repeaters
**Usage:**
- `get flood.channel.block`
- `get flood.channel.block.<n>`
- `get flood.channel.block <name|8_hex_prefix>`
- `get flood.channel.block.hops`
- `set flood.channel.block <key> <name> [h=<all|1-7|default>]`
- `set flood.channel.block.<n> <key> <name> [h=<all|1-7|default>]`
- `set flood.channel.block #channel [h=<all|1-7|default>]`
- `set flood.channel.block.<n> #channel [h=<all|1-7|default>]`
- `set flood.channel.block.hops <all|1-7>`
- `del flood.channel.block.<n>`
- `del flood.channel.block <name|8_hex_prefix>`

**Parameters:**
- `n`: Slot number from `1` to `15`.
- `key`: 128-bit or 256-bit channel key as hex.
- `#channel`: Public hashtag channel name; derives the 128-bit channel key from the hashtag and is stored as the row name.
- `name`: Local label for hex-key rows. Not needed for `#channel`; extra text after `#channel` is ignored unless it is a hop setting.
- `8_hex_prefix`: First 4 bytes of the derived channel hash, shown by single-entry `get`.
- `all`: Block matching flood channel packets at any received flood hop count.
- `1-7`: Maximum received flood path hash count to repeat. Matching packets over this hop count are blocked.
- `default`: Row inherits the global `flood.channel.block.hops` setting.

**Slot behavior:** Without `.n`, `set flood.channel.block` updates an existing
row with the same derived channel prefix or name, otherwise it uses the next
empty slot. If all 15 slots are full, the command fails. With `.n`, the command
writes that slot.

**Forwarding behavior:** Repeater firmware only. This only affects received
flood `GRP_TXT` and `GRP_DATA` channel packets. The repeater still receives and
logs the packet, but it does not retransmit it when a configured block entry can
validate/decode it. If `flood.channel.data` is `off`, all flood `GRP_DATA`
packets over the hop gate are blocked before this per-channel check runs.

**Hop gate:** `flood.channel.block.hops` defaults to `all`, which preserves the
original behavior. When set to `N` from `1` to `7`, both `flood.channel.data off`
and block rows that inherit the global setting only block packets whose received
flood path hash count is greater than `N`; packets at `N` hops or lower can
still repeat. For example, `set flood.channel.block.hops 1` repeats zero-hop and
one-hop matches but blocks two-hop and longer matches.

Each block row can override the global hop gate with `h=<all|1-7|default>`.
For example, `set flood.channel.block #wardriving h=3` blocks `#wardriving`
matches above three hops, while `set flood.channel.block #bot h=7` blocks
`#bot` matches above seven hops. Use `h=default` to make the row inherit the
global setting again.

`get flood.channel.block` includes the global default first, then adds per-row
overrides as `/h>N` or `/h=all`; inherited rows do not show a suffix. Single-row
`get` replies include that row's stored hop mode as `h=def`, `h=all`, or `h>N`.
List replies truncate displayed row names only when the full list would exceed
the remote-management response limit.

**Matching behavior:** Each block entry stores the first 4 bytes of the derived
channel hash for display and lookup. Current group packets carry only the first
channel-hash byte, so that byte is used as a cheap prefilter. Only entries whose
first hash byte matches the packet try MAC/decrypt with their stored key. If
multiple blocked channels share the same first byte, the repeater tries each
matching key until one validates; the packet is blocked only after a successful
MAC/decrypt.

**Examples:**
```
set flood.channel.block #test
set flood.channel.block.2 9cd8fcf22a47333b591d96a2b848b73f #test
set flood.channel.block.hops 3
set flood.channel.block #wardriving h=3
set flood.channel.block #bot h=7
get flood.channel.block
get flood.channel.block.hops
get flood.channel.block #test
del flood.channel.block.2
```

---

### ACL

#### Add, update or remove permissions for a companion
**Usage:** 
- `setperm <pubkey> <permissions>`

**Parameters:**
- `pubkey`: Companion public key
- `permissions`: 
  - `0`: Guest
  - `1`: Read-only
  - `2`: Read-write
  - `3`: Admin

**Note:** Removes the entry when `permissions` is omitted

---

#### View the current ACL
**Usage:** 
- `get acl`

**Serial Only:** Yes

---

#### View or change this room server's 'read-only' flag
**Usage:**
- `get allow.read.only`
- `set allow.read.only <state>`

**Parameters:**
- `state`: `on` (enable) or `off` (disable)

**Default:** `off`

---

### Region Management (v1.10.+)

#### Bulk-load region lists
**Usage:** 
- `region load`
- `region load <name> [flood_flag]`

**Parameters:**
- `name`: A name of a region. `*` represents the wildcard region

**Note:** `flood_flag`: Optional `F` to allow flooding

**Note:** Indentation creates parent-child relationships (max 8 levels)

**Note:** `region load` with an empty name will not work remotely (it's interactive)

---

#### Save any changes to regions made since reboot
**Usage:** 
- `region save`

---

#### Allow a region
**Usage:** 
- `region allowf <name>`

**Parameters:** 
- `name`: Region name (or `*` for wildcard)

**Note:** Setting on wildcard `*` allows packets without region transport codes

---

#### Block a region
**Usage:** 
- `region denyf <name>`

**Parameters:** 
- `name`: Region name (or `*` for wildcard)

**Note:** Setting on wildcard `*` drops packets without region transport codes

---

#### Show information for a region
**Usage:** 
- `region get <name>`

**Parameters:**
- `name`: Region name (or `*` for wildcard)

---

#### View or change the home region for this node
**Usage:** 
- `region home`
- `region home <name>`

**Parameters:**
- `name`: Region name

---

#### View or change the default scope region for this node
**Usage:** 
- `region default`
- `region default {name|<null>}`

**Parameters:**
- `name`: Region name,  or <null> to reset/clear

---

#### Create a new region
**Usage:** 
- `region put <name> [parent_name]`

**Parameters:**
- `name`: Region name
- `parent_name`: Parent region name (optional, defaults to wildcard)

---

#### Define region hierarchy (single line)
**Usage:**
- `region def <token> [<token> ...]`

**Parameters (tokens):** Space-separated. A logical **cursor** starts at the wildcard `*`.

- **`name`** — Create `name` as a child of the current cursor (equivalent to `region put name` with the cursor as parent). Cursor moves to `name`.
- **`name|jump`** *(or `name,jump`)* — Create `name` as a child of the current cursor, then move the cursor to `jump` (must already exist on the node, or have been created earlier in this command). `jump` is **not** the parent of `name`; use this form to pop back up and start another branch.

**Behavior:** Each created region defaults to flood-allowed (same as `region put`). The reply is the resulting region tree (same format as bare `region`); review it before running `region save` to persist. On error, the reply is `Err - ...` and any regions placed before the failure remain on the node, just like a partial chain of `region put`.

**Existing regions:** `region def` does not clear the existing tree — if a name already exists, its parent is updated to the current cursor; otherwise a new region is created. To start from scratch, `region remove` the unwanted regions first.

**Limits:** Repeater serial accepts one line up to **160 characters**. For larger trees, split across multiple `region def` commands; the cursor resets to `*` between commands, so lead the next command with `child|ancestor` to reposition. Each token splits at most once on `|` — `region def a|b|c|d` is not a flat-list shorthand; see the flat-list example below.

**Example — linear chain** (each token becomes a child of the previous):
```
region def a b c d e
region save
```

**Example — branched tree** (equivalent to `region put a`, `region put b a`, `region put c b`, `region put d c`, `region put e b`, `region put f e`):
```
region def a b c d|b e f
region save
```

**Example — error and partial state:**
```
region def a b c|nope d
```
The reply is `Err - unknown jump: nope`. `a`, `b`, and `c` were placed before the failure; `d` was not. Run `region` to inspect, then re-run with a corrected jump or repair with `region remove` / `region put`.

**Example — flat list** (each region a child of `*`). Use `|*` after each token to pop the cursor back to the root before the next token:
```
region def a|* b|* c|* d|* e|* f
region save
```

---

#### Remove a region
**Usage:** 
- `region remove <name>`

**Parameters:**
- `name`: Region name

**Note:** Must remove all child regions before the region can be removed 

---

#### View all regions
**Usage:** 
- `region list <filter>`

**Serial Only:** Yes

**Parameters:**
- `filter`: `allowed`|`denied`

**Note:** Requires firmware 1.12+

---

#### Dump all defined regions and flood permissions
**Usage:** 
- `region`

**Serial Only:** For firmware older than 1.12.0

---

### Region Examples

**Example 1: Using F Flag with Named Public Region**
```
region load
#Europe F
<blank line to end region load>
region save
```

**Explanation:**
- Creates a region named `#Europe` with flooding enabled
- Packets from this region will be flooded to other nodes

---

**Example 2: Using Wildcard with F Flag**
```
region load 
* F
<blank line to end region load>
region save
```

**Explanation:**
- Creates a wildcard region `*` with flooding enabled
- Enables flooding for all regions automatically
- Applies only to packets without transport codes

---

**Example 3: Using Wildcard Without F Flag**
```
region load 
*
<blank line to end region load>
region save
```
**Explanation:**
- Creates a wildcard region `*` without flooding
- This region exists but doesn't affect packet distribution
- Used as a default/empty region

---

**Example 4: Nested Public Region with F Flag**
```
region load 
#Europe F
  #UK
    #London
    #Manchester
  #France
    #Paris
    #Lyon
<blank line to end region load>
region save
```

**Explanation:**
- Creates `#Europe` region with flooding enabled
- Adds nested child regions (`#UK`, `#France`)
- All nested regions inherit the flooding flag from parent

---

**Example 5: Wildcard with Nested Public Regions**
```
region load 
* F
  #NorthAmerica
    #USA
      #NewYork
      #California
    #Canada
      #Ontario
      #Quebec
<blank line to end region load>
region save
```

**Explanation:**
- Creates wildcard region `*` with flooding enabled
- Adds nested `#NorthAmerica` hierarchy
- Enables flooding for all child regions automatically
- Useful for global networks with specific regional rules

---
### Direct Retry

Direct retry resends direct-routed packets when the downstream echo is not heard. It applies to direct messages, ACK packets, multipart packets carrying ACK payloads, and TRACE packets.

#### View or change direct retry state
**Usage:**
- `get direct.retry`
- `set direct.retry <state>`

**Parameters:**
- `state`: `on`|`off`

**Default:** `on`

**Notes:**
- New installs and older preference files without direct retry settings default to `on` with the `rooftop` preset.

**Examples:**
```
get direct.retry
set direct.retry on
set direct.retry off
```

---

#### View or change direct retry heard-table gate
**Usage:**
- `get direct.retry.heard`
- `set direct.retry.heard <state>`

**Parameters:**
- `state`: `on`|`off`

**Default:** `on`

**Note:** When enabled, the recent repeater table is the direct retry eligibility
gate. Prefixes missing from the table are assumed reachable; prefixes in the
table below the active SNR gate are blocked.

**Examples:**
```
get direct.retry.heard
set direct.retry.heard on
set direct.retry.heard off
```

---

#### View or apply a retry preset
**Usage:**
- `get retry.preset`
- `set retry.preset <preset>`

**Parameters:**
- `preset`: `infra`|`rooftop`|`mobile`

**Notes:**
- Applies shared direct retry and flood retry defaults.
- `infra`: fewer, slower retries for stable fixed infrastructure.
- `rooftop`: default long retry window for weak rooftop links.
- `mobile`: long retry count with shorter spacing for moving or changing links; flood retry count is `15`.
- Changing `direct.retry.count`, `direct.retry.base`, `direct.retry.step`, `direct.retry.margin`, `flood.retry.count`, or `flood.retry.path` makes the preset report as `custom`.

**Examples:**
```
get retry.preset
set retry.preset infra
set retry.preset rooftop
set retry.preset mobile
```

---

### Flood Retry

Flood retry resends flood-routed packets when the same packet is not heard from
another qualifying repeater.

#### View or change flood retry count
**Usage:**
- `get flood.retry.count`
- `set flood.retry.count <count>`

**Parameters:**
- `count`: Base retry attempts after the original send, from `0` to `15`. `0` disables flood retry.

**Note:** Actual attempts are capped at `15`. Hop 1 flood retries use `count * 2`; hop 2 flood retries use `count * 1.5`, rounded up.

**Defaults:**
- `infra`: `1`
- `rooftop`: `3`
- `mobile`: `15`

**Examples:**
```
get flood.retry.count
set flood.retry.count 0
set flood.retry.count 15
```

---

#### View or change flood retry path gate
**Usage:**
- `get flood.retry.path`
- `set flood.retry.path <count|off>`

**Parameters:**
- `count`: Maximum flood path hash count eligible for retry, from `0` to `63`.
- `off`: Disable the path-length gate.

**Defaults:**
- `infra`: `1`
- `rooftop`: `2`
- `mobile`: `1`

**Examples:**
```
get flood.retry.path
set flood.retry.path 1
set flood.retry.path off
```

---

#### View or change flood retry advert handling
**Usage:**
- `get flood.retry.advert`
- `set flood.retry.advert <on|off>`

**Parameters:**
- `on`: Retry node advert floods.
- `off`: Do not retry node advert floods.

**Default:** `off`

**Examples:**
```
get flood.retry.advert
set flood.retry.advert off
```

---

#### View or change flood retry target prefixes
**Usage:**
- `get flood.retry.prefixes`
- `set flood.retry.prefixes <prefixes|none|off>`

**Parameters:**
- `prefixes`: Comma-separated 3-byte path hash prefixes, up to 8 entries.
- `none` or `off`: Clear the list.

**Note:** When set, non-bridge flood retry only accepts same-packet echoes whose
last hop matches one of these prefixes. When unset, any non-ignored last hop can
cancel the retry.

**Examples:**
```
get flood.retry.prefixes
set flood.retry.prefixes A58296,860CCA,425E5C
set flood.retry.prefixes none
```

---

#### View or change flood retry ignored prefixes
**Usage:**
- `get flood.retry.ignore`
- `set flood.retry.ignore <prefixes|none|off>`

**Parameters:**
- `prefixes`: Comma-separated 3-byte path hash prefixes, up to 8 entries.
- `none` or `off`: Clear the list.

**Note:** Non-bridge flood retry does not cancel on same-packet echoes whose
last hop matches this list. Bridge mode also excludes these prefixes from bucket
and `other` hits.

**Examples:**
```
get flood.retry.ignore
set flood.retry.ignore 71CE82,C7618C
set flood.retry.ignore none
```

---

#### View or change flood retry bridge mode
**Usage:**
- `get flood.retry.bridge`
- `set flood.retry.bridge <on|off>`

**Note:** Bridge mode retries until each configured fresh bucket, plus the non-source `other` bucket, has been heard or the retry count is exhausted.

**Examples:**
```
get flood.retry.bridge
set flood.retry.bridge on
```

---

#### View or change flood retry bridge buckets
**Usage:**
- `get flood.retry.bucket.<n>`
- `set flood.retry.bucket <n> <prefixes|none|off>`

**Parameters:**
- `n`: Bucket number from `1` to `6`.
- `prefixes`: Comma-separated 3-byte path hash prefixes, up to 17 entries per bucket.
- `none` or `off`: Clear the bucket.

**Examples:**
```
get flood.retry.bucket.1
set flood.retry.bucket 1 71CE82,C7618C
set flood.retry.bucket 2 none
```

---

#### View or change direct retry count
**Usage:**
- `get direct.retry.count`
- `set direct.retry.count <count>`

**Parameters:**
- `count`: Maximum retry attempts after the original send, from `1` to `15`.

**Default:** `15` with the `rooftop` preset

**Examples:**
```
get direct.retry.count
set direct.retry.count 1
set direct.retry.count 4
set direct.retry.count 15
```

---

#### View or change direct retry base delay
**Usage:**
- `get direct.retry.base`
- `set direct.retry.base <ms>`

**Parameters:**
- `ms`: First retry wait in milliseconds, from `10` to `5000`.

**Default:** `175` with the `rooftop` preset

**Explanation:**
- The first retry waits `base` milliseconds after the failed echo window.
- For non-TRACE direct paths shorter than 6 remaining hops, the effective wait is scaled by `hops / 6`.
- Non-TRACE direct paths with 6 or more remaining hops use the configured value unchanged.
- TRACE retries shorter than 16 remaining hops use `hops / 16`; 16 or more remaining hops use the configured value unchanged.
- Larger values reduce channel pressure and give slow repeaters more time.
- Smaller values recover faster but create tighter retry bursts.

**Examples:**
```
get direct.retry.base
set direct.retry.base 175
set direct.retry.base 275
set direct.retry.base 500
```

---

#### View or change direct retry step delay
**Usage:**
- `get direct.retry.step`
- `set direct.retry.step <ms>`

**Parameters:**
- `ms`: Extra milliseconds added for each subsequent retry, from `0` to `5000`.

**Default:** `100` with the `rooftop` preset

**Explanation:**
- Retry delay is `base + attempt_index * step`.
- For non-TRACE direct paths shorter than 6 remaining hops, that computed delay is scaled by `hops / 6`.
- Non-TRACE direct paths with 6 or more remaining hops use the computed delay unchanged.
- TRACE retries shorter than 16 remaining hops use `hops / 16`; 16 or more remaining hops use the computed delay unchanged.
- With `base=175` and `step=100`, non-TRACE paths with 6 or more remaining hops wait about `175`, `275`, `375`, `475` ms, and so on.
- `step=0` keeps every retry at the same delay.
- Larger steps spread retries over time and are safer on busy channels.

**Examples:**
```
get direct.retry.step
set direct.retry.step 0
set direct.retry.step 50
set direct.retry.step 100
set direct.retry.step 250
```

---

#### View or change direct retry SNR margin
**Usage:**
- `get direct.retry.margin`
- `set direct.retry.margin <snr_db>`

**Parameters:**
- `snr_db`: Extra SNR margin above the SF receive floor, from `0` to `40`.

**Default:** `5.00` with the `rooftop` preset

**Notes:**
- Unknown repeaters are still retried.
- Known repeaters below the receive floor plus this margin are skipped.
- Failed attempts lower the recent repeater SNR estimate by `0.25 dB`.

**Examples:**
```
get direct.retry.margin
set direct.retry.margin 0
set direct.retry.margin 2.5
set direct.retry.margin 5
set direct.retry.margin 10
```

---

#### View or change adaptive direct retry coding rate
**Usage:**
- `get direct.retry.cr`
- `set direct.retry.cr off`
- `set direct.retry.cr <cr4_min>,<cr5_min>,<cr7_min>,<cr8_max>`

**Parameters:**
- `cr4_min`: Minimum SNR in dB to retry at CR4.
- `cr5_min`: Minimum SNR in dB to retry at CR5.
- `cr7_min`: Minimum SNR in dB to retry at CR7.
- `cr8_max`: Maximum SNR in dB that forces CR8.

**Default:** `10.00,7.50,2.50,2.50`

**Explanation:**
- Higher SNR uses faster coding rates.
- Lower SNR uses more robust coding rates.
- Repeater retry attempts escalate from the adaptive starting CR. CR4 starts as CR4, CR5, CR7, CR7, then CR8. CR5 starts as CR5, CR7, CR7, then CR8. CR7 gets two attempts, then CR8.
- Repeater adaptive CR selection intentionally skips CR6.
- Non-repeater retry packets start at the current radio CR and follow the same escalation pattern, clamped at CR8. With the normal CR5 radio setting this is CR5, CR7, CR7, then CR8.
- `off` disables per-packet retry CR overrides and uses the current radio CR.
- Direct path retry packets sent at CR4 or CR5 temporarily use a shorter 16-symbol preamble, then restore the radio's default preamble.
- Unknown repeaters start at `+3.00 dB` for adaptive CR selection.
- A failed unknown repeater is seeded at `+2.75 dB`.
- Each later failure lowers the SNR estimate by `0.25 dB`.

**Examples:**
```
get direct.retry.cr
set direct.retry.cr off
set direct.retry.cr 10.0,7.5,2.5,2.5
set direct.retry.cr 12.0,8.0,4.0,1.0
set direct.retry.cr 8.0,5.0,1.5,0
set direct.retry.cr 6.0,3.0,0,-2.0
set direct.retry.cr 20.0,12.0,6.0,2.0
set direct.retry.cr 4.0,2.0,0,-4.0
```

**Example profiles:**
- Conservative weak-link profile:
```
set direct.retry.cr 12.0,8.0,4.0,1.0
```
- Balanced rooftop profile:
```
set direct.retry.cr 10.0,7.5,2.5,2.5
```
- Faster strong-link profile:
```
set direct.retry.cr 6.0,3.0,0,-2.0
```
- Very cautious noisy-link profile:
```
set direct.retry.cr 20.0,12.0,6.0,2.0
```

---

#### View, seed, or clear the recent repeater table
**Usage:**
- `get recent.repeater`
- `get recent.repeater <page>`
- `get recent.repeaters <page>`
- `set recent.repeater <prefix> [snr_db]`
- `clear recent.repeater`

**Parameters:**
- `prefix`: Repeater path-hash prefix as hex.
- `snr_db`: Optional SNR in dB. If omitted or invalid, defaults to `3.0`.
- `page`: 1-based result page.

**Output order:**
- `get recent.repeater` lists 3-byte prefixes first, then 2-byte prefixes, then 1-byte prefixes.
- Within each prefix length, entries are sorted from highest SNR to lowest SNR.

**SNR details:**
- Recent repeater SNR is stored internally in quarter-dB units.
- Heard repeater samples update an existing table entry with a weighted blend: `75%` existing SNR and `25%` new heard SNR, rounded up.
- Direct retry success also feeds the heard echo SNR back into the same weighted table.
- Direct retry failure is not weighted: each final echo-timeout failure lowers that repeater's SNR by `0.25 dB`.
- Unknown repeaters start at `+3.00 dB` for adaptive CR selection.
- If an unknown repeater fails, it is seeded into the table at `+2.75 dB`.
- `set recent.repeater <prefix> [snr_db]` seeds a missing prefix or adds another weighted sample for an existing prefix.
- Successful `set recent.repeater` replies include the stored prefix and SNR, for example `OK - set A1B2C3 at 3.0 SNR`.

**Examples:**
```
get recent.repeater
get recent.repeater 2
set recent.repeater A1B2C3 8.5
set recent.repeater 71CE82 -3.25
set recent.repeater A1B2C3
clear recent.repeater
```

---
### GPS (When GPS support is compiled in)

#### View or change GPS state
**Usage:**
- `gps`
- `gps <state>`

**Parameters:**
- `state`: `on`|`off`

**Default:** `off`

**Note:** Output format:
- `off` when the GPS hardware is disabled
- `on, {active|deactivated}, {fix|no fix}, {sat count} sats` when the GPS hardware is enabled

---

#### Sync this node's clock with GPS time
**Usage:** 
- `gps sync`

---

#### Set this node's location based on the GPS coordinates
**Usage:** 
- `gps setloc`

---

#### View or change the GPS advert policy
**Usage:**
- `gps advert`
- `gps advert <policy>`

**Parameters:** 
- `policy`: `none`|`share`|`prefs` 
  - `none`: don't include location in adverts
  - `share`: share gps location (from SensorManager)
  - `prefs`: location stored in node's lat and lon settings

**Default:** `prefs`

---

### Sensors (When sensor support is compiled in)

#### View the list of sensors on this node
**Usage:** `sensor list [start]`

**Parameters:**
- `start`: Optional starting index (defaults to 0)

**Note:** Output format: `<var_name>=<value>\n`

---

#### View or change the value of a sensor
**Usage:** 
- `sensor get <key>`
- `sensor set <key> <value>`

**Parameters:**
- `key`: Sensor setting name
- `value`: The value to set the sensor to

---

### Bridge (When bridge support is compiled in)

#### View the compiled bridge type
**Usage:** `get bridge.type`

---

#### View or change the bridge enabled flag
**Usage:**
- `get bridge.enabled`
- `set bridge.enabled <state>`

**Parameters:**
- `state`: `on`|`off`

**Default:** `off`

---

#### Add a delay to packets routed through this bridge
**Usage:**
- `get bridge.delay`
- `set bridge.delay <ms>`

**Parameters:**
- `ms`: Delay in milliseconds (0-10000)

**Default:** `500`

---

#### View or change the source of packets bridged to the external interface
**Usage:**
- `get bridge.source`
- `set bridge.source <source>`

**Parameters:**
- `source`: 
  - `logRx`: bridges received packets
  - `logTx`: bridges transmitted packets

**Default:** `logTx`

---

#### View or change the speed of the bridge (RS-232 only)
**Usage:**
- `get bridge.baud`
- `set bridge.baud <rate>`

**Parameters:**
- `rate`: Baud rate (`9600`, `19200`, `38400`, `57600`, or `115200`)

**Default:** `115200`

---

#### View or change the channel used for bridging (ESPNow only)
**Usage:**
- `get bridge.channel`
- `set bridge.channel <channel>`

**Parameters:**
- `channel`: Channel number (1-14)

---

#### Set the ESP-Now secret
**Usage:** 
- `get bridge.secret`
- `set bridge.secret <secret>`

**Parameters:**
- `secret`: ESP-NOW bridge secret, up to 15 characters

**Default:** Varies by board

---

#### View the bootloader version (nRF52 only)
**Usage:** `get bootloader.ver`

---

#### View power management support
**Usage:** `get pwrmgt.support`

---

#### View the current power source
**Usage:** `get pwrmgt.source`

**Note:** Returns an error on boards without power management support.

---

#### View the boot reset and shutdown reasons
**Usage:** `get pwrmgt.bootreason`

**Note:** Returns an error on boards without power management support.

---

#### View the boot voltage
**Usage:** `get pwrmgt.bootmv`

**Note:** Returns an error on boards without power management support.

---
