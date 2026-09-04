# Hoe MeshUptime werkt

Dit bestand beschrijft de firmware **zoals hij nu is**. Waarom de keuzes zo
gevallen zijn staat in [beslissingen.md](beslissingen.md); de getallen waarop ze
berusten in [metingen.md](metingen.md); wat er niet werkt in
[openstaand.md](openstaand.md).

Alles hieronder is nagekeken in de broncode van de laatste vastgelegde versie.
Waar iets gebouwd maar nooit op het toestel gezien is, staat dat er met zoveel
woorden bij.

## Wat het apparaat is

Eén Heltec LoRa32 V3 met MeshCore-firmware. Basis is `examples/simple_sensor`
van upstream tag `companion-v1.17.0` (dezelfde commit als `repeater-v1.17.0` en
`room-server-v1.17.0`), met een eigen `SensorManager`, een wifi-taak en een
webserver erbij.

### Twee varianten, één bord

Er zijn **twee bouwvarianten** van dezelfde firmware, met dezelfde modules op
hetzelfde bord:

- **Sensor-variant** (`env:meshuptime`, `main.cpp` / `SensorMesh`). De
  oorspronkelijke node: één identiteit, kondigt zich aan als **`ADV_TYPE_SENSOR`**
  (`SensorMesh.cpp:297`) en antwoordt op node-discovery met
  `CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_SENSOR`. Dat is het eerlijke type: dit
  apparaat biedt sensoren aan. Blijft de terugvalweg.
- **Room-server-variant** (`env:meshuptime_room`, build-flag
  `ROOM_SERVER_VARIANT`, `main_room.cpp` / `RoomMesh`). **Het huidige
  hoofdproduct.** Eén toestel draagt nu meerdere identiteiten tegelijk — rooms,
  virtuele sensor-nodes en een bot — elk met een eigen sleutelpaar en een eigen
  advert-type. De rest van dit bestand beschrijft, tenzij anders vermeld, de
  room-variant; de sensor-basis (voeding, wifi, kanalen, monitors) zit in béide en
  wordt hieronder als gedeelde kern beschreven.

`SensorMesh.cpp` blijft in beide envs meegecompileerd omdat de `WebTask` er
type-mede aan gekoppeld is; alleen `main_room.cpp` en `RoomMesh.cpp` schakelen
tussen de envs (zie [bouwen.md](bouwen.md)).

### De identiteiten van de room-server

De room-variant is bewust géén enkele node met één sleutel maar een verzameling
virtuele identiteiten, elk met een eigen advert-type zodat de MeshCore-app ze in
het juiste vakje toont:

| identiteit | advert-type | join-URI-type | aantal | wat de app toont |
|---|---|---|---|---|
| room | `ADV_TYPE_ROOM` | 3 | `MAX_ROOMS=4` | een Room-server |
| sensor-node | `ADV_TYPE_SENSOR` | 4 | `MAX_SENSOR_NODES=4` | een sensor met telemetrie |
| bot | `ADV_TYPE_CHAT` | 1 | 1 | een gewoon chat-contact |

Room 0 hergebruikt de bestaande hoofdidentiteit (de sleutel uit
`/identity/_main.id` blijft behouden). Waarom aparte identiteiten: de app toont
telemetrie **alleen** voor sensor-type contacten en chatberichten voor chat-type
contacten. Eén node die zowel room, sensor als notifier wil zijn, moet die petten
dus letterlijk als aparte contacten dragen.

Doorsturen zit in de basis en staat standaard uit
(`SensorMesh::allowPacketForward`: `disable_fwd`, plus `flood_max` op een
floodpakket). Het is dus een vlag en geen tweede firmware — zie
[openstaand.md](openstaand.md) voor wat er nog moet gebeuren voordat die vlag
verantwoord aan kan.

## De kanaalindeling, en waarom die vast staat

CayenneLPP draagt **alleen kanaalnummers, geen namen**. Een node die telemetrie
opvraagt bewaart dus zelf "kanaal 6 = google". Schuift dat nummer ooit op, dan
wijst die koppeling **stil** naar een andere dienst: geen foutmelding, alleen
verkeerde cijfers op een dashboard.

Daarom: **een monitor krijgt bij aanmaken een kanaalnummer en houdt dat voor
altijd. Nummers van verwijderde monitors worden niet hergebruikt zolang er nog
een vrij nummer is.**

| kanaal | inhoud | LPP-type |
|---|---|---|
| 1 | batterijspanning | `LPP_VOLTAGE` (zet `SensorMesh` zelf) |
| 2 | netvoeding | `LPP_SWITCH` 0/1 |
| 3 | batterijvoeding | `LPP_SWITCH` 0/1, altijd het omgekeerde van 2 |
| 4 | wifi online | `LPP_SWITCH` 0/1 |
| 5 t/m 12 | monitors | `LPP_SWITCH` 0/1 plus `LPP_GENERIC_SENSOR` (ms) |

Acht monitorvakjes (`MON_MAX_MONITORS`), net zoveel als er kanalen zijn. Welke
nummers ooit vergeven zijn staat als bitmasker `ch_ever_used` in het
instellingenbestand, zodat een herstart geen nummer opnieuw uitdeelt.

`LPP_GENERIC_SENSOR` is met opzet gekozen: 4 byte, multiplier 1, exacte hele
getallen, en het belooft geen onwaar grootheid. `analoginput` loopt boven 327 ms
dood, `luminosity` beweert lux, `frequency` beweert hertz.

### Het bytebudget van één pakket

`SensorMesh` maakt zijn CayenneLPP als `MAX_PACKET_PAYLOAD - 4` = **180 byte**.
De vaste velden kosten 24 byte, een monitor in het duurste geval 9. Acht
monitors is 72 byte, dus er blijft 84 over voor omgevingssensoren die er later
bij komen. `querySensors()` rekent dat bij elke ronde na en kapt af in plaats van
op de goede wil van de bibliotheek te vertrouwen. De volledige begroting staat in
[metingen.md](metingen.md).

## De twee soorten monitor

Voor de telemetrie zien ze er **identiek** uit — een dashboard hoort niet te
hoeven weten hoe wij aan onze uitslag komen.

**PING.** Wij pingen zelf, op ons eigen interval (10–3600 s, standaard 60 s).
Eén ping tegelijk, alles in stappen vanuit `loop()`, nergens gewacht: de
LoRa-radio wordt uit dezelfde lus bediend en heeft de strengste tijdseisen van
het apparaat. Drie mislukte rondes maken een dienst "neer", twee gelukte maken
hem weer "op" (omhoog mag sneller: dat kost geen alarm). Een naam wordt hoogstens
elke tien minuten opnieuw opgezocht.

