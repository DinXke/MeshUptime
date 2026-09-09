# De IRC-server op de node

Vanaf **v2.9.0** draagt de room-server-variant (`env:meshuptime_room`) een echte
IRC-server op poort 6667. Een gewone IRC-client — HexChat, irssi, WeeChat, mIRC,
Textual — verbindt ermee en praat daarna op het LoRa-mesh.

Dit bestand beschrijft de brug zoals hij is. Waarom de bot-slots de identiteit
dragen en niet de rooms staat hieronder bij *Identiteit*; wat er nog niet werkt
in [openstaand.md](openstaand.md).

## Wat er op wat afgebeeld wordt

| IRC | MeshCore |
|---|---|
| account (wachtwoord bij inloggen) | een vast **bot-slot** met eigen sleutelpaar |
| `#kanaal` zonder sleutel | hashtag-kanaal, geheim uit de naam afgeleid |
| `#kanaal` met sleutel (`JOIN #x <hex>`) | group-channel met expliciet 16/32-byte geheim |
| bericht in een kanaal | group-datagram, geflood |
| `/msg <nick> tekst` | DM, ECDH vanaf jouw bot-sleutelpaar |
| `/whois <nick>` | pubkey, SNR, hopcount, laatst gehoord |
| `/list` | de kanaaltabel van de node |
| `/join`, `/part`, `/names`, `/topic` | **puur lokaal**, kosten geen airtime |

## Identiteit

### Waarom de bot-slots

`RoomMesh` draagt drie soorten identiteit op één radio, elk met een eigen
sleutelpaar, via de `self_id`-wissel in `onRecvPacket()`:

- **rooms** — server-rol; anderen loggen *in* en posten. Verkeerde kant op voor
  een chatdeelnemer.
- **snodes** — telemetriecontacten. Dragen geen gesprek.
- **bots** — CHAT-identiteiten die DM's kunnen *initiëren* en kanalen meelezen.

Een IRC-gebruiker is een chatdeelnemer, dus een bot. De machinerie bestond al
(`botSendTo`, de kanaaltabel, de per-bot advertenties); `IrcTask` is de IRC-kant
ervoor en niet een tweede identiteitsstelsel.

### Vaste toewijzing

Elk account heeft **permanent** hetzelfde bot-slot, ook als niemand ingelogd is.
Dat is een keuze en geen implementatiegemak: je pubkey is je adres op het mesh.
Zou een slot bij logout vrijkomen en later aan iemand anders gaan, dan komen DM's
die onderweg waren bij de verkeerde persoon aan, en praten contacten die jou
toegevoegd hebben opeens met een ander.

Gevolg: **het aantal accounts is hard begrensd op `MAX_BOTS`** (8 in
`env:meshuptime_room`). Twee slots zijn doorgaans al vergeven aan de alert-bot en
de MGMT-bot, dus reken op zes IRC-gebruikers.

### Wat een kanaalnaam wél en niet bewijst

In een MeshCore group-channel is de afzendernaam **alleen tekst**. Een
group-pakket draagt geen handtekening per afzender; het formaat is
`"<naam>: <bericht>"` in de versleutelde payload. Wie de kanaalsleutel heeft, kan
elke naam voorzetten — en de sleutel van het publieke kanaal is algemeen bekend.
Een nick in een kanaal is dus een beleefdheid, geen bewijs.

**DM's zijn wél gebonden.** Die lopen over een ECDH-geheim tussen twee
sleutelparen, met een MAC. Zonder de private key van de afzender is een DM niet
te vervalsen.

## De IRC-tab in de webinterface

Alles hieronder kan ook met de muis, op de **irc**-tab van de nodepagina. Die tab
beheert *IRC-gebruikers*, en dat is bewust één ding: een bot-slot plus het account
dat er permanent aan vastzit. Ze worden samen gemaakt en samen gewist, want los
van elkaar zijn ze allebei onbruikbaar. De service-bots (alert, MGMT) blijven op de
**bot**-tab en horen bij geen account.

De tab toont per gebruiker de nick, de identiteit, de pubkey en of er nu iemand op
ingelogd is, plus knoppen voor wachtwoord, *wis* (account weg, identiteit blijft)
en *wis+identiteit* (het sleutelpaar gaat mee — onomkeerbaar).

### App-config importeren

