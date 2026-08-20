# Metingen

Bijna elke keuze in dit project berust op een getal dat aan de echte hardware
gemeten is. Ze staan hier bij elkaar, met de datum, zodat een keuze na te rekenen
is in plaats van te moeten worden geloofd.

Drie soorten uitspraak, streng gescheiden:

- **gemeten** — aan het toestel gezien, met het cijfer erbij
- **gebouwd** — de code staat er en compileert, maar het gedrag is niet op het
  toestel bevestigd
- **onderbouwd** — uit de bron afgeleid, niet gemeten

## Voedingskalibratie — 19 augustus 2026

Gemeten aan de node zelf, niet uit een datasheet. De ruwe reeks staat in
[meting-voeding-2026-08-19.log](meting-voeding-2026-08-19.log): één spanning per
regel, elke ~20 s uitgelezen over HTTP.

| toestand | spanning | wat de reeks zegt |
|---|---|---|
| batterij, onder belasting | **4,034 V** | 7 van de 10 metingen op batterij precies 4,034; de uitschieters (4,052 tweemaal, 4,087 eenmaal) liggen alle drie in de eerste twee minuten |
| USB, lader klaar | **4,139 V** | de basislijn aan het begin van de reeks; de kop van het logbestand noemt hem stabiel |
| USB, actief ladend | **4,209–4,226 V** | zes metingen na het weer inpluggen: eenmaal 4,226, daarna vijfmaal 4,209 |

De USB-kant is dus een **bereik** en geen punt. Wie op één USB-meting een drempel
baseert, kiest de verkeerde.

### Waarom de drempel werkt

De 4,034 V is de batterij **onder belasting**. De node trekt stroom, dus de
klemspanning zakt; op USB vult de lader die belasting aan en blijft de rail hoog.
Dat is een fysisch verschil en geen meetgeluk — en het wordt *groter* als de
batterij ouder wordt en zijn inwendige weerstand stijgt.

Daaruit volgen de standaardwaarden in de firmware: **onder 4,09 V batterij, boven
4,12 V netspanning**, met 60 s insteltijd na een overgang (de transiënt van
~50 mV: 4,052 → 4,087 → 4,034 in de eerste minuut) en drie opeenvolgende
metingen aan dezelfde kant.

Een naïeve drempel op 4,2 V — wat een datasheet zou suggereren — zou "geen
netspanning" hebben gemeld met de stekker erin.

### Wat deze meting over drie aannames zei

Dit is de reden om te meten in plaats van te rekenen:

1. "USB leest boven 4,2 V, dus drempel op 4,2" — **fout**: USB met een
   klaarstaande lader leest 4,139.
2. "De spreiding van 53 mV is ruis, dus gebruik variantie in plaats van niveau" —
   **fout**: dat was een insteltransiënt van de eerste minuut; daarna staat het
   niveau muurvast en discrimineert het prima.
3. "Hij is van het netwerk af, dus wifi valt weg met de stroom" — **te snel**, op
   basis van één ontbrekende meting. Hij bleef eerst nog minuten bereikbaar.

## Het wifi-gedrag op batterij — 19 augustus 2026

De belangrijkste bevinding van die meetsessie gaat niet over spanning. Uit
dezelfde reeks:

1. Stekker eruit → korte hapering, daarna nog ongeveer vier minuten bereikbaar
   met **31% verlies**: 5 van de eerste 16 metingen kwamen niet aan.
2. Daarna volledig weg: **21 opeenvolgende mislukte metingen**, ruim zeven
   minuten.
3. De seriële console gaf de oorzaak: `WiFi disconnected (reason 202)` —
   `WIFI_REASON_AUTH_FAIL`. Dus niet door zwak bereik; de rssi was rond de
   -50 dBm (zie de noot onderaan).
4. **De USB er weer in bracht hem niet terug.** Pas een harde reset — veroorzaakt
   door het openen van COM4, wat DTR schakelt — herstelde de verbinding.

Drie gevolgen voor het ontwerp, alle drie gemeten en niet aangenomen:

- **Waarschuwingen moeten over LoRa.** Juist tijdens een stroomstoring is de
  netwerkweg het minst betrouwbaar. Een webhook van een bestaande Uptime Kuma kan
  deze node op dat moment per definitie niet bereiken, dus de eigen controles zijn
  geen luxe maar de kern.
- **Er moet een wifi-waakhond zijn.** Zonder een gedwongen herverbinding en een
  eigen toegangspunt als laatste redmiddel blijft een node na een storing
  onbereikbaar terwijl alles alweer werkt.
