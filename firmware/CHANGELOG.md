# Changelog — MeshUptime

Eigen semantische versionering (`MESHUPTIME_VERSION`), los van de MeshCore-versie
(v1.17.0). De branding toont beide: `MeshUptime vX.Y.Z (by DinX) - MeshCore v1.17.0`.
Getoond op het OLED-bootscherm, in de web-voettekst en via het `ver`-commando.

Alleen de room-server-variant (`env:meshuptime_room`, build-flag `ROOM_SERVER_VARIANT`)
tenzij anders vermeld; de sensor-variant (`env:meshuptime`) blijft de terugvalweg.

## v2.3.1 — kanaal op naam (sleutel afgeleid), zoals de app

- **Kanaal toevoegen ZONDER secret** (web-GUI, `/channel/add`, CLI `channel add`):
  laat het secret leeg en de node maakt een **hashtag-kanaal** — de sleutel wordt
  DETERMINISTISCH uit de naam afgeleid als de eerste 16 byte van `sha256(naam)`,
  EXACT zoals de MeshCore-app (`docs/companion_protocol.md`: `#test` ->
  `9cd8fcf22a47333b591d96a2b848b73f`). Zelfde naam = zelfde kanaal als de app, dus
  de node komt in precies hetzelfde kanaal. Een expliciete 32/64-hex sleutel blijft
  toegestaan (bv. de publieke `Public`-sleutel `8b3387e9c5cdea6ac9e5edbaa115cd72`).
  Bevinding uit de gevendorde code: publiek kanaal = vaste sleutel, hashtag =
  sha256(naam)[:16], privé = random — alleen de hashtag-afleiding is "op naam".
- De GUI/CLI/`/channel/add`-respons melden of de sleutel is **afgeleid** dan wel
  **eigen**, met de kanaal-hash; een afgeleide sleutel is niet geheim (wie de naam
  kent leidt hem af), een eigen secret wordt nooit teruggetoond.

## v2.3.0 — hashtag-kanalen + grote contactenlijst (naamresolutie)

- **Hashtag-/publieke kanalen**: de bot leest de ingeschakelde MeshCore
  group-channels mee en antwoordt IN het kanaal op `ping` (Pong), `test`
  (signaalrapport: SNR/RSSI/hops) en `path` (route met repeaternamen). Een kanaal
  is een gedeeld secret (32 hex = 128-bit of 64 hex = 256-bit); de kanaal-hash =
  eerste byte van `sha256(secret)` — exact het MeshCore-formaat. Beheer via de
  web-GUI (bot-tab: toevoegen/aan-uit/wissen); persistent in `/channels.cfg`. Het
  secret is schrijf-alleen (nooit teruggetoond). `searchChannelsByHash`/
  `onGroupDataRecv` overrides; antwoord geflood via `createGroupDatagram`.
- **Grote advert-/contactlijst** voor NAAMRESOLUTIE: de `NeighbourList` groeide van
  12 naar **200** ingangen (pubkey + naam + adv-type + snr/hops/laatst-gehoord),
  LRU op laatst-gehoord. ~13,6 kB RAM (de bewuste ruil). RAM-only: adverts komen
  te vaak voor per-advert-flashschrijven; na een herstart stromen de namen binnen
  minuten vanzelf weer binnen. Gebruikt voor ALLE naamresolutie: de `path`-
  repeaternamen, de DM-afzendernaam en het `/contacts.json`-lijstje (weergave
  gekapt op NB_JSON_MAX=64; de lijst zelf blijft groot voor resolutie).
- **Kanaal-afzendernaam inline**: een group-bericht draagt de afzendernaam IN het
  bericht (`"<naam>: <tekst>"`, zoals de MeshCore-app) — géén pubkey. De bot
  gebruikt die naam direct; zo tonen we ook namen van afzenders wier advert we
  nooit los hoorden. Resolutievolgorde: (a) naam uit het bericht (kanaal), (b) de
  grote advert-/contactlijst (path/DM), (c) hex-pubkey-prefix als laatste terugval.

## v2.2.2 — NTP/tijdzone instelbaar + lokale tijd + path-repeaters

