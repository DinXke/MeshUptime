# How MeshUptime works

This file describes the firmware **as it stands**. Why the choices fell the way
they did is in [decisions.md](decisions.md); the figures they rest on are in
[measurements.md](measurements.md); what does not work is in
[open-issues.md](open-issues.md).

Everything below was checked against the source of the last recorded version.
Where something is built but has never been seen on the device, it says so.

## What the device is

One Heltec LoRa32 V3 running MeshCore. The base is `examples/simple_sensor` from
upstream tag `companion-v1.17.0` (the same commit as `repeater-v1.17.0` and
`room-server-v1.17.0`), with a custom `SensorManager`, a WiFi task and a web
server added.

### Two variants, one board

There are **two build variants** of the same firmware, with the same modules on the
same board:

- **Sensor variant** (`env:meshuptime`, `main.cpp` / `SensorMesh`). The original
  node: one identity, advertises as **`ADV_TYPE_SENSOR`** (`SensorMesh.cpp:297`) and
  answers node discovery with `CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_SENSOR`. That
  is the honest type: this device offers sensors. Stays the rollback path.
- **Room-server variant** (`env:meshuptime_room`, build flag `ROOM_SERVER_VARIANT`,
  `main_room.cpp` / `RoomMesh`). **The current primary product.** One device now
  carries several identities at once — rooms, virtual sensor-nodes and a bot — each
  with its own keypair and its own advert type. Unless stated otherwise, the rest of
  this file describes the room variant; the sensor base (power, WiFi, channels,
  monitors) is in *both* and is described below as the shared core.

`SensorMesh.cpp` stays compiled into both envs because the `WebTask` is coupled to
it by type; only `main_room.cpp` and `RoomMesh.cpp` switch between the envs (see
[building.md](building.md)).

### The room-server's identities

The room variant is deliberately not a single node with one key but a collection of
virtual identities, each with its own advert type so the MeshCore app shows it in
the right box:

| identity | advert type | join-URI type | count | what the app shows |
|---|---|---|---|---|
| room | `ADV_TYPE_ROOM` | 3 | `MAX_ROOMS=4` | a room server |
| sensor-node | `ADV_TYPE_SENSOR` | 4 | `MAX_SENSOR_NODES=4` | a sensor with telemetry |
| bot | `ADV_TYPE_CHAT` | 1 | 1 | an ordinary chat contact |

Room 0 reuses the existing main identity (the key in `/identity/_main.id` is
preserved). Why separate identities: the app shows telemetry **only** for
sensor-type contacts and chat messages for chat-type contacts. A node that wants to
be room, sensor and notifier at once must therefore wear those hats as literally
separate contacts.

Packet forwarding is in the base and is off by default
(`SensorMesh::allowPacketForward` checks `disable_fwd`, plus `flood_max` on a
flood packet). So it is a flag, not a second firmware — see
[open-issues.md](open-issues.md) for what still has to happen before turning that
flag on is defensible.

## The channel map, and why it is frozen

CayenneLPP carries **channel numbers only, no names**. A node polling telemetry
therefore stores "channel 6 = google" on its own side. If that number ever
shifts, the stored mapping **silently** points at a different service: no error,
just wrong figures on a dashboard.

Hence: **a monitor is given a channel number when it is created and keeps it
forever. Numbers of deleted monitors are not reused while any free number
remains.**

| channel | content | LPP type |
|---|---|---|
| 1 | battery voltage | `LPP_VOLTAGE` (set by `SensorMesh` itself) |
| 2 | mains power | `LPP_SWITCH` 0/1 |
| 3 | battery power | `LPP_SWITCH` 0/1, always the inverse of 2 |
| 4 | WiFi online | `LPP_SWITCH` 0/1 |
| 5 through 12 | monitors | `LPP_SWITCH` 0/1 plus `LPP_GENERIC_SENSOR` (ms) |

Eight monitor slots (`MON_MAX_MONITORS`), exactly as many as there are channels.
Which numbers have ever been handed out is kept as the bitmask `ch_ever_used` in
the settings file, so a reboot never re-issues a number.

`LPP_GENERIC_SENSOR` is deliberate: 4 bytes, multiplier 1, exact integers, and it
does not claim a unit it has no business claiming. `analoginput` dies above
327 ms, `luminosity` claims lux, `frequency` claims hertz.

### The byte budget of one packet

