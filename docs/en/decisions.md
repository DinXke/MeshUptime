# Decisions, with the reasons

This file keeps *why* the firmware is the way it is — and deliberately also the
choices that were later **reversed**, because a reversed choice says more than one
that fell into place by itself. What is kept is the reasoning; what is not kept is
the chronology around it.

The figures used as evidence below are in [measurements.md](measurements.md); how
it works now is in [how-it-works.md](how-it-works.md).

## Which base: simple_sensor, via a detour through companion_radio

Position changed three times, and the route is worth keeping.

1. **First `companion_radio`**, because that is what the node already was: a
   companion with a home-grown web page, and that page was the administration
   path. From there the following still had to be added: answering telemetry
   requests, history, `CommonCLI` with passwords, forwarding safeguards — and the
   chat, contacts and channels had to come out.
2. **Then `simple_sensor`**, after a look at the source. That example is almost
   literally MeshUptime: `ADV_TYPE_SENSOR`, forwarding *with* safeguards,
   answering `REQ_TYPE_GET_TELEMETRY_DATA`, history on the device itself, and
   `CommonCLI` with passwords and CLI over DM. Exactly the list that would have
   had to be built on top of `companion_radio` was already finished there. What it
   does not have is WiFi.
3. **Then `companion_radio` again**, when Home Assistant became a requirement: it
   connected over the client protocol on port 5000, and that is the companion.
4. **Then `simple_sensor` for good**, when that requirement went away. The node is
   no longer Home Assistant's mesh gateway; that role was deliberately given up,
   and the old firmware and key are in a backup outside the repository.

What the detour produced: the list "must be added on top of `simple_sensor`"
(WiFi, web server, webhook, ping) is shorter than the reverse list, and it contains
nothing that touches the mesh.

**An assumption that collapsed along the way:** the upstream companion has no web
server. `WIFI_SSID` there enables `SerialWifiInterface` — the client protocol over
TCP, so the MeshCore app can connect over WiFi instead of BLE or USB. The web page
this node had was home-grown. So the web interface had to be written from scratch
on either base, which is an argument for neither.

## One radio identity, not two

The question was whether a node that both monitors and forwards "goes wrong on the
mesh". Checked in upstream, and the answer is the opposite.

| example | advertises as | forwards |
|---|---|---|
| `simple_repeater` | `ADV_TYPE_REPEATER` | yes, six checks |
| `simple_room_server` | `ADV_TYPE_ROOM` | yes, the same checks |
| `simple_sensor` | `ADV_TYPE_SENSOR` | yes, `disable_fwd` + `flood_max` |
| `companion_radio` | `ADV_TYPE_CHAT` | yes, but without a single check |

Forwarding in MeshCore is **not tied to the advert type**:
`allowPacketForward()` is consulted whatever a node announces. An identity that
forwards *and* runs an application is therefore the rule, not the exception.

**What is actually wrong is the wrong advert type.** A node that announces
`ADV_TYPE_CHAT` and relays other people's traffic is being dishonest with the
mesh: nobody can see it is infrastructure, nobody can ask it for statistics. The
fix is not a second identity but announcing the right type — `ADV_TYPE_SENSOR`.

Two radio identities have no example in upstream at all, and it requires a patch to
`src/Mesh.cpp` in nine places. MeshStats has already shown what a core patch
costs: it has to be re-applied and re-reviewed at every upstream version. The price
of one identity is that the node appears in one list in the MeshCore app rather
than two. The gain is no core patch.

**A correction to the assumption the idea came from:** SIREN does not do it
either. It sets `self_id = rooms[0].id` and leaves routing alone; the keys of rooms
1..N live above the mesh layer and encrypt room content. SIREN has one radio
identity.

The design of the patch that would be needed if the choice is ever reversed is kept
in [../../firmware/patches/LEESMIJ.md](../../firmware/patches/LEESMIJ.md), with the
trap included: the destination hash is one byte, so two random keys have a 1 in 256
chance of colliding, and then routing is ambiguous.

## Airtime: forwarding is the cost, not the monitoring

- The SX1262 has one modem. Receiving happens once regardless; two roles on one
  radio do not double it. That part is free.
- Transmitting is the scarce side. A monitoring alert is rare (only on a state
  change) and short (158 bytes at most). Against what a repeater transmits in
  relayed traffic, that is noise.
- The 1% duty cycle on 868 MHz is therefore consumed by the repeater function and
  not by the monitoring. Anyone worried about airtime should look at `flood_max`
  and loop detection, not at the number of sensors.

One real interaction remains: if an alert is ready while the radio is transmitting
a relayed packet, the alert waits. Hence the design rule that the radio comes
first and an alert belongs in the queue, not sent blocking.

