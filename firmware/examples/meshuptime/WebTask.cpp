#include "WebTask.h"
#include "WifiTask.h"
#include "MonitorSensors.h"

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
 * zijn stapel met de meshstapel, en een kilobyte antwoord hoort daar niet op.
 *
 * DE MAAT VAN g_json, NAGEREKEND en niet geraden. Het antwoord bestaat uit de
 * vaste velden plus één regel per kanaal:
 *
 *   vaste velden (fw, wifi, ip, rssi, ... ssid van 32 tekens)     ~ 300 byte
 *   4 vaste kanalen, korte namen en adressen                      4 x  ~130
 *   8 monitorvakjes, in het duurste geval:
 *     {"ch":12,"n":"<16 tekens>","h":"<40 tekens>","i":3600,"st":"pauze",
 *      "ms":4294967295,"f":4294967295,"c":4294967295,"k":"gemeld",
 *      "age":4294967295,"sev":"warn"}                             8 x  ~190
 *   de uitleg bij een ontbrekende sensorlaag ("monwarn")           ~ 130
 *                                                                 ----------
 *                                                                  ~ 2470
 *
 * 3072 geeft daar ruim 600 byte marge boven, en de lus kapt bovendien af zodra
 * er minder dan JSON_TAIL byte over is -- want een half JSON-document maakt de
 * pagina stuk op een plek waar niemand de oorzaak zoekt.
 */
static char g_json[3072];
#define JSON_TAIL  240      /* ruimte die altijd vrij blijft voor één regel + "]}" */

static char g_ssid_shown[33] = {0};   // alleen om te tonen; nooit het wachtwoord

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
void web_route_monadd() { if (g_self) g_self->handleMonAdd(); }
void web_route_mondel() { if (g_self) g_self->handleMonDel(); }

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

/* HIER STOND EEN EIGEN NAAMZEEF (validHookName). Die is weg: de keuring van
 * namen en adressen zit nu in MonitorSensors::validName/validHost, en dat is
 * dezelfde zeef die de CLI gebruikt. Twee zeven die 99% hetzelfde doen, geven
 * ooit een naam die het ene pad aanneemt en het andere weigert -- en dat is een
 * fout die je alleen vindt door beide paden naast elkaar te proberen.
 *
 * Wat die zeef oplevert blijft de reden dat er hieronder niets ontsnapt hoeft te
 * worden: namen en adressen bestaan alleen uit letters, cijfers, punt, streepje
 * en liggend streepje, en dat is veilig in JSON en in HTML-tekst. Het SSID is de
 * enige waarde die wél alles mag bevatten, en die gaat door jsonEscape().
 */

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
 * stylesheet, geen externe afbeelding. De pagina haalt zijn gegevens bij
 * /status.json; de browser stuurt daar dezelfde Basic-inloggegevens mee omdat het
 * dezelfde herkomst is.
 *
 * DE VORMTAAL IS DIE VAN MESHMANAGER (MeshStats/server/app/static/style.css), met
 * dezelfde kleurtokens en dezelfde tabel- en kaartopmaak, zodat de twee projecten
 * familie van elkaar zijn. Twee dingen zijn met opzet anders:
 *
 *  1. GEEN Google Fonts. MeshManager laadt "Space Grotesk" en "JetBrains Mono"
 *     van een CDN; deze pagina moet het zonder netwerk buiten deze node doen, dus
 *     alleen de fallback-stapels. De mono-stapel is daarbij niet cosmetisch:
 *     kanaalnummers, tijden en tellers horen in mono, want dan staan de cijfers
 *     onder elkaar en is een tabel met metingen in één blik te lezen.
 *  2. De kleuren zijn SEMANTISCH: groen = op, rood = neer, amber = pauze of
 *     verouderd, muted = onbekend. Dat is het hele punt van deze pagina -- in één
 *     oogopslag zien welke dienst stuk is -- en daarom staat er ook nergens een
 *     kleur alleen voor de sier.
 *
 * Donker is de standaard, licht komt via prefers-color-scheme; beide paletten
 * staan letterlijk in de tokens hieronder.
 */
