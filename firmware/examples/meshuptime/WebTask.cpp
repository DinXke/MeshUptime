#include "WebTask.h"
#include "WifiTask.h"

#include <WebServer.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>

/* Inloggegevens. Uit bouwvlaggen, met een terugval zodat een verse build
 * bereikbaar is zonder dat er iets ingesteld hoeft te worden.
 *
 * LET OP -- dit is HTTP Basic authenticatie over gewoon HTTP, zonder TLS. De
 * gebruikersnaam en het wachtwoord gaan bij ELK verzoek in base64 over het
 * netwerk, en base64 is geen versleuteling: wie het verkeer kan meelezen, leest
 * het wachtwoord. Dit is dus bedoeld voor een eigen netwerk. Zet deze node NIET
 * open naar buiten (geen poort doorsturen, geen publiek adres): dan is dit
 * wachtwoord meteen leesbaar mee te lezen. Wie hem van buiten nodig heeft,
 * zet er een VPN of een reverse proxy met TLS voor.
 *
 * De vlaggen horen in platformio.local.ini te staan, niet in de repo. De
 * terugval hieronder is met opzet zichtbaar en dus onbruikbaar als geheim.
 */
#ifndef WEB_USER
  #define WEB_USER "admin"
#endif
#ifndef WEB_PASS
  #define WEB_PASS "meshcore"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION "v1.17.0"
#endif

#define WEB_PORT        80
#define WEB_REALM       "MeshUptime"
#define WIFI_CFG_PATH   "/wifi.cfg"

/* Vaste buffers, in statisch geheugen en niet op de stapel: de loop-taak deelt
 * zijn stapel met de meshstapel, en een kilobyte antwoord hoort daar niet op. */
static char g_json[1024];
static char g_ssid_shown[33] = {0};   // alleen om te tonen; nooit het wachtwoord

static WebTask::Hook g_hooks[WebTask::MAX_HOOKS];

/* Eén WebServer, statisch. Geen new/malloc; de klasse houdt er een pointer
 * naar zodat WebServer.h buiten de header blijft. */
static WebServer g_server(WEB_PORT);

/* De routes van de synchrone server willen std::function. Met deze ene pointer
 * en handlers ZONDER capture blijft dat een gewone functiepointer, en zet
 * std::function niets op de heap. Een lambda die 'this' vangt zou dat wel
 * kunnen doen, en dat is precies wat hier niet mag. */
static WebTask* g_self = nullptr;

void web_route_root()   { if (g_self) g_self->handleRoot(); }
void web_route_status() { if (g_self) g_self->handleStatus(); }
void web_route_wifi()   { if (g_self) g_self->handleWifi(); }
void web_route_hook()   { if (g_self) g_self->handleHook(); }

/* ------------------------------ hulpmiddelen ------------------------------ */

/* Argument uit het verzoek naar een vaste buffer.
 *
 * AFWIJKING, met opzet: WebServer::arg() en argName() GEVEN een String terug --
 * dat zit in de bibliotheek en daar is geen andere ingang voor. Wat hier telt is
 * dat onze eigen verwerkingspaden geen String gebruiken: de waarde gaat meteen
 * naar een char-buffer met een vaste maat, en verder in deze module bestaat
 * alleen die buffer. Er wordt niets geplakt, bewaard of laten groeien.
 */
static bool getArg(WebServer& s, const char* name, char* out, size_t out_len) {
  out[0] = 0;
  int n = s.args();
  for (int i = 0; i < n; i++) {
    if (strcmp(s.argName(i).c_str(), name) == 0) {
      strlcpy(out, s.arg(i).c_str(), out_len);
      return true;
    }
  }
  return false;
}

/* Naam van een externe controle: hoogstens MAX_HOOK_NAME tekens, en alleen
 * letters, cijfers, punt, streepje en liggend streepje. Zo hoeft de naam
 * nergens ontsnapt te worden -- niet in JSON, niet in HTML, en niet in een
 * meshbericht straks. */
