# MeshUptimeCompanion

A custom **MeshCore companion** firmware for the Seeed **T1000-E** (nRF52840 +
LR1110). On top of a stock companion node it adds, running **standalone** (no
phone app required):

1. **Severity tunes** on incoming MeshUptime alert DMs (🔴/🟠/🟢), with green-LED
   patterns, default **Super Mario** melodies, a **named built-in tune library**,
   **per-slot volume**, mute + a configurable **quieter period**.
2. A **DM + serial-CLI command protocol** (one shared parser) with an allowlist,
   find-me, configurable tunes, presets, GPS control and persistence, **plus an
   interactive ASCII-art serial menu** on top.
3. **Button** actions: press-to-silence, short-press ack, long-press SOS+GPS.
4. **Fall / no-motion** detection on the QMA6100P accelerometer (default OFF).
5. **On-demand GPS + nRF light-sleep** for battery life, with **RXPS as an
   opt-in, default-OFF** option (continuous RX by default so no alert DM is ever
   missed).

App-compatibility is a **hard requirement**: every feature is strictly additive
and the stock MeshCore companion app keeps working unchanged (pairing, messages,
telemetry). Our **severity-marked alerts, find-me, low-battery and `!play`** sound
independently of the app's `buzzer_quiet` (which the app forces on while connected)
so the pager still alerts with a phone nearby; the generic **ordinary-message**
tune stays default-OFF and stock-like (respects `buzzer_quiet`). See
[`PROTOCOL.md`](PROTOCOL.md) §0 for the guarantee and the exact severity contract,
command set, menu and battery encoding as implemented.

### Coarse buzzer volume

The T1000-E buzzer is a **passive magnetic buzzer** with no true amplitude
control. Volume is implemented best-effort by driving the buzzer pin from the
nRF52 **hardware PWM at a variable duty cycle** (lower duty = quieter): a custom
non-blocking RTTTL player (`MuBuzzer`) replaces the stock fixed-50%-duty
`tone()` playback. Four coarse levels only — `0` silent, `1` soft, `2` normal,
`3` loud — a magnetic buzzer dims coarsely and levels 1–3 are approximate.

---

## How it overlays MeshCore

Nothing in the MeshCore checkout is modified. This folder ships:

- **New modules**: `MuAlert.cpp` (tune/LED/find engine + DM hook + serial CLI +
  low-battery warning), `MuCommand.cpp` (shared parser), `MuMenu.cpp`
  (interactive ASCII serial menu), `MuBuzzer.{h,cpp}` (custom PWM-duty RTTTL
  player with coarse volume), `MuButton.cpp`, `MuFall.cpp`, `QMA6100P.{h,cpp}`
  (accelerometer driver), `MuConfig.{h,cpp}` (additive persistence),
  `MuTunes.h` (named tune library + default RTTTL + LED patterns),
  `MuBattery.{h,cpp}` (battery curve + app-facing mV encoder), `MeshUptime.h`,
  `MuHooks.h`.
- **Two lightly-patched copies** of the stock companion sources, compiled
  *instead of* the originals. All edits are small and additive:
  - `app_main.cpp` — stock `examples/companion_radio/main.cpp` + calls to
    `mu_begin()` / `mu_loop()` and a `mu_wants_cpu()` guard on light-sleep. (The
    stock `genericBuzzer` global is dropped in favour of `MuBuzzer`.)
  - `MyMesh.cpp` — stock `examples/companion_radio/MyMesh.cpp` plus: **(1)** one
    hook line in `onMessageRecv()` calling `mu_on_direct_msg()` *after* the normal
    `queueMessage()` (never consumes the message); **(2)** RXPS made opt-in and
    default-OFF (continuous RX) via `mu_rxps_apply()`; **(3)** the standard
    battery frames (`CMD_GET_BATT_AND_STORAGE`, `CMD_GET_STATS`) fed an *encoded*
    mV so the app's own linear formula shows the correct % — structure unchanged.
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

- **Volume** (`!vol`, per-slot `!vol <slot>`) is best-effort: the passive
  magnetic buzzer has no true amplitude control. It is driven by PWM duty cycle
  (lower = quieter), which dims only **coarsely** — `0` silent, `1` soft, `2`
  normal, `3` loud, with 1–3 approximate (see PROTOCOL §2/§3). The duty→loudness
  mapping (`MuBuzzer.cpp vol_duty_pct`) is a first cut and wants on-device tuning.
- **Fall detection** ships **disabled** with placeholder thresholds; it needs
  on-device tuning (`MuFall.cpp` constants) and the QMA6100P register set is
  from the datasheet, not yet hardware-validated.
- **Battery %** uses the corrected LiPo discharge curve in the single
  `battery_percent_from_mv()` seam. The curve is for light load / rest — validate
  against a real on-device discharge log under LoRa-TX/GPS load.
- **RXPS** is opt-in and default-OFF (continuous RX). Enabling it (`!rxps
  conservative|balanced`, or menu §6) trades battery for a **risk of missed alert
  DMs** — the RX duty-cycle timings are from PowerSaving-v17 and unvalidated on
  this exact node.
- **App battery encoding** (PROTOCOL §7): the value in the app's battery frame is
  a virtual mV chosen so the app's own linear %-formula shows the correct %. If
  the app displays that number as a raw *voltage* anywhere, it will be the encoded
  value, not the true cell voltage. Our own DM/`!cfg`/serial/menu/telemetry paths
  always report the **real** voltage.
- The two copied companion sources are pinned to the MeshCore version they were
  copied from; re-copy + re-apply the small hooks when bumping MeshCore.
