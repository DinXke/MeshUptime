# MeshUptime — English documentation

MeshUptime is monitoring firmware for a Heltec LoRa32 V3: Uptime Kuma in spirit,
but with LoRa as the way out. It runs on a MeshCore node, offers its results as
telemetry sensors another node can poll, and raises alerts over the mesh when
something falls over.

It began as a single sensor node and grew into a **multiroom room-server**: one
device serves several rooms, virtual sensor-nodes and a chat bot at once, and runs
its own network diagnostics (ping, port, http, traceroute) and SNMP polls. The bot
is now a **two-way** mesh-diagnostics responder — it answers `ping`/`test`/`path`
both by DM and inside **hashtag/public channels** — the node syncs its clock over
**NTP** and shows human-facing times in the local timezone, and a **big contact
list** resolves node names everywhere. The current version is **MeshUptime v2.3.2**
(own versioning, separate from the MeshCore library v1.17.0); the branding shows
both: `MeshUptime v2.3.2 (by DinX) - MeshCore v1.17.0`. The feature history is in
[`firmware/CHANGELOG.md`](../../firmware/CHANGELOG.md).

There are **two PlatformIO envs**:

- **`meshuptime_room`** — the room-server variant (build flag `ROOM_SERVER_VARIANT`,
  `main_room.cpp` / `RoomMesh`). **This is the current primary product.**
- **`meshuptime`** — the original sensor variant (`main.cpp` / `SensorMesh`,
  advertises `ADV_TYPE_SENSOR`). Stays the rollback path and compiles unchanged.

The project is Dutch-first. These pages are the English counterparts of the files
in [`docs/`](../); the Dutch versions are the ones kept in step with the code
first, so if the two ever disagree, the Dutch page is the newer one.

| page | what is in it |
|---|---|
| **[open-issues.md](open-issues.md)** | **what does not work, and which suspicions have already been investigated and ruled out. Start here.** |
| [how-it-works.md](how-it-works.md) | roles, channel map, the two-way bot (DM + channels), NTP/timezone, hashtag/public channels, name resolution, web interface, CLI, DM commands, access control, alerts |
| [measurements.md](measurements.md) | every number with its date: power thresholds, WiFi behaviour on battery, heap per build, the byte budget |
| [decisions.md](decisions.md) | the choices with their reasons, explicitly including the ones that were later reversed |
| [building.md](building.md) | the two build ways, MeshCore as a pinned submodule dependency, the patch hook, and the CI tag matrix |

The raw measurement series is language-neutral:
[meting-voeding-2026-08-19.log](../meting-voeding-2026-08-19.log).

## The short version

Base is `examples/simple_sensor` from MeshCore tag `companion-v1.17.0`. The sensor
env advertises as `ADV_TYPE_SENSOR`; the room env serves rooms (`ADV_TYPE_ROOM`),
virtual sensor-nodes (`ADV_TYPE_SENSOR`) and a bot (`ADV_TYPE_CHAT`).

Recent builds:

| env | RAM | flash |
|---|---|---|
| `meshuptime_room` (primary) | ~54% (of 327.680) | ~46% (of 3.342.336) |
| `meshuptime` (rollback) | ~47% (of 327.680) | ~45% (of 3.342.336) |

The room env is larger — it carries the extra panels (rooms, sensor-nodes, bot,
SNMP, channels, time), the async network engine and the big advert/contact list
(~13.6 kB RAM, RAM-only). The older, smaller sensor-only figures are kept in
[measurements.md](measurements.md) as a historical baseline.

Confirmed on the device (in the sensor base): the power sensors with their
calibrated thresholds, the WiFi sensor and its watchdog, ping monitors that survive
a reboot, `/hook` for an existing Uptime Kuma, telemetry actually polled and read by
a second node, the web interface, and the serial CLI.

The room-server variant (v2.0.0 → v2.3.2) is the primary product: multiroom
(`MAX_ROOMS=4`), virtual sensor-nodes (`MAX_SENSOR_NODES=4`), passwordless per-key
ACL grants (`MAX_ACL_GRANTS=16`), a room/DM command set, the async network-task
engine (`port`/`scan`/`http`/`traceroute`), a node-side SNMP monitor kind, and a
chat bot for clean DM alerts that is also a two-way `ping`/`test`/`path` responder
over DM and in hashtag/public channels. NTP-synced local time (timezone-aware,
protocol/RTC stay UTC) and a 200-entry contact list resolve node names everywhere.
Full detail in [how-it-works.md](how-it-works.md).

Built but never actually fired: the mesh alerts. Biggest open problem: a
MeshManager repeater cannot poll these sensors — the round stalls on
`LOGIN_NOANSWER`. Both are in [open-issues.md](open-issues.md).

## Three kinds of claim

Kept strictly apart throughout this documentation, because the difference is the
whole point:

- **measured** — seen on the device, with the figure quoted
- **built** — the code is there and compiles, but the behaviour has not been
  confirmed on the device
- **reasoned** — derived from the source, not measured

## Building

MeshUptime is a **standalone PlatformIO project**; MeshCore is a pinned git
submodule under `firmware/vendor/MeshCore` (tag `companion-v1.17.0`). The full
story — both build ways, both envs, the CI tag matrix, why the patch runs via a
pre-build hook — is in [`building.md`](building.md).

The room-server variant (primary product):

    git submodule update --init firmware/vendor/MeshCore
    cd firmware
    python -m platformio run -e meshuptime_room
    python -m platformio run -e meshuptime_room -t upload --upload-port COM4

The sensor variant (rollback) is the same path with `-e meshuptime`. Both envs
share the same board, modules and patch hook; `meshuptime_room` extends
`meshuptime` and adds the `ROOM_SERVER_VARIANT` build flag on top.

WiFi credentials are **not** in this repository (placeholders in
`firmware/platformio.ini`; see
[`firmware/wifi.ini.voorbeeld`](../../firmware/wifi.ini.voorbeeld)). The single
patch against MeshCore in [`firmware/patches/`](../../firmware/patches/) — the
`sensors` global is hardcoded in the Heltec V3 variant — is applied automatically
by the pre-build hook; you only apply it by hand for the legacy build copy.

`upload` writes the program partition only, not SPIFFS, so the identity in
`/identity/_main.id` and the settings file survive.