## The repeater role comes last, and not with the companion flag

The companion has `repeat.disable_fwd`, but its `RepeatPrefs` is a copy of
`CommonCLI`'s in which four fields are **commented out**: `flood_max`,
`flood_max_unscoped`, `flood_max_advert` and `loop_detect`. The real repeater
performs six checks. Enabling `repeat` as it stood yields a repeater that relays
everything a real repeater filters out, flood loops included.

That is where the airtime problem lives — and not where the question suspected it.
A repeater button in the web interface without those four fields is a button that
does damage.

`simple_sensor` does have `disable_fwd` and `flood_max`, so for MeshUptime the
repeater role is a flag, not a new design. It is last in line because that is also
the safest order.

## Channel numbers are immutable

This is the single most important agreement in the project, and it comes from a
class of fault that has already cost time twice in MeshManager: the verdict stamped
on the wrong packet, and the MQTT topic that was computed instead of remembered.

CayenneLPP carries no names. A polling node therefore stores "channel 6 = google"
itself. If that number shifts after a deletion, the mapping silently points at
another service: no error, just wrong figures on a dashboard. **Remember, do not
recompute.** That costs one field in storage — the `ch_ever_used` mask — and it
prevents a failure that presents itself as "the figures are a bit off".

Without that byte the rule that numbers are never reused would only hold for one
power cycle.

## Names travel over the mesh by DM, not by telemetry

There is no better LPP type to pick; the format simply cannot carry a name.
Checked:

| candidate for ping time | why not |
|---|---|
| `analoginput` | 2 bytes, 0,01 resolution → dies above 327 ms |
| `luminosity` | 2 bytes, but claims "lux", which is untrue |
| `frequency` | 4 bytes exact, but claims "hertz" |
| **`genericsensor`** | 4 bytes, multiplier 1, exact integers, and promises nothing untrue |

Another type would not supply a name, only a different wrong word. So names travel
by three other paths: the DM command `list` (the only path over the mesh), the web
interface with the full channel map, and a dashboard that stores the mapping.

## Monitoring supplied from outside, *and* our own pings

An existing Uptime Kuma is better at monitoring than this device will ever be: it
has retries, maintenance windows and history. What it cannot do is deliver an alert
when the internet is gone.

Hence the division of labour: an **incoming webhook** (`/hook`) through which Kuma
pushes its verdict here, plus **our own checks** for the case where Kuma itself is
unreachable — which is precisely the case you want to know about. Kuma knows
*whether* something is broken; this node is the only one that can still say so.