`SensorMesh` builds its CayenneLPP as `MAX_PACKET_PAYLOAD - 4` = **180 bytes**.
The fixed fields cost 24, a monitor costs 9 in the worst case. Eight monitors is
72 bytes, leaving 84 for environment sensors added later. `querySensors()`
recomputes that on every round and truncates rather than trusting the library to
behave. The full budget is in [measurements.md](measurements.md).

## The two kinds of monitor

To telemetry they look **identical** — a dashboard has no business needing to
know how we arrived at a verdict.

**PING.** We ping ourselves, on our own interval (10–3600 s, default 60 s). One
ping at a time, everything in steps driven from `loop()`, never waiting: the LoRa
radio is serviced from the same loop and has the tightest timing requirements in
the device. Three failed rounds mark a service down, two good ones bring it back
up (up may be faster: that costs no alarm). A hostname is re-resolved at most
every ten minutes.

**PUSHED.** Something outside (an existing Uptime Kuma, say) reports the verdict
through `/hook`. We do not ping such a monitor; instead we let it **go stale** if
the report fails to arrive, after `max(3 × report period, 90 s)`. Stale means
**unknown**, not "down" — if the reporter itself is dead, we know nothing, and
that is not the same as "still up".

The kind is not stored in a separate field but read off the address: address `-`
means pushed. That is thrift with the file format, not a trick: `-` is neither a
valid hostname nor an IP address, so no real monitor can fall into that branch by
accident, and older files stay readable.

For a pushed service only **name, channel and report period** are stored. The
state is nowhere on disk, because it is volatile by definition: after a reboot we
know nothing about it until the first new report, and "unknown" is then the only
honest answer.

### Freezing instead of reporting "down"

If our own WiFi drops, we are measuring our own network and not the service. The
monitors then pause: the values shown are the last **measured** values, and
channel 4 at 0 tells the reader they are stale. After WiFi returns, do nothing
for 30 s first — DHCP, DNS and the route are not ready yet, and a ping failing on
that would declare a service down for no reason.

For the same reason the staleness clock of a pushed service does not run while we
ourselves are blind: without our WiFi, `/hook` cannot reach us, so the silence is
our fault and not the reporter's.

## Power: inferred, not measured

This board has **no VBUS pin, no USB detection and no charge status**. There is
only `getBattMilliVolts()`, an ADC reading of the battery. Mains power is
therefore **inferred** from terminal voltage, with thresholds calibrated on this
hardware (see [measurements.md](measurements.md)).

The state machine:

- two thresholds (Schmitt): by default below **4,09 V** battery, above
  **4,12 V** mains, so a voltage in between yields no opinion instead of flapping
- **three** consecutive readings on the same side for a transition, 10 s apart,
  so roughly 30 s of reaction time
- **60 s settling time** after a transition, because a transient of about 50 mV
  was measured in the first minute after unplugging
- both thresholds are **settings**, not constants, and they are persisted

Mains and battery are two separate channels, as asked for, but it is one
measurement with two names: channel 3 is by definition the mirror of channel 2.

The blind spot is worth stating plainly: on a full battery with no charging,
"power gone" looks like "power present". That is why the WiFi sensor belongs with
this. If the router loses power, WiFi disappears within seconds; WiFi gone **and**
a falling battery voltage is a power cut with high confidence and at the speed of
whichever of the two comes first.

## The WiFi watchdog

Not a precaution but a repair of measured behaviour: on 19 August 2026 the node
dropped off the network while on battery with `reason 202`
(`WIFI_REASON_AUTH_FAIL`) at ample signal strength, and it did **not** recover —
even with USB plugged back in it stayed unreachable until a hard reset.

`WiFi.reconnect()` is therefore not enough. The task has five states (off,
connecting, online, retrying, own AP) and runs on:

- 20 s per connection attempt, 15 s before the next
- after 3 minutes without a connection the radio goes fully down and up again
- after 3 consecutive failures a hard reset of the WiFi stack, after 6 an own
  access point (`MeshUptime-…`) so the device stays configurable

The reconnect count, the hard-reset count and the last disconnect reason are shown
in the web interface. "No WiFi" without a reason costs somebody an evening.

## Time: NTP, timezone and local display

Since v2.2.2 the **NTP server** and the **timezone** are settable via the web GUI
(the *Time* panel, stored in `/time.cfg`, `POST /time`). The default NTP server is
`pool.ntp.org` (a LAN time server is allowed too) and the default timezone is
Europe/Brussels as the POSIX TZ string `CET-1CEST,M3.5.0/2,M10.5.0/3` — so DST-aware.
Saving applies the TZ at once and requests a re-sync. The sync is robust: on
WiFi-connect *and* periodically, every 6 hours.

