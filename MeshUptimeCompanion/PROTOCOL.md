# MeshUptimeCompanion — protocol

Firmware for the Seeed **T1000-E** (nRF52840 + LR1110), built as a MeshCore
companion node. It works **standalone**: the mesh stack receives DMs and the
buzzer/CLI logic runs whether or not a phone app is connected over BLE.

Two things reach this device: **alert DMs** carrying a severity marker, and
**commands** (over an incoming DM or the serial console). Both are documented
below exactly as implemented.

---

## 0. App-compatibility guarantee (HARD requirement)

Every MeshUptimeCompanion feature is **strictly additive**. The device stays a
fully-compatible stock MeshCore companion: pairing, message sync and telemetry in
the official app/EasySkyMesh keep working unchanged. Concretely, and verified:

- **(a) Messages are never consumed.** The only hook in `MyMesh::onMessageRecv()`
  runs *after* the normal `queueMessage(...)` and only reads the text. An alert DM
  (emoji-prefixed) or a `!`-command triggers our action **and** is still delivered
  to the app as an ordinary message. The hook never returns early, never edits the
  packet/text, never deletes it.
- **(b) No standard protocol frame is restructured.** The APP_START handshake,
  contact/message frames, `CMD_GET_STATS`, etc. are untouched. The **only** change
  to an app-facing frame is the *value* in the battery field of
  `CMD_GET_BATT_AND_STORAGE` and `CMD_GET_STATS` (still a plain `uint16` mV) — see
  §7. Our battery **%** appears only in our own DM/`!cfg`/serial/menu output.
- **(c) Buzzer + button coexist; alerts are INDEPENDENT of `buzzer_quiet`.** We
  never write `NodePrefs.buzzer_quiet` (the stock code/app own it). Crucially, our
  alert audio is **intentionally decoupled** from it: the official app forces
  `buzzer_quiet = 1` whenever it is *connected* to the T1000-E, so honouring it
  would silence MeshUptime alerts exactly when a phone is nearby — defeating the
  pager. The intentional pager alerts — **severity-marked** DMs (🔴/🟠/🟢),
  **find-me**, **low-battery**, and **`!play`** previews — are therefore gated
  **only by our own `!mute` + quiet-period**, and sound whether or not the app is
  connected. Opt-in `!mute followapp on` (default **off**) makes even those follow
  `buzzer_quiet`. **Exception:** the generic **message** tune for *ordinary,
  non-severity* DMs / channel messages is **default OFF** and, when enabled, still
  respects `buzzer_quiet` (stock-like) — so ordinary chatter stays silent while the
  app is connected, exactly as the app intends. The button is otherwise unused by
  the stock T1000-E companion (no display build).
- **(d) Identity + FS layout unchanged.** We persist only `/mu_cfg.dat` on the same
  InternalFS. Identity keys, `NodePrefs`, contacts and channels are never rewritten
  by our code. (The menu's radio-param change calls the stock `savePrefs()` — the
  same prefs file the stock `CMD_SET_RADIO_PARAMS` writes — never the identity.)

**RXPS is opt-in and default-OFF** (continuous RX) so a carried pager never sleeps
through an alert DM — see §8.

---

## 1. Severity-emoji alert contract

The MeshUptime bot prefixes every alert DM with a severity emoji at the very
start of the message text. The firmware matches on the raw UTF-8 bytes:

| Severity          | Emoji | UTF-8 bytes    | Tune slot | Default melody (RTTTL)     |
|-------------------|-------|----------------|-----------|----------------------------|
| High              | 🔴    | `F0 9F 94 B4`  | `H`       | Mario "death" jingle       |
| Medium            | 🟠    | `F0 9F 9F A0`  | `M`       | Mario "power-down" warning |
| Low / recovery    | 🟢    | `F0 9F 9F A2`  | `L`       | Mario "1-up" (good news)   |

On an incoming DM whose text **starts with** one of those byte sequences, the
firmware plays the configured per-severity RTTTL and runs the matching green-LED
blink pattern, subject to mute and quiet-hours (see §3).

