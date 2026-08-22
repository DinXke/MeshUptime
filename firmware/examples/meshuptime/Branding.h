#pragma once

/* ============================================================================
 * Productnaam + eigen versionering, LOS van de MeshCore-versie.
 *
 * FIRMWARE_VERSION (in SensorMesh.h / RoomMesh.h) is de MeshCore-versie
 * ("v1.17.0") en blijft dat: het protocol-versieveld in de login-respons is een
 * apart byte (FIRMWARE_VER_LEVEL) en adverts dragen geen versietekst, dus deze
 * branding is puur voor display/web/`ver` en raakt de compatibiliteit niet.
 *
 * MESHUPTIME_VERSION is onze eigen mijlpaal (semantisch), LOS van MeshCore:
 *   v2.0.0 = de multiroom room-server-basis.
 *   v2.1.0 = web-GUI room-beheer + QR + backup/restore, /mon/alarm + guest-
 *            semantiek, virtuele sensor-nodes, per-sleutel-ACL, kanaalbeheer-panel,
 *            handmatig advert, ping-fix en de room/DM-commandoset.
 *   v2.2.0 = async netwerk-taak-engine (port/http/scan/traceroute) + node-side
 *            SNMP-sensor + bot (CHAT/notifier) + GUI-declutter.
 *   v2.2.1 = boot-stack-overflow-fix (dikke MonitorCfg van de stapel) + de bot
 *            als TWEERICHTINGS mesh-diagnose-responder (ping/path/help).
 *   v2.2.2 = NTP-server + tijdzone instelbaar (web-GUI), menselijke tijden lokaal
 *            (CET/CEST), protocol/RTC blijven UTC; bot-`path` toont de
 *            tussenliggende repeaters met naam.
 *   v2.3.0 = hashtag-/publieke kanalen (de bot leest mee + antwoordt op
 *            ping/test/path), grote advert-/contactlijst (naamresolutie overal),
 *            kanaal-afzendernaam inline uit het bericht.
 *   v2.3.1 = kanaal toevoegen op NAAM zonder secret: sleutel afgeleid uit de naam
 *            (sha256[:16], hashtag-kanaal, zoals de MeshCore-app).
 *   v2.3.2 = naam "Public" (met/zonder #) zonder secret -> de VASTE publieke
 *            sleutel (het echte publieke kanaal), zoals de app.
 * Zie CHANGELOG.md in de firmware-repo.
 * ==========================================================================*/

#ifndef MESHUPTIME_VERSION
  #define MESHUPTIME_VERSION   "v2.3.2"
#endif
#ifndef MESHUPTIME_AUTHOR
  #define MESHUPTIME_AUTHOR    "DinX"
#endif

/* "MeshUptime v2.0.0 (by DinX)" */
#define MESHUPTIME_BRAND   "MeshUptime " MESHUPTIME_VERSION " (by " MESHUPTIME_AUTHOR ")"

/* De volledige regel, met de MeshCore-versie erbij. mc is een string-literal
 * (FIRMWARE_VERSION). Scheidingsteken bewust ASCII (" - "): deze tekst gaat ook
 * door JSON (/cfg.json) en over de mesh-CLI, en daar is een non-ASCII byte vragen
 * om moeite. */
#define MESHUPTIME_BRAND_FULL(mc)   MESHUPTIME_BRAND " - MeshCore " mc
