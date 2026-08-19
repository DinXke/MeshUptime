# MeshUptime

Basale uptimebewaking op een MeshCore-companion (Heltec V3), met de resultaten
als virtuele telemetriesensoren die andere nodes over het mesh kunnen opvragen.

**Status: ontwerpfase.** Er is nog geen firmware. Dit document legt vast wat het
moet worden en — belangrijker — waar de grens ligt van wat op dit apparaat kan.

## Wat het moet doen

- **Eigen toestand bewaken**: draait de node op USB-spanning of op batterij, is
  de wifi weg.
- **Externe zaken bewaken**: een adres met een naam en een instelbaar interval,
  en periodiek controleren of het antwoordt.
- **Resultaten aanbieden als telemetrie**, opvraagbaar door een andere
  companion over het mesh.
- **Beheer via een webinterface** met inloggegevens, inclusief welke nodes
  telemetrie mogen opvragen.
- **Waarschuwen over het mesh**: naar een groep companions per sensor, en naar
  een kanaal (eventueel met sleutel).

## De bindende beperking: heap, niet flash

Gemeten op het doelapparaat (`/config.json`, 19 augustus 2026):

| | |
|---|---|
| Vrij geheugen | 24.844 bytes |
| Grootste vrije blok | **11.764 bytes** |
| Flash gebruikt | 51% van 3264 kB |

Flash is dus ruim; werkgeheugen niet. `platformio.local.ini` in de
MeshCore-werkkopie documenteert dat lwip bij ongeveer 22 kB vrije heap zijn
buffers niet rond kreeg en de synchrone webserver **halve antwoorden** schreef —
een storing die deze opstelling al twee keer een node heeft gekost. We zitten nu
al onder die grens.

Daaruit volgen ontwerpregels die niet onderhandelbaar zijn:

1. **Statisch alloceren.** Een vast maximum aantal monitors, vaste buffers. Geen
   `String`-concatenatie in het meetpad, geen groeiende containers.
2. **Geen tweede webserver.** De companion serveert met de *synchrone* Arduino
   `WebServer`. `ESPAsyncWebServer` erbij zetten kost een eigen taak en stack.
3. **Elke toevoeging wordt gemeten**, vóór en ná, met het grootste vrije blok
   als maat — niet met "vrij geheugen", want fragmentatie is hier het probleem.
4. **De radio gaat voor.** Een bewakingsronde mag de mesh-timing niet ophouden.
   Bewaken is de bijzaak; doorgeven van verkeer is de hoofdzaak.

## Wat er nog beslist moet worden

Zie `docs/ontwerp.md`.