- A DM starting with `!` is treated as a **command** (§2), never an alert.
- Any other (unmarked, non-command) DM optionally plays a subtle **message**
  tune (slot `msg`, default Mario "coin", `!msgtune on|off`, **default OFF** and
  respects `buzzer_quiet` — see §3/§0c).
- `find` slot (Mario main theme) and `msg` slot are configurable too.

The emoji check is on the DM **text**; it is independent of the sender. Command
handling *is* restricted to the allowlist (§2).

---

## 2. Command protocol (one shared parser)

The same parser serves two transports:

- **Incoming DM** — the message text must start with `!`. Accepted **only from
  allowlisted pubkeys**. Allowlist-management subcommands additionally require
  the sender to be an **admin** entry.
- **Serial console** — plain text lines over USB serial (115200). Always
  allowed and always treated as admin. The leading `!` is optional here. On
  connect (or first byte on terminals that never assert DTR) a short banner +
  hint is printed, typed characters are **echoed** (with backspace/DEL editing),
  and **`\r`, `\n` and `\r\n` are all accepted** as the line terminator, so it
  works on any terminal (no more black screen on LF-only clients). Reading stays
  non-blocking — only the bytes already available are drained each loop.

Replies go back the same way they arrived: a DM reply to the sender, or a
printed line on the serial console.

### Commands

| Command | Action |
|---|---|
| `!ping` | Replies `pong`. |
| `!find` | Loop the find melody + LED beacon until a button press, `!findstop`, or a 5-minute timeout. If started by DM, a short `gevonden` DM is sent back when a button press stops it. |
| `!findstop` | Stop the find loop. |
| `!mute on\|off` | Mute/unmute alert audio. LED alerts still show while muted. |
| `!mute followapp on\|off` | Opt-in (default **off**): when on, our alert audio also follows the app's `buzzer_quiet`. Default off = our marked alerts sound even while the app is connected. |
| `!vol 0..3` | **Global default** coarse volume. Driven by PWM duty (`0` silent, `1` soft, `2` normal, `3` loud). A passive magnetic buzzer dims only coarsely; 1–3 are approximate (see §3). |
| `!vol <H\|M\|L\|find\|msg> 0..3` | **Per-slot** volume override. `255` (reported) means "follow the global default". Report with no value. |
| `!tune <H\|M\|L\|find\|msg> <RTTTL>` | Store a new RTTTL for a slot (persisted) and play it as a preview. |
| `!tune <H\|M\|L\|find\|msg> preset <name\|nr>` | Assign a **built-in library tune** (§9) to the slot instead of pasting RTTTL. Persisted + previewed. |
| `!tune <H\|M\|L\|find\|msg>` | Report the slot's current RTTTL. |
| `!tunes` | List the built-in tune library (numbered). |
| `!play <name\|nr>` | Play a library tune now as a preview (buzzer), at the global volume. |
| `!rxps off\|conservative\|balanced` | RX power-saving level. **Default `off` = continuous RX.** `conservative`/`balanced` reuse the PowerSaving-v17 duty-cycle levels and trade battery for a **risk of missed alert DMs**. Persisted (§8). |
| `!quiet <startH>-<endH> [mute\|0..3]` | Quieter-period window in whole hours, UTC (e.g. `!quiet 22-7`). Trailing arg: `mute` (default, full silence) or a volume cap `0..3` (reduced-volume "quieter period"). Audio is capped/suppressed inside the window; LED still shows. Applied only when the RTC looks set. |
| `!quiet off` | Disable the quieter period. |
| `!gps on\|off\|ondemand` | GPS power policy. `on` = continuous, `off` = never, `ondemand` = normally off, woken by `!loc`. |
| `!loc` | Reply a machine-readable location report starting with the token `#LOC <lat>,<lon>` (decimal degrees, 5 decimals, e.g. `#LOC 51.23456,5.45678`) so the MeshUptime node can parse & plot it. If there is no fix and GPS is not `on`, it wakes the GPS and asks you to retry shortly. |
| `!cfg` | Report the full configuration (mute, vol, msgtune, battery mV + %, quiet, gps, fall, no-motion, allowlist count, target/sos set, last known location) as a few reply lines. |
| `!allow list` | List allowlist entries (index, 8-byte prefix, admin flag). |
| `!allow add <64hex>` | Add a pubkey (non-admin). Serial always; DM only from an admin. |
| `!allow del <hexprefix>` | Remove every entry matching the hex prefix. Serial always; DM only from an admin. |
| `!preset <1\|2\|3> <text>` | Store a preset message (persisted). |
| `!preset <1\|2\|3>` | Report a preset. |
| `!target <64hex>` | Set the recipient for button short-press (preset #1). |
| `!sos <64hex>` | Set the recipient for button long-press SOS and fall alerts. |
| `!fall on\|off` | Arm/disarm fall + no-motion detection. |
| `!fall nomotion <minutes>` | Dead-man no-motion timeout in minutes (`0` = off). |
| `!msgtune on\|off` | Enable/disable the subtle tune on plain (non-severity) DMs. **Default OFF.** When on it still respects `buzzer_quiet` (silent while the app is connected), unlike our severity alerts. |
| `!help` | List commands. |

### Allowlist seed (built-in defaults)

| Role | Pubkey | Admin |
|---|---|---|
| Owner | `2CB0C5EB473757805EAB00F9DD0594C229D6E50B2ACEC6B57403B6259C9E126F` | yes |
| MeshUptime bot | `E0841BF76B70FC407EF556531B610BB54584568A2560A4318B074181AE177146` | no |

Presets default to `#1 = "OK"`, `#2 = "SOS - hulp nodig"`, `#3 = "Onderweg"`.

---

## 3. Mute / quieter-period / volume interaction

The **effective volume** of a slot is computed as:

1. Start from the slot's own volume, or the global default if the slot is set to
   `255` (`mu_slot_base_vol()`).
2. If `!mute on` → **0** (silent). Otherwise the app's `buzzer_quiet` (forced on
   while the app is connected) silences a tune **only** when it is the generic
   **message** tune, or when `!mute followapp on` (default off). Severity alerts,
   find-me, low-battery and `!play` ignore `buzzer_quiet` by default.
