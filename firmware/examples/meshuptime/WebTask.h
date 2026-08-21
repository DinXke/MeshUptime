#pragma once

#include <Arduino.h>

/* Webbeheer voor MeshUptime: één pagina, één JSON-antwoord, één formulier.
 *
 * BEWUST DE SYNCHRONE Arduino-WebServer (<WebServer.h>), niet
 * ESPAsyncWebServer. Op dit project is eerder gemeten dat de asynchrone
 * variant bij te weinig AANEENGESLOTEN heap halve antwoorden wegschrijft: het
 * grootste vrije blok zat rond 11,7 kB en dan is een antwoord opeens
 * afgekapt. Er is nu ruim geheugen, maar de synchrone server is de bewezen en
 * voorspelbare weg, en 'geen tweede webserver' staat in docs/ontwerp.md bij de
 * regels die niet onderhandelbaar zijn.
 *
 * Daar hangt aan vast dat de pagina géén gzip-blob en géén generator-script is.
 * Dat was nodig toen het budget 5760 byte was; nu past een leesbaar HTML-
 * document rechtstreeks in flash (PROGMEM) en kan het gewoon gelezen worden.
 *
 * De koppeling met WiFi loopt via een pointer die main.cpp zet. Geen globale
 * variabele in een header: main.cpp bepaalt de levensduur en de volgorde, en
 * deze klasse doet niets voordat zij die pointer heeft.
 *
 * DE SENSORLAAG LOOPT LANGS DEZELFDE WEG. setMonitors() krijgt de pointer naar
 * MonitorSensors; zonder die aanroep werkt de pagina wel, maar zonder
 * monitoroverzicht en met een /hook die 503 antwoordt met de reden erbij. Dat is
 * met opzet zichtbaar in plaats van stil: een monitoroverzicht dat leeg blijft
 * zonder uitleg is precies het soort raadsel dat iemand een uur kost.
 *
 * De pointer staat NIET als extern global in deze header. main.cpp is de enige
 * plek die zowel de webtaak als de sensorlaag kent, en dat blijft zo.
 *
 * ------------------------------------------------------------------------
 * NODEBEHEER -- WAAROM DAT HIER ZIT EN NIET IN EEN COMPANION-APP
 *
 * Deze node is niet met de MeshCore-companion-app te beheren, en dat zijn twee
 * ontwerpkeuzes en geen fouten. Er zit GEEN BLE op (upstream houdt usb, ble en
 * wifi in drie aparte envs omdat ze elkaars heap opeten), en deze firmware is op
 * simple_sensor gebouwd en spreekt het companion-CLIENTprotocol niet.
 *
 * Wat de node WEL heeft is zijn CLI, en dat is precies de laag die de app zelf
 * ook gebruikt zodra zij een repeater beheert. Vandaar de opzet hieronder: EEN
 * route (POST /cli) die een opdracht doorgeeft aan SensorMesh::handleCommand, en
 * daarboven knoppen en formulieren die niets anders doen dan zo'n opdrachtregel
 * samenstellen. Er is dus geen tweede schrijfpad naast de CLI -- geen tweede
 * keuring, geen tweede boekhouding, en geen instelling die via het web anders
 * uitpakt dan via serieel.
 *
 * sender_timestamp is bij ons ALTIJD 0, net als bij de seriële console in
 * main.cpp. Dat is geen detail: een handvol opdrachten (set freq, erase, get acl,
 * log, stats-*) laat CommonCLI alleen op 0 door, omdat dat "de console" betekent
 * en niet "iemand op afstand". De webinterface IS de lokale weg naar deze node --
 * hij hangt aan hetzelfde eigen netwerk als de USB-kabel -- dus hoort hij daar
 * aan de goede kant van die grens te staan. Zie de weigerlijst in handleCli()
 * voor het handjevol opdrachten waar wij zelf een streep zetten.
 *
 * GET /cfg.json leest de huidige stand RECHTSTREEKS uit NodePrefs in plaats van
 * dertig keer "get <veld>" over de CLI te sturen. Dat is niet luiheid maar
 * hetzelfde motief als bij status.json: een pagina die zijn velden voorvult mag
 * dat niet met dertig verzoeken doen op een node die tussendoor een radio moet
 * bedienen. Schrijven gaat wel over de CLI, want daar zit de keuring en het
 * wegschrijven naar flash.
 */
class WifiTask;
class MonitorSensors;
class IWebNode;    // de node-interface (SensorMesh of RoomMesh); zie IWebNode.h
class WebServer;   // vooruit verklaard: WebServer.h hoort niet in deze header

