# MeshUptimeCompanion — dagelijks gebruik

**Ontwerpdocument, geen gebruikershandleiding van een af firmware.** Wat hier
staat is hoe het toestel is bedóeld te werken; waar het nog niet vaststaat —
een precieze knopactie, of iets al op het toestel getest is — staat dat er met
zoveel woorden bij. Zie [overzicht.md](overzicht.md) voor het grote plaatje en
[protocol.md](protocol.md) voor de exacte commando's.

## De deuntjes

Elke severity krijgt een eigen, herkenbaar RTTTL-deuntje op de buzzer, zodat
je zonder te kijken al weet hoe erg het is:

| severity | bedoelde indruk | herkenning |
|---|---|---|
| 🔴 High | kort, dringend motief | onmiddellijk anders dan de rest |
| 🟠 Medium | een tussenmotief | onderscheidbaar van hoog én laag |
| 🟢 Low / herstel | een aangenaam, "opgelost"-achtig motief | klinkt bewust geruststellend |

De standaardset is **Mario-thema-geïnspireerd** — drie korte motieven in die
stijl, in de geest van de bekende power-up-/muntjes-/level-klaar-geluiden.
De exacte RTTTL-strings staan in de firmwarebron en zijn hier met opzet niet
overgenomen (zie [protocol.md](protocol.md)): ze kunnen nog wijzigen terwijl
de firmware gebouwd wordt, en drie precieze notenreeksen hier neerzetten zou
alleen maar een schijnzekerheid geven. Wil je iets anders horen, dan zet je
je eigen tune met `!tune <H|M|L|find|msg> <RTTTL>`.

Een DM zonder severity-marker (een gewoon berichtje) krijgt de aparte
`msg`-tune — een neutraal signaal, om het te onderscheiden van een echte
alert.

## De knop

| actie | bedoeld gevolg |
|---|---|
| **korte druk** | verstuurt een van de drie vooraf ingestelde preset-berichten (zie `!preset` in [protocol.md](protocol.md)) — een snelle ack/statusmelding zonder dat je iets hoeft te typen |
| **lange druk** | trekt een SOS-alert plus een GPS-fix; verstuurt beide als DM naar de eigenaar/bot |
| **druk tijdens vind-me** | stopt het `!find`-deuntje, ongeacht hoe lang je op dat moment aan het zoeken bent |

**Nog niet vastgelegd:** *welke* van de drie presets een korte druk kiest —
of dat er één vast preset is, of dat meerdere korte drukken (dubbel-/drietik)
tussen de drie schakelen. Dit is bewust hier als open vraag genoteerd in
plaats van een keuze te verzinnen die de firmware misschien niet volgt.

## Stille uren

`!quiet <sH>-<eH>` (bv. `!quiet 22-7`) onderdrukt de gewone geluiden tussen
die twee uren; `!quiet off` zet het uit. Onbeantwoorde ontwerpvraag: of een
SOS- of valmelding de stille uren mag doorbreken. Voor een pager-achtig
toestel is "ja, noodmeldingen breken door" de voor de hand liggende keuze,
maar dat staat hier niet als bevestigd feit — controleer het gedrag op het
toestel zodra dat kan.

## Vind-me

`!find` laat het toestel herhaaldelijk het vind-me-deuntje spelen — handig als
het kwijt is in een tas of onder een bank. `!findstop`, of gewoon de knop
indrukken zodra je het gevonden hebt, stopt het weer.

## Valdetectie

De **QMA6100P**-accelerometer voedt een valdetectie die aan/uit staat via
`!fall on|off`. Bij een herkende val is het bedoelde gevolg vergelijkbaar met
de lange-druk-SOS: een alert-DM, vermoedelijk met een GPS-fix erbij. De
gevoeligheidsafstelling — hoe val onderscheiden wordt van gewoon stevig lopen
of neerzetten — is firmwarewerk dat nog loopt; hier wordt niet beweerd dat de
huidige drempel al goed staat.

## Batterij en stroom

Klein LiPo-toestel, dus stroom is de beperkende factor, niet rekenkracht:

- **RXPS** (ontvangst-power-save / duty-cycled luisteren): de radio luistert
  niet continu maar in korte, herhaalde vensters, om de batterij te sparen
  ten koste van iets meer latentie op inkomende berichten. De precieze
  duty-cycle-instelling is een firmware-afweging die nog gemaakt wordt.
- **GPS on-demand** (`!gps ondemand`): de GNSS-functie staat de meeste tijd
  uit en wordt alleen aangezet voor een fix (`!loc`, of bij een SOS/val),
  in plaats van continu te loggen — dat scheelt aanzienlijk stroom ten
  opzichte van `!gps on`. Zie ook de kanttekening bij GPS in
  [overzicht.md](overzicht.md): hoe zwaar een fix precies is, hangt af van
  een detail van de LR1110-GNSS-werkwijze dat nog niet vaststaat.

Concrete cijfers over stand-by-tijd of fix-duur staan hier niet, omdat ze
alleen op een echt toestel te meten zijn — dat gebeurt zodra de firmware er is
en niet vooraf geschat.

## Bediening vanuit MeshManager

Drie wegen, van meest naar minst directe integratie:

1. **De Send-DM-tab** in MeshManager: een tekstbericht rechtstreeks naar de
   pubkey van de T1000-E, dus gewoon `!vol 2` of `!find` intypen zoals je dat
   ook over een DM-app zou doen. Dit werkt zonder dat MeshManager iets van de
   companion-commando's hoeft te "kennen" — het is generieke DM-verzending.
2. **Companion-helpers** in MeshManager: knoppen die de veelgebruikte
   commando's (mute, vind-me, `!cfg` opvragen) klaarzetten zonder dat je de
   syntax uit je hoofd moet kennen. **Dit bestaat nog niet** — het is een
   voor de hand liggende uitbreiding zodra het protocol vaststaat, niet iets
   dat al gebouwd is.
3. **Browser Web-Serial**: rechtstreeks over USB vanuit een Chromium-browser
   (Web Serial API) met het toestel praten zonder Send-DM en zonder het mesh
   — hetzelfde idee als de seriële console in [protocol.md](protocol.md),
   maar vanuit de browser in plaats van een terminal. Handig voor de eerste
   instelronde (allowlist vullen, presets zetten) vóór het toestel ooit een
   DM over de radio hoeft te ontvangen.