3. If inside the quieter-period window, clamp to the window's cap
   (`mute` = 0, or the configured `0..3`).

A volume of `0` means no audio. On the passive magnetic buzzer, volume is applied
as a PWM **duty cycle** (`MuBuzzer`): `1` ≈ 4 %, `2` ≈ 15 %, `3` ≈ 50 % duty —
coarse, and the 1–3 loudness steps are approximate. Independently of audio:

- **Alert severities (H/M/L)** still run their **LED** pattern when audio is
  suppressed — a muted device still signals visually.
- The **message** tick (`msg`) follows the audio gate fully **and additionally
  respects `buzzer_quiet`** (stock-like). It is **default OFF** (`!msgtune on` to
  enable); a muted or app-connected device stays fully dark for ordinary chatter.
- **Find** ignores mute (it is an explicit request) but honours the quieter-period
  cap for its audio; the LED beacon always runs.
- The **fall pre-alarm** uses the `H` tune and therefore follows the same mute and
  quieter-period rules — note this if you rely on the dead-man alarm at night.
- The **low-battery warning** (§7) plays the built-in `warning` tune at the `H`
  slot's volume and is suppressed by `!mute`.

---

## 4. Button semantics

The T1000-E has a single user button (P0.6, active-high), read through the
board's `buttonStateChanged()` edge helper.

