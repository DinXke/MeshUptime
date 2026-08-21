# Mesh-uitlezing: waarom DinX-Home de node (nog) niet uitleest

Correctie vooraf: een eerdere versie van dit document concludeerde "het is
fysiek, waarschijnlijk de antenne". **Dat was fout.** De eigenaar wees op de
filterfirmware van DinX-Home die 1-byte hashes blokkeert, en dat bleek de kern.
Onderstaande vervangt de antenne-conclusie.

## Wat gemeten en uitgesloten is (code)

Zeven verdenkingen weerlegd met tests: klok, acl.strict, gedeeld geheim,
advert-type-filter (2.10.0), scope (floodScoped op 7 sites), packetfilter-`off`,
en radio-parameters (beide 869.618 / bw 62.5 / sf 8 / cr 8 / tx 22, gelijk).
Positief bewezen: de node ontvangt de login en antwoordt (reply_len=13).

## De echte oorzaak, bewezen (21 aug)

DinX-Home draait filterfirmware met een minimale hash-grootte van **2 byte**
(`/api/filter` → `hash: 2`); 1-byte hashes worden gedropt. De node zond met
`path.hash.mode = 0`, dus `getPathHashSize() = mode + 1 = 1 byte`.

Discriminator, hard bewijs:
- **mode 0 (1-byte):** elk advert van de node laat DinX' `drop.hash` met precies
  +1 oplopen (309 → 310 → 311). De pakketten BEREIKEN DinX dus — geen antenne —
  en worden op de 1-byte-hash gedropt.
- **mode 1 (2-byte):** één advert gaf `passed +1` op DinX. Een pakket van de node
  PASSEERT nu de filter. De hash-fix werkt op pakketniveau.

`filter off` hielp eerder niet omdat de hash-minimumgate losstaat van de
per-type-regels die `off` uitzet.

## Toegepast

`set path.hash.mode 1` op de node (blijvend opgeslagen). Voor dit mesh is 2-byte
sowieso juist: DinX filtert 1-byte weg. Dit hoort de standaard te zijn voor elke
node die door DinX gemonitord wordt.

## Wat er NA de hash-fix nog tussen zit

De node passeert de filter maar staat nog niet in DinX' heard-lijst en de login
blijft NOANSWER. Er is dus nog één poort. Meest waarschijnlijke spoor, nog te
bevestigen: het advert-type staat nu op **chat** (`adv.type=1`, gezet zodat de
MeshCore-app een tekstvenster geeft), terwijl DinX de node eerder wél hoorde toen
die zich als **sensor** (type 4) aankondigde. De heard-/monitor-kandidaatlogica
kan het type meewegen. Andere kandidaten: heruitgezonden kopieën via tussenliggende
repeaters die alsnog 1-byte-segmenten dragen, of advert-rate-limiting waardoor een
2-byte advert niet vaak genoeg uitgaat.

Volgende stap (geduldige RF-observatie, geen code): met mode 1 vast, één advert per
advert-interval sturen en DinX' heard-lijst + `passed`/`drop`-tellers volgen; en
eenmalig `adv.type` op 4 zetten om te zien of de node dán in heard verschijnt (dat
onderscheidt de type-hypothese van de rest). Niet blokkerend: de IP-weg is de
productiewerkweg; de mesh-weg is de redundante.

## Les

De eigenaar had de systeemkennis (de 1-byte-filter) die de reviewer miste, en de
reviewer trok te vroeg een fysieke conclusie. Meten wees de goede kant op zodra de
juiste hypothese getest werd — maar de juiste hypothese kwam van de eigenaar.


---

## Beslissing (21 aug): server-monitoring blijft WiFi

De eigenaar heeft bepaald: DinX-Home blijft de monitor en het hele
server-monitoring-verhaal (de node uitlezen) mag over WiFi/IP blijven zoals het
nu is. De mesh-telemetrie-uitlezing naar DinX-Home wordt daarmee NIET verder
nagejaagd -- het is geen open bug meer maar een bewuste keuze.

Mesh-first geldt onverkort voor de KERN van de node, en die is al mesh-native:
- de app bericht de node over LoRa (DM-commando's list/get/status/add/edit/del/
  ping/ok) -- werkt, node staat daarom op adv.type=chat;
- de node alarmeert de companion over LoRa (alertIf, herhaal-tot-ok).

De IP-wegen (server-poll van telemetrie, web-UI, web-push) zijn expliciet de
TERUGVAL/lokale laag, niet de kern. path.hash.mode blijft op 1 (2-byte): dat is
sowieso robuuster op een mesh waar een repeater 1-byte-hashes filtert, en het
kost de app-DM's niets merkbaars.

Niet meer heropenen tenzij de eigenaar de mesh-read expliciet wél wil.
