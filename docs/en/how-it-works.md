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

The node advertises as **`ADV_TYPE_SENSOR`** (`SensorMesh.cpp:297`) and answers
node discovery with `CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_SENSOR`. That is the
honest type: this device offers sensors.

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

## The web interface

One page from PROGMEM, one web server, no framework, no CDN, no external
stylesheet, no external font: a node without internet must be able to serve its
own admin page. Dark by default, light through `prefers-color-scheme`, system font
stacks with mono for the figures. Three tabs — monitoring, access, node — with
collapsible sections.

Colour is semantic, not decorative, which is what the `sev` field is for: "on" on
channel 2 (mains) is good, "on" on channel 3 (battery) is a warning. A page that
guesses the colour from the word paints those two the same, and then the colour
lies.

Everything sits behind HTTP Basic auth. Username and password come from build
flags (`WEB_USER` / `WEB_PASS`) and are still at their example values — see
[open-issues.md](open-issues.md).

### The routes

| route | method | what it does |
|---|---|---|
| `/` | GET | the page |
| `/status.json` | GET | everything: power, WiFi, monitors, heap, uptime |
| `/cfg.json` | GET | the whole of `NodePrefs` in one request |
| `/acl.json` | GET | access list, lock state, neighbour list |
| `/wifi` | POST | network settings |
| `/hook` | GET + POST | supply a verdict from outside |
| `/monitor`, `/monitor/del` | POST | create, delete a monitor |
| `/acl`, `/acl/del`, `/acl/strict` | POST | manage access |
| `/cli` | POST | the only write path for node administration |

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
using it as a pick list for the ACL. The list holds twelve entries, merges on key
(a true ring would show the same busy neighbour twelve times after twelve
adverts), and is **not** in SPIFFS: it describes what is on the air *now*, and a
saved copy would claim a node is nearby that has been gone for a week.

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
