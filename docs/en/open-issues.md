# What does not work

This is the most important file in this documentation. Anyone picking the project
up after six months will get more out of this page than out of all the others
together: it is full of things that have already been investigated, and therefore
do not need investigating again.

Ordered by importance.

---

## 1. MeshManager cannot poll these sensors over the mesh

**The polling round stalls on `LOGIN_NOANSWER`** — `lr=2` in the repeater's
`/api/mon`. The node does not answer the login, so a telemetry request is never
made.

### What is established

- The repeater runs MeshManager **2.9.0**.
- The repeater **hears the node's advert**: it appears in the heard list with
  `t: 4` (`ADV_TYPE_SENSOR`).
- The prefix resolves and the node is present in MeshManager as a monitor entry
  with `res=1`.
- **Other nodes are polled successfully by the same repeater.** For those the
  trace shows `login OK`, `status ok`, `nbrs len=144`, `pub`. So the repeater does
  monitor; just not this node.
- MeshManager's own documentation says why this is so hard to catch: *"A refused
  login cannot be told from an unreachable one — the far side answers a rejected
  login with silence."* Hence the name `LOGIN_NOANSWER` rather than a pretence of
  knowing which of the two it is.

### Four suspicions that have been INVESTIGATED AND RULED OUT

This is the most valuable knowledge on this page. **Do not chase them again.**

**1. The node's clock read 15 May 2024.** That is the ESP32 clock's fixed
fallback: `ESP32RTCClock::begin()` sets `1715770351` on a cold start
(`src/helpers/ESP32Board.h:208`), and the Heltec V3 uses that clock as the fallback
behind `AutoDiscoverRTCClock` (`variants/heltec_v3/target.cpp:15`). There is no
battery-backed clock on this board, so it happens again after every cold start. A
login carries a timestamp and `handleLoginReq` refuses a timestamp that is not
newer than the previous one, so this was a good suspicion. **The clock was set
correctly and the fault remained.**

*Correction to the earlier note:* it pointed at `CommonCLI.cpp:188`. The same
constant is there, but that is the `clkreboot` command — something you invoke
yourself, not the fallback behaviour at boot. The same value also appears in
`VolatileRTCClock` (`src/helpers/ArduinoHelpers.h:11`), which this board does not
use.

**2. The `acl.strict` lock.** Ruled out both by reading the code and by trying.
`acl_strict` is consulted in exactly one place: in `handleRequest`, for
`REQ_TYPE_GET_TELEMETRY_DATA`. It does not appear in `handleLoginReq` at all, so
the lock cannot refuse a login. **With the lock off: same fault.**

**3. The shared secret of an ACL entry added through the web interface.**
`handleLoginReq` sets `client->shared_secret` only in the password branch; in the
blank-password branch (a node already in the list) it does not. That much is true —
but `aclSetPerms` computes the secret at the moment of adding (directly with
`calcSharedSecret`, or via `ClientACL::applyPermissions`), so an entry from the web
interface is not missing it. **Entry deleted and a password login performed: same
fault.**

**4. The advert type.** MeshManager's candidate list filters on `ADV_TYPE`
(`MeshManagerNet.cpp`, around line 6784). That list initially admitted only
`ADV_TYPE_REPEATER`, which meant the one kind of node that exists to answer
telemetry was the one kind it would not offer. **A real interaction, and since
fixed — but not the cause:** it was a gap in *discovery*, not a block. `mon add`
and the polling round take an explicit key and never looked at the advert type, and
this node was already in the monitor list with `res=1`.

### The next diagnostic step, not yet taken

**Read `wifi mon trace` on the repeater**, over the serial console or via that
repeater's admin page. That trace is the only place where you can see how far the
round for *this* node gets: whether the login goes out at all, over which path, and
whether anything comes back that is not recognised as an answer. Without it every
further suspicion is guesswork again, because silence on our side and silence on
the far side look identical.

What comes into view after that, in this order: whether the path is right (flood
versus direct, and the hop count), and whether the password MeshManager uses for
this entry actually belongs to this node — the MeshManager side truncates a
password at 15 characters.

---

## 2. The admin password is still at its build-flag value

`ADMIN_PASSWORD` in the build flags has never been changed, and that is not
cosmetic: **anyone who knows that value can log in over the mesh, is made an admin
automatically, and gets not just the sensors but the entire CLI.** A login with the
correct password adds the sender with `PERM_ACL_ADMIN`, even with the `acl.strict`
lock closed.

The lock and the password are therefore one security measure, not two. Set
`password <new>` over the serial console; the stored value overrides the baked-in
one. The web interface compares against the build flag and shows a banner until it
has been set.

The same goes for the web interface: `WEB_USER` and `WEB_PASS` are at their example
values in the code. And **Basic auth over plain HTTP sends the password readable
over the network** — defensible on your own LAN, not enough for a node that becomes
reachable from outside.

---

## 3. The alerts are built but have never fired

The whole chain is there — the conditions per monitor slot, the four-place queue,
the choice of recipients on `PERM_RECV_ALERTS_LO` / `_HI`, the retrying and the
waiting on a real ACK — but **no real alert has ever gone over the radio**. So this
is "built", not "built and seen".

That is the worst kind of unknown: a node that only sends its first message during a
real outage is a node nobody knows will get through — and you find out at the
moment it matters.

Work is in progress in the working tree to be able to provoke this: a simulation
mode that forces a sensor temporarily and thereby pushes the message through the
**real** path, with a mandatory expiry so a node cannot be left in test mode. That
work is not committed yet and the build is temporarily broken because of it; this
page will be updated once there is something to report that has been seen on the
device.

---

## 4. Two stack overflows in upstream CommonCLI, not yet reported upstream