class WebTask {
public:
  /* firmware_version wordt alleen bewaard, niet gekopieerd: main.cpp geeft
   * FIRMWARE_VERSION mee en dat is een letterlijke tekst in flash. */
  void begin(WifiTask* wifi, const char* firmware_version);

  /* De sensorlaag, voor het monitoroverzicht en voor /hook. Aparte setter en
   * geen extra parameter aan begin(): zo blijft de bestaande aanroep in main.cpp
   * geldig en is de koppeling één regel die je ziet staan. */
  void setMonitors(MonitorSensors* monitors) { _mon = monitors; }

  /* De MESHLAAG, voor het toegangsbeheer. Langs dezelfde weg en om dezelfde
   * reden als setMonitors(): main.cpp is de enige plek die zowel de webtaak als
   * de mesh kent, en zonder deze aanroep werkt de pagina wel maar antwoordt
   * /acl.json met 503 en de reden erbij -- zichtbaar in plaats van stil.
   *
   * SensorMesh en niet ClientACL: het slot (acl.strict), de buurtlijst en de
   * gedeelde sleutels horen bij de mesh. Een webserver die zelf in ClientACL zou
   * schrijven, zou de gedeelde sleutel moeten uitrekenen en de uitgestelde
   * flashschrijving moeten kennen -- twee dingen die de mesh al doet.
   *
   * DEZELFDE POINTER DRAAGT NU OOK HET NODEBEHEER. handleCommand(), NodePrefs en
   * de eigen publieke sleutel zitten allemaal op SensorMesh, dus /cli en
   * /cfg.json hebben niets anders nodig dan wat main.cpp hier al meegeeft. Er is
   * daarom GEEN nieuwe regel in main.cpp nodig voor de beheerpagina; de naam van
   * deze setter is alleen ouder dan zijn taak. Hem omdopen zou main.cpp raken en
   * dat is de moeite van een naam niet waard -- vandaar deze noot in plaats van
   * een tweede setter die hetzelfde veld zet. */
  void setAcl(IWebNode* mesh) { _acl = mesh; }

  /* Kort en niet blokkerend; hoort in loop() naast the_mesh.loop(). */
  void loop();

  bool isServing() const { return _serving; }

private:
  WifiTask*       _wifi = nullptr;
  MonitorSensors* _mon = nullptr;
  IWebNode*       _acl = nullptr;
  WebServer*      _server = nullptr;
  const char*     _fw = "";
  bool            _serving = false;

  /* DE AUTHENTICATIE VAN DEZE NODE -- TWEE WEGEN, EEN SLOT.
   *
   * authOk() is de kern: een verzoek is toegelaten als het OFWEL een geldige
   * sessiecookie draagt (de mens, ingelogd via /login) OFWEL door HTTP Basic-auth
   * komt tegen de opgeslagen credential (de MeshManager-server, headless). Beide
   * paden geven dezelfde toegang; ze verschillen alleen in HOE je je aanmeldt.
   *
   * requireAuth() is de API-poort: authOk() of anders een 401 met een Basic-
   * uitdaging. De server (curl -u, MeshManager) stuurt Basic vooraf mee en werkt dus
   * gewoon door; een browser die /status.json ophaalt zonder geldige sessie krijgt
   * de 401 in zijn fetch() -- GEEN inlogpopup, want fetch() opent er geen -- en de
   * pagina stuurt zichzelf dan naar /login. Zo is de lelijke Basic-popup weg voor
   * mensen zonder dat de machineweg breekt.
   *
   * sessionValid() toetst alleen de cookie. Zie WebTask.cpp voor de sessietabel en
   * de afweging (statische RAM-tabel i.p.v. ondertekende stateless tokens). */
  bool authOk();
  bool sessionValid();
  bool requireAuth();
  void routes();

  void handleRoot();

  /* DE EIGEN INLOGPAGINA -- vervangt de browser-Basic-popup.
   *
   * handleLogin()      GET  /login  -- de nette inlogpagina (of 302 naar / als je al
   *                                     een geldige sessie hebt).
   * handleLoginPost()  POST /login  -- controleert user+pass tegen de opgeslagen
   *                                     credential, zet bij succes de sessiecookie en
   *                                     stuurt naar /; bij fout terug naar /login met
   *                                     een melding, met een niet-blokkerende anti-
   *                                     brute-force-rem (groeiende wachttijd).
   * handleLogout()     POST /logout -- wist de sessie aan beide kanten (tabel +
   *                                     verlopen cookie) en stuurt naar /login. */
  void handleLogin();
  void handleLoginPost();
  void handleLogout();