**GEMELD.** Iets van buiten (bijvoorbeeld een bestaande Uptime Kuma) meldt de
uitslag via `/hook`. Wij pingen zo'n monitor niet; wij laten hem **verouderen**
als de melding uitblijft, na `max(3 x meldperiode, 90 s)`. Verouderd is
**onbekend** en niet "neer" — als de melder zelf plat ligt, weten wij niets, en
dat is niet hetzelfde als "nog steeds up".

De soort wordt niet in een apart veld bewaard maar aan het adres afgelezen:
adres `-` betekent gemeld. Dat is geen truc maar zuinigheid met het
bestandsformaat: `-` is geen geldige hostnaam en geen IP-adres, dus er valt geen
echte monitor per ongeluk in die tak, en oude bestanden blijven leesbaar.

Van een gemelde dienst worden alleen **naam, kanaal en meldritme** bewaard. De
toestand staat nergens op schijf, want die is per definitie vluchtig: na een
herstart weten wij niets tot de eerste nieuwe melding, en "onbekend" is dan het
enige eerlijke antwoord.

### Bevriezen in plaats van "neer" melden

Valt onze eigen wifi weg, dan meten we ons eigen netwerk en niet de dienst.
De monitors gaan dan op pauze: de getoonde waarden zijn de laatst **gemeten**
waarden, en kanaal 4 op 0 vertelt de lezer dat ze oud zijn. Na terugkomst van
wifi eerst 30 s niets doen — DHCP, DNS en de route zijn dan nog niet klaar, en
een ping die daarop stukloopt zou een dienst onterecht down verklaren.

Om dezelfde reden loopt de verouderingsklok van een gemelde dienst niet zolang
wij zelf blind zijn: zonder onze wifi kan `/hook` ons niet bereiken, en dan is de
stilte onze fout en niet die van de melder.

## Voeding: afgeleid, niet gemeten

Dit bord heeft **geen VBUS-pin, geen USB-detectie en geen laadstatus**. Er is
alleen `getBattMilliVolts()`, een ADC-meting van de batterijspanning. Netspanning
wordt dus **afgeleid** uit de klemspanning, met drempels die op deze hardware
gekalibreerd zijn (zie [metingen.md](metingen.md)).

De toestandsmachine, allemaal instelbaar of nagekeken in de bron:

- twee drempels (Schmitt): standaard onder **4,09 V** batterij, boven
  **4,12 V** netspanning, zodat een spanning ertussen geen mening oplevert in
  plaats van geflikker
- **drie** opeenvolgende metingen aan dezelfde kant voor een overgang, met
  10 s tussen twee metingen — dus ongeveer 30 s reactietijd
- **60 s insteltijd** na een overgang, want in de eerste minuut na uittrekken is
  een transiënt van ongeveer 50 mV gemeten
- de drempels zijn **instellingen** en geen constanten, en ze worden bewaard

Netvoeding en batterijvoeding zijn twee aparte kanalen, zoals gevraagd, maar het
is één meting met twee namen: kanaal 3 is per definitie de spiegel van kanaal 2.

De blinde vlek is eerlijk te noemen: bij een volle batterij zonder lading lijkt
"stroom weg" op "stroom aanwezig". Daarom hoort de wifi-sensor erbij. Valt de
router weg, dan verdwijnt wifi binnen seconden; wifi weg **én** een dalende
batterijspanning is een stroomstoring met hoge zekerheid en met de snelheid van
de eerste van de twee.

## De wifi-waakhond

Geen voorzorg maar de reparatie van gemeten gedrag: op 19 augustus 2026 raakte de
node op batterij van het netwerk met `reason 202` (`WIFI_REASON_AUTH_FAIL`) bij
ruim voldoende bereik, en hij kwam er **niet** uit — ook nadat de USB er weer in
zat bleef hij onbereikbaar, tot een harde reset.

`WiFi.reconnect()` is daarom niet genoeg. De taak heeft vijf toestanden (uit,
verbinden, online, opnieuw, eigen AP) en loopt op:

- 20 s per verbindingspoging, 15 s wachten voor de volgende
- na 3 minuten zonder verbinding gaat de radio volledig omlaag en opnieuw op
- na 3 mislukte pogingen een harde reset van de wifi-stapel, na 6 een eigen
  toegangspunt (`MeshUptime-…`), zodat het toestel instelbaar blijft

Het aantal herverbindingen, het aantal harde resets en de laatste reden van
wegvallen staan in de webinterface. "Geen wifi" zonder reden kost iemand een
avond zoeken.

## Tijd: NTP, tijdzone en lokale weergave

Sinds v2.2.2 zijn de **NTP-server** en de **tijdzone** instelbaar via de web-GUI
(paneel *Tijd*, bewaard in `/time.cfg`, `POST /time`). Standaard is de NTP-server
`pool.ntp.org` (een LAN-tijdserver mag ook) en de tijdzone Europe/Brussels als
POSIX-TZ-string `CET-1CEST,M3.5.0/2,M10.5.0/3` — dus DST-bewust. Opslaan past de TZ
meteen toe en vraagt een her-sync aan. De sync is robuust: al bij WiFi-connect én
periodiek, elke 6 uur.

**Menselijke tijden lokaal, protocol en RTC in UTC.** Alle voor mensen bedoelde
tijden worden getoond in de **lokale** tijd mét de zone-afkorting (bv.
`Received at: 00:35:11 CEST`), via `localtime_r`/`strftime %Z` op de ingestelde TZ.
De RTC en **alle** MeshCore-protocoltijdstempels blijven UTC — de mesh rekent in
UTC. Vóór de eerste geslaagde sync toont de node `niet gesynct` in plaats van een
garbage-tijd (`TimeFmt.h`, `TIME_FLOOR`). `/cfg.json` legt dit bloot via
`ntp`, `tz`, `tlocal` (de lokale datum/tijd), `tsync`, `tsyncmsg` en `tsyncage`.

## De room-server

Alles hierboven — kanalen, monitors, voeding, wifi — is de gedeelde sensor-kern.
Hieronder staat wat de room-variant (`RoomMesh`) er bovenop legt.

### Rooms

Eén toestel bedient tot **vier** room-identiteiten (`MAX_ROOMS=4`), elk een
volwaardige MeshCore Room: eigen sleutelpaar, eigen naam, een **admin-** én een
**gastwachtwoord**, en een eigen toegangslijst (`ClientACL`). Room 0 hergebruikt de
hoofdidentiteit; de andere krijgen een vers sleutelpaar bij aanmaken.

De posts delen één pool (`MAX_TOTAL_POSTS=48`) met een quotum per room, zodat één
drukke room de andere niet leeghongert. In de MeshCore-app verschijnt elke room als
een aparte Room-server waar je met wachtwoord op inlogt; de gast-rol mag meelezen
maar niet beheren.

