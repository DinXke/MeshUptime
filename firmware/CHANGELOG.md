# Changelog — MeshUptime

Eigen semantische versionering (`MESHUPTIME_VERSION`), los van de MeshCore-versie
(v1.17.0). De branding toont beide: `MeshUptime vX.Y.Z (by DinX) - MeshCore v1.17.0`.
Getoond op het OLED-bootscherm, in de web-voettekst en via het `ver`-commando.

Alleen de room-server-variant (`env:meshuptime_room`, build-flag `ROOM_SERVER_VARIANT`)
tenzij anders vermeld; de sensor-variant (`env:meshuptime`) blijft de terugvalweg.

## v2.18.0 — de poller vraagt zijn doelen zelf uit

**Wat er stukging.** De zonnerepeater op het dak trok zijn accu leeg (3,55 V op
13/09 → 2,81 V op 15/09 00:00) en viel uit. Dat is geen ramp; wat wél een ramp was:
in MeshManager stonden er **drie** nodes tegelijk stil. Die repeater vroeg de andere
over LoRa uit en publiceerde hun cijfers via MQTT — hij was de koerier, en er was er
maar één.

**Waarom niets het overnam.** MeshManager vraagt uit zichzelf nooit een status op:
`refresh` komt alleen in de wachtrij als iemand op de knop drukt (`commanding.py`
noemt het letterlijk *manual status requests*). De regelmaat kwam volledig van die
ene node. Op deze node stond ondertussen de hele machinerie klaar — inloggen,
`REQ_TYPE_GET_STATUS`, de meting naar `/api/v1/ingest` — alleen keek er nooit iemand
op de klok.

**Nu wel.** Staat `auto_mins` op een getal, dan vraagt de node **elk doel** hoogstens
eens per zoveel minuten zelf uit. Elk doel, niet de hele lijst per interval: met vier
doelen en een uur gaat er gemiddeld elk kwartier één ronde de lucht in, niet vier
tegelijk. Daar bovenop een halve minuut tussen twee rondes, want acht doelen die
toevallig samen aan de beurt komen zijn anders acht sessies achter elkaar.

**Wat het nooit doet**, en dat is waar de meeste regels aan opgaan:

- **voorkruipen** — een opdracht van de server gaat voor; staat er iets in de
  wachtrij, dan slaat de eigen ronde die beurt over;
- **twee sessies tegelijk** — `RepeaterCli` doet er één, en een eigen ronde die op
  een serveropdracht botst levert niets op;
- **zenden zonder bestemming** — zonder push-url kan de meting nergens heen, en dan
  is het zendtijd voor niets. Zendtijd is het enige op een mesh dat echt op kan;
- **een doel zonder wachtwoord uitvragen** — dat wordt een login die stil blijft en
  een mislukking in de tellers, elke ronde opnieuw.

**Uit na een upgrade.** De derde regel in `/poller.cfg` mag ontbreken; een bestand van
vóór deze versie laat de eigen rondes uit staan. Zendtijd hoort niet vanzelf te gaan
lopen omdat er nieuwe firmware op staat.

**Eén detail dat een bug was geworden.** `delTarget()` schuift de lijst op en laat de
oude staart staan. Zonder `auto_last = 0` bij het toevoegen erft een nieuw doel het
beurt-moment van een gewist doel — en dan blijft het uren stil om een reden die
nergens te zien is.

**Wat hiermee niet opgelost is.** De dakrepeater blijft de plek waar de ruwe
pakketstroom (`/rx`) en het pakketfilter thuishoren: die horen bij een antenne op een
dak, niet bij een node binnenshuis. Wat hier verhuist is het koerierswerk, niet het
oor. En de accu die leegliep heeft nog altijd geen alarm gegeven.

## v2.17.0 — `discover.neighbors`: de opdracht die ontbrak

**Het was geen leesactie maar een opdracht.** *Discover neighbours* in de companion-
app haalt geen lijst op: de app voert eerst het CLI-commando `discover.neighbors`
uit op de repeater, en die gaat dan zélf zoeken. Pas daarna vraagt hij de lijst op
met REQ 0x06. Dat commando kende deze node niet, het viel door naar de gewone CLI,
en de app kreeg een onbekend commando terug — wat hij toont als "de firmware van de
repeater is te oud". Drie theorieën verder was dat de hele oorzaak; de eigenaar
wees het aan.

**Hoe de zoekronde werkt** (control-pakketten, zoals upstream `simple_repeater`):

```
wij → iedereen   0x80 [filter=1<<REPEATER][tag:4][sinds:4]   zero-hop
ieder → ons      0x90|type [snr×4][tag:4][pubkey:32]         zero-hop
```

**Zero-hop, en dat is de kern.** Een buur is iemand die je rechtstreeks hoort. Het
antwoord draagt de SNR die de ander van óns mat, dus je weet niet alleen dát hij er
is maar ook hoe goed de verbinding is. Een willekeurige vertraging op het antwoord,
want op één zoekronde antwoordt de hele buurt tegelijk.

**Wij antwoorden ook.** Een repeater die andermans zoekronde negeert is een gat in
de kaart van iedereen. Met dezelfde voorwaarden als upstream: niet als het
doorsturen uit staat (dan zijn we geen repeater), en met een rem van een halve
minuut — dit is een onversleuteld verzoek van wie dan ook en het antwoord kost
zendtijd.

**De 0x06-lijst is bijgesneden.** Onze buurtlijst bewaart alles wat we horen — ook
telefoons, rooms en nodes op vijf hops — omdat de webinterface daar iets aan heeft.
Maar *neighbours* betekent hier wat upstream ermee doet: een REPEATER die we
rechtstreeks horen. Een telefoon op drie hops als buurrepeater tonen zou de kaart
van de eigenaar bederven, dus die vallen er nu uit.

**Wat hiermee nog niet bewezen is.** Of het buurtscherm vult. Dat hangt er nu
vooral van af of er in zendbereik een repeater staat die zelf antwoordt.

## v2.16.0 — de buurtlijst overleeft een herstart (en `[req]`-logging)

**Eerst meten, dan repareren.** Na twee gegokte oorzaken voor "de firmware van de
repeater is te oud" (de niveaubyte, daarna de anonieme verzoeken) is er logging
bijgekomen die opschrijft wát er binnenkomt en wát er teruggaat: prefix `[req]`,
altijd aan, stil op een node die niets gevraagd wordt.

De eigenaar drukte op *discover neighbours* en de node schreef op:

```
[req] verzoek 0x06 (len 12) van een beheerder
[req] buren: 0 bekend, 0 meegestuurd, antwoord 8 byte (gevraagd 10 vanaf 0, volgorde 0, 4 byte sleutel)
```

Drie keer, netjes beantwoord. **De app weigerde dus niets** — hij vroeg gewoon, en
kreeg een lijst met nul ingangen terug. Beide eerdere theorieën waren fout: het
niveau was niet het probleem en de anonieme verzoeken ook niet (die kwamen niet
eens langs).

**De echte oorzaak.** De buurtlijst leefde alleen in RAM, en de node was drie
seconden eerder herstart — deze is die avond vijftien keer geflasht. Een repeater
die bij elke herstart vergeet wie hij hoort, heeft aan dat scherm niets.

