# Kernpatch: twee identiteiten

Deze map bestaat omdat MeshUptime één ding doet dat upstream MeshCore niet
ondersteunt: **twee radio-identiteiten in één node**. Eén als companion/sensor
voor de bewaking, één als repeater voor het doorsturen.

Deze keuze is bewust gemaakt na een uitdrukkelijk bezwaar (zie
`docs/ontwerp.md`, sectie "Twee identiteiten, of de juiste rol?"). Upstream heeft
er geen enkel voorbeeld van, en SIREN — waar het idee vandaan komt — doet het
ook niet: die zet `self_id = rooms[0].id` en laat de routering ongemoeid.

De prijs staat hier zodat hij vindbaar blijft: **deze patch moet bij elke
upstream-versie opnieuw aangebracht en opnieuw nagekeken worden.** Dat is precies
wat in MeshStats tijd gekost heeft. Daarom staat de patch als apart bestand naast
de firmware en niet als een vork van de hele boom.

## Waar de kern één identiteit veronderstelt

In `src/Mesh.cpp` (upstream `companion-v1.17.0`):

| regel | gebruik | rol bij twee identiteiten |
|---|---|---|
| 58 | `self_id.isHashMatch(payload+offset, 1<<path_sz)` | flood: **repeater** |
| 89 | `self_id.isHashMatch(pkt->path, ...)` | pad: **repeater** |
| 147 | `self_id.isHashMatch(&dest_hash)` | bestemming: **beide** |
| 207 | `self_id.isHashMatch(&dest_hash)` | bestemming: **beide** |
| 211 | `self_id.calcSharedSecret(secret, sender)` | moet de identiteit gebruiken **die matchte** |
| 263 | `self_id.matches(id.pub_key)` | eigen advert herkennen: **beide** |
| 349 | `self_id.copyHashTo(&packet->path[...])` | pad stempelen: **repeater** |
| 463, 504 | `self_id.copyHashTo(&packet->payload[len])` | afzender: **rol die verzendt** |

Regel 211 is de gevaarlijke. Als regel 207 op identiteit B matcht en 211 het
geheim met identiteit A berekent, ontcijfert het pakket niet en is de fout stil:
geen foutmelding, alleen een bericht dat nooit aankomt. Wie de patch aanbrengt,
moet die twee regels als één geheel behandelen.

## De valkuil die SIREN al tegenkwam

De adreshash is **één byte** (`PAYLOAD_VER_1`). Twee willekeurige sleutels hebben
dus **1 kans op 256** dat hun hash gelijk is. Gebeurt dat, dan is de routering
dubbelzinnig: elk pakket voor de ene identiteit matcht ook de andere.

Dit is geen theorie. In `docs/replication-protocol.md` van SIREN staat:

> **JES-732**: Dest-hash collision fix (prerequisite for correct multi-room
> replication)

Dus: **bij het genereren van het tweede sleutelpaar moet gecontroleerd worden dat
de hash verschilt van de eerste, en zo niet, opnieuw genereren.** Dat kost drie
regels bij het aanmaken en voorkomt een storing die zich als "soms komt een
bericht niet aan" zou voordoen — het soort fout dat weken kost.

## Aanpak

1. Een virtuele `bool matchesSelf(const uint8_t* hash, uint8_t len, LocalIdentity** which)`
   op `Mesh`, standaard alleen `self_id`. Dan blijft upstream-gedrag ongewijzigd
   voor elk ander voorbeeld en is de patch klein.
2. `MeshUptimeMesh` overschrijft die en geeft terug welke identiteit matchte.
3. Regel 211 gebruikt de teruggegeven identiteit, niet `self_id`.
4. Twee adverts met een eigen schema: `ADV_TYPE_SENSOR` voor de bewaking,
   `ADV_TYPE_REPEATER` voor het doorsturen. Twee adverts betekent twee keer
   zendtijd voor aankondigingen — dat hoort in het zendtijdbudget
   (`_prefs.airtime_factor`) meegerekend te worden.
5. Botsingscontrole bij het aanmaken van de tweede sleutel.

## Voorwaarde

De repeaterrol is de **laatste** stap, na de bewaking. Tot die tijd draait er
één identiteit en is deze patch niet aangebracht. Dat is ook de veiligste
volgorde: zolang er geen tweede identiteit is, kan de routering niet dubbelzinnig
worden.
