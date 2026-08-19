# Ontwerp

## Uitgangspunt (19 augustus 2026)

De companion krijgt een **volledig nieuw doel**. Alles wat er nu op staat en
niets met bewaking te maken heeft, mag weg:

| Weg | Waarom het heap kost |
|---|---|
| Chatclient (`page.html`, 5708 B gzip) | paginabuffer bij elk verzoek |
| Berichtenring (32 plaatsen) | vast RAM plus de cache-logica |
| MQTT-publisher (`PubSubClient`) | eigen buffer én een open TCP-socket |
| Doorsturen van ruwe pakketten | wachtrij van 4 plaatsen |
| Contacten- en kanalen-UI | opbouw van JSON in geheugen |

**Wat blijft**, want dat is nu juist het doel:

- de MeshCore-radiostapel en de identiteit (`/identity/_main.id`)
- contacten, om een waarschuwing te kunnen adresseren
- kanalen, om een bericht naar een kanaal te kunnen sturen
- de synchrone webserver, als enige beheerweg

## Waarom dit de sleutel is

Voor het opruimen: **24.844 byte vrij, grootste blok 11.764 byte**. Dat is onder
de grens waarbij lwip in dit project eerder al halve antwoorden veroorzaakte.
Met de bovenstaande lijst weg komt daar ruimte bij, en die ruimte is het budget
voor alles hieronder. Het getal ná het opruimen is dus de eerste meting die
gedaan moet worden, vóór er functionaliteit bij komt.

## Onderdelen, in volgorde van kosten

1. **Eigen toestand** — USB-spanning versus batterij, en wifi weg. Vrijwel
   gratis: die gegevens komen al langs (`bat` staat in `stats.json`), er is
   alleen een drempel en een toestandsovergang nodig.
2. **Externe controles** — naam, adres, interval. Statisch begrensd aantal.
   Open vraag: ICMP of een TCP-verbinding op een poort. TCP is eerlijker over
   "de dienst leeft" en gebruikt de socketlaag die er al is; ICMP zegt "de host
   leeft" en vraagt een raw PCB. Beide kosten een socket — meten welke minder.
3. **Waarschuwen over het mesh** — naar een groep contacten per sensor, en naar
   een kanaal. Hergebruikt wat de companion al kan (`/send`).
4. **Webbeheer met inloggegevens** — één pagina, geen framework, geen CDN.
5. **Virtuele telemetriesensoren** — resultaten opvraagbaar door een andere
   node. MeshCore heeft daarvoor een telemetrieverzoek met CayenneLPP; de
   decoder daarvan zit al in de repeaterfirmware van dit project. Let op: voor
   een gast staat `perm_mask` op nul, dus wie mag opvragen is een echte
   ontwerpvraag en geen vinkje.

## Regels die niet onderhandelbaar zijn

1. Statisch alloceren; vast maximum aantal monitors, vaste buffers.
2. Geen tweede webserver.
3. Elke toevoeging meten, vóór en ná, op het **grootste vrije blok**.
4. De radio gaat voor: een bewakingsronde mag de mesh-timing niet ophouden.
5. De identiteit blijft. Back-up staat buiten de repo; de herstelweg is
   gedocumenteerd naast de dump.

## Nog te beslissen

- ICMP of TCP voor de externe controle (meting bepaalt dit).
- Wie telemetrie mag opvragen: alleen bekende nodes, of ook gasten.
- Hoe een waarschuwing zich gedraagt bij herhaling — één bericht per
  toestandsovergang, of herhalen zolang de storing duurt. Herhalen kost
  zendtijd op een gedeelde band.

---

# Gemeten randvoorwaarden (19 augustus 2026)

Alles hieronder komt uit de schone upstream-bron op tag `companion-v1.17.0`,
niet uit herinnering en niet uit onze eerdere maatwerkbuild.

## Hoe lang mag een bericht zijn

`src/helpers/BaseChatMesh.h:8`

    #define MAX_TEXT_LEN (10*CIPHER_BLOCK_SIZE)   // must be LESS than
                                                  // (MAX_PACKET_PAYLOAD - 4 - CIPHER_MAC_SIZE - 1)

Met `CIPHER_BLOCK_SIZE 16` is dat **160 byte**. De grens waar het commentaar naar
wijst is 184 - 4 - 2 - 1 = 177, dus 160 is een gekozen ronde marge en geen hard
protocolplafond.