**De lijst gaat nu naar flash.** De 64 laatst gehoorde buren met hun volledige
sleutel, zodat een verzoek om meer dan een paar byte sleutel ook na een herstart
te beantwoorden is. Niet alle 200: dat is ruim twintig kilobyte voor ingangen die
niemand opvraagt. Lui geschreven — een minuut na de laatste wijziging, niet bij
elk advert, want op een druk mesh is dat een flashschrijfactie per advert. Bij het
opstarten gaat elke ingang door dezelfde `noteAdvert()` als een echt advert: een
tweede weg waarop een ingang ontstaat, is een tweede weg die kan afwijken.

**Wat hiermee nog niet bewezen is.** Of het buurtscherm in de app nu vult. Dat de
node antwoordt staat vast (het staat in de log, met de gevraagde parameters
erbij); of de app een niet-lege lijst ook toont, moet uit de app blijken. Wat wel
zeker is: met nul ingangen kon hij niets tonen, en dat lag aan ons.

## v2.15.0 - anonieme verzoeken beantwoorden

De companion-app bevraagt een repeater VOOR de login: ANON_REQ_TYPE_REGIONS (1),
_OWNER (2) en _BASIC (3, klok + eigenschappen). Deze node herkende ze sinds
v2.13.0 wel maar beantwoordde ze niet, en wie op zo'n vraag zwijgt kan voor oude
firmware doorgaan. Nu beantwoord, met het formaat van upstream (antwoordpad uit
het verzoek, 0xFF = flood) en een snelheidsrem van een antwoord per drie
seconden -- het zijn vragen zonder login, en elk antwoord kost zendtijd.

Achteraf bleek dit NIET de oorzaak van de klacht te zijn (zie v2.16.0): de app
stuurt deze verzoeken hier niet eens. Het hoort er wel: een repeater die erop
zwijgt is onvolledig.

## v2.14.1 - de app zei "firmware te oud": dat was een getal, geen versie

De companion-app weigerde "discover neighbours" met de melding dat de firmware van
de repeater te oud is -- terwijl 0x06 er sinds v2.14.0 in zit. Het lag niet aan
dat verzoek maar aan een byte in het LOGINANTWOORD: reply_data[12] draagt
FIRMWARE_VER_LEVEL, het protocolniveau los van elke versietekst. Upstream zet
dat op **1** in de room-server en op **2** in de repeater, en de app leest er zijn
mogelijkheden uit af -- in simple_repeater staat het er letterlijk bij:
REQ_TYPE_GET_OWNER_INFO 0x07 // FIRMWARE_VER_LEVEL >= 2.

Deze node erfde de 1 van de room-server. Zodra hij zich als repeater voorstelde
zag de app dus een repeater van niveau 1 en zette het buurtscherm uit voordat er
iets gevraagd werd.

**Niveau 2 melden mag pas als we het ook zijn.** Wat een repeater op niveau 2 na
de login extra kan is 0x06 (de buren, v2.14.0) en 0x07 (eigenaarsinfo). Die
laatste is er nu bij: versie, nodenaam en eigenaarstekst, met onze eigen branding
op de eerste regel omdat die de lezer meer zegt dan het kale MeshCore-nummer -- dat
staat er toch in.

**Alleen als repeater.** Stellen we ons als room voor, dan blijft het 1: dat is wat
upstream's room-server meldt, en een roomclient hoort geen repeaterbeloftes te
krijgen. Zelfde voorwaarde als bij het advert, de loginvorm en de statusvorm, zodat
wat we zeggen te zijn en wat we spreken nooit uit elkaar lopen.

**Niet meegenomen:** de drie ANON_REQ-types (regions/owner/clock). Die komen VOOR
de login, dus de client heeft ons niveau dan nog niet gezien en ze hangen niet aan
dit getal. Ze worden herkend en genegeerd in plaats van als loginpoging gelezen
(v2.13.0).

## v2.14.0 — het buurtscherm van de app, en een herstart die eerst antwoordt

**`REQ_TYPE_GET_NEIGHBOURS` (0x06).** Het buurtscherm van de companion-app werkt
nu ook op deze node. Het wire-formaat is letterlijk dat van upstream
(`simple_repeater`), want daar rekent de app op:

```
verzoek:  [0]=0x06 [1]=versie(0) [2]=aantal [3..4]=vanaf(uint16)
          [5]=volgorde [6]=lengte sleutelprefix [7..10]=blob
antwoord: [uint16 totaal][uint16 in dit antwoord]
          per buur: [sleutelprefix][uint32 seconden geleden][int8 snr x4]
```

Volgorde 0 nieuw→oud, 1 oud→nieuw, 2 sterk→zwak, 3 zwak→sterk; paginering via
`vanaf`/`aantal`; de resultaten passen in 130 byte en wat niet past volgt op de
volgende vraag. SNR gaat als SNR×4 over de draad, precies wat onze
`NeighbourEntry` al bewaart.

**Eén afwijking van upstream, met opzet.** Die zet een array van `MAX_NEIGHBOURS`
pointers op de **stack** en gooit er `std::sort` overheen. Bij ons is
`MAX_NEIGHBOURS` 200 — 800 byte stapel in een pakkethandler — en deze firmware
heeft al eens een stapeloverloop gehad van een grote buffer op een handlerstapel
(zie de les bij v2.2.0). Hier dus een **statische** indextabel van 200 byte en een
insertion sort: geen allocatie, geen stapel, en bij tweehonderd ingangen ruim snel
genoeg. De "seconden geleden" wordt op nul geklemd: in de reismodus kan de klok
verzet zijn sinds we die buur hoorden, en dan zou dat verschil als een enorm getal
doorkomen.

**De herstart na `travel on|off` antwoordt nu eerst.** `handleTravelCommand` riep
`_board->reboot()` rechtstreeks aan. Over serieel ging dat goed — daar print ik
het antwoord zelf vóór de herstart — maar over het **mesh** en over `POST /cli`
bouwt de aanroeper het antwoord pas ná `handleCommand()`. Dat antwoord vertrok dus
nooit: in de app zou `travel off` de node laten herstarten zonder een woord terug.
Juist bij dit commando is dat erg, want het antwoord vertelt hoe je terugkomt. Nu
zet het commando een tijdstip en herstart `RoomMesh::loop()` drie seconden later.

Gemeten vóór en ná: op de oude build gaf `POST /cli` met `travel on` een leeg
antwoord, op deze build komt
`reismodus AAN -- ... Herstart nu...` netjes terug en gaat de node daarna om.

**En die tekst klopt nu ook.** Hij zei "Terug: USB + `travel off`", maar sinds
v2.13.0 kan de CLI-console van de app over het mesh commando's sturen — en dat is
onderweg de bruikbare weg, want daar heb je geen kabel bij je. Beide staan er nu.

**Niet getest van deze kant.** Voor het buurtscherm is een MeshCore-client nodig
en die heb ik hier niet; het formaat is regel voor regel tegen upstream gelegd,
maar of het scherm in de app vult, moet uit de app zelf blijken. Wat wél getest
is: de node boot in de reismodus, blijft stil op de console, repeteert door
(`pad= 48d7` in het pakketarchief) en adverteert als
`repeater | BE-HSS-DinX-Mobile`.

## v2.13.0 — beheerbaar als een gewone repeater, ook in de reismodus

**Wat er stuk was, en waardoor.** Sinds deze node zich als repeater kan
voorstellen (v2.11.0, en de reismodus zet dat af fabriek aan) lukte inloggen over
het mesh niet meer. De oorzaak staat in de client, niet bij ons:
`BaseChatMesh::sendLogin()` kiest de vorm van het loginpakket op het
**advert-type** van het contact.