| Gesture | Action |
|---|---|
| Any press **while a tune/find is playing** | **Stops it immediately** (highest priority). If it was a DM-initiated `find`, a `gevonden` DM is sent back. Nothing else fires on release. |
| Any press **during a fall pre-alarm** | **Cancels** the pending fall alert. Nothing else fires. |
| **Short press** (idle) | Send preset **#1** ("ok/ack") to the `target` recipient. |
| **Long press** (≥ 2 s, idle) | **SOS**: send a `#LOC <lat>,<lon> (SOS) <preset #2>` DM to the `sos` recipient — the `#LOC` token lets the MeshUptime node map the SOS. With no fix it wakes GPS and still sends `(SOS) <preset #2> (geen GPS-fix)` so the alert is never lost. |

Preset cycling on short press is intentionally not implemented (documented
deviation — kept simple; use `!preset`/`!target` to configure).

---

## 5. Fall / no-motion detection (default OFF)

Uses the on-board **QMA6100P** accelerometer (I2C, address probed 0x12/0x13,
behind the `PIN_3V3_ACC_EN` rail). Two triggers:

- **Fall**: a free-fall signature (magnitude < 0.5 g sustained ≥ 70 ms) followed
  within ~1.2 s by an impact spike (> 2.5 g).
- **No-motion / dead-man**: no significant motion (|magnitude − 1 g| > 0.20 g)
  for `fall nomotion <minutes>`.

On a trigger the firmware enters a **30 s pre-alarm** (buzzer `H` + LED). A
button press during that window cancels it. If not cancelled, it sends a
machine-readable alert DM that starts with the `#LOC <lat>,<lon>` token followed
by `(val) VAL gedetecteerd` or `(geen beweging) GEEN BEWEGING`, so the MeshUptime
node can map it, to the `sos` recipient (falling back to `target`). With no GPS
fix it wakes GPS and still sends the human alert (without the token) so it is
never lost.

The detection thresholds are **conservative placeholders and need on-device
tuning**; the whole module ships **disabled** (`!fall on` to arm).

This feature is a **backup of the backup**: it is **not certified**, best-effort,
and **not a replacement for any internal/professional alarm system**. The reliable
core is the manual button (short = ack, long = SOS+GPS, §4); auto fall-detection
is an extra, unvalidated layer.

---

## 6. Persistence & key safety

All settings above are stored **additively** in a single LittleFS file
`/mu_cfg.dat` on the same InternalFS the stock companion uses. The MeshCore
identity (private/public key), NodePrefs, contacts and channels are never
touched, so an app-region reflash (nRF52 UF2) leaves identity **and** this
config intact. See `README.md` for the key-safe flashing procedure.

v2 of `/mu_cfg.dat` appends per-slot volumes, the quieter-period level and the
RXPS level at the end of the record; an older v1 file is migrated forward on load
(new fields back-filled with defaults, then rewritten) — it is never discarded, so
the allowlist/target/tunes survive the upgrade.

---

## 7. Battery percentage + app encoding + low-battery warning

The raw millivolt reading from the board (`getBattMilliVolts()`) is used
unchanged. The **true %** comes from `battery_percent_from_mv()` (`MuBattery.cpp`),
now the corrected piecewise-linear **LiPo discharge curve** (from analysis-agent
a17c9acb) instead of the old naive linear map that "stuck at 50–60 % then cliffed".

**Our own outputs** (`!cfg`, serial, menu §8, mesh **telemetry**/LPP voltage) always
report the **real voltage and the true %**.

**App encoding (compat trade-off).** The official app derives % from the raw mV it
receives with a fixed linear formula `pct = (mv - 3000) * 100 / 1200`, which is
wrong for a real LiPo. So the value we place in the standard battery field of
`CMD_GET_BATT_AND_STORAGE` and `CMD_GET_STATS` is a **virtual** mV:

```
app_mv = 3000 + true_percent * 12      (clamped to [3000, 4200])
```

Run through the app's own formula this recovers our true %. The frame **structure**
is unchanged (still a `uint16` mV), so full protocol compatibility is preserved.
**Trade-off:** if the app shows this number as a raw *voltage* anywhere, it will be
the encoded value, not the true cell voltage — but the **percentage is correct**.
`battery_app_mv()` (`MuBattery.cpp`) does the encoding; only the two app frames use
it. Telemetry deliberately does **not**, so mesh-side monitoring keeps true volts.