Met de configexport van de MeshCore-app wordt een gebruiker **dezelfde identiteit
als op je telefoon**. Het bestand wordt **in je browser** gelezen en niet geüpload:
met 350 contacten is het ~126 kB, en de synchrone webserver van deze node houdt een
POST-body in RAM. De pagina stuurt alleen door wat de node kan opslaan.

| Uit het bestand | Gaat naar de node? |
|---|---|
| `name` | ja, als botnaam (niet-ASCII eruit, 23 tekens) |
| `private_key` + `public_key` | alleen als je het vinkje zet — lees de waarschuwing |
| `channels[].secret` | de kanalen die je aanvinkt |
| `contacts[]` | de eerste 64, als naamtabel |
| `radio_settings` | **nee** — dat zou deze node van het mesh af zetten |
| `position_settings` | nee, dat is jouw positie |

### Naamtabel

Een node-brede tabel **naam → pubkey**, gevuld uit elke import, hoogstens
`MAX_NAMES` (64) ingangen, vol → de oudste eruit. Hij bestaat zodat
`/msg Lutterade` ook werkt voor een node die deze node zelf nooit heeft horen
adverteren. Node-breed en niet per gebruiker: een pubkey is geen persoonlijk bezit,
en per gebruiker zou 350 contacten × 8 accounts betekenen.

`ircResolveNick()` kijkt op volgorde: een geplakte hex-pubkey, de companion-store,
de naamtabel, en dan pas de buurtlijst. De naamtabel staat boven de buurtlijst
omdat hij expliciet is aangeleverd, terwijl een advert-naam is wat een node over
zichzelf beweert — en dat kan morgen anders zijn.

## Ledenlijst en geschiedenis

Twee dingen die IRC verwacht en een mesh niet heeft, en hoe ze nagebootst worden.

**`/NAMES` toont wie er gehoord is.** Een MeshCore-kanaal heeft geen aanwezigheid:
geen join, geen ledenlijst, alleen wie toevallig zendt. Een leeg `/NAMES` leest
echter als "hier is niemand". Daarom onthoudt de node per kanaal wie er gezonden
heeft en presenteert die als leden: de eerste keer een `JOIN`, na 45 minuten stilte
een `PART`. Wat je ziet is dus *recent gehoord*:

- wie meeleest maar nooit zendt, verschijnt **nooit**;
- wie een naam verzint, verschijnt **wel** — kanaalnamen zijn onbewezen tekst;
- de `PART` is een gok op stilte, geen vertrek.