Belangrijker staat in `composeMsgPacket` (`BaseChatMesh.cpp:422`):

    if (text_len > MAX_TEXT_LEN) return NULL;
    if (attempt > 3 && text_len > MAX_TEXT_LEN-2) return NULL;

**Een bericht langer dan 158 byte kan na de vierde poging niet meer opnieuw
verstuurd worden.** Dat knoopt twee eisen aan elkaar: lange antwoorden opknippen
en opnieuw proberen bij niet-bezorgen zijn geen losse functies. Wie op 160 knipt,
maakt precies de stukken die later niet herhaald kunnen worden.

**Daarom is de knipgrens 158 en niet 160.** Dat kost twee byte per stuk en levert
op dat elk stuk onbeperkt herhaalbaar blijft.

Let ook op `BaseChatMesh.cpp:496`: bij verzenden met voorvoegsel wordt de tekst
*stil* afgekapt op `MAX_TEXT_LEN - prefix_len`. Een stuknummer als `[2/5] ` gaat
dus van het budget af: zes byte voorvoegsel laat 152 byte tekst over.

## Bezorging: wat het protocol al weet

Bezorgcontrole hoeft niet verzonnen te worden.

- `composeMsgPacket` berekent `expected_ack`: vier byte SHA-256 over tijdstempel,
  pogingnummer en tekst, met de eigen publieke sleutel erbij
  (`BaseChatMesh.cpp:430`). De ontvanger rekent hetzelfde uit
  (`BaseChatMesh.cpp:242`) en stuurt het terug. Een ACK bewijst dus dat **deze**
  tekst aankwam, niet enkel dat er iets aankwam.
- Verzenden geeft `est_timeout` terug: hoe lang wachten redelijk is, gegeven het
  aantal hops. Niet zelf een getal kiezen, dit gebruiken.
- De companion heeft er al een melding voor: `PUSH_CODE_SEND_CONFIRMED 0x82`
  (`MyMesh.cpp:114`).
- `attempt` is twee bits (`temp[4] = (attempt & 3)`): vier pogingen, nul tot drie.
  De gevraagde "na 3 retries" valt exact samen met de eigen teller van het
  protocol, wat goed uitkomt, want daarboven is het protocol zelf op.

Een valkuil die de DM-opdrachten hieronder rechtstreeks raakt:
`BaseChatMesh.cpp:258` zegt over antwoorden van type `TXT_TYPE_CLI_DATA`
uitdrukkelijk dat er geen ACK verwacht wordt. Antwoorden we op een opdracht als
CLI-data, dan **weten we niet of het antwoord is aangekomen**. Antwoorden als
gewone tekst levert wel een ACK op. Bezorgcontrole op antwoorden vraagt dus om
`TXT_TYPE_PLAIN`.

## Opnieuw proberen na vier mislukte pogingen

Wat het protocol niet doet, is het later nog eens proberen.

- Per uitgaand bericht een plaats in een kleine statische tabel: bestemming,
  tekst, `expected_ack`, pogingnummer, moment van de volgende poging.
- Vier pogingen op protocolsnelheid, met `est_timeout` als wachttijd.
- Daarna oplopend wachten (1, 5, 15, 60 minuten) met een bovengrens. Dit is een
  gedeelde band; een node die eindeloos een onbezorgbare melding herhaalt is
  zelf een storing geworden.
- Een melding die na een lange pauze alsnog aankomt, hoort te zeggen hoe oud de
  waarneming is. "IP onbereikbaar" zonder tijdstempel, een uur later bezorgd, is
  misleidend.

## Kan de heap groter?

Ja. Dit is de belangrijkste vondst van vandaag.

Upstream houdt de companion in **drie aparte envs**:
`Heltec_v3_companion_radio_usb`, `..._ble` en `..._wifi`. Onze eerdere
maatwerkbuild zette WiFi en BLE *samen* aan. Dat verklaart de 24 kB grotendeels:
de BLE-stapel neemt bij initialisatie tientallen kB heap, naast de WiFi-stapel.
MeshUptime wordt beheerd over web, mesh en API en heeft BLE niet nodig.

Daarnaast drie statische posten die bij een chatclient horen en niet bij een
bewaker: `MAX_CONTACTS=350`, `MAX_GROUP_CHANNELS=40`, `OFFLINE_QUEUE_SIZE=256`.
Verlaagd naar 32, 4 en 16.