**Low-battery warning.** The env defines `AUTO_SHUTDOWN_MILLIVOLTS=3400` (matches
the heltec_v3 profile). Independently, `MuAlert.cpp` samples the battery every 30 s
and, from `AUTO_SHUTDOWN_MILLIVOLTS + 150` mV (with hysteresis), sounds the built-in
`warning` tune, prints a serial notice, and DMs the `target` (if set) roughly every
5 minutes with the real mV + true %, until the cell recovers.

---

## 8. RXPS — opt-in, default OFF

RX power-saving duty-cycles the receiver to save battery, at the cost of possibly
sleeping through an incoming alert DM. Because this device is a **carried pager**,
RXPS is **default OFF (continuous RX)**. It can be enabled with `!rxps
conservative|balanced` (DM/serial) or menu §6, and turned back to `off` at any time.
Levels reuse the PowerSaving-v17 mechanism (`CONSERVATIVE_LEVEL=1`,
`BALANCED_LEVEL=5`, preamble profile); the level is persisted in `/mu_cfg.dat` and
re-applied on boot and whenever the radio SF/BW changes. **Reliability-vs-battery:
`off` = never miss an alert; `conservative`/`balanced` = longer runtime, small risk
of missed DMs.**

---

## 9. Built-in tune library

`MuTunes.h` ships a named RTTTL library (`!tunes` to list). Names are matched
case-insensitively; commands also accept the 1-based number.

| # | Name | Feel |
|---|---|---|
| 1 | `mario-main` | Mario main theme (find beacon; loops) |
| 2 | `mario-die`  | Mario death jingle (default **H**) |
| 3 | `mario-1up`  | Mario 1-up / good news (default **L**) |
| 4 | `mario-warn` | Mario power-down motif (default **M**) |
| 5 | `coin`       | Mario coin tick (default **msg**) |
| 6 | `powerup`    | Mario power-up run |
| 7 | `warning`    | Urgent triple-beep (low-battery) |
| 8 | `chime`      | Neutral 3-note chime |
| 9 | `alert`      | Neutral repeated alert |
| 10 | `beep`      | Single short beep |

Defaults per severity slot stay Mario-themed and are drawn from this library
(`H`=mario-die, `M`=mario-warn, `L`=mario-1up, `find`=mario-main, `msg`=coin).
Use `!play <name|nr>` to preview and `!tune <slot> preset <name|nr>` to assign.

---

## 10. Interactive ASCII serial menu

On the USB serial console, a bare **Enter** or typing **`menu`** opens an
ASCII-art menu (`MuMenu.cpp`). It is a line-based, non-blocking UX layer over the
same `!`/CLI commands (so DM + scripting behaviour is identical); the mesh loop
keeps running between keystrokes. Navigation: a **number** picks an item, **`b`** =
back, **`q`** = quit.

Categories:

1. **Geluiden & deuntjes** — list/play library, assign preset to slot, global &
   per-slot volume, mute, quieter-period.
2. **Alerts & ernst-mapping** — show slot→tune, assign preset per slot, msg-tune.
3. **Knop & presets** — presets 1/2/3, target, sos.
4. **Find-me** — start/stop.
5. **Val-detectie** — arm/disarm, dead-man minutes (labelled *not certified*).
6. **Radio & Power** — show radio params; GPS mode; **RXPS** (with a missed-alert
   warning); **change radio params** and **restore the mesh preset**
   (869.618 / BW 62.5 / SF 8 / CR 8) — both behind an explicit **warning +
   `JA` confirmation**, because wrong freq/BW/SF/CR/TX drops the node off the mesh.
7. **Allowlist & beveiliging** — list/add/del pubkeys.
8. **Info / status / batterij / pubkey** — full cfg, real battery mV + %, pubkey,
   location.
9. **Opslaan & herstart** — force-save, reboot (confirmed).

The individual `!`/CLI commands keep working independently for DM use and
scripting; the menu is purely an extra serial UX on top.
