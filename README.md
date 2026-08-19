# MeshUptime

Bewaking op een Heltec V3, in de geest van UptimeKuma maar met LoRa als
uitweg. Draait op een MeshCore-node, meldt storingen over het mesh, en biedt de
resultaten aan als telemetriesensoren die een andere node kan opvragen.

## Waarom LoRa en niet het netwerk

Omdat dat gemeten is, niet aangenomen. Op 19 augustus 2026 is deze node op
batterij gezet met de stekker eruit:

- eerst nog een paar minuten bereikbaar, met **31% verlies** op de HTTP-metingen
- daarna volledig weg, met `WiFi disconnected (reason 202)` — `AUTH_FAIL` — bij
  een rssi van **-48**, dus niet door zwak bereik
- **de USB er weer in bracht hem niet terug**; pas een harde reset herstelde de
  verbinding

Juist tijdens een stroomstoring is de netwerkweg dus het minst betrouwbaar. Een
webhook vanaf een bestaande Uptime Kuma kan deze node op dat moment per definitie
niet bereiken. Daarom gaan de waarschuwingen over LoRa, en zijn de eigen
controles geen aanvulling maar de kern. De meetreeks staat in
[docs/meting-voeding-2026-08-19.log](docs/meting-voeding-2026-08-19.log).

Diezelfde meting leverde de wifi-waakhond op: na een aanhoudende storing gaat de
radio volledig omlaag en opnieuw op, en lukt dat niet, dan komt er een eigen
toegangspunt zodat het toestel instelbaar blijft. Zonder dat blijft een node na
een storing onbereikbaar terwijl alles weer werkt.

## Wat er werkt

| | |
|---|---|
| Basis | `examples/simple_sensor` van MeshCore `companion-v1.17.0` |
| Rol | `ADV_TYPE_SENSOR` |
| RAM | 18,2% (59.664 van 327.680) |
| Flash | 36,8% |
| Heap vrij | 235.156, grootste blok 225.268 |

- **Voedingssensoren** — netvoeding en batterijvoeding als aparte 0/1-waarden,
  met drempels die op deze hardware gekalibreerd zijn (zie onder)
- **Wifi-sensor** — en de waakhond erachter
- **Webinterface** met inloggegevens, `/status.json`, en een formulier voor de
  netwerkinstellingen
- **`/hook`** voor een bestaande Uptime Kuma, met keuring van de invoer
- **Instellingen** over de seriële console en over DM-CLI: `sensor set mains.hi 4.13`

In de maak: ping-monitors op kanaal 5 en hoger, `/hook` doorkoppelen naar
telemetrie en meshwaarschuwingen, de DM-opdrachten `list` / `get` / `status`, en
als laatste de repeaterrol met een tweede identiteit.

## De voedingsdrempels, en waarom ze zo zijn

Dit bord heeft **geen** VBUS- of USB-detectie. Er is geen PMU zoals op een
T-Beam, waar `PMU->isVbusIn()` bestaat; er is alleen een ADC-meting van de
batterijspanning. Netspanning wordt dus **afgeleid**, en de getallen komen van de
echte hardware:

| toestand | spanning |
|---|---|
| batterij, onder belasting | 4,034 V |
| USB, lader klaar | 4,139 V |
| USB, actief ladend | 4,209–4,226 V |

De drempel werkt omdat 4,034 V de batterij **onder belasting** is: de node trekt
stroom en de klemspanning zakt, terwijl de lader op USB die belasting aanvult.
Dat verschil wordt gróter als de batterij ouder wordt.

Vandaar: onder **4,09 V** batterij, boven **4,12 V** netspanning, met een
insteltijd van een minuut na een overgang (er zit een transiënt van ~50 mV) en
drie opeenvolgende metingen aan dezelfde kant. Beide drempels zijn
**instellingen**, geen constanten, zodat ze uit echte historiek bij te stellen
zijn zonder te flashen.