```
room:        [ts:4][sync_since:4][wachtwoord]     tlen = 8 + len
al de rest:  [ts:4][wachtwoord]                   tlen = 4 + len
```

Deze node las het wachtwoord altijd op offset 8 — de room-vorm. Kreeg hij de korte
vorm, dan vergeleek hij de **staart** van het wachtwoord met het echte: van
`Jramfa82-` bleef `a82-` over. Wie `allow.read.only` aan heeft staan kwam er nog
als gast in; wie dat uit heeft kreeg niets, en stil, want een foute login hoort
geen antwoord te krijgen.

**Nu worden beide vormen aanvaard.** Niet geraden op basis van hoe wij onszelf
adverteren — een client kan ons nog als het andere type in zijn lijst hebben van
vóór de omschakeling. We bieden allebei de posities aan en wat wint is de vorm
waarvan het wachtwoord **echt** klopt. Dat verzwakt niets: kloppen moet het in
beide gevallen. Bij de korte vorm is er geen `sync_since`, dus dan begint de
client zonder achterstand aan de roomposts — wat een repeater-client toch niet
opvraagt. Voor de korte vorm geldt dezelfde zeef als upstream gebruikt
(`data[4] == 0 || data[4] >= ' '` betekent "hier staat een wachtwoord"), zodat een
room-login zonder wachtwoord precies zo geweigerd blijft als voorheen.

**Het statusantwoord in de repeatervorm.** Dezelfde valkuil, een laag dieper. Op
`REQ_TYPE_GET_STATUS` antwoordt een room-server met `ServerStats` (52 byte) en een
repeater met `RepeaterStats` (56 byte). De eerste 48 byte zijn identiek; alleen de
staart verschilt — `n_posted`/`n_post_push` tegenover
`total_rx_air_time_secs`/`n_recv_errors`. Een app die een repeater verwacht leest
die staart verkeerd of verwerpt het hele antwoord; onze eigen `RepeaterCli` doet
dat laatste expliciet. Stellen we ons als repeater voor, dan antwoorden we nu ook
als repeater. Zelfde voorwaarde als bij het advert en bij de login, zodat wat we
zeggen te zijn en wat we spreken nooit uit elkaar lopen.

**Getypeerde anon-verzoeken worden niet meer als login gelezen.** De app stuurt
vóór een login soms een ANON_REQ met een *type* in plaats van een wachtwoord
(`ANON_REQ_TYPE_REGIONS` 1, `_OWNER` 2, `_BASIC` 3). Die las deze node als een
loginpoging; met `allow.read.only` aan leverde dat een gast-ingang in de ACL op
van iemand die alleen maar iets vroeg. We beantwoorden ze niet — maar we doen ook
niet meer alsof het een login was.

**`neighbors` doet eindelijk iets.** Dat antwoordde `not supported` terwijl de
node de buurtlijst gewoon heeft (de webinterface toont hem). De app beheert een
repeater met een CLI-console, dus daar komt dezelfde informatie nu langs:

```
0906 12.2dB 2hop 0min; 50C7 8.5dB 1hop 12min (+6 meer)
```

Eenheden voluit: `2h 0m` leest als twee uur nul minuten en dat is precies het
verkeerde. Het antwoord past in de CLI-buffer van 256 byte, dus het is een keuze
en geen volledige lijst — en dan hoort erbij te staan hoeveel er níet getoond is.

**Wat er NIET in zit.** Upstream beantwoordt daarnaast een binair
`REQ_TYPE_GET_NEIGHBOURS` (0x06) met sortering, paginering en instelbare
sleutellengte, plus `REQ_TYPE_GET_ACCESS_LIST` (0x05). Die zijn niet
geïmplementeerd: dat is een flink stuk protocol voor informatie die via de
CLI-console bij dezelfde gebruiker terechtkomt. Het neighbours-scherm in de app
blijft dus leeg; de console niet.

**Geverifieerd** (13 sep 2026): `neighbors` over `/cli` gaf eerst
`geen buren gehoord` (de lijst leeft in RAM en is na een herstart leeg) en na het
eerste gehoorde advert `0906 12.2dB 2hop 0min`.

## v2.12.0 — reismodus: alles uit behalve repeteren

Alleen de room-server-variant. Standaard **uit**; er verandert niets tot je hem
aanzet.

**Waarvoor.** Deze node is een room-server, een bot, een IRC-server, een monitor
en een poller — en dat kost stroom, want WiFi staat dan continu aan. Wie hem als
losse repeater wil meenemen heeft van dat alles niets nodig. Eén schakelaar op de
seriële CLI zet alles uit behalve wat een repeater nodig heeft.

```
travel            de stand
travel on|off     zetten; de node herstart meteen
```

**Wat er uit gaat:** WiFi (en daarmee de webinterface, de push naar MeshManager,
de poller, de IRC-server en de netwerkmonitors), de adverts van de andere rooms,
de sensor-nodes en de bots, de geplande kanaalberichten, de alarmmotor, en het
scherm. **Wat er blijft:** de radio, het **doorsturen**, en één advert — dat van
room 0, als repeater, met de naam uit de repeater-instelling (v2.11.0). Onderweg
is hij dus een repeater met een naam in plaats van een naamloze hop.

**Waarom een herstart.** WiFi en alles wat eraan hangt wordt bij het opstarten
opgezet. Halverwege afbreken laat brokken achter: open sockets, een webserver
zonder netwerk, een poller die in een time-out loopt. De schakelaar bewaart dus
alleen de keuze (`/travel.cfg`) en herstart; bij het opstarten wordt die keuze één
keer gelezen en dan klopt alles met elkaar.

**De weg terug is de USB-kabel.** In de reismodus is er geen webinterface — dat is
het punt. `travel off` over serieel en hij herstart als de gewone node. Dat staat
ook in het antwoord van het commando zelf, zodat je het niet pas onderweg ontdekt.

**De alarmmotor moest er apart uit.** Na de eerste versie bleef
`[fixedge] mon BEVROREN` doorlopen op de console: die regels komen uit
`onSensorDataRead()`, en dat is een callback van de **mesh** (de periodieke
sensoruitlezing), niet van `sensors.loop()`. Dat hij niets verstuurde kwam alleen
doordat een monitor zonder netwerk "niet meetbaar" is en de grendel uit v2.3.7 dan
zwijgt — de batterijalarmen zijn wél meetbaar en zouden dus wél de lucht in zijn
gegaan, vanaf een node waarvan de bots bewust uit staan. **Gevolg dat je moet
weten: in de reismodus is er geen batterijwaarschuwing.**

**De klok komt uit het mesh.** Zonder WiFi is er geen NTP, en de RTC begint na
elke herstart op de vaste terugval. Gemeten op de lucht: de adverts droegen alle
drie `advert_ts 1715770357` — 15 mei 2024. Clients die adverts op volgorde bewaken
(tegen replay) negeren zo'n advert van een node die ze al kennen, en dan is de
node weer een naamloze hop — precies wat de repeaternaam moest oplossen. Daarom
neemt de node in de reismodus de tijd over uit het eerste advert dat hij hoort,
onder vier voorwaarden: alleen in de reismodus, één keer per herstart, alleen als
zijn eigen klok nog onder de ondergrens staat, en alleen uit een tijd die zelf
plausibel is (boven de ondergrens, niet meer dan twintig jaar daarboven). Alleen
vooruit, zoals overal in dit project. Met een logregel, want een klok die stil
verspringt is erger dan een klok die verkeerd staat.

