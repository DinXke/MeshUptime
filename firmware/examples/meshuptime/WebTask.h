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
 */
class WifiTask;
class MonitorSensors;
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

  /* Kort en niet blokkerend; hoort in loop() naast the_mesh.loop(). */
  void loop();

  bool isServing() const { return _serving; }

private:
  WifiTask*       _wifi = nullptr;
  MonitorSensors* _mon = nullptr;
  WebServer*      _server = nullptr;
  const char*     _fw = "";
  bool            _serving = false;

  bool requireAuth();
  void routes();

  void handleRoot();
  void handleStatus();
  void handleWifi();
  void handleHook();
  void handleMonAdd();
  void handleMonDel();

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