Een naïeve drempel op 4,2 V — wat een datasheet zou suggereren — zou "geen
netspanning" hebben gemeld met de stekker erin.

## Kanaalnummers zijn onveranderlijk

Een monitor krijgt bij aanmaken een kanaalnummer en houdt dat voor altijd.
Nummers van verwijderde monitors worden niet hergebruikt.

CayenneLPP draagt namelijk alleen kanaalnummers, geen namen. Een node die
telemetrie opvraagt bewaart dus zelf de koppeling "kanaal 6 = google", en als
nummers opschuiven wijst die koppeling **stil** naar de verkeerde dienst: geen
foutmelding, alleen verkeerde cijfers op een dashboard. Onthouden, niet
herberekenen.

| kanaal | inhoud |
|---|---|
| 1 | batterijspanning |
| 2 | netvoeding (0/1) |
| 3 | batterijvoeding (0/1) |
| 4 | wifi online (0/1) |
| 5 en hoger | ping-monitors |

Namen gaan daarom over het mesh via de DM-opdracht `list`, niet via telemetrie.

## Berichten zijn 158 byte, niet 160

`MAX_TEXT_LEN` is 160, maar `BaseChatMesh.cpp:423` weigert een herhaalde
verzending van een bericht langer dan `MAX_TEXT_LEN-2` zodra `attempt > 3`. Op
160 knippen levert dus precies de stukken op die later niet meer opnieuw
verstuurd kunnen worden. Twee byte per stuk is de prijs voor stukken die altijd
herhaalbaar blijven.

Antwoorden gaan als `TXT_TYPE_PLAIN` en niet als `TXT_TYPE_CLI_DATA`, omdat voor
CLI-antwoorden per ontwerp geen ACK verwacht wordt — dan weet je niet of je
antwoord is aangekomen.

## Bouwen

De WiFi-gegevens staan **niet** in deze repo. Kopieer
[firmware/wifi.ini.voorbeeld](firmware/wifi.ini.voorbeeld) naar de
`platformio.local.ini` van je MeshCore-bouwkopie (dat bestand staat in
MeshCore's eigen `.gitignore`) en vul je eigen netwerk in.

    python -m platformio run -e meshuptime
    python -m platformio run -e meshuptime -t upload --upload-port COM4

`upload` schrijft alleen de programmapartitie, niet SPIFFS: de identiteit in
`/identity/_main.id` blijft dus staan.

## Kernpatch

MeshUptime heeft één patch op MeshCore nodig, en straks een tweede. Ze staan in
[firmware/patches/](firmware/patches/) met de reden erbij, want elke patch moet
bij een nieuwe upstream-versie opnieuw aangebracht en nagekeken worden.

## Bekende gebreken

Opgeschreven in plaats van weggelaten; het volledige verhaal staat in
[docs/ontwerp.md](docs/ontwerp.md).

- `EnvironmentSensorManager::querySensors()` zet `next_available_channel` terug
  op 2 en kan gevonden I2C-sensoren bovenop onze vaste kanalen 2/3/4 zetten. Deze
  node heeft er geen; `begin()` waarschuwt zodra er één opduikt.
- Het grootste vrije blok zakte van 225.268 naar 163.828 na het bedienen van
  webverzoeken. Ruim genoeg, maar dit is de maat die dit project vanaf het begin
  bepaald heeft.
- Basic-auth over gewone HTTP stuurt het wachtwoord leesbaar over het netwerk.
  Verdedigbaar op een eigen LAN, niet genoeg voor een node die van buiten
  bereikbaar wordt.
- Deze node was de mesh-gateway van Home Assistant (poort 5000). Die rol is
  opgegeven; de oude firmware en sleutel staan buiten de repo in een back-up.

## Documentatie

- [docs/ontwerp.md](docs/ontwerp.md) — het volledige ontwerpverhaal, met de
  metingen en de bijgestelde aannames
- [docs/meting-voeding-2026-08-19.log](docs/meting-voeding-2026-08-19.log) — de
  ruwe meetreeks
