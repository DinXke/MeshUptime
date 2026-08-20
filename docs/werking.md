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

De node kondigt zich aan als **`ADV_TYPE_SENSOR`** (`SensorMesh.cpp:297`) en
antwoordt op node-discovery met `CTL_TYPE_NODE_DISCOVER_RESP | ADV_TYPE_SENSOR`.
Dat is het eerlijke type: dit apparaat biedt sensoren aan.

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

## De webinterface

Één pagina uit PROGMEM, één webserver, geen framework, geen CDN, geen externe
stylesheet, geen extern lettertype: een node zonder internet moet zijn eigen
beheerpagina kunnen tonen. Donker als standaard, licht via
`prefers-color-scheme`, systeemlettertypen met mono voor de getallen. Drie
tabbladen — bewaking, toegang, node — met inklapbare secties.

Kleur is semantisch en niet decoratief. Daarvoor bestaat het veld `sev`: "aan"
op kanaal 2 (netvoeding) is goed, "aan" op kanaal 3 (batterijvoeding) is juist
een waarschuwing. Een pagina die de kleur uit het woord raadt, verft die twee
hetzelfde en dan liegt de kleur.

Alles zit achter HTTP Basic-auth. Gebruikersnaam en wachtwoord komen uit
bouwvlaggen (`WEB_USER` / `WEB_PASS`) en die staan nog op hun voorbeeldwaarde —
zie [openstaand.md](openstaand.md).

### De routes

| route | methode | wat het doet |
|---|---|---|
| `/` | GET | de pagina |
| `/status.json` | GET | de hele stand: voeding, wifi, monitors, heap, uptime |
| `/cfg.json` | GET | de hele stand van `NodePrefs` in één verzoek |
| `/acl.json` | GET | toegangslijst, stand van het slot, buurtlijst |
| `/wifi` | POST | netwerkinstellingen |
| `/hook` | GET + POST | uitslag van buiten aanleveren |
| `/monitor`, `/monitor/del` | POST | monitor aanmaken, verwijderen |
| `/acl`, `/acl/del`, `/acl/strict` | POST | toegang beheren |
| `/cli` | POST | de enige schrijfweg voor nodebeheer |

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
de voorwaarde om hem als keuzelijst voor de ACL te gebruiken. De lijst houdt
twaalf ingangen, voegt samen op sleutel (een echte ring zou na twaalf adverts van
één drukke buur twaalf keer diezelfde buur tonen) en staat **niet** in SPIFFS: hij
beschrijft wat er nú in de lucht is, en een bewaarde versie zou beweren dat een
node in de buurt is die er al een week niet meer is.

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

De WiFi-gegevens staan **niet** in deze repo. Kopieer
[../firmware/wifi.ini.voorbeeld](../firmware/wifi.ini.voorbeeld) naar de
`platformio.local.ini` van je MeshCore-bouwkopie (dat bestand staat in MeshCore's
eigen `.gitignore`) en vul je eigen netwerk in.

Er is één patch op MeshCore nodig,
[0001-sensor-manager-class-heltec-v3.patch](../firmware/patches/0001-sensor-manager-class-heltec-v3.patch):
de global `sensors` is in `variants/heltec_v3/target.{h,cpp}` hardgecodeerd op
`EnvironmentSensorManager`, en zonder die drie regels is er geen enkele plek waar
een toepassing een andere `SensorManager` kan opgeven zonder de variant te
forken. Zonder de macro's bouwt de variant exact wat hij ervoor bouwde, dus
upstream-gedrag blijft ongewijzigd.

    python -m platformio run -e meshuptime
    python -m platformio run -e meshuptime -t upload --upload-port COM4

`upload` schrijft alleen de programmapartitie en niet SPIFFS: de identiteit in
`/identity/_main.id` en het instellingenbestand blijven dus staan. Wil je een
nieuwe sleutel, dan moet dat bestand of SPIFFS gewist worden — een aparte
handeling.
