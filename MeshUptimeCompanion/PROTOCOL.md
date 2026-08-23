# MeshUptimeCompanion — protocol

Firmware for the Seeed **T1000-E** (nRF52840 + LR1110), built as a MeshCore
companion node. It works **standalone**: the mesh stack receives DMs and the
buzzer/CLI logic runs whether or not a phone app is connected over BLE.

Two things reach this device: **alert DMs** carrying a severity marker, and
**commands** (over an incoming DM or the serial console). Both are documented
below exactly as implemented.

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
  tune (slot `msg`, default Mario "coin", `!msgtune on|off`).
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
  allowed and always treated as admin. The leading `!` is optional here.

Replies go back the same way they arrived: a DM reply to the sender, or a
printed line on the serial console.

### Commands

| Command | Action |
|---|---|
| `!ping` | Replies `pong`. |
| `!find` | Loop the find melody + LED beacon until a button press, `!findstop`, or a 5-minute timeout. If started by DM, a short `gevonden` DM is sent back when a button press stops it. |
| `!findstop` | Stop the find loop. |
| `!mute on\|off` | Mute/unmute alert audio. LED alerts still show while muted. |
| `!vol 0..3` | Coarse volume. **Limitation:** the T1000-E buzzer is a fixed-amplitude passive buzzer driven by a square wave — there is no true volume control. `0` = silent (treated as muted); `1..3` all play at full amplitude (stored, best-effort). |
| `!tune <H\|M\|L\|find\|msg> <RTTTL>` | Store a new RTTTL for a slot (persisted) and play it as a preview. |
| `!tune <H\|M\|L\|find\|msg>` | Report the slot's current RTTTL. |
| `!quiet <startH>-<endH>` | Quiet-hours window in whole hours, UTC (e.g. `!quiet 22-7`). Audio is suppressed inside the window; LED still shows. Applied only when the RTC looks set. |
| `!quiet off` | Disable quiet-hours. |
| `!gps on\|off\|ondemand` | GPS power policy. `on` = continuous, `off` = never, `ondemand` = normally off, woken by `!loc`. |
| `!loc` | Reply the current GPS `lat,lon`. If there is no fix and GPS is not `on`, it wakes the GPS and asks you to retry shortly. |
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
| `!msgtune on\|off` | Enable/disable the subtle tune on plain DMs. |
| `!help` | List commands. |

### Allowlist seed (built-in defaults)

| Role | Pubkey | Admin |
|---|---|---|
| Owner | `2CB0C5EB473757805EAB00F9DD0594C229D6E50B2ACEC6B57403B6259C9E126F` | yes |
| MeshUptime bot | `E0841BF76B70FC407EF556531B610BB54584568A2560A4318B074181AE177146` | no |

Presets default to `#1 = "OK"`, `#2 = "SOS - hulp nodig"`, `#3 = "Onderweg"`.

---

## 3. Mute / quiet-hours / volume interaction

Audio for a tune plays only when **all** hold: not muted, `vol > 0`, and not
inside quiet-hours. Independently:

- **Alert severities (H/M/L)** still run their **LED** pattern when audio is
  suppressed — a muted device still signals visually.
- The **message** tick (`msg`) follows the audio gate fully (a muted device is
  fully dark for ordinary chatter).
- **Find** ignores mute (it is an explicit request) but honours quiet-hours for
  its audio; the LED beacon always runs.
- The **fall pre-alarm** uses the `H` tune and therefore follows the same mute
  and quiet-hours rules — note this if you rely on the dead-man alarm at night.

---

## 4. Button semantics

The T1000-E has a single user button (P0.6, active-high), read through the
board's `buttonStateChanged()` edge helper.

| Gesture | Action |
|---|---|
| Any press **while a tune/find is playing** | **Stops it immediately** (highest priority). If it was a DM-initiated `find`, a `gevonden` DM is sent back. Nothing else fires on release. |
| Any press **during a fall pre-alarm** | **Cancels** the pending fall alert. Nothing else fires. |
| **Short press** (idle) | Send preset **#1** ("ok/ack") to the `target` recipient. |
| **Long press** (≥ 2 s, idle) | **SOS**: send preset **#2** + current GPS location to the `sos` recipient. |

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
button press during that window cancels it. If not cancelled, it sends an alert
DM (`VAL gedetecteerd` or `GEEN BEWEGING`) **plus GPS location** to the `sos`
recipient (falling back to `target`).

The detection thresholds are **conservative placeholders and need on-device
tuning**; the whole module ships **disabled** (`!fall on` to arm).

---

## 6. Persistence & key safety

All settings above are stored **additively** in a single LittleFS file
`/mu_cfg.dat` on the same InternalFS the stock companion uses. The MeshCore
identity (private/public key), NodePrefs, contacts and channels are never
touched, so an app-region reflash (nRF52 UF2) leaves identity **and** this
config intact. See `README.md` for the key-safe flashing procedure.

The battery percentage shown by `!cfg` comes from a single seam
`battery_percent_from_mv()` (`MuBattery.cpp`) — a deliberately dumb linear
placeholder awaiting the corrected discharge curve. The raw millivolt reading
from the board is used unchanged.