**Human times local, protocol and RTC in UTC.** All human-facing times are shown in
**local** time with the zone abbreviation (e.g. `Received at: 00:35:11 CEST`), via
`localtime_r`/`strftime %Z` on the configured TZ. The RTC and **all** MeshCore
protocol timestamps stay UTC — the mesh reckons in UTC. Before the first successful
sync the node shows `niet gesynct` ("not synced") instead of a garbage time
(`TimeFmt.h`, `TIME_FLOOR`). `/cfg.json` exposes this via `ntp`, `tz`, `tlocal` (the
local date/time), `tsync`, `tsyncmsg` and `tsyncage`.

## The room-server

Everything above — channels, monitors, power, WiFi — is the shared sensor core.
What follows is what the room variant (`RoomMesh`) adds on top.

### Rooms

One device serves up to **four** room identities (`MAX_ROOMS=4`), each a full
MeshCore Room: its own keypair, its own name, an **admin** *and* a **guest**
password, and its own access list (`ClientACL`). Room 0 reuses the main identity;
the others get a fresh keypair when created.

The posts share one pool (`MAX_TOTAL_POSTS=48`) with a per-room quota, so one busy
room cannot starve the others. In the MeshCore app each room appears as a separate
room server you log into with a password; the guest role may read along but not
administer.

### Virtual sensor-nodes

Alongside the rooms the node carries up to **four** sensor-node identities
(`MAX_SENSOR_NODES=4`), each of the sensor type (`ADV_TYPE_SENSOR`) with its own
keypair. The reason is purely the app: it shows telemetry only for sensor-type
contacts, not for rooms. Whoever wants to see their measurements in the app couples
them to a sensor-node.

Each sensor carries only one CayenneLPP packet, and that packet is bounded (see *The
byte budget* above). With multiple sensor-nodes you can **spread** the monitors:
node A carries channels 1–6, node B carries 7–12 — subset-telemetry per node, so you
get past the one-packet limit. Every sensor(-monitor) therefore has two masks:

- **rooms mask** — which rooms the sensor sends its up/down alerts to;
- **sensornodes mask** — which sensor-node(s) carry the telemetry value out.

Both masks may be empty (coupled nowhere) or set several bits. A sensor can thus
alarm in two rooms and feed telemetry through two sensor-nodes at the same time.

### Per-key ACL: passwordless access

Beside the password login there is a **per-key ACL** (`MAX_ACL_GRANTS=16`, stored in
`/acl_grants`): a list of grants that each bind a **pubkey** to one of three levels —
`read`, `readwrite` or `admin`.

Access is granted **without a password**, and that is not a hole: the reply is
encrypted with the ECDH secret derived from that pubkey. Only the holder of the
matching private key can read it, so possession of the key *is* the authentication.
These grants are **persistent** and separate from the volatile login sessions: a
reboot clears the sessions but not the grants.

### Channel-management panel

Coupling is done node-centrically, in one panel: per sensor you set the rooms and
sensornodes masks (couple/uncouple). From the same panel comes the **manual
advert**: send an advert for a room, a sensor-node or the bot, as **flood** (the
whole mesh) or **zero-hop** (direct neighbours only). Handy to make a fresh identity
known without waiting for the next automatic advert.

### The room/DM command set

In a room, or in a DM to a room, the node answers an extended command set, **gated
by the sender's permission level**:

| level | commands |
|---|---|
| read | `dns`, `ping <host> [n]`, `port <host> <port>`, `http <url>`, `scan`, `traceroute <host>`, `neighbors`/`nb`, `wifi`, `sys`/`health`, `history <s>` |
| readwrite | `checknow`, `mute`/`unmute`, `snooze`, `test` |
| admin | `sendto <pubkey> <msg>`, `reboot`, `ntp`/`sync` |

`ping` reports not just reachability but **loss%** and **jitter**. Deferred results
(ping and the network diagnostics) come back **to the origin** — the same room or DM
the command came from — via the general origin-routing mechanism (the v2.1.0
"ping fix", now for all slow tasks).

Alongside these there are the **subsystem commands** that manage the identities and
channels, over serial and (for an admin) over a DM too: `room …`, `sensornode …`,
`bot list|add|del|post|sendto|advert|uri` and `channel list|add|del|on|off`.