static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="nl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeshUptime</title><style>
:root{
--bg:#0b0f14;--card:#121a23;--border:#1e2b3a;--text:#d7e2ea;--muted:#7d8fa0;
--accent:#35e08c;--amber:#ffb454;--cyan:#4cc9f0;--red:#ff5c5c;
--sans:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
--mono:ui-monospace,"Cascadia Code",Consolas,monospace}
@media (prefers-color-scheme:light){:root{
--bg:#eef3f1;--card:#ffffff;--border:#d2ddd7;--text:#16241d;--muted:#5b6b63;
--accent:#0e9c60;--amber:#b8741a;--cyan:#0b7fa8;--red:#cf3b3b}}
*{box-sizing:border-box}
body{font-family:var(--sans);font-size:15px;line-height:1.45;margin:0;
padding:1.2rem;max-width:62rem;color:var(--text);background:var(--bg)}
h1{font-size:1.15rem;letter-spacing:.02em;margin:0 0 .15rem}
h1 span{font-family:var(--mono);font-size:.66rem;text-transform:uppercase;
letter-spacing:.16em;color:var(--muted);display:block;margin-top:.25rem}
h2{font-family:var(--mono);font-size:.68rem;text-transform:uppercase;
letter-spacing:.13em;color:var(--muted);margin:1.6rem 0 .5rem;font-weight:600}
.card{background:linear-gradient(180deg,rgba(255,255,255,.025),transparent 55%),
var(--card);border:1px solid var(--border);border-radius:10px;padding:1rem}
/* De kanaaltabel heeft acht kolommen en past op een telefoon niet. Dan schuift
   ZIJ, binnen haar eigen kaart -- en niet de hele pagina, want een body die
   horizontaal scrollt maakt alle andere tekst onleesbaar. */
.card.pad0{padding:.35rem .25rem;overflow-x:auto}
.card.pad0 table{min-width:34rem}
.tilegrid{display:grid;grid-template-columns:repeat(auto-fill,minmax(155px,1fr));
gap:.65rem}
.tile{background:linear-gradient(180deg,rgba(255,255,255,.025),transparent 55%),
var(--card);border:1px solid var(--border);border-radius:10px;padding:.7rem .8rem}
.tile .k{font-family:var(--mono);font-size:.62rem;text-transform:uppercase;
letter-spacing:.13em;color:var(--muted)}
.tile .v{font-family:var(--mono);font-size:1.2rem;margin-top:.25rem}
.tile .s{font-family:var(--mono);font-size:.7rem;color:var(--muted)}
table{width:100%;border-collapse:collapse}
th,td{text-align:left;padding:.5rem .6rem;border-bottom:1px solid var(--border)}
th{font-family:var(--mono);font-size:.68rem;text-transform:uppercase;
letter-spacing:.13em;color:var(--muted);font-weight:600}
tr:last-child td{border-bottom:none}
td.num,th.num{text-align:right;font-family:var(--mono)}
td.nm{font-family:var(--mono)}
/* De vier vaste kanalen laten hun bijzaken naar achteren zakken, maar NIET hun
   naam en NIET hun toestand: die twee zijn waar iemand met een app ernaast naar
   kijkt, en de toestand moet zijn semantische kleur houden. */
tr.fix td:not(.st):not(.nm){color:var(--muted)}
.statusdot{width:9px;height:9px;border-radius:50%;display:inline-block;
margin-right:.45rem;vertical-align:baseline}
.on{background:var(--accent);box-shadow:0 0 8px var(--accent);
animation:pulse 2.5s infinite}
.off{background:var(--red);box-shadow:0 0 6px rgba(255,92,92,.5)}
.warn{background:var(--amber);box-shadow:0 0 6px var(--amber)}
.unk{background:var(--muted)}
@keyframes pulse{50%{box-shadow:0 0 3px var(--accent)}}
.st{font-family:var(--mono);font-size:.85rem;white-space:nowrap}
.c-on{color:var(--accent)}.c-off{color:var(--red)}.c-warn{color:var(--amber)}
.c-unk{color:var(--muted)}
.note{font-size:.84rem;color:var(--muted);margin:.6rem 0 0}
.note b{color:var(--text);font-weight:600}
.why{border-left:3px solid var(--cyan);background:linear-gradient(90deg,
rgba(76,201,240,.07),transparent 70%);padding:.7rem .9rem;border-radius:0 8px 8px 0;
font-size:.87rem;margin:0 0 .8rem}
.why b{color:var(--cyan)}
code,kbd{font-family:var(--mono);font-size:.85em;background:var(--bg);
border:1px solid var(--border);border-radius:4px;padding:.05em .35em}
.row{display:flex;gap:.7rem;flex-wrap:wrap}.row>label{flex:1 1 9rem}
label{display:block;font-family:var(--mono);font-size:.64rem;
text-transform:uppercase;letter-spacing:.12em;color:var(--muted);margin:.2rem 0 0}
input{width:100%;margin-top:.3rem;padding:.45rem .55rem;font-family:var(--mono);
font-size:.9rem;color:var(--text);background:var(--bg);border:1px solid var(--border);
border-radius:6px}
input:focus{outline:none;border-color:var(--cyan)}
button{margin-top:.9rem;padding:.5rem 1.1rem;font-family:var(--mono);
font-size:.72rem;text-transform:uppercase;letter-spacing:.12em;cursor:pointer;
color:var(--bg);background:var(--accent);border:1px solid var(--accent);
border-radius:6px}
button:hover{filter:brightness(1.1)}
td button{margin:0;padding:.15rem .5rem;font-size:.62rem;color:var(--red);
background:transparent;border-color:var(--border)}
td button:hover{border-color:var(--red);filter:none}
.ok{color:var(--accent)}.bad{color:var(--red)}
#msg{font-family:var(--mono);font-size:.8rem;min-height:1.2rem;margin-top:.5rem}
</style></head><body>

<h1>MeshUptime<span id="sub">bewaking &middot; heltec v3</span></h1>

<div class="tilegrid" id="t"></div>

<h2>Monitoroverzicht &mdash; de kanaalkaart</h2>
<p class="why"><b>Waarom een MeshCore-app hier <i>switch</i> en <i>genericsensor</i>
toont en niet deze namen:</b> de telemetrie gaat als CayenneLPP over het mesh, en
dat formaat draagt per waarde alleen een <b>kanaalnummer</b>, een type en de
waarde. Er is geen naamveld &mdash; er is dus niets dat de app zou kunnen tonen.
Deze tabel <i>is</i> de koppeling tussen kanaalnummer en dienst; leg hem naast je
app. Over het mesh is dezelfde lijst op te vragen met de DM-opdracht
<code>list</code>, en over serieel met <code>sensor list</code>.</p>
<div class="card pad0"><table id="k"></table></div>
<p class="note"><b>Toestand:</b>
<span class="c-on">op</span> / <span class="c-off">neer</span> gemeten &middot;
<span class="c-warn">pauze</span> = onze wifi is weg, dus dit is de laatst gemeten
stand en niet de huidige &middot;
<span class="c-unk">?</span> = nog geen uitslag &middot;
<span class="c-warn">stil</span> = een gemelde dienst waarvan de melding te lang
uitblijft, toestand dus onbekend.<br>
Kanalen 1&ndash;4 staan vast. Een kanaal dat eenmaal is uitgedeeld wordt niet
opnieuw gebruikt, ook niet na verwijderen: een dashboard dat &quot;kanaal 6&quot;
bewaard heeft, mag nooit stil naar een andere dienst gaan wijzen.</p>

<h2>Monitor toevoegen</h2>
<div class="card"><form id="a">
<div class="row">
<label>Naam<input name="name" maxlength="16" required placeholder="google"></label>
<label>Adres<input name="host" maxlength="40" required placeholder="8.8.8.8"></label>
<label>Interval (s)<input name="int" type="number" min="10" max="3600" value="60" required></label>
</div>
<button>Toevoegen</button>
<div id="msg"></div>
<p class="note">Adres <code>-</code> maakt geen ping-monitor maar een van buiten
<b>gemelde</b> dienst: die pingen wij niet, Uptime Kuma levert de uitslag via
<code>/hook?name=&lt;naam&gt;&amp;up=&lt;0|1&gt;[&amp;ms=&lt;getal&gt;][&amp;every=&lt;s&gt;]</code>.
Het interval is dan de afgesproken meldperiode; blijft een melding drie perioden
uit (minimaal 90&nbsp;s), dan wordt de toestand onbekend en gaat er een
waarschuwing over het mesh. Een onbekende naam op <code>/hook</code> maakt de
dienst zelf aan en antwoordt met het kanaal dat hij kreeg.</p>
</form></div>

<h2>WiFi</h2>
<div class="card"><form method="post" action="/wifi">
<div class="row">
<label>Netwerk (SSID)<input name="ssid" id="ssid" maxlength="32" required></label>
<label>Wachtwoord<input name="pwd" type="password" maxlength="64"></label>
</div>
<button>Opslaan</button>
<p class="note">Opgeslagen instellingen gaan voor op de ingebouwde. Pas actief na
herstart.</p>
</form></div>

<script>
/* Ernst -> kleurklasse. Eén plek, want de tegels, de bolletjes en de tabel moeten
   dezelfde betekenis aan dezelfde kleur geven. De ernst komt uit /status.json en
   wordt hier NIET uit de toestandstekst geraden: "aan" betekent op kanaal 2 iets
   goeds en op kanaal 3 iets om op te letten. */
var SEV={ok:"on",bad:"off",warn:"warn"};
function cls(s){return SEV[s]||"unk"}
function dot(s){return'<span class="statusdot '+cls(s)+'"></span>'}
function hms(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60);
return h?h+"h"+(m<10?"0":"")+m+"m":m+"m"}
function kb(b){return(b/1024).toFixed(0)+" kB"}

function tiles(d){
var mon=d.mon||[],up=0,tot=0;
mon.forEach(function(m){if(m.k=="vast"){return}tot++;if(m.sev=="ok"){up++}});
var pw=d.mains==1?"net":(d.mains==0?"batterij":"?");
var t=[["voeding",pw,d.volts+" V",cls(d.mains==1?"ok":(d.mains==0?"warn":"unk"))],
["wifi",d.wifi,d.rssi+" dBm / "+d.ip,cls(d.wifi=="online"?"ok":"bad")],
["monitors",tot?up+"/"+tot:"0",d.paused?"bevroren: geen wifi":"op / totaal",
cls(tot?(up==tot?"ok":"warn"):"unk")],
["uptime",hms(d.uptime),"resets "+d.resets,"unk"],
["vrije heap",kb(d.heap),"grootste blok "+kb(d.largest),"unk"],
["firmware",d.fw,"verbindingen "+d.reconnects,"unk"]];
var h="";t.forEach(function(x){h+='<div class="tile"><div class="k">'+x[0]+
'</div><div class="v c-'+x[3]+'">'+x[1]+'</div><div class="s">'+x[2]+"</div></div>"});
document.getElementById("t").innerHTML=h}

var KH=[["kan","num"],["naam",""],["adres",""],["interval","num"],
["toestand",""],["ms","num"],["mislukt","num"],["",""]];

function table(d){
var e=document.getElementById("k");e.innerHTML="";
var hr=e.insertRow();KH.forEach(function(x){var h=document.createElement("th");
h.textContent=x[0];h.className=x[1];hr.appendChild(h)});
if(d.monwarn){var r=e.insertRow(),c=r.insertCell();c.colSpan=KH.length;
c.className="bad";c.textContent=d.monwarn;return}
(d.mon||[]).forEach(function(m){
var r=e.insertRow();if(m.k=="vast"){r.className="fix"}
var c=r.insertCell();c.className="num";c.textContent=m.ch;
c=r.insertCell();c.className="nm";c.textContent=m.n;
c=r.insertCell();c.textContent=m.h;
c=r.insertCell();c.className="num";c.textContent=m.i?m.i+" s":"–";
c=r.insertCell();c.className="st c-"+cls(m.sev);
c.innerHTML=dot(m.sev)+m.st+(m.k=="gemeld"&&m.age?" "+m.age+"s":"");
c=r.insertCell();c.className="num";c.textContent=m.ms?m.ms:"–";
c=r.insertCell();c.className="num";
c.textContent=m.k=="vast"?"–":m.f+"/"+m.c;
c=r.insertCell();
if(m.k!="vast"){var b=document.createElement("button");b.textContent="wis";
b.onclick=function(){if(confirm("Monitor '"+m.n+"' verwijderen?\n\nKanaal "+m.ch+
" wordt daarna NIET opnieuw uitgedeeld zolang er nog een nieuw nummer vrij is."))
{post("monitor/del","name="+encodeURIComponent(m.n))}};c.appendChild(b)}})}

function say(t,ok){var m=document.getElementById("msg");
m.className=ok?"ok":"bad";m.textContent=t}

function post(u,b){return fetch(u,{method:"POST",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b})
.then(function(r){return r.text().then(function(t){
say(t.trim(),r.ok);if(r.ok){u2()}})})}

function u2(){fetch("status.json").then(function(r){return r.json()})
.then(function(d){tiles(d);table(d);
var s=document.getElementById("ssid");if(!s.value){s.value=d.ssid}
document.getElementById("sub").textContent=d.ssid?"bewaking · "+d.ssid:"bewaking"})}

document.getElementById("a").onsubmit=function(ev){ev.preventDefault();
/* f.elements[..] en niet f.name: op een formulier IS .name het name-attribuut
   van het formulier zelf en niet het veld dat zo heet. */
var f=ev.target.elements;
post("monitor","name="+encodeURIComponent(f["name"].value)+
"&host="+encodeURIComponent(f["host"].value)+
"&int="+encodeURIComponent(f["int"].value))};

u2();setInterval(u2,5000);
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
  /* Beheer alleen via POST. Een GET die een monitor aanmaakt of wist, wordt door
   * elke browser, elke prefetch en elke linkchecker gevolgd -- en verwijderen is
   * hier niet omkeerbaar, want het kanaal komt niet terug. */
  _server->on("/monitor", HTTP_POST, web_route_monadd);
  _server->on("/monitor/del", HTTP_POST, web_route_mondel);
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

/* DE TABEL VAN 8 MELDINGEN IS WEG.
 *
 * Er stond hier een eigen rij Hook-structs waarin /hook zijn meldingen zette.
 * Die is opgeruimd, en niet omdat het netter staat: het was een TWEEDE
 * boekhouding naast MonitorSensors, met zijn eigen namen en zijn eigen plaatsen.
 * Twee boekhoudingen lopen na de eerste verwijdering uit elkaar en dan is er geen
 * manier meer om te zien welke van de twee de waarheid is. Een gemelde dienst is
 * nu een monitor als elke andere -- met een kanaal uit dezelfde toewijzer.
 */

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
      "\"heap\":%lu,\"largest\":%lu,\"ssid\":\"%s\","
      "\"mains\":%d,\"volts\":\"%.3f\",\"paused\":%d,\"mon\":[",
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
      esc,
      /* Zonder sensorlaag geen mening in plaats van een verzonnen mening: -1 en
       * 0.000 zijn voor de pagina het teken dat er niets te tonen is. */
      _mon != nullptr ? (_mon->isMains() ? 1 : 0) : -1,
      _mon != nullptr ? _mon->lastVolts() : 0.0f,
      _mon != nullptr && _mon->monitorsPaused() ? 1 : 0);

  if (n < 0 || (size_t)n >= sizeof(g_json)) {   /* kan niet; vangnet */
    _server->send(500, "text/plain", "antwoord te groot");
    return;
  }

  n = appendMonitors(g_json, sizeof(g_json), n);

  /* Zonder sensorlaag is het overzicht niet leeg maar UITGELEGD. Een lege tabel
   * zonder reden is precies het raadsel waar deze pagina voor gemaakt is. */
  if (_mon == nullptr && (size_t)n < sizeof(g_json) - 160) {
    n += snprintf(g_json + n, sizeof(g_json) - n,
                  ",\"monwarn\":\"sensorlaag niet gekoppeld: "
                  "voeg in main.cpp setup() toe: web_task.setMonitors(&sensors);\"");
  }

  strlcat(g_json, "}", sizeof(g_json));

  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_json);
}

