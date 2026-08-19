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
