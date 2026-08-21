# Building

MeshUptime is a **standalone PlatformIO project**. MeshCore is not a tree you copy
our files into, but a **pinned dependency**: a git submodule under
`firmware/vendor/MeshCore`, fixed at tag `companion-v1.17.0`.

There are two ways. The first is preferred; the second stays around during the
transition so flashing does not break.

---

## The two envs

Separate from the two build ways, there are **two PlatformIO envs**, both for the
same board with the same modules:

| env | variant | main / mesh class | build flag |
|---|---|---|---|
| `meshuptime_room` | room-server (**primary**) | `main_room.cpp` / `RoomMesh` | `ROOM_SERVER_VARIANT=1` |
| `meshuptime` | sensor (rollback) | `main.cpp` / `SensorMesh` | — |

`meshuptime_room` **extends** `meshuptime` (`extends = env:meshuptime`) and only
adds the `ROOM_SERVER_VARIANT` build flag on top. The difference is in the
`build_src_filter`: the sensor env excludes `main_room.cpp` and `RoomMesh.cpp`, the
room env re-enables them and excludes `main.cpp`. `SensorMesh.cpp` stays in **both**
envs (the `WebTask` is coupled to it by type). Defaults for the room env —
`MAX_ROOMS=4`, `MAX_TOTAL_POSTS=48` — live in `RoomMesh.h` and can be overridden in
`env:meshuptime_room` if the RAM budget allows.

Last known build sizes (`Heltec_lora32_v3`, no PSRAM, 327,680 B RAM / 3,342,336 B
flash):

| env | RAM | flash |
|---|---|---|
| `meshuptime_room` | 40.5% (132,728) | 45.2% (1,509,589) |
| `meshuptime` | 33.9% (111,072) | 44.6% (1,491,325) |

The room env is larger because of the extra panels (rooms, sensor-nodes, bot, SNMP)
and the async network engine. If the figures differ sharply, something did not carry
over.

---

## Way 1 — the standalone project (preferred)

One source: this repo. No copy steps, no two-tree sync.

```sh
git clone https://github.com/DinXke/MeshUptime.git
cd MeshUptime
git submodule update --init firmware/vendor/MeshCore
cd firmware
python -m platformio run -e meshuptime_room
```

Flash (writes the program partition only, not SPIFFS — identity and settings
survive):

```sh
python -m platformio run -e meshuptime_room -t upload --upload-port COM4
```

Replace `meshuptime_room` with `meshuptime` for the sensor variant (rollback); all
other steps are the same.

Under the hood:

- **`firmware/platformio.ini`** is the project file. Its `[platformio]` section
  points PlatformIO's directories (`src_dir`, `boards_dir`, `lib_dir`,
  `include_dir`) at the submodule and loads MeshCore's own env definitions plus
  the heltec_v3 variant via `extra_configs`. Our `meshuptime` env extends
  `Heltec_lora32_v3`, exactly as in the old build copy.
- The **variants patch** (`firmware/patches/0001-...patch`) is applied to the
  submodule idempotently by the pre-build hook **`firmware/apply_patches.py`**.
  The vendored MeshCore tree therefore stays untouched in the repo at the pinned
  commit; the patch only lives in the build. Run twice and the second run does
  nothing (`al aangebracht, overgeslagen` — "already applied, skipped").
- A few flags differ from the old env because `src_dir` now points into the
  submodule and some MeshCore paths are project-relative: the include
  `-I vendor/MeshCore/variants/heltec_v3`, the `build_src_filter` reaching
  `+<../../../examples/meshuptime>`, and the OTA lib with an explicit `file://`
  path. This is purely build structure; firmware **behaviour** is unchanged. See
  the comments in `firmware/platformio.ini`.

### WiFi credentials

Are **not** in the repo. `firmware/platformio.ini` carries placeholders; the
credential stored on the device wins anyway (see `firmware/wifi.ini.voorbeeld`).
To pass real values at build time, put them in a local, uncommitted
`firmware/platformio.local.ini` (matched by `*.local.ini` in `.gitignore`).
**Never** put real values in `platformio.ini` or `platformio.ci.ini`.

### Why a submodule and not `lib_deps`

MeshCore's build relies entirely on its own `platformio.ini`: the env chain
`arduino_base → esp32_base → sensor_base → Heltec_lora32_v3`, its `variants/`, and
its `boards/`. As a PlatformIO library (via `library.json` + `build_as_lib.py`)
MeshCore ships only source files — none of that env/variant/board machinery. Our
env extends `Heltec_lora32_v3` directly, which is only possible with MeshCore's
config in play. `lib_deps` would force us to rebuild all of that ourselves:
exactly the drift we want gone. A submodule keeps the whole tree at the pinned
commit inside the repo. Tested empirically: the submodule path builds; the
lib_deps path would require a large manual reconstruction.

### The patch: hook, not hand-editing

The patch replaces three lines IN `variants/heltec_v3/target.{h,cpp}` (the
hardcoded `EnvironmentSensorManager sensors`). Those files compile via MeshCore's
own `build_src_filter`; overriding them without excluding the upstream files is
impossible, and excluding-plus-duplicating would bring back the very drift we are
removing. So the hook applies the small patch at build time instead of us
carrying our own copy of target.cpp. The cost: after a build the submodule shows
as "dirty" in `git status` (the patch is applied). That is intentional; a
`git -C firmware/vendor/MeshCore checkout -- .` resets it.

---

## Way 2 — the separate build copy (legacy, during the transition)

The old way: a separate MeshCore checkout (e.g. `C:\Users\Public\MeshUptime-build`)
at the tag, with the firmware copied in and a hand-written `platformio.local.ini`.
This stays working as long as the flashing workflow still uses it. In short:

1. A MeshCore checkout at `companion-v1.17.0`.
2. `git apply` of `firmware/patches/0001-...patch` onto that checkout.
3. `firmware/examples/meshuptime/*` into the checkout's `examples/meshuptime/`.
4. A `platformio.local.ini` (with your real WiFi) — MeshCore reads it via
   `extra_configs`. `firmware/platformio.ci.ini` is the placeholder mirror of it.
5. `pio run -e meshuptime` (or `-e meshuptime_room`) inside the checkout.

Downside — and the reason way 1 is preferred: this is the two-tree sync that gave
broken builds when several people/agents worked at once. Once flashing runs on
way 1, this copy (and `platformio.ci.ini`) can go.

---

## CI

`.github/workflows/build.yml` builds **way 1** against multiple upstream tags:

- **`companion-v1.17.0`** (pinned) — MUST be green.
- **the latest upstream companion tag** (discovered dynamically) — informational,
  `continue-on-error`. If it fails, a MeshCore release changed something: visible
  as a warning and in the summary, without turning the required build red. That
  turns an upgrade into a tick beforehand instead of a surprise.

The expected build sizes per env are above in *The two envs*; if they differ
sharply on a fresh checkout, something did not carry over. (An older, sensor-only
baseline was RAM 32.0% / flash 41.4% — from before the room-server code; purely for
historical comparison.)
