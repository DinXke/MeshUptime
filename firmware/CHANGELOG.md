# Changelog — MeshUptime

Eigen semantische versionering (`MESHUPTIME_VERSION`), los van de MeshCore-versie
(v1.17.0). De branding toont beide: `MeshUptime vX.Y.Z (by DinX) - MeshCore v1.17.0`.
Getoond op het OLED-bootscherm, in de web-voettekst en via het `ver`-commando.

Alleen de room-server-variant (`env:meshuptime_room`, build-flag `ROOM_SERVER_VARIANT`)
tenzij anders vermeld; de sensor-variant (`env:meshuptime`) blijft de terugvalweg.

## v2.3.6 — reactietijd netvoeding/wifi-alert runtime-instelbaar

De netvoeding-/wifi-alert duurde ~1 minuut. Oorzaak was een bewust trage detectie
die HARDGECODEERD in de firmware stond en waarvan de marges opstapelden:

- `SAMPLE_INTERVAL_MS = 10000` (spanning meten elke 10 s),
- `SAMPLES_TO_SWITCH = 3` (3 bevestigende metingen → ~30 s om `_mains` te wisselen),
- `SETTLE_MS = 60000` (na een reset/overgang mocht `_mains` 60 s niet wisselen —
  domineerde vlak na een reboot),
- de leesronde-cadans `SENSOR_READ_INTERVAL_SECS` (compile-time `-D`), plus de losse
  `alert.debounce` (5 s). Samen ~60 s.

**FIX.** Deze vier reactietijd-parameters zijn nu RUNTIME-INSTELBAAR (web-GUI + CLI +
DM), persistent in `/monitors.cfg`, en worden LIVE gelezen in `samplePower()`/`loop()`/
de leesronde — een wijziging werkt dus meteen zonder herstart. De hi/lo-hysterese
(`mains.hi`/`mains.lo`) blijft ongewijzigd de eigenlijke ontruising.

Nieuwe instellingen (CLI/DM `sensor set <sleutel> <waarde>`; ook in de web-GUI-sectie
**Reactietijd** met per-veld "?"-help):

| sleutel | vervangt | default (was) | bereik |
|---|---|---|---|
| `power.sample` | `SAMPLE_INTERVAL_MS` | 2 s (10 s) | 1–60 |
| `power.confirm` | `SAMPLES_TO_SWITCH` | 2 (3) | 1–10 |
| `power.settle` | `SETTLE_MS` | 8 s (60 s) | 0–300 |
| `read.interval` | `SENSOR_READ_INTERVAL_SECS` | 3 s | 1–60 |
| `alert.debounce` | (bestond al, `fdeb`) | 2 s (5 s) | 0–3600 |

De `-D SENSOR_READ_INTERVAL_SECS` blijft enkel de default-seed voor `read.interval`
(`MON_READ_DEFAULT`), nu op 3. De waarden staan in `status.json` onder `"timing"`
(huidige waarde + `*_min`/`*_max` per veld) zodat de MeshManager-server ze kan tonen
en later zetten. Netto met de defaults: netvoeding/wifi-alert in ~6–10 s.

Geldt voor BEIDE envs: de leesronde-cadans is ook in de sensor-variant
(`env:meshuptime`) runtime-instelbaar. Persistentie: een `/monitors.cfg` van een
oudere versie zonder de nieuwe regels (`psample`/`pconfirm`/`psettle`/`read`) houdt
gewoon de nieuwe snelle defaults.

## v2.3.5 — spook-recovery ECHT weg (harde grendel) + debounce vereenvoudigd

Fysiek getest bleek de spook-"netvoeding terug na 0s" ná v2.3.4 nog steeds te komen,
en — nieuwe aanwijzing van de gebruiker — OOK bij een geforceerde down via `/sim`.
Dat wees de vinger van de rauwe spanning-flap naar de RECOVERY-DISPATCH zelf.

OORZAAK. De symmetrische debounce van v2.3.4 was het ENIGE dat een spurieuze "terug"
tegenhield, en dat is ontoereikend:
- `alert.debounce` mag 0 zijn ("0 = uit"). Dan kantelt `fixedRawTick` de toestand
  bij elke rauwe wisseling meteen; een settle-flap bij uittrekken gaf zo binnen een
  paar 250 ms-tikken down→up, met een geregistreerde duur < 1 s → `durText(0)` =
  "0s". De 45 s-default hielp alleen zolang niemand de debounce lager zette.
