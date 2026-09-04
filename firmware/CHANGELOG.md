# Changelog — MeshUptime

Eigen semantische versionering (`MESHUPTIME_VERSION`), los van de MeshCore-versie
(v1.17.0). De branding toont beide: `MeshUptime vX.Y.Z (by DinX) - MeshCore v1.17.0`.
Getoond op het OLED-bootscherm, in de web-voettekst en via het `ver`-commando.

Alleen de room-server-variant (`env:meshuptime_room`, build-flag `ROOM_SERVER_VARIANT`)
tenzij anders vermeld; de sensor-variant (`env:meshuptime`) blijft de terugvalweg.

## v2.7.0 — statusverzoeken: "Status nu opvragen" werkt op elke repeater

Alleen de room-server-variant (`env:meshuptime_room`). Additief op v2.6.0; de
bewaking en de instellingenopvragingen blijven ongewijzigd.

**Waarom.** v2.6.0 haalde Home Assistant uit de keten voor *instellingen*opvragingen,
maar liet `refresh`-verzoeken (statusverzoeken) vallen. MeshManager zette de knop
**Status nu opvragen** daarom uit, met als reden "de poller voert
instellingenopvragingen uit en laat statusverzoeken vallen". Die reden is nu weg
doordat het gewoon werkt — op **elke** repeater waarvan deze node het wachtwoord
kent, ook zonder onze eigen firmware aan de andere kant.

**1. Een statusronde is een eigen LoRa-sessie.** Dezelfde aanpak als een
settings-job: **inloggen** via `RepeaterCli` (ook hier verplicht —
`MyMesh::handleRequest` wordt alleen bereikt via `onPeerDataRecv`, en dat vereist dat
de afzender in de toegangslijst staat; de anonieme weg kent alleen
LOGIN/REGIONS/OWNER/CLOCK), en dan **één `REQ_TYPE_GET_STATUS`** als
`PAYLOAD_TYPE_REQ`. De verzoekvorm is letterlijk die van
`BaseChatMesh::sendRequest(recipient, req_type, ...)`: `[0..3]` tag, `[4]` req_type,
`[5..8]` reserved 0, `[9..12]` random. De tag is niet decoratief — de repeater kaatst
hem terug in `reply_data[0..3]`, zodat het antwoord aan ons verzoek te koppelen is.
Dat is hier nodig omdat een **loginantwoord met hetzelfde `PAYLOAD_TYPE_RESPONSE`
binnenkomt**; ze worden door de staat (`RCLI_LOGIN` vs de nieuwe `RCLI_STATUS`) én
door die tag gescheiden.

**2. Retries mogen hier wél.** Een statusverzoek is een **leesactie**, dus de gewone
drie pogingen. Dat is het verschil met een muterend CLI-commando, waar een herhaling
het commando op de tegenkant opnieuw zou uitvoeren.

**3. Aflevering: `POST /api/v1/ingest`** (nieuw `PushTask` `KIND_INGEST`, in dezelfde
stijl als `KIND_REPCLI`/`KIND_POLL`: eigen ring en body-bouwer, dezelfde
host/token/DNS-cache/socketmachine). Body:
`{"repeater":{"pubkey_prefix":"<12 hex>"},"metrics":{...}}`. Geen `ts` — dan stempelt
de server met zijn eigen klok, en dat is hier de betrouwbaardere: de klok van de
doelrepeater kan ver weglopen (dat is juist een van de dingen die we langs deze weg
willen repareren).

De metrieknamen zijn het contract met `server/app/metrics.py`, met de omrekeningen
die daar gedocumenteerd staan: `bat` (mV/1000 → V), `uptime` (s/86400 → dagen),
`airtime` en `rx_airtime` (s/60 → minuten), `last_snr` (de struct draagt ×4 → dB), en
ongewijzigd `tx_queue_len`, `noise_floor`, `last_rssi`, `nb_recv`, `nb_sent`,
`recv_flood`, `recv_direct`, `sent_flood`, `sent_direct`, `flood_dups`,
`direct_dups`, `recv_errors`, `full_evts` (= `err_events`, dat in de struct nog
"was n_full_events" heet).

**Wat de firmware niet meldt, laten we weg** — geen nul verzinnen: op de server ís
een nul een meting. Concreet: `bat` blijft weg als de tegenkant 0 mV rapporteert
(geen accu / geen ADC), want 0,000 V zou een lege accu voorwenden.

**`airtime_utilization` en `rx_airtime_utilization` sturen we NIET**, ook al kent de
server die namen. Ze zijn daar **afgeleid**, geen opgeslagen meting: `db._UTIL_BASIS`
zegt "de node stuurt een oplopende airtime-teller (in minuten), en de benutting is de
helling ervan", en `computed_utilization()` voegt toe: *"Computed here instead of read
from the node because the meshcore-side figure resets on every Home Assistant
restart"*. De grafiek van die twee namen loopt via `_utilisatie_reeks()` en negeert
een opgeslagen waarde toch. Wat wij zouden kunnen sturen is een levensduur-gemiddelde
(airtime/uptime) — een **ander** getal dan de helling die de tegel toont, onder
dezelfde naam. Dus: wij leveren de tellers, de server rekent.

**4. Lukt het niet, dan melden we niets.** Mislukte login, drie keer stilte, of een
antwoord dat de plausibiliteitstoets niet haalt: een logregel en een teller
(`status_fail` in `/poller.json`), geen meting. `/api/v1/ingest` neemt metingen aan,
en een halve of verzonnen meting zou daar als echte waarde in de reeksen en grafieken
belanden. Ook een ronde die in `RepeaterCli` strandt (waar bewust geen callback
volgt) wordt geteld — "niets gemeld" mag nooit hetzelfde lijken als "niets gebeurd".

