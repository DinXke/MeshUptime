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
 */
class WifiTask;
class WebServer;   // vooruit verklaard: WebServer.h hoort niet in deze header

class WebTask {
public:
  /* Maximaal aantal externe controles. Statisch, want dat is de eerste regel
   * uit docs/ontwerp.md: vast maximum, vaste buffers. */
  static const uint8_t MAX_HOOKS = 8;
  static const uint8_t MAX_HOOK_NAME = 16;   // zonder afsluiter

  /* Eén melding van buiten (Uptime Kuma) over één dienst. */
  struct Hook {
    char     name[MAX_HOOK_NAME + 1];
    bool     used;
    bool     up;
    uint32_t ms;         // gemelde reactietijd, 0 als niet meegegeven
    uint32_t at_millis;  // wanneer de melding binnenkwam
  };

  /* firmware_version wordt alleen bewaard, niet gekopieerd: main.cpp geeft
   * FIRMWARE_VERSION mee en dat is een letterlijke tekst in flash. */
  void begin(WifiTask* wifi, const char* firmware_version);

  /* Kort en niet blokkerend; hoort in loop() naast the_mesh.loop(). */
  void loop();

  bool isServing() const { return _serving; }

  /* Voor later: MonitorSensors leest hier de gemelde toestanden uit. */
  const Hook* hooks() const;
  uint8_t     hookCount() const;

private:
  WifiTask*   _wifi = nullptr;
  WebServer*  _server = nullptr;
  const char* _fw = "";
  bool        _serving = false;

  bool requireAuth();
  void routes();

  void handleRoot();
  void handleStatus();
  void handleWifi();
  void handleHook();

  friend void web_route_root();
  friend void web_route_status();
  friend void web_route_wifi();
  friend void web_route_hook();
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
