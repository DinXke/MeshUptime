# MeshUptime

Bewaking op een Heltec LoRa32 V3, in de geest van Uptime Kuma maar met LoRa als
uitweg. Draait op een MeshCore-node, biedt zijn uitslagen aan als
telemetriesensoren die een andere node kan opvragen, en waarschuwt over het mesh
als er iets omvalt.

*English documentation: [docs/en/](docs/en/README.md).*

## Waarom LoRa en niet het netwerk

Omdat het gemeten is en niet aangenomen. Op 19 augustus 2026 is deze node op
batterij gezet met de stekker eruit:

- eerst nog enkele minuten bereikbaar, met **31% verlies** op de HTTP-metingen
- daarna volledig weg — 21 opeenvolgende mislukte metingen — met
  `WiFi disconnected (reason 202)`, dus `AUTH_FAIL` en niet zwak bereik
- **de USB er weer in bracht hem niet terug**; pas een harde reset herstelde de
  verbinding

Juist tijdens een stroomstoring is de netwerkweg dus het minst betrouwbaar. Een
webhook vanaf een bestaande Uptime Kuma kan deze node op dat moment per definitie
niet bereiken. Daarom gaan de waarschuwingen over LoRa, zijn de eigen controles
de kern en niet een aanvulling, en zit er een wifi-waakhond in.

## Wat werkt, en hoe zeker

| | |
|---|---|
| Basis | `examples/simple_sensor` van MeshCore tag `companion-v1.17.0` |
| Rol | `ADV_TYPE_SENSOR` |
| RAM | 22,9% (75.184 van 327.680) |
| Flash | 39,5% (1.321.521 van 3.342.336) |
| Heap in bedrijf | ~221 kB vrij, grootste blok ~212 kB |
| Beheerpagina | 66.505 byte in PROGMEM |

**Op het toestel gezien:** de voedingssensoren met hun gekalibreerde drempels, de
wifi-sensor met de waakhond erachter, ping-monitors die een herstart overleven,
`/hook` voor een bestaande Uptime Kuma, telemetrie die door een tweede node
opgevraagd en gelezen wordt, de webinterface met bewaking/toegang/nodebeheer, en
de CLI over serieel.

**Gebouwd maar nooit afgegaan:** de waarschuwingen over het mesh. Dat en al het
andere dat nog niet klopt staat in **[docs/openstaand.md](docs/openstaand.md)** —
begin daar als je dit project oppakt.

## De harde cijfers

De voedingsdrempels zijn afgeleid en niet gemeten: dit bord heeft geen VBUS-pin,
geen USB-detectie en geen laadstatus, alleen een ADC-meting van de
batterijspanning.

| toestand | spanning |
|---|---|
| batterij, onder belasting | 4,034 V |
| USB, lader klaar | 4,139 V |
| USB, actief ladend | 4,209–4,226 V |

Vandaar: onder **4,09 V** batterij, boven **4,12 V** netspanning, met 60 s
insteltijd na een overgang en drie opeenvolgende metingen aan dezelfde kant.
Beide drempels zijn instellingen en worden bewaard. Een naïeve drempel op 4,2 V —
wat een datasheet zou suggereren — zou "geen netspanning" hebben gemeld met de
stekker erin.

Twee andere getallen die overal doorwerken: een telemetriepakket heeft **180 byte**
te vergeven (24 vast, 9 per monitor, acht monitors), en een meshbericht wordt op
**158 byte** geknipt en niet op 160 — boven 158 kan een bericht na de vierde
poging niet meer opnieuw verstuurd worden.

**Kanaalnummers zijn onveranderlijk.** CayenneLPP draagt alleen nummers, geen
namen; schuift een nummer op, dan wijst een bewaarde koppeling stil naar de
verkeerde dienst.

| kanaal | inhoud |
|---|---|
| 1 | batterijspanning |
| 2 | netvoeding (0/1) |
| 3 | batterijvoeding (0/1) |
| 4 | wifi online (0/1) |
| 5 t/m 12 | monitors, zelf gepingd of van buiten gemeld |

## Bouwen en flashen

De WiFi-gegevens staan **niet** in deze repo. Kopieer
[firmware/wifi.ini.voorbeeld](firmware/wifi.ini.voorbeeld) naar de
`platformio.local.ini` van je MeshCore-bouwkopie (dat bestand staat in MeshCore's
eigen `.gitignore`) en vul je eigen netwerk in.

Er is één patch op MeshCore nodig, in
[firmware/patches/](firmware/patches/): de global `sensors` is in de Heltec
V3-variant hardgecodeerd, en zonder die drie regels kan een toepassing geen eigen
`SensorManager` opgeven zonder de variant te forken.

    python -m platformio run -e meshuptime
    python -m platformio run -e meshuptime -t upload --upload-port COM4

`upload` schrijft alleen de programmapartitie en niet SPIFFS: de identiteit in
`/identity/_main.id` en het instellingenbestand blijven staan.

## Documentatie

- **[docs/openstaand.md](docs/openstaand.md)** — wat niet werkt, en welke
  verdenkingen al onderzocht en weerlegd zijn. Het belangrijkste bestand.
- [docs/werking.md](docs/werking.md) — hoe het werkt: rollen, kanaalindeling,
  webinterface, CLI, DM-opdrachten, toegangsbeheer, waarschuwingen
- [docs/metingen.md](docs/metingen.md) — de metingen met hun getallen en datum
- [docs/beslissingen.md](docs/beslissingen.md) — de keuzes met de reden, ook de
  keuzes die later zijn omgedraaid
- [firmware/patches/LEESMIJ.md](firmware/patches/LEESMIJ.md) — de patches, en de
  patch die er met opzet niet is
- [docs/meting-voeding-2026-08-19.log](docs/meting-voeding-2026-08-19.log) — de
  ruwe meetreeks