**5. De plausibiliteitstoets**, omdat de doelrepeater niet onze build hoeft te draaien
(JessaZH draait `v1.17.1-PS+filter+rollback` van dutchmeshcore). De bytes worden met
`memcpy` op **expliciete offsets** gelezen (nagerekend uit `struct RepeaterStats`,
`simple_repeater/MyMesh.h:43` — geen struct-cast: de buffer is niet gegarandeerd
uitgelijnd en een cast zou meeveranderen met onze eigen padding). Een fork die een
veld *toevoegt* aan het eind is onschadelijk; een fork die een veld *invoegt* of
*herordent* levert geldig uitziende getallen op de verkeerde plaats, en dat is aan de
bytes niet te zien. Daarom worden de velden met een bekend fysiek bereik getoetst:
accuspanning (0 of 1500–6000 mV), uptime (< 20 jaar), **airtime ≤ uptime** en
**rx_airtime ≤ uptime** (een radio kan niet langer gezonden hebben dan hij aan stond —
de scherpste toets, die precies een verschuiving pakt), TX-wachtrij ≤ 4096,
ruisvloer/RSSI −200…50 dBm, SNR −50…50 dB. Faalt er één, dan wordt het **hele**
antwoord verworpen.

**6. Capability-opgave.** De poll-URL meldt nu `?caps=settings,refresh` in plaats van
`?caps=settings`. Daarop zet MeshManager de knop vanzelf aan (`db.note_poller_seen`);
aan de site verandert niets — dat is met opzet zo gebouwd. Dit is de **ene** plek die
mee moet als er een soort bijkomt of wegvalt.

**7. Airtime en voorrang, ongewijzigd.** Eén sessie tegelijk: een statusronde en een
settings-verzoek delen dezelfde `RepeaterCli` en dezelfde clear-on-read-wachtrij, dus
een statusronde kan nooit een settings-verzoek laten sneuvelen. Komen beide soorten in
één poll binnen, dan worden ze na elkaar afgehandeld en wordt wat niet in de wachtrij
past geteld en gelogd, zoals voorheen. De pollerlus staat achteraan in `loop()`; de
bewaking gaat voor.

**Nieuw in `/poller.json`:** `refresh_seen` (statusverzoeken in de laatste poll; was
`refresh_dropped`), `status_ok` en `status_fail`. De GUI-kaart toont
"status N ok/M mislukt".

**Gewijzigde bestanden:** `RepeaterCli.{h,cpp}` (`RCLI_STATUS`-staat, `queueStatus`,
`sendStatusReq`, `parseStatus` + toets, stats-callback), `PushTask.{h,cpp}`
(`KIND_INGEST` + `queueRepeaterStats` + `buildIngestBody`, `?caps=settings,refresh`),
`Poller.{h,cpp}` (refresh in dezelfde wachtrij, `startStatus`, tellers),
`WebTask.cpp` (poller.json-velden + kaarttekst), `Branding.h` (versie).

## v2.6.0 — MeshManager-poller: Home Assistant valt uit de keten

Alleen de room-server-variant (`env:meshuptime_room`). De bewaking (monitoren,
alarmen, de push naar MeshManager, de webinterface) blijft **ongewijzigd**; dit is
een **toevoeging**.

**Sleutelresolutie zonder advert (toegevoegd bij de eerste inzet, 2026-09-04).**
MeshManager stuurt in een verzoek altijd 12 hex, en `RepeaterCli` lost die alleen
op via de buurtlijst — die na elke herstart leeg is tot de repeater weer
adverteert (JessaZH: om de ~2 uur). Zet daarom de **volle 64-hex sleutel** in de
doeltabel: `Poller::keyFor()` geeft die dan door in plaats van de prefix, en de
sessie start meteen. Een 12-hex doelingang blijft werken zodra de advert gehoord
is. De volle sleutel staat in elk ADVERT-pakket van de repeater (in MeshManager:
`packets.raw`, vanaf de positie van de 12-hex prefix, 64 tekens).

**Antwoord in meerdere pakketten (idem, 2026-09-04).** De eerste versie leverde
het EERSTE TXT_MSG-pakket meteen af als volledig antwoord. `RepeaterCli`
verzamelt nu tot vier stukken met hun tijdstempel, sorteert ze en levert pas na
3 s stilte af (`RCLI_CHUNK_GAP_MS`); de antwoordbuffer ging van 176 naar 400
tekens (PushTask volgt). Een lopende verzameling wordt nooit onderbroken door een
herhaling (die zou het commando op de tegenkant opnieuw uitvoeren). Bevinding
bij het testen op JessaZH: `filter count` geeft daar alleen de limiettabel (één
pakket van 138 B); de statusregel met tellers komt van het kale `filter`. Dat is
een MeshManager-kwestie (beide opvragen en samenvoegen), geen firmwarekwestie.

**Waarom.** MeshManager kon al opdrachten in een wachtrij zetten voor een repeater
die alleen iets anders over LoRa kan bereiken (`db.request_settings` /
`request_refresh`). Tot nu toe leegde **Home Assistant** die wachtrij: het polde
`/api/v1/commands`, sprak de repeater aan via een companion-node en pushte de
antwoorden terug — drie schakels (HA, de integratie, de companion) tussen twee
dozen op hetzelfde dak. v2.6.0 doet dat werk op de node zelf en haalt HA volledig
uit de keten.