Wat **niet** kan: meer RAM erbij. Het bord is een ESP32-S3 zonder PSRAM
(`board = esp32-s3-devkitc-1`; de Heltec V3 draagt de S3FN8). Er valt niets bij
te schakelen, er valt alleen minder te gebruiken.

## Bewaking van buiten aanleveren (API)

Zelf pingen is niet de enige weg en waarschijnlijk niet de beste. Een bestaande
Uptime Kuma is beter in bewaken dan dit apparaat ooit wordt: die kent
herhaalpogingen, onderhoudsvensters en historiek. Wat hij niet kan, is een
melding bezorgen als het internet weg is.

Daarom een tweede, goedkopere weg: een **inkomende webhook**. Uptime Kuma stuurt
zijn melding hierheen, de node zet die om in een meshbericht en in een
telemetriewaarde. Dat kost een route en geen enkele socket voor uitgaand pingen,
en het is de juiste taakverdeling: Kuma weet *of* iets stuk is, deze node is de
enige die het nog kan vertellen.

Beide wegen blijven. De eigen controles zijn onmisbaar voor het geval dat Kuma
zelf onbereikbaar is, en dat is nu juist het geval dat je wil weten.

## Sensoren opvragen via een DM

Naast web en telemetrie ook opdrachten per DM, met 158 byte als harde vorm:

    list            alle sensoren, naam en toestand, in stukken
    get <naam>      een sensor met detail
    status          eigen toestand: voeding, wifi, uptime

- Antwoorden als `TXT_TYPE_PLAIN`, zodat bezorging bevestigd wordt.
- Opknippen op woordgrens waar dat kan, met `[n/m] ` ervoor: 152 byte tekst.
- Een `list` met veel sensoren wordt veel berichten. Er hoort een bovengrens op
  het aantal stukken, met een slotregel over wat er niet in paste. Twintig
  berichten uitzenden omdat iemand `list` typte, is zendtijd van iedereen.
- Wie mag dit? Alleen bekende contacten; dezelfde vraag als bij telemetrie, en
  nog te beslissen.

---

# Kan deze node tegelijk repeater zijn? (19 augustus 2026)

## Ja, en het zit er al in

`examples/companion_radio/MyMesh.cpp:485`

    bool MyMesh::allowPacketForward(const mesh::Packet* packet) {
      return _prefs.isRepeatEn();
    }

`allowPacketForward()` is een virtuele methode op `Mesh` en de companion
overschrijft die al. Er is een instelling `repeat.disable_fwd` met
`isRepeatEn()` / `setRepeatEn()`, en die is over de bestaande opdrachtinterface
te zetten (`MyMesh.cpp:1398`). Doorsturen is dus geen tweede firmware en geen
tweede identiteit: het is een vlag die er al staat.

## Maar het doorsturen van de companion is naïef

Dit is de eigenlijke waarschuwing. Vergelijk de twee versies.

De companion (`NodePrefs.h:82`) — let op de weggecommentarieerde regels:

    class RepeatPrefs : public ConfigSerializer {  // COPIED from CommonCLI (for now)
      uint8_t disable_fwd = 1;
      void structure() override {
        def("disable", disable_fwd);
        //def("f_max", flood_max);
        //def("f_max_uns", flood_max_unscoped);
        //def("f_max_adv", flood_max_advert);
        //def("loop", loop_detect);
      }
    };

De echte repeater (`simple_repeater/MyMesh.cpp:426`) doet zes controles:
`disable_fwd`, `flood_max`, `flood_max_unscoped`, `flood_max_advert`, een
regio- en transportcodecontrole, en lusdetectie in drie standen.

**De companion heeft geen van die zes.** De velden bestaan niet eens in zijn
struct. `repeat` aanzetten zoals het nu is, levert een repeater die alles
doorstuurt wat een echte repeater juist wegfiltert, inclusief floodlussen.

Dat is waar het zendtijdprobleem zit — en niet waar de vraag het vermoedde.

## Zendtijd: de rollen botsen niet, het doorsturen is de kostenpost

- De SX1262 heeft één modem. Ontvangen gebeurt hoe dan ook één keer; twee rollen
  op één radio verdubbelen het ontvangen niet. Dat is gratis.
- Zenden is de schaarse kant. Een bewakingsmelding is zeldzaam (alleen bij een
  toestandsovergang) en kort (maximaal 158 byte). In verhouding tot wat een
  repeater aan doorgestuurd verkeer uitzendt, is dat ruis.
