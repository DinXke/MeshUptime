# Wat er niet werkt

Dit is het belangrijkste bestand van deze documentatie. Wie dit project na een
half jaar oppakt, heeft hier meer aan dan aan de rest samen: het staat vol met
dingen die al onderzocht zijn en die je dus niet nog eens hoeft te onderzoeken.

Volgorde is naar belang.

---

## 1. MeshManager kan deze sensoren niet over het mesh uitlezen

**De uitvraagronde strandt op `LOGIN_NOANSWER`** — `lr=2` in `/api/mon` van de
repeater. De node antwoordt niet op de login, dus er komt nooit een
telemetrieverzoek.

### Wat vaststaat

- De repeater draait MeshManager **2.9.0**.
- De repeater **hoort het advert** van deze node: hij staat in de gehoorde lijst
  met `t: 4` (`ADV_TYPE_SENSOR`).
- De prefix is opgelost en de node staat in MeshManager als monitoringang met
  `res=1`.
- **Andere nodes worden door dezelfde repeater succesvol uitgevraagd.** Het spoor
  toont voor die nodes `login OK`, `status ok`, `nbrs len=144`, `pub`. De
  repeater monitort dus wel degelijk; alleen deze node niet.
- De documentatie van MeshManager zegt zelf waarom dit zo moeilijk te vangen is:
  *"A refused login cannot be told from an unreachable one — the far side answers
  a rejected login with silence."* Vandaar de naam `LOGIN_NOANSWER` in plaats van
  een bewering over welk van de twee het is.

### Vier verdenkingen die ONDERZOCHT EN WEERLEGD zijn

Dit is de waardevolste kennis op deze pagina. **Jaag ze niet opnieuw na.**

**1. De klok van de node stond op 15 mei 2024.** Dat is de vaste terugvalwaarde
van de ESP32-klok: `ESP32RTCClock::begin()` zet bij een koude start
`1715770351` (`src/helpers/ESP32Board.h:208`), en de Heltec V3 gebruikt die klok
als terugval achter `AutoDiscoverRTCClock` (`variants/heltec_v3/target.cpp:15`).
Er zit geen batterijgevoede klok op dit bord, dus na elke koude start gebeurt dat
opnieuw. Een login draagt een tijdstempel en `handleLoginReq` weigert een
tijdstempel die niet nieuwer is dan de vorige, dus dit was een goede verdenking.
**De klok is goed gezet en de fout bleef.**

*Correctie op de eerdere aantekening:* die verwees naar `CommonCLI.cpp:188`. Daar
staat dezelfde constante, maar dat is de opdracht `clkreboot` — een handeling die
je zelf aanroept, niet het terugvalgedrag bij het opstarten. Dezelfde waarde staat
ook in `VolatileRTCClock` (`src/helpers/ArduinoHelpers.h:11`), die dit bord niet
gebruikt.

**2. Het slot `acl.strict`.** Weerlegd door de code te lezen én door te proberen.
`acl_strict` grijpt op precies één plaats in: in `handleRequest`, bij
`REQ_TYPE_GET_TELEMETRY_DATA`. In `handleLoginReq` komt het niet voor, dus het
slot kan een login niet weigeren. **Met het slot uit: zelfde fout.**

**3. Het gedeelde geheim van een ACL-ingang die via de webinterface is
toegevoegd.** `handleLoginReq` zet `client->shared_secret` alleen in de
wachtwoord-tak; in de tak voor een leeg wachtwoord (een node die al in de lijst
staat) gebeurt dat niet. Dat is waar — maar `aclSetPerms` rekent het geheim bij
het toevoegen al uit (rechtstreeks met `calcSharedSecret`, of via
`ClientACL::applyPermissions`), dus een ingang uit de webinterface mist het niet.
**Ingang gewist en een login met het wachtwoord gedaan: zelfde fout.**

**4. Het advert-type.** De kandidatenlijst van MeshManager filtert op `ADV_TYPE`
(`MeshManagerNet.cpp`, rond regel 6784). Die lijst liet eerst alleen
`ADV_TYPE_REPEATER` toe, wat betekende dat juist het ene soort node dat bestaat om
telemetrie te antwoorden niet aangeboden werd. **Een echte wisselwerking, en
inmiddels ook opgelost — maar niet de oorzaak:** het was een gat in het
*ontdekken*, geen blokkade. `mon add` en de uitvraagronde nemen een expliciete
sleutel en hebben nooit naar het adverttype gekeken, en deze node stond al met
`res=1` in de monitorlijst.

### De volgende diagnostische stap, nog niet gedaan

**`wifi mon trace` uitlezen op de repeater**, over de seriële console of via de
beheerpagina van die repeater. Dat spoor is de enige plek waar te zien is hoe ver
de ronde voor déze node komt: of de login überhaupt uitgaat, over welk pad, en of
er iets terugkomt dat niet als antwoord herkend wordt. Zonder dat spoor is elke
volgende verdenking weer gokken, want stilte aan onze kant en stilte aan de
overkant zien er hetzelfde uit.

