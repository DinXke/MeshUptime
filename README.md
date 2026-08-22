# MeshUptime

Bewaking op een Heltec LoRa32 V3, in de geest van Uptime Kuma maar met LoRa als
uitweg. Draait op een MeshCore-node, biedt zijn uitslagen aan als
telemetriesensoren die een andere node kan opvragen, en waarschuwt over het mesh
als er iets omvalt.

Begonnen als één sensor-node, uitgegroeid tot een **multiroom room-server**: één
toestel bedient meerdere rooms, virtuele sensor-nodes en een chat-bot tegelijk, en
doet zelf zijn netwerk-diagnoses (ping, port, http, traceroute) en SNMP-metingen.
De bot is intussen een **tweerichtings** mesh-diagnose-responder — hij antwoordt op
`ping`/`test`/`path` zowel per DM als in **hashtag-/publieke kanalen** — de node
synct zijn klok over **NTP** en toont menselijke tijden in de lokale tijdzone, en
een **grote contactenlijst** lost overal node-namen op.
De huidige versie is **MeshUptime v2.3.2** (eigen versionering, los van de
MeshCore-bibliotheek v1.17.0); de branding toont beide:
`MeshUptime v2.3.2 (by DinX) - MeshCore v1.17.0`. De feature-geschiedenis staat in
**[firmware/CHANGELOG.md](firmware/CHANGELOG.md)**.

Er zijn **twee PlatformIO-envs**:

- **`meshuptime_room`** — de room-server-variant (build-flag `ROOM_SERVER_VARIANT`,
  `main_room.cpp` / `RoomMesh`). **Dit is het huidige hoofdproduct.**
- **`meshuptime`** — de oorspronkelijke sensor-variant (`main.cpp` / `SensorMesh`,
  adverteert `ADV_TYPE_SENSOR`). Blijft de terugvalweg en compileert ongewijzigd.

*English documentation: [docs/en/](docs/en/README.md).*

## Waarom LoRa en niet het netwerk

Omdat het gemeten is en niet aangenomen. Op 19 augustus 2026 is deze node op
batterij gezet met de stekker eruit:

- eerst nog enkele minuten bereikbaar, met **31% verlies** op de HTTP-metingen
- daarna volledig weg — 21 opeenvolgende mislukte metingen — met
  `WiFi disconnected (reason 202)`, dus `AUTH_FAIL` en niet zwak bereik
- **de USB er weer in bracht hem niet terug**; pas een harde reset herstelde de
  verbinding

Juist tijdens een stroomstoring is de netwerkweg dus het minst betrouwbaar. Een
webhook vanaf een bestaande Uptime Kuma kan deze node op dat moment per definitie
niet bereiken. Daarom gaan de waarschuwingen over LoRa, zijn de eigen controles
de kern en niet een aanvulling, en zit er een wifi-waakhond in.

## Wat werkt, en hoe zeker

| | |
|---|---|
| Basis | `examples/simple_sensor` van MeshCore tag `companion-v1.17.0` |
| Rol (sensor-env) | `ADV_TYPE_SENSOR` |
| Rol (room-env) | rooms (`ADV_TYPE_ROOM`), sensor-nodes (`ADV_TYPE_SENSOR`), bot (`ADV_TYPE_CHAT`) |
| RAM, room-env | ~54% (van 327.680) |
| Flash, room-env | ~46% (van 3.342.336) |
| RAM, sensor-env | ~47% (van 327.680) |
| Flash, sensor-env | ~45% (van 3.342.336) |

De room-env is groter: hij draagt de extra panelen (rooms, sensor-nodes, bot,
SNMP, kanalen, tijd), de async netwerk-engine en de grote advert-/contactlijst
(~13,6 kB RAM, RAM-only). De sensor-env is de kale terugvalweg. De oudere,
kleinere cijfers van de eerste sensor-only bouw staan in
[docs/metingen.md](docs/metingen.md) als historisch ijkpunt.

**Kern (op het toestel gezien in de sensor-basis):** de voedingssensoren met hun
gekalibreerde drempels, de wifi-sensor met de waakhond erachter, ping-monitors die
een herstart overleven, `/hook` voor een bestaande Uptime Kuma, telemetrie die
door een tweede node opgevraagd en gelezen wordt, de webinterface met
bewaking/toegang/nodebeheer, en de CLI over serieel.

**Room-server-variant (v2.0.0 → v2.3.2), het huidige hoofdproduct:**

- **Multiroom** (`RoomMesh`): tot vier room-identiteiten (`MAX_ROOMS=4`), elk met
  eigen sleutelpaar, naam, admin- en gastwachtwoord en toegangslijst; gedeelde
  post-pool (`MAX_TOTAL_POSTS=48`, per-room quota). Room 0 hergebruikt de
  bestaande hoofdidentiteit. De MeshCore-app ziet elke room als een aparte Room.