static bool validHookName(const char* n) {
  size_t len = strlen(n);
  if (len == 0 || len > WebTask::MAX_HOOK_NAME) return false;
  for (size_t i = 0; i < len; i++) {
    char c = n[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
           || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

/* Een SSID mag alles bevatten, dus die moet wél ontsnapt worden voordat hij in
 * JSON gaat. Anders maakt één aanhalingsteken in de netwerknaam de pagina stuk
 * en zoek je dat op het verkeerde niveau. */
static void jsonEscape(const char* in, char* out, size_t out_len) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 7 < out_len; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
    else if (c < 0x20)         { o += snprintf(out + o, out_len - o, "\\u%04x", c); }
    else                       { out[o++] = c; }
  }
  out[o] = 0;
}

static const char* wifiStateName(const WifiTask* w) {
  if (w == nullptr) return "onbekend";
  switch (w->state()) {
    case WifiTask::OFF:        return "uit";
    case WifiTask::CONNECTING: return "verbinden";
    case WifiTask::ONLINE:     return "online";
    case WifiTask::RETRYING:   return "opnieuw";
    case WifiTask::AP_MODE:    return "eigen ap";
  }
  return "onbekend";
}

/* ------------------------------- de pagina -------------------------------- */

/* Eén zelfstandig document in flash: geen CDN, geen framework, geen externe
 * stylesheet. De pagina haalt zijn gegevens bij /status.json; de browser stuurt
 * daar dezelfde Basic-inloggegevens mee omdat het dezelfde herkomst is. */
static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="nl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeshUptime</title><style>
body{font:15px system-ui,sans-serif;margin:0;padding:1em;max-width:34em;color:#111;background:#f6f6f6}
h1{font-size:1.2em;margin:0 0 .6em}h2{font-size:1em;margin:1.5em 0 .3em}
table{border-collapse:collapse;width:100%}td,th{text-align:left;padding:.25em .4em;border-bottom:1px solid #ddd}
th{width:11em;font-weight:600;color:#444}
label{display:block;margin:.6em 0 .1em}
input{width:100%;padding:.4em;box-sizing:border-box}
button{margin-top:.8em;padding:.5em 1.2em}
.m{color:#666;font-size:.85em}
</style></head><body>
<h1>MeshUptime</h1>
<table id="s"></table>
<h2>Externe controles</h2>
<table id="h"></table>
<p class="m">Gemeld via /hook?name=&lt;naam&gt;&amp;up=&lt;0|1&gt;[&amp;ms=&lt;getal&gt;]</p>
<h2>WiFi</h2>
<form method="post" action="/wifi">
<label>Netwerk (SSID)<input name="ssid" id="ssid" maxlength="32" required></label>
<label>Wachtwoord<input name="pwd" type="password" maxlength="64"></label>
<button>Opslaan</button>
<p class="m">Opgeslagen instellingen gaan voor op de ingebouwde. Pas actief na herstart.</p>
</form>
<script>
var F=[["fw","Firmware"],["wifi","WiFi"],["ip","IP"],["rssi","RSSI (dBm)"],
["reason","Laatste reden"],["reconnects","Verbindingen"],["resets","Harde resets"],
["uptime","Uptime (s)"],["heap","Vrije heap"],["largest","Grootste blok"]];
function u(){fetch("status.json").then(function(r){return r.json()}).then(function(d){
var t=document.getElementById("s");t.innerHTML="";
F.forEach(function(f){var r=t.insertRow(),h=document.createElement("th");
h.textContent=f[1];r.appendChild(h);r.insertCell().textContent=d[f[0]]});
var e=document.getElementById("h");e.innerHTML="";
if(!d.hooks.length){e.insertRow().insertCell().textContent="nog geen meldingen"}
d.hooks.forEach(function(k){var r=e.insertRow();r.insertCell().textContent=k.n;
r.insertCell().textContent=k.up?"op":"neer";r.insertCell().textContent=k.ms+" ms";
r.insertCell().textContent=k.age+" s"});
var s=document.getElementById("ssid");if(!s.value){s.value=d.ssid}
})}
u();setInterval(u,5000);
</script></body></html>)HTML";

/* -------------------------------- opslag ---------------------------------- */

bool loadWifiConfig(char* ssid, size_t ssid_len, char* pwd, size_t pwd_len) {
  if (ssid == nullptr || ssid_len < 2) return false;
  ssid[0] = 0;
  if (pwd != nullptr && pwd_len > 0) pwd[0] = 0;

  /* SPIFFS geeft een File-object terug en dat alloceert intern. Dat is geen
   * verwerkingspad: dit gebeurt één keer bij het opstarten en één keer bij het
   * opslaan, nooit per verzoek. */
  File f = SPIFFS.open(WIFI_CFG_PATH, FILE_READ);
  if (!f) return false;

  size_t n = f.readBytesUntil('\n', ssid, ssid_len - 1);
  ssid[n] = 0;
  if (pwd != nullptr && pwd_len > 1) {
    n = f.readBytesUntil('\n', pwd, pwd_len - 1);
    pwd[n] = 0;
  }
  f.close();

  /* Regeleindes van een editor of van een browser weghalen. */
  for (char* p = ssid; *p; p++) if (*p == '\r') { *p = 0; break; }
  if (pwd != nullptr) for (char* p = pwd; *p; p++) if (*p == '\r') { *p = 0; break; }

  return ssid[0] != 0;
}

bool saveWifiConfig(const char* ssid, const char* pwd) {
  File f = SPIFFS.open(WIFI_CFG_PATH, FILE_WRITE);
  if (!f) return false;
  f.printf("%s\n%s\n", ssid ? ssid : "", pwd ? pwd : "");
  f.close();
  return true;
}

/* ------------------------------- WebTask ---------------------------------- */

void WebTask::begin(WifiTask* wifi, const char* firmware_version) {
  _wifi = wifi;
  _fw = firmware_version ? firmware_version : FIRMWARE_VERSION;
  _server = &g_server;
  g_self = this;

  memset(g_hooks, 0, sizeof(g_hooks));

  /* Wat er in het formulier komt te staan: eerst de opgeslagen instelling,
   * anders de gebakken vlag. Het wachtwoord wordt nooit getoond of verstuurd. */
  char dummy_pwd[2];
  if (!loadWifiConfig(g_ssid_shown, sizeof(g_ssid_shown), dummy_pwd, sizeof(dummy_pwd))) {
#ifdef WIFI_SSID
    strlcpy(g_ssid_shown, WIFI_SSID, sizeof(g_ssid_shown));
#endif
  }

  routes();
}

void WebTask::routes() {
  _server->on("/", HTTP_GET, web_route_root);
  _server->on("/status.json", HTTP_GET, web_route_status);
  _server->on("/wifi", HTTP_POST, web_route_wifi);
  /* Uptime Kuma laat de methode vrij; beide kunnen dus. */
  _server->on("/hook", HTTP_GET, web_route_hook);
  _server->on("/hook", HTTP_POST, web_route_hook);
  _server->onNotFound([]() { g_server.send(404, "text/plain", "niet gevonden"); });
}

void WebTask::loop() {
  if (_wifi == nullptr) return;

  /* Alleen bedienen als er een netwerk is. In eigen-AP-modus juist WEL: dat is
   * dan de enige weg naar de instellingen, en dat is waarvoor die modus in
   * WifiTask bestaat. */
  if (!_wifi->isOnline() && !_wifi->isApMode()) return;

  if (!_serving) {
    _server->begin();
    /* Zonder dit doet handleClient() een delay(1) als er geen verzoek is. Eén
     * milliseconde per ronde is genoeg om de LoRa-timing te laten schuiven, en
     * de radio gaat voor. */
    _server->enableDelay(false);
    _serving = true;
  }

  /* Keert meteen terug als er geen verzoek klaarstaat: de bibliotheek werkt met
   * een toestandsmachine en gaat pas lezen zodra er gegevens zijn. Wij wachten
   * hier zelf op niets en gebruiken nergens delay(). */
  _server->handleClient();
}

const WebTask::Hook* WebTask::hooks() const { return g_hooks; }

uint8_t WebTask::hookCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_HOOKS; i++) if (g_hooks[i].used) n++;
  return n;
}

/* Elke route zit hierachter. Mislukt het, dan een 401 met de realm, zodat een
 * browser om inloggegevens vraagt en een script (Uptime Kuma) een duidelijke
 * fout krijgt in plaats van stilte. */
bool WebTask::requireAuth() {
  if (_server->authenticate(WEB_USER, WEB_PASS)) return true;
  _server->requestAuthentication(BASIC_AUTH, WEB_REALM);
  return false;
}

void WebTask::handleRoot() {
  if (!requireAuth()) return;
  _server->sendHeader("Cache-Control", "no-store");
  _server->send_P(200, "text/html", PAGE_HTML);
}

void WebTask::handleStatus() {
  if (!requireAuth()) return;

  IPAddress ip = _wifi->isApMode() ? WiFi.softAPIP() : WiFi.localIP();
  char esc[80];
  jsonEscape(g_ssid_shown, esc, sizeof(esc));

  int n = snprintf(g_json, sizeof(g_json),
      "{\"fw\":\"%s\",\"wifi\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"rssi\":%d,"
      "\"reason\":%u,\"reconnects\":%lu,\"resets\":%lu,\"uptime\":%lu,"
      "\"heap\":%lu,\"largest\":%lu,\"ssid\":\"%s\",\"hooks\":[",
      _fw, wifiStateName(_wifi),
      ip[0], ip[1], ip[2], ip[3],
      (int)WiFi.RSSI(),
      (unsigned)_wifi->lastDisconnectReason(),
      (unsigned long)_wifi->reconnectCount(),
      (unsigned long)_wifi->hardResetCount(),
      (unsigned long)(millis() / 1000),
      (unsigned long)ESP.getFreeHeap(),
      /* Het getal dat in dit project telt. Vrije heap zegt te weinig: lwip
       * heeft een AANEENGESLOTEN blok nodig, en juist daar ging het eerder mis. */
      (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
      esc);

  bool first = true;
  for (uint8_t i = 0; i < MAX_HOOKS && n > 0 && (size_t)n < sizeof(g_json) - 24; i++) {
    if (!g_hooks[i].used) continue;
    n += snprintf(g_json + n, sizeof(g_json) - n,
                  "%s{\"n\":\"%s\",\"up\":%d,\"ms\":%lu,\"age\":%lu}",
                  first ? "" : ",", g_hooks[i].name, g_hooks[i].up ? 1 : 0,
                  (unsigned long)g_hooks[i].ms,
                  (unsigned long)((millis() - g_hooks[i].at_millis) / 1000));
    first = false;
  }
  strlcat(g_json, "]}", sizeof(g_json));

  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_json);
}

void WebTask::handleWifi() {
  if (!requireAuth()) return;

  char ssid[33], pwd[65];
  if (!getArg(*_server, "ssid", ssid, sizeof(ssid)) || ssid[0] == 0) {
    _server->send(400, "text/plain", "ssid ontbreekt");
    return;
  }
  getArg(*_server, "pwd", pwd, sizeof(pwd));   // leeg mag: open netwerk

  if (!saveWifiConfig(ssid, pwd)) {
    _server->send(500, "text/plain", "opslaan mislukt");
    return;
  }
  strlcpy(g_ssid_shown, ssid, sizeof(g_ssid_shown));

  /* Zelf niets aan de radio doen. Midden in een verzoek de WiFi omleggen haalt
   * de verbinding weg waarover je antwoord nog moet gaan, en WifiTask heeft zijn
   * eigen toestandsmachine met een waakhond. Opslaan en om een herstart vragen
   * is genoeg, en het is bovendien de eerlijke boodschap. */
  _server->send(200, "text/html",
      "<!DOCTYPE html><meta charset=\"utf-8\"><p>Opgeslagen. Herstart de node om "
      "het nieuwe netwerk te gebruiken.<p><a href=\"/\">terug</a>");
}

/* GET of POST /hook?name=<naam>&up=<0|1>[&ms=<getal>]
 *
 * TODO: deze ronde valideert de route en zet het resultaat in de tabel. De
 * koppeling naar telemetrie (een waarde per controle) en naar meshwaarschuwingen
 * (bericht bij een toestandsovergang, met opnieuw proberen) komt daarna. Zie
 * docs/ontwerp.md: één bericht per overgang of herhalen zolang de storing duurt
 * is nog een open vraag, en herhalen kost zendtijd op een gedeelde band.
 */
void WebTask::handleHook() {
  if (!requireAuth()) return;

  char name[MAX_HOOK_NAME + 2], up[8], ms[12];
  if (!getArg(*_server, "name", name, sizeof(name)) || !validHookName(name)) {
    _server->send(400, "text/plain", "name: 1-16 tekens uit a-z A-Z 0-9 . - _");
    return;
  }
  if (!getArg(*_server, "up", up, sizeof(up))
      || (strcmp(up, "0") != 0 && strcmp(up, "1") != 0)) {
    _server->send(400, "text/plain", "up: 0 of 1");
    return;
  }
  uint32_t ms_val = 0;
  if (getArg(*_server, "ms", ms, sizeof(ms)) && ms[0] != 0) {
    char* end = nullptr;
    unsigned long v = strtoul(ms, &end, 10);
    if (end == ms || *end != 0) {
      _server->send(400, "text/plain", "ms: getal");
      return;
    }
    ms_val = (uint32_t)v;
  }

  /* Bestaande naam bijwerken, anders een vrije plaats. Vol is vol: een vast
   * maximum is hier een eigenschap en geen beperking die weggewerkt moet
   * worden. Wie er meer wil, verhoogt MAX_HOOKS en meet het geheugen opnieuw. */
  Hook* slot = nullptr;
  for (uint8_t i = 0; i < MAX_HOOKS; i++) {
    if (g_hooks[i].used && strcmp(g_hooks[i].name, name) == 0) { slot = &g_hooks[i]; break; }
  }
  if (slot == nullptr) {
    for (uint8_t i = 0; i < MAX_HOOKS; i++) {
      if (!g_hooks[i].used) { slot = &g_hooks[i]; break; }
    }
  }
  if (slot == nullptr) {
    _server->send(503, "text/plain", "tabel vol (max 8)");
    return;
  }

  strlcpy(slot->name, name, sizeof(slot->name));
  slot->used = true;
  slot->up = (up[0] == '1');
  slot->ms = ms_val;
  slot->at_millis = millis();

  _server->send(200, "text/plain", "ok");
}
