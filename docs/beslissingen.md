# Beslissingen, met de reden

Dit bestand bewaart *waarom* de firmware is zoals hij is — en met opzet ook de
keuzes die later zijn **omgedraaid**, want een omgedraaide keuze zegt meer dan een
keuze die vanzelf goed viel. Wat er wél staat is de reden; wat er niet meer staat
is de chronologie eromheen.

De getallen die hieronder als onderbouwing dienen staan in
[metingen.md](metingen.md); hoe het nu werkt in [werking.md](werking.md).

## Welke basis: simple_sensor, en de omweg via companion_radio

Drie keer van standpunt gewisseld, en de heenweg is de moeite van het bewaren
waard.

1. **Eerst `companion_radio`**, omdat de node dat al was: het was een companion
   met een eigen webpagina, en die pagina was de beheerweg. Van daaruit moesten
   erbij: antwoorden op telemetrieverzoeken, historiek, `CommonCLI` met
   wachtwoorden, doorstuurbeveiligingen — en de chat, contacten en kanalen moesten
   eruit.
2. **Toen `simple_sensor`**, na een blik in de bron. Dat voorbeeld is bijna
   letterlijk MeshUptime: `ADV_TYPE_SENSOR`, doorsturen mét beveiliging,
   antwoorden op `REQ_TYPE_GET_TELEMETRY_DATA`, historiek op het apparaat zelf, en
   `CommonCLI` met wachtwoorden en CLI over DM. Precies het lijstje dat vanaf
   `companion_radio` nog gebouwd had moeten worden, was daar al af. Wat er niet in
   zat, is wifi.
3. **Toen weer `companion_radio`**, toen Home Assistant een eis werd: die kwam
   binnen over het clientprotocol op poort 5000, en dat is de companion.
4. **Toen definitief `simple_sensor`**, toen die eis verviel. De node is de
   mesh-gateway van Home Assistant niet meer; die rol is bewust opgegeven, en de
   oude firmware en sleutel staan buiten de repo in een back-up.

Wat die omweg heeft opgeleverd: het lijstje "vanaf `simple_sensor` moet erbij"
(wifi, webserver, webhook, ping) is kleiner dan het omgekeerde lijstje, en het
bevat niets dat het mesh raakt.

**Een aanname die onderweg onderuit ging:** de upstream companion heeft géén
webserver. `WIFI_SSID` zet daar `SerialWifiInterface` aan — het clientprotocol
over TCP, zodat de MeshCore-app over wifi kan koppelen in plaats van over BLE of
USB. De webpagina die deze node had, was eigen werk. De webinterface moest dus op
elke basis van nul, en dat is geen argument voor of tegen een van de twee.

## Eén radio-identiteit, niet twee

De vraag was of een node die zowel bewaakt als doorstuurt "fout loopt op het
mesh". Nagekeken in upstream, en het antwoord is het omgekeerde.

| voorbeeld | kondigt zich aan als | stuurt door |
|---|---|---|
| `simple_repeater` | `ADV_TYPE_REPEATER` | ja, zes controles |
| `simple_room_server` | `ADV_TYPE_ROOM` | ja, dezelfde controles |
| `simple_sensor` | `ADV_TYPE_SENSOR` | ja, `disable_fwd` + `flood_max` |
| `companion_radio` | `ADV_TYPE_CHAT` | ja, maar zonder één enkele controle |

Doorsturen is in MeshCore **niet gebonden aan het advert-type**:
`allowPacketForward()` wordt geraadpleegd wat een node ook aankondigt. Een
identiteit die doorstuurt én een toepassing draait is dus niet de uitzondering
maar de regel.

**Wat wél fout is, is een verkeerd advert-type.** Een node die `ADV_TYPE_CHAT`
aankondigt en andermans verkeer doorstuurt, is oneerlijk tegenover het mesh:
niemand ziet dat hij infrastructuur is, niemand kan hem om statistieken vragen.
De oplossing is niet een tweede identiteit maar het juiste type aankondigen —
`ADV_TYPE_SENSOR`.

