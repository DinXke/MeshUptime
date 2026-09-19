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
 *   v2.5.0 = MEERDERE BOT-IDENTITEITEN op de node (MAX_BOTS=4). De enkele notifier-
 *            bot is veralgemeend naar N onafhankelijke bots, elk met een eigen
 *            persistent sleutelpaar, naam, aan/uit-vlag, ontvangerslijst en zend-
 *            diagnose. Bot #0 blijft de BESTAANDE alert-bot ("BE-HSS-DinX-Bot",
 *            /bot_id + /bot_recips ONGEWIJZIGD) en draagt de alert-rol (dispatchAlert);
 *            bot #1 is een NIEUWE "BE-HSS-DinX-MGMT"-bot (nieuw sleutelpaar) voor
 *            companion-MANAGEMENT-verkeer. onRecvPacket matcht alle actieve bots en
 *            handleBotDm antwoordt/ontsleutelt als de bot die de DM ontving (per-bot
 *            recips/diag; companion-#LOC + self-loopback-guard per bot). Additieve
 *            persistentie: bot #i>0 in /bot_id_i + /bot_recips_i, per-slot config
 *            (bezet/rol/actief/naam) in /bots.cfg. /bot/* endpoints nemen een
 *            optionele bot=<idx-of-naam> (default: de alert-bot); de companion-GUI
 *            stuurt via de MGMT-bot. Nieuw /bots.json geeft alle bots (naam+pubkey+
 *            rol) zodat MeshManager de MGMT-pubkey oppikt. Web-GUI "Bots"-beheer:
 *            lijst + selecteren, toevoegen (genereert sleutel), hernoemen, aan/uit,
 *            wissen (niet de laatste/alert-bot) en de alert-rol zetten.
 *   v2.5.1 = INSTANT companion-push + volledige companion-command-GUI + radio-GUI.
 *            (1) Een ontvangen companion-#LOC/val gaat nu METEEN naar MeshManager
 *            (POST {push.url}/api/companion via dezelfde PushTask: host/token/DNS/
 *            socket hergebruikt, eigen retry-ring) i.p.v. te wachten op de poll van
 *            /companions.json (die blijft als terugval). Body:
 *            {"companions":[{"pubkey","lat","lon","seen","fall_ts","fall_kind"}]}.
 *            (2) Het companion-commandopaneel dekt nu de VOLLEDIGE set (parity met
 *            CLI/menu): status/tunes/rxps/gps/preset(1-3)/vol per-slot/allow
 *            add|del|list, naast de bestaande find/findstop/loc/ping/cfg/mute/vol/
 *            play/tune/quiet en de volledige !fall-groep. Alles via de MGMT-bot.
 *            (3) Radio-instellingen in de GUI (freq/bw/sf/cr/tx-power) ACHTER een
 *            expliciete confirm + rode waarschuwing ("kan de companion van de mesh
 *            doen vallen; fysieke seriële recovery nodig"), plus een 'radio show'
 *            leesknop. Stuurt '!radio <veld> <waarde> confirm'.
 *   v2.6.0 = MESHMANAGER-POLLER: Home Assistant valt uit de keten. De node haalt
 *            zelf de opdrachtwachtrij op (GET {push.url}/api/v1/commands, elke
 *            poll_secs, Bearer=sensorpush-token) en voert de instellingen-
 *            opvragingen uit langs RepeaterCli -- dat sinds nu een JOB van N
 *            commando's in EEN sessie draait (eenmaal inloggen, dan de N commando's
 *            achter elkaar), met de param->commando-vertaling van de oude HA-pusher
 *            ("cmd:X"->X letterlijk, anders P->"get P"). Antwoorden gaan terug via
 *            POST /api/v1/repeater_settings; geen antwoord -> null ("gevraagd, geen
 *            antwoord"). Doel-wachtwoorden in een persistent tabelletje
 *            (/rep_targets.cfg, cap 8, + standaardwachtwoord), beheerd via de web-GUI
 *            en /repeater_targets.json (wachtwoorden nooit teruggelezen). Gevaarlijke
 *            commando's (clkreboot/reboot/erase/set radio/...) komen NIET uit de
 *            wachtrij; muterende worden niet herhaald. refresh-(status)verzoeken
 *            worden gelogd als niet-ondersteund en vallen weg (bekende beperking).
 *            Nieuw: PushTask KIND_POLL (niet-blokkerende GET langs dezelfde socket-
 *            machine), /poller.json + /poller + /repeater_targets.json + /repeater/
 *            target, en een poller-tegel op de statuspagina. Alles niet-blokkerend;
 *            de bewaking gaat voor en één sessie tegelijk.
 *   v2.7.0 = STATUSVERZOEKEN uit de MeshManager-wachtrij. De poller voerde alleen
 *            instellingenopvragingen uit en liet `refresh` vallen; nu doet hij ze
 *            allebei. Per prefix dezelfde sessie-aanpak (login via RepeaterCli),
 *            maar na de login EEN REQ_TYPE_GET_STATUS i.p.v. CLI-tekst; het
 *            antwoord (RepeaterStats, 4+56 byte) wordt ontleed, op plausibiliteit
 *            getoetst en als METINGEN naar POST /api/v1/ingest gestuurd (nieuw
 *            PushTask KIND_INGEST). Statusverzoeken zijn LEESacties, dus de gewone
 *            drie pogingen. Mislukt de ronde of is het antwoord niet plausibel, dan
 *            wordt er NIETS gemeld (geen halve of verzonnen meting). De poll-URL
 *            meldt nu ?caps=settings,refresh, waarop MeshManager de knop "Status nu
 *            opvragen" vanzelf aanzet.
 *   v2.10.0 = DE TWEE LIJNEN SAMEN. De IRC-server (irc-server) en de geplande
 *            kanaalberichten (companion-hub-node) zijn parallel ontwikkeld en
 *            droegen ALLEBEI het nummer v2.9.0 -- op verschillende takken, met
 *            verschillende inhoud. Dat nummer is daarmee dubbelzinnig; de
 *            samenvoeging krijgt daarom 2.10.0 en de twee regels hieronder
 *            blijven staan zoals ze waren, want allebei zijn ze echt geflasht
 *            geweest. MAX_CHANNELS is 16 (het hoogste van de twee: 12 voor de
 *            aankondigingen, 16 omdat een app-import er negen meebrengt).
 *   v2.9.1 = EEN KANAAL PER KEER. v2.9.0 zette alle kanalen van een aankondiging in
 *            een keer in de zendwachtrij met 4 s verschil. Dat is geen zendritme: het
 *            luchtbudget (duty cycle 10 %) laat dat niet toe en de pakketbeheerder
 *            gooit weg wat niet in de wachtrij past -- in het ene kanaal kwam het aan
 *            en in het andere niet (gemeld door de eigenaar, meetbaar). Nu een kanaal
 *            per keer met een instelbare pauze (standaard 30 s), en de log/GUI zegt
 *            "in de wachtrij" waar hij eerst "verzonden" zei -- dat is niet hetzelfde.
 *   v2.9.0 = GEPLANDE KANAALBERICHTEN. De bot antwoordde alleen op wat hij HOORDE, dus
 *            het advies over pad-hashes en scope bereikte alleen wie hem aansprak.
 *            Nu ook de andere richting: tot vier tijdstippen (lokale tijd, dagmasker)
 *            die zelf een bericht in gekozen kanalen zetten. EIGEN kanaalkeuze, los van
 *            de kanalen waar hij meeleest. Zwijgt zolang de klok niet gesynct is en
 *            stuurt hetzelfde tijdstip nooit twee keer (ook niet na een herstart).
 *            /announce.cfg, /announce.json + /announce[/del|/test], GUI op het
 *            bot-tabblad. sendChannelReply() meldt nu of het pakket echt de lucht in
 *            ging -- dat mislukte voorheen stil bij een te lange tekst.
 *   v2.9.0 = IRC-SERVER OP DE NODE. Poort 6667; een gewone IRC-client praat op het
 *            mesh. De BOT-SLOTS dragen de identiteit (eigen sleutelpaar, kunnen DM's
 *            initieren en kanalen meelezen) -- rooms zijn de server-rol en snodes
 *            telemetrie, allebei de verkeerde kant op voor een chatdeelnemer. Elk
 *            account houdt permanent hetzelfde slot, want je pubkey is je adres:
 *            een hergebruikt slot laat DM's die onderweg waren bij de verkeerde
 *            persoon landen. Airtime is de begrenzing, niet het protocol -- zie
 *            docs/irc.md.
 *   v2.8.2 = DEZELFDE NODE IN TWEE ROLLEN. Een repeater die ooit op onze room-server
 *            inlogde staat in de ACL van room 0; is diezelfde repeater ook het DOEL van
 *            een CLI-sessie, dan ontsleutelt zijn loginantwoord op die ACL-ingang en niet
 *            op de rcli-kandidaat -- dezelfde sleutel, want rcli logt in met de identiteit
 *            van room 0. Het antwoord belandde dan in de client-tak, die alleen het pad
 *            bewaart en verder enkel naar een ACK kijkt: het RESPONSE werd stil weggegooid
 *            en elke sessie liep in zijn time-out ('geen loginantwoord na 3 pogingen').
 *            Nu wordt zo'n pakket eerst aan de lopende sessie aangeboden (op de VOLLE
 *            sleutel, zie isTargetPub) en pas daarna als clientverkeer verwerkt.
 *   v2.8.1 = [rcli]-DIAGNOSE. Een repeater kan een login keurig antwoorden terwijl
 *            deze kant 'geen loginantwoord' meldt (gemeten op JessaZH.VIR02). De vier
 *            stappen ertussen -- pakket binnen, sessie als kandidaat, ontsleuteld,
 *            inhoud -- waren van buiten niet te zien omdat MESH_DEBUG uit staat.
 *            Altijd-aan logging met prefix [rcli], stil zolang er geen sessie loopt.
 *   v2.8.0 = DE KLOK VAN EEN REPEATER RECHTZETTEN, als EEN job (cmd:clockfix uit de
 *            MeshManager-wachtrij). De firmware van de tegenkant weigert een klok
 *            achteruit ("ERR: clock cannot go backwards", CommonCLI), dus bij een
 *            node die VOORloopt is clkreboot (klok -> mei 2024 + herstart) de enige
 *            weg, gevolgd door 'time <epoch>'. Tussen die twee is de node
 *            onzichtbaar voor wie zijn oude tijdstempel onthield, en dat venster mag
 *            geen HTTP-ronde of clear-on-read-wachtrij bevatten -- vandaar EEN job
 *            op de node, die de sessie vasthoudt. Verloop: 'clock' lezen ->
 *            < 60 s afwijking = NIETS doen (geen herstart) -> loopt achter = alleen
 *            'time' -> loopt voor = clkreboot (geen antwoord verwacht), dan tot 3
 *            minuten elke 10 s opnieuw login + 'time' met een verse epoch, dan
 *            'clock' teruglezen. Antwoord = EEN mensleesbare zin onder cmd:clockfix.
 *            NOOIT een tweede clkreboot. clkreboot blijft als LOS commando geweigerd
 *            uit de wachtrij (de BRICK-zeef blijft staan). Nieuw in /poller.json:
 *            clockfix_ok/clockfix_fail/clockfix_last; caps meldt nu
 *            settings,refresh,clockfix.
 * Zie CHANGELOG.md in de firmware-repo.
 * ==========================================================================*/

#ifndef MESHUPTIME_VERSION
  #define MESHUPTIME_VERSION   "v2.22.5"
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