**Terugspoelen na het inloggen.** Een gedeelde ringbuffer van de laatste 32
berichten (kanaalregels en DM's door elkaar). Je krijgt terug wat er langskwam
*terwijl je weg was*: per account onthoudt de node de tijd van je laatste uitloggen,
en alles van daarna komt bij het inloggen (je DM's) of bij `JOIN` (dat kanaal)
alsnog binnen, met `[uu:mm]` ervoor. Bovengrens 12 uur — daarna is het archief en
daar is deze node de plek niet voor. De buffer staat puur in RAM: een herstart wist
hem, dit is een radio met een chatserver erop en geen logserver.

## Accounts aanmaken

Registratie kan **niet** over IRC: een account claimt een bot-slot, en dat is een
beheerdaad. Het gaat via de CLI — serieel op 115200, of via het CLI-venster van de
webinterface (`POST /cli`). Alleen `irc key set` kan **niet** over het web; zie
*Je eigen sleutel meebrengen*.

```
bot list                                  # welke bot-slots bestaan er
bot add IRC-bjorn                         # nieuw slot met vers sleutelpaar
irc user add bjorn hunter2xx IRC-bjorn    # account -> dat slot
irc list                                  # wat staat er
```

Overige commando's:

| Commando | Wat |
|---|---|
| `irc list` | sessies en accounts |
| `irc user add <nick> <wachtwoord> <bot>` | account maken (wachtwoord ≥ 6 tekens) |
| `irc user pass <nick> <wachtwoord>` | wachtwoord wijzigen |
| `irc user del <nick>` | account weg; een open sessie wordt weggestuurd |
| `irc key set <bot> <privhex> <pubhex>` | je **eigen** sleutelpaar in een slot leggen |

Accounts staan in `/irc_accounts` op SPIFFS, als
`nick⇥salt⇥sha256(salt, wachtwoord)⇥bot-slot`. Het wachtwoord zelf staat er niet.

## Inloggen met een client

Server: het IP van de node, poort **6667**, geen TLS. Nick = je accountnaam,
serverwachtwoord = je accountwachtwoord.

```
/server add meshuptime <node-ip>/6667 -auto
/set -clear password
/connect meshuptime
```

In HexChat: *Netwerk toevoegen* → server `<node-ip>/6667`, *Wachtwoord* invullen,
*Nick* op je accountnaam. Van nick wisselen na het inloggen wordt geweigerd — je
nick zit vast aan je mesh-identiteit.

Anderen voegen je toe met de `meshcore://contact/add`-link die in het bericht van
de dag staat.

## Je eigen sleutel meebrengen (BYOK)

`irc key set` legt het sleutelpaar van je telefoon in een bot-slot. Daarna ben je
op IRC dezelfde MeshCore-identiteit als in de app: je bestaande contacten
bereiken je, zonder iets toe te voegen.

**Lees eerst wat je weggeeft.** Vanaf dat moment:

- staat je private key in SPIFFS op de node, en kan **wie de node of de
  webinterface beheert je permanent nadoen** — ook als je nooit meer inlogt. Er
  is in MeshCore geen revocation en geen manier om zo'n bericht te herkennen.
- zit dezelfde sleutel op twee plaatsen tegelijk (telefoon en node), en dat is
  precies het patroon dat een gestolen sleutel onzichtbaar maakt.

Daarom kan het **alleen over de seriële console**. Niet over IRC, want dat is
onversleuteld: een private key die je in een chatvenster typt staat daarna in je
client-log, in je scrollback en op elke switch onderweg. En niet over de
webinterface, want die is HTTP zonder TLS met het wachtwoord in base64 ernaast —
`POST /cli` weigert `irc key set` om dezelfde reden waarom hij `prv.key` al
weigerde. Of gebruik gewoon de sleutel die de node zelf gemaakt heeft.

## Stappenplan: nieuwe IRC-client met je bestaande sleutelpaar

Uitgangspunt: je hebt al een MeshCore-identiteit (telefoon of companion) en je wil
op IRC diezelfde persoon zijn. Lees eerst de waarschuwing hierboven — dit is
onomkeerbaar in de zin dat je de sleutel niet kunt terugtrekken.

**1. Haal je sleutelpaar op.** In de MeshCore-app: instellingen → identiteit /
back-up. Op een companion met CLI: `get prv.key` en `get pub.key` over serieel.
Je hebt beide nodig: 64 hextekens privaat, 64 hextekens publiek.

**2. Maak een bot-slot** op de node, over serieel (115200, regels afsluiten met CR):

```
bot list
bot add IRC-<jouwnaam>
```

**3. Leg je eigen sleutel in dat slot.** Alleen serieel — de webinterface weigert
dit, en over IRC kan het niet:

```
irc key set IRC-<jouwnaam> <privhex64> <pubhex64>
```

De node antwoordt `OK bot <n> draagt nu jouw sleutel; advert de lucht in`.

**4. Zet je telefoon-identiteit stil.** Vanaf nu adverteren twee apparaten
dezelfde pubkey. Repeaters cachen routes per pubkey, dus twee zenders op één
identiteit laat het pad heen en weer klappen en dan komen DM's soms op het
verkeerde toestel aan. Kies er één: laat de telefoon niet meer adverteren, of
gebruik op de node een nieuwe sleutel in plaats van BYOK.

**5. Maak het IRC-account:**

```
irc user add <nick> <wachtwoord> IRC-<jouwnaam>
irc list
```

**6. Zoek het IP van de node** (webinterface, of je router) en verbind. In irssi:

```
/server add -auto -network mesh <node-ip> 6667 <wachtwoord>
/connect mesh
```

In HexChat: *Netwerklijst* → *Toevoegen* → server `<node-ip>/6667`, bij *Wachtwoord*
je accountwachtwoord, *Nick* je accountnaam, en **Autoconnect** aan als je wil.
Geen SSL aanvinken.

**7. Join een kanaal.** `/list` toont wat de node al kent; `/join #test` volgt een
bestaand of nieuw hashtag-kanaal, `/join #geheim <hex>` een privé-kanaal.

**8. Controleer.** `/whois <jouwnick>` moet jouw pubkey tonen — dezelfde als op je
telefoon. Zo niet, dan is stap 3 niet aangekomen.

## Airtime

Een matig druk IRC-kanaal produceert meer verkeer dan een LoRa-mesh kan dragen.
EU868 kent 1% duty cycle en een MeshCore-tekstbericht is maximaal 160 tekens.
Zonder rem is deze brug een zendmachine die het eigen netwerk platlegt. Daarom:

| Rem | Standaard | Vlag |
|---|---|---|
| minimaal tussen twee verzendingen per gebruiker | 3 s | `IRC_TX_MIN_MS` |
| gedeelde emmer over alle gebruikers | 6 berichten | `IRC_BUCKET_MAX` |
| hervultempo van die emmer | 1 per 10 s | `IRC_BUCKET_MS` |
| tekstlengte incl. `"<naam>: "` | 160 tekens | `BOT_MAX_TEXT_LEN` |

`JOIN`, `PART`, `QUIT`, `TOPIC` en `NAMES` gaan **nooit** het mesh op. `NOTICE`
evenmin — clients sturen daar automatische dingen mee. CTCP wordt genegeerd,
behalve `ACTION` (`/me`), dat als gewone tekst met een `*` ervoor meegaat.

Wat geweigerd wordt komt terug als `NOTICE` met de wachttijd erbij, niet als
stilte: een client die niet weet dat zijn regel weggegooid is, typt hem opnieuw.

## Beveiliging

- **Geen TLS.** Wachtwoord en gesprek gaan in leesbare vorm over de lijn. Draai
  dit op een vertrouwd LAN of achter een VPN; zet poort 6667 nooit open naar het
  internet. Een ESP32-S3 die naast mesh, WiFi en de webserver ook TLS-sessies
  moet dragen heeft de heap niet, en een halve TLS is erger dan geen.
- **Eén sessie per account.** Een tweede login gooit de eerste eruit; twee
  toetsenborden op één mesh-identiteit maakt onnavolgbaar wie wat verstuurde.
- **De kanaalsleutel wordt nooit teruggegeven.** `MODE #x` zegt of er een sleutel
  is (`+k`), niet welke — een MODE-antwoord belandt in elke client-log.
- **Wachtwoorden** staan als `sha256(salt ‖ wachtwoord)` met een eigen 8-byte
  salt per account.
- **Sessies verlopen.** Na 2 minuten stilte een `PING`, na 3 minuten weg.

## Wat het niet is

Geen IRC-netwerk: geen server-naar-server-koppeling, geen NickServ/ChanServ, geen
ban-lijsten, geen channel-operators, geen geschiedenis. `/names` toont de lokale
sessies die een kanaal volgen — niet wie er op het mesh meeleest, want dat is niet
te weten. Een mesh-afzender verschijnt zodra hij zendt.

## Bouwvlaggen

| Vlag | Standaard | Betekenis |
|---|---|---|
| `IRC_PORT` | 6667 | luisterpoort |
| `IRC_MAX_CLIENTS` | `MAX_BOTS` | gelijktijdige sessies |
| `IRC_NICK_MAX` | 20 | nicklengte |
| `IRC_TX_MIN_MS` | 3000 | zendrem per gebruiker |
| `IRC_BUCKET_MAX` / `IRC_BUCKET_MS` | 6 / 10000 | gedeelde emmer |
| `IRC_SEEN_PER_CHAN` / `IRC_SEEN_TTL_MS` | 10 / 45 min | geëmuleerde ledenlijst |
| `IRC_LOG_MAX` / `IRC_LOG_TTL_S` | 32 / 12 u | terugspoelbuffer (~190 byte per regel) |
| `MAX_NAMES` | 64 | gedeelde naamtabel (52 byte per ingang) |

Meer gebruikers = meer bot-slots. `MAX_BOTS` staat in `env:meshuptime_room` op 8.
Gemeten op een Heltec V3:

| Build | RAM |
|---|---|
| zonder IRC | 60,1% (197 024 B) |
| IRC, `MAX_BOTS=4` | 61,0% (199 944 B) |
| IRC, `MAX_BOTS=8` | 62,8% (205 840 B) |

De server zelf kost 2 920 byte, elk extra slot (bot + sessie) ~1 475 byte. Verder
opdraaien kan statisch, maar wat overblijft moet naast mesh, WiFi en de webserver
nog een HTTP-antwoord kunnen samenstellen — en dat is waar het eerder knelde. Meet
opnieuw voor je verder gaat.