- **De wifi-sensor meet iets anders dan gedacht.** Niet "mijn eigen stroom is
  weg" — de node bleef leven op batterij — maar "mijn router is stroom kwijt".
  Bij een echte stroomstoring gebeurt allebei, dus wifi blijft het snelle signaal
  en de spanning de bevestiging.

**Noot bij de rssi.** In de broncode staat -52 dBm
(`firmware/examples/meshuptime/WifiTask.h`), in de oudere documentatie -48. Welk
van de twee bij het moment van wegvallen hoort is niet meer te herleiden; de
ruwe reeks bevat geen rssi. Voor de conclusie maakt het niet uit: beide zijn ruim
voldoende bereik, en dat is het hele punt bij reason 202.

## Heap en vlucht per bouw

Dit project is van begin tot eind bepaald door het **grootste vrije blok**, niet
door het totaal vrije geheugen: dat is de maat waarop lwip in dit project eerder
al halve antwoorden gaf. Alle getallen hieronder komen uit de bouwuitvoer en de
seriële console van de betreffende ronde. RAM is telkens van 327.680 byte, flash
van 3.342.336.

| ronde (2026) | RAM | flash | pagina |
|---|---|---|---|
| oude companion, wifi + BLE samen | — | — | — |
| eerste flash: `simple_sensor` + wifi | 17,3% (56.624) | 35,2% (1.174.893) | — |
| SensorManager + webserver | 18,2% (59.664) | 36,8% (1.229.989) | — |
| ping-monitors + DM-opdrachten | 19,4% (63.472) | 37,2% (1.243.797) | — |
| kanaalkaart + `/hook` | 20,0% (65.376) | 37,6% (1.256.701) | 11.433 |
| toegangsbeheer | 22,1% (72.344) | 38,1% (1.272.989) | 22.415 |
| nodebeheer (laatste vastgelegde bouw) | 22,9% (75.184) | 39,5% (1.321.521) | 66.505 |

De heap, uit de seriële console:

| moment | vrij | grootste blok |
|---|---|---|
| oude companion, vóór het opruimen | 24.844 | 11.764 |
| na `simple_sensor` + eigen SensorManager + webserver | 235.156 | 225.268 |
| ná het bedienen van webverzoeken (fragmentatie) | — | 163.828 |
| tijdens bedrijf, uptime 25 min | ~221 kB | ~212 kB |

Het grootste blok ging dus met een factor 19 omhoog. Waar dat vandaan komt:

- **BLE eruit.** Upstream houdt de companion in drie aparte envs (usb, ble,
  wifi); de oude maatwerkbouw zette wifi en BLE *samen* aan. De BLE-stapel neemt
  bij initialisatie tientallen kB heap. MeshUptime wordt beheerd over web, mesh
  en CLI en heeft BLE niet nodig.
- **Drie statische posten die bij een chatclient horen en niet bij een bewaker**:
  `MAX_CONTACTS` van 350 naar 32, `MAX_GROUP_CHANNELS` van 40 naar 4,
  `OFFLINE_QUEUE_SIZE` van 256 naar 16.
- De chatclient, de berichtenring, de MQTT-publisher en het doorsturen van ruwe
  pakketten zijn er niet meer.

Wat **niet** kan: meer RAM erbij. Het bord is een ESP32-S3 zonder PSRAM. Er valt
niets bij te schakelen, alleen minder te gebruiken.

## De bytebegroting van een telemetriepakket

`SensorMesh` maakt zijn CayenneLPP als `MAX_PACKET_PAYLOAD - 4` = **180 byte**
(die 4 zijn de tijdstempel die vóór de gegevens gaat). Elk veld kost 2 byte kop
plus zijn gegevens. Dit is **onderbouwd** uit de bron, niet met een pakketdump
nagemeten:

| veld | type | byte |
|---|---|---|
| kanaal 1, batterijspanning | `LPP_VOLTAGE` | 2 + 2 = 4 |
| kanaal 1, GPS als actief | `LPP_GPS` | 2 + 9 = 11 |
| kanaal 2, netvoeding | `LPP_SWITCH` | 2 + 1 = 3 |
| kanaal 3, batterijvoeding | `LPP_SWITCH` | 2 + 1 = 3 |
| kanaal 4, wifi | `LPP_SWITCH` | 2 + 1 = 3 |
| | **vast totaal** | **24** |

Per monitor, in het duurste geval (up, dus mét pingtijd):

| veld | type | byte |
|---|---|---|
| toestand | `LPP_SWITCH` | 2 + 1 = 3 |
| rondetijd | `LPP_GENERIC_SENSOR` | 2 + 4 = 6 |
| | **per monitor** | **9** |