### The async network-task engine

The diagnostics `port`, `scan`, `http` and `traceroute` would block if run naively,
and blocking is forbidden here: the LoRa radio is serviced from the same `loop()`
and has the tightest timing requirements. Hence a **cooperative, non-blocking task
engine**: one task at a time, in small steps from `loop()`, with the result routed
back to the origin.

- **`port <host> <port>`** — a non-blocking TCP connect: if the connection
  succeeds, the port is open.
- **`scan`** — an asynchronous WiFi scan of the surroundings.
- **`http <url>`** — fetches the **status code** and the **response time**.
- **`traceroute <host>`** — measures the **hop distance** honestly: with `esp_ping`
  the TTL is raised until the destination replies; that TTL is then the hop count.
  Bounded at **30 hops**, ~1.2 s per TTL. **The intermediate hop IPs are not
  available on this lwIP/esp_ping platform** — you get the distance, not the route.
  That is stated honestly on purpose; it is a platform limit, not a bug.

### SNMP as a monitor kind

Beside *ping* and *pushed* there is a third monitor kind: **SNMP**. The node itself,
on the poll interval, does a **non-blocking SNMP-GET** (BER/ASN.1 encoding, v2c over
UDP:161). It is not a new send path — the polled value flows through **exactly the
same** telemetry and alert pipeline as any other monitor: CayenneLPP (generic
channel) → sensor-nodes → mesh → MeshManager for the value; up/down → rooms/DM for
the alerts. Only the **source** is new.

An SNMP monitor carries: name, target IP (port :161 fixed), the **v2c community**,
an OID, an **interpretation** (`numeric` / `rate` / `status`) with an `snmparg` (the
compare value for status or the reference for rate), an interval, and the usual
am/rm/sn masks (alarm, rooms and sensornodes masks).

The community string is a secret and is treated as one: stored **obfuscated** (XOR +
hex in `/monitors.cfg`) and **never** logged, exported or shown in a UI. Management
is via the web GUI (SNMP form + `POST /monitor/snmp`), the CLI, and room/DM
(`mon.<channel>.type` / `.community` / `.oid` / `.interp` / `.snmparg`).

### The bot: a virtual chat/notifier contact

The per-sensor `dm` and `both` alerts used to come from the room identity, and that
fitted badly: in the app you saw a Room send you a DM. Hence a separate **bot
identity**, `BE-HSS-DinX-Bot`: its own persistent keypair (`/bot_id`), advertises as
**`ADV_TYPE_CHAT`**, and so appears as an ordinary chat contact.

The bot sends **clean DMs** — for flash notifications *and* for the per-sensor
`dm`/`both` alerts, rewired from the room to the bot, with **repeat-until-ACK** per
recipient. Its recipient list is persistent (`/bot_recips`, 16 full pubkeys) and is
seeded with the owner on a fresh node. The bot computes its shared secret **itself**
from the recipient pubkey (`calcSharedSecret` + `createDatagram`), so it does not
hang off a room session.

**Two-way since v2.2.1.** The bot used to be send-only: an incoming DM to the bot
fell through to the identity of `rooms[0]` and was decrypted with the **wrong key**
(and so silently dropped) — that was the bug. `onRecvPacket` now also matches the bot
identity `_bot_id`. A chat DM carries only a 1-byte src-hash and **no** pubkey, so the
sender pubkey is resolved from the neighbour list (heard adverts) and the recipient
list, and the shared secret is computed from it. That lets the bot decrypt and reply
without a password login.

The bot has a small command set of its own over DM (**not** the monitoring console):

| command | reply |
|---|---|
| `ping` | `Pong (HH:MM:SS <zone>)` — the local time with zone abbreviation |
| `test` | signal report: SNR / RSSI / hops of the incoming packet |
| `path` | the sender + the intermediate repeaters by **name** + SNR/RSSI + the received-at time (`Received at:` local) |
| `help` | lists the bot commands; unknown text → a short hint |

The reply is a clean DM from the bot, with an ACK on the incoming message. The fields
come from the incoming packet (path/hops, SNR×4→dB, RSSI, time); the reply buffers are
`static`, not on the loopTask stack.

Control:

- **CLI**: `bot list | add <pubkey> | del <prefix> | post <msg> |
  sendto <pubkey> <msg> | advert [flood] | uri`.
- **Room/DM**: the admin command `sendto <pubkey> <msg>`.
- **Web GUI**: the bot tab (join/QR, recipient management, `sendto`/`post`).