- Er was GEEN onafhankelijke voorwaarde dat er überhaupt een "weg" gemeld was: zodra
  `main_room::edge` een down-kant zag, kon de bijbehorende up-kant een "terug" vuren,
  ook als de "down" nooit de deur uit ging (bv. onderdrukt door de opstart-genade) of
  sub-seconde was. Een sim die snel toggelt reproduceerde net zo goed.
- De opstart-genade zat IN `fixedAlertDown` (gaf tijdens de eerste ~60 s "niet neer"),
  terwijl de OLED (`fixedIsDown`) al "neer" toonde. Die desync kon de kantelaar uit
  de pas met de echte toestand laten lopen.

FIX (root, niet symptoom). De grendel staat nu LOS van de debounceduur:
1. **Harde grendel op de herstelmelding** (`main_room::fixedEdge`): een vaste-kanaal-
   "terug" komt ALLEEN als (i) wij voor die onderbreking echt een "weg" gedispatcht
   hebben (`st_*_announced`) EN (ii) de gemeten onderbreking minstens de settle-drempel
   duurde en nooit onder `FIXED_MIN_RECOVER_MS` (1 s) — `fixedRecoveryOk()`. Een
   0s/sub-seconde/flap-"onderbreking" geeft dus nooit meer een herstel, ook bij `/sim`
   of met `alert.debounce == 0`.
2. **Boot-grace zonder desync**: de opstart-genade zit niet meer in `fixedAlertDown`
   (die geeft nu exact dezelfde stabiele toestand als `fixedIsDown`/OLED). `fixedEdge`
   onderdrukt tijdens de genade de dispatch én houdt de baseline stil bij — zo maakt
   het einde van de genade geen kunstmatige "weg"-flank en spreken scherm en alarm
   elkaar nooit tegen.
3. **Debounce vereenvoudigd tot een KORTE settle** (`MON_FDEB_DEFAULT` 45 → 5 s). De
   reboot-vloed wordt door de opstart-genade (~60 s) gedempt, niet door de debounce;
   die hoeft dus alleen nog de spanning-settle bij uittrekken te ontruisen (accu zakt
   dwars door de mains.hi/lo-band). In steady-state meldt de node vrijwel direct.
4. **Geen tweede, ongedebouncet herstelpad** in de room-variant: het rauwe
   `trackRecovery(isMains()/isWifiOnline())` in `loopRecovery()` staat nu onder
   `#ifndef ROOM_SERVER_VARIANT` (het voedt enkel de sim-recovery van de sensor-variant).
5. **Aliasing-hazard weg**: `fixedEdge` haalt de alerttekst pas op in de vurende tak;
   `fixedAlertText()`/`fixedRecoverAlertText()` delen één statische buffer, dus beide
   tegelijk als argument doorgeven (zoals de oude `edge()`-aanroep) kon de ene tekst de
   andere laten overschrijven.

DIAGNOSE-LOGGING (tijdelijk, blijft in v2.3.5): altijd-aan seriële logging op COM4
`[fixdiag]` (elke `fixedRawTick`-kanteling: kanaal, rauw, nieuwe toestand,
`down_start`, `last_down_ms`, drempel, boot-grace) en `[fixedge]` (elke down/herstel-
beslissing: kanaal, gemeld/onderdrukt, `announced`, `dur_ok`). MESH_DEBUG staat op dit
bord uit, vandaar directe `Serial.printf`. Zo kan de coordinator live verifiëren wat er
vuurt bij uittrekken, insteken en `/sim`.

## v2.3.4 — spook-recovery weg (symmetrisch gedebouncete storingstoestand)

Fysiek gemeld: USB uittrekken gaf terecht "storing netvoeding weg" op de OLED, maar
tegelijk een SPOOK-alert "netvoeding terug na 0s" (re-plug gaf wél de juiste duur).