### Virtuele sensor-nodes

Naast de rooms draagt de node tot **vier** sensor-node-identiteiten
(`MAX_SENSOR_NODES=4`), elk van het sensor-type (`ADV_TYPE_SENSOR`) met een eigen
sleutelpaar. De reden is puur de app: die toont telemetrie alleen voor sensor-type
contacten, niet voor rooms. Wie zijn metingen in de app wil zien, koppelt ze aan
een sensor-node.

Elke sensor draagt maar één CayenneLPP-pakket, en dat pakket is begrensd (zie *Het
bytebudget* hierboven). Met meerdere sensor-nodes kun je de monitors **spreiden**:
node A draagt kanaal 1–6, node B draagt 7–12 — subset-telemetrie per node, zodat je
voorbij de één-pakket-limiet komt. Elke sensor(-monitor) heeft daarom twee maskers:

- **rooms-masker** — aan welke rooms de sensor zijn up/down-alerts stuurt;
- **sensornodes-masker** — via welke sensor-node(s) de telemetriewaarde uitgaat.

Beide maskers mogen leeg zijn (nergens gekoppeld) of meerdere bits zetten. Een
sensor kan dus tegelijk in twee rooms alarmeren en via twee sensor-nodes telemetrie
voeden.

### Per-sleutel-ACL: wachtwoordloze toegang

Naast de wachtwoord-login is er een **per-sleutel-ACL** (`MAX_ACL_GRANTS=16`,
bewaard in `/acl_grants`): een lijst grants die elk een **pubkey** aan één van drie
niveaus binden — `read`, `readwrite` of `admin`.

De toegang wordt verleend **zonder wachtwoord**, en dat is geen gat: het antwoord
wordt versleuteld met het ECDH-geheim dat uit die pubkey afgeleid is. Alleen de
houder van de bijbehorende privésleutel kan het lezen, dus het bezit van de sleutel
ís de authenticatie. Deze grants zijn **persistent** en staan los van de vluchtige
login-sessies: een reboot wist de sessies maar niet de grants.

### Kanaalbeheer-panel

Het koppelen gebeurt node-centrisch, in één panel: per sensor kun je de rooms- en
sensornodes-maskers zetten (koppelen/ontkoppelen). Vanuit hetzelfde panel gaat ook
het **handmatig advert**: per room, per sensor-node of voor de bot een advert
uitsturen, als **flood** (het hele mesh) of **zero-hop** (alleen directe buren).
Handig om een verse identiteit bekend te maken zonder op de volgende automatische
advert te wachten.

### De room/DM-commandoset

In een room of in een DM aan een room reageert de node op een uitgebreide
commandoset, **gestuurd door het rechtenniveau** van de afzender:

| niveau | commando's |
|---|---|
| read | `dns`, `ping <host> [n]`, `port <host> <poort>`, `http <url>`, `scan`, `traceroute <host>`, `neighbors`/`nb`, `wifi`, `sys`/`health`, `history <s>` |
| readwrite | `checknow`, `mute`/`unmute`, `snooze`, `test` |
| admin | `sendto <pubkey> <msg>`, `reboot`, `ntp`/`sync` |

`ping` geeft niet alleen bereikbaarheid maar ook **verlies%** en **jitter**.
Uitgestelde uitslagen (ping en de netwerk-diagnoses) komen terug **naar de
oorsprong** — dezelfde room of DM waar het commando vandaan kwam — via het algemene
oorsprong-routing-mechanisme (de v2.1.0 "ping-fix", nu voor alle trage taken).

Daarnaast zijn er de **subsysteem-commando's** die de identiteiten en kanalen
beheren, ook over serieel en (voor een beheerder) over een DM: `room …`,
`sensornode …`, `bot list|add|del|post|sendto|advert|uri` en
`channel list|add|del|on|off`.

### De async netwerk-taak-engine

De diagnoses `port`, `scan`, `http` en `traceroute` zouden blokkeren als je ze
naïef uitvoert, en blokkeren is hier verboden: de LoRa-radio wordt uit dezelfde
`loop()` bediend en heeft de strengste tijdseisen. Daarom een **coöperatieve,
niet-blokkerende taak-engine**: één taak tegelijk, in kleine stappen vanuit
`loop()`, met het resultaat terug naar de oorsprong.

- **`port <host> <poort>`** — een non-blocking TCP-connect: lukt de verbinding, dan
  is de poort open.
- **`scan`** — een asynchrone WiFi-scan van de omgeving.
- **`http <url>`** — haalt de **statuscode** en de **responstijd** op.
- **`traceroute <host>`** — meet de **hop-afstand** eerlijk: met `esp_ping` wordt de
  TTL opgevoerd tot de bestemming antwoordt; die TTL is dan het aantal hops.
  Begrensd op **30 hops**, ~1,2 s per TTL. **De tussenliggende hop-IP's zijn op dit
  lwIP/esp_ping-platform niet beschikbaar** — je krijgt de afstand, niet de route.
  Dat staat er met opzet eerlijk bij; het is geen bug maar een platformgrens.

### SNMP als monitor-soort

Naast *ping* en *gemeld* is er een derde monitor-soort: **SNMP**. De node doet zelf,
op het poll-interval, een **niet-blokkerende SNMP-GET** (BER/ASN.1-codering, v2c
over UDP:161). Het is geen nieuw verzendpad — de gepollde waarde stroomt door
**precies dezelfde** telemetrie- en alert-pijplijn als elke andere monitor:
CayenneLPP (generiek kanaal) → sensor-nodes → mesh → MeshManager voor de waarde;
up/down → rooms/DM voor de alerts. Alleen de **bron** is nieuw.

Een SNMP-monitor draagt: naam, doel-IP (poort :161 vast), de **v2c community**, een
OID, een **interpretatie** (`numeric` / `rate` / `status`) met een `snmparg` (de
vergelijkwaarde bij status of de referentie bij rate), een interval en de gewone
am/rm/sn-maskers (alarm-, rooms- en sensornodes-maskers).

De community-string is een geheim en wordt zo behandeld: **geobfusceerd** bewaard
(XOR + hex in `/monitors.cfg`) en **nooit** gelogd, geëxporteerd of in een UI
getoond. Beheer kan via de web-GUI (SNMP-formulier + `POST /monitor/snmp`), via de
CLI en via room/DM (`mon.<kanaal>.type` / `.community` / `.oid` / `.interp` /
`.snmparg`).

### De bot: een virtueel chat/notifier-contact