/* Het monitoroverzicht in JSON, achter de "mon":[ die de aanroeper al schreef.
 * Sluit de rij zelf af met ']' en geeft de nieuwe schrijfpositie terug.
 *
 * DE VOLLEDIGE KAART, dus ook de vier vaste kanalen. Wie met een app ernaast
 * zit, zoekt niet "mijn ping-monitors" op maar "wat is kanaal 3", en dan moet het
 * antwoord er staan -- ook als dat antwoord "batterijvoeding" is. De vaste vier
 * hebben geen interval en geen mislukkingen; die velden staan er toch, met een 0,
 * zodat de pagina niet per regel hoeft te kijken of een veld bestaat.
 *
 * Namen en adressen hoeven niet ontsnapt te worden: MonitorSensors::validName en
 * validHost laten alleen letters, cijfers, punt, streepje en liggend streepje
 * door. Dat is de reden dat die zeef zo streng is.
 *
 * WAAROM ER EEN "sev"-VELD BIJ ZIT en de pagina de kleur niet zelf uit de
 * toestandstekst raadt: de BETEKENIS van een toestand kent alleen deze kant.
 * "aan" op kanaal 2 (netvoeding) is goed, "aan" op kanaal 3 (batterijvoeding) is
 * juist een waarschuwing -- dezelfde tekst, de omgekeerde betekenis. Een pagina
 * die op het woord afgaat, verft die twee hetzelfde en dan liegt de kleur. Vandaar
 * één veld met de ernst: "ok", "warn", "bad" of "unk".
 */