- **NTP-server + tijdzone via de web-GUI** (paneel *Tijd*, `/time.cfg`, POST `/time`):
  de NTP-server is instelbaar (bv. een LAN-tijdserver) en een POSIX-TZ-string zet de
  tijdzone (standaard Europe/Brussels `CET-1CEST,M3.5.0/2,M10.5.0/3`, DST-bewust).
  Opslaan past de TZ meteen toe en vraagt een her-sync aan; de GUI toont de huidige
  lokale tijd en de laatste-sync-status.
- **Robuustere sync**: al bij WiFi-connect én nu ook periodiek (elke 6 u) her-syncen,
  zodat de driftende ESP32-klok bijblijft.
- **Menselijke tijden LOKAAL, protocol/RTC UTC**: de bot-`path` "Received at" (en
  andere weergaven) tonen lokale tijd mét zone-afkorting (bv. `00:35:11 CEST`) via
  de ingestelde TZ; de RTC en de MeshCore-protocoltijdstempels blijven UTC. Vóór de
  eerste geslaagde sync: "niet gesynct" i.p.v. een garbage-tijd (`TimeFmt.h`,
  `TIME_FLOOR`). De oude `epoch % 86400`-formattering (die per definitie UTC toonde)
  is vervangen door `localtime_r`/`strftime %Z`.
- **`path` toont de tussenliggende repeaters met NAAM**: elke pad-hash wordt via de
  buurtlijst opgelost naar een node-naam (terugval op de hex-hash), route als
  `via [A] > [B] > [C]`. Begrensd zodat het antwoord in één pakket past.

## v2.2.1 — boot-stack-fix + tweerichtings-bot

- **Boot-stack-overflow gefixt** (regressie in v2.2.0): de SNMP-velden bliezen een
  `MonitorCfg` op tot ~6 kB; `MonitorStore::load` en `WebTask::begin` hielden er één
  OP DE STAPEL, waardoor de 8 kB Arduino-loopTask overliep tijdens de SPIFFS-lees
  bij boot (stack-canary-panic vlak na "Room-server ID"). Beide buffers zijn nu
  `static`; de SNMP-BER-opbouwbuffers idem. `SET_LOOP_TASK_STACK_SIZE(16 kB)` +
  een stapel-waakhond (laagste vrije loopTask-stapel, eens/30 s) als marge.
- **Bot als TWEERICHTINGS mesh-diagnose-responder**: de bot ontvangt nu ook DM's
  (voorheen alleen-zenden — inkomende pakketten aan de bot vielen door naar
  rooms[0] en werden met de verkeerde sleutel ontsleuteld). `onRecvPacket` matcht nu
  ook `_bot_id`; de afzender-pubkey wordt uit de buurtlijst/ontvangerslijst
  opgelost (geen wachtwoord-login) en het gedeelde geheim eruit berekend. Klein,
  eigen commandoset (GEEN monitoring-console):
  - `ping` → `Pong`
  - `path` → `ack @[<naam>] | <hop-hashes csv> (<N> hops) | SNR: <x.x> dB | RSSI: <y> dBm | Received at: <HH:MM:SS>`
  - `help` → som de bot-commando's op; onbekende tekst → korte hint.
  Antwoord als schone DM vanaf de bot (met ACK op het inkomende bericht). De
  velden komen uit het inkomende pakket (pad/hops, SNR×4→dB, RSSI, RTC-tijd).
  Antwoordbuffers static (niet op de loopTask-stapel).

## v2.2.0 — async netwerk-engine + SNMP

- **Async netwerk-taak-engine**: coöperatief, niet-blokkerend, gestapt vanuit
  `loop()` — verstoort de LoRa-timing niet. Eén taak tegelijk; resultaat komt terug
  naar de oorsprong (room/DM) via de bestaande oorsprong-routing.