De per-sensor `dm`- en `both`-alerts kwamen vroeger van de room-identiteit, en dat
paste slecht: in de app zag je een Room je een DM sturen. Daarom een aparte
**bot-identiteit**, `BE-HSS-DinX-Bot`: een eigen persistent sleutelpaar
(`/bot_id`), adverteert als **`ADV_TYPE_CHAT`**, en verschijnt dus als een gewoon
chat-contact.

De bot stuurt **schone DM's** — voor flash-meldingen én voor de per-sensor
`dm`/`both`-alerts, herbedraad van de room naar de bot, met **herhaal-tot-ACK** per
ontvanger. Zijn ontvangerslijst is persistent (`/bot_recips`, 16 volledige
pubkeys) en wordt op een verse node geseed met de eigenaar. De bot rekent zijn
gedeelde geheim **zelf** uit de ontvanger-pubkey (`calcSharedSecret` +
`createDatagram`), dus hij hangt niet aan een room-sessie.

**Tweerichtings sinds v2.2.1.** De bot was eerst alleen-zenden: een inkomende DM
aan de bot viel door naar de identiteit van `rooms[0]` en werd met de **verkeerde
sleutel** ontsleuteld (en dus stil verworpen) — dat was de bug. `onRecvPacket`
matcht nu ook de bot-identiteit `_bot_id`. Een chat-DM draagt alleen een 1-byte
src-hash en géén pubkey, dus de afzender-pubkey wordt opgezocht in de buurtlijst
(gehoorde adverts) en de ontvangerslijst, en het gedeelde geheim wordt daaruit
berekend. Zo kan de bot ontsleutelen en terugantwoorden zonder wachtwoord-login.

De bot heeft een kleine, eigen commandoset over DM (GÉÉN monitoring-console):

| commando | antwoord |
|---|---|
| `ping` | `Pong (HH:MM:SS <zone>)` — de lokale tijd met zone-afkorting |
| `test` | signaalrapport: SNR / RSSI / hops van het inkomende pakket |
| `path` | de afzender + de tussenliggende repeaters bij **naam** + SNR/RSSI + de ontvangst­tijd (`Received at:` lokaal) |
| `help` | somt de bot-commando's op; onbekende tekst → korte hint |

Het antwoord is een schone DM vanaf de bot, met een ACK op het inkomende bericht.
De velden komen uit het inkomende pakket (pad/hops, SNR×4→dB, RSSI, tijd); de
antwoordbuffers staan `static` en niet op de loopTask-stapel.

Bediening:

- **CLI**: `bot list | add <pubkey> | del <prefix> | post <msg> |
  sendto <pubkey> <msg> | advert [flood] | uri`.
- **Room/DM**: het admin-commando `sendto <pubkey> <msg>`.
- **Web-GUI**: het bot-tab (join/QR, ontvangerbeheer, `sendto`/`post`).

### Hashtag- en publieke kanalen

Sinds v2.3.0 leest de bot ook de ingeschakelde MeshCore **group-channels** mee en
antwoordt IN het kanaal op `ping` (Pong), `test` (signaalrapport) en `path` (route
met repeaternamen) — dezelfde stijl als de DM-responder, maar met de naam van de
afzender ervoor. Het antwoord is een geflood group-bericht `"<botnaam>: <antwoord>"`
(`createGroupDatagram`); het meelezen loopt via de overrides
`searchChannelsByHash`/`onGroupDataRecv`.

Een kanaal is in MeshCore niets meer dan een **gedeeld secret** (16 byte = 128-bit
of 32 byte = 256-bit); de **kanaal-hash** is de eerste byte van `sha256(secret)`.
De node volgt exact dat formaat. Beheer via de web-GUI (bot-tab:
toevoegen/aan-uit/wissen), persistent in `/channels.cfg`; endpoints
`GET /channels.json` en `POST /channel/add|del|toggle`; CLI
`channel list|add <naam> [secrethex]|del <naam>|on <naam>|off <naam>`.

Er zijn **drie soorten sleutel**, zoals de GUI/CLI/`/channel/add`-respons ook
melden:

- **eigen sleutel** — een expliciete 32- of 64-hex secret. Wint altijd, wordt nooit
  teruggetoond.
- **sleutel afgeleid uit naam** (v2.3.1) — voeg een kanaal toe met een naam maar
  **zonder** secret en de node maakt een **hashtag-kanaal**: de sleutel is de eerste
  16 byte van `sha256(naam)`, EXACT zoals de MeshCore-app (`#test` →
  `9cd8fcf22a47333b591d96a2b848b73f`). Zelfde naam = zelfde kanaal als de app. Een
  afgeleide sleutel is niet geheim: wie de naam kent, leidt hem af.
- **publiek kanaal (vaste sleutel)** (v2.3.2) — de naam `Public` (case-insensitief,
  met of zonder `#`) zonder secret gebruikt de vaste, welbekende publieke sleutel
  `8b3387e9c5cdea6ac9e5edbaa115cd72` i.p.v. `sha256("public")`, zodat de node op het
  ECHTE publieke kanaal uitkomt, net als de app. De `pub`-vlag in `/channels.json`
  (`is_public`) wordt afgeleid door het secret met de publieke sleutel te
  vergelijken, niet apart bewaard.

### Naamresolutie en de grote contactenlijst

Om in `path`, in een DM en in `/contacts.json` echte node-**namen** te tonen groeide
de buurtlijst (`NeighbourList`) in v2.3.0 van 12 naar **200** ingangen: pubkey, naam,
advert-type, snr/hops en laatst-gehoord, LRU op laatst-gehoord. Dat kost ~13,6 kB RAM
— de bewuste ruil — en de lijst blijft **RAM-only**: adverts komen te vaak voor een
per-advert-flashschrijf, en na een herstart stromen de namen binnen minuten vanzelf
weer binnen.

De **resolutievolgorde** is een gevolg van wat een bericht draagt:

1. **inline naam uit het bericht** — een group-bericht draagt de afzendernaam IN de
   tekst (`"<naam>: <tekst>"`, zoals de app) maar géén pubkey; die naam gebruiken we
   direct.
2. **de grote advert-/contactlijst**, op pubkey — voor `path` en DM's, die wél een
   pubkey dragen maar géén naam.
3. **hex-pubkey-prefix** als laatste terugval, als de naam nergens bekend is.

Kort: een kanaalbericht heeft de naam (geen pubkey), een DM heeft de pubkey (geen
naam) — vandaar de twee verschillende wegen naar hetzelfde antwoord.

### Ontdekte contacten en de kiezer

`GET /contacts.json` geeft de **buurtlijst met volledige pubkey** plus naam,
snr/hops, laatst-gehoord en teller. De weergave is gekapt op `NB_JSON_MAX=64`
ingangen; de lijst zelf blijft 200 groot voor de resolutie hierboven. De web-GUI
gebruikt die lijst als **kiezer** op twee plekken: bij de bot-ontvangers en bij de
per-slot ACL-grants. Zo kies je uit gehoorde nodes in plaats van een sleutel van 64
hextekens met de hand te plakken. Er gaan **alleen publieke sleutels** over deze weg
— nooit een geheim.