- De 1%-duty-cycle op 868 MHz wordt dus door de repeaterfunctie opgegeten, niet
  door de bewaking. Wie zich zorgen maakt over zendtijd, moet naar `flood_max`
  en lusdetectie kijken, niet naar het aantal sensoren.

Eén echte samenloop blijft: als een melding klaarstaat terwijl de radio een
doorgestuurd pakket uitzendt, wacht de melding. Bij een storing wil je dat niet.
Vandaar de regel uit het ontwerp dat de radio voorgaat: de melding moet in de
wachtrij en niet blokkerend verzonden worden, en de bezorgcontrole hierboven
vangt op wat daardoor te laat komt.

## Twee identiteiten: dat is niet wat SIREN doet

Dit is een correctie op de aanname in de vraag, en het is nagekeken in de bron
van SIREN zelf.

- `firmware/examples/siren_room_server/MyMesh.cpp:260`: `self_id = rooms[0].id;`
- `firmware/src/Mesh.cpp` regels 58, 88, 142, 197: nog steeds
  `self_id.isHashMatch(...)`, precies zoals upstream. **Geen enkele aanpassing
  voor meerdere identiteiten in de routering.**
- `docs/replication-protocol.md`: *"Room 0 is the node identity layer and never
  carries chat posts"*.

SIREN heeft dus **één radio-identiteit** (room 0). De sleutels van room 1..N
leven boven de meshlaag: ze versleutelen kamerinhoud en vullen de `room_hash` in
de synchronisatieframes. De node antwoordt op het radioniveau op één adreshash,
niet op meerdere.

Een node die als twee aparte nodes op het mesh zichtbaar is, vraagt daarom een
aanpassing in `src/Mesh.cpp` op vijf plaatsen (`isHashMatch` bij dest-hash, bij
path-hash en bij flood), plus een keuze welke identiteit ECDH doet. Dat is een
kernpatch, en we weten uit MeshStats wat een kernpatch kost: hij moet bij elke
upstream-versie opnieuw worden aangebracht.

**Advies: één identiteit.** De node kondigt zich aan met één `ADV_TYPE_*`
(`CHAT=1`, `REPEATER=2`, `ROOM=3`, `SENSOR=4`) en doet alles: doorsturen,
bewaken, DM-opdrachten beantwoorden, telemetrie leveren. De prijs is dat hij in
de MeshCore-app in één lijstje staat en niet in twee. De opbrengst is geen
kernpatch.

Dit is wel een keuze die jij hoort te maken, want hij bepaalt hoe het apparaat
er voor iedereen op het mesh uitziet.

## Eén webserver voor beide rollen

Beheer van bewaking én repeaterinstellingen op dezelfde webserver, in tabbladen
of inklapbare secties. Dat volgt uit de ontwerpregel dat er geen tweede
webserver bij komt, en het is dezelfde vorm die de repeater-GUI in MeshManager
gekregen heeft.

Voorwaarde: de vier ontbrekende repeatervelden moeten dan eerst bestaan.
`flood_max`, `flood_max_unscoped`, `flood_max_advert` en `loop_detect` staan in
`CommonCLI.h:141` klaar en zijn in de companion enkel uitgecommentarieerd — die
overnemen is de eerste stap, en zonder die stap is een repeaterknop in de
webinterface een knop die schade doet.

---

# Twee identiteiten, of de juiste rol? (19 augustus 2026)

De vraag was of één identiteit die zowel bewaakt als doorstuurt "fout loopt op
het mesh" en de MeshCore-standaard niet volgt. Nagekeken in upstream, en het
antwoord is het omgekeerde van wat de vraag vermoedde.

## Elk niet-companion voorbeeld stuurt door met één identiteit

| voorbeeld | kondigt zich aan als | stuurt door |
|---|---|---|
| `simple_repeater` | `ADV_TYPE_REPEATER` | ja, zes controles |
| `simple_room_server` | `ADV_TYPE_ROOM` | ja, dezelfde controles |
| `simple_sensor` | `ADV_TYPE_SENSOR` | ja, `disable_fwd` + `flood_max` |
| `companion_radio` | `ADV_TYPE_CHAT` | ja, maar zonder één enkele controle |

Doorsturen is in MeshCore **niet gebonden aan het advert-type**.
`allowPacketForward()` wordt geraadpleegd wat een node ook aankondigt. Een
identiteit die doorstuurt én een toepassing draait, is dus niet de uitzondering
maar de regel: drie van de vier voorbeelden doen precies dat.