- **Virtuele sensor-nodes** (`MAX_SENSOR_NODES=4`, `ADV_TYPE_SENSOR`): aparte
  identiteiten zodat de app telemetrie toont; subset-telemetrie per node om voorbij
  de één-pakket-CayenneLPP-limiet te komen. Elke sensor is aan nul of meer rooms én
  nul of meer sensor-nodes gekoppeld.
- **Per-sleutel-ACL** (wachtwoordloos, `MAX_ACL_GRANTS=16`): drie niveaus
  (read/readwrite/admin) gebonden aan een pubkey; toegang zonder wachtwoord omdat
  het antwoord met het ECDH-geheim van die sleutel versleuteld wordt. Persistent in
  `/acl_grants`, los van de vluchtige login-sessies.
- **Kanaalbeheer-panel** (node-centrisch): elke sensor koppelen/ontkoppelen aan
  rooms/sensor-nodes, plus handmatig advert (flood of zero-hop) per room /
  sensor-node / bot.
- **Room/DM-commandoset** (rechten-gestuurd): read `dns`, `ping <host> [n]`,
  `port <host> <poort>`, `http <url>`, `scan`, `traceroute <host>`,
  `neighbors`/`nb`, `wifi`, `sys`/`health`, `history <s>`; readwrite `checknow`,
  `mute`/`unmute`, `snooze`, `test`; admin `sendto <pubkey> <msg>`, `reboot`,
  `ntp`/`sync`. Uitgestelde uitslagen komen terug naar de oorsprong-room/DM.
- **Async netwerk-taak-engine** (v2.2.0): coöperatief, niet-blokkerend, gestapt
  vanuit `loop()` — verstoort de LoRa-timing niet. Diagnoses `port`, `scan`, `http`
  en `traceroute` (hop-afstand via TTL-oplopende `esp_ping`; tussenliggende hop-IP's
  zijn op dit lwIP/esp_ping-platform niet beschikbaar).
- **SNMP-monitorsoort** (v2.2.0): de node doet zelf een niet-blokkerende SNMP-GET
  (BER/ASN.1, v2c over UDP:161); de waarde stroomt door de bestaande
  telemetrie/alert-pijplijn. De community-string wordt geobfusceerd bewaard en nooit
  gelogd, geëxporteerd of getoond.
- **Bot** (v2.2.0): een persistent chat-contact `BE-HSS-DinX-Bot` (`ADV_TYPE_CHAT`)
  dat schone DM's stuurt — voor flash-meldingen én voor de per-sensor `dm`/`both`-
  alerts, met herhaal-tot-ACK per ontvanger. Persistente ontvangerslijst
  (`/bot_recips`), CLI en room/DM-`sendto`.
- **Tweerichtings-bot** (v2.2.1): de bot ontvangt nu ook DM's en antwoordt op een
  kleine eigen commandoset — `ping`, `test` (SNR/RSSI/hops), `path` (route met
  repeaternamen) en `help` — als schone DM met ACK. De afzender-pubkey wordt uit de
  buurtlijst/ontvangerslijst opgelost en het gedeelde geheim eruit berekend.
- **NTP + tijdzone** (v2.2.2): NTP-server en tijdzone instelbaar via de web-GUI
  (paneel *Tijd*, `/time.cfg`, `POST /time`; standaard Europe/Brussels, DST-bewust).
  Menselijke tijden worden LOKAAL getoond mét zone-afkorting (bv. `00:35:11 CEST`);
  de RTC en alle MeshCore-protocoltijdstempels blijven UTC. Her-sync bij
  WiFi-connect en elke 6 u.
- **Hashtag- en publieke kanalen** (v2.3.0 → v2.3.2): de bot leest ingeschakelde
  MeshCore group-channels mee en antwoordt IN het kanaal op `ping`/`test`/`path`.
  Kanaalbeheer in de web-GUI (bot-tab: toevoegen/aan-uit/wissen), persistent in
  `/channels.cfg`, endpoints `/channels.json` + `/channel/add|del|toggle`, en CLI
  `channel list|add|del|on|off`. Toevoegen op naam zonder secret leidt de sleutel af
  (`sha256(naam)[:16]`, zoals de app); de naam `Public` gebruikt de vaste publieke
  sleutel `8b3387e9c5cdea6ac9e5edbaa115cd72`; een eigen 32/64-hex secret wint altijd.
- **Grote contactenlijst + naamresolutie** (v2.3.0): de buurtlijst groeide van 12
  naar **200** ingangen (RAM-only, ~13,6 kB) en lost overal node-namen op — de
  `path`-repeaters, de DM-afzender en `/contacts.json`. Kanaalberichten dragen de
  afzendernaam inline (géén pubkey); DM's dragen de pubkey (géén naam).
- **Ontdekte contacten**: `/contacts.json` (buurtlijst met volledige pubkey) voedt
  een kiezer in de web-GUI voor zowel de bot-ontvangers als de per-slot ACL-grants.