### Join-URI's en QR

Elke identiteit is te delen met een MeshCore join-URI, in de web-GUI client-side
als QR getekend (geen externe assets):

    meshcore://contact/add?name=<naam>&public_key=<64hex>&type=N

met `type` **1** = chat (de bot), **2** = repeater, **3** = room-server (rooms),
**4** = sensor (sensor-nodes). Een kanaal delen gaat met
`meshcore://channel/add?name=<naam>&secret=<32hex>`; voor een hashtag-kanaal wordt de
sleutel uit de naam afgeleid en voor `Public` is het de vaste publieke sleutel (zie
*Hashtag- en publieke kanalen*).

## De webinterface

Één pagina uit PROGMEM, één webserver, geen framework, geen CDN, geen externe
stylesheet, geen extern lettertype: een node zonder internet moet zijn eigen
beheerpagina kunnen tonen. Donker als standaard, licht via
`prefers-color-scheme`, systeemlettertypen met mono voor de getallen. De
sensor-variant heeft drie tabbladen — bewaking, toegang, node; de room-variant
voegt de tabbladen **rooms**, **sensor-nodes** en **bot** toe. Op de sensor-variant
blijven die tabbladen verborgen en geven de bijbehorende endpoints `501`. Alle
secties zijn inklapbaar.

Kleur is semantisch en niet decoratief. Daarvoor bestaat het veld `sev`: "aan"
op kanaal 2 (netvoeding) is goed, "aan" op kanaal 3 (batterijvoeding) is juist
een waarschuwing. Een pagina die de kleur uit het woord raadt, verft die twee
hetzelfde en dan liegt de kleur.

Alles zit achter een login met een **sessiecookie**: `POST /login` zet de cookie,
`POST /logout` wist hem, en de webgegevens zijn zelf te zetten via `/web/cred`
(en te resetten via `/web/cred/reset`). Ook de gevoelige endpoints — waaronder de
room-backup/restore die sleutels dragen — zitten achter dezelfde auth.

### De routes

`GET`:

| route | wat het doet |
|---|---|
| `/` | de pagina |
| `/login` | het loginformulier |
| `/status.json` | de hele stand: voeding, wifi, monitors, heap, uptime |
| `/cfg.json` | de hele stand van `NodePrefs` in één verzoek (incl. `ntp`/`tz`/`tlocal`/`tsync`/`tsyncmsg`/`tsyncage`) |
| `/acl.json` | toegangslijst, stand van het slot, ontdekte buren |
| `/rooms.json` | de rooms + sensor-nodes met hun koppelingen |
| `/contacts.json` | buurtlijst met volledige pubkey (kiezer voor bot/ACL) |
| `/bot.json` | de bot: identiteit, join/QR, ontvangerslijst |
| `/channels.json` | de kanalen: naam, kanaal-hash, aan/uit, `pub`-vlag (secret nooit) |
| `/rooms/backup` | volledige room-config incl. sleutels (achter auth) |
| `/cli/remote` | stand van de lopende/laatste remote-CLI-opdracht (v2.6.0) |
| `/poller.json` | stand + config van de MeshManager-poller (v2.6.0) |
| `/repeater_targets.json` | doel-prefixen + "wachtwoord gezet: ja/nee" — **nooit** het wachtwoord (v2.6.0) |
| `/hook` | uitslag van buiten aanleveren (mag ook GET, zie onder) |

`POST`:

| route(s) | wat het doet |
|---|---|
| `/login`, `/logout` | sessie openen/sluiten |
| `/wifi` | netwerkinstellingen |
| `/time` | NTP-server + tijdzone zetten (`/time.cfg`) |
| `/hook` | uitslag van buiten aanleveren |
| `/monitor`, `/monitor/del` | ping/gemelde monitor aanmaken, verwijderen |
| `/monitor/snmp` | SNMP-monitor aanmaken |
| `/mon/alarm` | de alarm-route/maskers van een monitor zetten |
| `/acl`, `/acl/del`, `/acl/strict` | per-sleutel-ACL en het slot beheren |
| `/cli` | de schrijfweg voor nodebeheer (console) |
| `/web/cred`, `/web/cred/reset` | de webgegevens zetten/resetten |
| `/sim`, `/sim/clear` | een toestand simuleren / wissen (test) |
| `/alert/test` | een testwaarschuwing afvuren |
| `/room/add`, `/room/edit`, `/room/del` | rooms beheren |
| `/rooms/restore` | room-config terugzetten (achter auth) |
| `/snode/add`, `/snode/edit`, `/snode/del` | sensor-nodes beheren |
| `/room/acl`, `/snode/acl` | de ACL van een room / sensor-node zetten |
| `/room/advert`, `/snode/advert` | handmatig advert per room / sensor-node |
| `/bot/recipient` | een bot-ontvanger toevoegen/verwijderen |
| `/bot/advert`, `/bot/sendto`, `/bot/post` | bot adverteren / DM sturen / posten |
| `/channel/add`, `/channel/del`, `/channel/toggle` | kanaal toevoegen / wissen / aan-uit |
| `/cli/remote` | één admin-opdracht naar een ANDERE repeater over LoRa (v2.6.0) |
| `/poller` | de MeshManager-poller aan/uit + interval (v2.6.0) |
| `/repeater/target` | doel-wachtwoord of standaardwachtwoord zetten/wissen (v2.6.0) |

Op de sensor-variant bestaan de room-, snode-, bot- en kanaal-endpoints niet als
functie: ze geven **`501`** en de bijbehorende GUI-tabbladen blijven verborgen. De
remote-CLI- en poller-endpoints (`/cli/remote`, `/poller`, `/repeater/target` en hun
JSON-lezers) geven daar **`503`** met de reden erbij: ze leunen op `RepeaterCli` en
`Poller`, die alleen de room-variant draagt.

**Niets dat verandert gaat via GET**, en dat is geen formaliteit. Een GET die
een monitor wist, wordt door elke browser, elke prefetch en elke linkchecker
gevolgd — en verwijderen is hier niet omkeerbaar, want het kanaal komt niet
terug. Een `GET /cli?cmd=erase` zou een link zijn die de hele opslag wist zodra
een browser hem voorlaadt.

`/hook` is de uitzondering en mag ook GET, omdat Uptime Kuma de methode vrij
laat. Argumenten: `name`, `up` (0 of 1), `ms`, en optioneel `every` (10–3600 s,
de meldperiode). Het antwoord is `200` met **het kanaalnummer erin**
(`ok <naam> -> kanaal N`), want dat nummer is het enige waaraan de aanroeper zijn
dienst later terugvindt.