- **Netwerk-diagnoses** (room/DM, read-niveau): `port <host> <poort>` (non-blocking
  TCP-connect), `scan` (WiFi-scan, async), `http <url>` (statuscode + responstijd),
  `traceroute <host>` (hop-AFSTAND via TTL-oplopende esp_ping: verhoog de TTL tot de
  bestemming antwoordt; die TTL = het aantal hops. Begrensd op 30 hops, ~1,2 s per
  TTL. Tussenliggende hop-IP's zijn op dit platform niet beschikbaar).
- **SNMP als monitor-soort**: de node doet zelf een niet-blokkerende SNMP-GET
  (BER/ASN.1 over UDP:161, v2c). De gepollde waarde stroomt door de bestaande
  telemetrie/alert-pijplijn (CayenneLPP → sensor-nodes → mesh → MeshManager; alerts
  → rooms/DM). Geen nieuw verzendpad; alleen een nieuwe bron.
- **Bot: virtuele CHAT/notifier-identiteit** (`BE-HSS-DinX-Bot`): een eigen
  persistent chat-contact (ADV_TYPE_CHAT/type=1, join-URI type=1) dat SCHONE DM's
  stuurt — voor flash-meldingen én voor de per-sensor `dm`/`both`-alerts (herbedraad
  van de room-identiteit naar de bot, herhaal-tot-ACK per ontvanger). Persistente
  DM-ontvangerslijst (`/bot_recips`, 16 ingangen, geseed met de eigenaar). CLI
  `bot list|add|del|post|sendto|advert`, room/DM-commando `sendto`, en web-GUI
  (bot-tab: join/QR, ontvangerbeheer, sendto/post). Endpoints `/bot.json`,
  `/bot/recipient`, `/bot/advert`, `/bot/sendto`, `/bot/post`.
- **Ontdekte contacten**: `/contacts.json` (buurtlijst met VOLLEDIGE pubkey + naam +
  snr/hops/laatst-gehoord) voedt een kiezer in de web-GUI voor zowel de
  bot-ontvangers als de per-slot ACL-grants (kiezen uit gehoorde nodes i.p.v. alleen
  handmatig plakken). Alleen publieke sleutels.

## v2.1.0 — room-beheer, sensor-nodes, ACL, bediening

- **Web-GUI room-beheer**: rooms toevoegen/bewerken/verwijderen, delen via inline
  QR-code + join-link (client-side, geen externe assets), backup/restore van de
  volledige room-config incl. sleutels.
- **`/mon/alarm`-setter** + `/room/edit` guest-semantiek (leeg = ongewijzigd,
  wissen expliciet via `guest_clear`).
- **Virtuele sensor-nodes**: meerdere identiteiten van het sensor-type (ADV_TYPE_
  SENSOR) zodat de MeshCore-app telemetrie toont; subset-telemetrie per node
  (sensoren over meerdere nodes verdelen voorbij de één-pakket-CayenneLPP-limiet);
  dubbele koppeling per sensor (rooms-masker + sensornodes-masker).
- **Per-sleutel ACL** op elk room/sensor-node-slot: drie niveaus (read/readwrite/
  admin) met wachtwoordloze toegang op basis van de pubkey; grants persistent en
  apart van de vluchtige login-sessies.
- **Kanaalbeheer-panel** (node-centrisch), **handmatig advert** (flood/zero-hop) per
  room/sensor-node.
- **Ping-fix**: een `ping` gevraagd in een room krijgt zijn uitslag terug IN die
  room (niet meer als aparte DM) — algemeen mechanisme voor uitgestelde resultaten.
- **Room/DM-commandoset**: `dns`, `neighbors`/`nb`, `wifi`, `sys`/`health`,
  `history` (read); `checknow`, `mute`/`unmute`, `snooze`, `test` (readwrite);
  `reboot`, `ntp`/`sync` (admin). `ping` uitgebreid met verlies% en jitter.
  Mute/snooze bevriezen het alarm-kantelpunt zolang gedempt.

## v2.0.0 — multiroom room-server-basis

- MeshUptime als MULTIROOM room-server (`RoomMesh`): één toestel bedient meerdere
  room-identiteiten tegelijk, elk met een eigen sleutelpaar, naam, wachtwoord en
  toegangslijst. Room 0 = de bestaande hoofdidentiteit (behouden).
- Per-sensor alarm-config (route dm/room/both + room-set), room-CLI, en de eerste
  web/monitoring-integratie.