**Wat wél fout is, is een verkeerd advert-type.** Een node die `ADV_TYPE_CHAT`
aankondigt en andermans verkeer doorstuurt, is oneerlijk tegenover het mesh:
niemand ziet dat hij infrastructuur is, niemand kan hem om statistieken vragen.
Dat is de echte afwijking van de standaard — en de oplossing is niet een tweede
identiteit, maar het juiste type aankondigen.

Twee radio-identiteiten hebben in upstream **geen enkel voorbeeld**. SIREN doet
het ook niet (zie de vorige sectie). Het vraagt een patch in `src/Mesh.cpp`.

## simple_sensor is bijna letterlijk MeshUptime

De belangrijkste vondst. `examples/simple_sensor` heeft al:

- `ADV_TYPE_SENSOR` — precies wat dit apparaat is: het biedt sensoren aan
- doorsturen mét beveiliging (`SensorMesh.cpp:304`), dus de repeaterstap achteraan
  is dan een vlag omzetten en geen nieuw ontwerp
- antwoorden op `REQ_TYPE_GET_TELEMETRY_DATA` — de virtuele telemetriesensoren
- `TimeSeriesData.cpp/.h` — historiek op het apparaat zelf
- `CommonCLI`, en dus `password` / `guest_password` / admin-login én CLI over DM.
  Dat is exact het mechanisme voor "wie mag telemetrie opvragen" en voor de
  `list` / `get` opdrachten
- antwoord op node-discovery (`CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_SENSOR`)

Wat het **niet** heeft: WiFi. En dat is de reden dat de keuze niet vanzelf gaat.

## Correctie op een aanname van mij

De upstream companion heeft **geen webserver**. `WIFI_SSID` in
`companion_radio_wifi` zet `SerialWifiInterface` aan: het clientprotocol over
TCP, zodat de MeshCore-app over WiFi kan koppelen in plaats van over BLE of USB.
De webpagina die deze node nu heeft, was onze eigen toevoeging en staat niet in
upstream.

De webinterface moet dus op elke basis van nul gebouwd worden. Dat is geen
argument voor of tegen een van de twee.

## De afweging

Vanaf `companion_radio` moeten erbij: telemetrie antwoorden, historiek,
CommonCLI met wachtwoorden, doorstuurbeveiligingen — en de chat, contacten en
kanalen moeten eruit.

Vanaf `simple_sensor` moet erbij: WiFi, webserver, webhook, ping.

Het tweede lijstje is kleiner en bevat niets dat het mesh raakt. Het eerste
lijstje is precies het lijstje dat `simple_sensor` al af heeft.

---

# Kunnen de sensoren hun naam uit de web-GUI krijgen? (19 augustus 2026)

Namen als "router", "google", "donotica" instellen in de webinterface: ja. Maar
**het telemetriepakket kan die naam niet dragen**, en dat is een beperking van
het formaat en niet van onze code.

## Waarom niet

CayenneLPP is een reeks drietallen: kanaalnummer, type, waarde. Er is geen
naamveld. MeshCore heeft er ook niets voor: het enige wat bestaat is
`TELEM_CHANNEL_SELF 1` (`src/helpers/SensorManager.h:10`) en een oplopende
`next_available_channel` (`EnvironmentSensorManager.h:20`). Kanalen zijn getallen.

Een node die telemetrie opvraagt ziet dus:

    kanaal 3, LPP_SWITCH, 1
    kanaal 3, LPP_GENERIC_SENSOR, 12

en niet "google is bereikbaar in 12 ms".

## Waar de namen dan wel langs komen

Drie wegen, en ze vullen elkaar aan:

1. **De DM-opdrachten.** `list` geeft naam én toestand in leesbare tekst. Dit is
   de natuurlijke plek voor namen, en het is een extra reden dat die interface er
   moet zijn — niet als gemak, maar omdat het de enige weg is waarlangs een naam
   over het mesh gaat.
2. **Een CLI-opdracht** die de koppeling kanaal → naam teruggeeft, zodat een
   opvragende node die één keer ophaalt en bewaart. Dat is hoe MeshManager het
   met repeaters doet.
3. **De webinterface en de webhook** gebruiken namen direct; daar speelt het
   formaat geen rol.

## De valkuil: kanaalnummers moeten onveranderlijk zijn

Dit is het belangrijkste van deze sectie.