### Nodebeheer: een console en niet dertig formulieren

De MeshCore-app beheert een node over dezelfde CLI. Die blootleggen geeft in één
keer volledigheid, in plaats van dertig formulieren die elk apart kunnen
verouderen. De formulieren erboven zijn gemak voor wat je vaak doet; de console
is de garantie dat er niets ontbreekt.

**Radio achter drie sloten.** `freq`, `bw`, `sf` en `cr` bepalen of deze node nog
op hetzelfde mesh zit. Daarom een vinkje dat de knop vrijgeeft, een `confirm()`
die de oude naast de nieuwe waarden zet, en — het enige dat echt telt — **een
servergrens die `set radio` en `set freq` met 409 weigert zonder
`confirm=radio`**. De browser is niet het slot; een losse fetch komt er niet
langs. Ook een in de console getypte radio-opdracht krijgt de bevestiging, zodat
typen geen sluipweg is. `tx` staat apart, want dat is in vergelijking
ongevaarlijk.

Geweigerd met de reden erbij: alles met `prv.key` (privésleutel over HTTP zonder
TLS), `start ota` (opent een eigen AP én een tweede webserver op poort 80 — deze
pagina valt eronder weg) en `poweroff` (diepe slaap; alleen een fysieke reset
helpt).

**Monitors bewerken.** Naam, adres en interval zijn per regel te bewerken; het
kanaal verandert nooit. Alleen gewijzigde velden worden gestuurd, elk als
`sensor set mon.<kanaal>.<veld>`, dus via dezelfde zeef als de CLI en zonder
tweede schrijfpad. Een **adreswijziging** krijgt een eigen bevestiging: hetzelfde
kanaal betekent dan een andere dienst, de naam hoort mee te veranderen, en een
dashboard aan de andere kant moet bijgewerkt worden.

Dat bewerken moest er komen omdat kanalen schaars zijn: zonder bewerken kost elke
typefout een nummer, want een uitgedeeld nummer wordt niet hergebruikt.

### Een CLI-opdracht naar een ANDERE repeater (v2.6.0)

Sommige repeaters — bijvoorbeeld de dak-node `BE-HSS-JessaZH.VIR` (`e3d3f4d7edd0`)
— zijn alleen over LoRa te bereiken. `RepeaterCli` logt als beheerder op zo'n node
in en voert er CLI-opdrachten uit, precies zoals de MeshCore-app dat doet, maar dan
vanaf deze node in plaats van via een companion + Home Assistant.

De **handmatige** weg is de CLI-console: typ `@<pubkey>[:<wachtwoord>] <opdracht>`,
bijvoorbeeld `@e3d3f4d7edd0:geheim filter count`. De pubkey mag een prefix van 12
hextekens zijn als deze node ooit een advert van die repeater hoorde (dan staat de
volledige sleutel in de buurtlijst en kan het gedeelde ECDH-geheim berekend worden);
anders geef je de volle 64 hex. Het antwoord komt tien tot zestig seconden later
over LoRa terug — de console is niet-blokkerend en antwoordt "gestart" — en gaat
daarna ook naar MeshManager onder de naam `cmd:<opdracht>`.

Er loopt er **één tegelijk**: zendtijd is een gedeelde band. Een muterend commando
wordt **niet herhaald** (een herhaling zou het op de tegenkant opnieuw uitvoeren), en
`clkreboot`/`reboot`/`erase`/`set radio` eisen een expliciete `confirm=…` in de POST
— de gevaarlijkste is `clkreboot`, die de klok van de doelnode op mei 2024 zet en
hem herstart, waarna hij zonder een tijdig `time <epoch>` tot 2026 onzichtbaar is
voor iedereen die hem al kende (adverts met een niet-gestegen tijdstempel worden
weggegooid).

### De MeshManager-poller: Home Assistant valt uit de keten (v2.6.0)

MeshManager kan opdrachten in een wachtrij zetten voor een repeater die alleen iets
anders kan bereiken. Tot v2.6.0 leegde **Home Assistant** die wachtrij (poll →
companion → antwoord terug). De poller (`Poller.*`) doet dat nu op de node zelf:

1. **pollen** — elke `poll_secs` (standaard 30, minimaal 10) een
   `GET {push.url}/api/v1/commands` met het **sensorpush-token** als `Bearer`
   (dezelfde `push.url`/`push.token` als de sensorpush; de server aanvaardt dat token
   sinds kort ook op deze route). Antwoord:
   `{"refresh":[…], "settings":[{"prefix":"<hex>","params":[…]}, …]}`;
2. **uitvoeren** — voor elk `settings`-verzoek één `RepeaterCli`-sessie: eenmaal
   inloggen, dan de N parameters achter elkaar. `cmd:X` → stuur `X` letterlijk; elke
   andere param `P` → `get P` (exact de vertaling van de oude HA-pusher);
3. **terugmelden** — elk antwoord naar `POST /api/v1/repeater_settings`; **geen
   antwoord → `null`** ("gevraagd, geen antwoord"), nooit stilte.

**Clear-on-read.** De wachtrij wist zich bij het uitreiken, dus wat je ophaalt
bestaat daarna nergens meer. De poller polt daarom alleen als zijn eigen wachtrij
leeg is (plaats om te bewaren) en telt/logt wat er tóch niet in past.

**Wachtwoorden per doel.** Om in te loggen kent de node het admin-wachtwoord van het
doel uit een klein persistent tabelletje (`/rep_targets.cfg`, hoogstens 8):
pubkey-prefix → wachtwoord, plus één standaardwachtwoord als terugval. Beheer op de
kaart *MeshManager-poller* (nodebeheer-tabblad) en via `/repeater_targets.json` /
`/repeater/target`. Wachtwoorden worden **nooit teruggelezen** — de JSON zegt alleen
"gezet: ja/nee". Een onbekend doel zonder wachtwoord wordt niet uitgevoerd; elke
gevraagde parameter gaat als `null` terug.

**Veiligheid.** Eén sessie tegelijk (een nieuw verzoek wacht), niets blokkeert de
bewaking (alles stapsgewijs uit `loop()`, ná de monitoren en alarmen), geen
herhaling van muterende commando's, en **gevaarlijke commando's komen niet uit de
wachtrij de lucht in** (`clkreboot`/`reboot`/`erase`/`set radio`/`poweroff`/
`shutdown`/`start ota`/`prv.key` → geweigerd, `null` gemeld, gelogd). In de praktijk
stuurt MeshManager alleen leescommando's plus `cmd:filter count`/`cmd:region`; deze
zeef is de gordel voor het geval de `cli_params`-lijst wordt uitgebreid.