**1. Pollerlus (`Poller.*`).** Elke `poll_secs` (config, standaard 30, minimaal 10)
een `GET {push.url}/api/v1/commands` met `Authorization: Bearer {push.token}` — het
**sensorpush-token**, dat de server sinds kort ook op deze route aanvaardt (geen
tweede geheim op de node). Antwoord (clear-on-read):

```
{"refresh":["<hex>",...],
 "settings":[{"prefix":"<hex>","params":["name","radio","cmd:filter count",...]},...]}
```

Omdat het **clear-on-read** is (`db.pop_settings_requests` wist de wachtrij bij het
uitreiken), moet alles wat binnenkomt afgehandeld of expliciet gemeld worden. De
poller polt daarom **alleen als zijn eigen wachtrij leeg is** (plaats om te
bewaren), en telt/logt wat er tóch niet meer in past (`POLLER_PENDING_MAX = 4`
repeaters).

**2. Param → commando, één sessie per settings-verzoek.** Exact de vertaling van de
oude HA-pusher (`pusher.py ~393`): `cmd:X` → stuur `X` letterlijk; elke andere
param `P` → `get P`. Het antwoord gaat terug onder de **oorspronkelijke**
parameternaam. `RepeaterCli` draait sinds nu een **JOB** van N commando's in **één**
sessie: **eenmaal inloggen**, dan de N commando's achter elkaar (met dezelfde
tussenpauzes en airtime-grenzen), niet N keer login. Elk antwoord (of het uitblijven
ervan) wordt per commando afgeleverd en via `PushTask::queueRepeaterSetting` naar
`POST /api/v1/repeater_settings` gepusht; **geen antwoord → `null`** ("gevraagd,
geen antwoord", zoals `routes_api.repeater_settings` `None` behandelt), nooit stilte.

**3. Wachtwoorden per doel.** Om als admin op een repeater in te loggen kent de node
diens beheerderswachtwoord uit een klein persistent tabelletje
(`/rep_targets.cfg`, cap 8): pubkey-prefix (12–64 hex) → wachtwoord, plus één
**standaardwachtwoord** als terugval. Beheer via de web-GUI (kaart *MeshManager-
poller* op het nodebeheer-tabblad) en `/repeater_targets.json` (GET) +
`/repeater/target` (POST). **Wachtwoorden worden nooit teruggelezen** — de JSON zegt
alleen "gezet: ja/nee". Onbekend doel zonder wachtwoord → het verzoek wordt niet
uitgevoerd, elke param gaat als `null` terug en er komt een logregel.

**4. `refresh`-verzoeken (`REQ_TYPE_GET_STATUS`, een ander protocol) worden in deze
versie NIET uitgevoerd** — ze worden gelogd als "niet ondersteund" en vallen weg.
Bekende beperking; zie `docs/werking.md`.

**5. Airtime & veiligheid — de dak-repeater mag nooit onbereikbaar worden.** Eén
sessie tegelijk (`RepeaterCli` is single-session); een nieuw verzoek wacht tot de
vorige klaar is; de pollerlus blokkeert nooit (alles stapsgewijs uit `loop()`, ná de
bewaking). **Geen automatische herhaling van muterende commando's**
(`RepeaterCli::isMutating` → één poging; een herhaling zou het commando opnieuw
uitvoeren). **Gevaarlijke commando's komen niet uit de wachtrij de lucht in**:
`clkreboot`, `reboot`, `erase`, `set radio/freq`, `poweroff`/`shutdown`, `start ota`
en alles met `prv.key` worden geweigerd (`null` gepusht + gelogd) — op afstand is
daar geen bevestiging voor te geven. In de praktijk stuurt MeshManager alleen
leescommando's plus `cmd:filter count`/`cmd:region`; deze zeef is de gordel voor het
geval iemand de `cli_params`-lijst uitbreidt.

**Onder de motorkap.** `PushTask` kreeg **`KIND_POLL`**: een niet-blokkerende
`GET`-poll langs exact dezelfde DNS-cache/connect/send/recv-machine als de pushes
(geen tweede HTTP-client naast mesh/wifi/webserver). De antwoordbuffer groeide van
512 naar 2560 byte (de commands-JSON is genest en groter). `queueRepeaterSetting`
aanvaardt nu `value == nullptr` → JSON `null`. Nieuwe endpoints: `GET /poller.json`,
`POST /poller` (aan/uit + interval), `GET /repeater_targets.json`,
`POST /repeater/target`, plus een poller-tegel op de hoofdstatuspagina
(`status.json` veld `"poller"`).

**Hoe aanzetten.** De poller staat standaard **UIT**. Op het nodebeheer-tabblad:
zet in de kaart *MeshManager-poller* het standaardwachtwoord (of een wachtwoord per
doel-prefix), vink **poller aan**, kies eventueel het interval, en klik *opslaan*.
De push-URL en het token zijn dezelfde als voor de sensorpush (tabblad *Instellingen*
→ `push.url` / `push.token`).

**Handmatige tweelingweg.** Dezelfde `RepeaterCli` bedient ook `POST /cli/remote`
(één opdracht naar één repeater, `@<pubkey>[:<wachtwoord>] <opdracht>` in de
CLI-console), met een `GET /cli/remote` voor de stand. Dat is een job van één
commando; de poller is een job van N. `clkreboot`/`reboot`/`erase`/`set radio` eisen
daar een expliciete `confirm=…` (uit de wachtrij komen ze helemaal niet).

**Nieuwe/gewijzigde bestanden:** `RepeaterCli.{h,cpp}` (job i.p.v. enkel commando,
+ `/cli/remote`), `Poller.{h,cpp}` (nieuw), `PushTask.{h,cpp}` (`KIND_POLL` +
`null`-value), `WebTask.{h,cpp}` (endpoints + GUI-kaart + statustegel),
`main_room.cpp` (bedrading), `Branding.h` (versie).

## v2.5.1 — instant companion-push + volledige companion-command-GUI + radio-GUI

Drie additieve uitbreidingen op de companion-hub; alleen de room-server-variant
(`env:meshuptime_room`). De multi-bot-kern, de companion-store en de `#LOC`-afhandeling
blijven ongewijzigd.

**1. INSTANT-PUSH van companion-locatie/val naar MeshManager.** Tot nu toe *pollde*
MeshManager `/companions.json` (tot ~1 min oud). Nu **duwt** de node: zodra een
companion-`#LOC`/val ontvangen én bewaard is (`handleBotDm` → `companionUpdateLoc` /
`companionRecordFall`), roept `RoomMesh::pushCompanionNow(idx)` de **bestaande**
`PushTask` aan met de volledige stand. `PushTask::queueCompanion(...)` zet die in een
eigen kleine ring en post hem — via **dezelfde** host/token/DNS-cache/socket-machine als
de sensorpush, maar naar een **nieuw pad** **`POST {push.url}/api/companion`**:

```
{"companions":[{"pubkey":"<64hex>","lat":<float>,"lon":<float>,
                "seen":<uint>,"fall_ts":<uint>,"fall_kind":"val|nomotion|sos|"}]}
```

`lat`/`lon` worden **weggelaten** als er geen locatie bekend is; `fall_ts:0` +
`fall_kind:""` als er geen val-event is. Companion-pushes gaan **vóór** de heartbeat en
verzetten de heartbeat-klok niet; ze delen de retry/queue (overloop → oudste valt eruit,
geteld in `lostCount()`). Fire-moment: **precies bij het opslaan** van een companion-#LOC.
Het poll-endpoint `/companions.json` blijft bestaan als **terugval**.

**2. Volledige companion-command-parity in de web-GUI.** Het companion-commandopaneel
dekt nu de **complete** set (parity met CLI/menu): `!status`, `!tunes`, `!rxps`,
`!gps on|off`, `!preset 1|2|3`, **volume per slot** (`!vol H|M|L <0-3>`, naast de globale
`!vol <0-3>`) en **allow** (`!allow add <64hex>` / `!allow del <prefix≥12hex>` /
`!allow list`) — naast de bestaande `!find`/`!findstop`/`!loc`/`!ping`/`!cfg`/`!mute`/
`!play`/`!tune`/`!quiet` en de volledige `!fall`-groep (on|off / sens / nomotion /
prealarm / target add|del|list / mm on|off / test / status). Alle companion-**management**-
commando's gaan via de **MGMT-bot** (`bot=BE-HSS-DinX-MGMT`, via `cmCmd` → `/bot/sendto`).

**3. Radio-instellingen in de GUI, achter bevestiging + waarschuwing.** Nieuwe knoppen
voor **freq / bw / sf / cr / tx-power** die `!radio <veld> <waarde> confirm` sturen —
**elk** achter een expliciete JS-`confirm()` met de waarschuwing dat dit *"de companion
van de mesh kan doen vallen (fysieke seriële recovery nodig)"*, plus een rood
`.rw`-waarschuwingskader. Ook een **`radio show`**-leesknop. (De companion-firmware
implementeert `radio … confirm` al.)

## v2.5.0 — meerdere bot-identiteiten (alert-bot + companion-MANAGEMENT-bot)

De enkele notifier-bot is veralgemeend naar **N onafhankelijke bots** (`MAX_BOTS = 4`),
zodat één bot het alarmverkeer draagt en een tweede het companion-**MANAGEMENT**-verkeer.
Alleen de room-server-variant (`env:meshuptime_room`).

**Multi-bot-kern (firmware).** Elke bot is een eigen `BotSlot` met een eigen persistent
**sleutelpaar**, **naam**, **aan/uit-vlag**, eigen **ontvangerslijst** + **zend-diagnose**
en de rol-vlag **`is_alert`** (precies één bot draagt de alert-rol). `onRecvPacket`
matcht de bestemmings-hash tegen **alle actieve bots**; `handleBotDm(bot, …)` weet welke
bot de DM ontving en ontsleutelt/antwoordt als díe bot (eigen `self_id`, recips en diag).
De companion-`#LOC`-afhandeling, de silent-accept van companion-replies en de
self-loopback-guard (tegen álle eigen node-identiteiten) gelden **per bot**. `dispatchAlert`
gebruikt de **alert-bot**.

**Bot #0 blijft ONGEWIJZIGD.** De bestaande alert-bot (`BE-HSS-DinX-Bot`, pubkey
`E0841BF7…AE177146`) laadt exact dezelfde sleutel (`/bot_id`) en ontvangerslijst
(`/bot_recips`) en houdt de alert-rol. Bij een upgrade van v2.4.0 (nog geen `/bots.cfg`)
wordt automatisch een **tweede** bot **`BE-HSS-DinX-MGMT`** aangemaakt in slot #1 met een
**nieuw** sleutelpaar (bedoeld voor companion-MANAGEMENT).