Als een opvragende node de koppeling "kanaal 3 = google" bewaart, en er wordt
later een monitor verwijderd waarna de rest opschuift, dan wijst die bewaarde
koppeling **stil** naar de verkeerde dienst. Geen foutmelding, alleen verkeerde
cijfers op een dashboard.

Dat is precies de foutklasse die MeshManager twee keer gekost heeft: het
verdict dat op het verkeerde pakket gestempeld werd, en het MQTT-topic dat
berekend werd in plaats van onthouden.

**Regel: een monitor krijgt bij aanmaken een kanaalnummer en houdt dat voor
altijd. Nummers van verwijderde monitors worden niet hergebruikt.** Onthouden,
niet herberekenen. Dat kost één veld in de opslag en het voorkomt een storing
die zich voordoet als "de cijfers kloppen niet helemaal".

Kanaal 1 blijft `TELEM_CHANNEL_SELF` (de node zelf: voeding, batterij), dus
monitors beginnen bij 2. Met 16 monitors in één pakket als bovengrens is er ruimte
tot kanaal 17.

---

# Voeding, WiFi en identiteit (19 augustus 2026)

## Netspanning als sensor: wat het bord echt kan

Gevraagd: `netvoeding: 0/1` en `batterijvoeding: 0/1` als twee aparte sensoren.
Dat kan, en het kost weinig: twee keer `LPP_SWITCH` is 2 x (2 kop + 1 waarde) =
**6 byte**.

Maar er is een hardwarebeperking die eerlijk vermeld moet worden, want ik heb
ernaar gezocht en het is er niet.

`variants/heltec_v3/HeltecV3Board.h` heeft alleen `getBattMilliVolts()`: een
ADC-meting van `PIN_VBAT_READ` via `PIN_ADC_CTRL`. Er is **geen VBUS-pin, geen
USB-detectie en geen laadstatus** — niet in het bord, niet in
`src/helpers/ESP32Board.h`. Gezocht op VBUS, USB, charge; nul resultaten.

**Netspanning kan dus niet gemeten worden, alleen afgeleid.** Uit de
batterijspanning:

| meting | betekenis |
|---|---|
| ~4,15 V en hoger, stabiel | vrijwel zeker USB aangesloten (lader houdt hem daar) |
| dalend over meerdere minuten | vrijwel zeker op batterij |
| 3,9 V stabiel | **dubbelzinnig**: volle batterij zonder lading, of USB met inactieve lader |

Die derde regel is de blinde vlek, en het is geen detail: precies bij een volle
batterij lijkt "stroom weg" op "stroom aanwezig". Een trendmeting lost het op,
maar heeft minuten nodig — te traag voor "de stroom is uit, waarschuw me nu".

**Daarom hoort de wifi-sensor hierbij.** Valt de router weg, dan verdwijnt WiFi
binnen seconden. WiFi weg én batterijspanning die begint te dalen is een
stroomstoring met hoge zekerheid en met de snelheid van de eerste van de twee.
De twee sensoren blijven apart, zoals gevraagd, maar de waarschuwing over een
stroomstoring hoort op de combinatie te staan en niet op de spanning alleen.

Wat de twee sensoren eerlijk betekenen:

- `netvoeding` — afgeleid: spanning hoog of stabiel. Traag en met een blinde vlek.
- `batterijvoeding` — afgeleid: spanning daalt. Betrouwbaarder, maar pas na enige
  tijd.

Beide krijgen in de webinterface die uitleg erbij. Een sensor die "0" toont
terwijl niemand weet hoe zeker dat is, is erger dan geen sensor.

## WiFi instellen: vier lagen

1. **Bij het flashen.** `WIFI_SSID` / `WIFI_PWD` als bouwvlag. Staat in de
   `platformio.local.ini` van de bouwkopie, die door MeshCore's `.gitignore`
   (regel 19) genegeerd wordt — nagekeken met `git check-ignore` voordat de
   gegevens er in gingen. De repo heeft alleen `firmware/wifi.ini.voorbeeld`
   met plaatshouders.
2. **Seriële console.** `CommonCLI::handleCommand` heeft al generieke `get ` en
   `set ` (`CommonCLI.cpp:264` en `:266`), en de configuratielaag werkt met
   `def("naam", veld)`. Twee velden toevoegen levert dus **gratis**
   `set wifi_ssid ...` en `get wifi_ssid` op, over serieel én over DM-CLI. Geen
   nieuwe opdrachtparser.