**Bekende beperking.** `refresh`-verzoeken (een statusverzoek, `REQ_TYPE_GET_STATUS`
— een ander protocol dan de CLI-sessie) worden in deze versie **niet** uitgevoerd:
ze worden gelogd als "niet ondersteund" en vallen weg.

**Aanzetten.** De poller staat standaard **uit**. Zet het standaardwachtwoord (of een
wachtwoord per doel-prefix), vink *poller aan* aan, kies eventueel het interval, en
klik *opslaan*.

## De CLI

Over de seriële console en — voor een beheerder — over een DM als
`TXT_TYPE_CLI_DATA`. Alles wat upstream `CommonCLI` kan, plus de
sensorinstellingen. De vorm is `sensor set <naam> <waarde>`, met het
`sensor`-voorvoegsel: de generieke get/set van `CommonCLI` zit daarachter en niet
los.

| instelling | betekenis |
|---|---|
| `mains.hi` | drempel omhoog; hierboven netspanning |
| `mains.lo` | drempel omlaag; hieronder batterij |
| `mains.state` | alleen lezen: huidige uitkomst |
| `mon.count` | alleen lezen: aantal monitors |
| `mon.add` | knop: `naam,adres[,interval]` |
| `mon.del` | knop: `naam` |
| `mon.<kanaal>.name` / `.host` / `.int` / `.state` | per monitor |

**Het getal in `mon.5.host` is het kanaal en geen rangnummer.** Een rangnummer
schuift op bij verwijderen en wijst dan stil naar een andere dienst.

Instellingen worden bewaard in `/monitors.cfg`: platte tekst, regelgebaseerd, met
`#MU1` als kop en een `.` als sluitregel. Schrijven gaat naar `/monitors.tmp` en
wordt daarna omgenoemd, dus een node die tijdens het schrijven zijn stroom
verliest houdt de oude, complete versie. Ontbreekt de sluitregel, dan wordt
**alles** verworpen in plaats van half toegepast: een halve monitorlijst is erger
dan geen, want dan bewaakt de node stil minder dan je denkt.

Tekst en geen binaire struct, om drie redenen: het is met `cat` te lezen over de
seriële console, een veld erbij maakt oude bestanden niet onleesbaar, en er is
geen uitlijning of endianness om je aan te vergissen.

## De DM-opdrachten

Dit zijn de basis-DM-opdrachten van de sensor-kern. De room-variant legt daar de
veel grotere, rechten-gestuurde **room/DM-commandoset** bovenop (zie *De
room-server* → *De room/DM-commandoset*): `ping`/`port`/`http`/`scan`/`traceroute`,
`checknow`/`mute`/`snooze`, `sendto`/`reboot`/`ntp` enzovoort. De vier hieronder
blijven de kern.

Vier opdrachten, in een gewoon tekstbericht over het mesh:

    list            alle sensoren: kanaal, naam, toestand
    get <naam>      één sensor met detail
    status          eigen toestand: voeding, wifi, uptime, heap
    help  (of ?)    de opdrachten

**Dit is de enige weg waarlangs een naam over het mesh gaat.** CayenneLPP heeft
geen naamveld, in geen enkele versie. `list` is dus geen gemak maar de sleutel
bij de telemetrie: zonder die lijst weet een ontvanger niet wat kanaal 7
betekent, en met een verouderde lijst leest hij verkeerde cijfers zonder
foutmelding. Wie de vorm van `list` verandert, verandert de enige naamdrager die
er is.

De vorm van een antwoord:

- Antwoorden gaan als **`TXT_TYPE_PLAIN`** en niet als `TXT_TYPE_CLI_DATA`,
  omdat voor CLI-antwoorden per ontwerp geen ACK verwacht wordt — dan weet je
  niet of je antwoord is aangekomen.
- Knippen op **158 byte** en niet op 160. `MAX_TEXT_LEN` is 160, maar
  `composeMsgPacket` weigert een herhaling van een bericht langer dan
  `MAX_TEXT_LEN-2` zodra `attempt > 3`. Op 160 knippen levert precies de stukken
  op die later niet meer opnieuw verstuurd kunnen worden. **Zet dit niet terug
  naar 160.**
- Een stuknummer `[2/5] ` is 6 byte en wordt vooraf van het budget afgetrokken —
  `sendGroupMessage` kapt een niet-passend voorvoegsel **stil** af. Dus 152 byte
  tekst per stuk; bij een antwoord van één stuk geen voorvoegsel en de volle 158.
- Hoogstens **vier stukken**. Bij SF11 kost een vol bericht ongeveer een seconde
  zendtijd, en die seconde is van iedereen — ook van de repeaters die hem
  doorgeven. Wat niet past wordt niet stil weggelaten: het laatste stuk eindigt
  met een slotregel die zegt hoeveel regels er niet in gingen.
- Drie pogingen per stuk, met echte ACK's. De pogingteller van het protocol is
  twee bits breed, dus vier is het maximum en drie laat er een over.
- Eén antwoord tegelijk. Komt er tussendoor een vraag van iemand anders, dan
  wordt die **niet** geACKt: zijn client meldt "niet bezorgd" en hij vraagt zelf
  opnieuw, en tegen die tijd is de vorige beurt klaar. Een wachtrij zou betekenen
  dat een handvol vragers samen twintig berichten kan laten uitzenden.

Wie geen leesrecht heeft krijgt één korte weigering zonder enig detail, en
hoogstens eens per tien minuten. Stil negeren is afgevallen omdat de afzender dan
niet weet of de node stuk is, buiten bereik is of hem weigert — daar gaat hij van
herhalen, en dat kost meer zendtijd dan een antwoord.

Let op: **in de praktijk bereiken de DM-opdrachten alleen een beheerder**. Zie
[openstaand.md](openstaand.md).

## Toegangsbeheer

De toegangslijst en het slot dat hem afdwingt horen bij de mesh en niet bij de
webserver: anders had een node zonder wifi geen toegangsbeheer. De webinterface
is één van de bedieningswegen; de andere zijn `setperm` en `set acl.strict` over
serieel of over een DM.

**Rechten.** De rol (gast, leesrecht, beheerder) plus twee bits voor
waarschuwingen (`PERM_RECV_ALERTS_LO` / `_HI`). De gastenrol betekent hier iets
eigens: mag gewaarschuwd worden, mag niet uitlezen. Dat is een zinnig recht —
denk aan een telefoon die alleen alarm hoeft te krijgen — en het is de reden dat
daar een paar regels naast `ClientACL::applyPermissions` staan, dat zo'n ingang
juist zou verwijderen.