int WebTask::appendMonitors(char* buf, size_t len, int n) {
  char volts[16];

  /* De vaste vier. Kanaal 1 is van SensorMesh zelf (LPP_VOLTAGE); hij staat erbij
   * omdat de kaart anders bij 2 begint en de lezer zich afvraagt wat 1 was. */
  if (_mon != nullptr) {
    snprintf(volts, sizeof(volts), "%.3f V", _mon->lastVolts());

    /* Op netvoeding is goed, op batterij is een waarschuwing -- op BEIDE
     * kanalen, want ze zeggen hetzelfde met het omgekeerde woord. */
    const char* pw_sev = _mon->isMains() ? "ok" : "warn";

    n += snprintf(buf + n, len - n,
        "{\"ch\":1,\"n\":\"spanning\",\"h\":\"batterij\",\"i\":0,\"st\":\"%s\","
        "\"ms\":0,\"f\":0,\"c\":0,\"k\":\"vast\",\"age\":0,\"sev\":\"unk\"},"
        "{\"ch\":%u,\"n\":\"netvoeding\",\"h\":\"klemspanning\",\"i\":0,\"st\":\"%s\","
        "\"ms\":0,\"f\":0,\"c\":0,\"k\":\"vast\",\"age\":0,\"sev\":\"%s\"},"
        "{\"ch\":%u,\"n\":\"batterijvoeding\",\"h\":\"klemspanning\",\"i\":0,\"st\":\"%s\","
        "\"ms\":0,\"f\":0,\"c\":0,\"k\":\"vast\",\"age\":0,\"sev\":\"%s\"},"
        "{\"ch\":%u,\"n\":\"wifi\",\"h\":\"deze node\",\"i\":0,\"st\":\"%s\","
        "\"ms\":0,\"f\":0,\"c\":0,\"k\":\"vast\",\"age\":0,\"sev\":\"%s\"}",
        volts,
        (unsigned)MonitorSensors::CH_MAINS,   _mon->isMains() ? "aan" : "uit", pw_sev,
        (unsigned)MonitorSensors::CH_BATTERY, _mon->isMains() ? "uit" : "aan", pw_sev,
        (unsigned)MonitorSensors::CH_WIFI,    _mon->isWifiOnline() ? "online" : "weg",
        _mon->isWifiOnline() ? "ok" : "bad");

    const bool paused = _mon->monitorsPaused();

    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) {
      if (!_mon->monitorUsed(i)) continue;
      /* Afkappen in plaats van over de rand schrijven. snprintf zou zelf ook
       * afkappen, maar dan midden in een regel, en dan is het HELE document geen
       * geldige JSON meer -- de pagina blijft dan leeg zonder dat er iets in de
       * logs staat. */
      if ((size_t)n > len - JSON_TAIL) break;

      const bool push = _mon->monitorIsPush(i);

      /* Dezelfde rangorde als in getSettingValue() en in de DM-lijst, en dat is
       * geen toeval: drie plekken die dezelfde toestand anders zouden benoemen,
       * zijn drie plekken die elkaar tegenspreken.
       *
       * "pauze" en "stil" zijn amber en niet rood, want ze zeggen niet dat een
       * dienst stuk is -- ze zeggen dat WIJ het niet weten. Rood is voorbehouden
       * aan wat is vastgesteld. */
      const char* st;
      const char* sev;
      if (paused)                      { st = "pauze"; sev = "warn"; }
      else if (_mon->monitorIsStale(i)) { st = "stil";  sev = "warn"; }
      else if (!_mon->monitorSeeded(i)) { st = "?";     sev = "unk";  }
      else if (_mon->monitorIsUp(i))    { st = "op";    sev = "ok";   }
      else                              { st = "neer";  sev = "bad";  }

      n += snprintf(buf + n, len - n,
          ",{\"ch\":%u,\"n\":\"%s\",\"h\":\"%s\",\"i\":%u,\"st\":\"%s\","
          "\"ms\":%lu,\"f\":%lu,\"c\":%lu,\"k\":\"%s\",\"age\":%lu,\"sev\":\"%s\"}",
          (unsigned)_mon->monitorChannel(i),
          _mon->monitorName(i),
          push ? "(gemeld)" : _mon->monitorHost(i),
          (unsigned)_mon->monitorInterval(i),
          st,
          (unsigned long)_mon->monitorPingMs(i),
          (unsigned long)_mon->monitorFails(i),
          (unsigned long)_mon->monitorChecks(i),
          push ? "gemeld" : "ping",
          (unsigned long)_mon->monitorReportAge(i),
          sev);
    }
  }

  n += snprintf(buf + n, len - n, "]");
  return n;
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