**Geverifieerd op de lucht** (12 sep 2026). Na `travel on` herstartte de node en
meldde de console `REISMODUS: geen wifi, geen web, geen bots, geen IRC, geen
monitors`; daarna bleef de console **stil**. De webinterface was onbereikbaar
(`http=000`). En hij repeteerde gewoon door: in het pakketarchief staat om
22:33:49 een REQ met `pad= 48d7` — de hash van deze node, dus die heeft hem
doorgestuurd. MeshManager zag hem intussen als
`{"name": "BE-HSS-DinX-Mobile", "node_type": "repeater"}`.

**Wat dit NIET is: diepe slaap.** MeshCore houdt de radio continu in ontvangst en
de CPU draait door. WiFi is verreweg de grootste verbruiker op een ESP32-S3 en die
is nu weg, plus het scherm — maar wat overblijft (CPU + LoRa in RX) is nog altijd
tientallen milliampère. Reken op een flinke verbetering, niet op een factor tien.
Een lagere CPU-klok zou daar nog een stuk af halen; dat is bewust niet in deze
versie meegenomen, want dat hoort gemeten te worden voordat het aan staat.

## v2.11.0 — adverteren als repeater, met een eigen naam

Alleen de room-server-variant. Standaard **uit**; er verandert niets tot je hem
aanzet.

**Het probleem.** Deze node stuurt pakketten door (`allowPacketForward` staat
open), en de hop die hij daarbij in het pad stempelt is de sleutel van
**room 0** — `Mesh::routeRecvPacket()` doet `self_id.copyHashTo(...)`. Maar die
sleutel adverteert zichzelf als *room*. In een app of in MeshManager komt die hop
dus wél voorbij terwijl er geen repeaternaam aan hangt: je ziet een room, of
niets. Gemeten vóór de wijziging: MeshManager kende `48d7aade232b` als
`{"name": "BE-HSS-DinX-Storingen", "node_type": "room"}`.

**De instelling.** Staat hij aan, dan stelt diezelfde sleutel zich in zijn advert
voor als `ADV_TYPE_REPEATER` met een naam die je zelf kiest. Alleen room 0 — de
andere rooms stempelen niets in een pad en blijven gewoon rooms. Een lege naam
valt terug op de nodenaam en anders op de roomnaam: er gaat nooit een naamloos
advert uit, want dat toont in elke app als "(unnamed)".

**Waarom dit een keuze is en geen verbetering-voor-iedereen.** Eén sleutel draagt
in het MeshCore-advert precies **één** type, en een app onthoudt per sleutel één
contact. "Allebei adverteren" bestaat dus niet: twee adverts voor dezelfde sleutel
laten het contact heen en weer klappen. Zolang dit aanstaat ziet een app deze
sleutel **niet meer als room** — de room blijft draaien en blijft joinbaar via zijn
QR/join-link, alleen het ontdekken via het advert valt weg. Die prijs staat in de
GUI, niet alleen hier.

**Waarom geen aparte sleutel voor de repeater.** Dat zou het probleem verplaatsen
in plaats van oplossen: de hops blijven dan de hash van room 0 dragen, dus de
naam van die tweede sleutel zou nog steeds niet bij de hop horen. Het enige dat
werkt is dat de sleutel die het doorsturen dóet zich ook zo voorstelt.

**Web:** stand in `/rooms.json` (`repadv`), zetten via `POST /repeater/advert`
(`on`, `name`) — POST-only, want dit verandert hoe de node zich aan de hele mesh
voorstelt en stuurt meteen een advert. Config in `/repeater.cfg` (`#MUREP1`).
Bediening in de Rooms-tab, onder de roomtabel.

**Geverifieerd op de lucht** (12 sep 2026). Aanzetten met een proefnaam, en 35
seconden later kende MeshManager dezelfde sleutel als
`{"name": "BE-HSS-DinX-RPT", "node_type": "repeater"}`. De ruwe advertbytes uit
het pakketarchief bevestigen het: zelfde pubkey `48d7aade232b54c8…`,
`node_type: repeater`. Daarna teruggezet op uit en gecontroleerd dat het contact
weer `room | BE-HSS-DinX-Storingen` werd.

**Wat die test óók liet zien.** Van de drie adverts die ik er doorheen stuurde,
haalde er één de overkant niet: LoRa kent geen ontvangstbevestiging en een los
zero-hop-advert kan gewoon wegvallen. Dat staat nu bij de instelling — met de
verwijzing naar de bestaande advert-knop bij room 0, die exact hetzelfde pakket
stuurt.

## v2.10.0 — de twee lijnen samen

De IRC-server en de geplande kanaalberichten zijn parallel ontwikkeld, op twee
takken, en droegen **allebei het nummer v2.9.0**. Dat nummer zegt dus niets meer:
welke v2.9.0 op een node staat hangt af van welke tak er geflasht is. De
samenvoeging krijgt daarom 2.10.0. De twee secties hieronder blijven staan zoals
ze geschreven zijn — allebei hebben ze echt op het toestel gedraaid.

Wat de samenvoeging zelf raakt:

- **`MAX_CHANNELS` is 16.** Beide takken liepen tegen dezelfde volle tabel aan en
  verhoogden hem los van elkaar (12 voor de aankondigingen, 16 omdat een
  app-import er negen meebrengt). Het hoogste wint; 16 dekt allebei.
- **Twee zenders op één radio.** De aankondigingen en de IRC-brug posten allebei
  in kanalen. De luchtbegroting van de IRC-server meet `getTotalAirTime()` van de
  radio, dus de aankondigingen tellen daar vanzelf in mee: staat er een reeks
  aankondigingen in de wachtrij, dan wijkt het chatverkeer daarvoor. Andersom niet
  — een aankondiging gaat altijd door, en dat hoort ook: die is gepland en de chat
  is dat niet.
- **`botSay()` erfde een fout die `sendChannelReply()` net kwijt was.** De
  IRC-kant is destijds van die functie gekopieerd, inclusief het stil weggooien
  van een `NULL` uit `createGroupDatagram()`. v2.9.0 (aankondigingen) repareerde
  dat aan die kant; hier is dezelfde reparatie in `botSay()` aangebracht.

## v2.9.1 — een kanaal per keer (waarom sommige kanalen het bericht niet kregen)

Gemeld door de eigenaar: "ik zie het in sommige kanalen wel en sommige niet". Dat
was geen RF-pech maar een fout in v2.9.0.

