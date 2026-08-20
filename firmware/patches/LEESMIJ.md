# Patches op MeshCore

Elke patch op de kern moet bij **elke** nieuwe upstream-versie opnieuw
aangebracht en opnieuw nagekeken worden. Dat is precies wat in MeshStats tijd
gekost heeft. Daarom staat elke patch hier als apart bestand naast de firmware en
niet als een vork van de hele boom, en daarom is "kan het zonder patch?" bij elke
ronde de eerste vraag.

Er is er op dit moment **één**, en dat moet zo blijven.

## 0001 — SENSOR_MANAGER_CLASS voor variants/heltec_v3 (AANGEBRACHT, NODIG)

`0001-sensor-manager-class-heltec-v3.patch`, tegen tag `companion-v1.17.0`.

    cd <meshcore-kopie>
    git apply ../MeshUptime/firmware/patches/0001-sensor-manager-class-heltec-v3.patch

De volledige reden staat in het patchbestand zelf, met de drie doodlopende wegen
die eraan voorafgingen. Kort: MeshUptime moet eigen velden in het
telemetrieantwoord zetten, en dat kan alleen vanuit een eigen `SensorManager` —
`handleRequest` is niet virtueel, `reply_data` is private, en `handleRequest`
roept `onSensorDataRead()` niet aan. De global `sensors` is echter in
`variants/heltec_v3/target.{h,cpp}` hardgecodeerd op `EnvironmentSensorManager`.

Drie regels vervangen en een macro met terugval toevoegen. Zonder
`SENSOR_MANAGER_CLASS` bouwt de variant exact wat hij ervoor bouwde, dus het
upstream-gedrag is ongewijzigd en de patch is kandidaat om ooit stroomopwaarts
aan te bieden.

Bij het nakijken na een upstream-versie: de constructor van de eigen klasse moet
dezelfde vorm hebben als die van `EnvironmentSensorManager` — met
`LocationProvider&` als `ENV_INCLUDE_GPS` aan staat (dat is zo voor Heltec V3),
zonder argumenten als hij uit staat.

---

## Twee radio-identiteiten (AFGEWEZEN, bewaard als ontwerp)

**Deze patch bestaat niet en is niet aangebracht.** De node draait op **één**
radio-identiteit en kondigt zich aan als `ADV_TYPE_SENSOR`. Waarom die keuze zo
gevallen is staat in [../../docs/beslissingen.md](../../docs/beslissingen.md),
sectie "Eén radio-identiteit, niet twee": doorsturen is in MeshCore niet gebonden
aan het advert-type, drie van de vier upstream-voorbeelden sturen door mét een
eigen toepassing, en wat er werkelijk fout is aan een node die doorstuurt is een
verkéérd advert-type — niet het ontbreken van een tweede identiteit.

Wat hieronder staat is dus geen plan maar het **ontwerp van een patch die er niet
is**, bewaard voor het geval de keuze ooit omgaat. Het is opgeschreven omdat het
werk al gedaan was, en omdat een halve herinnering hiervan gevaarlijker is dan
geen.

Ter correctie van de aanname waar het idee uit kwam: SIREN doet dit **niet**. Die
zet `self_id = rooms[0].id` (`firmware/examples/siren_room_server/MyMesh.cpp:260`)
en laat de routering in `src/Mesh.cpp` ongemoeid; de sleutels van room 1..N leven
boven de meshlaag. In `docs/architecture.md` van SIREN staat room 0 met zoveel
woorden als de identiteitslaag beschreven.

### Waar de kern één identiteit veronderstelt

In `src/Mesh.cpp` (upstream `companion-v1.17.0`; alle negen regelnummers
nagekeken):

| regel | gebruik | rol bij twee identiteiten |
|---|---|---|
| 58 | `self_id.isHashMatch(&pkt->payload[...], 1<<path_sz)` | flood: **repeater** |
| 89 | `self_id.isHashMatch(pkt->path, ...)` | pad: **repeater** |
| 147 | `self_id.isHashMatch(&dest_hash)` | bestemming: **beide** |
| 207 | `self_id.isHashMatch(&dest_hash)` | bestemming: **beide** |
| 211 | `self_id.calcSharedSecret(secret, sender)` | moet de identiteit gebruiken **die matchte** |
| 263 | `self_id.matches(id.pub_key)` | eigen advert herkennen: **beide** |
| 349 | `self_id.copyHashTo(&packet->path[...])` | pad stempelen: **repeater** |
| 463, 504 | `self_id.copyHashTo(&packet->payload[len])` | afzender: **rol die verzendt** |

Regel 211 is de gevaarlijke. Als regel 207 op identiteit B matcht en 211 het
geheim met identiteit A berekent, ontcijfert het pakket niet en is de fout stil:
geen foutmelding, alleen een bericht dat nooit aankomt. Die twee regels horen als
één geheel behandeld te worden.

### De valkuil die SIREN al tegenkwam

De adreshash is **één byte** (`PAYLOAD_VER_1`). Twee willekeurige sleutels hebben
dus **1 kans op 256** dat hun hash gelijk is. Gebeurt dat, dan is de routering
dubbelzinnig: elk pakket voor de ene identiteit matcht ook de andere.

Dit is geen theorie. In `docs/replication-protocol.md` van SIREN staat:

> **JES-732**: Dest-hash collision fix (prerequisite for correct multi-room
> replication)

SIREN loste het achteraf op door bij een botsing alle passende kamers te proberen
en de kamer te nemen waar het ontcijferen lukt. Voor twee identiteiten is de
goedkopere weg vooraf: **bij het genereren van het tweede sleutelpaar
controleren dat de hash verschilt van de eerste, en zo niet, opnieuw genereren.**
Dat kost drie regels bij het aanmaken en voorkomt een storing die zich als "soms
komt een bericht niet aan" zou voordoen — het soort fout dat weken kost.

### De aanpak, als het er ooit van komt

1. Een virtuele
   `bool matchesSelf(const uint8_t* hash, uint8_t len, LocalIdentity** which)`
   op `Mesh`, standaard alleen `self_id`. Dan blijft upstream-gedrag ongewijzigd
   voor elk ander voorbeeld en is de patch klein.
2. De eigen mesh-klasse overschrijft die en geeft terug welke identiteit matchte.
3. Regel 211 gebruikt de teruggegeven identiteit en niet `self_id`.
4. Twee adverts met een eigen schema: `ADV_TYPE_SENSOR` voor de bewaking,
   `ADV_TYPE_REPEATER` voor het doorsturen. Twee adverts betekent twee keer
   zendtijd voor aankondigingen, en dat hoort in het zendtijdbudget
   (`_prefs.airtime_factor`) meegerekend te worden.
5. Botsingscontrole bij het aanmaken van de tweede sleutel.

De prijs van níet patchen is dat de node in de MeshCore-app in één lijstje staat
en niet in twee. Dat is de hele prijs.