/* Een geheel getal uit een argument, met grenzen. Geeft false bij rommel; een
 * lege of ontbrekende waarde levert 'def'. Dit is het enige plekje waar /hook en
 * de beheerroutes cijfers lezen, dus hier staat het één keer. */
static bool getUInt(WebServer& s, const char* name, unsigned long lo,
                    unsigned long hi, unsigned long def, unsigned long* out) {
  char buf[12];
  *out = def;
  if (!getArg(s, name, buf, sizeof(buf)) || buf[0] == 0) return true;

  char* end = nullptr;
  unsigned long v = strtoul(buf, &end, 10);
  if (end == buf || *end != 0) return false;
  if (v < lo || v > hi) return false;
  *out = v;
  return true;
}

/* Antwoord op een MonResult. 400 voor een verkeerde invoer, 409 voor een botsing
 * met iets dat er al staat, 503 als de node vol zit -- de aanroeper (Uptime Kuma,
 * een script) kan daarop iets anders doen, en op een 400 niet. */
static int httpCodeFor(MonitorSensors::MonResult r) {
  switch (r) {
    case MonitorSensors::MON_OK:          return 200;
    case MonitorSensors::MON_ERR_TAKEN:
    case MonitorSensors::MON_ERR_KIND:    return 409;
    case MonitorSensors::MON_ERR_FULL:    return 503;
    case MonitorSensors::MON_ERR_UNKNOWN: return 404;
    default:                              return 400;
  }
}

