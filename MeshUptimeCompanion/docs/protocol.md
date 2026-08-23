# MeshUptimeCompanion — protocol

**Ontwerpdocument.** Dit legt de twee contracten vast waarop de firmware
gebouwd wordt: hoe een alert herkend wordt, en welke commando's er bestaan.
De firmware zelf staat er (parallel, elders) nog aan te komen; wat hier staat
is de afspraak, niet een bevestigd resultaat. Zie [overzicht.md](overzicht.md)
voor de context en [gebruik.md](gebruik.md) voor wat een gebruiker er in de
praktijk van merkt.

## Contract 1 — de severity-marker

Een alert-DM van de MeshUptime-bot begint met één van drie emoji plus een
spatie, gevolgd door de vrije tekst van de melding:

| marker | severity | betekenis | UTF-8-bytes (incl. de spatie erna) |
|---|---|---|---|
| 🔴 | **High** | actieve storing, hoge prioriteit | `F0 9F 94 B4 20` |
| 🟠 | **Medium** | actieve storing, gewone prioriteit | `F0 9F 9F A0 20` |
| 🟢 | **Low / herstel** | lage prioriteit, of "weer in orde" | `F0 9F 9F A2 20` |

De drie emoji zijn respectievelijk **U+1F534** (rode cirkel), **U+1F7E0**
(oranje cirkel) en **U+1F7E2** (groene cirkel) — elk een standaard 4-byte
UTF-8-reeks, hierboven uitgeschreven. De companion match op die eerste vijf
bytes van het bericht en niet op een geparste emoji-library: dat is genoeg om
de drie te onderscheiden en het is de vorm waarin ze al over de radio komen.

**Wat dit contract wel en niet regelt.** Het regelt alleen *welk deuntje*
gekozen wordt (zie de `!tune`-toewijzing hieronder en de standaardtunes in
[gebruik.md](gebruik.md)). Het regelt niet wat er met de tekst ná de marker
gebeurt — de T1000-E heeft geen scherm, dus die tekst wordt hoogstens gelogd.
Een DM die met geen van de drie markers begint (een gewoon berichtje, geen
alert) valt onder de aparte `msg`-tune, zie de `!tune`-rij hieronder.

**Waar de marker vandaan komt.** Dit is een afspraak aan de kant van de
MeshUptime-bot (zie [../../docs/werking.md](../../docs/werking.md)) — de
companion stelt hem niet zelf op, hij herkent hem alleen. Verandert die
opmaak ooit aan de bot-kant, dan moet dit contract in beide repo's tegelijk
worden bijgewerkt.

## Contract 2 — de DM-/CLI-commandoset

**Twee vormen van dezelfde opdrachten.** Over een DM beginnen ze met `!`
(`!vol 2`); over de seriële console (USB, lokaal, dus impliciet vertrouwd)
zonder voorvoegsel (`vol 2`). Verder is het dezelfde parser en dezelfde
uitvoering.

