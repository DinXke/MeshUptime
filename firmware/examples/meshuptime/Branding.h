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
 *   v2.3.3 = wifi/netvoeding-alerts: SIMULATIE-merkteken alleen bij een echte
 *            forcering (geen mislabel), + debounce/opstart-genade tegen de vloed
 *            van alerts bij korte blips en de eigen reboot.
 *   v2.3.4 = spook-recovery ("netvoeding terug na 0s") weg: symmetrisch
 *            gedebouncete storingstoestand voor vaste kanalen, OLED + alarm delen
 *            die toestand.
 *   v2.3.5 = spook-recovery ECHT weg (bug bleef in v2.3.4): harde grendel --
 *            een vaste-kanaal-"terug" komt alleen na een echt GEMELDE, lang-
 *            genoeg onderbreking (nooit "na 0s", ook bij sim/alert.debounce=0).
 *            Debounce vereenvoudigd tot korte settle (5 s std; reboot-vloed doet
 *            de opstart-genade), boot-grace-desync tussen OLED en alarm weg.
 *   v2.3.6 = reactietijd van de netvoeding/wifi-alert RUNTIME-INSTELBAAR (web-GUI
 *            "Reactietijd" + CLI/DM + persistent) i.p.v. hardgecodeerd: de vroeger
 *            vaste consts SAMPLE_INTERVAL_MS/SAMPLES_TO_SWITCH/SETTLE_MS +
 *            SENSOR_READ_INTERVAL_SECS stapelden op tot ~60 s. Nieuwe snelle
 *            defaults (power.sample 2 s, power.confirm 2, power.settle 8 s,
 *            read.interval 3 s, alert.debounce 2 s) -> alert in ~6-10 s. Live
 *            gelezen, in status.json ("timing") voor de MeshManager-server.
 *   v2.3.7 = ALLE alert-types onder ÉÉN gegrendelde kantelaar (main_room::
 *            edgeLatched). De ping-monitors EN de batterij (crit/laag) draaiden nog
 *            op de kale edge() en hadden exact de bugs die de vaste kanalen vóór
 *            v2.3.5 hadden: een pauze (wifi weg) las als "weer bereikbaar", een flap
 *            gaf een spook-"terug", de duur klopte niet (down_since 0 -> "na
 *            <uptime>", vandaar de 2u21) en een echte storing kon als SIMULATIE
 *            gelabeld worden. Nu voor iedereen dezelfde grendels: freeze bij niet-
 *            meetbaar/gemute, herstel alleen na een echt GEMELDE storing (announced)
 *            + plausibele duur, sim-merk alleen bij een echte forcering, en een
 *            gedeelde symmetrische debounce (debounceStep) -- de batterij kreeg
 *            Schmitt-hysterese (3,40/3,50 V crit, 3,60/3,70 V laag) tegen geflikker.
 *   v2.3.8 = ZEND-DIAGNOSE achter de ping/test/path-antwoorden (DM en kanaal):
 *            " | 2-byte [duim]" of " | 1-byte [sad]" (pad-hashgrootte) en
 *            " | scoped [duim]" of " | geen scope [sad]" (TRANSPORT_FLOOD met
 *            transport-codes vs. kale ROUTE_TYPE_FLOOD). Onafhankelijke oordelen,
 *            dus mengelingen komen voor. Zero-hop DIRECT draagt geen hash-grootte
 *            -> daar geen oordeel. Aan/uit via de GUI (Bot-pagina), standaard AAN.
 *   v2.3.9 = zend-diagnose fijnregelbaar in de GUI: PER COMMANDO aan/uit (masker
 *            ping/test/path) en een uitleg-URL bij "geen scope" in drie standen
 *            (uit / inline tussen haakjes / apart geflood kanaalbericht "Meer
 *            info over regions en scopes: <url>"). Inline wordt NOOIT half
 *            afgekapt: past de URL niet in de 160 tekens, dan valt hij helemaal
 *            weg -- de GUI toont daarom live per commando hoeveel tekens er nog
 *            in passen (handig bij een shortener).
 *   v2.3.10 = de bot beantwoordt nu de VOLLEDIGE monitoring/admin-commandoset via
 *            DM, maar enkel voor de alert-ontvangers (volledige pubkey in de
 *            recipientlijst). ping/path/help blijven open voor iedereen; de
 *            commando's draaien met volle admin-rechten. Async net-diagnostiek
 *            (dns/ping/...) komt via een aparte bot-DM terug. [botcmd]-debuglogging.
 *   v2.3.11 = per-monitor ERNST (hoog/midden/laag) + een ernst-emoji vooraan elke
 *            alert-DM, zodat een companion (T1000-E) er zijn buzzer-tune op kiest:
 *            🔴 hoog, 🟠 midden, 🟢 laag. Herstelmeldingen zijn altijd 🟢 groen.
 *            De ernst is per monitor en per vaste bron (netvoeding/wifi/batterij/
 *            test) in te stellen via de web-GUI, de CLI (mon.<ch>.sev / fa.<idx>.sev)
 *            en /mon/alarm (sev). ALLEEN de DM krijgt de emoji; de room-post niet.
 *   v2.3.12 = FIX: room-variant riep dm.loop() nooit aan (stond enkel in main.cpp,
 *            de sensor-variant). Daardoor werd de uitgestelde uitslag van async
 *            net-commando's (ping/dns/http/traceroute/scan <host>) in een room wel
 *            berekend (zichtbaar in de web-GUI) maar nooit teruggepost in de room.
 *            main_room.cpp roept nu the_mesh.dm.loop() aan na sensors.loop().
 *   v2.3.13 = bot-DM verfijningen: (a) `ping <ip/host>` in DM -> ICMP-pingtest i.p.v.
 *            mesh-Pong (kaal `ping` blijft Pong); (b) self-loopback-guard: negeer een
 *            DM waarvan de afzender een eigen node-identiteit is (bot/room/snode);
 *            (c) companion-verbs (find/play/fall/vol/tune/mute/quiet/loc/...) -> gerichte
 *            hint "stuur naar je companion" i.p.v. de node-commandolijst; (d) ernst-emoji
 *            (rood/oranje/groen) nu OOK vooraan de room-post-alerts, niet enkel de DM.
 *   v2.3.14 = bot-DM strípt nu een leidende '!'/'/' vóór het parsen -> "!play"/"!ping 1.1.1.1"
 *            worden correct herkend (verb "play"/"ping"), niet meer als "!play" genegeerd.
 *   v2.4.0 = COMPANION-HUB OP DE NODE: een persistente companion-lijst
 *            (/companions.cfg, cap 16: pubkey+naam+laatste lat/lon+last_seen).
 *            Web-GUI-tab "companions" (beheer add/edit/del, commandoknoppen die
 *            !find/!findstop/!loc/!mute/!vol/!tune/!quiet/!cfg/!ping/!play als
 *            bot-DM sturen + een eigen valdetectie-groep met de !fall-subcommando's
 *            on|off/mm on|off/sens/nomotion/prealarm/target add|del|list/test/
 *            status, en een Leaflet/OSM-kaartje met terugval naar lat,lon-tekst).
 *            De node ontvangt #LOC-locatierapporten: een DM van een BEKENDE
 *            companion is altijd een antwoord/rapport, nooit een commando ->
 *            begint hij met "#LOC <lat>,<lon>" dan wordt de locatie bewaard, en
 *            draagt hij een val-merkteken ((val)/(geen beweging)/(SOS)) dan ook een
 *            val-event (fall_ts/fall_kind); anders stil aanvaard (geen "onbekend
 *            commando"-bounce meer op companion-replies). /companions.json (auth)
 *            geeft lat/lon + fall_ts/fall_kind voor MeshManager-escalatie.
 *            Werkt ook als MeshManager plat ligt.
 * Zie CHANGELOG.md in de firmware-repo.
 * ==========================================================================*/

#ifndef MESHUPTIME_VERSION
  #define MESHUPTIME_VERSION   "v2.4.0"
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
