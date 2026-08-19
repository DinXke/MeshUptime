# Ontwerp

## Uitgangspunt (19 augustus 2026)

De companion krijgt een **volledig nieuw doel**. Alles wat er nu op staat en
niets met bewaking te maken heeft, mag weg:

| Weg | Waarom het heap kost |
|---|---|
| Chatclient (`page.html`, 5708 B gzip) | paginabuffer bij elk verzoek |
| Berichtenring (32 plaatsen) | vast RAM plus de cache-logica |
| MQTT-publisher (`PubSubClient`) | eigen buffer én een open TCP-socket |
| Doorsturen van ruwe pakketten | wachtrij van 4 plaatsen |
| Contacten- en kanalen-UI | opbouw van JSON in geheugen |

**Wat blijft**, want dat is nu juist het doel:

- de MeshCore-radiostapel en de identiteit (`/identity/_main.id`)
- contacten, om een waarschuwing te kunnen adresseren
- kanalen, om een bericht naar een kanaal te kunnen sturen
- de synchrone webserver, als enige beheerweg

## Waarom dit de sleutel is

Voor het opruimen: **24.844 byte vrij, grootste blok 11.764 byte**. Dat is onder
de grens waarbij lwip in dit project eerder al halve antwoorden veroorzaakte.
Met de bovenstaande lijst weg komt daar ruimte bij, en die ruimte is het budget
voor alles hieronder. Het getal ná het opruimen is dus de eerste meting die
gedaan moet worden, vóór er functionaliteit bij komt.

## Onderdelen, in volgorde van kosten

1. **Eigen toestand** — USB-spanning versus batterij, en wifi weg. Vrijwel
   gratis: die gegevens komen al langs (`bat` staat in `stats.json`), er is
   alleen een drempel en een toestandsovergang nodig.
2. **Externe controles** — naam, adres, interval. Statisch begrensd aantal.
   Open vraag: ICMP of een TCP-verbinding op een poort. TCP is eerlijker over
   "de dienst leeft" en gebruikt de socketlaag die er al is; ICMP zegt "de host
   leeft" en vraagt een raw PCB. Beide kosten een socket — meten welke minder.
3. **Waarschuwen over het mesh** — naar een groep contacten per sensor, en naar
   een kanaal. Hergebruikt wat de companion al kan (`/send`).
4. **Webbeheer met inloggegevens** — één pagina, geen framework, geen CDN.
5. **Virtuele telemetriesensoren** — resultaten opvraagbaar door een andere
   node. MeshCore heeft daarvoor een telemetrieverzoek met CayenneLPP; de
   decoder daarvan zit al in de repeaterfirmware van dit project. Let op: voor
   een gast staat `perm_mask` op nul, dus wie mag opvragen is een echte
   ontwerpvraag en geen vinkje.

## Regels die niet onderhandelbaar zijn

1. Statisch alloceren; vast maximum aantal monitors, vaste buffers.
2. Geen tweede webserver.
3. Elke toevoeging meten, vóór en ná, op het **grootste vrije blok**.
4. De radio gaat voor: een bewakingsronde mag de mesh-timing niet ophouden.
5. De identiteit blijft. Back-up staat buiten de repo; de herstelweg is
   gedocumenteerd naast de dump.

## Nog te beslissen

- ICMP of TCP voor de externe controle (meting bepaalt dit).
- Wie telemetrie mag opvragen: alleen bekende nodes, of ook gasten.
- Hoe een waarschuwing zich gedraagt bij herhaling — één bericht per
  toestandsovergang, of herhalen zolang de storing duurt. Herhalen kost
  zendtijd op een gedeelde band.

---

# Gemeten randvoorwaarden (19 augustus 2026)

Alles hieronder komt uit de schone upstream-bron op tag `companion-v1.17.0`,
niet uit herinnering en niet uit onze eerdere maatwerkbuild.

## Hoe lang mag een bericht zijn

`src/helpers/BaseChatMesh.h:8`

    #define MAX_TEXT_LEN (10*CIPHER_BLOCK_SIZE)   // must be LESS than
                                                  // (MAX_PACKET_PAYLOAD - 4 - CIPHER_MAC_SIZE - 1)

Met `CIPHER_BLOCK_SIZE 16` is dat **160 byte**. De grens waar het commentaar naar
wijst is 184 - 4 - 2 - 1 = 177, dus 160 is een gekozen ronde marge en geen hard
protocolplafond.

Belangrijker staat in `composeMsgPacket` (`BaseChatMesh.cpp:422`):

    if (text_len > MAX_TEXT_LEN) return NULL;
    if (attempt > 3 && text_len > MAX_TEXT_LEN-2) return NULL;

**Een bericht langer dan 158 byte kan na de vierde poging niet meer opnieuw
verstuurd worden.** Dat knoopt twee eisen aan elkaar: lange antwoorden opknippen
en opnieuw proberen bij niet-bezorgen zijn geen losse functies. Wie op 160 knipt,
maakt precies de stukken die later niet herhaald kunnen worden.

**Daarom is de knipgrens 158 en niet 160.** Dat kost twee byte per stuk en levert
op dat elk stuk onbeperkt herhaalbaar blijft.

Let ook op `BaseChatMesh.cpp:496`: bij verzenden met voorvoegsel wordt de tekst
*stil* afgekapt op `MAX_TEXT_LEN - prefix_len`. Een stuknummer als `[2/5] ` gaat
dus van het budget af: zes byte voorvoegsel laat 152 byte tekst over.