**Allowlist.** Een `!`-commando over DM wordt alleen uitgevoerd als de
afzender-pubkey op de allowlist staat: de **eigenaar** en de
**MeshUptime-bot-pubkey**. Elke andere afzender krijgt geen antwoord en geen
uitvoering — geen foutmelding, om dezelfde reden als elders in dit ecosysteem
een geweigerd verzoek stilte is en geen leeg antwoord (zie
[../../docs/werking.md](../../docs/werking.md) → *Toegangsbeheer*): een
draagbaar toestel dat op elke DM van een onbekende afzender reageert, is een
toestel dat een vreemde op afstand kan laten zoemen. De bot staat op de lijst
zodat het contract in principe symmetrisch is (de bron van de alert-DM's kan
ook commando's sturen), ook al is het in de praktijk vooral de eigenaar — via
zijn eigen companion, of via **MeshManager**'s Send-DM-tab, zie
[gebruik.md](gebruik.md) — die dit gebruikt. Serieel over USB kent geen
allowlist: fysieke toegang tot het toestel is al het vertrouwen.

| commando | argumenten | uitleg |
|---|---|---|
| `!find` | — | speelt herhaaldelijk het vind-me-deuntje af, om het toestel terug te vinden als het zoekgeraakt is. Stopt door `!findstop` of door de knop in te drukken (zie [gebruik.md](gebruik.md)). |
| `!findstop` | — | stopt `!find` voortijdig. |
| `!mute` | `on` \| `off` | schakelt alle geluid (severity-tunes, vind-me, meldingstoon) stil. Of dit ook de valdetectie-/SOS-tunes dempt staat niet vast — zie de kanttekening in [gebruik.md](gebruik.md). |
| `!vol` | `0`..`3` | zet het volume van de buzzer in vier stappen. |
| `!tune` | `<H\|M\|L\|find\|msg>` `<RTTTL>` | overschrijft het deuntje voor een categorie: `H`/`M`/`L` = de drie severities, `find` = het vind-me-deuntje, `msg` = de tune voor een DM zonder severity-marker. De waarde is een RTTTL-string (zie *RTTTL, kort* hieronder). |
| `!quiet` | `<sH>-<eH>` \| `off` | stille uren als twee uurgetallen (0–23), bv. `!quiet 22-7`. Buiten dat venster gewoon geluid; `off` schakelt de stille uren uit. Of een SOS/valmelding de stille uren doorbreekt is een ontwerpvraag die nog niet beantwoord is. |
| `!gps` | `on` \| `off` \| `ondemand` | GPS-gedrag: doorlopend aan, uit, of alleen een fix nemen wanneer nodig (bij `!loc`, en vermoedelijk bij een lange-druk-SOS). Zie de onzekerheid over de GNSS-werkwijze in [overzicht.md](overzicht.md). |
| `!loc` | — | vraagt een locatiebepaling op; het antwoord komt terug als DM. Vorm en nauwkeurigheid van dat antwoord staan nog niet vast. |
| `!cfg` | — | toont de huidige instellingen (mute, volume, stille uren, gps-modus, allowlist) als DM. |
| `!allow` | `add <64hex>` \| `list` \| `del <prefix>` | beheert de allowlist. Toevoegen vraagt de **volledige** 64-hex-pubkey (zoals elders in dit ecosysteem — zie [../../docs/werking.md](../../docs/werking.md) → *Sleutels toevoegen en wissen* — omdat het gedeelde geheim uit de volle sleutel gerekend wordt); wissen mag op een prefix, zolang die niet dubbelzinnig is. |
| `!preset` | `<1\|2\|3>` `<tekst>` | legt een van drie kant-en-klare berichten vast die de knop kan versturen als ack/statusmelding. Welke druk welk preset kiest staat in [gebruik.md](gebruik.md), met de nog openstaande vraag erbij. |
| `!fall` | `on` \| `off` | schakelt valdetectie (QMA6100P) aan of uit. |
| `!ping` | — | levenstekentest: de companion antwoordt met een DM dat hij bereikbaar is. Of dat antwoord ook batterij/signaal meestuurt (zoals de bot dat doet bij zijn eigen `ping`, zie [../../docs/werking.md](../../docs/werking.md)) staat niet vast. |

### RTTTL, kort

RTTTL (Ring Tone Text Transfer Language) is de tekstindeling die klassieke
Nokia-ringtonebibliotheken en veel Arduino-buzzerbibliotheken gebruiken:

```
naam:d=standaardnootduur,o=standaardoctaaf,b=tempo:noot,noot,noot,...
```

Bijvoorbeeld, puur ter illustratie van de **vorm** en niet een van de
werkelijke standaardtunes van dit project:

```
Voorbeeld:d=4,o=5,b=120:c,d,e,f,g
```

De echte standaardtunes zijn Mario-thema-geïnspireerde motieven per severity
(zie [gebruik.md](gebruik.md)); de exacte RTTTL-strings staan in de
firmwarebron en zijn hier bewust niet overgenomen, omdat ze nog kunnen
wijzigen terwijl de firmware gebouwd wordt.

## Wat hier nog niet in zit

Dit protocol beschrijft de commando's en de marker, niet de framing eronder
(hoe een tekst-DM als `TXT_TYPE_PLAIN` versus `TXT_TYPE_CLI_DATA` verstuurd
wordt, of er een ACK op zit, en of lange antwoorden — zoals `!cfg` — over
meerdere pakketten geknipt moeten worden zoals elders in dit ecosysteem, zie
[../../docs/werking.md](../../docs/werking.md) → *De DM-opdrachten*). Dat is
een firmware-detail dat aan de bouw wordt overgelaten en hier niet wordt
voorgewend al vastgelegd te zijn.
