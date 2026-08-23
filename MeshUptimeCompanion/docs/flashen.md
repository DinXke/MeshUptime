# MeshUptimeCompanion — flashen, sleutel-veilig

**Lees dit vóór je een T1000-E met deze firmware flasht.** De node heeft een
MeshCore-identiteit (privé-/publieke sleutel) plus zijn instellingen
(allowlist, presets, tunes) op het toestel staan, meest waarschijnlijk in een
**LittleFS**-bestandssysteem op het nRF52840-flashgeheugen — net zoals de
Heltec-varianten van MeshUptime hun identiteit en instellingen in hun eigen
bestandssysteem bewaren en een gewone `upload` daar niet aankomt (zie
[../../docs/werking.md](../../docs/werking.md): *"upload schrijft alleen de
programmapartitie en niet SPIFFS"*). Hetzelfde principe geldt hier, met een
andere chip en een ander bestandssysteem. **Verlies je die sleutel, dan is het
toestel voor het mesh een compleet nieuw, onbekend contact** — iedereen die
het als bekende companion had, moet het opnieuw toevoegen, en er is geen weg
terug naar de oude identiteit.

## De procedure, in drie stappen

### 1. Eerst een volledige back-up, via de bootloader

De nRF52840 op de T1000-E draait (zoals gebruikelijk bij dit soort borden) een
**UF2-bootloader**. Die roep je op door het toestel **twee keer snel achter
elkaar te resetten** ("double-reset"): het toestel verschijnt daarna als een
gewoon USB-opslagstation.

Op dat opslagstation staat (bij de gangbare Adafruit-achtige nRF52-bootloader)
een bestand **`CURRENT.UF2`**: een dump van wat er nu op het toestel staat.
**Kopieer dat bestand eerst naar je PC, vóór je iets nieuws flasht.** Dat is
je terugvalpositie.

### 2. Flash alleen de app-regio

Flashen gaat met een `.uf2`-bestand: ofwel door het naar het opstart-station
te slepen, ofwel via `pio run -t upload` (PlatformIO doet dat slepen zelf).
Een normale app-upload beschrijft **alleen de programmaregio**, niet het
bestandssysteem waar de sleutel en de instellingen in staan — analoog aan hoe
`upload` bij de ESP32-varianten van MeshUptime SPIFFS met rust laat. Na het
flashen start de nieuwe firmware met **dezelfde** identiteit en instellingen
die er al stonden.

### 3. Controleer de public key ná het flashen

Voor je het toestel weer vertrouwt: vraag zijn **public key** op (over de
seriële console, of via `!cfg`/de webweergave zodra die bestaat) en vergelijk
hem met wat je van vóór het flashen had. **Onveranderd = geslaagd.** Een
andere sleutel — of een sleutel die er niet meer is — betekent dat de
identiteit toch is aangetast, en dan is `CURRENT.UF2` van stap 1 je enige weg
terug.

## Wat NOOIT mag

**Nooit een volledige chip-erase** ("full-chip erase", soms een aparte
bootloader-optie of een `nrfjprog --eraseall`-achtig commando). Dat wist ook
het bestandssysteem waar de sleutel in staat, en dat is onomkeerbaar zonder de
back-up van stap 1. Als iets in het flashproces om een "volledige wis" vraagt
voordat je een nieuw beeld zet, is dat een teken om te stoppen en eerst
`CURRENT.UF2` veilig te stellen — niet om door te klikken.

## Wat hier nog bevestigd moet worden op het echte toestel

Dit is een sleutel-veilige procedure **op papier**, opgesteld naar het
patroon dat elders in dit ecosysteem al klopt (de ESP32-`upload`-garantie).
Drie dingen staan nog niet vast omdat de firmware en de partitie-indeling
parallel gebouwd worden:

- **Of `CURRENT.UF2` op de T1000-E werkelijk het hele beeld dumpt**, inclusief
  het bestandssysteem met de sleutel, of alleen de programmaregio. Sommige
  nRF52-bootloaderversies doen het eerste, sommige het tweede — dat verschil
  bepaalt of stap 1 daadwerkelijk een sleutel-back-up is of alleen een
  firmware-back-up.
- **De exacte adresgrens** tussen de app-regio en het bestandssysteem op dít
  bord, en of de PlatformIO-`upload`-target die grens ook werkelijk
  respecteert zoals aangenomen.
- **De naam van het opstart-station en het aantal/tempo van de
  double-reset-tikken** — dat verschilt per bootloaderversie en is nog niet
  met de hand op een T1000-E nagekeken voor dit project.

Tot die drie punten op een echt toestel bevestigd zijn: behandel `CURRENT.UF2`
als je enige harde vangnet, en flash de eerste keer op een manier waarbij je
die back-up al hebt liggen — niet achteraf.