**Wat er mis was.** fireAnnounce() zette alle kanalen in EEN keer in de
zendwachtrij, met 4000 ms verschil in hun scheduled_for. Dat is geen zendritme
maar een wachtrij. De dispatcher rekent met een **luchtbudget**
(airtime_factor, op deze node 9,0 → een duty cycle van 1/(1+9) = 10 %), dus na
een flood van ~1,5 s mag de radio ~15 s niets. De pakketten kwamen dus bij een
radio die ze niet mocht sturen, en StaticPoolPacketManager::queueOutbound()
**gooit weg** wat niet meer in de wachtrij past ("send queue full, dropping
packet"). Vandaar: in het ene kanaal wel, in het andere niet.

En de log loog mee. "4 kanaal(en) verzonden" betekende "vier keer in de wachtrij
gezet" — niet hetzelfde, en het verhulde precies wat er gebeurde.

**Wat er nu gebeurt.** Eén kanaal per keer, aangestuurd vanuit
loopAnnounces(), met een instelbare pauze (standaard **30 s**, 5–900) tussen
twee kanalen. Elk bericht is dan het enige nieuwe pakket in de wachtrij en het
budget heeft tijd om bij te komen. Een lopende reeks gaat vóór een nieuwe: er
staat nooit meer dan één aankondiging per ronde in de wachtrij. Wat nog te doen is
staat alleen in RAM — na een herstart halverwege is de helft al verstuurd, en de
rest een uur later nasturen is vreemder dan hem overslaan.

De woorden zijn rechtgezet: log, JSON en GUI zeggen nu **"in de wachtrij"** waar
ze "verzonden" zeiden. Of een pakket de lucht in gaat beslist de dispatcher, en
dat weet de aanroeper niet.

**Geverifieerd op de lucht** (10 sep): twee privékanalen, nu sturen gaf
{"ok":true,"queued":2,"gap":30} en de log toont 15:54:19 (kanaal 8) en
15:54:52 (kanaal 9) — 33 s ertussen (30 s pauze plus de tick van 5 s).

## v2.9.0 — geplande kanaalberichten (de bot zegt ook zelf iets, op tijd)

Alleen de room-server-variant (`env:meshuptime_room`). Additief: de bewaking, de
opvragingen, de klok-job en het bestaande bot-gedrag blijven ongewijzigd.

**Waarom dit er is.** De bot antwoordt op wat hij *hoort*: iemand zet `ping`,
`test` of `path` in een kanaal en krijgt een antwoord met het oordeel over
pad-hash en scope erachter (`2-byte 👍 | geen scope 😞`). Daarmee bereikt dat
advies alleen wie de bot aanspreekt — en dat is precies niet de groep die het
nodig heeft. Dit is de andere richting: tot vier tijdstippen die zelf een bericht
in gekozen kanalen zetten.

**Twee losse lijsten, met opzet.** Waar de bot *meeleest* staat in de kanaaltabel
(`enabled`); waar hij *aankondigt* staat per aankondiging als masker over die
tabel. Een kanaal mag dus aankondigingen krijgen zonder dat de bot er meeleest en
omgekeerd. Het masker gaat over **alle** ingangen van de tabel en niet alleen de
ingeschakelde — anders zou het uitzetten van het meelezen stil de aankondiging
meenemen. Geverifieerd op de lucht met een kanaal dat op `enabled=0` stond.

**De klok is de harde voorwaarde.** Onder `TIME_FLOOR` staat de RTC op zijn vaste
terugval (15 mei 2024) en dan weten we de tijd niet: er gaat dan **niets** uit.
Een bericht op het verkeerde moment is erger dan geen bericht, en een node die na
een stroomstoring om 2 uur 's nachts alle vier de aankondigingen tegelijk
uitspuugt is een bot die niemand meer op zijn kanaal wil. De tijd is **lokale**
tijd (de ingestelde zone, zie `/time.cfg`); opslag en protocol blijven UTC.

**Nooit twee keer.** Het tijdstip matcht een hele minuut lang en de klokcontrole
loopt elke vijf seconden — zonder rem zou dat twaalf berichten geven. Er geldt een
ondergrens van vijf minuten tussen twee verzendingen van dezelfde ingang, en het
moment van de laatste verzending staat in het **bestand** en niet alleen in RAM:
een herstart binnen die minuut mag het bericht niet opnieuw versturen.

**Wat er nog bij hoort:**

- **`nu sturen`** per ingang (`POST /announce/test`): verstuurt meteen, wat de klok
  ook zegt, en laat het geplande tijdstip van vandaag staan. De enige manier om te
  zien of het aankomt zonder tot morgen te wachten. Met opzet geen GET — een link
  die een browser of linkchecker kan volgen mag geen bericht de mesh in zetten.
- **De tekst mag leeg**: dan gaat de ingebouwde adviestekst uit, met de uitleg-URL
  van de bot erachter (dezelfde instelling als die achter "geen scope" in de
  antwoorden staat — een tweede plek om hetzelfde te onderhouden is een plek die
  gaat afwijken).
- **De zendruimte wordt vooraf gerekend** (`announceRoom()`): de mesh-tekstlimiet
  min de `<botnaam>: ` die ervoor komt. De GUI toont daardoor exact wat er uitgaat,
  en een URL komt er **heel** bij of helemaal niet. Dat was geen theorie: de eerste
  proef logde *"tekst afgekapt van 147 naar 135 byte (naam kost 17)"* en die twaalf
  byte waren precies het staartje van de link.
- **Meerdere kanalen** krijgen het bericht met vier seconden tussenruimte, zodat de
  radio niet in één keer volgeblazen wordt.
- **`sendChannelReply()` geeft nu `bool` terug.** `createGroupDatagram` levert NULL
  bij een te lange tekst of een lege pakketpool, en dat werd genegeerd: het bericht
  verdween dan zonder spoor. Nu staat het in de log (`[ann]`), en dat geldt ook voor
  de bestaande bot-antwoorden die langs dezelfde functie gaan.
- **`MAX_CHANNELS` van 8 naar 12.** De tabel van de node zat vol, en dan kun je een
  kanaal om op aan te kondigen niet eens toevoegen. Vier ingangen kosten ~256 byte.

**Web:** `/announce.json` (GET; de lijst, de kanaalingangen mét hun index en de
lokale klok van de node), `/announce`, `/announce/del` en `/announce/test` (POST).
GUI op het bot-tabblad, onder de kanalen. Config in `/announce.cfg` (`#MUANN1`),
een eigen bestand — een nieuw veld in `bots.cfg` of `channels.cfg` zou op een node
die al draait door een bestaande regel heen lopen.

**Geverifieerd op de lucht** (10 sep 2026): een aankondiging gepland op 15:38 naar
een privékanaal waar de bot niet meeleest ging op 15:38:00 CEST de lucht in
(`[ann] ingang 0 op tijd: 1 kanaal(en) verzonden`), exact één keer; de instelling
en het "laatst verzonden"-moment overleefden een herstart; `nu sturen` gaf
`{"ok":true,"sent":1}` zonder dat tijdstip aan te raken; en de tekst kwam op 116
byte uit met de link er compleet in.
## v2.9.0 — een IRC-server op de node

Alleen de room-server-variant (`env:meshuptime_room`). Een gewone IRC-client
(HexChat, irssi, WeeChat, mIRC) verbindt met poort 6667 en praat daarna op het
mesh. Volledige uitleg: [`../docs/irc.md`](../docs/irc.md).

**De bot-slots dragen de identiteit, niet de rooms.** Dat is de kern en het is
geen nieuw mechanisme. `RoomMesh` droeg al drie soorten identiteit op één radio
via de `self_id`-wissel in `onRecvPacket()`: rooms (server-rol, anderen loggen
*in*), snodes (telemetrie) en bots (CHAT-identiteiten met een eigen sleutelpaar
die DM's kunnen *initiëren* en kanalen meelezen). Een IRC-gebruiker is een
chatdeelnemer, dus een bot. Er kwam geen tweede identiteitsstelsel bij; alleen de
IRC-kant erop.

Elk account houdt **permanent** hetzelfde bot-slot, ook uitgelogd. Je pubkey is je
adres op het mesh: kwam een slot bij logout vrij en ging het later naar iemand
anders, dan landden DM's die onderweg waren bij de verkeerde persoon en praatten
contacten die jou toegevoegd hadden opeens met een ander. Prijs daarvan: het
aantal accounts is hard begrensd op `MAX_BOTS`, en die staat in
`env:meshuptime_room` daarom op **8** in plaats van 4 — twee slots zijn al
vergeven aan de alert-bot en de MGMT-bot. Gemeten op een Heltec V3: zonder IRC
60,1% RAM, met IRC en 4 slots 61,0%, met 8 slots 62,8%. De server zelf kost
2 920 byte, elk slot ~1 475 byte.

**Nieuw in de firmware.**

| | |
|---|---|
| `IrcTask.{h,cpp}` | de server: synchrone `WiFiServer`, geen async-stack — dezelfde regel als WebTask |
| `RoomMesh::botSay()` | post in een kanaal onder de naam van bot *b*; `sendChannelReply()` kon dat niet, die zet altijd de alert-bot ervoor |
| `RoomMesh::channelFindBySecret()` | welke kanaalingang bij een ontsleuteld pakket hoort — op de 1-byte hash matchen wijst bij een botsing juist het verkeerde kanaal aan |
| `RoomMesh::ircResolveNick()` / `ircWhois()` | nick → pubkey via hex, companion-store of buurtlijst; meldt dubbelzinnigheid in plaats van stil de verkeerde te kiezen |
| `RoomMesh::saveBotIdentity()` | tegenhanger van `loadOrCreateBotIdentity()`, nodig voor BYOK |
| CLI `irc ...` | `list`, `user add/pass/del`, `key set` |

**Kanaaltekst gaat naar IRC vóór de commandofilter.** De haak in
`handleChannelText()` staat met opzet boven de vroege returns voor "geen
bot-commando": dat onderscheid gaat over waar de *bot* op antwoordt, terwijl een
IRC-gebruiker het hele gesprek wil zien.

**Airtime is de begrenzing, niet het protocol.** 3 s tussen twee verzendingen per
gebruiker, een gedeelde emmer van 6 berichten die per 10 s met één hervult, en
160 tekens inclusief het `"<naam>: "`-voorvoegsel. `JOIN`/`PART`/`QUIT`/`TOPIC`/
`NAMES` gaan nooit de lucht in, `NOTICE` evenmin, CTCP wordt genegeerd behalve
`ACTION`. Een geweigerde regel komt terug als `NOTICE` met de wachttijd erbij —
stilte laat iemand zijn regel opnieuw typen.

**Wat een nick in een kanaal niet bewijst.** Een group-pakket draagt geen
handtekening per afzender; `"<naam>: <bericht>"` is gewone tekst in de
versleutelde payload. Wie de kanaalsleutel heeft kan elke naam voorzetten, en de
sleutel van het publieke kanaal is algemeen bekend. DM's zijn wél gebonden (ECDH
+ MAC). Dat staat er zo bij in `botSay()` en in de docs, omdat de brug het anders
suggereert.

**BYOK kan, maar niet over IRC.** `irc key set <bot> <prv> <pub>` legt je eigen
sleutelpaar in een slot, zodat je op IRC dezelfde identiteit bent als in de app.
Alleen over serieel of de webinterface: IRC is onversleuteld, en een private key
die je in een chatvenster typt staat daarna in je client-log en op elke switch
onderweg. Wie hem zet, geeft de node bovendien de mogelijkheid hem permanent na te
doen — MeshCore kent geen revocation.

**De IRC-tab in de webinterface.** Beheert *gebruikers*: bot-slot en account in één
handeling, met terugdraaien als het account faalt — anders blijft er een verse
identiteit achter die al geadverteerd heeft en die niemand kan gebruiken. Nieuw:
`/irc.json`, `/irc/user`, `/irc/key`, `/irc/name`, en `webIrc*`/`webName*` in
`IWebNode`.

**Import van een MeshCore-app-config, geparsed in de BROWSER.** Zo'n export is
~126 kB met 350 contacten en past niet in het RAM van de synchrone webserver. De
pagina leest het bestand lokaal en post alleen wat de node kan opslaan: naam,
sleutelpaar (als je het aanvinkt), de aangevinkte kanalen en de eerste 64 contacten
als naamtabel. Radio-instellingen gaan er expliciet **niet** in — die zouden de node
van het mesh af zetten.

**De gedeelde naamtabel** (`MAX_NAMES` 64, ~3,3 kB) maakt `/msg <naam>` werkend voor
nodes die deze node zelf nooit hoorde adverteren. Node-breed en niet per gebruiker:
een pubkey is geen persoonlijk bezit, en per gebruiker zou 350 × 8 betekenen.
`ircResolveNick()` zet hem bóven de buurtlijst — hij is expliciet aangeleverd,
terwijl een advert-naam is wat een node over zichzelf beweert.

**Een geëmuleerde ledenlijst.** Een mesh-kanaal heeft geen aanwezigheid, en een leeg
`/NAMES` leest als "hier is niemand". Wie zendt krijgt een `JOIN`, na 45 min stilte
een `PART`. Dat is *gehoord*, niet *lidmaatschap*: meelezers verschijnen nooit, een
verzonnen naam wel, en de `PART` is een gok op stilte.

**Terugspoelen sinds je laatste sessie.** Een ringbuffer van 32 berichten (~6 kB);
per account de tijd van het laatste uitloggen, en alles daarna komt bij het inloggen
(DM's) of bij `JOIN` (het kanaal) alsnog binnen, met `[uu:mm]` ervoor. Bovengrens
12 uur. Puur RAM — een herstart wist hem, dit is geen logserver.

**`irc key set` mag wél over het web.** Eerst geblokkeerd, daarna teruggedraaid:
`/rooms/backup` levert de room- en snode-sleutels al over dezelfde onversleutelde
verbinding uit, dus de blokkade was een inconsistentie en geen bescherming. Het
staat nu achter een expliciete waarschuwing en een bevestiging, met de reden erbij:
de node kan je daarna permanent nadoen, en twee apparaten die dezelfde pubkey
adverteren laten de routecache van repeaters klappen.

**Antwoorden, in twee lagen.** MeshCore v1.17.0 heeft geen antwoordveld — de
tekstlaag is drie types en verder niets. Op de draad wordt een antwoord daarom een
leesbare quote (`>origineel.. tekst`, met de `.. ` altijd als scheider, zodat hij
deterministisch terug te vinden is); op de IRC-verbinding doen `message-tags` en
`server-time` het echte werk. Elk uitgeleverd bericht krijgt een `msgid`, een
`+draft/reply=<msgid>` uit de client wordt de quote, en een binnenkomende mesh-regel
die met een quote begint wordt teruggekoppeld aan de ringbuffer. Tags gaan alleen
over TCP en kosten geen airtime; clients zonder die capability zien precies wat de
MeshCore-app ziet. `CAP` houdt de registratie nu ook netjes vast tot `CAP END`.

**Public is geen hashtag-kanaal**, en de MOTD beweerde dat wel. Zijn sleutel is de
vaste `PUBLIC_GROUP_SECRET_HEX`; de naam doet er niet aan mee, en daarom staat hij
zonder `#` in de tabel. De `#` die IRC eist is daar cosmetisch. De code maakte het
onderscheid al goed (`channelSecretIsPublic()` kijkt naar de sleutel, niet naar de
naam) — alleen de tekst niet.

**Wat er richting de radio mag — nagerekend en aangescherpt.** De eerste
berichtenteller (6 per emmer, 1 erbij per 10 s) was een tempo en geen bescherming:
met de radio-instellingen van deze node kost een vol bericht ~1,6 s lucht, dus zes
per minuut is ~16 % zendtijd terwijl de sub-band 869,4–869,65 MHz op 10 % staat. Een
gekoppeld script of een sensor die elke drie seconden iets stuurt zat daarmee boven
de wettelijke grens, en de repeaters die het floodverkeer herhalen kwamen daar nog
bovenop. Nieuw:

- een **luchtbegroting op de gemeten zendtijd van de radio**
  (`Dispatcher::getTotalAirTime()`, dus inclusief adverts en alarmen — chat wijkt
  voor het echte werk), standaard 2 % over het laatste uur;
- de **aanloop na een herstart** telt niet mee: het boot-advert kostte gemeten ~9 s
  in twintig seconden, en zonder uitzondering lag chat daarna acht minuten plat
  zonder dat er iemand iets getypt had;
- **opmaak en controltekens eruit** — kleur, vetdruk, cursief, reset. Op een mesh
  betekenen ze niets en in de app zijn ze vuil. Blijft er niets over, dan geen
  pakket;
- **te lang wordt GEWEIGERD**, niet afgekapt: op het mesh is aan een halve zin niet
  te zien dat er iets miste. De melding landt in het kanaalvenster, noemt tekens en
  bytes apart zodra ze verschillen (accenten tellen dubbel), en `JOIN` zegt vooraf
  hoeveel er in dat kanaal passen;
- een **herhalingsrem**: exact dezelfde regel naar hetzelfde doel binnen 10 minuten
  wordt geweigerd. Dit is de rem tegen precies het soort koppeling dat eerder op dit
  mesh kanalen volpompte;
- **per pakket afrekenen** in plaats van per opdracht: een lange DM wordt in stukken
  geknipt en die tellen allemaal mee.

**Een ingebouwde `HELP`** met numerics 704/705/706, en onderwerpen KANALEN, DM,
ANTWOORDEN, AIRTIME, IDENTITEIT, LEDEN en GEMIST. Deze server doet een half dozijn
dingen die op een gewoon IRC-netwerk niet bestaan; die moet je kunnen opzoeken
zonder de repo open te hebben. In HexChat en irssi is `/help` een commando van de
client zelf, dus daar is het `/quote HELP <onderwerp>`.

**De naam van wie je citeert staat nu in de quote** (`>Jan: wat is de.. 869.618`).
In een kanaal draagt elk bericht al `"<naam>: "` als gewone tekst, dus de lezer zag
wie er antwoordde maar niet aan wie; vragen twee mensen iets soortgelijks, dan is
het antwoord niet meer thuis te brengen. In een DM blijft de naam weg — daar zijn
maar twee partijen en is hij verspilde airtime. `quoteLookup()` slaat de naamprefix
over en gebruikt hem om de juiste ring-ingang te kiezen.

**Geen TLS**, met opzet: naast mesh, WiFi en de webserver is er geen heap voor
TLS-sessies, en een halve TLS is erger dan geen. Vertrouwd LAN of VPN; 6667 niet
open naar het internet.

## v2.8.2 — dezelfde node in twee rollen (en waarom dat een sessie stil liet mislukken)

Alleen de room-server-variant (`env:meshuptime_room`). Een fix plus de logging die
hem zichtbaar maakte; niets aan de bewaking, de opvragingen of de klok-job.

**Het verschijnsel.** Elke CLI-sessie naar `BE-HSS-JessaZH.VIR02` eindigde in
`geen loginantwoord na 3 pogingen`, twaalf keer op rij. Tegelijk lag in het
pakketarchief van een node die het meehoorde het bewijs dat VIR02 **elk** verzoek
binnen een seconde antwoordde:

```
19:52:50 ANON_REQ  hops=0  ->cb   len=57
19:52:52 PATH      hops=0  cb->48 len=26
```

Dat tweede pakket is een `createPathReturn` met het loginantwoord als `extra`
erin. Naar `JessaZH.VIR` (dezelfde firmware, hetzelfde wachtwoord, dezelfde weg)
werkte de oefening wél. Met een verkeerd wachtwoord bleef VIR02 volledig stil, dus
het wachtwoord was juist en het verzoek kwam aan.

**De oorzaak.** VIR02 is **zowel een client van onze room-server als de repeater
waar wij client van zijn**. Dat is geen randgeval maar de normale gang van zaken op
één mesh. Zijn loginantwoord ontsleutelt daarom op zijn **ACL-ingang** en niet op de
rcli-kandidaat: de sleutel is dezelfde, want `rcliUseClientIdentity()` logt in met de
identiteit van room 0, en `Mesh::onRecvPacket` neemt de eerste kandidaat die de
MAC-controle overleeft. Het pakket belandde zo in de client-tak van
`onPeerPathRecv`, en die bewaart alleen het pad en kijkt verder enkel naar een
`ACK` — het `RESPONSE` erin werd stil weggegooid. Jessa stond niet in die ACL, dus
daar bleef alleen de rcli-kandidaat over en liep alles langs de bedoelde weg.

**De fix.** Is de al ontsleutelde afzender de node waarmee nu een sessie loopt, dan
krijgt de sessie het pakket eerst — en pas daarna loopt het door de gewone
clientmachinerie. In `onPeerPathRecv` (het pad-antwoord) en in `onPeerDataRecv` (het
gewone datagram, dat `onPeerData` opeist door `true` te geven).

Op de **volle sleutel** en niet op de 1-byte hash, en dat onderscheid is de moeite:
`matchesSrcHash()` biedt een *kandidaat* aan en een botsing kost daar hoogstens een
mislukte ontsleuteling, maar `isTargetPub()` leidt een **al ontsleuteld** pakket
ergens naartoe. Een botsing zou daar het pakket van een andere node als loginantwoord
laten lezen.

**`[rcli]`-diagnose (blijft staan).** `MESH_DEBUG` staat in deze build uit, dus van
de vier stappen tussen "de repeater antwoordt" en "de sessie loopt in zijn time-out"
was er geen enkel spoor. Deze logging maakt ze alle vier zichtbaar — pakket binnen,
kandidaat aangeboden, ontsleuteld, en wat erin zat — en print **alleen tijdens een
sessie**, dus ze kan blijven zonder de console vol te zetten. Het beslissende
fragment was:

```
[rcli] in: ptype=8 dest=48 src=CB route=0 hops=64 plen=20
[rcli] zoek: hash=CB acl-treffers=1 rcli=1 (slot=0 snode=-1 bot=0)
```

`acl-treffers=1`: er was een ACL-ingang met dezelfde hash, en die won.

**Geverifieerd op de lucht.** `ver` naar VIR02 geeft
`v1.17.1-PS+filter+rollback (Build: 14 Aug 2026)`; de vijf filtercommando's uit
MeshManager komen alle vijf beantwoord terug (filter aan, 118 pakketten weg op de
hoplimiet, beide limiettabellen en de kanaallijst).

## v2.8.0 — de klok van een repeater rechtzetten, als één job

Alleen de room-server-variant (`env:meshuptime_room`). Additief; de bewaking, de
instellingenopvragingen en de statusverzoeken blijven ongewijzigd.

**Waarom dit een node-job is en geen reeks serververzoeken.** De firmware van de
tegenkant weigert een klok **achteruit** te zetten:
`CommonCLI::handleCommand` antwoordt bij `time <epoch>` met
`(ERR: clock cannot go backwards)` zodra `secs <= curr` — dezelfde grendel zit op
`clock sync`. Loopt een node dus **voor** (JessaZH liep 18 minuten voor), dan is de
enige uitweg `clkreboot`: die zet de klok op `1715770351` (15 mei 2024, 20:50) en
roept meteen `_board->reboot()` — hij **antwoordt nooit**. Daarna moet er een
`time <epoch>` landen.

Tussen die twee is de node **onzichtbaar** voor iedereen die zijn oude, in de
toekomst liggende advert-tijdstempel onthield: een advert met een lagere tijdstempel
wordt overal weggegooid. Dat venster mag geen HTTP-ronde, geen clear-on-read-wachtrij
en geen serverstoring bevatten. Daarom staat de hele reeks in **één job op de node**:
zolang die loopt blijft `RepeaterCli::busy()` waar, houdt de job de enige sessie
bezet en kan geen ander verzoek ertussen komen.

**Het verloop.** MeshManager levert de opdracht als settings-parameter
`cmd:clockfix`. Die wordt bij het ontleden **uit de parameterlijst gehaald** en een
eigen job — het woord "clockfix" gaat nooit naar de repeater, die kent het niet.

1. `clock` lezen en de afwijking bepalen.
2. **Minder dan 60 s afwijking → niets doen.** Antwoord: "niets te doen", geen
   herstart. Een node herstarten voor niets is de duurste vorm van ijver. 60 s is
   ook de eerlijke ondergrens van de meting: `clock` antwoordt
   `"%02d:%02d - %d/%d/%d UTC"`, dus **zonder seconden**. We schatten de tegenkant op
   het midden van die minuut (+30 s), waarmee er ±30 s onzekerheid overblijft; een
   drempel korter dan een minuut zou op ruis staan.
3. **Loopt achter → alleen `time <epoch>`**, geen herstart. Vooruit mag altijd; dat
   is de hele winst van deze tak.
4. **Loopt voor → `clkreboot`.** Er wordt **geen antwoord verwacht**; stilte betekent
   hier "verstuurd" en niet "mislukt". De wachttijd op die stilte is daarom kort
   (`RCLI_CF_NOREPLY_MS`, 4 s) en niet de gewone 25 s: elke seconde in dit venster is
   een seconde waarin de node onzichtbaar is.
5. Daarna, na `RCLI_CF_REBOOT_WAIT_MS` (12 s), elke `RCLI_CF_RETRY_GAP_MS` (10 s)
   opnieuw **login + `time <epoch>` met een VERSE epoch**, tot `clock set` terugkomt
   of tot `RCLI_CF_WINDOW_MS` (3 minuten) om is. De login is tegelijk de
   "is hij al terug?"-proef, en zorgt dat we weer in zijn toegangslijst staan als die
   de herstart niet overleefde. 4 s + 12 s brengt de eerste poging op ~16 s na de
   herstart — precies waar de handmatige run op JessaZH raakte (15 s).
6. `clock` teruglezen, zodat het antwoord de **echte** nieuwe stand draagt en niet
   onze aanname.

**Het antwoord** is één zin voor een mens, afgeleverd onder `cmd:clockfix`:

```
OK - niets te doen: klok wijkt 12 s af (11:39 UTC), geen herstart
OK - klok gezet: 11:22 UTC (was 11:05, liep 1040 s achter, geen herstart nodig)
OK - klok gezet: 11:22 UTC (was 11:39, liep 1040 s voor; clkreboot + 2 pogingen)
MISLUKT - clkreboot verstuurd maar 'time' niet bevestigd na 15 pogingen; klok staat op mei 2024
MISLUKT - node weigert achteruit; onze eigen klok staat mogelijk fout
```

**Geen tweede clkreboot, ooit.** `_cf_reboot_sent` staat maar één keer per job aan, en
een `backwards`-antwoord ná een clkreboot leidt tot MISLUKT en niet tot een tweede
herstart. Twee clkreboots achter elkaar is precies hoe je een node op een dak in 2024
achterlaat. Ook `clkreboot` als **los** commando uit de wachtrij blijft geweigerd: de
BRICK-zeef in `Poller::paramAllowedFromQueue` staat er nog, dus "iemand zet clkreboot
in `cli_params`" blijft onmogelijk. Alleen deze job mag hem sturen, en alleen als stap
4 van het verloop hierboven.

**Geen lead op de epoch.** We sturen de epoch van het verzendmoment, zonder marge voor
de vluchttijd. Overschatten zou de node opnieuw **vóór** laten lopen, en dat is de
dure richting (die kost weer een clkreboot); een paar seconden achterlopen is
onschadelijk en wordt door de gewone `clock sync` van elke app rechtgetrokken.

**Airtime en voorrang.** Deze job kan minuten duren en dat is aanvaard: hij houdt de
enige sessie bezet, dus een ander pollerverzoek **wacht** — maar de **bewaking** loopt
door, want alles gebeurt stapsgewijs vanuit `loop()` en er wordt nergens gewacht. De
job heeft een eigen bovengrens (`RCLI_CF_MAX_MS`, 4 minuten) los van de gewone
job-cap.

**Zichtbaarheid.** Elke stap gaat serieel het logboek in, en `/poller.json` draagt nu
`clockfix_ok`, `clockfix_fail` en `clockfix_last` (de laatste uitkomst voluit). De
GUI-kaart toont "klok N ok/M mislukt" plus die zin. Dit is de handeling waarvan je
achteraf wilt kunnen zien wat er gebeurde.

**Capability-opgave.** De poll-URL meldt nu `?caps=settings,refresh,clockfix`.
MeshManager laat de knop alleen verschijnen als die vlag er staat, dus zolang deze
firmware niet draait verandert er niets aan de site.

**Wat er gebeurt als de node halverwege zélf herstart** (stroomdip, watchdog): de job
merkt dat als stilte. In stap 1/3/6 eindigt hij met MISLUKT en er is **niets**
gewijzigd (geen clkreboot verstuurd). Zit hij al in stap 5, dan is een herstart van de
tegenkant niet te onderscheiden van de herstart die wij zelf uitlokten — en dat hoeft
ook niet: de herkansingslus blijft gewoon inloggen en `time` sturen tot het lukt.
Herstart **onze eigen** node halverwege, dan is de job weg (hij leeft alleen in RAM) en
staat de doelrepeater op mei 2024 tot iemand de knop opnieuw indrukt; dat staat als
risico in het rapport en is de reden dat `clockfix_last` bewaard blijft tot de
volgende job.

**Gewijzigde bestanden:** `RepeaterCli.{h,cpp}` (`RCLI_JOB_CLOCKFIX` + `CfStep`,
`queueClockFix`, `cfBegin`/`cfOnAnswer`/`cfOnTimeout`/`cfFinish`, `parseClock` +
`civilToEpoch`), `Poller.{h,cpp}` (`cmd:clockfix` eruit filteren, `PEND_CLOCKFIX`,
`startClockFix`, `noteClockFix`, tellers), `PushTask.cpp` (caps),
`WebTask.cpp` (`/poller.json`-velden + GUI-kaart), `main_room.cpp` (de klok-uitkomst
ook aan de Poller melden), `Branding.h` (versie).

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