### Hashtag and public channels

Since v2.3.0 the bot also reads the enabled MeshCore **group channels** and answers
IN the channel to `ping` (Pong), `test` (signal report) and `path` (route with
repeater names) — the same style as the DM responder, but prefixed with the sender's
name. The reply is a flooded group message `"<botname>: <reply>"`
(`createGroupDatagram`); reading along runs via the `searchChannelsByHash` /
`onGroupDataRecv` overrides.

A channel in MeshCore is nothing more than a **shared secret** (16 bytes = 128-bit or
32 bytes = 256-bit); the **channel hash** is the first byte of `sha256(secret)`. The
node follows exactly that format. Management via the web GUI (bot tab:
add/toggle/delete), persistent in `/channels.cfg`; endpoints `GET /channels.json` and
`POST /channel/add|del|toggle`; CLI
`channel list|add <name> [secrethex]|del <name>|on <name>|off <name>`.

There are **three kinds of key**, as the GUI/CLI/`/channel/add` response also report:

- **own key** — an explicit 32- or 64-hex secret. Always wins, is never shown back.
- **key derived from the name** (v2.3.1) — add a channel with a name but **no**
  secret and the node makes a **hashtag channel**: the key is the first 16 bytes of
  `sha256(name)`, EXACTLY like the MeshCore app (`#test` →
  `9cd8fcf22a47333b591d96a2b848b73f`). Same name = same channel as the app. A derived
  key is not secret: whoever knows the name derives it.
- **public channel (fixed key)** (v2.3.2) — the name `Public` (case-insensitive, with
  or without `#`) with no secret uses the fixed, well-known public key
  `8b3387e9c5cdea6ac9e5edbaa115cd72` instead of `sha256("public")`, so the node lands
  on the REAL public channel, like the app. The `pub` flag in `/channels.json`
  (`is_public`) is derived by comparing the secret with the public key, not stored
  separately.

### Name resolution and the big contact list

To show real node **names** in `path`, in a DM and in `/contacts.json`, the neighbour
list (`NeighbourList`) grew in v2.3.0 from 12 to **200** entries: pubkey, name,
advert type, snr/hops and last-heard, LRU by last-heard. That costs ~13.6 kB RAM —
the deliberate trade — and the list stays **RAM-only**: adverts arrive too often for
a per-advert flash write, and after a reboot the names stream back in on their own
within minutes.

The **resolution order** follows from what a message carries:

1. **inline name from the message** — a group message carries the sender name IN the
   text (`"<name>: <text>"`, like the app) but no pubkey; we use that name directly.
2. **the big advert/contact list**, keyed by pubkey — for `path` and DMs, which *do*
   carry a pubkey but no name.
3. **hex pubkey prefix** as a last fallback, if the name is not known anywhere.

In short: a channel message has the name (no pubkey), a DM has the pubkey (no name) —
hence the two different paths to the same answer.

### Discovered contacts and the picker

`GET /contacts.json` returns the **neighbour list with full pubkey** plus name,
snr/hops, last-heard and count. The display is capped at `NB_JSON_MAX=64` entries;
the list itself stays 200 for the resolution above. The web GUI uses that list as a
**picker** in two places: the bot recipients and the per-slot ACL grants. So you
choose from heard nodes instead of pasting a 64-hex key by hand. Only **public keys**
travel this way — never a secret.

### Join URIs and QR

Each identity can be shared with a MeshCore join URI, drawn client-side as a QR in
the web GUI (no external assets):

    meshcore://contact/add?name=<name>&public_key=<64hex>&type=N

with `type` **1** = chat (the bot), **2** = repeater, **3** = room server (rooms),
**4** = sensor (sensor-nodes). Sharing a channel uses
`meshcore://channel/add?name=<name>&secret=<32hex>`; for a hashtag channel the key is
derived from the name, and for `Public` it is the fixed public key (see *Hashtag and
public channels*).

## The web interface

One page from PROGMEM, one web server, no framework, no CDN, no external
stylesheet, no external font: a node without internet must be able to serve its
own admin page. Dark by default, light through `prefers-color-scheme`, system font
stacks with mono for the figures. The sensor variant has three tabs — monitoring,
access, node; the room variant adds the **rooms**, **sensor-nodes** and **bot**
tabs. On the sensor variant those tabs stay hidden and the corresponding endpoints
return `501`. All sections are collapsible.

