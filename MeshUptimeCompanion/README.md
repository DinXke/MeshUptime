# MeshUptimeCompanion

A custom **MeshCore companion** firmware for the Seeed **T1000-E** (nRF52840 +
LR1110). On top of a stock companion node it adds, running **standalone** (no
phone app required):

1. **Severity tunes** on incoming MeshUptime alert DMs (🔴/🟠/🟢), with green-LED
   patterns, default **Super Mario** melodies, mute + quiet-hours.
2. A **DM + serial-CLI command protocol** (one shared parser) with an allowlist,
   find-me, configurable tunes, presets, GPS control and persistence.
3. **Button** actions: press-to-silence, short-press ack, long-press SOS+GPS.
4. **Fall / no-motion** detection on the QMA6100P accelerometer (default OFF).
5. **PowerSaving-v17 RXPS**, on-demand GPS and nRF light-sleep for battery life.

See [`PROTOCOL.md`](PROTOCOL.md) for the exact severity contract and the full
command set as implemented.

---

## How it overlays MeshCore

Nothing in the MeshCore checkout is modified. This folder ships:

- **New modules**: `MuAlert.cpp` (tune/LED/find engine + DM hook + serial CLI),
  `MuCommand.cpp` (shared parser), `MuButton.cpp`, `MuFall.cpp`,
  `QMA6100P.{h,cpp}` (accelerometer driver), `MuConfig.{h,cpp}` (additive
  persistence), `MuTunes.h` (default RTTTL + LED patterns), `MuBattery.{h,cpp}`
  (the battery-curve seam), `MeshUptime.h`, `MuHooks.h`.
- **Two lightly-patched copies** of the stock companion sources, compiled
  *instead of* the originals:
  - `app_main.cpp` — stock `examples/companion_radio/main.cpp` + calls to
    `mu_begin()` / `mu_loop()` and a `mu_wants_cpu()` guard on light-sleep.
  - `MyMesh.cpp` — stock `examples/companion_radio/MyMesh.cpp` + **one** hook
    line in `onMessageRecv()` calling `mu_on_direct_msg()`.
  Everything else (`MyMesh.h`, `DataStore.*`, `NodePrefs.h`, `AbstractUITask.h`)
  is used unmodified from the MeshCore checkout.
- **`platformio.local.ini.example`** — the build env, layered onto MeshCore via
  its root `extra_configs`.

The `mu_on_direct_msg()` hook fires for every decrypted incoming DM regardless
of any BLE/phone connection, which is what makes the alert tunes and `!`
commands work standalone.

---

## Building

**Requirements**: the nRF52 build needs an **ASCII path** (the xtensa/nrf
toolchain dislikes non-ASCII), hence the `C:\Users\Public\...` locations below.

1. **MeshCore on PowerSaving-v17** (v1.17.1 — has RXPS + the T1000-E power
   profile). The non-destructive way, leaving any existing checkout untouched:

   ```bash
   cd /path/to/MeshCore
   git worktree add --detach ../MeshCore-ps17 iotthinks/PowerSaving-v17
   ```

2. **Sibling layout** — put this repo next to the MeshCore checkout:

   ```
   <parent>/MeshCore-ps17
   <parent>/MU-companion/MeshUptimeCompanion
   ```

3. **Overlay the env** — copy the example into the MeshCore root:

   ```bash
   cp MU-companion/MeshUptimeCompanion/platformio.local.ini.example \
      MeshCore-ps17/platformio.local.ini
   ```

4. **Build**:

   ```bash
   cd MeshCore-ps17
   python -m platformio run -e t1000e_meshuptime_companion
   ```

   Artifacts land in
   `MeshCore-ps17/.pio/build/t1000e_meshuptime_companion/` —
   `firmware.hex` and `firmware.zip` (nrfutil DFU package). A `.uf2` is produced
   by `uf2conv` if configured; otherwise convert `firmware.hex` with
   `uf2conv.py -c -f 0xADA52840 firmware.hex -o firmware.uf2`.

The env is BLE-based (phone app over BLE) and leaves the USB serial port free
for the text CLI. `ADVERT_NAME='"@@MAC"'` names the node from its MAC on first
boot; the persisted node name is used thereafter.

---

## KEY-SAFE flashing (nRF52 UF2 — app region only)

The device is a live, carried companion. **Never full-erase it.** On the
nRF52840 the application and the LittleFS filesystem live in different flash
regions; a UF2 drag-drop writes only the application region, so the MeshCore
**identity (private/public key), prefs, contacts, channels and the
`/mu_cfg.dat` settings all survive** a reflash.

Rules:

1. **Back up the current firmware first.** Double-tap reset to enter the UF2
   bootloader (the `T1000E_OTA`/`XIAO-SENSE`-style mass-storage drive appears),
   and **copy `CURRENT.UF2` off the drive** before writing anything.
2. **Flash app-region only**: drag the new `firmware.uf2` onto the bootloader
   drive. This is an application update — it does **not** touch the filesystem.
3. **Do NOT** use `esptool erase_flash`, a full-chip merged image, or any
   "erase all / factory" option. Those wipe LittleFS and destroy the identity.
4. If you build a `.hex`/DFU zip and flash over serial DFU (`nrfutil`/`adafruit-
   nrfutil`), use the **application** DFU package only — never a full/SoftDevice
   erase.

If identity is ever lost, the node comes back with a **new** public key and must
be re-added to every contact and re-authorised on the mesh — so back up first.

---

## Status & limitations

- **Volume** (`!vol`) is best-effort only: the passive buzzer has no amplitude
  control; `0` = silent, `1..3` = full volume (see PROTOCOL §2).
- **Fall detection** ships **disabled** with placeholder thresholds; it needs
  on-device tuning (`MuFall.cpp` constants) and the QMA6100P register set is
  from the datasheet, not yet hardware-validated.
- **Battery %** uses a placeholder linear curve behind the single
  `battery_percent_from_mv()` seam; the corrected curve drops in there.
- The two copied companion sources are pinned to the MeshCore version they were
  copied from; re-copy + re-apply the small hooks when bumping MeshCore.