3. **Eigen SSID als terugval.** Lukt verbinden niet, dan een eigen toegangspunt
   zodat de webinterface bereikbaar blijft. Dit is nu betaalbaar: er is 270 kB
   RAM vrij, tegen de 24 kB waarmee dit project begon.
4. **Webinterface.** Netwerk instellen zonder kabel, met de opgeslagen waarde als
   baas over de gebakken waarde.

Die volgorde is ook de veiligheidsvolgorde: de opgeslagen instelling wint van de
gebakken, zodat een node die van netwerk verhuist niet opnieuw geflasht hoeft te
worden, en een node die je weggeeft schoongemaakt kan worden door de opslag te
wissen. Let wel: **een gebakken wachtwoord is met een flashdump terug te lezen.**
Voor een apparaat op je eigen netwerk is dat aanvaardbaar; het is geen geheim
meer zodra iemand de node fysiek heeft.

## Een nieuwe sleutel, en dat is de juiste keuze

De vraag was of dit apparaat, nu het geen companion meer is, een eigen sleutelpaar
moet krijgen. **Ja.**

- Andere nodes hebben de oude sleutel opgeslagen als `ADV_TYPE_CHAT`, een
  chatcontact. Dit apparaat wordt `ADV_TYPE_SENSOR`. Dezelfde sleutel hergebruiken
  betekent dat er in andermans contactenlijst een chatcontact staat dat stilletjes
  van soort verandert.
- De oude sleutel is veilig: volledige flashdump plus de losse spiffs-partitie in
  `MeshUptime-backup`, met `/identity/_main.id` erin en een herstelbeschrijving.
  Er gaat niets verloren.
- Er komt hoe dan ook een tweede sleutel bij voor de repeaterrol, met
  botsingscontrole op de adreshash. Verse sleutels genereren zit dus al in het plan.

De prijs is dat wie deze node als contact had, hem opnieuw moet toevoegen. Voor
een bewakingsnode is dat juist gedrag: het is een ander apparaat geworden.

---

# Kalibratiemeting voeding, 19 augustus 2026

Gemeten aan de echte node (`192.168.110.160`), niet uit een datasheet. Ruwe reeks
in `docs/meting-voeding-2026-08-19.log`.

## Resultaten

| toestand | spanning | stabiliteit |
|---|---|---|
| batterij, onder belasting | **4,034 V** | 7 van 8 metingen identiek |
| USB, lader klaar | **4,139 V** | 5 van 5 identiek |
| USB, actief aan het laden | **4,209–4,226 V** | licht variabel |

De USB-kant is dus een **bereik**, geen punt: 4,139 als de lader klaar is,
4,21–4,23 terwijl hij laadt. Wie op één USB-meting een drempel baseert, kiest de
verkeerde.

## Waarom de drempel toch werkt

De 4,034 V is de batterij **onder belasting**. De node trekt stroom, dus de
klemspanning zakt; op USB vult de lader die belasting aan en blijft de rail hoog.
Dat is een fysisch verschil en geen meetgeluk — en het wordt *groter* als de
batterij ouder wordt en zijn inwendige weerstand stijgt.

**Drempel rond 4,09 V**, met:
- twee grenzen (Schmitt): onder 4,09 → batterij, boven 4,12 → netspanning
- een wachttijd van ongeveer een minuut na een overgang, want daar zit een
  insteltransiënt van ~50 mV (4,052 → 4,087 → 4,034 in de eerste minuut)
- drempels als **instelling**, niet als constante
- de **ruwe spanning ook als sensor** (`LPP_VOLTAGE`, 2 byte), zodat de drempel
  later uit echte historiek bijgesteld kan worden zonder te flashen

## De belangrijkste bevinding gaat niet over spanning

Op batterij werd de node onbereikbaar over WiFi. Verloop:

1. uittrekken → korte hapering, daarna ~5 minuten bereikbaar met **31% verlies**
   (5 van 16 metingen kwamen niet aan)
2. daarna volledig weg, negen opeenvolgende mislukte metingen
3. seriële console gaf de oorzaak: `WiFi disconnected (reason 202)` —
   `WIFI_REASON_AUTH_FAIL`. Niet zwak bereik: rssi was -52
4. **USB er weer in bracht hem niet terug.** Pas een harde reset (veroorzaakt
   door het openen van COM4, wat DTR schakelt) herstelde de verbinding

Gevolgen voor het ontwerp, alle drie gemeten en niet aangenomen:

- **Waarschuwingen moeten over LoRa.** Juist tijdens een stroomstoring is de
  netwerkweg het minst betrouwbaar. Een webhook van Uptime Kuma kan deze node dan
  per definitie niet bereiken, dus de eigen controles zijn geen luxe maar de kern.
- **Er moet een wifi-waakhond komen.** Na N minuten zonder verbinding een
  volledige herverbinding forceren, en lukt dat niet, de WiFi-stapel of de node
  herstarten. Zonder dat blijft een node na een storing onbereikbaar terwijl
  alles weer werkt. Dit is een echte zwakte in de huidige firmware.
- **De wifi-sensor meet iets anders dan gedacht.** Niet "mijn eigen stroom is
  weg" — de node bleef leven op batterij — maar "mijn router is stroom kwijt".
  Bij een echte stroomstoring gebeurt allebei, dus WiFi blijft het snelle signaal
  en de spanning de bevestiging.

## Wat deze meting over mijn eigen aannames zei

Drie keer bijgesteld, en dat is de reden om te meten:

1. "USB leest boven 4,2 V, dus drempel op 4,2" — **fout**: USB met klaarstaande
   lader leest 4,139.
2. "De spreiding van 53 mV is ruis, dus gebruik variantie in plaats van niveau" —
   **fout**: dat was een insteltransiënt van de eerste minuut; daarna staat het
   niveau muurvast en discrimineert het prima.
3. "Hij is van het netwerk af, dus WiFi valt weg met de stroom" — **te snel**,
   op basis van één ontbrekende meting. Hij bleef eerst nog minuten bereikbaar.
   De echte reden kwam pas uit de seriële console.

---

# Eerste werkende flash (19 augustus 2026)

Basis `simple_sensor` plus `WifiTask`, geflasht over COM4, hash gecontroleerd.

    RAM:   17,3% (56.624 van 327.680)
    Flash: 35,2% (1.174.893 van 3.342.336)

WiFi met waakhond kost **128 byte** statisch bovenop de kale basis (56.496 →
56.624). Na de flash: `Sensor ID: 48D7AADE232B…` — dezelfde prefix als ervoor,
dus de identiteit is bewaard. Dat is geen toeval maar hoe `upload` werkt: het
schrijft alleen de programmapartitie, niet SPIFFS, dus `/identity/_main.id`
bleef staan. Wil je een nieuwe sleutel, dan moet dat bestand of SPIFFS gewist
worden — een aparte handeling, met de back-up als terugweg.

Netwerk: 4 van 4 pings op hetzelfde adres 192.168.110.160.

Wat hiermee weg is: poort 5000 en dus de mesh-verbinding van Home Assistant.
Bewuste keuze.

## Twee openstaande zaken uit deze ronde

**Eigen telemetrie vraagt een eigen SensorManager.** Onderzocht en de twee
makkelijke wegen zijn dicht:

- `SensorMesh::handleRequest` staat niet als `virtual` en `Mesh.h` kent hem niet,
  dus overschrijven in de app kan niet.
- `reply_data` staat achter `private:` (`SensorMesh.h:134` en `:140`), dus de
  buffer aanvullen kan ook niet.

Belangrijker: `handleRequest` roept `onSensorDataRead()` **niet** aan (vergelijk
`SensorMesh.cpp:172-188` met `:924-936`). Sensoren die alleen daar toegevoegd
worden, zouden dus wel bijgehouden zijn maar **niet in het antwoord op een
telemetrieverzoek verschijnen** — een stille fout, gevonden voordat er code was.

De juiste weg is `SensorManager::querySensors()`, die in beide paden aangeroepen
wordt. Die klasse heeft ook al een instellingen-interface
(`getNumSettings` / `getSettingName` / `setSettingValue`), en dat is precies wat
de monitors nodig hebben: naam, adres en interval instelbaar over serieel en over
DM-CLI zonder eigen parser. Kosten: `sensors` is een global van concreet type in
`variants/heltec_v3/target.{h,cpp}`, dus een patch van twee regels om er onze
subklasse van te maken. Komt bij de patches te staan.

**De waakhond heeft geen logging.** Zelf gebouwd en zelf een gebrek: `WifiTask`
schrijft niets naar de console, dus de toestand waarin hij zit en het aantal
harde resets zijn onzichtbaar. Bij een onderdeel dat bestaat om een moeilijk te
betrappen storing te repareren, is dat het verkeerde soort stilte. Gaat mee in
de volgende ronde, samen met de sensoren, want die vraagt toch een nieuwe flash.