  /* POST /web/cred/reset -- zet de web-login terug op de GEBAKKEN standaard
   * (admin/meshcore) door /web.cfg te verwijderen. Achter dezelfde auth als de rest:
   * de eigenaar roept hem NA het flashen aan met de HUIDIGE (geroteerde) login. Zie
   * de uitleg boven de implementatie in WebTask.cpp. */
  void handleWebCredReset();
  void handleStatus();
  void handleWifi();
  void handleHook();
  void handleMonAdd();
  void handleMonDel();

  /* SIMULEREN EN TESTEN VAN WAARSCHUWINGEN.
   *
   * Drie POSTs en geen enkele GET, en dat is hier geen formaliteit: een GET die
   * een sensor forceert is een link die iemand kan sturen, en een browser die hem
   * voorlaadt zet de bewaking van een kanaal uit zonder dat er iemand geklikt
   * heeft. Een GET die een testbericht stuurt kost bovendien zendtijd op een
   * gedeelde band bij elke keer dat een linkchecker langskomt.
   *
   * DE KEURING ZIT HIER NIET. De grenzen -- de vervaltijd, hoeveel forceringen
   * tegelijk, hoe vaak een testbericht -- zijn eigenschappen van de sensorlaag en
   * niet van de webserver; ze staan in MonitorSensors::simSet() en
   * testRequest(). Deze drie functies lezen argumenten en zetten een SimResult om
   * in een HTTP-code, precies zoals handleMonAdd() dat met MonResult doet. Zo is
   * er één plek waar de rem zit, en niet een halve in de browser en een halve
   * hier.
   *
   * handleAlertTest() telt wél zelf de ONTVANGERS: dat is de toegangslijst en die
   * hangt aan de meshlaag (_acl), niet aan de sensorlaag. MonitorSensors krijgt
   * het getal mee en bewaart het bij de uitslag, zodat "naar hoeveel verstuurd"
   * en "hoeveel bevestigd" over dezelfde test gaan -- ook als er ondertussen
   * iemand aan de lijst sleutelt. */
  void handleSim();
  void handleSimClear();
  void handleAlertTest();

  /* Hoeveel ingangen op dit moment waarschuwingen krijgen (PERM_RECV_ALERTS_LO).
   * Staat hier omdat zowel /status.json als /alert/test het getal nodig heeft, en
   * twee keer dezelfde lus is ooit twee verschillende antwoorden. */
  uint8_t countAlertRecipients() const;

  /* NODEBEHEER.
   *
   * handleCli() is de enige route die iets aan de node verandert wat niet over
   * de monitors of de toegangslijst gaat. Alle knoppen en alle formulieren op het
   * beheertabblad komen hier langs met een opdrachtregel; er is geen tweede
   * schrijfpad. handleCfgJson() is de leeskant en verandert niets.
   *
   * DE UITGESTELDE OPDRACHT. board.reboot() en powerOff() KOMEN NIET TERUG. Een
   * opdracht die dat doet mag dus niet uitgevoerd worden vóór het antwoord de
   * deur uit is, anders krijgt de browser een afgebroken verbinding en weet
   * niemand of de herstart gelukt is of de node is omgevallen. Daarom onthoudt
   * handleCli() zo'n opdracht en voert loop() hem een halve seconde later uit --
   * dan is het antwoord verstuurd en heeft de TCP-stapel zijn beurt gehad. Geen
   * delay(), alleen een tijdstip. */
  void handleCli();
  void handleCfgJson();
  void runDeferred();

  /* DE EIGEN WEB-LOGIN VAN DEZE NODE.
   *
   * handleWebCred() is POST /web/cred: de rotatieroute. De statsserver roept hem
   * aan MET de HUIDIGE Basic-auth en zet er de nieuwe user/pass mee; dat is de
   * enige manier waarop de vloot van de gebakken standaard-login afkomt. De
   * credential leeft in /web.cfg (MonitorStore), niet in NodePrefs en niet in de
   * gebakken WEB_USER/WEB_PASS -- opgeslagen wint van gebakken, net als bij WiFi.
   * requireAuth() toetst tegen de opgeslagen waarde; het wachtwoord wordt nooit
   * getoond of teruggelezen. */
  void handleWebCred();