## Bezorging: wat het protocol al weet

Bezorgcontrole hoeft niet verzonnen te worden.

- `composeMsgPacket` berekent `expected_ack`: vier byte SHA-256 over tijdstempel,
  pogingnummer en tekst, met de eigen publieke sleutel erbij
  (`BaseChatMesh.cpp:430`). De ontvanger rekent hetzelfde uit
  (`BaseChatMesh.cpp:242`) en stuurt het terug. Een ACK bewijst dus dat **deze**
  tekst aankwam, niet enkel dat er iets aankwam.
- Verzenden geeft `est_timeout` terug: hoe lang wachten redelijk is, gegeven het
  aantal hops. Niet zelf een getal kiezen, dit gebruiken.
- De companion heeft er al een melding voor: `PUSH_CODE_SEND_CONFIRMED 0x82`
  (`MyMesh.cpp:114`).
- `attempt` is twee bits (`temp[4] = (attempt & 3)`): vier pogingen, nul tot drie.
  De gevraagde "na 3 retries" valt exact samen met de eigen teller van het
  protocol, wat goed uitkomt, want daarboven is het protocol zelf op.

Een valkuil die de DM-opdrachten hieronder rechtstreeks raakt:
`BaseChatMesh.cpp:258` zegt over antwoorden van type `TXT_TYPE_CLI_DATA`
uitdrukkelijk dat er geen ACK verwacht wordt. Antwoorden we op een opdracht als
CLI-data, dan **weten we niet of het antwoord is aangekomen**. Antwoorden als
gewone tekst levert wel een ACK op. Bezorgcontrole op antwoorden vraagt dus om
`TXT_TYPE_PLAIN`.

## Opnieuw proberen na vier mislukte pogingen

Wat het protocol niet doet, is het later nog eens proberen.

- Per uitgaand bericht een plaats in een kleine statische tabel: bestemming,
  tekst, `expected_ack`, pogingnummer, moment van de volgende poging.
- Vier pogingen op protocolsnelheid, met `est_timeout` als wachttijd.
- Daarna oplopend wachten (1, 5, 15, 60 minuten) met een bovengrens. Dit is een
  gedeelde band; een node die eindeloos een onbezorgbare melding herhaalt is
  zelf een storing geworden.
- Een melding die na een lange pauze alsnog aankomt, hoort te zeggen hoe oud de
  waarneming is. "IP onbereikbaar" zonder tijdstempel, een uur later bezorgd, is
  misleidend.

## Kan de heap groter?

Ja. Dit is de belangrijkste vondst van vandaag.

Upstream houdt de companion in **drie aparte envs**:
`Heltec_v3_companion_radio_usb`, `..._ble` en `..._wifi`. Onze eerdere
maatwerkbuild zette WiFi en BLE *samen* aan. Dat verklaart de 24 kB grotendeels:
de BLE-stapel neemt bij initialisatie tientallen kB heap, naast de WiFi-stapel.
MeshUptime wordt beheerd over web, mesh en API en heeft BLE niet nodig.

Daarnaast drie statische posten die bij een chatclient horen en niet bij een
bewaker: `MAX_CONTACTS=350`, `MAX_GROUP_CHANNELS=40`, `OFFLINE_QUEUE_SIZE=256`.
Verlaagd naar 32, 4 en 16.

Wat **niet** kan: meer RAM erbij. Het bord is een ESP32-S3 zonder PSRAM
(`board = esp32-s3-devkitc-1`; de Heltec V3 draagt de S3FN8). Er valt niets bij
te schakelen, er valt alleen minder te gebruiken.

## Bewaking van buiten aanleveren (API)

Zelf pingen is niet de enige weg en waarschijnlijk niet de beste. Een bestaande
Uptime Kuma is beter in bewaken dan dit apparaat ooit wordt: die kent
herhaalpogingen, onderhoudsvensters en historiek. Wat hij niet kan, is een
melding bezorgen als het internet weg is.

Daarom een tweede, goedkopere weg: een **inkomende webhook**. Uptime Kuma stuurt
zijn melding hierheen, de node zet die om in een meshbericht en in een
telemetriewaarde. Dat kost een route en geen enkele socket voor uitgaand pingen,
en het is de juiste taakverdeling: Kuma weet *of* iets stuk is, deze node is de
enige die het nog kan vertellen.

Beide wegen blijven. De eigen controles zijn onmisbaar voor het geval dat Kuma
zelf onbereikbaar is, en dat is nu juist het geval dat je wil weten.

## Sensoren opvragen via een DM

Naast web en telemetrie ook opdrachten per DM, met 158 byte als harde vorm:

    list            alle sensoren, naam en toestand, in stukken
    get <naam>      een sensor met detail
    status          eigen toestand: voeding, wifi, uptime

- Antwoorden als `TXT_TYPE_PLAIN`, zodat bezorging bevestigd wordt.
- Opknippen op woordgrens waar dat kan, met `[n/m] ` ervoor: 152 byte tekst.
- Een `list` met veel sensoren wordt veel berichten. Er hoort een bovengrens op
  het aantal stukken, met een slotregel over wat er niet in paste. Twintig
  berichten uitzenden omdat iemand `list` typte, is zendtijd van iedereen.
- Wie mag dit? Alleen bekende contacten; dezelfde vraag als bij telemetrie, en
  nog te beslissen.
