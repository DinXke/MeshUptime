# MeshUptime — English documentation

MeshUptime is monitoring firmware for a Heltec LoRa32 V3: Uptime Kuma in spirit,
but with LoRa as the way out. It runs on a MeshCore node, offers its results as
telemetry sensors another node can poll, and raises alerts over the mesh when
something falls over.

The project is Dutch-first. These pages are the English counterparts of the four
files in [`docs/`](../); the Dutch versions are the ones kept in step with the
code first, so if the two ever disagree, the Dutch page is the newer one.

| page | what is in it |
|---|---|
| **[open-issues.md](open-issues.md)** | **what does not work, and which suspicions have already been investigated and ruled out. Start here.** |
| [how-it-works.md](how-it-works.md) | roles, channel map, web interface, CLI, DM commands, access control, alerts |
| [measurements.md](measurements.md) | every number with its date: power thresholds, WiFi behaviour on battery, heap per build, the byte budget |
| [decisions.md](decisions.md) | the choices with their reasons, explicitly including the ones that were later reversed |

The raw measurement series is language-neutral:
[meting-voeding-2026-08-19.log](../meting-voeding-2026-08-19.log).

## The short version

Base is `examples/simple_sensor` from MeshCore tag `companion-v1.17.0`. The node
advertises as `ADV_TYPE_SENSOR`. Last recorded build: RAM 22,9% (75.184 of
327.680), flash 39,5% (1.321.521 of 3.342.336), admin page 66.505 bytes in
PROGMEM, roughly 221 kB heap free and a 212 kB largest block while running.

Confirmed on the device: the power sensors with their calibrated thresholds, the
WiFi sensor and its watchdog, ping monitors that survive a reboot, `/hook` for an
existing Uptime Kuma, telemetry actually polled and read by a second node, the
web interface, and the serial CLI.

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

WiFi credentials are **not** in this repository. Copy
[`firmware/wifi.ini.voorbeeld`](../../firmware/wifi.ini.voorbeeld) into the
`platformio.local.ini` of your MeshCore build copy (that file is in MeshCore's own
`.gitignore`) and fill in your own network.

One patch against MeshCore is required, in
[`firmware/patches/`](../../firmware/patches/): the `sensors` global is hardcoded
in the Heltec V3 variant, and without those three lines an application cannot
supply its own `SensorManager` without forking the variant.

    python -m platformio run -e meshuptime
    python -m platformio run -e meshuptime -t upload --upload-port COM4

`upload` writes the program partition only, not SPIFFS, so the identity in
`/identity/_main.id` and the settings file survive.