180 − 24 = 156 byte over, dus er zouden 17 monitors passen. Er zijn er 8
toegestaan omdat er 8 vaste kanalen zijn (5 t/m 12): 72 byte, en dat laat 84 byte
over voor omgevingssensoren die er later bij komen. `querySensors()` rekent het
bij elke ronde na en kapt af — een aanname die alleen in commentaar staat is geen
aanname waar je op kunt bouwen.

De ruwe spanning gaat **niet** nog eens mee op kanaal 2: kanaal 1 heeft die al,
en dubbel verzenden kost bytes zonder iets toe te voegen.

## De maat van een bericht

**Onderbouwd** uit upstream, en met een model nagerekend.

`MAX_TEXT_LEN` is 160 (`helpers/BaseChatMesh.h`, 10 × `CIPHER_BLOCK_SIZE`). De
grens waar het commentaar daar naar wijst is 184 − 4 − 2 − 1 = 177, dus 160 is
een gekozen ronde marge en geen hard protocolplafond. Maar `composeMsgPacket`
doet twee controles:

    if (text_len > MAX_TEXT_LEN) return NULL;
    if (attempt > 3 && text_len > MAX_TEXT_LEN-2) return NULL;

**Een bericht van 159 of 160 byte gaat er de eerste keer wel uit, maar kan na de
vierde poging niet meer opnieuw verstuurd worden.** Vandaar de knipgrens van 158.

De knipper is met een model in Python op 20.000 gevallen doorgerekend. Vóór de
reparatie: 132 berichten van tot 184 byte, dus boven `MAX_TEXT_LEN` — de
slotregel werd achter het laatste stuk geplakt zonder dat er ruimte voor
gereserveerd was. Na de reparatie: 0 overtredingen, langste bericht exact 158.
Twaalf sensoren met naam en toestand is precies 158 byte; met de knipgrens op 160
was dat een bericht geweest dat na de vierde poging niet meer te versturen is.

## Wat er op het toestel bevestigd is

**Gemeten**, seriële console en webinterface, 19 en 20 augustus 2026:

- De identiteit overleeft een `upload`: dezelfde sleutelprefix voor en na. Dat is
  geen toeval maar hoe `upload` werkt — het schrijft de programmapartitie en niet
  SPIFFS.
- De toestandsmachine van de voeding doet op de echte hardware wat ze moet doen:
  `mains.state=1` op een node die aan USB hangt, met de gekalibreerde drempels.
- De webserver antwoordt 401 zonder inloggegevens en 200 met.
- `/hook` keurt zijn invoer: een naam met een puntkomma erin wordt met 400
  afgewezen; een geldige melding krijgt 200 met het kanaalnummer erin.
- Ping-monitors op kanaal 5 en 6 overleven een herstart, inclusief het
  `ch_ever_used`-masker, dus een nummer wordt na een herstart niet opnieuw
  uitgedeeld. Direct na het opstarten staat er `pauze` in plaats van `op`: dat is
  de bedoelde insteltijd van 30 s nadat wifi opkomt, en na 45 s stonden de
  metingen er.
- **Telemetrie end-to-end**: een tweede node vraagt telemetrie op en ziet zes
  kanalen. Daarmee is ook in de praktijk goed opgelost wat statisch gevonden was
  — dat `handleRequest` `onSensorDataRead()` niet aanroept en de sensoren dus in
  `querySensors()` moeten staan.
- Het toegangsbeheer: slot open (de standaard), één ACL-ingang met leesrecht plus
  waarschuwingen. In de proef scheidde SNR twee nodes met dezelfde naam precies
  zoals bedoeld: 0 hop met -11,5 tegen 2 hop met -19,8.
- Nodebeheer over `/cli`: `ver` geeft `v1.17.0 (Build: 9 Aug 2026)`, `advert`
  geeft `OK - Advert sent`, en een `set radio` zonder bevestiging krijgt 409.
- Een volle pagina in bedrijf: net 4,157 V, wifi online op -48 dBm, 2 van 4
  monitors op, uptime 25 min, 0 resets, ~221 kB vrij en ~212 kB grootste blok.

## Een storing die geen storing was

Bij het nakijken van de pagina in een browser kwam er een stroom consolefouten:
*"Request cannot be constructed from a URL that includes credentials"*. Dat was
**geen fout in de firmware** maar in hoe de pagina geopend werd: met de
inloggegevens in de URL, en `fetch()` weigert een relatieve URL te herleiden
tegen zo'n adres. Zonder die gegevens in de URL vult de pagina zich normaal.
Opgeschreven omdat het er precies uitzag als een echte fout.