**Gebouwd maar nooit afgegaan:** de waarschuwingen over het mesh. Dat en al het
andere dat nog niet klopt staat in **[docs/openstaand.md](docs/openstaand.md)** —
begin daar als je dit project oppakt.

## De harde cijfers

De voedingsdrempels zijn afgeleid en niet gemeten: dit bord heeft geen VBUS-pin,
geen USB-detectie en geen laadstatus, alleen een ADC-meting van de
batterijspanning.

| toestand | spanning |
|---|---|
| batterij, onder belasting | 4,034 V |
| USB, lader klaar | 4,139 V |
| USB, actief ladend | 4,209–4,226 V |

Vandaar: onder **4,09 V** batterij, boven **4,12 V** netspanning, met 60 s
insteltijd na een overgang en drie opeenvolgende metingen aan dezelfde kant.
Beide drempels zijn instellingen en worden bewaard. Een naïeve drempel op 4,2 V —
wat een datasheet zou suggereren — zou "geen netspanning" hebben gemeld met de
stekker erin.

Twee andere getallen die overal doorwerken: een telemetriepakket heeft **180 byte**
te vergeven (24 vast, 9 per monitor, acht monitors), en een meshbericht wordt op
**158 byte** geknipt en niet op 160 — boven 158 kan een bericht na de vierde
poging niet meer opnieuw verstuurd worden.

**Kanaalnummers zijn onveranderlijk.** CayenneLPP draagt alleen nummers, geen
namen; schuift een nummer op, dan wijst een bewaarde koppeling stil naar de
verkeerde dienst.

| kanaal | inhoud |
|---|---|
| 1 | batterijspanning |
| 2 | netvoeding (0/1) |
| 3 | batterijvoeding (0/1) |
| 4 | wifi online (0/1) |
| 5 t/m 12 | monitors, zelf gepingd of van buiten gemeld |

## Bouwen en flashen

MeshUptime is een **zelfstandig PlatformIO-project**; MeshCore is een gepinde
git-submodule onder `firmware/vendor/MeshCore` (tag `companion-v1.17.0`). Volledige
uitleg — beide bouwwegen, beide envs, CI, de patch — staat in
[docs/bouwen.md](docs/bouwen.md).

De room-server-variant (hoofdproduct):

    git submodule update --init firmware/vendor/MeshCore
    cd firmware
    python -m platformio run -e meshuptime_room
    python -m platformio run -e meshuptime_room -t upload --upload-port COM4

De sensor-variant (terugvalweg) is dezelfde weg met `-e meshuptime`. Beide envs
delen hetzelfde bord, dezelfde modules en dezelfde patch-hook; `meshuptime_room`
erft alles van `meshuptime` en zet er de build-flag `ROOM_SERVER_VARIANT` bovenop.

De WiFi-gegevens staan **niet** in deze repo (plaatshouders in
`firmware/platformio.ini`; de op het toestel opgeslagen instelling is de baas,
zie [firmware/wifi.ini.voorbeeld](firmware/wifi.ini.voorbeeld)). De ene patch op
MeshCore in [firmware/patches/](firmware/patches/) — de global `sensors` is in de
Heltec V3-variant hardgecodeerd — wordt door de pre-build hook automatisch
aangebracht; met de hand hoeft dat alleen nog in de losse bouwkopie.

`upload` schrijft alleen de programmapartitie en niet SPIFFS: de identiteit in
`/identity/_main.id` en het instellingenbestand blijven staan.

## Documentatie

- **[firmware/CHANGELOG.md](firmware/CHANGELOG.md)** — de feature-geschiedenis per
  versie (v2.0.0 t/m v2.3.2); de gezaghebbende samenvatting.
- **[docs/openstaand.md](docs/openstaand.md)** — wat niet werkt, en welke
  verdenkingen al onderzocht en weerlegd zijn. Het belangrijkste bestand.
- [docs/werking.md](docs/werking.md) — hoe het werkt: rollen, kanaalindeling,
  rooms, sensor-nodes, per-sleutel-ACL, de async netwerk-engine, SNMP, de bot
  (tweerichtings, DM + kanalen), NTP/tijdzone, hashtag-/publieke kanalen,
  naamresolutie, webinterface, CLI, room/DM-opdrachten, toegangsbeheer,
  waarschuwingen
- [docs/metingen.md](docs/metingen.md) — de metingen met hun getallen en datum
- [docs/beslissingen.md](docs/beslissingen.md) — de keuzes met de reden, ook de
  keuzes die later zijn omgedraaid
- [docs/bouwen.md](docs/bouwen.md) — de twee bouwwegen, de twee envs
  (`meshuptime` + `meshuptime_room`), de submodule als gepinde afhankelijkheid, de
  patch-hook en de CI-tagmatrix
- [firmware/patches/LEESMIJ.md](firmware/patches/LEESMIJ.md) — de patches, en de
  patch die er met opzet niet is
- [docs/meting-voeding-2026-08-19.log](docs/meting-voeding-2026-08-19.log) — de
  ruwe meetreeks