The choice between ICMP and a TCP connection was initially left open ("measure
which costs less"). It became ICMP: one ping at a time, in steps, never waiting,
with name resolution repeated at most every ten minutes.

**One channel allocator, not two.** Pushed services go through the same
`allocChannel()` and the same `ch_ever_used` byte as the ping monitors; the
distinction lives in the address field. The old eight-entry hook table was removed
— that was a second set of books alongside the monitors, and two sets of books
about the same things diverge after a deletion. `createMonitor()` is now the only
place a monitor comes into being; CLI, web and `/hook` all pass through it.

## Alerts over LoRa and not over the network

Not assumed but measured: precisely during a power cut the network path is the
least reliable. See [measurements.md](measurements.md) — the node dropped off the
network on battery with `reason 202` at good signal strength and did not recover
without a hard reset.

From that it also follows that the WiFi watchdog is not a luxury, and that the
node's own checks are the core rather than an extra.

## Replies as TXT_TYPE_PLAIN, and chunking at 158

`BaseChatMesh` states explicitly that replies of type `TXT_TYPE_CLI_DATA` are not
acknowledged. If a DM command is answered as CLI data, **we do not know whether the
answer arrived**. As ordinary text an ACK does come back. Delivery confirmation on
replies therefore requires `TXT_TYPE_PLAIN`.

And that choice is a dead letter without retrying on the ACK: knowing the answer
did not arrive is only worth something if you act on it.

Delivery confirmation did not have to be invented. The protocol computes an
`expected_ack` itself (four bytes of SHA-256 over timestamp, attempt number and
text, with the sender's own public key), so an ACK proves **this** text arrived and
not merely that something arrived. Sending also returns an `est_timeout`: how long
waiting is reasonable given the hop count. Do not pick a number yourself.

The 158-byte limit rather than 160 is explained in
[measurements.md](measurements.md) and [how-it-works.md](how-it-works.md).

## A new key pair at the role change

The node was an `ADV_TYPE_CHAT` companion and became an `ADV_TYPE_SENSOR`. Reusing
the same key would mean that somebody else's contact list holds a chat contact that
quietly changes kind. So: a new key.

The price is that anyone who had this node as a contact has to add it again. For a
monitoring node that is correct behaviour: it has become a different device. The old
key is not lost — a full flash dump plus the separate SPIFFS partition are outside
the repository, with recovery instructions.

## The stored setting beats the baked-in one

WiFi in four layers, and that order is the safety order: build flag → serial
console → own access point as fallback → web interface, with the **stored** value
overriding the baked-in one. So a node that moves to another network does not have
to be reflashed, and a node you give away can be cleaned by wiping storage.

One thing belongs with that honestly: **a baked-in password can be read back from a
flash dump.** For a device on your own network that is acceptable; it is no longer
a secret once somebody has the node physically.

The sensor settings needed storage of their own. `CommonCLI` does have
`sensor set`, but it does not persist the result: at startup exactly one sensor
setting is restored from preferences (`gps`). An adjusted `mains.hi` therefore held
until the next reboot — precisely the setting the measurement said would need
adjusting one day. And ping monitors would have to be retyped after every power
cut, which is no option for a monitoring node.

## The custom SensorManager, and why the two easy routes were closed

Custom fields in the telemetry reply cannot be added from application code:

- `SensorMesh::handleRequest` is not declared `virtual`, so it cannot be
  overridden.
- `reply_data` is `private:`, so the buffer cannot be topped up either.

More importantly: **`handleRequest` does not call `onSensorDataRead()`**, whereas
the periodic round does. Sensors added only there would be tracked but would **not
appear in the reply to a telemetry request** — a silent fault, found before any
code existed.

`SensorManager::querySensors()` is the only hook that runs in both paths. That
class also already has a settings interface (`getNumSettings` / `getSettingName` /
`setSettingValue`), which is exactly what the monitors need: name, address and
interval settable over serial and over DM-CLI without a parser of our own.

The price is one patch: the `sensors` global is hardcoded to
`EnvironmentSensorManager` in the variant. Three lines replaced and a macro with a
fallback added; without the macros the variant builds exactly what it built before,
so the patch is a candidate to offer upstream one day.

## Refusal is silence, not an empty answer

For a telemetry request without permission: `return 0`, the existing path for "no
answer". An empty answer costs airtime per refused request, and nothing stops an
asker asking every second — at which point the refusal itself becomes a way to burn
the duty cycle. On top of that, "zero sensors" reads in an app as a broken sensor
rather than a closed door. Silence is indistinguishable from out of range, and that
is exactly what an asker without permission should see.

For a **DM command** the trade-off came out differently: there, one short refusal
goes out, at most once per ten minutes per sender. The difference is who is on the
other side. A DM sender is already in the access list and therefore already knows
this node exists; silently ignoring them would only make them retry, and that costs
more airtime than one answer.

## A correction to what used to stand here about access

It said that **every** node on the mesh could read the sensors. That was too
strong, and the nuance is worth keeping.

The hole in `handleRequest` was real: `perms` came in and was not used, so even an
entry with alert permission only could read everything. But that is not the path by
which a second node saw six channels: only nodes **already in the access list**
reach `onPeerDataRecv`, because `searchPeersByHash` searches nothing else.

The way in is the login with the admin password — and that adds a node
automatically as **admin**, even with the lock closed. Which is why the lock and the
password are one security measure and not two: enabling `acl.strict` without
changing the password is locking a door next to an open window.

## Deliberately not done

- **Opening the DM commands to read-only contacts.** It is one condition to
  remove. Not done because widening permissions is the owner's call and not an
  agent's. See [open-issues.md](open-issues.md).
- **Fixing the channel collision with the base class.**
  `EnvironmentSensorManager::querySensors()` resets its channel counter to 2 and
  hands out from there to discovered I2C sensors, on top of our fixed 2/3/4. Not
  fixable without cutting into the base class, because the reset happens inside the
  parent. This node has no environment sensors and `begin()` warns as soon as one
  turns up.
- **One cosmetic log line.** A transition to online still prints `(reden 0, ...)`;
  the reason has no business being there. The substantive error is gone (it used to
  print a misleading 202). Left alone because a flash for one log line is not worth
  it.

## The rules that are not negotiable

They are here because they decided every round.

1. Allocate statically: a fixed maximum number of monitors, fixed buffers, no
   `String` in our own processing paths.
2. No second web server.
3. Measure every addition, before and after, on the **largest free block**.
4. The radio comes first: a monitoring round must not hold up mesh timing, and
   nowhere is there a wait or a `delay()`.
5. The identity stays. The backup is outside the repository; the recovery path is
   documented next to the dump.
6. One sieve per kind of input. Two sieves that do 99% of the same thing will one
   day accept a name on one path that the other rejects — and you only find that by
   trying both paths side by side.