Colour is semantic, not decorative, which is what the `sev` field is for: "on" on
channel 2 (mains) is good, "on" on channel 3 (battery) is a warning. A page that
guesses the colour from the word paints those two the same, and then the colour
lies.

Everything sits behind a login with a **session cookie**: `POST /login` sets the
cookie, `POST /logout` clears it, and the web credentials themselves are settable via
`/web/cred` (and resettable via `/web/cred/reset`). The sensitive endpoints too —
including the room backup/restore that carry keys — sit behind the same auth.

### The routes

`GET`:

| route | what it does |
|---|---|
| `/` | the page |
| `/login` | the login form |
| `/status.json` | everything: power, WiFi, monitors, heap, uptime |
| `/cfg.json` | the whole of `NodePrefs` in one request (incl. `ntp`/`tz`/`tlocal`/`tsync`/`tsyncmsg`/`tsyncage`) |
| `/acl.json` | access list, lock state, discovered neighbours |
| `/rooms.json` | the rooms + sensor-nodes with their couplings |
| `/contacts.json` | neighbour list with full pubkey (picker for bot/ACL) |
| `/bot.json` | the bot: identity, join/QR, recipient list |
| `/channels.json` | the channels: name, channel hash, on/off, `pub` flag (secret never) |
| `/rooms/backup` | full room config incl. keys (behind auth) |
| `/hook` | supply a verdict from outside (may also be GET, see below) |

`POST`:

| route(s) | what it does |
|---|---|
| `/login`, `/logout` | open/close a session |
| `/wifi` | network settings |
| `/time` | set NTP server + timezone (`/time.cfg`) |
| `/hook` | supply a verdict from outside |
| `/monitor`, `/monitor/del` | create, delete a ping/pushed monitor |
| `/monitor/snmp` | create an SNMP monitor |
| `/mon/alarm` | set a monitor's alert route/masks |
| `/acl`, `/acl/del`, `/acl/strict` | manage the per-key ACL and the lock |
| `/cli` | the write path for node administration (console) |
| `/web/cred`, `/web/cred/reset` | set/reset the web credentials |
| `/sim`, `/sim/clear` | simulate / clear a state (testing) |
| `/alert/test` | fire a test alert |
| `/room/add`, `/room/edit`, `/room/del` | manage rooms |
| `/rooms/restore` | restore room config (behind auth) |
| `/snode/add`, `/snode/edit`, `/snode/del` | manage sensor-nodes |
| `/room/acl`, `/snode/acl` | set the ACL of a room / sensor-node |
| `/room/advert`, `/snode/advert` | manual advert per room / sensor-node |
| `/bot/recipient` | add/remove a bot recipient |
| `/bot/advert`, `/bot/sendto`, `/bot/post` | bot advert / DM / post |
| `/channel/add`, `/channel/del`, `/channel/toggle` | add / delete / toggle a channel |

On the sensor variant the room, snode, bot and channel endpoints do not exist as a
function: they return **`501`** and the corresponding GUI tabs stay hidden.

**Nothing that changes anything goes over GET**, and that is not a formality. A
GET that deletes a monitor is followed by every browser, every prefetch and every
link checker — and deletion here is not reversible, because the channel does not
come back. A `GET /cli?cmd=erase` would be a link that wipes the entire storage as
soon as a browser preloads it.

`/hook` is the exception and may also be a GET, because Uptime Kuma leaves the
method free. Arguments: `name`, `up` (0 or 1), `ms`, and optionally `every`
(10–3600 s, the report period). The answer is `200` **with the channel number in
it** (`ok <name> -> kanaal N`), because that number is the only handle by which the
caller finds its service again.

### Node administration: a console, not thirty forms

The MeshCore app administers a node over the same CLI. Exposing it gives
completeness in one step, instead of thirty forms that can each go stale on their
own. The forms above it are convenience for what you do often; the console is the
guarantee that nothing is missing.

**Radio behind three locks.** `freq`, `bw`, `sf` and `cr` decide whether this node
is still on the same mesh. Hence a checkbox that releases the button, a
`confirm()` that puts the old values next to the new, and — the only one that
really counts — **a server-side rule that refuses `set radio` and `set freq` with
409 unless `confirm=radio` is present**. The browser is not the lock; a bare fetch
does not get past. A radio command typed into the console gets the same
confirmation, so typing is not a bypass. `tx` is separate, being harmless by
comparison.

