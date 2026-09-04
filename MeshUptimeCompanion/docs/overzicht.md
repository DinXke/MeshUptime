# MeshUptimeCompanion — overzicht

**Dit bestand beschrijft een ONTWERP.** De firmware wordt parallel gebouwd door
een andere sessie/agent; niets hieronder is op een echt toestel geverifieerd
tenzij dat er expliciet bij staat. Waar het gedrag nog kan verschuiven — een
knopactie, een precieze drempel, een RTTTL-string — staat dat met zoveel
woorden vermeld. Beschouw dit als het contract waartegen de firmware gebouwd
wordt, niet als een verslag van wat hij al doet.

## Wat het is

**MeshUptimeCompanion** is eigen companion-firmware voor de **Seeed T1000-E**,
die van dat kaartje een draagbare **annunciator/afstandsbediening** maakt voor
het MeshUptime-bewakingsmesh (zie de hoofd-repo, [../../README.md](../../README.md)
en [../../docs/werking.md](../../docs/werking.md)). Geen scherm, geen app: een
DM met een gekleurde stip erin komt binnen, en het kastje aan je pols of
sleutelbos speelt er een herkenbaar deuntje bij. Andersom kun je het kastje
sturen — vind het terug, demp het, vraag zijn locatie op — met een kort
commando, zonder dat je bij de MeshUptime-node zelf hoeft te zijn.

Het idee is dus tweeledig:

- **Pager**: binnenkomende waarschuwingen van MeshUptime worden hoorbaar en
  onderscheidbaar naar prioriteit, zonder dat je een scherm moet aflezen.
- **Afstandsbediening**: een kleine, met een allowlist beveiligde
  commandoset laat je het kastje op afstand bedienen — vanaf de MeshUptime-bot
  zelf, vanaf **MeshManager**, of rechtstreeks over een DM.

## De hardware (Seeed T1000-E)

| onderdeel | rol | zekerheid |
|---|---|---|
| **nRF52840** (SoC) | draait de firmware, BLE/USB, flash | vast |
| **LR1110** | LoRa-radio (MeshCore-transport) én GNSS-scanfunctie | vast qua radio; de precieze werkwijze van de locatiebepaling (zie *GPS* hieronder) staat nog niet vast |
| **RTTTL-buzzer** (piëzo) | speelt de per-severity deuntjes en het vind-me-signaal | vast |
| **QMA6100P** (accelerometer) | valdetectie | vast qua aanwezigheid; de gevoeligheid/false-positive-afweging is firmwarewerk in uitvoering |
| **groene LED** | statusindicatie naast geluid (bv. bij mute, bij GPS-fix) | aanwezig op het bord; welk gedrag eraan hangt is nog niet vastgelegd |
| **knop** | korte/lange druk, stopt ook vind-me | aanwezig; het exacte drukpatroon per functie staat in [gebruik.md](gebruik.md) met de nog openstaande vragen erbij |
| **GPS** | locatie op verzoek of doorlopend | zie hieronder — dit is het meest onzekere hardware-punt |
| **kleine LiPo** | voeding, draagbaar | vast; het stroombudget (RXPS, GPS-on-demand) staat in [gebruik.md](gebruik.md) |

**Over GPS, met opzet voorzichtig geformuleerd.** De LR1110 heeft een
GNSS-scanfunctie die typisch ruwe satellietdata levert in plaats van zelf een
klaar-berekende positie — bij veel LR1110-toepassingen wordt die ruwe scan
naar een cloud-solver gestuurd om er een coördinaat uit te halen. Een
mesh-toestel zonder internetverbinding heeft die cloud-weg niet. Of
`!gps`/`!loc` (zie [protocol.md](protocol.md)) een echte autonome fix leveren,
een grovere schatting, of überhaupt al werken op het huidige firmwarestadium,
is **niet bevestigd** in dit document — dat hoort bij de firmware-agent thuis
en wordt hier niet voorgewend zeker te zijn.

## Hoe het in het ecosysteem past

```
MeshUptime-node (room-server)               MeshManager (meshmanager.net)
        │  alert-DM: "🔴 UDM_Pro plat"                │  Send-DM-tab
        │  (severity-marker vooraan, zie protocol.md) │  ("!vol 2" naar T1000-E)
        ▼                                              ▼
   ────────────────────────────────────────────────────────
                      MeshCore-mesh (LoRa)
   ────────────────────────────────────────────────────────
                              │
                              ▼
                    Seeed T1000-E + MeshUptimeCompanion
                    (speelt tune, of voert commando uit)
```

Twee onafhankelijke ingangen op dezelfde DM-inbox van de companion:

1. **Alert-DM's van de MeshUptime-bot.** De bot (`BE-HSS-DinX-Bot`, zie
   [../../docs/werking.md](../../docs/werking.md) → *De bot*) stuurt bij een
   status­wijziging een schone DM die begint met een severity-emoji. De
   companion herkent die marker, kiest het bijbehorende deuntje en speelt het
   — verder gebeurt er niets met de tekst erachter dan hem eventueel
   te tonen/loggen (geen scherm, dus in de praktijk: niets).
2. **Stuurcommando's**, met een `!`-voorvoegsel over DM (zonder voorvoegsel
   over serieel). Die komen van de eigenaar zelf of, in principe, van
   MeshManager's **Send-DM**-tab, die een tekstbericht naar elke bekende
   pubkey kan sturen — inclusief die van de T1000-E. Beide wegen zijn aan een
   **allowlist** onderworpen; zie [protocol.md](protocol.md).

Dat de MeshUptime-bot en de companion **twee aparte identiteiten met twee
aparte firmwares** zijn is bewust: de bot is de vaste, netgevoede
room-server; de companion is het draagbare, batterijgevoede tegenovergestelde
uiteinde van diezelfde waarschuwingsketen. Beide praten hetzelfde
MeshCore-protocol en hebben verder niets gemeen.

## Documentatie in deze map

- **[protocol.md](protocol.md)** — de twee contracten: de severity-marker
  (met de UTF-8-bytes) en de volledige DM-/CLI-commandoset.
- **[flashen.md](flashen.md)** — de sleutel-veilige flashprocedure: eerst een
  volledige back-up via de nRF52-bootloader, dan alleen de app-regio
  herschrijven, dan de public key controleren.
- **[gebruik.md](gebruik.md)** — dagelijks gebruik: wat de deuntjes betekenen,
  wat de knop doet, stille uren, vind-me, valdetectie, stroombudget, en
  bediening vanuit MeshManager.