  /* Toegangsbeheer. Eén GET met de hele stand en drie POSTs die er iets aan
   * veranderen; net als bij de monitors gaat er niets veranderends via GET. Bij
   * een toegangslijst weegt dat zwaarder dan bij een monitor: een GET die
   * leesrecht uitdeelt is een link die iemand kan sturen. */
  void handleAclJson();
  void handleAclSet();
  void handleAclDel();
  void handleAclStrict();

  /* ROOM-BEHEER (alleen zinvol op de room-server-variant; op de sensor-variant
   * kent de node geen rooms en antwoorden deze routes met 'niet ondersteund').
   *
   * Net als bij de toegangslijst: EEN GET met de hele stand (/rooms.json, incl. de
   * join-URI per room) en POSTs die iets veranderen. De backup draagt de
   * VOLLEDIGE config incl. sleutels en staat daarom achter dezelfde auth; hij wordt
   * niet gelogd. De QR wordt in de PAGINA getekend uit de join-URI (een kleine
   * inline generator), zodat er geen QR-lib in de flash hoeft en geen externe
   * asset geladen wordt. De join-URI zelf komt uit /rooms.json, zodat ook de
   * MeshManager-server zijn eigen QR kan tekenen. */
  void handleRoomsJson();
  void handleRoomAdd();
  void handleRoomEdit();
  void handleRoomDel();
  void handleRoomsBackup();
  void handleRoomsRestore();
  /* Gemeenschappelijke poort voor de room-routes: meshlaag gekoppeld EN deze node
   * kent rooms. Verstuurt zelf het 503/501-antwoord en geeft dan false. */
  bool roomsAvailable();

  /* VIRTUELE SENSOR-NODES. Symmetrisch met /room/add|edit|del; de lijst zit in
   * /rooms.json (aparte "snodes"-array), backup/restore loopt mee met /rooms/*. */
  void handleSNodeAdd();
  void handleSNodeEdit();
  void handleSNodeDel();

  /* POST /mon/alarm -- per-sensor alarmroute (am) + room-set (rm) zetten. De
   * MeshManager-server stuurt hier de alarminstelling van een monitor naartoe.
   * Loopt via de bestaande CLI ('sensor set mon.<ch>.alert/rooms') zodat er geen
   * tweede schrijfpad ontstaat, en verifieert daarna via de getters. */
  void handleMonAlarm();

  /* Schrijft het monitoroverzicht in buf achter positie n en geeft de nieuwe
   * positie terug. Apart van handleStatus() omdat die anders één functie van
   * honderd regels wordt met twee onderwerpen erin. */
  int  appendMonitors(char* buf, size_t len, int n);

  friend void web_route_root();
  friend void web_route_status();
  friend void web_route_wifi();
  friend void web_route_hook();
  friend void web_route_monadd();
  friend void web_route_mondel();
  friend void web_route_acljson();
  friend void web_route_aclset();
  friend void web_route_acldel();
  friend void web_route_aclstrict();
  friend void web_route_cli();
  friend void web_route_cfgjson();
  friend void web_route_webcred();
  friend void web_route_credreset();
  friend void web_route_login();
  friend void web_route_loginpost();
  friend void web_route_logout();
  friend void web_route_sim();
  friend void web_route_simclear();
  friend void web_route_alerttest();
  friend void web_route_roomsjson();
  friend void web_route_roomadd();
  friend void web_route_roomedit();
  friend void web_route_roomdel();
  friend void web_route_roomsbackup();
  friend void web_route_roomsrestore();
  friend void web_route_monalarm();
  friend void web_route_snodeadd();
  friend void web_route_snodeedit();
  friend void web_route_snodedel();
};

/* WiFi-instellingen uit SPIFFS (/wifi.cfg, twee regels: SSID en wachtwoord).
 *
 * Geeft true als er een bruikbare opgeslagen instelling staat. ONTWERPEIS: de
 * opgeslagen waarde gaat vóór op de gebakken bouwvlag WIFI_SSID/WIFI_PWD. Een
 * node die naar een ander netwerk verhuist hoeft dan niet opnieuw geflasht te
 * worden -- en dat is precies het geval waarin je er niet bij kan, want zonder
 * netwerk is er ook geen webinterface.
 *
 * Vereist dat SPIFFS.begin() al gedaan is; main.cpp doet dat in setup().
 */
bool loadWifiConfig(char* ssid, size_t ssid_len, char* pwd, size_t pwd_len);

/* Schrijft /wifi.cfg. Wordt door POST /wifi gebruikt en is hier openbaar zodat
 * een instelling ook van elders (mesh-opdracht, seriële console) gezet kan
 * worden zonder de webserver na te bouwen. */
bool saveWifiConfig(const char* ssid, const char* pwd);