/* GET of POST /hook?name=<naam>&up=<0|1>[&ms=<getal>][&every=<s>]
 *
 * Dit is nu WEL doorgekoppeld: de gemelde dienst krijgt een kanaal uit de
 * kanaaltoewijzer van MonitorSensors -- dezelfde die de ping-monitors gebruikt --
 * en verschijnt daarna als LPP_SWITCH plus, als hij op staat, een
 * LPP_GENERIC_SENSOR met de gemelde tijd. Waarschuwen bij een overgang loopt via
 * monitorAlert()/monitorAlertText(), die main.cpp al aan alertIf() hangt; er is
 * dus geen tweede waarschuwingsweg.
 *
 * 'every' is de meldperiode die de melder aanhoudt. Blijft een melding drie van
 * die perioden uit (met een ondergrens van 90 s), dan is de toestand onbekend --
 * anders zou een dienst waarvan Uptime Kuma zelf plat ligt eeuwig "op" blijven
 * staan, en dat is juist het geval dat de gebruiker wil weten.
 */
void WebTask::handleHook() {
  if (!requireAuth()) return;

  if (_mon == nullptr) {
    _server->send(503, "text/plain",
        "sensorlaag niet gekoppeld: voeg in main.cpp setup() toe: "
        "web_task.setMonitors(&sensors);\n");
    return;
  }

  /* Ruim genomen zodat een te lange naam als "naam" afgekeurd wordt en niet
   * stil afgekapt tot een naam die wel door de zeef komt. */
  char name[MON_NAME_LEN + 4], up[8];
  if (!getArg(*_server, "name", name, sizeof(name))
      || !MonitorSensors::validName(name)) {
    _server->send(400, "text/plain", MonitorSensors::monResultText(MonitorSensors::MON_ERR_NAME));
    return;
  }
  if (!getArg(*_server, "up", up, sizeof(up))
      || (strcmp(up, "0") != 0 && strcmp(up, "1") != 0)) {
    _server->send(400, "text/plain", "up: 0 of 1");
    return;
  }

  unsigned long ms_val = 0, every = 0;
  if (!getUInt(*_server, "ms", 0, 0xFFFFFFFFUL, 0, &ms_val)) {
    _server->send(400, "text/plain", "ms: getal");
    return;
  }
  if (!getUInt(*_server, "every", MON_INTERVAL_MIN, MON_INTERVAL_MAX, 0, &every)) {
    _server->send(400, "text/plain", "every: 10-3600 (s)");
    return;
  }

  uint8_t ch = 0;
  MonitorSensors::MonResult r =
      _mon->reportMonitor(name, up[0] == '1', (uint32_t)ms_val, (uint16_t)every, &ch);

  if (r != MonitorSensors::MON_OK) {
    _server->send(httpCodeFor(r), "text/plain", MonitorSensors::monResultText(r));
    return;
  }

  /* 200 en niet 202: de melding IS nu telemetrie. Het kanaalnummer staat erin
   * omdat dat het enige is waaraan de aanroeper zijn dienst later terugvindt --
   * CayenneLPP draagt geen namen. */
  char msg[80];
  snprintf(msg, sizeof(msg), "ok %s -> kanaal %u\n", name, (unsigned)ch);
  _server->send(200, "text/plain", msg);
}