**Additieve persistentie.** Bot #0 → `/bot_id` + `/bot_recips` (formaat `#MUBOT1`,
onveranderd). Bot #i>0 → `/bot_id_i` + `/bot_recips_i`. De per-slot config
(bezet/rol/actief/naam) staat los in **`/bots.cfg`** (`#MUBOTS1`). De identiteitsopslag
en `/companions.cfg` worden niet aangeraakt.

**Endpoints.** `/bot/sendto`, `/bot/post`, `/bot/recipient`, `/bot/advert`, `/bot/diag`
en `/bot.json` nemen nu een optionele **`bot=<idx-of-naam>`** (default: de alert-bot, zodat
bestaand gedrag behouden blijft); de **companion-GUI** stuurt haar `!`-commando's via de
**MGMT-bot**. Nieuw **`/bots.json`** geeft álle bots (idx, naam, **pubkey**, rol, #ontvangers)
zodat MeshManager de MGMT-pubkey kan oppikken. Nieuw **`/bot/manage`** (add/rename/enable/
del/setalert).

**Web-GUI "Bots".** Het Bot-tabblad toont nu een **botlijst** (naam, pubkey-prefix,
#ontvangers, rol/aan-uit) met **selecteren**; een nieuwe bot **toevoegen** (genereert een
sleutel), **hernoemen**, **aan/uit**, **wissen** (nooit de laatste/alert-bot) en de
**alert-rol** zetten. De ontvangers-, zend-diagnose- en handmatige-DM-secties werken op de
**gekozen** bot. Zelfde decluttered "?"-stijl.

## v2.4.0 — companion-hub op de node (beheer + locatie), werkt ook zonder MeshManager

Companions (T1000-E e.d.) worden nu **rechtstreeks op de node** beheerd en gevolgd,
zodat aansturen en locatie-opvolging blijven werken **ook als MeshManager plat ligt**.
De bestaande notifier-bot ("BE-HSS-DinX-Bot") is de zendweg.

**Companion-store (firmware).** Een persistente lijst in **`/companions.cfg`** (cap
**16**), elk `{ pubkey[32], naam[24], last_lat, last_lon, last_seen }`. Additief
opgeslagen (regelformaat `c <pubkeyhex> <lat> <lon> <seen> <naam>`, header `#MUCOMP1`);
de identiteitsopslag wordt niet aangeraakt. Er wordt niets voorgeseeded. Add/edit
(zelfde pubkey → naam bijwerken, locatie blijft) / delete-op-prefix.

**Web-GUI-tab "companions".** Zichtbaar op room-nodes. Toont per companion naam,
pubkey-prefix en laatst bekende locatie + ouderdom. Toevoegen/wijzigen met naam +
64-hex pubkey (kiezer uit gehoorde contacten of plakken). Een **commandopaneel** met
knoppen die het bijbehorende `!`-commando als **bot-DM** naar de gekozen companion
sturen (via het bestaande `/bot/sendto`-pad): **Find** (`!find`), **Stop-find**
(`!findstop`), **Locate** (`!loc`), **Mute** (`!mute on|off`), **Volume**
(`!vol <0-3>`), **Tune-per-ernst** (`!tune <H|M|L> preset <naam>`), **Quiet**
(`!quiet …`), **Config** (`!cfg`), **Ping** (`!ping`), **Play** (`!play <naam>`).
Een **kaartje** (Leaflet + OSM-tiles van de CDN `unpkg.com`) tekent de posities
zodra er coördinaten zijn; **zonder internet** valt de GUI terug op `lat,lon`-tekst
+ een OpenStreetMap-link per companion. Alle uitleg zit achter de bestaande
"?"-declutter.

**Valdetectie als eigen instelgroep.** De companion draait valdetectie op hardware
en houdt het AAN; de node-GUI stelt het nu volledig bij via `!fall`-subcommando's
(alle via `/bot/sendto`, geen node-firmware erbij): aan/uit (`!fall on|off`),
MeshManager-koppeling (`!fall mm on|off` — de companion meldt z'n val/SOS ook naar de
MeshManager-server), gevoeligheid (`!fall sens low|med|high`), geen-beweging/dead-man
(`!fall nomotion <min>`, 0=uit), pre-alarm/annuleervenster (`!fall prealarm <sec>`).
De companion-firmware gebruikt een **doel-LIJST**, dus de GUI stuurt
`!fall target add <64hex>` / `!fall target del <prefix>` (≥12 hex) /
`!fall target list` (kiezer uit gehoorde contacten of plakken, met hex-check).
Verder **test** (`!fall test` — start de pre-alarm nu, annuleerbaar, achter een
bevestiging, zonder echt te vallen) en **status** (`!fall status` — de companion
rapporteert z'n huidige val-config terug als DM).

**#LOC-locatierapporten + val-events + geen "onbekend"-bounce meer (firmware).** Een
companion is een apparaat dat wij aansturen; z'n inkomende DM's zijn
**antwoorden/rapporten** ("play: coin", "Pong", een `#LOC`-rapport), nooit commando's
aan ons. In `handleBotDm` (RoomMesh.cpp) draait een DM van een **bekende companion**
daarom nooit het commando-/"onbekend"-pad meer:

- begint de DM met **`#LOC <lat>,<lon>`** (decimale graden) → parse en werk
  `last_lat`/`last_lon`/`last_seen` van die companion bij. Draagt de tekst óók een
  **val-merkteken** — `(val)`, `(geen beweging)` of `(SOS)` (hoofdletterongevoelig) —
  dan wordt bovendien een **val-event** vastgelegd: `fall_ts` (= nu) en `fall_kind`
  (`val` / `nomotion` / `sos`). SOS wint van val wint van geen-beweging. Zo ziet de
  MeshManager-poller aan de oplopende `fall_ts` een NIEUWE val en kan hij escaleren;
- **elk ander** companion-antwoord → **stil aanvaard** (geen tekstantwoord).

In beide gevallen wordt wel op transportniveau ge-ACK't zodat de companion niet blijft
herzenden. Dit stopt meteen de spammy "onbekend commando"-bounce op companion-replies.
Het val-event wordt persistent bewaard (een `f`-regel in `/companions.cfg`, ADDITIEF
achter de `c`-regel; een oudere parser slaat 'm over).

**HTTP `/companions.json` (auth).** `{ "max": N, "companions": [ { name, pubkey, lat,
lon, seen, fall_ts, fall_kind } … ] }` — `lat`/`lon` ontbreken zolang er geen locatie
is, en `fall_ts`/`fall_kind` (`"val"`/`"nomotion"`/`"sos"`) alleen als er een val-event
is; zo kan MeshManager de companion-lijst + laatste posities + val-events pollen en op
een nieuw val-event escaleren. Auth zoals de andere endpoints; alleen publieke pubkeys,
nooit geheimen.

Alleen de room-variant heeft companions; de sensor-variant laat de IWebNode-standaarden
staan (`webCompanionMax()==0`) en `/companions.json` geeft een lege lijst, `/companion`
antwoordt `501`.

## v2.3.11 — per-monitor ernst + ernst-emoji vooraan de alert-DM (companion-buzzer)

Een companion (T1000-E) die naast de node hangt, moet aan een **binnenkomende
alert-DM** kunnen zien hoe erg de storing is, zonder de tekst te lezen: hij kiest
zijn **buzzer-tune** op de eerste tekens. Daarvoor begint vanaf nu **elke alert-DM**
met een **ernst-emoji + spatie**:

- 🔴 (`F0 9F 94 B4`) — **hoog**
- 🟠 (`F0 9F 9F A0`) — **midden**
- 🟢 (`F0 9F 9F A2`) — **laag** en **elke herstelmelding** ("weer bereikbaar"),
  ongeacht de ingestelde ernst.

Voorbeeld: `🔴 hoas onbereikbaar (30s)` · `🟠 Batterij laag (3.55V)` ·
`🟢 hoas weer bereikbaar (na 2m)`.

**Alleen de DM krijgt de emoji.** De room-post (de tekst die in een room verschijnt,
voor mensen in de app) blijft schoon — de emoji is er voor de companion die de DM
parseert. Zo blijft de room-tekst leesbaar en verandert er niets aan wat mensen zien.

**Per monitor én per vaste bron instelbaar.** Elke monitor draagt een ernst-veld
(`severity`, H/M/L); ook de vaste bronnen (netvoeding, wifi, batterij-kritisch,
batterij-laag, test) hebben er een. **Standaarden:** netvoeding / wifi / monitor-down
/ batterij-kritisch = **hoog**, batterij-laag = **midden**. Herstel is altijd laag.
Nieuwe monitors starten op **hoog** — de veiligste, meest opvallende stand. Een
bestand van vóór deze versie leest elke monitor als **hoog** in (geen stille
verlaging na een update): `MON_SEV_HIGH == 0`, dus een ontbrekend veld valt op hoog.

**Drie instelwegen, één zeef.** De ernst gaat door dezelfde keuring en dezelfde
opslag (`/monitors.cfg`) als de rest:

- **web-GUI** — een `e:`-selector (🔴 hoog / 🟠 midden / 🟢 laag) naast de bestaande
  `a:`/`r:`/`s:`-velden in de bewerk-rij van een monitor; uitleg staat in de
  "?"-help onder de tabel.
- **CLI / DM** — `sensor set mon.<ch>.sev high|medium|low` (of `h`/`m`/`l`, `0`/`1`/`2`),
  `sensor set fa.<idx>.sev ...` voor de vaste bronnen, en `edit <naam> sev=high` over
  een bot-DM.
- **POST `/mon/alarm`** — een optioneel `sev`-veld (0 hoog / 1 midden / 2 laag) naast
  `am`/`rm`/`sn`; afwezig = ongewijzigd.

**Persistent, backward-compatibel.** De ernst staat als **laatste veld** op de `m`- en
`fa`-regels in `/monitors.cfg`, achter de bestaande velden, zodat een oude parser hem
negeert en een oud bestand door deze versie (met de veilige hoog-default) gelezen wordt.
`status.json` draagt de ingestelde ernst als `msv` (0/1/2), los van het bestaande
weergave-`sev` (ok/bad/warn/unk).

Het alert-dispatch-pad (`RoomMesh::dispatchAlert`) zet de emoji op het DM-tekst-bouwmoment
via `sevEmoji()`; de v2.3.7 `edgeLatched`/debounce-logica is ongemoeid gelaten (herstel
geeft `MON_SEV_LOW` mee, storing de ernst van de bron).

## v2.3.10 — bot beantwoordt de volledige commandoset via DM (gated op de recipientlijst)

De notifier-bot (`BE-HSS-DinX-Bot`) was tot nu een kleine mesh-diagnose-responder:
over DM kende hij enkel `ping`, `path` en `help`. Vanaf nu beantwoordt hij de
**volledige monitoring/admin-commandoset** via DM — maar **alleen** voor de
alert-ontvangers, d.w.z. wie met zijn **volledige pubkey** in de recipientlijst
(`_bot_recips`) staat. Dat is dezelfde lijst die de waarschuwingen krijgt.

**Open voor iedereen.** `ping`, `path` en `help`/`?` blijven ongewijzigd en open
voor elke afzender. Een niet-ontvanger die iets anders stuurt, krijgt exact de oude
weigering (`onbekend commando. stuur \`ping\` of \`path\``) — geen hint over de
commandoset.

**Gated + volle rechten.** Voor een ontvanger draaien de commando's met **volledige
admin-rechten**. handleBotDm bouwt daarvoor een tijdelijke `ClientInfo` (admin +
alarmrechten, geen room-scope) en hergebruikt dezelfde `DmCommands::renderReply`-kern
als het room- en DM-pad. Zo werken `list`/`ls`, `status`, `get <naam>`,
`add`/`edit`/`del` en de node-/netwerk-/bedien-commando's (`neighbors`, `wifi`, `sys`,
`history`, `dns`, `ping`, `port`, `http`, `scan`, `traceroute`, `checknow`, `mute`,
`snooze`, `reboot`, `ntp`, ...).

**Lange antwoorden + async.** Commando-antwoorden kunnen groter zijn dan één pakket
(bv. `list`/`status`) en gaan daarom **gechunkt** via `botSendTo()`. Een uitgestelde
net-diagnose (`dns`, `ping <host>`, `traceroute`, ...) antwoordt eerst "gestart...";
de uitslag komt **later als aparte bot-DM** terug (de room-routing van renderReply
wordt daarvoor geannuleerd, de bot levert zelf af via `botAdhocPoll()`). `help` toont
een ontvanger ook de extra commando's.

**Debuglogging.** Altijd-aan `[botcmd]`-regel op Serial (afzender-prefix, recip-vlag,
verb, lengte, async).

## v2.3.9 — zend-diagnose fijnregelbaar + uitleg-URL

Voortbouwend op v2.3.8, alles instelbaar op de Bot-pagina van de web-GUI:

**Per commando aan/uit.** De enkele schakelaar is een masker geworden: `ping`, `test`
en `path` zijn los aan te vinken (bit0/1/2). 0 = zend-diagnose helemaal uit. Een oude
`d 1`-regel in het configbestand wordt gelezen als "alle drie aan".

**Uitleg-URL bij `geen scope`** in drie standen:

1. **uit** — geen URL.
2. **inline** — tussen haakjes achter `geen scope 😞`.
3. **apart bericht** (standaard) — een losse, **geflooda** kanaalpost
   `Meer info over regions en scopes: <url>`, ~3× de normale antwoordvertraging later
   zodat hij niet met het antwoord zelf botst in de zendwachtrij. Flood is bewust:
   ook stations verderop moeten meelezen dat 1-byte en ongescoped niet meer de
   bedoeling zijn. Gaat alleen naar het kanaal waar het commando vandaan kwam; het
   DM-pad krijgt geen los bericht (daar zegt de inline variant al genoeg).

De URL verschijnt uitsluitend bij een **ongescopet** pakket — dat is het moment waarop
de uitleg iets toevoegt.

**Nooit een halve link.** Een antwoord mag hoogstens 160 tekens zijn (MeshCore's
`MAX_TEXT_LEN`). Past de inline-URL er niet volledig in, dan laat de bot hem *helemaal*
weg in plaats van hem af te kappen tot een kapotte link; de korte 👍/😞-oordelen krijgen
voorrang. Omdat een `path`-antwoord met repeaterlijst al tegen die grens zit, is
"apart bericht" daar de betrouwbare stand.

**Live ruimte-teller in de GUI.** Naast het URL-veld staat hoeveel tekens er per
commando nog inline in passen (`dfit` uit `bot.json`, berekend op het krappere
kanaal-pad inclusief de `<botnaam>: `-prefix). Nuttig bij een URL-shortener: je ziet
meteen of een verkorte link bij `ping`/`test`/`path` past of stilletjes wegvalt.

Endpoint: `POST /bot/diag` met `mask=0..7`, `urlmode=0..2`, `url=<tekst>` (elk veld
optioneel). Leeskant: `bot.json` velden `diag`, `durlmode`, `durl`, `durlmax`, `dfit`.
Persistent als `d <masker>` en `u <modus> <url>` in het ontvangersbestand.

## v2.3.8 — zend-diagnose (👍/😞) achter de ping/test/path-antwoorden

De ping/test/path-antwoorden van de bot (zowel DM als in-kanaal) melden nu achteraan
HOE de afzender zond, zodat zowel legacy- als correcte instellingen zichtbaar worden
in het kanaal zelf:

- ` | 2-byte 👍` / ` | 1-byte 😞` — de pad-hashgrootte van het inkomende pakket.
  1-byte betekent dat de afzender nog op `path.hash.mode 0` staat; delen van de mesh
  (o.a. DinX-Home) filteren die. De echte grootte wordt getoond (2 of 3).
- ` | scoped 👍` / ` | geen scope 😞` — gescopete flood (`TRANSPORT_FLOOD` mét
  transport-codes) versus een kale `ROUTE_TYPE_FLOOD`.

De twee oordelen zijn ONAFHANKELIJK: hashgrootte en scope zitten in verschillende
pakketvelden, dus elke mengeling komt voor en wordt ook zo getoond —
`| 2-byte 👍 | geen scope 😞` (modern gehasht maar ongescoped) net zo goed als
`| 1-byte 😞 | scoped 👍`. Beide duimen omhoog = niets aan te merken.

Detectie: `packet->getPathHashSize()` (topbits van `path_len`, ook bij zero-hop floods
gezet door `setPathHashSizeAndCount`) en `packet->isRouteFlood() && !packet->hasTransportCodes()`.
Twee bewuste randgevallen: een zero-hop **DIRECT** pakket draagt geen hash-grootte →
geen `1-byte`-oordeel; en `geen scope` geldt alleen voor floods, want een direct-geroute
DM floodt niet en heeft dus geen scope nodig. Beide vlaggen tegelijk kan.
Implementatie: één gedeelde `appendTxDiag()` in `RoomMesh.cpp`, aangeroepen na het
opbouwen van de reply (DM: alleen `ping`/`path`; kanaal: alle drie de commando's).

**Aan/uit in de GUI:** checkbox op de Bot-pagina ("verklik legacy-afzenders"), standaard
AAN. Leeskant `bot.json` veld `diag`; zetten via `POST /bot/diag` (form `enabled=0|1`).
Persistent als extra regeltype `d <0|1>` in het bestaande ontvangersbestand (`#MUBOT1`);
oude parsers slaan die regel gewoon over. De bot-antwoorden zelf blijven ALTIJD een
gescopete flood met 2-byte pad-hashes (`sendFloodScoped`, `path.hash.mode+1`) — juist
zodat iedereen in het kanaal de verklikker meeleest, ook wie zero-hop zond.

## v2.3.7 — alle alert-types onder één gegrendelde kantelaar

De grendels die de vaste kanalen (netvoeding/wifi) in v2.3.5 kregen, golden nog NIET
voor de ping-monitors en de batterij-alerts. Die draaiden in `main_room::onSensorDataRead`
op de **kale `edge()`** en vertoonden daardoor exact dezelfde fouten die de vaste kanalen
vóór v2.3.5 hadden — fysiek gemeld op de monitors mon.5/mon.6/mon.9:

> "UDM_Pro weer bereikbaar na 2u21 — dit is een SIMULATIE, geen echte storing", verscheen
> terwijl de monitor juist WEGVIEL.

Drie symptomen in één melding, met drie oorzaken:

1. **Vals herstel op het verkeerde moment.** `monitorsPaused()` (bv. onze wifi valt weg → we
   meten ons eigen netwerk niet meer) maakte de storingsconditie `false`. De kale `edge()`
   las dat als een overgang neer→op en vuurde "weer bereikbaar" — terwijl de dienst NIET
   terug was. Een pauze werd als herstel gelezen.
2. **Echte storing als SIMULATIE gelabeld.** `recoverAlertText()` merkt met `was_sim`; dat
   veld werd in het room-pad nooit netjes gezet/gewist en lekte naar echte events. Bovendien
   werden `monitorAlertText()` en `recoverAlertText()` (beide dezelfde statische buffer)
   in v2.3.6 als twee argumenten aan `edge()` gegeven — de tweede overschreef de eerste, dus
   de storingsmelding kon de herstel-/sim-tekst dragen.
3. **Rare duur (2u21).** In het room-pad wordt `monitorAlert()` niet aangeroepen, dus
   `down_since` bleef 0 → `recoverAlertText()` rekende `millis()/1000` = de **uptime** (≈2u21).

**FIX — unificatie.** De kale `edge()` én de losse `fixedEdge()` zijn vervangen door ÉÉN
getemplate-de, gegrendelde `main_room::edgeLatched()` die ALLE alert-types gebruiken
(batterij crit/laag, netvoeding, wifi, elke ping-monitor). Dezelfde grendels voor iedereen,
zodat een toekomstig alert-type ze automatisch erft:

- **Freeze bij niet-meetbaar/gemute/gesnoozed** — geen dispatch én de baseline niet
  bijwerken (een openstaande overgang blijft bewaard en vuurt alsnog bij hervatten). Een
  pauze is nooit "herstel". Voor monitors: `monitorMeasurable()` = geseed én niet gepauzeerd
  (of een forcering actief).
- **Herstel alleen na een echt GEMELDE storing** (`announced`-vlag per alert) **+ plausibele
  duur** (`recovery_ok`, ≥ `FIXED_MIN_RECOVER_MS`, nooit 0 s/sub-seconde — ook niet met
  `alert.debounce == 0` of bij een sim).
- **SIMULATIE-merk alleen bij een echte forcering.** `monitorNoteDown()/monitorNoteClear()`
  zetten/wissen `was_sim`/`was_stale` op de huidige stand; een echte storing draagt nooit
  het merkteken, een sim-lek naar een latere echte storing kan niet meer.
- **Sane duur.** `monitorNoteDown()` legt het echte beginmoment vast (`down_since`), dus de
  "na X"-duur is de onbereikbaarheidsduur, niet de uptime.
- **Lazy teksten.** De helper haalt in de vurende tak precies één tekst op — geen aliasing
  meer van de gedeelde `s_alert_buf`.

**Batterij: Schmitt-hysterese + debounce.** De batterij-alerts (`< 3,4 V` crit, `< 3,6 V`
laag) waren kale drempelvergelijkingen die rond de drempel konden flikkeren. Ze lopen nu
door dezelfde **gedeelde symmetrische debounce** (`MonitorSensors::debounceStep`, uitgefactord
uit `fixedRawTick`) met een Schmitt-band: crit aan < 3,40 V / weer op peil > 3,50 V; laag aan
< 3,60 V / > 3,70 V. Settle-drempel = `alert.debounce` (consistent met de reactietijd), plus
de opstart-genade (`fixedInBootGrace`) zodat de reboot-dip geen batterij-alarm geeft.

**Testalarm** (room-commando `test`) blijft een bewust stateless one-shot: geen edge,
geen herstel, geen duur, geen sim — geen van de bugs (a)–(f) is van toepassing.

Alleen de room-variant; de sensor-variant (`env:meshuptime`, `main.cpp`) gebruikt nog het
bestaande `monitorAlert()/recoverAlert()`-pad en is ongewijzigd. Debug-logging blijft:
`[fixedge]` (per edge-beslissing) en `[fixdiag]` (debounce-flips, nu ook batterij).

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