Twee radio-identiteiten hebben in upstream geen enkel voorbeeld, en het vraagt een
patch in `src/Mesh.cpp` op negen plaatsen. Uit MeshStats is bekend wat een
kernpatch kost: hij moet bij elke upstream-versie opnieuw aangebracht en
nagekeken worden. De prijs van één identiteit is dat de node in de MeshCore-app in
één lijstje staat en niet in twee. De opbrengst is geen kernpatch.

**Een correctie op de aanname waar het idee uit kwam:** SIREN doet het ook niet.
Die zet `self_id = rooms[0].id` en laat de routering ongemoeid; de sleutels van
room 1..N leven boven de meshlaag en versleutelen kamerinhoud. SIREN heeft één
radio-identiteit.

De uitwerking van de patch die er zou moeten komen als de keuze ooit omgaat, staat
bewaard in [../firmware/patches/LEESMIJ.md](../firmware/patches/LEESMIJ.md), met
de valkuil erbij: de adreshash is één byte, dus twee willekeurige sleutels hebben
1 kans op 256 dat hun hash gelijk is, en dan is de routering dubbelzinnig.

## Zendtijd: het doorsturen is de kostenpost, niet de bewaking

- De SX1262 heeft één modem. Ontvangen gebeurt hoe dan ook één keer; twee rollen
  op één radio verdubbelen dat niet. Dat is gratis.
- Zenden is de schaarse kant. Een bewakingsmelding is zeldzaam (alleen bij een
  toestandsovergang) en kort (maximaal 158 byte). In verhouding tot wat een
  repeater aan doorgestuurd verkeer uitzendt, is dat ruis.
- De 1%-duty-cycle op 868 MHz wordt dus door de repeaterfunctie opgegeten en niet
  door de bewaking. Wie zich zorgen maakt over zendtijd moet naar `flood_max` en
  lusdetectie kijken, niet naar het aantal sensoren.

Eén echte samenloop blijft: staat een melding klaar terwijl de radio een
doorgestuurd pakket uitzendt, dan wacht de melding. Vandaar de ontwerpregel dat
de radio voorgaat en een melding in de wachtrij hoort, niet blokkerend verzonden.

## De repeaterrol komt als laatste, en niet met de companion-vlag

De companion heeft `repeat.disable_fwd`, maar zijn `RepeatPrefs` is een kopie van
`CommonCLI` waarin vier velden zijn **uitgecommentarieerd**: `flood_max`,
`flood_max_unscoped`, `flood_max_advert` en `loop_detect`. De echte repeater doet
zes controles. `repeat` aanzetten zoals dat er stond, levert een repeater die
alles doorstuurt wat een echte repeater juist wegfiltert, inclusief floodlussen.

Daar zit het zendtijdprobleem — en niet waar de vraag het vermoedde. Een
repeaterknop in de webinterface zonder die vier velden is een knop die schade
doet.

`simple_sensor` heeft `disable_fwd` en `flood_max` wél, dus voor MeshUptime is de
repeaterrol een vlag omzetten en geen nieuw ontwerp. Hij staat achteraan omdat dat
ook de veiligste volgorde is.

## Kanaalnummers zijn onveranderlijk

Dit is de belangrijkste afspraak van het hele project, en hij komt uit een
foutklasse die eerder al twee keer geld gekost heeft in MeshManager: het verdict
dat op het verkeerde pakket gestempeld werd, en het MQTT-topic dat berekend werd
in plaats van onthouden.

CayenneLPP draagt geen namen. Een opvragende node bewaart dus zelf "kanaal 6 =
google". Schuift dat nummer op na een verwijdering, dan wijst die koppeling stil
naar een andere dienst: geen foutmelding, alleen verkeerde cijfers op een
dashboard. **Onthouden, niet herberekenen.** Dat kost één veld in de opslag — het
`ch_ever_used`-masker — en het voorkomt een storing die zich voordoet als "de
cijfers kloppen niet helemaal".

Zonder die byte zou de afspraak dat nummers nooit hergebruikt worden alleen
gelden voor één stroomvoorziening.

## Namen gaan over het mesh via een DM, niet via telemetrie

Er is geen beter LPP-type te kiezen; het formaat kan een naam eenvoudig niet
dragen. Nagekeken:

| kandidaat voor pingtijd | waarom niet |
|---|---|
| `analoginput` | 2 byte, 0,01-resolutie → loopt dood boven 327 ms |
| `luminosity` | 2 byte, maar beweert "lux" en dat is onwaar |
| `frequency` | 4 byte exact, maar beweert "hertz" |
| **`genericsensor`** | 4 byte, multiplier 1, exacte hele getallen, en het belooft niets onwaars |

Een ander type zou geen naam geven, alleen een ander verkeerd woord. Daarom lopen
namen langs drie andere wegen: de DM-opdracht `list` (de enige weg over het
mesh), de webinterface met de volledige kanaalkaart, en een dashboard dat de
koppeling bewaart.

## Bewaking van buiten aanleveren, én zelf pingen

Een bestaande Uptime Kuma is beter in bewaken dan dit apparaat ooit wordt: die
kent herhaalpogingen, onderhoudsvensters en historiek. Wat hij niet kan, is een
melding bezorgen als het internet weg is.

Vandaar de taakverdeling: een **inkomende webhook** (`/hook`) waarmee Kuma zijn
uitslag hierheen stuurt, plus **eigen controles** voor het geval dat Kuma zelf
onbereikbaar is — en dat is nu juist het geval dat je wil weten. Kuma weet *of*
iets stuk is; deze node is de enige die het nog kan vertellen.