/* POST /monitor  (name, host, int)
 *
 * Dezelfde keuring als "sensor set mon.add", want het IS dezelfde keuring:
 * createMonitor() doet validName/validHost en de intervalgrenzen zelf. Deze
 * functie leest alleen argumenten uit en zet het antwoord om in een HTTP-code.
 */
void WebTask::handleMonAdd() {
  if (!requireAuth()) return;

  if (_mon == nullptr) {
    _server->send(503, "text/plain", "sensorlaag niet gekoppeld");
    return;
  }

  char name[MON_NAME_LEN + 4], host[MON_HOST_LEN + 4];
  getArg(*_server, "name", name, sizeof(name));
  getArg(*_server, "host", host, sizeof(host));

  unsigned long ivl = 0;
  if (!getUInt(*_server, "int", MON_INTERVAL_MIN, MON_INTERVAL_MAX,
               MON_INTERVAL_DEFAULT, &ivl)) {
    _server->send(400, "text/plain",
                  MonitorSensors::monResultText(MonitorSensors::MON_ERR_INTERVAL));
    return;
  }

  uint8_t ch = 0;
  MonitorSensors::MonResult r = _mon->createMonitor(name, host, (uint16_t)ivl, &ch);
  if (r != MonitorSensors::MON_OK) {
    _server->send(httpCodeFor(r), "text/plain", MonitorSensors::monResultText(r));
    return;
  }

  char msg[80];
  snprintf(msg, sizeof(msg), "ok %s -> kanaal %u\n", name, (unsigned)ch);
  _server->send(200, "text/plain", msg);
}

/* POST /monitor/del  (name)
 *
 * Het kanaal komt hierna NIET terug zolang er nog een nummer is dat nooit is
 * uitgedeeld -- zie allocChannel(). Dat staat in het antwoord, want wie dit via
 * een script doet ziet de waarschuwing in de browser niet. */
void WebTask::handleMonDel() {
  if (!requireAuth()) return;

  if (_mon == nullptr) {
    _server->send(503, "text/plain", "sensorlaag niet gekoppeld");
    return;
  }

  char name[MON_NAME_LEN + 4];
  getArg(*_server, "name", name, sizeof(name));

  MonitorSensors::MonResult r = _mon->deleteMonitor(name);
  if (r != MonitorSensors::MON_OK) {
    _server->send(httpCodeFor(r), "text/plain", MonitorSensors::monResultText(r));
    return;
  }

  char msg[96];
  snprintf(msg, sizeof(msg), "ok %s verwijderd; kanaal blijft vergeven\n", name);
  _server->send(200, "text/plain", msg);
}