OORZAAK: in v2.3.3 was de DOWN-kant gedebouncet (`fixedAlertDown` telde de
onderbrekingsklok) maar de UP/recovery-kant NIET: `fixedRawTick` zette
`_fixed_raw_down_since` op 0 zodra de rauwe toestand één keer "op" las, waarna
`main_room::edge` meteen een recovery vuurde met een bijna-0 geregistreerde duur.
Een korte afwijking (de settle door de mains.hi/lo-band bij uittrekken/insteken)
volstond zo voor een spurieuze "terug na 0s".

FIX (root): de vaste kanalen draaien nu op een SYMMETRISCH GEDEBOUNCETE
toestandsmachine (`_fixed_state_down` + `_fixed_change_since`): de rauwe meting moet
de debounce-drempel lang AANEENGESLOTEN afwijken voordat de toestand kantelt --
identiek voor down en up. Gevolgen:
1. Geen recovery meer met een duur onder de drempel: een recovery vergt dat de
   toestand eerst ECHT neer stond (>= debounce) en daarna stabiel terug is (>=
   debounce), dus "terug na ~0s" kan niet meer.
2. De recovery is nu symmetrisch gedebounced, net als de down-kant.
3. Bovenop de bestaande hysterese van `samplePower()` (mains.hi/lo-band +
   SAMPLES_TO_SWITCH + SETTLE) laat deze extra laag een flap tijdens het settelen de
   alarmtoestand niet meer kantelen.
4. De OLED (STORING-scherm) leest nu dezelfde gedebouncete toestand (`fixedIsDown`)
   als het alarmpad (`fixedAlertDown`), zodat scherm en alarm elkaar nooit
   tegenspreken. De herstelmelding toont de echte onderbrekingsduur
   (`_fixed_last_down_ms`).

## v2.3.3 — wifi/netvoeding-alerts: geen mislabel + debounce

Twee fixes tegen de vloed van "wifi simulatie"-alerts die de gebruiker meldde.

- **Mislabeling weg**: `fixedAlertText`/`fixedRecoverAlertText` zetten het
  SIMULATIE-merkteken (`TEST ` + " -- dit is een SIMULATIE, geen echte storing")
  nu ALLEEN wanneer de storing echt geforceerd is (`fixedIsSim()`: de sim-modus van
  dat vaste kanaal staat aan). Een ECHTE wifi-/netvoedingsonderbreking — de
  room-variant vuurt op de gemeten `isWifiOnline()`/`isMains()` — krijgt een normale
  tekst ("wifi weg, monitors bevroren" / "netvoeding weg …") zonder simulatie-staart.
- **Debounce + opstart-genade**: nieuwe `fixedAlertDown()` (gebruikt door
  `main_room::edge`) meldt een vast kanaal pas "neer" als de onderbreking langer dan
  de drempel aanhoudt (instelbaar `alert.debounce`, standaard 45 s; 0 = uit) EN de
  opstart-genadeperiode (60 s na boot) voorbij is. Zo alarmeert een korte blip of de
  eigen reboot/herverbinding niet meer, en vuurt er dus ook geen spookherstel
  ("wifi net terug") puur omdat de node opstartte. De debounce-drempel is persistent
  (`fdeb` in /monitors.cfg) en te zetten via CLI/web (`alert.debounce`). De
  herstelmelding toont nu de echte onderbrekingsduur (`_fixed_last_down_ms`).

## v2.3.2 — naam "Public" -> vaste publieke sleutel

- **Speciale naam "Public"**: een kanaal toevoegen met de naam `Public` (case-
  insensitief, met of zonder `#`) en ZONDER secret gebruikt nu de vaste publieke
  sleutel `8b3387e9c5cdea6ac9e5edbaa115cd72` i.p.v. `sha256("public")` — zodat de
  node op het ECHTE publieke kanaal uitkomt, net als de MeshCore-app. Andere namen
  blijven hashtag-kanalen (`sha256(naam)[:16]`); een expliciete secret wint altijd.
  De GUI/CLI/`/channel/add`-respons tonen nu drie soorten: "publiek kanaal (vaste
  sleutel)", "sleutel afgeleid uit naam" of "eigen sleutel" (nieuwe `pub`-vlag in
  `/channels.json`; `is_public` afgeleid door het secret met de publieke sleutel te
  vergelijken, niet apart bewaard).

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