De keuze voor ICMP boven een TCP-verbinding stond eerst open ("meten welke
minder kost"). Het is ICMP geworden: één ping tegelijk, in stappen, zonder ooit te
wachten, met een naamsopzoeking die hoogstens elke tien minuten opnieuw gebeurt.

**Eén kanaaltoewijzer, geen tweede.** Gemelde diensten lopen door dezelfde
`allocChannel()` en dezelfde `ch_ever_used`-byte als de ping-monitors; het
onderscheid zit in het adresveld. De oude hooktabel van acht ingangen is
verwijderd — dat was een tweede boekhouding naast de monitors, en twee
boekhoudingen over dezelfde dingen lopen na een verwijdering uit elkaar.
`createMonitor()` is nu de enige plek waar een monitor ontstaat; CLI, web en
`/hook` komen er alle drie langs.

## Waarschuwingen over LoRa en niet over het netwerk

Niet aangenomen maar gemeten: juist tijdens een stroomstoring is de netwerkweg
het minst betrouwbaar. Zie [metingen.md](metingen.md) — de node raakte op batterij
van het netwerk met `reason 202` bij goed bereik, en kwam er zonder harde reset
niet uit.

Daaruit volgt ook dat de wifi-waakhond geen luxe is, en dat de eigen controles de
kern zijn en niet een aanvulling.

## Antwoorden als TXT_TYPE_PLAIN, en knippen op 158

`BaseChatMesh` zegt uitdrukkelijk dat voor antwoorden van type
`TXT_TYPE_CLI_DATA` geen ACK verwacht wordt. Antwoorden we een DM-opdracht als
CLI-data, dan **weten we niet of het antwoord is aangekomen**. Als gewone tekst
komt er wel een ACK. Bezorgcontrole op antwoorden vraagt dus `TXT_TYPE_PLAIN`.

En die keuze is dode letter zonder herhalen op de ACK: de kennis dat het antwoord
niet aankwam is alleen iets waard als er ook naar gehandeld wordt.

Bezorgcontrole hoefde verder niet verzonnen te worden. Het protocol berekent zelf
een `expected_ack` (vier byte SHA-256 over tijdstempel, pogingnummer en tekst,
met de eigen publieke sleutel erbij), dus een ACK bewijst dat **deze** tekst
aankwam en niet enkel dat er iets aankwam. Verzenden geeft ook een `est_timeout`
terug: hoe lang wachten redelijk is gegeven het aantal hops. Niet zelf een getal
kiezen.

De knipgrens van 158 in plaats van 160 staat uitgelegd in
[metingen.md](metingen.md) en in [werking.md](werking.md).

## Een nieuw sleutelpaar bij de rolwissel

De node was een `ADV_TYPE_CHAT`-companion en werd een `ADV_TYPE_SENSOR`. Dezelfde
sleutel hergebruiken zou betekenen dat er in andermans contactenlijst een
chatcontact staat dat stilletjes van soort verandert. Dus een nieuwe sleutel.

De prijs is dat wie deze node als contact had hem opnieuw moet toevoegen. Voor een
bewakingsnode is dat juist gedrag: het is een ander apparaat geworden. De oude
sleutel is niet weg — er staat een volledige flashdump plus de losse
spiffs-partitie buiten de repo, met een herstelbeschrijving.

## De opgeslagen instelling gaat vóór de gebakken

Wifi in vier lagen, en die volgorde is de veiligheidsvolgorde: bouwvlag → seriële
console → eigen toegangspunt als terugval → webinterface, met de **opgeslagen**
waarde als baas over de gebakken. Zo hoeft een node die naar een ander netwerk
verhuist niet opnieuw geflasht te worden, en is een node die je weggeeft schoon te
maken door de opslag te wissen.

Eén ding hoort daar eerlijk bij: **een gebakken wachtwoord is met een flashdump
terug te lezen.** Voor een apparaat op je eigen netwerk is dat aanvaardbaar; het
is geen geheim meer zodra iemand de node fysiek heeft.

Voor de sensorinstellingen was een eigen opslag nodig. `CommonCLI` kent wel
`sensor set`, maar bewaart het resultaat niet: bij het opstarten wordt uit de
voorkeuren maar één sensorinstelling teruggezet (`gps`). Een bijgestelde
`mains.hi` gold dus tot de volgende herstart — precies de instelling waarvan uit
de meting bekend was dat hij ooit bijgesteld zou moeten worden. En ping-monitors
zou je na elke stroomstoring opnieuw moeten intypen, en dat is voor een
bewakingsnode geen optie.

## De eigen SensorManager, en waarom de twee makkelijke wegen dicht zaten

Eigen velden in het telemetrieantwoord kunnen niet vanuit de toepassingscode:

- `SensorMesh::handleRequest` staat niet als `virtual`, dus overschrijven kan
  niet.
- `reply_data` staat achter `private:`, dus de buffer aanvullen kan ook niet.

Belangrijker nog: **`handleRequest` roept `onSensorDataRead()` niet aan**, terwijl
de periodieke ronde dat wel doet. Sensoren die alleen daar toegevoegd worden,
zouden wel bijgehouden zijn maar **niet in het antwoord op een telemetrieverzoek
verschijnen** — een stille fout, gevonden voordat er code was.

`SensorManager::querySensors()` is de enige haak die in beide paden loopt. Die
klasse heeft ook al een instellingen-interface (`getNumSettings` /
`getSettingName` / `setSettingValue`), en dat is precies wat de monitors nodig
hebben: naam, adres en interval instelbaar over serieel en over DM-CLI zonder
eigen parser.

De prijs is één patch: de global `sensors` is in de variant hardgecodeerd op
`EnvironmentSensorManager`. Drie regels vervangen en een macro met terugval
toevoegen; zonder de macro's bouwt de variant exact wat hij ervoor bouwde, dus de
patch is kandidaat om ooit stroomopwaarts aan te bieden.

## Weigeren is stilte, geen leeg antwoord

Voor een telemetrieverzoek zonder recht: `return 0`, de bestaande weg voor "geen
antwoord". Een leeg antwoord kost zendtijd per geweigerd verzoek, en niets belet
een vrager elke seconde te vragen — dan is de weigering zelf een manier om de duty
cycle op te maken. Bovendien leest "nul sensoren" in een app als een kapotte
sensor en niet als een gesloten deur. Stilte is niet te onderscheiden van buiten
bereik, en dat is precies wat een vrager zonder recht hoort te zien.

Voor een **DM-opdracht** is de afweging anders uitgevallen: daar komt één korte
weigering, hoogstens eens per tien minuten per afzender. Het verschil zit in wie
er aan de andere kant staat. Een afzender van een DM staat al in de toegangslijst
en weet dus al dat deze node bestaat; stil negeren zou hem alleen aan het
herhalen zetten, en dat kost meer zendtijd dan één antwoord.

## Een correctie op wat hier eerder stond over toegang

Er stond dat **elke** node op het mesh de sensoren kon uitlezen. Dat was te sterk,
en de nuance is belangrijk genoeg om te bewaren.

Het gat in `handleRequest` was echt: `perms` kwam binnen en werd niet gebruikt,
dus zelfs een ingang met alleen alarmrecht kon alles opvragen. Maar dat is niet
de weg waarlangs een tweede node zes kanalen zag: alleen nodes die **al in de
toegangslijst staan** bereiken `onPeerDataRecv`, want `searchPeersByHash` zoekt
niets anders af.

De weg naar binnen is de login met het beheerderswachtwoord — en die voegt een
node automatisch als **beheerder** toe, ook met het slot dicht. Daarom zijn het
slot en het wachtwoord samen één beveiliging en niet twee: `acl.strict` aanzetten
zonder het wachtwoord te wijzigen is een deur op slot doen naast een open raam.

## Bewust niet gedaan

- **De DM-opdrachten openzetten voor leescontacten.** Het is één voorwaarde
  weghalen. Niet gedaan omdat rechten verruimen een keuze van de eigenaar is en
  niet van een agent. Zie [openstaand.md](openstaand.md).
- **Kanaalbotsing met de basisklasse oplossen.**
  `EnvironmentSensorManager::querySensors()` zet zijn kanaalteller terug op 2 en
  deelt vanaf daar uit aan gevonden I2C-sensoren, bovenop onze vaste 2/3/4. Niet
  op te lossen zonder in de basisklasse te snijden, want de reset gebeurt binnen
  de ouder. Deze node heeft geen omgevingssensoren en `begin()` waarschuwt zodra
  er één opduikt.
- **Eén cosmetische logregel.** Bij een overgang naar online staat er nog
  `(reden 0, ...)`; de reden hoort daar niet te staan. De inhoudelijke fout is
  weg (er stond eerder misleidend 202). Blijven staan omdat een flash voor één
  logregel het niet waard is.

## De regels die niet onderhandelbaar zijn

Ze staan hier omdat ze elke ronde opnieuw de doorslag hebben gegeven.

1. Statisch alloceren: vast maximum aantal monitors, vaste buffers, geen `String`
   in de eigen verwerkingspaden.
2. Geen tweede webserver.
3. Elke toevoeging meten, vóór en ná, op het **grootste vrije blok**.
4. De radio gaat voor: een bewakingsronde mag de mesh-timing niet ophouden, en
   nergens wordt gewacht of `delay()` gebruikt.
5. De identiteit blijft. De back-up staat buiten de repo; de herstelweg is
   gedocumenteerd naast de dump.
6. Eén zeef per soort invoer. Twee zeven die 99% hetzelfde doen, geven ooit een
   naam die het ene pad aanneemt en het andere weigert — en dat vind je alleen
   door beide paden naast elkaar te proberen.

## Waarom de webconsole 'icmp' over de draad stuurt als jij 'ping' typt (20 aug 2026)

Een IPS in het netwerk van de eigenaar (UDM Pro, Intrusion Prevention) reset elke
HTTP-verbinding waarvan de body exact kleine-letters `ping` plus witruimte bevat --
de klassieke command-injection-signatuur. Gemeten, niet vermoed: `pinx 8.8.8.8` en
`PING 8.8.8.8` bereikten de node (200), `ping 8.8.8.8` stierf op het pad, en vanaf
een ander netwerksegment kwam exact dezelfde opdracht wel aan en draaide de ping
gewoon (`3/3 ok`).

De keuze: niet de gebruiker zijn IPS laten herconfigureren, maar het woord op de
draad veranderen. De console-JS herschrijft een getypte `ping` naar `icmp` voor het
verzenden; de node accepteert beide (zelfde lengte, zelfde route, zelfde engine).
Over de DM (LoRa, versleuteld) blijft het `ping`, want daar kijkt geen middlebox
mee. Een beveiligingsdoos die je eigen beheerverkeer sloopt is een omgevingsfeit;
er stil omheen werken met een alias is goedkoper dan documentatie die zegt "zet je
IPS uit".