**Het slot (`acl.strict`).** Staat hij aan, dan antwoordt de node op een
telemetrieverzoek alleen aan een afzender met minstens leesrecht. Staat hij uit,
dan blijft het gedrag van upstream: iedereen op het mesh mag uitlezen. **Uit is
de standaard**, met opzet: bij het flashen mag een werkende opstelling zichzelf
niet buitensluiten. De webinterface zegt in dat geval met zoveel woorden dat de
deur open staat.

Een geweigerd verzoek is **stilte** en geen leeg antwoord. Een leeg antwoord kost
zendtijd per geweigerd verzoek, en niets belet een vrager elke seconde te vragen
— dan is de weigering zelf een manier om de duty cycle op te maken. Bovendien
leest "nul sensoren" in een app als een kapotte sensor en niet als een gesloten
deur.

Het slot staat niet in `NodePrefs`: die struct is van upstream, en een veld erbij
raakt elke andere rol en het formaat van `/new_prefs`. Eén byte in een eigen
bestandje is het goedkoopste eerlijke antwoord. Het slot schrijft **direct** naar
flash — wie het dichtzet en meteen herstart moet hem dicht aantreffen — terwijl
ACL-wijzigingen met vertraging worden weggeschreven, want de lijst wordt bij elk
padbericht van een beheerder vies.

**Sleutels toevoegen en wissen.** Toevoegen vraagt 64 hextekens, want het
gedeelde geheim wordt uit de volle publieke sleutel gerekend: een prefix-ingang
zou een ingang zijn waarmee niet te praten valt, en een prefix is te vervalsen
door sleutelparen te grinden. Wissen mag vanaf 6 byte — zoveel geeft MeshCore
zelf terug over het mesh — en weigert zodra de prefix op meer dan één ingang
past.

**De buurtlijst.** Adverts worden opgevangen in `onAdvertRecv()`, de bestaande
virtuele haak, dus zonder patch in `src/`. Die plek omdat hij ná de
ed25519-controle draait: de sleutel is dan bewijsbaar van de afzender, en dat is
de voorwaarde om hem als keuzelijst voor de ACL te gebruiken. De lijst hield eerst
twaalf ingangen; sinds v2.3.0 zijn dat er **200** (voor de naamresolutie, zie *De
room-server* → *Naamresolutie*), LRU op laatst-gehoord. Hij voegt samen op sleutel
(een echte ring zou na 200 adverts van één drukke buur diezelfde buur telkens weer
tonen) en staat **niet** in SPIFFS: hij beschrijft wat er nú in de lucht is, en een
bewaarde versie zou beweren dat een node in de buurt is die er al een week niet meer
is.

Bij een buur staat **SNR en niet RSSI**, met het aantal hops erbij. `mesh::Packet`
bewaart alleen `_snr`; RSSI bestaat alleen live in de radio, en flood-pakketten
gaan met een scorevertraging in de wachtrij — dan hoort dat register al bij een
later pakket.

**De weg naar binnen is het beheerderswachtwoord.** Een login met het juiste
wachtwoord voegt een node automatisch als **beheerder** toe, ook met het slot
dicht, en geeft hem daarmee de hele CLI. Het slot en het wachtwoord zijn samen
één beveiliging, niet twee.

## Waarschuwingen over het mesh

De waarschuwingslus is die van upstream `SensorMesh`; MeshUptime levert de
voorwaarden aan. Elke leesronde (standaard elke 60 s) kijkt
`onSensorDataRead()` naar de batterijspanning en naar elk monitorvakje, en zet
per geval een `Trigger`. Vuurt een trigger, dan loopt hij de toegangslijst af en
stuurt naar iedereen met het passende waarschuwingsbit: `PERM_RECV_ALERTS_HI`
voor hoge prioriteit, `PERM_RECV_ALERTS_LO` voor lage.

- Een `Trigger` per **vakje** en niet per actieve monitor: dat verband mag niet
  verschuiven als er een monitor verdwijnt. Kost ongeveer 1,7 kB, want elke
  trigger draagt een tekstbuffer.
- Hoogstens **twee** monitorwaarschuwingen tegelijk. De wachtrij heeft vier
  plaatsen en de batterij gebruikt er al twee; zou er meer tegelijk mogen, dan
  zou een echte batterijwaarschuwing stil overgeslagen worden. Wordt een
  waarschuwing overgeslagen omdat de rij vol is, dan blijft de trigger
  onaangeroerd en probeert de volgende leesronde het opnieuw — er gaat niets
  verloren.
- Monitorwaarschuwingen zijn **lage** prioriteit, dus één poging per contact in
  plaats van vier.

**Deze waarschuwingen zijn gebouwd maar nooit op het toestel afgegaan.** Zie
[openstaand.md](openstaand.md); dat is een van de belangrijkste openstaande
punten.

## Het scherm

Het OLED-scherm is dat van upstream `simple_sensor`: opstartlogo, daarna de
gewone MeshCore-schermen, met een automatische uitschakeling na 20 s. Er is voor
MeshUptime niets aan toegevoegd — de beheerweg is de webinterface, de CLI en het
mesh.

## Bouwen en flashen

MeshUptime is een **zelfstandig PlatformIO-project**; MeshCore is een gepinde
git-submodule onder `firmware/vendor/MeshCore`. De volledige uitleg — beide
bouwwegen, beide envs, de tag-matrix in de CI en waarom de patch via een pre-build
hook loopt — staat in [bouwen.md](bouwen.md).

    git submodule update --init firmware/vendor/MeshCore
    cd firmware
    python -m platformio run -e meshuptime_room
    python -m platformio run -e meshuptime_room -t upload --upload-port COM4

Vervang `meshuptime_room` door `meshuptime` voor de sensor-variant (terugvalweg).

De WiFi-gegevens staan **niet** in deze repo (plaatshouders in
`firmware/platformio.ini`; zie [../firmware/wifi.ini.voorbeeld](../firmware/wifi.ini.voorbeeld)).
De ene patch op MeshCore,
[0001-sensor-manager-class-heltec-v3.patch](../firmware/patches/0001-sensor-manager-class-heltec-v3.patch),
zet de hardgecodeerde global `sensors` in `variants/heltec_v3/target.{h,cpp}` om
naar een macro met terugval — zonder die macro's bouwt de variant exact wat hij
ervoor bouwde, dus upstream-gedrag blijft ongewijzigd. In het zelfstandige project
wordt die patch door de pre-build hook automatisch aangebracht.

`upload` schrijft alleen de programmapartitie en niet SPIFFS: de identiteit in
`/identity/_main.id` en het instellingenbestand blijven dus staan. Wil je een
nieuwe sleutel, dan moet dat bestand of SPIFFS gewist worden — een aparte
handeling.
