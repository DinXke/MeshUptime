# MeshUptimeCompanion v2.0.0 — testprotocol (T1000-E)

**Wat is er veranderd t.o.v. v1.0.x**
Herbouwd op de **standaard MeshCore companion_radio v1.17.1** (commit `d929643`) —
exact dezelfde basis als de officiële `t1000e_companion_radio_ble` die bij jou werkt.
De **PowerSaving-v17 fork is volledig verlaten**: er zit géén RX-duty-cycling meer in
de build. De radio staat **altijd in continu-RX**. Dit is de fix voor "verzenden geeft
geen *heard*" / "af en toe RX". Al onze features (deuntjes, alerts, val, find-me,
batterij-%, menu, `!`-commando's) zijn mee overgezet, en elke toevoeging is
niet-blokkerend. Val-detectie staat **default UIT**.

---

## 0. Flashen (behoudt private/public key!)

Beide bestanden zijn **app-only** → LittleFS (identiteit/keys/contacten) blijft staan.

- **Makkelijkst — `.uf2`:** dubbelklik de reset zodat de **T1000-E-USB-schijf**
  verschijnt, sleep `…-v2.0.0-….uf2` erop. Toestel herstart zelf.
- **Of DFU — `.zip`:** via nRF Connect (Desktop/Android) of
  `adafruit-nrfutil dfu serial -pkg …-v2.0.0-….zip -p COM5 -b 115200`.

**Direct na flashen controleren dat je key behouden is:** open de MeshCore-app →
je node heet nog `🇧🇪BE-HSS-DinX` en de pubkey begint met **`2CB0C5EB`**
(serieel: `menu` → 8 = Info/pubkey). Zo niet: **niet verder testen**, meld het.

---

## 1. KERNTEST — het eigenlijke probleem (doe dit eerst)

Op dezelfde plek waar EasySkyMesh werkte:

1. **Discover:** app → Tools → *Discover nodes*. Binnen ~1 min moet de
   **repeater/node verschijnen** (niet leeg blijven).
2. **Verzenden = heard:** stuur een kort bericht naar een contact/repeater.
   Je moet een **"heard"/aflever-indicatie** terugkrijgen (bij v1.0.x bleef die weg).
3. **Ontvangen:** laat iemand/de node een DM sturen → komt **betrouwbaar** binnen,
   niet "soms wel, vaak niet".

➡️ **Werkt 1–3 stabiel? Dan is de kern opgelost.** Ga door naar 2.
➡️ **Werkt het niet?** Noteer wat wél/niet lukt en meld het — dan is het niet de
   power-saving-basis en kijken we verder (dit is dan bewijs, geen giswerk).

---

## 2. Features (pas testen nadat de kern werkt)

- **Alert-deuntjes:** stuur een DM die begint met 🔴 / 🟠 / 🟢 → hoog/midden/laag-deuntje.
- **`!`-commando's** (DM of serieel, `!` mag weg op serieel):
  - `!ping` → pong; `!status` → config-overzicht; `!play coin` → speelt af.
  - `!find` → find-me-deuntje herhaalt tot je op de **knop** drukt; `!findstop` stopt.
  - `!loc` → antwoordt `#LOC <lat>,<lon>` (buiten/bij raam voor GPS-fix).
- **Batterij-%:** in de app moet het %-cijfer kloppen met de echte lading
  (wij encoderen de mV zodat de app-formule het juiste % toont).
- **Val-detectie (optioneel, default uit):** `!fall on` → `!fall test` simuleert een
  val → pre-alarm-buzzer (20 s, knop annuleert) → alert naar je targets. Laat weer
  `!fall off` als je het niet wil. *Backup-van-de-backup, niet-gecertificeerd.*

---

## 3. Officiële-app-compat (regressiecheck)

Pairen, berichten sturen/ontvangen en telemetrie in de **officiële MeshCore-app**
moeten normaal blijven werken — onze toevoegingen zijn additief.

## 4. Terugrollen
Niet goed? Flash gewoon opnieuw de standaard `t1000e_companion_radio_ble-v1.17.1`
(.uf2/.zip) die je al had. Ook app-only → keys blijven.