Refused with the reason given: anything involving `prv.key` (private key over HTTP
without TLS), `start ota` (opens its own AP *and* a second web server on port 80 —
this page disappears underneath it) and `poweroff` (deep sleep; only a physical
reset helps).

**Editing monitors.** Name, address and interval are editable per row; the channel
never changes. Only changed fields are sent, each as
`sensor set mon.<channel>.<field>`, so through the same sieve as the CLI and
without a second write path. An **address change** gets its own confirmation: the
same channel then means a different service, the name should change with it, and a
dashboard on the other side has to be updated.

Editing had to exist because channels are scarce: without it every typo costs a
number, since an issued number is not reused.

## The CLI

Over the serial console and — for an admin — over a DM as `TXT_TYPE_CLI_DATA`.
Everything upstream `CommonCLI` can do, plus the sensor settings. The form is
`sensor set <name> <value>`, with the `sensor` prefix: the generic get/set of
`CommonCLI` sits behind it, not beside it.

| setting | meaning |
|---|---|
| `mains.hi` | upper threshold; above this, mains |
| `mains.lo` | lower threshold; below this, battery |
| `mains.state` | read-only: current verdict |
| `mon.count` | read-only: number of monitors |
| `mon.add` | button: `name,address[,interval]` |
| `mon.del` | button: `name` |
| `mon.<channel>.name` / `.host` / `.int` / `.state` | per monitor |

**The number in `mon.5.host` is the channel, not an index.** An index shifts on
deletion and then silently points at another service.

Settings are stored in `/monitors.cfg`: plain text, line-based, with `#MU1` as the
header and a `.` as the closing line. Writing goes to `/monitors.tmp` and is then
renamed, so a node that loses power mid-write keeps the old, complete version. If
the closing line is missing, **everything** is rejected rather than half applied:
half a monitor list is worse than none, because then the node quietly monitors
less than you think.

Text and not a binary struct, for three reasons: it can be read with `cat` over
the serial console, an extra field does not make older files unreadable, and there
is no alignment or endianness to get wrong.

## The DM commands

These are the basic DM commands of the sensor core. The room variant layers the far
larger, permission-gated **room/DM command set** on top (see *The room-server* →
*The room/DM command set*): `ping`/`port`/`http`/`scan`/`traceroute`,
`checknow`/`mute`/`snooze`, `sendto`/`reboot`/`ntp` and so on. The four below stay
the core.

Four commands, in an ordinary text message over the mesh:

    list            all sensors: channel, name, state
    get <name>      one sensor with detail
    status          own state: power, WiFi, uptime, heap
    help  (or ?)    the commands

**This is the only path by which a name travels over the mesh.** CayenneLPP has no
name field, in any version. So `list` is not a convenience but the key to the
telemetry: without that list a receiver does not know what channel 7 means, and
with a stale list it reads wrong figures with no error. Whoever changes the shape
of `list` changes the only name carrier there is.

The shape of a reply:

- Replies go as **`TXT_TYPE_PLAIN`** and not as `TXT_TYPE_CLI_DATA`, because CLI
  replies are by design not acknowledged — and then you do not know whether your
  reply arrived.
- Chunked at **158 bytes**, not 160. `MAX_TEXT_LEN` is 160, but
  `composeMsgPacket` refuses to re-send a message longer than `MAX_TEXT_LEN-2`
  once `attempt > 3`. Chunking at 160 produces exactly the chunks that can never
  be re-sent. **Do not put this back to 160.**
- A chunk marker `[2/5] ` is 6 bytes and is subtracted from the budget up front —
  `sendGroupMessage` truncates a prefix that does not fit **silently**. So 152
  bytes of text per chunk; a single-chunk reply carries no prefix and may use the
  full 158.
- At most **four chunks**. At SF11 a full message costs roughly a second of
  airtime, and that second belongs to everyone — including the repeaters that
  relay it. What does not fit is not dropped silently: the last chunk ends with a
  line saying how many rows did not make it.
- Three attempts per chunk, against real ACKs. The protocol's attempt counter is
  two bits wide, so four is the maximum and three leaves one spare.
- One reply at a time. A question arriving from somebody else in the meantime is
  deliberately **not** acknowledged: their client reports "not delivered" and they
  ask again, by which time the previous turn is done. A queue would mean a handful
  of askers could jointly have twenty messages transmitted.