Wat daarna in beeld komt, in deze volgorde: klopt het pad (flood versus direct,
en het aantal hops), en hoort het wachtwoord dat MeshManager voor deze ingang
gebruikt wel bij deze node — de MeshManager-kant kapt een wachtwoord af op 15
tekens.

---

## 2. Het beheerderswachtwoord staat nog op de bouwvlagwaarde

`ADMIN_PASSWORD` in de bouwvlaggen is nooit gewijzigd, en dat is geen
schoonheidsfoutje: **wie die waarde weet, logt over het mesh in, wordt automatisch
beheerder en heeft niet alleen de sensoren maar de hele CLI.** Een login met het
juiste wachtwoord voegt de afzender toe met `PERM_ACL_ADMIN`, ook met het slot
`acl.strict` dicht.

Het slot en het wachtwoord zijn daarom samen één beveiliging en niet twee. Zet
`password <nieuw>` over de seriële console; de opgeslagen waarde gaat voor op de
gebakken. De webinterface vergelijkt met de bouwvlag en toont een banner tot het
gezet is.

Hetzelfde geldt voor de webinterface: `WEB_USER` en `WEB_PASS` staan op hun
voorbeeldwaarde in de code. En **Basic-auth over gewone HTTP stuurt het wachtwoord
leesbaar over het netwerk** — verdedigbaar op een eigen LAN, niet genoeg voor een
node die van buiten bereikbaar wordt.

---

## 3. De waarschuwingen zijn gebouwd maar nooit afgegaan

De hele keten staat er — de voorwaarden per monitorvakje, de wachtrij met vier
plaatsen, de keuze van ontvangers op `PERM_RECV_ALERTS_LO` / `_HI`, het herhalen
en het wachten op een echte ACK — maar er is **nooit een echte waarschuwing over
de radio gegaan**. Dit is dus "gebouwd", niet "gebouwd en gezien".

Dat is het slechtste soort onbekend: een node die pas bij een echte storing voor
het eerst een bericht stuurt, is een node waarvan niemand weet of dat bericht
aankomt — en dan blijkt het op het moment dat het uitkomt.

In de werkmap staat werk in uitvoering om dit te kunnen uitlokken: een
simulatiestand die een sensor tijdelijk forceert en het bericht daarmee door het
**echte** pad stuurt, met een verplichte vervaltijd zodat een node niet in
testmodus blijft hangen. Dat werk is nog niet vastgelegd en de bouw is er tijdelijk
door stuk; deze pagina wordt bijgewerkt zodra er iets te melden is dat op het
toestel gezien is.

---

## 4. Twee stack-overflows in upstream CommonCLI, nog niet upstream gemeld

Beide zitten in `src/helpers/CommonCLI.cpp` en zijn niet van dit project. De rand
is dichtgezet aan de kant die wij bezitten, maar de fout staat er nog.

**`sensor list`.** De lus schrijft met een **ongebonden `sprintf`** en stopt pas
als de schrijfpositie 134 voorbij is; daarna komt er nog `... next:N` bij:

    for (i = start; i < end && (dp-reply < 134); i++) {
      sprintf(dp, "%s=%s\n", getSettingName(i), getSettingValue(i));
      ...
    }
    if (i < end) sprintf(dp, "... next:%d", i);

Die grens van 134 veronderstelt korte waarden. Met een regel als
`mon.5.host=<een IP-adres>` staat de positie na die regel op 156, en de elf byte
van `... next:8` brengen het op **168 in een buffer van 160**. Gemeten: de node
ging om in `Guru Meditation Error (LoadProhibited)`, `EXCVADDR: 0x0000000c`, en
wel precies op `sensor list` en `sensor list 0` terwijl `sensor list 1` het
overleefde — met beginindex 1 eindigt het op 141 en past het net.

**Dit is over het mesh bereikbaar**, want dezelfde CLI is per DM aan te roepen
door een beheerder. Onze kant: `reply[160]` → `reply[256]` in `main.cpp` en
`temp[166]` → `temp[262]` in `SensorMesh.cpp`. Met een hostnaam van maximaal 40
tekens is de bovengrens 134 + 53 + 12 = 199, dus 256 geeft ruimte.

**`sensor set`.** `strcpy(tmp, &command[11])` zonder lengtecontrole, in
`CommonCLI.cpp:284`, naar de buffer `char tmp[PRV_KEY_SIZE*2 + 4]` die op
`CommonCLI.h:258` gedeclareerd staat. Met `PRV_KEY_SIZE` 64 is dat **132 byte**,
en de opdrachtregel mag 160 lang zijn: een `sensor set` van meer dan 143 tekens
loopt dus over. Niet gemeten, wel na te rekenen.

*Correctie op de eerdere aantekening:* daar stond "een buffer van 68 byte op
`CommonCLI.h:258`". De regel is juist, de maat niet — 68 zou gelden bij
`PRV_KEY_SIZE` 32. En de `strcpy` zelf staat in het `.cpp`, niet in de header.

**Beide horen upstream gemeld te worden.** Dat is nog niet gebeurd.