Both are in `src/helpers/CommonCLI.cpp` and are not this project's. The edge has
been closed on the side we own, but the fault is still there.

**`sensor list`.** The loop writes with an **unbounded `sprintf`** and only stops
once the write position has passed 134; after that `... next:N` is appended:

    for (i = start; i < end && (dp-reply < 134); i++) {
      sprintf(dp, "%s=%s\n", getSettingName(i), getSettingValue(i));
      ...
    }
    if (i < end) sprintf(dp, "... next:%d", i);

That limit of 134 assumes short values. With a line like
`mon.5.host=<an IP address>` the position after that line is 156, and the eleven
bytes of `... next:8` take it to **168 in a buffer of 160**. Measured: the node went
down in `Guru Meditation Error (LoadProhibited)`, `EXCVADDR: 0x0000000c`, and
precisely on `sensor list` and `sensor list 0` while `sensor list 1` survived — with
start index 1 it ends at 141 and just fits.

**This is reachable over the mesh**, since the same CLI can be invoked by DM by an
admin. Our side: `reply[160]` → `reply[256]` in `main.cpp` and `temp[166]` →
`temp[262]` in `SensorMesh.cpp`. With a hostname of at most 40 characters the upper
bound is 134 + 53 + 12 = 199, so 256 leaves room.

**`sensor set`.** `strcpy(tmp, &command[11])` with no length check, at
`CommonCLI.cpp:284`, into the buffer `char tmp[PRV_KEY_SIZE*2 + 4]` declared at
`CommonCLI.h:258`. With `PRV_KEY_SIZE` at 64 that is **132 bytes**, and the command
line may be 160 long: a `sensor set` of more than 143 characters overruns it. Not
measured, but it can be worked out on paper.

*Correction to the earlier note:* it said "a buffer of 68 bytes at
`CommonCLI.h:258`". The line is right, the size is not — 68 would hold if
`PRV_KEY_SIZE` were 32. And the `strcpy` itself is in the `.cpp`, not the header.

**Both should be reported upstream.** That has not happened yet.

---

## 5. The DM commands only reach an admin

`SensorMesh::onPeerDataRecv` only lets a text message through to
`handleIncomingMsg` (and therefore to `DmCommands`) if the sender is
`from->isAdmin()`. The permission test in `DmCommands::isAllowed()` is wider — it
also admits read permission and the two alert bits — but in practice that code is
never reached by anything other than an admin.

Opening it up to read-only contacts means removing that condition and letting
`isAllowed()` do the test. **Deliberately not done:** widening permissions is the
owner's call, not that of whoever writes the code.

The awkward consequence: `list` is the only path by which a name travels over the
mesh, so a node with read permission only can poll telemetry but cannot find out
what channel 7 means.

---

## 6. `region load` silently does nothing, and `region save` fails

For `region load`, `CommonCLI` calls the callback `startRegionsLoad()`. In
`CommonCLI.h` that is a virtual method with an **empty** body, and `SensorMesh` does
not override it — only `simple_repeater` and `simple_room_server` do. The command
also sets no reply text, so you get literally nothing back and nothing happens.

`region save` goes the same way through `saveRegions()`, which returns `false` by
default; that command therefore answers `Err - save failed`.

For this role that is no disaster — a sensor node has no need of a region map — but
it is a command that exists in the web console and does not do what it promises.

---

## 7. Commands that answer without doing anything

They are on the admin page by now, but they belong together here:

- `neighbors` answers `"not supported"`: `formatNeighborsReply()` is a fixed string
  in this role. The real neighbour list is on the access tab of the web interface,
  fed from `onAdvertRecv()`.
- `log …` and `clear stats` likewise: the callbacks are empty in this role.
- `get pass` does not exist; the admin password can only be **set**.
- `bw`, `sf` and `cr` have no `set` of their own: only `set radio <f,bw,sf,cr>`.
  Which is fortunate, because you cannot set them halfway.

---

## 8. Channel collision with the base class

`EnvironmentSensorManager::querySensors()` resets its channel counter to 2 and hands
out from there to discovered I2C environment sensors — on top of our fixed channels
2, 3 and 4. In the format that does not collide (a channel may carry several types
and a decoder looks at the type), but it makes a dashboard confusing.

Not fixable without cutting into the base class, because the reset happens inside
the parent. This node has no environment sensors attached, and `begin()` warns in
the debug output as soon as one turns up.

---

## 9. The repeater role does not exist yet

Forwarding is off (`disable_fwd`). The base has `disable_fwd` and `flood_max`, so
enabling it is a flag — but the four repeater fields a real repeater uses
(`flood_max_unscoped`, `flood_max_advert`, `loop_detect`, and the region and
transport-code checks) are not all in play in this role. While that is so, a
repeater button in the web interface is a button that can do damage.

The second identity that used to come with it is a choice that was **rejected** —
see [decisions.md](decisions.md). The design of that patch is kept in
[../../firmware/patches/LEESMIJ.md](../../firmware/patches/LEESMIJ.md) in case the
choice is ever reversed; it is **not** applied.

---

## 10. Fragmentation from the web server

The largest free block dropped from 225.268 to 163.828 after serving web requests.
Ample, but this is the figure that has governed the project from the start, and the
page has grown from 11 kB to 66 kB since. Anyone adding to it should measure before
and after.

---

## 11. Things that are deliberately gone

Not defects, but the kind of surprise that costs somebody an evening:

- **Port 5000, and with it Home Assistant's mesh connection.** This node was the
  gateway; that role was given up at the switch to `simple_sensor`. The old
  firmware and key are in a backup outside the repository.
- **The old key.** The node has a new key pair; anyone who had it as a contact has
  to add it again.