An unauthorised sender gets one short refusal with no detail at all, and at most
once per ten minutes. Silently ignoring was rejected because the sender then
cannot tell whether the node is broken, out of range, or refusing them — which
makes them retry, and that costs more airtime than an answer.

Note: **in practice the DM commands only reach an admin.** See
[open-issues.md](open-issues.md).

## Access control

The access list and the lock that enforces it belong to the mesh, not to the web
server: otherwise a node without WiFi would have no access control. The web
interface is one of the control paths; the others are `setperm` and
`set acl.strict` over serial or over a DM.

**Permissions.** The role (guest, read-only, admin) plus two bits for alerts
(`PERM_RECV_ALERTS_LO` / `_HI`). The guest role means something specific here: may
be alerted, may not read. That is a sensible permission — think of a phone that
only needs to receive alarms — and it is why a few lines sit beside
`ClientACL::applyPermissions`, which would delete such an entry outright.

**The lock (`acl.strict`).** With it on, the node answers a telemetry request only
to a sender with at least read permission. With it off, upstream behaviour stands:
anybody on the mesh may read. **Off is the default**, deliberately: flashing must
not lock a working setup out of itself. The web interface says in so many words
that the door is open in that case.

A refused request is **silence**, not an empty answer. An empty answer costs
airtime per refused request, and nothing stops an asker asking every second — at
which point the refusal itself becomes a way to burn this node's duty cycle. On
top of that, "zero sensors" reads in an app as a broken sensor rather than as a
closed door.

The lock is not in `NodePrefs`: that struct is upstream's, and adding a field
touches every other role and the format of `/new_prefs`. One byte in its own small
file is the cheapest honest answer. The lock is written to flash **immediately** —
someone who closes it and reboots at once must find it closed — while ACL changes
are written lazily, because the list is dirtied by every path message from an
admin.

**Adding and removing keys.** Adding requires 64 hex characters, because the
shared secret is computed from the full public key: a prefix entry would be an
entry you cannot talk to, and a prefix can be forged by grinding key pairs.
Removal is allowed from 6 bytes — that is what MeshCore itself returns over the
mesh — and is refused as soon as the prefix matches more than one entry.

**The neighbour list.** Adverts are caught in `onAdvertRecv()`, the existing
virtual hook, so with no patch in `src/`. That spot because it runs *after* the
ed25519 check: the key is then provably the sender's, which is the precondition for
using it as a pick list for the ACL. The list held twelve entries at first; since
v2.3.0 it holds **200** (for name resolution, see *The room-server* → *Name
resolution*), LRU by last-heard. It merges on key (a true ring would show the same
busy neighbour again and again after 200 adverts) and is **not** in SPIFFS: it
describes what is on the air *now*, and a saved copy would claim a node is nearby
that has been gone for a week.

A neighbour is listed with **SNR and not RSSI**, plus the hop count.
`mesh::Packet` keeps only `_snr`; RSSI exists only live in the radio, and flood
packets go into the queue with a score delay — by then that register belongs to a
later packet.

**The way in is the admin password.** A login with the correct password adds a
node automatically as **admin**, even with the lock closed, and thereby gives it
the whole CLI. The lock and the password are one security measure, not two.

## Alerts over the mesh

The alerting loop is upstream `SensorMesh`'s; MeshUptime supplies the conditions.
Every read round (60 s by default) `onSensorDataRead()` looks at the battery
voltage and at every monitor slot and sets a `Trigger` per case. When a trigger
fires, it walks the access list and sends to everyone with the matching alert bit:
`PERM_RECV_ALERTS_HI` for high priority, `PERM_RECV_ALERTS_LO` for low.

- One `Trigger` per **slot** and not per active monitor: that association must not
  shift when a monitor disappears. Costs roughly 1,7 kB, because every trigger
  carries a text buffer.
- At most **two** monitor alerts at a time. The queue has four places and the
  battery already uses two; allowing more would mean a real battery alert being
  skipped silently. If an alert is skipped because the queue is full, the trigger
  is left untouched and the next read round tries again — nothing is lost.
- Monitor alerts are **low** priority, so one attempt per contact instead of four.

**These alerts are built but have never fired on the device.** See
[open-issues.md](open-issues.md); that is one of the most important open items.

## The screen

The OLED is upstream `simple_sensor`'s: boot logo, then the ordinary MeshCore
screens, with an automatic switch-off after 20 s. Nothing was added for
MeshUptime — the administration paths are the web interface, the CLI and the mesh.

## Building and flashing

See [README.md](README.md) in this directory.