---

## 5. De DM-opdrachten bereiken alleen een beheerder

`SensorMesh::onPeerDataRecv` laat een tekstbericht alleen naar
`handleIncomingMsg` (en dus naar `DmCommands`) doorlopen als de afzender
`from->isAdmin()` is. De rechtentoets in `DmCommands::isAllowed()` is ruimer — hij
laat ook leesrecht en de twee waarschuwingsbits toe — maar die code wordt in de
praktijk nooit met iets anders dan een beheerder bereikt.

Openzetten voor leescontacten is het weghalen van die voorwaarde en de toets aan
`isAllowed()` laten. **Bewust niet gedaan:** rechten verruimen is een keuze van de
eigenaar en niet van wie de code schrijft.

Het gevolg dat het lastig maakt: `list` is de enige weg waarlangs een naam over
het mesh gaat, dus een node met alleen leesrecht kan telemetrie opvragen maar niet
te weten komen wat kanaal 7 betekent.

---

## 6. `region load` doet stil niets, en `region save` faalt

`CommonCLI` roept voor `region load` de callback `startRegionsLoad()` aan. Die is
in `CommonCLI.h` een virtuele methode met een **leeg** lichaam, en `SensorMesh`
overschrijft hem niet — alleen `simple_repeater` en `simple_room_server` doen dat.
De opdracht zet ook geen antwoordtekst, dus je krijgt letterlijk niets terug en
niets gebeurt.

`region save` gaat dezelfde weg langs `saveRegions()`, die standaard `false`
teruggeeft; die opdracht antwoordt dus met `Err - save failed`.

Voor deze rol is dat geen ramp — een sensornode heeft geen regiokaart nodig — maar
het is een opdracht die in de webconsole bestaat en niet doet wat hij belooft.

---

## 7. Opdrachten die antwoorden zonder iets te doen

Staat inmiddels ook op de beheerpagina, maar hoort hier bij elkaar:

- `neighbors` antwoordt `"not supported"`: `formatNeighborsReply()` is in deze rol
  een vaste tekst. De echte buurtlijst staat op het toegangstabblad van de
  webinterface, uit `onAdvertRecv()`.
- `log …` en `clear stats` idem: de callbacks zijn in deze rol leeg.
- `get pass` bestaat niet; het beheerderswachtwoord is alleen te **zetten**.
- `bw`, `sf` en `cr` hebben geen eigen `set`: alleen `set radio <f,bw,sf,cr>`.
  Gunstig eigenlijk, want je kunt ze niet half zetten.

---

## 8. Kanaalbotsing met de basisklasse

`EnvironmentSensorManager::querySensors()` zet zijn kanaalteller terug op 2 en
deelt vanaf daar uit aan gevonden I2C-omgevingssensoren — bovenop onze vaste
kanalen 2, 3 en 4. In het formaat botst dat niet (per kanaal mogen meerdere types
staan en een decoder kijkt naar het type), maar een dashboard wordt er verwarrend
van.

Niet op te lossen zonder in de basisklasse te snijden, want de reset gebeurt
binnen de ouder. Deze node heeft geen omgevingssensoren aangesloten, en `begin()`
waarschuwt in de debug-uitvoer zodra er één opduikt.

---

## 9. De repeaterrol is er nog niet

Doorsturen staat uit (`disable_fwd`). De basis heeft `disable_fwd` en `flood_max`,
dus aanzetten is een vlag omzetten — maar de vier repeatervelden die een echte
repeater gebruikt (`flood_max_unscoped`, `flood_max_advert`, `loop_detect`, en de
regio- en transportcodecontrole) zijn in deze rol niet allemaal in beeld. Zolang
dat zo is, is een repeaterknop in de webinterface een knop die schade kan doen.

De tweede identiteit die daar eerder bij hoorde, is een keuze die is **afgewezen**
— zie [beslissingen.md](beslissingen.md). De uitwerking van die patch staat
bewaard in [../firmware/patches/LEESMIJ.md](../firmware/patches/LEESMIJ.md) voor
het geval de keuze ooit omgaat; hij is **niet** aangebracht.

---

## 10. Fragmentatie door de webserver

Het grootste vrije blok zakte van 225.268 naar 163.828 na het bedienen van
webverzoeken. Ruim genoeg, maar dit is de maat die dit project vanaf het begin
bepaald heeft, en de pagina is sindsdien van 11 kB naar 66 kB gegroeid. Wie hier
iets toevoegt, hoort vóór en ná te meten.

---

## 11. Dingen die er niet meer zijn, met opzet

Geen gebrek, maar wel het soort verrassing dat iemand een avond kost:

- **Poort 5000 en daarmee de mesh-verbinding van Home Assistant.** Deze node was
  de gateway; die rol is opgegeven bij de wissel naar `simple_sensor`. De oude
  firmware en sleutel staan buiten de repo in een back-up.
- **De oude sleutel.** De node heeft een nieuw sleutelpaar; wie hem als contact
  had, moet hem opnieuw toevoegen.
