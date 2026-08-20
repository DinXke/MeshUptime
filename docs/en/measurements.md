# Measurements

Nearly every choice in this project rests on a figure measured on the real
hardware. They are collected here, with their dates, so a choice can be
recomputed instead of having to be believed.

Three kinds of claim, kept strictly apart:

- **measured** — seen on the device, with the figure quoted
- **built** — the code is there and compiles, but the behaviour is not confirmed
  on the device
- **reasoned** — derived from the source, not measured

## Power calibration — 19 August 2026

Measured on the node itself, not read off a datasheet. The raw series is in
[meting-voeding-2026-08-19.log](../meting-voeding-2026-08-19.log): one voltage per
line, polled over HTTP roughly every 20 s.

| state | voltage | what the series says |
|---|---|---|
| battery, under load | **4,034 V** | 7 of the 10 battery readings are exactly 4,034; the outliers (4,052 twice, 4,087 once) all fall in the first two minutes |
| USB, charger idle | **4,139 V** | the baseline at the start of the series; the log header calls it stable |
| USB, actively charging | **4,209–4,226 V** | six readings after plugging back in: 4,226 once, then 4,209 five times |

The USB side is therefore a **range**, not a point. Basing a threshold on a single
USB reading picks the wrong one.

### Why the threshold works

The 4,034 V is the battery **under load**. The node draws current, so the terminal
voltage sags; on USB the charger makes up that load and the rail stays high. That
is a physical difference and not measurement luck — and it grows *larger* as the
battery ages and its internal resistance rises.

Hence the firmware defaults: **below 4,09 V battery, above 4,12 V mains**, with
60 s of settling time after a transition (the ~50 mV transient: 4,052 → 4,087 →
4,034 in the first minute) and three consecutive readings on the same side.

A naive threshold at 4,2 V — which a datasheet would suggest — would have reported
"no mains" with the plug in.

### What this measurement said about three assumptions

This is the reason to measure rather than calculate:

1. "USB reads above 4,2 V, so put the threshold at 4,2" — **wrong**: USB with an
   idle charger reads 4,139.
2. "The 53 mV spread is noise, so use variance instead of level" — **wrong**: that
   was a settling transient in the first minute; after it the level is rock solid
   and discriminates perfectly well.
3. "It is off the network, so WiFi drops with the power" — **too quick**, based on
   one missing reading. It stayed reachable for minutes first.

## WiFi behaviour on battery — 19 August 2026

The most important finding of that session is not about voltage. From the same
series:

1. Plug out → a brief hiccup, then still reachable for about four minutes with
   **31% loss**: 5 of the first 16 polls did not arrive.
2. Then gone entirely: **21 consecutive failed polls**, a good seven minutes.
3. The serial console gave the cause: `WiFi disconnected (reason 202)` —
   `WIFI_REASON_AUTH_FAIL`. So not weak signal; rssi was around -50 dBm (see the
   note below).
4. **Plugging USB back in did not bring it back.** Only a hard reset — caused by
   opening COM4, which toggles DTR — restored the connection.

Three consequences for the design, all three measured rather than assumed:

- **Alerts must go over LoRa.** Precisely during a power cut the network path is
  the least reliable. A webhook from an existing Uptime Kuma cannot reach this node
  at that moment by definition, so the node's own checks are the core rather than
  an extra.
- **There must be a WiFi watchdog.** Without a forced reconnection and an own
  access point as a last resort, a node stays unreachable after an outage while
  everything else is already working again.
- **The WiFi sensor measures something other than assumed.** Not "my own power is
  gone" — the node stayed alive on battery — but "my router lost power". In a real
  power cut both happen, so WiFi remains the fast signal and voltage the
  confirmation.

**Note on the rssi.** The source says -52 dBm
(`firmware/examples/meshuptime/WifiTask.h`), the older documentation said -48.
Which of the two belongs to the moment of disconnection can no longer be
established; the raw series carries no rssi. It does not affect the conclusion:
both are ample signal, and that is the whole point about reason 202.

## Heap and footprint per build

This project has been governed from beginning to end by the **largest free
block**, not by total free memory: that is the figure at which lwip previously
produced half answers in this project. All figures below come from the build
output and serial console of the round in question. RAM is out of 327.680 bytes,
flash out of 3.342.336.

| round (2026) | RAM | flash | page |
|---|---|---|---|
| old companion, WiFi + BLE together | — | — | — |
| first flash: `simple_sensor` + WiFi | 17,3% (56.624) | 35,2% (1.174.893) | — |
| SensorManager + web server | 18,2% (59.664) | 36,8% (1.229.989) | — |
| ping monitors + DM commands | 19,4% (63.472) | 37,2% (1.243.797) | — |
| channel map + `/hook` | 20,0% (65.376) | 37,6% (1.256.701) | 11.433 |
| access control | 22,1% (72.344) | 38,1% (1.272.989) | 22.415 |
| node administration (last recorded build) | 22,9% (75.184) | 39,5% (1.321.521) | 66.505 |

The heap, from the serial console:

| moment | free | largest block |
|---|---|---|
| old companion, before the clear-out | 24.844 | 11.764 |
| after `simple_sensor` + own SensorManager + web server | 235.156 | 225.268 |
| after serving web requests (fragmentation) | — | 163.828 |
| in service, uptime 25 min | ~221 kB | ~212 kB |

So the largest block went up by a factor of 19. Where that came from:

- **BLE out.** Upstream keeps the companion in three separate envs (usb, ble,
  wifi); the old custom build enabled WiFi and BLE *together*. The BLE stack takes
  tens of kB of heap at initialisation. MeshUptime is administered over web, mesh
  and CLI and does not need BLE.
- **Three static costs that belong to a chat client and not to a monitor**:
  `MAX_CONTACTS` from 350 to 32, `MAX_GROUP_CHANNELS` from 40 to 4,
  `OFFLINE_QUEUE_SIZE` from 256 to 16.
- The chat client, the message ring, the MQTT publisher and raw packet forwarding
  are gone.

What is **not** possible: adding RAM. The board is an ESP32-S3 without PSRAM.
There is nothing to switch on, only less to use.

## The byte budget of a telemetry packet

`SensorMesh` builds its CayenneLPP as `MAX_PACKET_PAYLOAD - 4` = **180 bytes**
(those 4 are the timestamp that precedes the data). Every field costs a 2-byte
header plus its data. This is **reasoned** from the source, not verified with a
packet dump:

| field | type | bytes |
|---|---|---|
| channel 1, battery voltage | `LPP_VOLTAGE` | 2 + 2 = 4 |
| channel 1, GPS when active | `LPP_GPS` | 2 + 9 = 11 |
| channel 2, mains | `LPP_SWITCH` | 2 + 1 = 3 |
| channel 3, battery | `LPP_SWITCH` | 2 + 1 = 3 |
| channel 4, WiFi | `LPP_SWITCH` | 2 + 1 = 3 |
| | **fixed total** | **24** |

Per monitor, worst case (up, so with a ping time):

| field | type | bytes |
|---|---|---|
| state | `LPP_SWITCH` | 2 + 1 = 3 |
| round trip | `LPP_GENERIC_SENSOR` | 2 + 4 = 6 |
| | **per monitor** | **9** |

180 − 24 = 156 bytes left, so 17 monitors would fit. Eight are allowed because
there are eight fixed channels (5 through 12): 72 bytes, leaving 84 for
environment sensors added later. `querySensors()` recomputes this every round and
truncates — an assumption that lives only in a comment is not an assumption you
can build on.

The raw voltage is **not** sent again on channel 2: channel 1 already has it, and
sending it twice costs bytes without adding anything.

## The size of a message

**Reasoned** from upstream, and verified with a model.

`MAX_TEXT_LEN` is 160 (`helpers/BaseChatMesh.h`, 10 × `CIPHER_BLOCK_SIZE`). The
limit the comment there points at is 184 − 4 − 2 − 1 = 177, so 160 is a chosen
round margin and not a hard protocol ceiling. But `composeMsgPacket` performs two
checks:

    if (text_len > MAX_TEXT_LEN) return NULL;
    if (attempt > 3 && text_len > MAX_TEXT_LEN-2) return NULL;

**A message of 159 or 160 bytes goes out the first time, but cannot be re-sent
after the fourth attempt.** Hence the 158-byte chunk limit.

The chunker was run through a Python model over 20.000 cases. Before the fix: 132
messages of up to 184 bytes, i.e. above `MAX_TEXT_LEN` — the closing line was
appended to the last chunk without space having been reserved for it. After the
fix: 0 violations, longest message exactly 158. Twelve sensors with name and state
is precisely 158 bytes; with the limit at 160 that would have been a message that
could not be re-sent after the fourth attempt.

## What has been confirmed on the device

**Measured**, serial console and web interface, 19 and 20 August 2026:

- The identity survives an `upload`: the same key prefix before and after. Not
  luck but how `upload` works — it writes the program partition, not SPIFFS.
- The power state machine does what it should on the real hardware:
  `mains.state=1` on a node hanging off USB, with the calibrated thresholds.
- The web server answers 401 without credentials and 200 with.
- `/hook` validates its input: a name containing a semicolon is refused with 400;
  a valid report gets 200 with the channel number in it.
- Ping monitors on channels 5 and 6 survive a reboot, including the
  `ch_ever_used` mask, so a number is not re-issued after a restart. Right after
  boot they read `pauze` instead of `op`: that is the intended 30 s settling time
  after WiFi comes up, and after 45 s the readings were there.
- **Telemetry end to end**: a second node polls telemetry and sees six channels.
  That also confirms in practice the fix for what had been found statically —
  that `handleRequest` does not call `onSensorDataRead()`, so the sensors must sit
  in `querySensors()`.
- Access control: lock open (the default), one ACL entry with read permission plus
  alerts. In the trial SNR separated two nodes with the same name exactly as
  intended: 0 hops at -11,5 against 2 hops at -19,8.
- Node administration over `/cli`: `ver` gives `v1.17.0 (Build: 9 Aug 2026)`,
  `advert` gives `OK - Advert sent`, and a `set radio` without confirmation gets
  409.
- A full page in service: mains 4,157 V, WiFi online at -48 dBm, 2 of 4 monitors
  up, uptime 25 min, 0 resets, ~221 kB free and a ~212 kB largest block.

## A fault that was not a fault

Reviewing the page in a browser produced a stream of console errors: *"Request
cannot be constructed from a URL that includes credentials"*. That was **not a
firmware fault** but a consequence of how the page was opened: with the
credentials in the URL, and `fetch()` refuses to resolve a relative URL against
such an address. Without them in the URL the page populates normally. Recorded
because it looked exactly like a real fault.
