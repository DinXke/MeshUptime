#include "WebTask.h"
#include "WifiTask.h"
#include "MonitorSensors.h"
#include "MonitorStore.h"
#include "SensorMesh.h"

#include <WebServer.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>

/* De GEBAKKEN radio-instellingen, om ze op de pagina NAAST de huidige te kunnen
 * zetten. Dat is de kern van de waarschuwing bij freq/bw/sf/cr: een getal alleen
 * zegt niets, "869.618 en de rest van dit mesh staat op 869.618" zegt alles.
 *
 * Dezelfde #ifndef-terugval als in SensorMesh.cpp, en om dezelfde reden: LORA_CR
 * komt bij deze variant niet uit een bouwvlag en de waarde staat daar in de .cpp
 * en niet in een header. Twee plekken met dezelfde terugval is hier minder erg
 * dan een header aanpassen die van upstream is. */
#ifndef LORA_FREQ
  #define LORA_FREQ   869.618
#endif
#ifndef LORA_BW
  #define LORA_BW      62.5
#endif
#ifndef LORA_SF
  #define LORA_SF         8
#endif
#ifndef LORA_CR
  #define LORA_CR         5
#endif

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
 *   het budget-, simulatie- en testblok ("tb"/"sim"/"test")        ~ 370
 *   4 vaste kanalen, korte namen en adressen                      4 x  ~210
 *   MON_MAX_MONITORS (32) vakjes, in het duurste geval:
 *     {"ch":36,"n":"<16 tekens>","h":"<40 tekens>","i":3600,"st":"pauze",
 *      "ms":4294967295,"f":4294967295,"c":4294967295,"k":"gemeld",
 *      "age":4294967295,"sev":"warn","si":33,"sm":"down","sl":3600,
 *      "tms":1,"tb":9,"drop":0}                                   32 x  ~235
 *   de uitleg bij een ontbrekende sensorlaag ("monwarn")           ~ 130
 *                                                                 ----------
 *                                                                  ~ 9160
 *
 * DAAROM 10240. Dat is een grote sprong en die is verdiend door twee
 * wijzigingen die elkaar versterken: MON_MAX_MONITORS is van 8 naar 32 gegaan, en
 * elke regel draagt nu ook zijn simulatiestand en zijn bytekosten.
 *
 * WAAROM NIET AFKAPPEN IN PLAATS VAN GROEIEN. Dat is precies de fout die dit
 * antwoord juist moet helpen voorkomen: een monitor die stil uit de LIJST valt
 * is even onzichtbaar als een monitor die stil uit de TELEMETRIE valt. De lus
 * kapt nog steeds af als het onverwacht toch niet past -- een half JSON-document
 * maakt de pagina leeg zonder spoor in de logs -- maar met deze maat gebeurt dat
 * bij 32 volle monitors niet.
 *
 * DE PRIJS, en die hoort hier te staan: dit antwoord wordt elke 5 seconden
 * opgehaald, dus bij 32 monitors is dat ~9 kB per 5 s over de wifi. Dat is op een
 * eigen netwerk te doen, maar het is wel de reden waarom /acl.json indertijd een
 * eigen antwoord met een eigen tempo kreeg. Wordt dit ooit krap, dan is de
 * volgende stap dezelfde: de monitorlijst uit status.json halen en apart en
 * langzamer ophalen, met alleen de METINGEN op 5 s.
 */
static char g_json[10240];
#define JSON_TAIL  320      /* ruimte die altijd vrij blijft voor één regel + "]}" */

/* Dwingt de rekensom hierboven af bij het compileren in plaats van bij het eerste
 * stil afgekapte antwoord op een node in het veld -- net als de static_assert bij
 * g_acl. MON_MAX_MONITORS is met een bouwvlag te verhogen; dan moet deze buffer
 * mee. */
static_assert(300 + 370 + 4 * 210 + MON_MAX_MONITORS * 235 + 130 + JSON_TAIL
              <= sizeof(g_json),
              "g_json te klein voor MON_MAX_MONITORS -- zie de rekensom hierboven");

/* EEN EIGEN BUFFER VOOR /acl.json, en niet g_json erbij.
 *
 * Twee redenen. De maat: een toegangslijst draagt VOLLEDIGE publieke sleutels,
 * en dat is 64 hextekens per regel -- dat past niet in de marge van g_json.
 * En het tempo: de pagina haalt status.json elke 5 seconden op omdat metingen
 * veranderen, maar een toegangslijst verandert alleen als er iemand op klikt.
 * Die twee in één antwoord proppen zou 4 kB per 5 seconden over de wifi sturen
 * voor gegevens die stilstaan.
 *
 * DE MAAT, NAGEREKEND:
 *   vaste velden ("strict", "max", de tellers)                    ~   80 byte
 *   MAX_CLIENTS (20) ingangen:
 *     {"k":"<64 hex>","n":"<47>","p":255,"a":4294967295,"hb":1}   20 x ~ 165
 *   MAX_NEIGHBOURS (12) buren, idem plus signaal en hops:
 *     {"k":"<64>","n":"<47>","t":4,"h":63,"s":"-20.2",
 *      "a":4294967295,"c":4294967295,"in":1}                      12 x ~ 185
 *                                                                 ----------
 *                                                                  ~ 4520
 *
 * De 47 is geen schatting: de naam gaat door jsonEscape() naar een buffer van
 * NB_JSON_NAME byte, en die kapt af. Zonder die grens zou een naam van 23
 * tekens die volledig uit aanhalingstekens bestaat 6x zo lang worden en klopt
 * de rekensom niet meer. 6144 geeft ruim 1,5 kB marge, en de lus kapt bovendien
 * af zodra er minder dan ACL_TAIL over is -- een half JSON-document maakt de
 * pagina stuk op een plek waar niemand de oorzaak zoekt.
 */
static char g_acl[6144];
#define ACL_TAIL  320       /* ruimte die altijd vrij blijft voor één regel + "]}" */

/* De rekensom hierboven is geen decoratie. MAX_CLIENTS (20) en MAX_NEIGHBOURS
 * (12) zijn met een bouwvlag te verhogen -- platformio.local.ini zet nu al
 * MAX_CONTACTS=32 -- en dan moet g_acl mee. Dit dwingt dat af bij het compileren
 * in plaats van bij het eerste stil afgekapte antwoord op een node in het veld. */
static_assert(80 + MAX_CLIENTS * 165 + MAX_NEIGHBOURS * 185 + ACL_TAIL <= sizeof(g_acl),
              "g_acl te klein voor MAX_CLIENTS/MAX_NEIGHBOURS -- zie de rekensom hierboven");

static char g_ssid_shown[33] = {0};   // alleen om te tonen; nooit het wachtwoord

/* ---------------------- buffers voor het nodebeheer ----------------------- */

/* DE OPDRACHTBUFFER. 176 en niet 160, en handleCommand krijgt een MUTEERBARE
 * char* omdat hij in de opdracht schrijft (hij hakt hem in stukken op de spaties,
 * zie parseTextParts en de 'xx|'-prefix bovenaan SensorMesh::handleCommand). Een
 * const char* of een String zou hier dus niet eens werken.
 *
 * De grens die we afdwingen is 160, dezelfde als de seriële console in main.cpp:
 * wat daar past hoort hier te passen en omgekeerd, anders neemt het ene pad een
 * opdracht aan die het andere afkapt. De extra 16 byte zijn er alleen zodat een
 * TE LANGE opdracht als te lang AFGEKEURD wordt en niet stil afgekapt tot iets
 * dat wel door de zeef komt -- afkappen midden in een waarde is precies hoe je
 * per ongeluk 'set name MeshUptim' zet. */
static char g_cmd[176];
#define CLI_CMD_MAX  160

/* DE ANTWOORDBUFFER, NAGEREKEND en niet geraden. De langste uitvoer die
 * CommonCLI kan schrijven:
 *
 *   'sensor list <n>' -- de lus stopt pas als de schrijfpositie 134 VOORBIJ is,
 *   dus de laatste regel begint nog op 133. Onze langste regel is
 *   "mon.12.state=" (13) + de waarde uit s_mon_val_buf (MON_HOST_LEN-1 = 40)
 *   + "\n"                                                     -> 133 + 54 = 187
 *   daarna komt "... next:%d" er nog achter (hoogstens 12)      ->        199
 *   en een 'xx|'-prefix wordt vooraan teruggekaatst (+3)        ->        202
 *
 *   'region ...'      -- exportTo(reply, 160), dus 160 harde grens
 *   'get owner.info'  -- "> " plus 159 tekens                   ->        161
 *   'set prv.key ...' -- vaste tekst (33) + 64 hextekens        ->         97
 *
 * 202 is dus de bovengrens en 320 geeft daar meer dan 100 byte marge boven. De
 * fout zelf (ongebonden sprintf in CommonCLI.cpp) is niet van ons; dit is de rand
 * dichtzetten aan de kant die we bezitten, net als main.cpp doet.
 *
 * NIET korter maken zonder deze som opnieuw te maken: met een buffer van 160
 * ging de node in een LoadProhibited-panic op precies 'sensor list' met
 * beginindex 0. */
static char g_reply[320];

/* GET /cfg.json. Eén antwoord met de hele stand van NodePrefs, hoogstens één keer
 * per keer dat iemand het beheertabblad opent -- dus geen buffer die naast
 * g_json of g_acl hoeft te passen in tempo, alleen in maat:
 *
 *   ~40 velden, gemiddeld 20 byte per veld  ("f_max_uns":255,)      ~ 800 byte
 *   node_name (32) en owner_info (120), ontsnapt tot 6x per teken,
 *   maar AFGEKAPT op CFG_TXT_MAX zodat de som klopt              2 x ~ 200
 *   de eigen publieke sleutel, 64 hextekens                          ~  75
 *                                                                 ----------
 *                                                                  ~ 1275
 *
 * 1792 geeft daar ruim 500 byte marge boven. */
static char g_cfg[1792];

/* Wat er hoogstens van een tekstveld in cfg.json terechtkomt. Zonder deze grens
 * is de maat van g_cfg niet na te rekenen: jsonEscape() kan één teken tot zes
 * tekens maken, en owner_info mag 119 tekens lang zijn. Afkappen is hier
 * onschuldig -- het veld is er om VOOR TE VULLEN, en wie zijn eigenaarsregel op
 * 199 tekens ontsnapte tekens heeft staan ziet hem in de console met
 * 'get owner.info' voluit. */
#define CFG_TXT_MAX  200

/* De UITGESTELDE OPDRACHT: alleen voor opdrachten die niet terugkomen (reboot,
 * clkreboot). Kort, want dat is de hele verzameling; te lang zou hier betekenen
 * dat er iets anders in staat dan bedoeld. */
static char g_deferred[16] = {0};
static unsigned long g_deferred_at = 0;

/* Het masker van kanalen dat OOIT is uitgedeeld (bit 0 = kanaal 5).
 *
 * MonitorSensors houdt dit bij in _cfg.ch_ever_used, en dat veld is privé en
 * heeft geen accessor -- MonitorSensors.* mag in deze opdracht niet aangeraakt
 * worden, dus is er geen directe weg. Wat er WEL is: het staat in
 * /monitors.cfg, en MonitorStore::load() is publiek. Vandaar deze twee bronnen
 * bij elkaar geveegd:
 *
 *  1. bij begin() EEN keer het bestand lezen -- dat dekt kanalen die vóór deze
 *     start al vergeven en daarna verwijderd zijn, en die zijn nergens anders
 *     meer te zien;
 *  2. daarna bij elke opbouw van cfg.json de kanalen die NU in gebruik zijn
 *     erbij OR-en. Dat dekt het gat van de uitgestelde flashschrijving: een
 *     monitor die net is aangemaakt staat twee seconden lang nog niet in het
 *     bestand, maar zijn kanaal is meteen in gebruik.
 *
 * Het is dus een verzamelaar die alleen bits BIJ zet en nooit weghaalt -- want
 * dat is ook wat ch_ever_used doet. Eén keer per start naar SPIFFS en daarna
 * nooit meer: een verzoekpad hoort niet in flash te gaan lezen. */
static uint32_t g_ever_mask = 0;

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
void web_route_acljson()   { if (g_self) g_self->handleAclJson(); }
void web_route_aclset()    { if (g_self) g_self->handleAclSet(); }
void web_route_acldel()    { if (g_self) g_self->handleAclDel(); }
void web_route_aclstrict() { if (g_self) g_self->handleAclStrict(); }
void web_route_cli()       { if (g_self) g_self->handleCli(); }
void web_route_cfgjson()   { if (g_self) g_self->handleCfgJson(); }
void web_route_sim()       { if (g_self) g_self->handleSim(); }
void web_route_simclear()  { if (g_self) g_self->handleSimClear(); }
void web_route_alerttest() { if (g_self) g_self->handleAlertTest(); }

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
<title>MeshUptime</title>
<script>
/* HET THEMA VOOR HET EERSTE RENDEREN. In de kop en niet onderaan bij de rest van
   het script, en dat is de hele reden dat dit hier staat: een keuze die pas na
   het laden gezet wordt, geeft bij elke verversing een flits van het verkeerde
   thema. Dat is precies het detail waar een themaknop op stukloopt.

   Het attribuut komt op het ROOT-element. Dat is met opzet: de tabellen en de
   tegels worden door JavaScript herbouwd bij elke verversing, en alles wat op die
   elementen staat is dan weg. Het root-element wordt nooit herbouwd -- dezelfde
   les als bij de knoppen die hun handler kwijtraakten.

   In een try, want localStorage kan geweigerd worden (privacystand van de
   browser). Dan valt het gewoon terug op de systeemstand in plaats van de pagina
   stuk te maken op een voorkeur. */
try{var t=localStorage.getItem("mu-theme");
if(t=="light"||t=="dark"){document.documentElement.setAttribute("data-theme",t)}}
catch(e){}
</script>
<style>
/* DE DRIE THEMASTANDEN.
 *
 *   systeem (standaard)  geen data-theme; volgt prefers-color-scheme
 *   licht                data-theme="light"
 *   donker               data-theme="dark"
 *
 * DONKER STAAT OP :root en is dus de basis; licht komt er twee keer bovenop, en
 * die twee keer is geen slordigheid maar het gevolg van hoe CSS werkt: een
 * media-query en een attribuutselector zijn niet in één regel te combineren
 * zonder :has() of een bouwstap, en dit document heeft geen bouwstap. De twee
 * blokken staan daarom pal onder elkaar, zodat een kleur die in het ene verandert
 * meteen naast het andere staat.
 *
 * De :not([data-theme="dark"]) in de media-query is het scharnier van het geheel:
 * zonder die uitzondering zou een browser die op licht staat de EXPLICIETE
 * donkerkeuze overrulen, en dan doet de knop de helft van de tijd niets. */
:root{
--bg:#0b0f14;--card:#121a23;--border:#1e2b3a;--text:#d7e2ea;--muted:#7d8fa0;
--accent:#35e08c;--amber:#ffb454;--cyan:#4cc9f0;--red:#ff5c5c;
--sans:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;
--mono:ui-monospace,"Cascadia Code",Consolas,monospace}
@media (prefers-color-scheme:light){:root:not([data-theme="dark"]){
--bg:#eef3f1;--card:#ffffff;--border:#d2ddd7;--text:#16241d;--muted:#5b6b63;
--accent:#0e9c60;--amber:#b8741a;--cyan:#0b7fa8;--red:#cf3b3b}}
:root[data-theme="light"]{
--bg:#eef3f1;--card:#ffffff;--border:#d2ddd7;--text:#16241d;--muted:#5b6b63;
--accent:#0e9c60;--amber:#b8741a;--cyan:#0b7fa8;--red:#cf3b3b}
*{box-sizing:border-box}
/* De kop met de themaknop ernaast. De knop staat rechtsboven, in de vorm van de
   andere knoppen op deze pagina, en toont WELKE stand actief is -- een knop die
   alleen een symbool toont laat je raden of je in de systeemstand zit of in een
   afgedwongen stand die er nu net hetzelfde uitziet. */
.top{display:flex;align-items:flex-start;gap:1rem}
.top h1{flex:1 1 auto}
.top button{margin:0;flex:0 0 auto;background:transparent;color:var(--muted);
border-color:var(--border)}
.top button:hover{color:var(--cyan);border-color:var(--cyan);filter:none}
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
.card.pad0 table{min-width:44rem}
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
td button:disabled{opacity:.45;cursor:not-allowed}
.ok{color:var(--accent)}.bad{color:var(--red)}
#msg,#kmsg,#tmsg{font-family:var(--mono);font-size:.8rem;min-height:1.2rem;
margin-top:.5rem}
/* DE MELDREGEL ONDER DE RIJ DIE BEWERKT WORDT.
   Dit is de reparatie van een gemelde fout: een afgekeurde naam ("UDM Pro" --
   een spatie mag niet) meldde zich in #msg, en dat vakje hoort bij het
   formulier 'Monitor toevoegen' zeven honderd pixels lager. De knop deed dus
   wel wat, maar het antwoord stond buiten beeld -- en een knop waarvan je het
   antwoord niet ziet, is voor wie erop klikt een knop die niets doet. De
   melding hoort waar de klik was: in een eigen regel direct onder de rij. */
tr.emsg td{padding:.35rem .6rem;font-family:var(--mono);font-size:.78rem;
white-space:normal}
/* Vinkjes horen NIET in de hoofdletterstijl van de andere labels: die stijl is
   voor kopjes boven een invoerveld, en hier is de tekst het label zelf. */
label.cb{flex:0 0 auto;font-family:var(--sans);font-size:.9rem;
text-transform:none;letter-spacing:normal;color:var(--text);margin:0;
display:flex;align-items:center;gap:.35rem;cursor:pointer}
label.cb input{width:auto;margin:0}
/* Een sleutel van 64 hextekens past nergens. De eerste en laatste tekens zijn
   wat iemand vergelijkt; de volle waarde zit in title= en in het klembord. */
td.key{font-family:var(--mono);font-size:.8rem;white-space:nowrap;cursor:copy}
/* HET SLOT. De open stand is GEEL EN NIET STIL: een open deur die zich als open
   deur presenteert is een keuze, een open deur die eruitziet als een gesloten
   deur is een fout. Daarom is dit een banner en geen vinkje ergens rechts. */
.lock{border:1px solid var(--border);border-radius:10px;padding:.85rem 1rem;
display:flex;gap:.9rem;align-items:center;flex-wrap:wrap;margin-bottom:.8rem}
.lock.open{border-color:var(--amber);background:linear-gradient(90deg,
rgba(255,180,84,.1),transparent 75%)}
.lock.shut{border-color:var(--accent);background:linear-gradient(90deg,
rgba(53,224,140,.08),transparent 75%)}
.lock .t{flex:1 1 18rem;font-size:.9rem}
.lock .t b{font-family:var(--mono);text-transform:uppercase;letter-spacing:.1em;
font-size:.7rem;display:block;margin-bottom:.2rem}
.lock.open .t b{color:var(--amber)}.lock.shut .t b{color:var(--accent)}
.lock button{margin:0;flex:0 0 auto}
.lock.shut button{color:var(--text);background:transparent;
border-color:var(--border)}
/* ------------------------------ tabbladen -------------------------------- */
/* De pagina heeft drie onderwerpen gekregen en past niet meer op één rol. De
   KANAALKAART is waarvoor dit apparaat bestaat, dus die staat op het eerste
   tabblad en zakt niet meer weg onder dertig instelvelden. Tabbladen en geen
   losse pagina's: er is één document in flash, en een tweede pagina zou een
   tweede route en een tweede keer dezelfde CSS zijn. */
.tabs{display:flex;gap:.35rem;margin:1rem 0 .2rem;border-bottom:1px solid
var(--border);flex-wrap:wrap}
.tabs button{margin:0;padding:.45rem .9rem;background:transparent;
color:var(--muted);border:1px solid transparent;border-bottom:none;
border-radius:8px 8px 0 0}
.tabs button:hover{color:var(--text);filter:none}
.tabs button.on{color:var(--accent);border-color:var(--border);
background:var(--card);margin-bottom:-1px}
/* Inklapbaar per groep. Dicht bij het openen, want een beheerpagina die alles
   openslaat leest als een berg en niet als een keuze. */
details{border:1px solid var(--border);border-radius:10px;margin:.5rem 0;
background:var(--card)}
details[open]{border-color:var(--cyan)}
summary{cursor:pointer;padding:.7rem .9rem;font-family:var(--mono);
font-size:.7rem;text-transform:uppercase;letter-spacing:.13em;color:var(--text)}
summary:hover{color:var(--cyan)}
summary::marker{color:var(--muted)}
details>div{padding:0 .9rem .9rem}
details.rf[open]{border-color:var(--red)}
details.rf summary{color:var(--red)}
/* ------------------------------- de console ------------------------------ */
/* De uitvoer is een <pre> in mono en met een vaste hoogte: opdrachtuitvoer
   heeft betekenisvolle witruimte (region-bomen, sensor list) en die mag niet
   platgeslagen worden. Nieuwste bovenaan, want dat is waar je net op geklikt
   hebt en dan hoef je niet te scrollen om je eigen antwoord te zien. */
#out{font-family:var(--mono);font-size:.8rem;white-space:pre-wrap;
word-break:break-word;margin:.7rem 0 0;padding:.7rem .8rem;height:15rem;
overflow-y:auto;background:var(--bg);border:1px solid var(--border);
border-radius:8px}
#out .e{border-bottom:1px solid var(--border);padding-bottom:.45rem;
margin-bottom:.45rem}
#out .e:last-child{border-bottom:none}
#out .q{color:var(--cyan)}
#out .r{color:var(--text)}
#out .x{color:var(--red)}
.cmdrow{display:flex;gap:.5rem;align-items:stretch}
.cmdrow input{margin-top:0;flex:1 1 auto}
.cmdrow button{margin-top:0;flex:0 0 auto}
/* Snelknoppen: één rij, allemaal even zwaar behalve de gevaarlijke. */
.quick{display:flex;gap:.45rem;flex-wrap:wrap;margin-top:.2rem}
.quick button{margin-top:.5rem;background:transparent;color:var(--text);
border-color:var(--border)}
.quick button:hover{border-color:var(--cyan);color:var(--cyan);filter:none}
.quick button.dng{color:var(--red)}
.quick button.dng:hover{border-color:var(--red);color:var(--red)}
/* Een veld met zijn HUIDIGE waarde ernaast. Dat kleine getal is het hele punt
   van een instelformulier op een node die al ergens op staat: je ziet waarvan je
   afwijkt voordat je iets intypt. */
.cur{font-family:var(--mono);font-size:.62rem;color:var(--cyan);
text-transform:none;letter-spacing:0}
.cur.dev{color:var(--amber)}
select{width:100%;margin-top:.3rem;padding:.45rem .55rem;font-family:var(--mono);
font-size:.9rem;color:var(--text);background:var(--bg);
border:1px solid var(--border);border-radius:6px}
/* ------------------------- de radiowaarschuwing --------------------------- */
/* ROOD EN NIET AMBER, en met een eigen kader in plaats van een noot onderaan.
   freq, bw, sf en cr bepalen of deze node nog op hetzelfde mesh zit; één
   verkeerd getal en hij is er meteen af. Hij blijft dan over WiFi bereikbaar --
   daarom staat deze knop er ook -- maar over LoRa hoort niemand hem nog, en dat
   merk je pas als je op een waarschuwing zit te wachten die nooit komt. */
.rw{border:1px solid var(--red);border-left-width:3px;border-radius:0 8px 8px 0;
background:linear-gradient(90deg,rgba(255,92,92,.09),transparent 75%);
padding:.75rem .9rem;font-size:.87rem;margin:.2rem 0 .9rem}
.rw b{color:var(--red)}
.rw .baked{font-family:var(--mono);font-size:.8rem;color:var(--text)}
/* Twee sloten voor de radio: eerst het vinkje, dan pas de knop. Een confirm()
   alleen is te makkelijk weg te klikken voor de enige knop op deze pagina die
   de node van het mesh kan halen. */
.arm{display:flex;align-items:center;gap:.5rem;margin-top:.8rem;flex-wrap:wrap}
.arm button:disabled{opacity:.4;cursor:not-allowed}
/* Het wachtwoordalarm. Verdwijnt zodra het opgelost is -- een waarschuwing die
   blijft staan wordt genegeerd, en dan is de volgende ook niets meer waard. */
.alarm{border:1px solid var(--red);border-radius:10px;padding:.85rem 1rem;
background:linear-gradient(90deg,rgba(255,92,92,.1),transparent 75%);
margin-bottom:.8rem;font-size:.9rem}
.alarm b{color:var(--red);font-family:var(--mono);text-transform:uppercase;
letter-spacing:.1em;font-size:.7rem;display:block;margin-bottom:.25rem}
.alarm.ok{border-color:var(--accent);
background:linear-gradient(90deg,rgba(53,224,140,.08),transparent 75%)}
.alarm.ok b{color:var(--accent)}
/* -------------------- monitor bewerken in de tabelregel ------------------- */
/* De invoervelden staan IN de regel en niet in een formulier eronder, want het
   kanaalnummer links moet in beeld blijven: dat is het enige dat niet mag
   veranderen en het is waar de wijziging over gaat. */
td input{margin-top:0;padding:.2rem .35rem;font-size:.8rem}
td input.n1{width:7rem}td input.n2{width:11rem}td input.n3{width:4.5rem}
tr.edit{background:linear-gradient(90deg,rgba(76,201,240,.07),transparent 80%)}
td.acts{white-space:nowrap;text-align:right}
td.acts button{margin-left:.3rem}
td button.go{color:var(--accent)}
td button.go:hover{border-color:var(--accent)}
/* ---------------------- simuleren en testen ------------------------------- */
/* AMBER, EN OVERAL DEZELFDE AMBER. Een gesimuleerde sensor is noch goed noch
   stuk: hij zegt niets over de dienst, hij zegt dat wij iets beweren. Groen zou
   'in orde' suggereren en rood 'stuk'; beide zijn een leugen over een test.
   Dezelfde kleur als 'pauze' en 'stil', en dat is consequent: dat zijn ook de
   standen waarin de tabel niet de werkelijkheid van de dienst toont. */
.sb{border:1px solid var(--amber);border-radius:10px;padding:.85rem 1rem;
display:flex;gap:.9rem;align-items:center;flex-wrap:wrap;margin:.2rem 0 .9rem;
background:linear-gradient(90deg,rgba(255,180,84,.12),transparent 75%)}
.sb .t{flex:1 1 20rem;font-size:.9rem}
.sb .t b{font-family:var(--mono);text-transform:uppercase;letter-spacing:.1em;
font-size:.7rem;display:block;margin-bottom:.2rem;color:var(--amber)}
.sb button{margin:0;flex:0 0 auto}
/* De cel met de simulatiestand. De knoppen zijn klein en zonder kleur zolang er
   niets geforceerd staat -- ze horen niet om aandacht te vragen. Zodra er wél
   iets staat, is de cel amber en staat de resterende tijd erbij: dat getal is
   het antwoord op de enige vraag die dan telt, namelijk wanneer deze monitor
   weer de waarheid vertelt. */
td.sim{white-space:nowrap;font-family:var(--mono);font-size:.72rem}
td.sim button{margin:0 .15rem 0 0;font-size:.6rem;color:var(--muted)}
td.sim button:hover{border-color:var(--amber);color:var(--amber)}
td.sim .lft{color:var(--amber)}
tr.simrow{background:linear-gradient(90deg,rgba(255,180,84,.09),transparent 80%)}
tr.simrow td:first-child{box-shadow:inset 3px 0 0 var(--amber)}
/* De aflevering van het testbericht: verstuurd en aangekomen NAAST elkaar, want
   het verschil tussen die twee is de hele reden dat er op ACK's gelet wordt. Een
   getal onder het andere zou ze als twee losse feiten laten lezen. */
.deliv{display:flex;gap:1.4rem;flex-wrap:wrap;margin-top:.7rem}
.deliv div{font-family:var(--mono);font-size:.8rem}
.deliv span{display:block;font-size:.62rem;text-transform:uppercase;
letter-spacing:.13em;color:var(--muted)}
.deliv .part{color:var(--amber)}.deliv .none{color:var(--red)}
.deliv .all{color:var(--accent)}
/* DE BUDGETBALK. Eén beeld van 180 byte, met het vaste deel apart: dat is het
   deel waar je niets aan kunt doen, en dat hoort te zien te zijn voordat iemand
   zich afvraagt waarom er maar 156 byte voor hem is. Amber vanaf 15 byte over
   (minder dan twee monitors), rood bij vol -- dezelfde drempels als bij het
   kanaalbudget eronder, want het is dezelfde soort schaarste. */
.bar{display:flex;height:.85rem;border-radius:5px;overflow:hidden;
border:1px solid var(--border);background:var(--bg);margin-top:.2rem}
.bar i{display:block;height:100%}
.bar .b-fix{background:var(--muted)}
.bar .b-mon{background:var(--accent)}
.bar .b-lo{background:var(--amber)}
.bar .b-no{background:var(--red)}
/* Kanaalbudget: drie getallen in mono, want ze horen naast elkaar gelezen te
   worden en niet in een zin. */
.budget{display:flex;gap:1.4rem;flex-wrap:wrap;margin-top:.7rem}
.budget div{font-family:var(--mono);font-size:.8rem}
.budget span{display:block;font-size:.62rem;text-transform:uppercase;
letter-spacing:.13em;color:var(--muted)}
.budget .lo{color:var(--amber)}.budget .no{color:var(--red)}
</style></head><body>

<div class="top">
<h1>MeshUptime<span id="sub">bewaking &middot; heltec v3</span></h1>
<button id="thm" title="Wissel licht/donker thema">thema</button>
</div>

<nav class="tabs">
<button class="on" data-p="1">bewaking</button>
<button data-p="2">toegang</button>
<button data-p="3">node</button>
</nav>

<section id="p1">

<div id="simban"></div>
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
<div id="tmsg"></div>
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

<p class="why"><b>Waarom je een monitor BEWERKT en niet weggooit:</b> de
kanaalnummers (5&nbsp;t/m&nbsp;36) worden <b>nooit hergebruikt</b> zolang er nog
een nummer is dat nooit vergeven is. Wie een naam of een adres verkeerd typt en
de monitor daarom verwijdert, <i>verbrandt</i> een kanaal &mdash; het nummer
blijft vergeven en komt niet terug. Klik daarom op <b>bewerk</b>: naam, adres en
interval zijn ter plaatse te wijzigen en het <b>kanaal blijft hetzelfde</b>.<br>
<b>Maar let op met het ADRES.</b> Een naam wijzigen is onschuldig &mdash; die is
voor de mens en reist niet mee in de telemetrie. Een <b>adres</b> wijzigen geeft
hetzelfde kanaalnummer een andere <i>betekenis</i>: een dashboard dat
&quot;kanaal&nbsp;6 = google&quot; onthouden heeft, toont daarna de metingen van
een andere dienst onder de oude naam. Dat is precies de stille fout waarvoor die
onveranderlijke nummering bedoeld is. Verander je het adres, verander dan ook de
<b>naam</b> mee, en werk die naam ook bij in MeshManager &mdash; anders liegt de
grafiek daar zonder dat er iets stuk lijkt.</p>

<div class="card"><div class="budget" id="bud"></div>
<p class="note" id="budnote"></p></div>

<h2>Het bytebudget van de telemetrie</h2>
<p class="why"><b>Het aantal monitors is niet de grens &mdash; de bytes zijn de
grens.</b> De telemetrie van deze node gaat als CayenneLPP in één pakket van
<b>180 byte</b>, en daar moet alles in: de batterijspanning, de GPS als die
aanstaat, de drie vaste kanalen en dan de monitors. Een monitor kost <b>9
byte</b> als zijn pingtijd meegaat en <b>3 byte</b> als alleen zijn
op/neer-schakelaar meegaat. Er passen er dus zeventien of ruim vijftig,
afhankelijk van hoe je ze zet &mdash; en daarom staat hier een budget en niet een
maximum.<br>
<b>Waarom dit zichtbaar is:</b> wie zijn achttiende monitor toevoegt en daarna
merkt dat er willekeurig een paar kanalen uit zijn telemetrie verdwenen zijn,
heeft geen foutmelding gekregen maar wel verkeerde gegevens op zijn dashboard.
De node <b>weigert</b> daarom een monitor die er niet meer in past, en wat er
tegen alle verwachting toch buiten valt staat in de kolom <b>byte</b> in het
rood.<br>
<b>&quot;ms uit&quot; betekent niet dat er niet gemeten wordt.</b> De pingtijd
wordt gewoon gemeten en blijft hier, in <code>sensor list</code> en in de
DM-lijst te zien; hij gaat alleen niet meer over het mesh. Dat zijn twee heel
verschillende dingen &mdash; wie ze verwart, gaat een sensor repareren die
werkt.</p>

<div class="card"><div class="bar" id="tbar"></div>
<div class="budget" id="tbud"></div>
<p class="note" id="tbnote"></p></div>

<h2>Waarschuwingen simuleren en testen</h2>
<p class="why"><b>Waarom dit er is:</b> de waarschuwingen van deze node zijn
gebouwd maar zijn nog nooit afgegaan. Een node die pas bij een echte storing voor
het eerst een bericht stuurt, is een node waarvan niemand weet <i>of</i> dat
bericht aankomt &mdash; en dan blijkt dat op het slechtste moment. Hiermee is dat
vooraf te weten.<br>
<b>Het gaat door het echte pad.</b> Een forcering verandert wat de sensorlaag
<i>teruggeeft</i>; daarna doet dezelfde <code>alertIf()</code> zijn werk als bij
een echte storing: een echt DM-pakket, dezelfde keuze van ontvangers op het
<b>alarm</b>-recht, echte pogingen en echte ACK's. Er is met opzet géén tweede
verzendweg en géén nepbericht &mdash; dan zou deze pagina zichzelf testen in
plaats van het systeem. De forcering staat om dezelfde reden óók in de
<b>telemetrie</b>: een dashboard aan de andere kant hoort hetzelfde te zien.<br>
<b>Reken op maximaal een minuut.</b> Waarschuwingen worden alleen bij de
periodieke leesronde beoordeeld (elke 60&nbsp;s), en die ronde is precies het
pad dat we willen testen. Een knop die sneller was, was een andere weg.</p>

<div class="card">
<div class="row">
<label>Vervaltijd van een forcering (s)<input id="simsecs" type="number"
min="30" max="3600" value="600"><span class="cur">30 t/m 3600; een forcering
kan niet blijven staan</span></label>
<label>Rust voor een herstelmelding (s)<input id="rhold" type="number"
min="0" max="3600" value="120"><span class="cur" id="rholdcur">nu: –</span></label>
</div>
<div class="row" style="margin-top:.6rem">
<label class="cb"><input type="checkbox" id="recon"> ook melden als iets weer
<b>werkt</b></label>
</div>
<div class="quick">
<button id="rgo">herstelmelding opslaan</button>
<button id="tgo">testbericht sturen</button>
<button id="sclr">alles vrijgeven</button>
</div>
<div id="smsg"></div>
<div class="deliv" id="deliv"></div>
<p class="note"><b>Herstelmeldingen &mdash; waarom die er horen te zijn.</b>
<code>alertIf()</code> stuurt een bericht bij het BEGIN van een storing en doet
bij het einde alleen zijn Trigger opruimen. Zonder herstelmelding krijg je dus
&quot;router onbereikbaar&quot; en daarna nooit meer iets &mdash; en dan is
&quot;het is opgelost&quot; niet te onderscheiden van &quot;de node is zelf
gestopt met melden&quot;. Dat tweede is precies het geval dat je wil weten. De
melding zegt er de <b>duur</b> bij (&quot;weer bereikbaar na 4 min&quot;), want
dat is het enige wat zo'n bericht boven een geruststelling uittilt.<br>
<b>Drie remmen zitten erop.</b> Er gaat alleen een herstelmelding uit als er
werkelijk een <i>storingsmelding</i> de deur uit is geweest &mdash; een dienst
die even wegviel zonder dat iemand er iets van hoorde, levert geen &quot;weer
bereikbaar&quot; op. De dienst moet de <b>rust</b> hierboven aaneengesloten op
zijn: zonder die drempel stuurt een dienst die elke minuut op en neer gaat elke
minuut twee berichten, en dat is de ergste vorm van een alarmsysteem &mdash; het
leert mensen om meldingen te negeren. En een herstelmelding gaat altijd met
<b>lage</b> prioriteit (één poging per ontvanger in plaats van vier): een
gemiste &quot;het werkt weer&quot; is hinderlijk, een gemiste &quot;het is
stuk&quot; is erger.<br>
<b>Een gemelde dienst die uit &quot;stil&quot; terugkomt</b> krijgt een eigen
tekst: die was <i>onbekend</i> en niet neer, dus dan meldt de node dat de
<i>melder</i> weer meldt en niet dat een dienst hersteld is.<br>
<b>Elke forcering loopt van zichzelf af</b>, en dat is geen
gemak maar de belangrijkste regel hier. Een forcering die blijft staan zet die
monitor stil uit: hij meldt dan niet meer wat er echt gebeurt en niemand ziet
dat. Een node die na een test in testmodus blijft hangen is erger dan een node
zonder testknop. Bij het aflopen valt hij terug op de waarde die ondertussen
gewoon gemeten is &mdash; de meting wordt nooit overschreven, alleen
overstemd.<br>
<b>De rem, en waarom hij er is.</b> Elke simulatie kost echte zendtijd op een
band die je met anderen deelt. Daarom hoogstens <b>één testbericht per
minuut</b> en hoogstens <b>twee forceringen tegelijk</b>. Dat tweede getal is
niet willekeurig: de wachtrij voor waarschuwingen heeft vier plaatsen en de
batterijbewaking gebruikt er twee. Zouden er meer simulaties tegelijk mogen, dan
zou een <i>echte</i> batterijwaarschuwing stil overgeslagen worden &mdash; de
test mag de bewaking nooit verdringen.<br>
<b>Verstuurd is niet aangekomen.</b> Daarom staan hierboven twee getallen en
niet één. <i>Ontvangers</i> is naar hoeveel sleutels met het alarmrecht het
bericht gaat; <i>bevestigd</i> is hoeveel er een ACK terugstuurden. Zijn die
niet gelijk, dan is er iemand die je waarschuwingen niet krijgt &mdash; en dat
is precies wat je wil weten vóórdat er iets stuk is. De teller zegt
<i>minstens</i>: een ACK die pas aankomt nadat de node al naar de volgende
ontvanger gestuurd heeft, is niet meer te herkennen.<br>
<b>Niets hiervan overleeft een herstart.</b> De simulatiestand staat alleen in
RAM en met opzet niet in de opslag: een node die na een stroomstoring in
testmodus opstart, zwijgt over een echte storing.</p>
</div>

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

</section>
<section id="p2" hidden>

<h2>Toegang &mdash; wie mag de sensoren uitlezen</h2>
<div id="lock"></div>
<p class="why"><b>Waarom hier een slot zit:</b> een telemetrieverzoek over het
mesh werd tot nu toe door <i>iedereen</i> beantwoord. Het rechtenveld kwam wel
binnen maar werd niet gebruikt; het enige masker dat werkte was dat van de vrager
zelf, en dat zegt alleen welke kanalen hij wil <i>ontvangen</i>. Met het slot aan
antwoordt deze node alleen aan een sleutel die hieronder staat &mdash; de rest
krijgt <b>geen antwoord</b>, en niet een leeg antwoord: een weigering mag geen
zendtijd kosten, anders is zij zelf een manier om de radio bezig te houden.</p>

<div class="card pad0"><table id="al"></table></div>
<p class="note"><b>Rechten:</b>
<span class="c-on">lezen</span> = mag telemetrie opvragen &middot;
<span class="c-warn">alarm</span> = krijgt waarschuwingen toegestuurd &middot;
<span class="c-off">beheer</span> = mag ook instellingen wijzigen en
CLI-opdrachten sturen. Beheer omvat lezen. <b>Laatst actief</b> is vluchtig: na
een herstart staat elke ingang op <i>nooit</i> tot hij weer iets doet.<br>
Een node die met het beheerderswachtwoord inlogt, komt hier <b>zelf</b> in te
staan als beheerder &mdash; ook met het slot aan. Wie het slot dichtzet en het
wachtwoord op de standaardwaarde laat, heeft de deur op slot en de sleutel onder
de mat.</p>

<h2>Sleutel toevoegen</h2>
<div class="card"><form id="ka">
<label>Publieke sleutel (64 hextekens)<input name="key" maxlength="64"
minlength="64" pattern="[0-9a-fA-F]{64}" required
placeholder="a1b2c3&hellip;"></label>
<div class="row" style="margin-top:.6rem">
<label class="cb"><input type="checkbox" name="rd" checked> lezen</label>
<label class="cb"><input type="checkbox" name="alerts"> alarm</label>
<label class="cb"><input type="checkbox" name="admin"> beheer</label>
</div>
<button>Toevoegen</button>
<div id="kmsg"></div>
<p class="note">De <b>volledige</b> sleutel is nodig, geen prefix. De gedeelde
sleutel waarmee deze node met de tegenpartij praat wordt uit de hele publieke
sleutel gerekend &mdash; uit een prefix is hij niet te berekenen, dus een ingang
op een prefix zou een ingang zijn waarmee niet te praten is. En een prefix is te
vervalsen: wie sleutelparen blijft maken tot de eerste bytes overeenkomen, erft
het recht dat jij aan iemand anders gaf. Bij <b>wissen</b> mag een prefix wel
&mdash; dat is omkeerbaar, en de node weigert als de prefix op meer dan één
ingang past.</p>
</form></div>

<h2>In de buurt gehoord</h2>
<div class="card pad0"><table id="nb"></table></div>
<p class="note">Wat hier staat is <b>niet bewaard</b> en verdwijnt bij een
herstart: het is een bewering over het heden. Een bewaarde lijst zou beweren dat
een node in de buurt is die er al een week niet meer is.<br>
<b>SNR</b> en <b>hop</b> staan erbij om twee gelijknamige nodes te scheiden:
<code>0</code> hop is een node die deze node zelf hoort, hoger is via een omweg,
en de hoogste SNR is meestal die van jou. Alleen adverts met een geldige
<b>ondertekening</b> komen in deze lijst, dus de sleutel is bewijsbaar van de
afzender.</p>

</section>
<section id="p3" hidden>

<div id="pwal"></div>

<h2>CLI-console</h2>
<div class="card">
<div class="cmdrow"><input id="ci" spellcheck="false" autocomplete="off"
maxlength="160" placeholder="ver"><button id="cb">stuur</button></div>
<div class="quick">
<button data-c="ver">ver</button>
<button data-c="clock">clock</button>
<button data-c="clock sync">clock sync</button>
<button data-c="advert">advert</button>
<button data-c="advert.zerohop">advert.zerohop</button>
<button data-c="neighbors">neighbors</button>
<button data-c="sensor list">sensor list</button>
<button data-c="get radio">get radio</button>
<button class="dng" data-c="reboot">reboot</button>
<button class="dng" data-c="erase">erase</button>
</div>
<div id="out"></div>
<p class="note"><b>Dit is dezelfde CLI</b> als de seriële console en als die
waarmee een MeshCore-app een repeater beheert. Alles wat een app kan, kan hier
dus ook &mdash; de formulieren hieronder stellen niets anders samen dan zo'n
regel. Wat er niet in een formulier staat, typ je hier.<br>
<b>Opdrachten:</b> <code>advert</code> <code>advert.zerohop</code>
<code>clock</code> <code>clock sync</code> <code>time &lt;epoch&gt;</code>
<code>reboot</code> <code>region</code> <code>region def &lt;&hellip;&gt;</code>
<code>region home &lt;naam&gt;</code> <code>region default &lt;naam&gt;</code>
<code>neighbors</code> <code>ver</code> <code>board</code>
<code>password &lt;nieuw&gt;</code> <code>setperm &lt;sleutel&gt; &lt;n&gt;</code>
<code>erase</code> <code>log start</code> <code>log stop</code>
<code>log erase</code> <code>clear stats</code> <code>stats-core</code>
<code>stats-radio</code> <code>stats-packets</code>
<code>tempradio &lt;f,bw,sf,cr,min&gt;</code> <code>io</code>.<br>
<b>Lezen en zetten:</b> <code>get &lt;veld&gt;</code> /
<code>set &lt;veld&gt; &lt;waarde&gt;</code> op <code>radio</code>
(&quot;freq,bw,sf,cr&quot;) <code>freq</code> <code>tx</code> <code>af</code>
<code>dutycycle</code> <code>name</code> <code>lat</code> <code>lon</code>
<code>owner.info</code> <code>advert.interval</code>
<code>flood.advert.interval</code> <code>repeat</code> <code>flood.max</code>
<code>flood.max.unscoped</code> <code>flood.max.advert</code>
<code>loop.detect</code> <code>rxdelay</code> <code>txdelay</code>
<code>direct.txdelay</code> <code>multi.acks</code>
<code>path.hash.mode</code> <code>radio.rxgain</code>
<code>radio.fem.rxgain</code> <code>agc.reset.interval</code>
<code>int.thresh</code> <code>cad</code> <code>allow.read.only</code>
<code>adc.multiplier</code> <code>guest.password</code>
<code>acl.strict</code> <code>public.key</code> <code>role</code>.<br>
<b>Onze eigen instellingen</b> gaan via <code>sensor get &lt;sleutel&gt;</code> /
<code>sensor set &lt;sleutel&gt; &lt;waarde&gt;</code>:
<code>mains.hi</code> <code>mains.lo</code> <code>mains.state</code>
<code>mon.count</code> <code>mon.add</code> <code>mon.del</code> en per kanaal
<code>mon.&lt;kan&gt;.name</code> <code>mon.&lt;kan&gt;.host</code>
<code>mon.&lt;kan&gt;.int</code> <code>mon.&lt;kan&gt;.state</code>. En
<code>sensor list</code> voor de hele lijst.<br>
<b>Drie dingen weigert deze pagina</b>, met de reden in het antwoord:
alles met <code>prv.key</code> (de privésleutel over HTTP zonder TLS is de
identiteit van de node weggeven), <code>start ota</code> (dat opent een eigen
accesspoint en een TWEEDE webserver op poort&nbsp;80, dus precies deze pagina
valt eronder weg) en <code>poweroff</code>/<code>shutdown</code> (diepe slaap:
alleen een fysieke reset haalt hem daaruit, en dat kan niemand van hier). Die
horen aan de seriële kabel. <code>erase</code> en <code>set radio</code> mogen
wel, maar alleen mét de bevestiging die de knop en het formulier meesturen.<br>
<b>En drie antwoorden wél maar doen niets</b> in deze firmware &mdash; beter hier
te lezen dan zelf te ontdekken. <code>neighbors</code> geeft
<i>&quot;not supported&quot;</i>: SensorMesh vult die uitvoer niet in, de échte
buurtlijst staat op het tabblad <b>toegang</b>. <code>log&nbsp;&hellip;</code> en
<code>clear stats</code> hangen aan lege callbacks, dus ze melden &quot;logging
on&quot; en &quot;stats reset&quot; zonder dat er iets gebeurt. Dat is gedrag van
de sensorrol en niet van deze pagina.</p>
</div>

<h2>Instellingen</h2>
<p class="note">Elk formulier stuurt alleen de velden die je hebt
<b>veranderd</b>, elk als één CLI-regel, en het antwoord komt in de console
hierboven. De waarde die nu geldt staat naast het label; wijkt hij af van de
gebakken standaard, dan staat die er in amber achter.</p>

<details class="rf"><summary>Radio &mdash; freq / bw / sf / cr &nbsp;&middot;&nbsp; mesh-kritiek</summary><div>
<p class="rw"><b>Deze vier bepalen of deze node nog op hetzelfde mesh zit.</b>
Ze zijn geen instelling van de node maar een afspraak met alle andere nodes: één
verkeerd getal en niemand hoort hem nog, en hij hoort niemand. Hij blijft dan wel
over WiFi bereikbaar &mdash; daarom kun je het van hier terugzetten &mdash; maar
over LoRa is hij weg, en dat merk je pas als je op een waarschuwing wacht die
nooit komt. Een <b>herstart</b> is nodig voordat ze gaan gelden.<br>
Gebakken in deze firmware, dus wat de rest van dit mesh naar alle
waarschijnlijkheid gebruikt: <span class="baked" id="baked"></span><br>
Wil je iets uitproberen zonder je vast te leggen: <code>tempradio
&lt;freq,bw,sf,cr,minuten&gt;</code> in de console zet het TIJDELIJK en valt na
die minuten van zichzelf terug. Dat is de veilige manier om te kijken of je nog
gehoord wordt.</p>
<div class="row" id="g-rf"></div>
<div class="arm">
<label class="cb"><input type="checkbox" id="rfarm"> ja, ik verander de
mesh-afspraak van deze node</label>
<button id="rfgo" disabled>set radio</button>
</div>
<p class="note">In het zusterproject <b>MeshManager</b> geldt de regel dat
radio-instellingen NIET van afstand over het mesh gewijzigd mogen worden, alleen
<code>tx</code>. Die regel gaat over een verzoek van een server naar een verre
node: daar kun je het gevolg niet zien en niet terugdraaien. Deze pagina is de
<b>lokale</b> weg naar de node zelf, en daar hoort het te kunnen &mdash; maar de
gevolgen zijn dezelfde, en daarom staat de waarschuwing er wel.</p>
</div></details>

<details><summary>Zendvermogen en ontvangst</summary><div>
<p class="note"><code>tx</code> staat met opzet <b>niet</b> bij freq/bw/sf/cr:
zendvermogen verandert hoe VER je gehoord wordt, niet OF je gehoord wordt. Te
laag is jammer, te hoog is onnodig airtime &mdash; maar je blijft op hetzelfde
mesh. Hij werkt bovendien meteen, zonder herstart.</p>
<div class="row" id="g-pwr"></div>
<button data-g="pwr">Opslaan</button>
</div></details>

<details><summary>Aankondigen</summary><div>
<div class="row" id="g-adv"></div>
<button data-g="adv">Opslaan</button>
<p class="note">Het <b>advert-interval</b> is in minuten (0 = uit, anders
minimaal 2 en hoogstens 240); het <b>flood-advert-interval</b> in uren (0 = uit,
anders 3 t/m 168). Een flood-advert gaat over het hele mesh en kost bij iedereen
airtime, dus daar is zuinig zijn geen zuinigheid maar hoffelijkheid.<br>
<b>Locatie in advert</b>: <code>none</code> stuurt niets mee,
<code>prefs</code> de breedte/lengte hierboven, <code>share</code> de gemeten
GPS-positie. Deze node heeft geen GPS, dus <code>prefs</code> is hier de zinnige
stand. In de <b>eigenaarsregel</b> wordt een <code>|</code> een regeleinde.</p>
</div></details>

<details><summary>Doorsturen en filteren</summary><div>
<div class="row" id="g-fwd"></div>
<button data-g="fwd">Opslaan</button>
<p class="note">Deze node is een <b>sensor</b> en geen repeater:
<code>repeat</code> staat daarom standaard <b>uit</b>. Aanzetten maakt hem tot
doorgeefluik en dat kost batterij en airtime &mdash; op een node die aan het net
hangt en goed staat kan het het mesh helpen, op een node op batterij niet.<br>
De <code>flood.max</code>-getallen zijn hop-grenzen: een pakket dat meer hops
achter zich heeft wordt niet meer doorgestuurd. De vertragingen zijn er om te
voorkomen dat drie nodes die hetzelfde horen ook alle drie tegelijk beginnen te
zenden.</p>
</div></details>

<details><summary>Padhash</summary><div>
<p class="note">Dit getal bepaalt de <b>padhash</b> die deze node in een pakket
zet: <code>getPathHashSize() = hash_mode + 1</code>, dus <b>0</b> geeft 1 byte
per hop, <b>1</b> geeft 2 en <b>2</b> geeft 3.<br>
<b>De ruil.</b> Een padhash is een verkorte vingerafdruk van een node in het pad.
Meer byte per hop maakt de kans kleiner dat twee nodes dezelfde hash hebben en
een pakket dus de verkeerde kant op gestuurd wordt &mdash; maar elk pakket wordt
er per hop een byte langer van, en dat is meer zendtijd voor iedereen op het
mesh. Op een klein mesh is 1 byte ruim; op een groot mesh met veel nodes betaalt
2 of 3 zich terug. Kies dit dus niet uit voorzichtigheid, maar naar de grootte
van het mesh waarop je zit.<br>
<b>Deze node stuurt zijn DM-antwoorden met dezelfde maat</b>: main.cpp geeft
<code>path_hash_mode + 1</code> door aan DmCommands bij het opstarten. Een
wijziging hier geldt dus pas voor de DM-kant na een <b>herstart</b>.</p>
<div class="row" id="g-hash"></div>
<button data-g="hash">Opslaan</button>
</div></details>

<details><summary>Overig</summary><div>
<div class="row" id="g-misc"></div>
<button data-g="misc">Opslaan</button>
<p class="note"><b>cad</b> laat de radio eerst luisteren of het kanaal vrij is
voordat hij zendt; dat kost een fractie voor elke zending maar voorkomt dat twee
nodes elkaar overschreeuwen. <b>Interferentiedrempel</b> 0 is uit.
<b>Gastlezen</b> hoort bij het gastwachtwoord: staat het uit, dan doet dat
wachtwoord niets. De <b>adc-vermenigvuldiger</b> ijkt de spanningsmeting &mdash;
en daar hangt op deze node de netvoeding/batterij-beslissing aan, dus verander
hem niet zonder een meting ernaast. De drempels zelf zijn
<code>mains.hi</code>/<code>mains.lo</code> via <code>sensor set</code>.<br>
Wat hier niet staat, staat wel in de console: <code>dutycycle</code>,
<code>powersaving</code>, <code>gps&nbsp;&hellip;</code>,
<code>extra.sf</code>, <code>tempradio</code>, <code>neighbor.remove</code>,
<code>setperm</code>, <code>time</code>, <code>io</code>. Die zijn met opzet geen
formulier: ze zijn zeldzaam, of ze bestaan alleen op andere borden, en een
formulier dat &quot;Unknown command&quot; oplevert is erger dan geen
formulier.</p>
</div></details>

<details><summary>Regio</summary><div>
<p class="note">Regio's zijn de <b>scopes</b> waarin een flood mag rondgaan. Ze
zitten niet in NodePrefs maar in een eigen tabel, en die is alleen via de CLI te
lezen &mdash; vandaar knoppen en een vrij veld in plaats van vooringevulde
velden.<br>
<code>region</code> toont de boom, <code>region def &lt;&hellip;&gt;</code>
herschrijft hem in één keer (namen gescheiden door spaties, een
<code>|</code> of <code>,</code> achter een naam springt terug naar die tak),
<code>region home &lt;naam&gt;</code> zet waar deze node staat en
<code>region default &lt;naam&gt;</code> de scope die hij op uitgaande pakketten
zet. <code>region save</code> legt het vast op flash &mdash; <b>zonder die
opdracht is de wijziging weg na een herstart</b>.</p>
<div class="quick">
<button data-c="region">region</button>
<button data-c="region home">region home</button>
<button data-c="region default">region default</button>
<button data-c="region list allowed">region list allowed</button>
<button data-c="region list denied">region list denied</button>
<button data-c="region save">region save</button>
</div>
<div class="cmdrow" style="margin-top:.8rem">
<input id="rgi" spellcheck="false" autocomplete="off" maxlength="140"
placeholder="region def eu be|eu nl"><button id="rgb">stuur</button></div>
</div></details>

<details><summary>Wachtwoorden</summary><div>
<p class="note">Het <b>beheerderswachtwoord</b> is de tweede deur naar deze node,
naast de toegangslijst: wie het over het mesh meestuurt bij een login wordt
automatisch in de lijst gezet <b>als beheerder</b> &mdash; ook met het slot aan
&mdash; en heeft daarmee deze hele CLI. Het slot en dit wachtwoord zijn dus samen
één beveiliging en niet twee.<br>
Het <b>gastwachtwoord</b> geeft alleen leesrecht, en alleen als
<code>allow.read.only</code> aanstaat.<br>
Hoogstens 15 tekens (NodePrefs bewaart 16 byte). Het antwoord van de node kaatst
het nieuwe wachtwoord terug, zodat je zeker weet wat er staat &mdash; dat staat
dan ook in de console hierboven, dus wis die als iemand meekijkt.</p>
<div class="row">
<label>Beheerderswachtwoord<input id="pw1" maxlength="15" type="password"
autocomplete="new-password" placeholder="nieuw"></label>
<label>Gastwachtwoord<input id="pw2" maxlength="15" type="password"
autocomplete="new-password" placeholder="nieuw"></label>
</div>
<button id="pwgo">Opslaan</button>
</div></details>

<details><summary>WiFi</summary><div>
<form method="post" action="/wifi">
<div class="row">
<label>Netwerk (SSID)<input name="ssid" id="ssid" maxlength="32" required></label>
<label>Wachtwoord<input name="pwd" type="password" maxlength="64"></label>
</div>
<button>Opslaan</button>
<p class="note">Opgeslagen instellingen gaan voor op de ingebouwde. Pas actief na
herstart.</p>
</form>
</div></details>

<details><summary>De andere twee wegen naar deze node</summary><div>
<p class="note"><b>1. De seriële console.</b> USB erin, 115200 baud, en je hebt
letterlijk dezelfde opdrachten als hierboven &mdash; deze pagina stuurt ze met
<code>sender_timestamp = 0</code> en dat betekent voor CommonCLI precies &quot;de
console&quot;. Dit is de weg die altijd werkt, ook als de wifi weg is en ook als
je jezelf met een radio-instelling van het mesh hebt gehaald.<br>
<b>2. Een MeshCore-app over LoRa.</b> Deze node is <i>geen</i> companion &mdash;
er zit geen BLE op en deze firmware spreekt het companion-clientprotocol niet.
Maar een app kan hem beheren zoals zij een <b>repeater</b> beheert: voeg hem toe
als contact, log in met het beheerderswachtwoord, en je krijgt dezelfde CLI over
het mesh. Handig om te weten, want dat werkt als de wifi weg is.<br>
Over die weg zijn een paar opdrachten met opzet <b>niet</b> beschikbaar
(<code>set freq</code>, <code>erase</code>, <code>get acl</code>,
<code>log</code>, <code>stats-*</code>): CommonCLI laat die alleen door met
<code>sender_timestamp = 0</code>, dus alleen van de console. Deze pagina zit aan
die kant van de streep, een app op afstand niet.<br>
Deze node toevoegen als contact gaat met de publieke sleutel hieronder; hij komt
ook in elk advert langs, dus als de app hem al gehoord heeft staat hij er al
tussen.</p>
<div class="row"><label>Naam<input id="idname" readonly></label></div>
<label>Publieke sleutel<input id="idkey" readonly></label>
<p class="note">De <b>rol</b> die deze node in zijn advert zet is
<code>sensor</code>. Een app die op &quot;repeater&quot; filtert ziet hem dus
niet in dat lijstje staan; zoeken op naam werkt wel.</p>
</div></details>

</section>

<script>
/* ============================ het thema ==================================
 *
 * Drie standen, één knop die er rondloopt: systeem -> licht -> donker -> systeem.
 * Dezelfde drie als in de repeater-webinterface en in MeshManager.
 *
 * IN localStorage EN NIET OP DE NODE, en dat is een ontwerpkeuze: dit is een
 * voorkeur van de BROWSER die ernaar kijkt en niet van het apparaat. Twee mensen
 * die dezelfde node openen mogen een andere keuze hebben, en een themaknop hoort
 * geen flashschrijving op een bewakingsnode te kosten.
 *
 * De stand zelf wordt al in de KOP gezet, vóór het eerste renderen; hier staat
 * alleen het omzetten en het label. Zie de opmerking bij dat stukje script. */
var THEMES=["system","light","dark"];
var THNAME={system:"systeem",light:"licht",dark:"donker"};

function thGet(){try{var t=localStorage.getItem("mu-theme");
return(t=="light"||t=="dark")?t:"system"}catch(e){return"system"}}

function thApply(t){
var el=document.documentElement;
/* De SYSTEEMSTAND is het ONTBREKEN van het attribuut en niet een derde waarde.
   Zo is er precies één manier waarop de media-query weer aan het woord komt, en
   kan er geen stand bestaan die noch systeem noch expliciet is. */
if(t=="system"){el.removeAttribute("data-theme")}
else{el.setAttribute("data-theme",t)}
try{if(t=="system"){localStorage.removeItem("mu-theme")}
else{localStorage.setItem("mu-theme",t)}}catch(e){}
var b=document.getElementById("thm");
b.textContent=THNAME[t];
b.title="Thema: "+THNAME[t]+(t=="system"?
" (volgt de instelling van je toestel)":" (afgedwongen)")+
" — klik om te wisselen"}

document.getElementById("thm").onclick=function(){
var i=THEMES.indexOf(thGet());
thApply(THEMES[(i+1)%THEMES.length])}

thApply(thGet());

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
["toestand",""],["ms","num"],["mislukt","num"],["byte","num"],["simulatie",""],
["",""]];

/* De kolommen waarvan de PLAATS ergens anders gebruikt wordt. Als getal en niet
   als aanname: editRow() schrijft in de knoppencel, en elke keer dat er een kolom
   tussen komt schuift die op. Een hard geschreven 7 op vier plaatsen is precies
   hoe zo'n uitbreiding stil de verkeerde cel leegmaakt -- dat scheelde bij de
   kolom 'simulatie' weinig en bij 'byte' opnieuw. */
var CBYTE=7, CSIM=8, CACTS=9;

/* HET KANAAL DAT NU BEWERKT WORDT, of 0. De tabel wordt elke 5 s opnieuw
   opgebouwd omdat de metingen veranderen; dat mag niet gebeuren terwijl iemand in
   een invoerveld staat te typen -- dan is zijn tekst weg zonder dat hij iets fout
   deed. Zolang dit getal niet 0 is, laat table() de tabel staan. De TEGELS
   verversen wel door, want die zeggen niets over deze regel. */
var EDIT=0;

/* De keuring, LETTERLIJK dezelfde als MonitorSensors::validName/validHost: alleen
   letters, cijfers, punt, streepje en liggend streepje, 1..16 voor een naam en
   1..40 voor een adres. Hij staat hier niet om de node te vervangen -- die keurt
   zelf, en dat blijft de waarheid -- maar om een fout in de browser te melden in
   plaats van er een mislukte opdracht voor over het netwerk te sturen. */
var NM=/^[A-Za-z0-9._-]{1,16}$/, HS=/^[A-Za-z0-9._-]{1,40}$/;

/* DE BUFFERGRENS VAN 'sensor set'. Upstream doet daar strcpy(tmp,&command[11])
   in een buffer van PRV_KEY_SIZE*2+4 = 68 byte, dus alles achter "sensor set "
   moet onder de 68 tekens blijven. Wij houden 59 aan -- de marge die de node zelf
   ook aanhoudt -- en weigeren hier wat er niet in past in plaats van het de node
   in te sturen. Reken maar na: "mon.12.host " is 12 tekens en een adres mag 40,
   dus 52; het past, maar niet met veel over. */
function fits(s){return s.length<=59}

/* ---- WAAR EEN MELDING OVER DE KANAALTABEL TERECHTKOMT ----
 *
 * Er zijn twee plaatsen, en het onderscheid is niet cosmetisch:
 *
 *  rowsay()  -- "ik weiger dit te versturen". Een eigen regel DIRECT onder de
 *               rij die bewerkt wordt, dus precies waar de klik was. De
 *               bewerkmodus blijft staan, dus deze regel leeft even lang als
 *               het probleem. Dit is de reparatie van de gemelde fout: die
 *               meldingen stonden in #msg, onderaan het formulier 'Monitor
 *               toevoegen', ver buiten beeld.
 *  tsay()    -- "dit heeft de node geantwoord". Onder de tabel, want op dat
 *               moment is de bewerkmodus voorbij en wordt de tabel herbouwd --
 *               een melding IN de tabel zou door die herbouw gewist worden
 *               precies op het moment dat hij iets te zeggen heeft.
 */
function rowsay(r,txt,ok){
var nx=r.nextElementSibling;
if(!nx||nx.className!="emsg"){
var tr=document.createElement("tr");tr.className="emsg";
tr.insertCell().colSpan=KH.length;
r.parentNode.insertBefore(tr,nx);nx=tr}
var td=nx.cells[0];
td.className=ok?"ok":"bad";
td.textContent=txt;
return nx}

function rowsayClear(r){
var nx=r.nextElementSibling;
if(nx&&nx.className=="emsg"){nx.parentNode.removeChild(nx)}}

function tsay(txt,ok){var m=document.getElementById("tmsg");
m.className=ok?"ok":"bad";m.textContent=txt}

/* De knoppencel van één rij. EEN functie, want twee plekken bouwen hem: de
   herbouw van de tabel en het ANNULEREN van een bewerking -- en die tweede moet
   de rij meteen terugzetten en niet wachten op het volgende status.json. */
/* ---- DE BYTECEL ----
 *
 * Wat deze monitor in het telemetriepakket kost, en de knop om dat van 9 naar 3
 * te brengen. Die knop staat IN de tabel en niet in een instellingenformulier,
 * want de vraag "welke van mijn monitors kan er zonder pingtijd" is een vraag die
 * je rij voor rij beantwoordt, met de bytes en de gemeten tijd ernaast.
 *
 * VIEL HIJ BUITEN HET PAKKET, dan is dat het enige dat deze cel nog zegt, in
 * rood. Dat is de fout die dit hele budget moet voorkomen: een monitor die stil
 * uit de telemetrie verdwijnt, staat er wel op de pagina en niet op het
 * dashboard, en dan zoekt iemand een uur in de verkeerde hoek.
 */
function byteCell(c,m){
c.textContent="";c.className="num";
if(m.drop){
c.className="num c-off";
c.textContent=m.tb+"b !";
c.title="Deze monitor PASTE NIET in het laatste telemetriepakket en staat dus "+
"niet in de gegevens die over het mesh gaan. Zet bij deze of bij een andere "+
"monitor de pingtijd uit om ruimte te maken.";
return}
if(m.tms<0){c.textContent=m.tb+"b";c.className="num";return}
var s=document.createElement("span");
s.textContent=m.tb+"b ";
c.appendChild(s);
var b=document.createElement("button");
b.textContent=m.tms?"ms aan":"ms uit";
if(!m.tms){b.className="go"}
b.title=m.tms?
"De pingtijd gaat nu mee over het mesh (9 byte). Uitzetten maakt er 3 byte van; "+
"de tijd wordt dan nog steeds GEMETEN en blijft hier zichtbaar, hij gaat alleen "+
"de ether niet meer in.":
"Alleen de schakelaar gaat mee (3 byte). Aanzetten stuurt ook de pingtijd mee "+
"(9 byte) -- als het budget dat toelaat.";
b.onclick=function(){
/* De volle 'sensor set' over de CLI, net als de andere monitorvelden: daar zit
   de keuring en daar zit het wegschrijven naar flash. */
tsay("bezig...",1);
cliSeq(["sensor set mon."+m.ch+".ms "+(m.tms?0:1)],function(good,bad,last){
if(bad){tsay("geweigerd: "+last.trim()+" (past de pingtijd nog in het budget?)",0)}
else{tsay("kanaal "+m.ch+": pingtijd "+(m.tms?"gaat niet meer":"gaat weer")+
" mee over het mesh",1)}
u2()})};
c.appendChild(b)}

/* ---- DE SIMULATIECEL ----
 *
 * Twee standen, en met opzet niet drie knoppen naast elkaar:
 *
 *  niets geforceerd -> twee kleine, kleurloze knopjes 'op' en 'neer'. Ze horen
 *                      geen aandacht te vragen; dit is niet de knop waarvoor dit
 *                      apparaat er staat.
 *  wel geforceerd   -> de stand in amber, de RESTERENDE TIJD erbij, en één knop
 *                      'vrij'. Die tijd is dan het antwoord op de enige vraag die
 *                      telt: wanneer vertelt deze monitor weer de waarheid?
 *
 * Kanaal 1 heeft si == -1: dat kanaal is van SensorMesh zelf en niet van ons, dus
 * daar valt niets te forceren. Een streepje en geen knop, want een knop die niets
 * doet is erger dan geen knop.
 */
function simCell(c,m){
c.textContent="";c.className="sim";
if(m.si===undefined||m.si<0){c.textContent="–";return}
function b(txt,mode,ttl){var x=document.createElement("button");
x.textContent=txt;x.title=ttl;
x.onclick=function(){postsim(m.si,mode)};c.appendChild(x)}
if(m.sm=="off"){
b("op","up","forceer de goede stand -- hiermee test je dat een lopende "+
"waarschuwing OPGERUIMD wordt");
b("neer","down","forceer de slechte stand -- hiermee gaat er een echte "+
"waarschuwing over het mesh, gemarkeerd als test");
return}
var s=document.createElement("span");s.className="lft";
s.textContent=(m.sm=="up"?"op":"neer")+" · "+(m.sl?m.sl+"s":"–")+" ";
c.appendChild(s);
b("vrij","off","hef de forcering nu op en val terug op de meting")}

function actsCell(c,r,m){
c.textContent="";c.className="acts";
if(m.k=="vast"){return}
var eb=document.createElement("button");eb.textContent="bewerk";
eb.className="go";eb.onclick=function(){editRow(r,m)};c.appendChild(eb);
var b=document.createElement("button");b.textContent="wis";
b.onclick=function(){if(confirm("Monitor '"+m.n+"' verwijderen?\n\nKanaal "+m.ch+
" wordt daarna NIET opnieuw uitgedeeld zolang er nog een nieuw nummer vrij is."+
"\n\nWou je alleen een typefout herstellen? Gebruik dan 'bewerk' -- dan houdt "+
"deze dienst hetzelfde kanaal en verbrand je er geen."))
{post("monitor/del","name="+encodeURIComponent(m.n))}};c.appendChild(b)}

function table(d){
if(EDIT){return}
var e=document.getElementById("k");e.innerHTML="";
var hr=e.insertRow();KH.forEach(function(x){var h=document.createElement("th");
h.textContent=x[0];h.className=x[1];hr.appendChild(h)});
if(d.monwarn){var r=e.insertRow(),c=r.insertCell();c.colSpan=KH.length;
c.className="bad";c.textContent=d.monwarn;return}
(d.mon||[]).forEach(function(m){
var r=e.insertRow();
/* De simulatietint gaat VOOR op de 'fix'-stijl: bij een geforceerde regel is het
   feit dat er iets geforceerd staat belangrijker dan dat het een vast kanaal is. */
r.className=m.sm&&m.sm!="off"?"simrow":(m.k=="vast"?"fix":"");
var c=r.insertCell();c.className="num";c.textContent=m.ch;
c=r.insertCell();c.className="nm";c.textContent=m.n;
c=r.insertCell();c.textContent=m.h;
c=r.insertCell();c.className="num";c.textContent=m.i?m.i+" s":"–";
c=r.insertCell();c.className="st c-"+cls(m.sev);
c.innerHTML=dot(m.sev)+m.st+(m.k=="gemeld"&&m.age?" "+m.age+"s":"");
c=r.insertCell();c.className="num";c.textContent=m.ms?m.ms:"–";
c=r.insertCell();c.className="num";
c.textContent=m.k=="vast"?"–":m.f+"/"+m.c;
byteCell(r.insertCell(),m);
simCell(r.insertCell(),m);
actsCell(r.insertCell(),r,m)})}

/* Een regel uit de kanaalkaart ter plaatse bewerkbaar maken.
 *
 * WAAROM IN DE REGEL EN NIET IN EEN FORMULIER ERONDER: het kanaalnummer staat
 * links en moet in beeld blijven, want dat is het enige dat NIET verandert en het
 * is waar de hele wijziging over gaat. Een formulier onderaan de pagina zou dat
 * nummer uit het zicht halen op het moment dat het telt.
 *
 * DE WEG NAAR DE NODE IS DE CLI, en dat is geen omweg maar de bedoeling.
 * 'sensor set mon.<kanaal>.name' loopt door MonitorSensors::setSettingValue en
 * dus door validName/validHost en de intervalgrenzen -- dezelfde zeef als de
 * seriële console en dezelfde als /monitor. Een tweede schrijfpad hierheen zou een
 * tweede keuring zijn, en twee keuringen lopen ooit uiteen.
 *
 * GEMETEN FOUT, HIER GEREPAREERD -- twee dingen, want ze zagen er hetzelfde uit:
 *
 *  1. OPSLAAN leek niets te doen bij een afgekeurde invoer. Hij deed wel wat: de
 *     naam "UDM Pro" bevat een spatie, de zeef weigerde hem, en de reden werd in
 *     #msg gezet -- het meldvakje van het formulier 'Monitor toevoegen', bij een
 *     opengeklapte pagina zo'n 700 pixels onder de tabel. Wie op een knop klikt
 *     en geen antwoord ziet, heeft een knop die niets doet. Alle meldingen van
 *     deze knop gaan nu naar rowsay(), een regel direct onder de rij zelf.
 *  2. ANNULEER zette de rij pas terug NA het volgende status.json. Dat is een
 *     netwerkronde op een node die tussendoor een radio bedient, en mislukt die
 *     ronde dan blijft de rij in bewerkmodus staan zonder dat er iets gebeurt.
 *     Annuleren is een plaatselijke handeling en hoort geen netwerk nodig te
 *     hebben: hij zet de rij nu meteen terug uit de gegevens die er al zijn.
 *
 * De knoppen zaten dus WEL aan een handler; dat is nagemeten in een browser
 * (typeof onclick == "function", elementFromPoint gaf de knop zelf, EDIT werd 5).
 * Een gedelegeerde handler op de tabel zou dit niet gerepareerd hebben, en de
 * verversing van 5 s bleek de rij ook niet te overschrijven -- table() stapt er
 * uit zolang EDIT staat. Daarom is die opzet gebleven.
 */
function editRow(r,m){
EDIT=m.ch;
var cells=r.cells;
r.className="edit";
rowsayClear(r);
function inp(cell,val,max,cl){cell.textContent="";
var i=document.createElement("input");i.value=val;i.maxLength=max;
i.className=cl;i.spellcheck=false;cell.appendChild(i);return i}
var iN=inp(cells[1],m.n,16,"n1");
var iH=inp(cells[2],m.k=="gemeld"?"-":m.h,40,"n2");
var iI=inp(cells[3],""+m.i,4,"n3");

cells[CACTS].textContent="";
var ok=document.createElement("button");ok.textContent="opslaan";ok.className="go";
var no=document.createElement("button");no.textContent="annuleer";
cells[CACTS].appendChild(ok);cells[CACTS].appendChild(no);

/* METEEN terugzetten, zonder netwerk. De drie velden komen uit de momentopname
   waarmee deze rij gebouwd is; de eerstvolgende verversing zet er daarna de
   verse meting weer in. */
no.onclick=function(){
rowsayClear(r);
cells[1].textContent=m.n;
cells[2].textContent=m.h;
cells[3].textContent=m.i?m.i+" s":"–";
r.className=m.k=="vast"?"fix":"";
actsCell(cells[CACTS],r,m);
EDIT=0;
u2()};

ok.onclick=function(){
var nn=iN.value.trim(),nh=iH.value.trim(),ni=iI.value.trim();
/* De spatie apart benoemd, want dat is de fout die in de praktijk gemaakt wordt:
   "UDM Pro" is een naam die een mens logisch vindt en die de node weigert. Een
   zeef die alleen zijn eigen regel opdreunt laat de gebruiker zelf zoeken welk
   teken hij bedoelt. */
if(!NM.test(nn)){rowsay(r,/\s/.test(nn)?
"naam: een SPATIE mag niet -- gebruik bijvoorbeeld "+nn.replace(/\s+/g,"-")+
" of "+nn.replace(/\s+/g,"").toLowerCase()+
" (toegestaan: 1-16 tekens uit letters, cijfers, . - _)":
"naam: 1-16 tekens uit letters, cijfers, . - _",0);return}
if(!HS.test(nh)){rowsay(r,/\s/.test(nh)?
"adres: een SPATIE mag niet in een adres":
"adres: 1-40 tekens uit letters, cijfers, . - _ (of '-' "+
"voor een gemelde dienst)",0);return}
var iv=parseInt(ni,10);
if(!(iv>=10&&iv<=3600)){rowsay(r,"interval: 10 t/m 3600 s",0);return}

/* Het ADRES is de gevaarlijke: de naam is voor de mens, het adres bepaalt wat
   het kanaal BETEKENT. Vandaar een eigen bevestiging, en alleen voor dit veld. */
var oh=m.k=="gemeld"?"-":m.h;
if(nh!=oh&&!confirm("Adres van kanaal "+m.ch+" wijzigen?\n\n"+oh+"  ->  "+nh+
"\n\nHetzelfde kanaalnummer gaat daarmee over een ANDERE dienst. Een dashboard "+
"dat 'kanaal "+m.ch+" = "+m.n+"' bewaard heeft, toont vanaf nu de metingen van "+
nh+" onder die oude naam.\n\nVerander dan ook de naam mee, en werk die naam bij "+
"in MeshManager."+
(oh=="-"||nh=="-"?"\n\nLET OP: hiermee verandert ook de SOORT -- een adres '-' "+
"is een van buiten gemelde dienst, een echt adres is een ping-monitor.":"")))
{return}

/* Alleen wat veranderd is, en elk veld als EEN regel. Niet alle drie altijd
   sturen: elke gelukte 'sensor set' zet _dirty en dus een flashschrijving in de
   wacht, en een adreswijziging gooit bovendien de gemeten toestand van dat vakje
   weg. Wat niet veranderd is, hoort niets te veroorzaken. */
var cmds=[];
if(nn!=m.n)  {cmds.push("sensor set mon."+m.ch+".name "+nn)}
if(nh!=oh)   {cmds.push("sensor set mon."+m.ch+".host "+nh)}
if(iv!=m.i)  {cmds.push("sensor set mon."+m.ch+".int "+iv)}
if(!cmds.length){tsay("niets veranderd aan kanaal "+m.ch,1);
no.onclick();return}

for(var i=0;i<cmds.length;i++){
if(!fits(cmds[i].slice(11))){
rowsay(r,"te lang voor de opdrachtbuffer van de node (max 59 tekens achter "+
"'sensor set'); kort de naam of het adres in",0);return}}

/* De knoppen op slot zolang de reeks loopt. Twee keer klikken zou twee reeksen
   'sensor set' sturen, en elke gelukte set zet een flashschrijving in de wacht. */
ok.disabled=true;no.disabled=true;
rowsay(r,"bezig met "+cmds.length+" wijziging(en)...",1);

/* De naam MOET als eerste als hij verandert: de volgende opdrachten zoeken hun
   vakje op KANAAL en niet op naam, dus de volgorde maakt hier eigenlijk niets uit
   -- maar bij een mislukking is het prettiger dat de naam al klopt dan dat er een
   nieuw adres onder een oude naam staat. */
cliSeq(cmds,function(good,bad,last){
/* De uitslag gaat naar tsay() en niet naar rowsay(): EDIT gaat hier op 0 en dan
   herbouwt u2() de hele tabel, inclusief de meldregel. Onder de tabel blijft de
   uitslag staan -- en juist die wil je lezen. */
EDIT=0;rowsayClear(r);u2();cfg();
/* De node antwoordt op een geweigerde 'sensor set' met "can't find custom var"
   -- dat is zijn enige foutmelding en hij zegt dus niet WAAROM. Vandaar dat het
   antwoord hier letterlijk doorgegeven wordt en er een hint bij staat: bijna
   altijd is het een naam die al bestaat of een waarde buiten de grenzen. */
if(bad){tsay(bad+" van de "+(good+bad)+" wijzigingen geweigerd: "+last.trim()+
" (naam al in gebruik? interval buiten 10-3600?)",0)}
else{tsay(good+" wijziging(en) doorgevoerd; kanaal "+m.ch+" is niet veranderd",1)}
})}}

function say(t,ok){var m=document.getElementById("msg");
m.className=ok?"ok":"bad";m.textContent=t}
function say2(t,ok){var m=document.getElementById("kmsg");
m.className=ok?"ok":"bad";m.textContent=t}

function post(u,b){return fetch(u,{method:"POST",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b})
.then(function(r){return r.text().then(function(t){
say(t.trim(),r.ok);if(r.ok){u2()}})})}

/* ---- simuleren en testen ----
 *
 * EIGEN MELDREGEL (#smsg), en dat is de les uit de gerepareerde bewerkfout: een
 * antwoord hoort te staan bij de knop waarop geklikt is. Niet bij een ander
 * formulier, en niet zeven honderd pixels lager.
 *
 * De ANTWOORDTEKST VAN DE NODE gaat letterlijk door. Die teksten leggen uit
 * waarom iets geweigerd is -- "er staan al 2 forceringen", "te snel achter
 * elkaar", "geen enkele ingang heeft het alarmrecht" -- en daarover is de node de
 * waarheid. Een eigen vertaling hier zou een tweede lijst redenen zijn, en twee
 * lijsten lopen uiteen.
 */
function ssay(t,ok){var m=document.getElementById("smsg");
m.className=ok?"ok":"bad";m.textContent=t}

function secsNow(){var v=parseInt(document.getElementById("simsecs").value,10);
return(v>=30&&v<=3600)?v:600}

function psim(u,b){return fetch(u,{method:"POST",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b})
.then(function(r){return r.text().then(function(t){ssay(t.trim(),r.ok);u2()})})
.catch(function(){ssay("geen verbinding met de node",0)})}

function postsim(i,mode){
return psim("sim","i="+i+"&m="+mode+"&secs="+secsNow())}

/* DE BANNER. Amber en niet stil, om precies dezelfde reden als bij het slot van
   de toegangslijst: een node die iets anders meldt dan hij meet mag zich niet
   voordoen als een node die gewoon meet. Hij staat bovenaan het tabblad, boven de
   tegels, want dit is wat je als eerste moet weten voordat je naar de cijfers
   eronder kijkt. Zonder forcering staat er niets -- een banner die er altijd staat
   wordt niet meer gelezen. */
function simban(d){
var e=document.getElementById("simban");
var s=d.sim||{n:0,max:2};
if(!s.n){e.innerHTML="";e.className="";return}
var lft=0;
(d.mon||[]).forEach(function(m){if(m.sl>lft){lft=m.sl}});
e.className="sb";
e.innerHTML='<div class="t"><b>'+s.n+" van "+s.max+" sensoren gesimuleerd</b>"+
"Deze node meldt op die kanalen <b>niet wat hij meet</b> &mdash; niet in de tabel "+
"hieronder, niet in de telemetrie en niet in zijn waarschuwingen. De langste "+
"forcering valt over <b>"+(lft?lft+" s":"enkele seconden")+"</b> van zichzelf "+
"terug op de meting.</div>";
var b=document.createElement("button");b.textContent="alles vrijgeven";
b.onclick=function(){psim("sim/clear","")};
e.appendChild(b)}

/* HET BYTEBUDGET, live. Een balk en vier getallen.
 *
 * De balk toont het VASTE deel in grijs en de monitors in groen (amber bij weinig
 * ruimte, rood bij vol). Dat grijze stuk is er niet voor de sier: het is het deel
 * waar niemand iets aan kan doen, en zonder dat onderscheid vraagt iemand zich af
 * waarom er van 180 byte maar 156 voor hem is. Zet hij GPS aan, dan schuift dat
 * grijze stuk 11 byte op -- en dan is meteen te zien waar die byte heen zijn.
 */
function tbud(d){
var b=d.tb;if(!b){return}
TB=b;
var bar=document.getElementById("tbar");
var cls=b.left==0?"b-no":(b.left<15?"b-lo":"b-mon");
var fp=Math.round(b.fixed*100/b.total), mp=Math.round(b.mons*100/b.total);
/* AFKAPPEN OP 100%, want 'used' KAN boven 'total' liggen: de monitors staan er
   met hun duurste stand in, en querySensors kapt pas bij het echte inpakken af.
   Zonder deze grens loopt de balk over zijn eigen rand heen en ziet 190 byte er
   net zo uit als 180 -- precies het onderscheid dat hij moet tonen. */
if(fp>100){fp=100}
if(fp+mp>100){mp=100-fp}
bar.innerHTML="";
function seg(w,c,t){if(w<=0){return}var i=document.createElement("i");
i.style.width=w+"%";i.className=c;i.title=t;bar.appendChild(i)}
seg(fp,"b-fix","vast: "+b.fixed+" byte (spanning, GPS als die aanstaat, en de "+
"drie vaste kanalen)");
seg(mp,cls,"monitors: "+b.mons+" byte");

var e=document.getElementById("tbud");e.innerHTML="";
[[b.used+" / "+b.total,"byte gebruikt",""],
[""+b.left,"byte vrij",b.left==0?"no":(b.left<15?"lo":"")],
[""+b.fixed,"vast (niet vrij te maken)",""],
[b.nms+" × 9b","met pingtijd",""]].forEach(function(x){
var v=document.createElement("div");v.className=x[2];v.textContent=x[0];
var s=document.createElement("span");s.textContent=x[1];
v.appendChild(s);e.appendChild(v)});

/* HOEVEEL ER NOG BIJ KUNNEN, in twee getallen. Dat is de vraag die iemand
   werkelijk heeft, en "34 byte vrij" is daar het antwoord niet op. */
var fit9=Math.floor(b.left/9), fit3=Math.floor(b.left/3);
var n=document.getElementById("tbnote");
n.innerHTML="Er is <b>"+b.left+" byte</b> vrij: dat is nog <b>"+fit9+"</b> "+
"monitor(s) mét pingtijd of <b>"+fit3+"</b> zonder."+
(b.drop?" <b class=\"bad\">"+b.drop+" monitor(s) vielen bij de laatste "+
"uitlezing BUITEN het pakket</b> en staan dus niet in de telemetrie &mdash; zie "+
"de rode getallen in de kolom <i>byte</i>. Zet bij een paar monitors de pingtijd "+
"uit; dat maakt per stuk 6 byte vrij.":"")+
(b.left==0&&!b.drop?" <b>Het pakket is vol.</b> Een monitor erbij wordt "+
"geweigerd, en dat is met opzet: stil afkappen zou verkeerde gegevens op je "+
"dashboard geven.":"")+
"<br>Het vaste deel is <b>"+b.fixed+" byte</b>: "+b.base+" byte van de "+
"basislaag (batterijspanning, en 11 byte extra zodra <b>GPS</b> aanstaat) plus "+
"3 &times; 3 byte voor de kanalen 2, 3 en 4."+
(b.meas?"":" <b>Dit getal is nog niet gemeten</b> &mdash; het is de bekende "+
"ondergrens tot de eerste leesronde (hoogstens 60 s na het opstarten) en kan "+
"daarna hoger blijken.")}

/* DE AFLEVERING VAN HET TESTBERICHT. Drie getallen naast elkaar, en het middelste
   is waar het om gaat: 'bevestigd' naast 'ontvangers'. Zijn ze niet gelijk, dan
   krijgt iemand je waarschuwingen niet -- en dat hoort niet groen te zijn. */
function deliv(d){
var e=document.getElementById("deliv"),t=d.test;
e.innerHTML="";
if(!t||t.st=="niets"){
var v=document.createElement("div");v.className="";
v.textContent=(t&&t.rcnow?t.rcnow:0)+" ontvanger(s) klaar";
var sp=document.createElement("span");
sp.textContent="nog geen test gedaan";v.appendChild(sp);e.appendChild(v);return}
var cl=t.ack>=t.rc?"all":(t.ack?"part":"none");
var st=t.st=="wacht"?"aangevraagd, wacht op de leesronde":
(t.st=="bezig"?"onderweg":"klaar");
[[""+t.rc,"verstuurd naar",""],
[t.ack+" / "+t.rc,"bevestigd met een ack",t.st=="klaar"?cl:"part"],
[st,"test #"+t.seq+", "+t.age+"s geleden",""]].forEach(function(x){
var v=document.createElement("div");v.className=x[2];v.textContent=x[0];
var sp=document.createElement("span");sp.textContent=x[1];
v.appendChild(sp);e.appendChild(v)});
if(t.st=="klaar"&&t.ack<t.rc){
var w=document.createElement("div");w.className="none";
w.textContent="!";var sp=document.createElement("span");
sp.textContent=(t.rc-t.ack)+" ontvanger(s) bevestigden niet";
w.appendChild(sp);e.appendChild(w)}}

/* Aparte post voor het toegangsdeel: eigen meldregel en eigen verversing, want
   status.json en acl.json zijn twee antwoorden met twee tempo's. */
function post3(u,b){return fetch(u,{method:"POST",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b})
.then(function(r){return r.text().then(function(t){
say2(t.trim(),r.ok);u3()})})}

/* ---- toegangsbeheer ---- */
var AT=["?","chat","repeater","kamer","sensor"];
function atype(t){return AT[t]||("type "+t)}
function shortkey(k){return k.slice(0,8)+"…"+k.slice(-4)}
function age(a){if(a<0){return"nooit"}if(a<60){return a+"s"}
if(a<3600){return Math.floor(a/60)+"m"}return hms(a)}

/* Eén cel met de sleutel: afgekort in beeld, volledig in title= en met één klik
   naar het klembord. De volle waarde MOET bereikbaar zijn -- dat is het enige
   waarmee iemand op een andere node kan controleren dat dit de juiste node is. */
function keycell(r,k){var c=r.insertCell();c.className="key";
c.textContent=shortkey(k);c.title=k+"  (klik om te kopieren)";
c.onclick=function(){if(navigator.clipboard){navigator.clipboard.writeText(k);
say2("sleutel gekopieerd",1)}};return c}

/* Eén vinkje. Stuurt de HELE nieuwe stand terug en niet alleen het veranderde
   veld: rechten zijn samen één byte, en een POST die maar één bit beschrijft
   zou de andere twee moeten raden. */
function perm(r,d,k,st,f){var c=r.insertCell();
var b=document.createElement("input");b.type="checkbox";b.checked=!!st[f];
b.title=d;b.onchange=function(){
var q={rd:st.rd,al:st.al,ad:st.ad};q[f]=b.checked?1:0;
if(!q.rd&&!q.al&&!q.ad){b.checked=true;
say2("het laatste recht kan niet uit; gebruik 'wis' om de ingang te verwijderen",
0);return}
post3("acl","key="+k+"&rd="+q.rd+"&alerts="+q.al+"&admin="+q.ad)};
c.appendChild(b);return c}

function lock(d){var e=document.getElementById("lock");
var open=!d.strict;
e.className="lock "+(open?"open":"shut");
e.innerHTML='<div class="t"><b>'+(open?"slot uit":"slot aan")+"</b>"+
(open?"Elke node op het mesh mag de sensoren van deze node uitlezen, ook een "+
"node die hieronder niet staat. De lijst hieronder bepaalt dan alleen wie "+
"waarschuwingen krijgt en wie mag beheren.":
"Alleen de sleutels hieronder met leesrecht krijgen antwoord op een "+
"telemetrieverzoek. De rest krijgt niets: geen leeg antwoord, maar stilte.")+
"</div>";
var b=document.createElement("button");
b.textContent=open?"slot aanzetten":"slot uitzetten";
b.onclick=function(){if(open&&!confirm("Slot aanzetten?\n\nAlleen sleutels met "+
"leesrecht krijgen daarna nog telemetrie. Staat jouw eigen app er niet bij, dan "+
"stopt die met werken.")){return}
post3("acl/strict","on="+(open?1:0))};
e.appendChild(b)}

var AH=[["sleutel",""],["naam",""],["lezen",""],["alarm",""],["beheer",""],
["rechten","num"],["laatst actief","num"],["",""]];

function acltab(d){var e=document.getElementById("al");e.innerHTML="";
var hr=e.insertRow();AH.forEach(function(x){var h=document.createElement("th");
h.textContent=x[0];h.className=x[1];hr.appendChild(h)});
var L=d.acl||[];
if(!L.length){var r=e.insertRow(),c=r.insertCell();c.colSpan=AH.length;
c.className="c-warn";c.textContent=d.strict?
"leeg EN het slot staat aan: niemand kan de sensoren uitlezen":
"leeg: nog geen enkele node toegelaten";return}
L.forEach(function(a){var r=e.insertRow();
keycell(r,a.k);
var c=r.insertCell();c.className="nm";c.textContent=a.n||"–";
/* De rol zit in de onderste twee bits (0=gast, 1=lezen, 3=beheer); alarm is
   een los bit. Een vinkje uitzetten stuurt de HELE nieuwe stand terug, want het
   rechtenveld is één byte en geen drie losse instellingen. */
var role=a.p&3,st={rd:role>=1?1:0,al:(a.p&192)?1:0,ad:role==3?1:0};
perm(r,"mag telemetrie opvragen",a.k,st,"rd");
perm(r,"krijgt waarschuwingen",a.k,st,"al");
perm(r,"mag beheren (omvat lezen)",a.k,st,"ad");
c=r.insertCell();c.className="num";c.textContent="0x"+
("0"+a.p.toString(16)).slice(-2).toUpperCase();
c=r.insertCell();c.className="num";c.textContent=age(a.a);
c=r.insertCell();var b=document.createElement("button");b.textContent="wis";
b.onclick=function(){if(confirm("Sleutel "+shortkey(a.k)+" verwijderen?"))
{post3("acl/del","key="+a.k)}};c.appendChild(b)})}

var NH=[["sleutel",""],["naam",""],["soort",""],["hop","num"],["snr","num"],
["gehoord","num"],["adverts","num"],["",""]];

function nbtab(d){var e=document.getElementById("nb");e.innerHTML="";
var hr=e.insertRow();NH.forEach(function(x){var h=document.createElement("th");
h.textContent=x[0];h.className=x[1];hr.appendChild(h)});
var L=(d.nb||[]).slice().sort(function(x,y){return x.a-y.a});
if(!L.length){var r=e.insertRow(),c=r.insertCell();c.colSpan=NH.length;
c.className="c-unk";c.textContent=
"nog niets gehoord (adverts komen met minuten tussenruimte)";return}
L.forEach(function(m){var r=e.insertRow();
keycell(r,m.k);
var c=r.insertCell();c.className="nm";c.textContent=m.n||"–";
c=r.insertCell();c.textContent=atype(m.t);
c=r.insertCell();c.className="num";c.textContent=m.h;
c=r.insertCell();c.className="num";c.textContent=m.s;
c=r.insertCell();c.className="num";c.textContent=age(m.a);
c=r.insertCell();c.className="num";c.textContent=m.c;
c=r.insertCell();
if(m.in){c.className="c-on";c.textContent="in lijst"}else{
var b=document.createElement("button");b.textContent="+ lezen";
b.style.color="var(--accent)";
b.onclick=function(){post3("acl","key="+m.k+"&rd=1&alerts=0&admin=0")};
c.appendChild(b)}})}

function u3(){fetch("acl.json").then(function(r){
if(!r.ok){return r.text().then(function(t){say2(t.trim(),0)})}
return r.json().then(function(d){lock(d);acltab(d);nbtab(d)})})}

function u2(){return fetch("status.json").then(function(r){return r.json()})
.then(function(d){tiles(d);simban(d);table(d);tbud(d);deliv(d);recui(d);
var s=document.getElementById("ssid");if(!s.value){s.value=d.ssid}
document.getElementById("sub").textContent=d.ssid?"bewaking · "+d.ssid:"bewaking"})
/* EEN MISLUKTE VERVERSING MOET ZICH MELDEN. Zonder deze tak bleef de pagina
   staan met gegevens van een minuut geleden alsof het het heden was, en de fout
   verdween in een afgewezen promise die niemand ziet. Een tabel die stilstaat is
   niet te onderscheiden van een node waar niets gebeurt -- en dat is precies het
   verschil dat deze pagina moet tonen. */
.catch(function(){tsay("geen verbinding met de node -- wat hieronder staat is de "+
"laatst ontvangen stand en niet het heden",0)})}

/* ============================== nodebeheer =============================== */

/* ---- tabbladen ---- */
/* Drie secties, één zichtbaar. Geen router en geen hash in de URL: dit is één
   document en een herlaadde pagina hoort op het overzicht te beginnen, want dat
   is waarvoor dit apparaat er staat. */
var TB=document.querySelectorAll(".tabs button");
for(var i=0;i<TB.length;i++){TB[i].onclick=function(){
var p=this.getAttribute("data-p");
for(var j=0;j<TB.length;j++){TB[j].className=TB[j]==this?"on":""}
for(var k=1;k<=3;k++){document.getElementById("p"+k).hidden=(""+k)!=p}
if(p=="3"){cfg()}}}

/* ---- de console ---- */
/* Nieuwste bovenaan en hoogstens 40 regels. Zonder die grens groeit dit venster
   ongelimiteerd op een pagina die dagen open kan staan. */
function logline(cmd,txt,ok){
var o=document.getElementById("out");
var e=document.createElement("div");e.className="e";
var q=document.createElement("div");q.className="q";q.textContent="> "+cmd;
var r=document.createElement("div");r.className=ok?"r":"x";
r.textContent=txt&&txt.trim()?txt.trim():"(geen antwoord)";
e.appendChild(q);e.appendChild(r);
o.insertBefore(e,o.firstChild);
while(o.childElementCount>40){o.removeChild(o.lastChild)}}

/* EEN AFGEWEZEN OPDRACHT KOMT MET EEN 200 TERUG, en dat is met opzet: de HTTP-code
   zegt of het VERZOEK gelukt is, niet of de node het eens was met de opdracht. Een
   CLI antwoordt in tekst, en /cli geeft die tekst ongewijzigd door -- de console
   moet weergeven wat de node zei en er niet zijn eigen oordeel voor zetten.

   Voor de KLEUR is er wel een oordeel nodig, en dat is deze zeef. Het zijn de
   letterlijke foutvormen van CommonCLI: "Err - ", "ERR:", "ERROR:", "Error,",
   "Unknown command", "can't find custom var" en "??: " voor een onbekend veld. Op
   'sensor set' is het bovendien het enige onderscheid dat er is: die antwoordt
   "ok" of "can't find custom var" en zegt nooit waarom. */
function isErr(t){return /^(err|error|unknown|can't|\?\?)/i.test((t||"").trim())}

/* Eén opdracht. Geeft een promise met {ok,txt} terug zodat een reeks opdrachten
   op elkaar kan wachten -- niet omdat het moet, maar omdat vijf tegelijk op een
   node die tussendoor een radio bedient vijf keer een flashschrijving in de wacht
   zet. Achter elkaar is hier vriendelijker dan tegelijk. */
function cli(cmd,cf){
var b="cmd="+encodeURIComponent(cmd);
if(cf){b+="&confirm="+cf}
return fetch("cli",{method:"POST",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b})
.then(function(r){return r.text().then(function(t){
var ok=r.ok&&!isErr(t);logline(cmd,t,ok);return{ok:ok,txt:t}})})
.catch(function(){logline(cmd,"geen verbinding met de node",0);
return{ok:false,txt:"geen verbinding"}})}

/* Een reeks, achter elkaar. done(gelukt, mislukt, laatste tekst). */
function cliSeq(cmds,done){
var list=cmds.slice(),good=0,bad=0,last="";
function step(){
if(!list.length){if(done){done(good,bad,last)}return}
cli(list.shift()).then(function(res){
if(res.ok){good++}else{bad++}last=res.txt;step()})}
step()}

function send(cmd,cf){return cli(cmd,cf)}

document.getElementById("cb").onclick=function(){
var el=document.getElementById("ci"),c=el.value.trim();
if(!c){return}
/* De twee onomkeerbare hier ook achter een bevestiging, ook als iemand ze
   intypt. De node vraagt zelf niets -- een CLI kent geen 'weet je het zeker'. */
if(/^erase\b/.test(c)&&!confirm("Bestandssysteem WISSEN?\n\nDit gooit de "+
"monitorlijst, de toegangslijst, de wifi-instelling en de voorkeuren weg. Het "+
"kanaalgeheugen (welk nummer al vergeven is) gaat mee. Niet omkeerbaar.")){return}
if(/^(reboot|clkreboot)\b/.test(c)&&!confirm("Node herstarten?\n\nDe bewaking "+
"staat een halve minuut stil en de gemeten toestanden beginnen weer op '?'."))
{return}
/* Ook een INGETYPTE radio-opdracht krijgt de vraag. De server weigert hem zonder
   confirm=radio en die stuurt de console mee -- dus zonder deze vraag zou typen
   een sluipweg om de waarschuwing heen zijn, en dan is de waarschuwing bij het
   formulier een formaliteit in plaats van een grens. */
var rf=/^set (radio|freq) /.test(c);
if(rf&&!confirm("MESH-AFSPRAAK VAN DEZE NODE WIJZIGEN\n\n  "+c+"\n\nfreq, bw, sf "+
"en cr bepalen of deze node nog op hetzelfde mesh zit. Klopt er een niet met de "+
"rest, dan hoort niemand hem nog over LoRa en hoort hij niemand.\n\nHij blijft "+
"over WiFi bereikbaar, dus dit is terug te zetten -- maar de bewaking is tot dan "+
"stil. Gaat pas gelden na een herstart.\n\nHet formulier onder 'Radio' zet de "+
"huidige en de gebakken waarde ernaast; dat is de veiligere weg.\n\nDoorgaan?"))
{return}
send(c,/^erase\b/.test(c)?"erase":(rf?"radio":""));
el.value=""}

document.getElementById("ci").onkeydown=function(ev){
if(ev.key=="Enter"){document.getElementById("cb").onclick()}}

/* Snelknoppen. Twee ervan staan in het rood en vragen eerst. */
var QB=document.querySelectorAll(".quick button");
for(var i=0;i<QB.length;i++){QB[i].onclick=function(){
var c=this.getAttribute("data-c");
document.getElementById("ci").value=c;
document.getElementById("cb").onclick()}}

document.getElementById("rgb").onclick=function(){
var el=document.getElementById("rgi"),c=el.value.trim();
if(!c){return}
if(!/^region\b/.test(c)){c="region "+c}
send(c).then(function(){el.value=""})}

/* ---- de instelformulieren ----
 *
 * EEN TABEL EN GEEN DERTIG HANDGESCHREVEN VELDEN. Elk veld is
 * [sleutel in cfg.json, label, CLI-voorvoegsel, soort], en één functie maakt er
 * een formulier van en één functie maakt er weer opdrachtregels van. Dat houdt de
 * pagina klein, maar belangrijker: er is precies één plek waar de koppeling
 * tussen een veld en zijn CLI-opdracht staat. Dertig handgeschreven formulieren
 * zijn dertig plaatsen waar die koppeling stil verkeerd kan staan.
 *
 * Soort: "n" = getal, "t" = tekst, "s:a|b|c" = keuzelijst.
 */
var GRP={
rf:[["freq","frequentie (MHz)","","n"],["bw","bandbreedte (kHz)","","n"],
["sf","spreading factor","","n"],["cr","coding rate","","n"]],
pwr:[["tx","zendvermogen (dBm)","set tx","n"],
["af","airtime-factor","set af","n"],
["agc","agc-reset (s)","set agc.reset.interval","n"],
["rxgain","rx boosted gain","set radio.rxgain","s:on|off"],
["femrx","fem lna rxgain","set radio.fem.rxgain","s:on|off"]],
adv:[["name","nodenaam","set name","t"],
["lat","breedtegraad","set lat","n"],["lon","lengtegraad","set lon","n"],
["advint","advert-interval (min)","set advert.interval","n"],
["fadvint","flood-advert (uur)","set flood.advert.interval","n"],
["advloc","locatie in advert","gps advert","s:none|prefs|share"],
["owner","eigenaarsregel","set owner.info","t"]],
fwd:[["repeat","doorsturen","set repeat","s:on|off"],
["fmax","flood max hops","set flood.max","n"],
["fmaxuns","flood max ongescoped","set flood.max.unscoped","n"],
["fmaxadv","flood max advert","set flood.max.advert","n"],
["loopd","lusdetectie","set loop.detect","s:off|minimal|moderate|strict"],
["rxdelay","rx-vertraging (basis)","set rxdelay","n"],
["txdelay","tx-vertraging flood","set txdelay","n"],
["dtxdelay","tx-vertraging direct","set direct.txdelay","n"],
["multiack","meervoudige acks","set multi.acks","n"]],
hash:[["hashmode","padhash-modus","set path.hash.mode","s:0|1|2"]],
misc:[["cad","cad voor zenden","set cad","s:on|off"],
["intthr","interferentiedrempel","set int.thresh","n"],
["rdonly","gastlezen toestaan","set allow.read.only","s:on|off"],
["adcmult","adc-vermenigvuldiger","set adc.multiplier","n"]]};

/* De waarde zoals hij bij het laatste ophalen was. Hierop wordt bij Opslaan
   vergeleken, zodat er alleen gestuurd wordt wat echt veranderd is. */
var WAS={};
/* De besturingselementen, per groep en per sleutel. */
var CTL={};

function build(g,cfgd){
var box=document.getElementById("g-"+g);if(!box){return}
box.innerHTML="";CTL[g]={};
GRP[g].forEach(function(f){
var key=f[0],val=cfgd[key];if(val===undefined){val=""}
val=""+val;
WAS[g+"."+key]=val;
var lab=document.createElement("label");
var t=document.createTextNode(f[1]+" ");lab.appendChild(t);
var cur=document.createElement("span");cur.className="cur";
cur.textContent="nu: "+(val===""?"–":val);
/* Alleen bij de radio is er een gebakken waarde om naast te leggen, en juist
   daar is afwijken het gevaar. Amber, niet rood: afwijken kan bedoeld zijn. */
if(g=="rf"&&cfgd.baked&&cfgd.baked[key]!==undefined&&
(""+cfgd.baked[key])!=val){cur.className="cur dev";
cur.textContent+=" ≠ gebakken "+cfgd.baked[key]}
lab.appendChild(cur);
var ctl;
if(f[3].slice(0,2)=="s:"){
ctl=document.createElement("select");
f[3].slice(2).split("|").forEach(function(o){
var op=document.createElement("option");op.value=o;op.textContent=o;
if(o==val){op.selected=true}ctl.appendChild(op)})}
else{ctl=document.createElement("input");ctl.spellcheck=false;
ctl.value=val;ctl.maxLength=key=="owner"?119:(f[3]=="t"?31:12)}
lab.appendChild(ctl);box.appendChild(lab);CTL[g][key]=ctl})}

/* Wat er veranderd is, als opdrachtregels. */
function changed(g){
var out=[];
GRP[g].forEach(function(f){
var key=f[0],c=CTL[g]&&CTL[g][key];if(!c||!f[2]){return}
var v=(""+c.value).trim();
if(v===WAS[g+"."+key]){return}
if(v===""){return}   /* leeg = niet bedoeld; wissen gaat met een echte waarde */
out.push(f[2]+" "+v)});
return out}

function saveGroup(g){
var cmds=changed(g);
if(!cmds.length){logline("(opslaan)","niets veranderd in deze groep",1);return}
cliSeq(cmds,function(){cfg()})}

var SB=document.querySelectorAll("button[data-g]");
for(var i=0;i<SB.length;i++){SB[i].onclick=function(){
saveGroup(this.getAttribute("data-g"))}}

/* ---- de radio: apart, en met twee sloten ----
 *
 * freq, bw, sf en cr gaan in EEN opdracht ('set radio f,bw,sf,cr'), want zo zet
 * CommonCLI ze ook: er is geen 'set bw' en geen 'set sf'. Dat is hier gunstig --
 * je kunt de vier niet half zetten en daarmee niet halverwege van het mesh
 * vallen.
 *
 * Twee sloten: het vinkje ernaast moet aan (anders is de knop uit) en daarna
 * vraagt confirm() nog een keer, met de OUDE en de NIEUWE waarde naast elkaar en
 * met wat er gebeurt in woorden. De server zet een derde slot: POST /cli weigert
 * 'set radio' en 'set freq' zonder confirm=radio, zodat een losse fetch of een
 * voorgeladen link er niet bij kan.
 */
var RFARM=document.getElementById("rfarm");
RFARM.onchange=function(){document.getElementById("rfgo").disabled=!RFARM.checked};

document.getElementById("rfgo").onclick=function(){
if(!RFARM.checked){return}
var c=CTL.rf;if(!c){return}
var v={};var keys=["freq","bw","sf","cr"];
for(var i=0;i<keys.length;i++){
v[keys[i]]=(""+c[keys[i]].value).trim();
if(v[keys[i]]===""){logline("(set radio)","alle vier de velden moeten "+
"ingevuld zijn",0);return}}
var lines=keys.map(function(k){
var w=WAS["rf."+k];return "  "+k+": "+w+(v[k]==w?"  (ongewijzigd)":"   ->   "+v[k])});
if(!confirm("MESH-AFSPRAAK VAN DEZE NODE WIJZIGEN\n\n"+lines.join("\n")+
"\n\nDit bepaalt of deze node nog op hetzelfde mesh zit. Klopt een van deze vier "+
"niet met de rest van het mesh, dan hoort niemand hem nog en hoort hij niemand.\n\n"+
"Hij blijft over WiFi bereikbaar, dus je kunt het van hier terugzetten -- maar "+
"de bewaking is tot dan stil, en waarschuwingen over LoRa komen niet aan.\n\n"+
"Gaat pas gelden na een herstart. Doorgaan?")){return}
RFARM.checked=false;document.getElementById("rfgo").disabled=true;
send("set radio "+v.freq+","+v.bw+","+v.sf+","+v.cr,"radio")
.then(function(){cfg()})}

/* ---- wachtwoorden ---- */
document.getElementById("pwgo").onclick=function(){
var a=document.getElementById("pw1"),b=document.getElementById("pw2");
var cmds=[];
if(a.value){cmds.push("password "+a.value)}
if(b.value){cmds.push("set guest.password "+b.value)}
if(!cmds.length){logline("(wachtwoord)","niets ingevuld",0);return}
if(a.value&&!confirm("Beheerderswachtwoord wijzigen?\n\nElke app en elke node "+
"die met het OUDE wachtwoord inlogde, moet het nieuwe krijgen. Ingangen die al "+
"in de toegangslijst staan blijven werken -- die hebben het wachtwoord niet meer "+
"nodig.")){return}
cliSeq(cmds,function(){a.value="";b.value="";cfg()})}

/* ---- de leeskant: één GET met de hele stand ---- */
function cfg(){
return fetch("cfg.json").then(function(r){
if(!r.ok){return r.text().then(function(t){
logline("(cfg.json)",t,0)})}
return r.json().then(function(d){
build("rf",d);build("pwr",d);build("adv",d);build("fwd",d);
build("hash",d);build("misc",d);
var bk=d.baked||{};
document.getElementById("baked").textContent=
bk.freq+" MHz · "+bk.bw+" kHz · sf "+bk.sf+" · cr "+bk.cr;
document.getElementById("idname").value=d.name||"";
document.getElementById("idkey").value=d.pubkey||"";
pwalarm(d);budget(d)})})}

/* HET WACHTWOORDALARM. Rood zolang het beheerderswachtwoord nog de gebakken
   standaard is (of leeg), en dan GROEN als het gezet is -- niet weg. Een
   waarschuwing die verdwijnt zodra het opgelost is leest als voortgang; een die
   blijft staan wordt genegeerd, en dan is de volgende ook niets meer waard. Dat
   het groene vakje er even staat is het punt: je ziet dat het gelukt is. */
function pwalarm(d){
var e=document.getElementById("pwal");
if(d.pwdef||d.pwempty){
e.className="alarm";
e.innerHTML="<b>het beheerderswachtwoord staat nog op de standaard</b>"+
(d.pwempty?"Het is <i>leeg</i>. ":"Het is nog de waarde die in de bouwvlag "+
"<code>ADMIN_PASSWORD</code> staat. ")+
"Wie die waarde kent, logt over het mesh in op deze node, wordt daarmee "+
"<b>automatisch beheerder</b> in de toegangslijst — ook met het slot aan "+
"— en heeft dan deze hele CLI: instellingen, radio, wissen. Het slot en dit "+
"wachtwoord zijn samen één beveiliging en niet twee. Zet het onder "+
"<i>Wachtwoorden</i> hieronder, of met <code>password &lt;nieuw&gt;</code> in de "+
"console."}
else{e.className="alarm ok";
e.innerHTML="<b>beheerderswachtwoord is gezet</b>Het wijkt af van de gebakken "+
"standaard. Wie over het mesh op deze node wil inloggen heeft het nodig, en "+
"alleen dan wordt hij als beheerder in de toegangslijst gezet."}}

/* HET KANAALBUDGET. Drie getallen die naast elkaar horen: hoeveel vakjes bezet
   zijn, hoeveel nummers ooit vergeven zijn, en hoeveel er nog nieuw uit te delen
   valt. Dat laatste getal is waar het om gaat -- het loopt terug en het komt niet
   terug, en dat hoort iemand te zien VOORDAT hij de ruimte opmaakt. */
function budget(d){
var free=d.ch_free,e=document.getElementById("bud");
var cl=free==0?"no":(free<=2?"lo":"");
e.innerHTML="";
[[d.mon_used+" / "+d.mon_max,"vakjes bezet",""],
[""+d.ch_ever,"nummers ooit vergeven",""],
[""+free,"nog nieuw uit te delen",cl]].forEach(function(x){
var v=document.createElement("div");v.className=x[2];
v.textContent=x[0];
var s=document.createElement("span");s.textContent=x[1];
v.appendChild(s);e.appendChild(v)});
var n=document.getElementById("budnote");
n.innerHTML="Kanaal "+d.ch_first+" t/m "+d.ch_last+", dus "+d.mon_max+" in "+
"totaal. Een nummer dat is uitgedeeld wordt <b>niet opnieuw gebruikt</b> zolang "+
"er nog één over is die nooit vergeven is — ook niet nadat je de "+
"monitor verwijderd hebt. Pas als alle "+d.mon_max+" een keer op zijn, begint de "+
"node te hergebruiken, en dan gaat een bewaarde koppeling in een dashboard "+
"onvermijdelijk naar een andere dienst wijzen."+
(free==0?" <b>Dat punt is nu bereikt.</b> Loop je dashboards na voordat je nog "+
"een monitor aanmaakt.":(free<=2?" <b>Er zijn er nog "+free+".</b> Bewerk een "+
"bestaande monitor in plaats van hem weg te gooien en opnieuw aan te maken.":""))+
"<br>Het getal 'ooit vergeven' komt uit <code>ch_ever_used</code> in "+
"<code>/monitors.cfg</code>, bij het opstarten gelezen, plus de kanalen die nu in "+
"gebruik zijn. Kort na een wijziging kan het één achterlopen: die byte "+
"wordt met twee seconden uitstel naar flash geschreven."}

/* De laatst bekende budgetstand, om VOOR het versturen te kunnen zeggen dat het
   niet past. De node weigert het zelf ook (createMonitor -> MON_ERR_BYTES); dit is
   gemak en niet het slot. Maar het is wel het gemak dat telt: een melding vóór de
   klik voorkomt een kanaal dat voor niets vergeven wordt. */
var TB=null;

document.getElementById("a").onsubmit=function(ev){ev.preventDefault();
/* f.elements[..] en niet f.name: op een formulier IS .name het name-attribuut
   van het formulier zelf en niet het veld dat zo heet. */
var f=ev.target.elements;
/* Een NIEUWE monitor wordt met pingtijd aangemaakt en kost dus 9 byte. */
if(TB&&TB.left<9){
say("past niet meer in het telemetriepakket: er is "+TB.left+" byte vrij en een "+
"nieuwe monitor kost 9 byte (schakelaar 3 + pingtijd 6). Zet in de tabel "+
"hierboven bij "+Math.ceil((9-TB.left)/6)+" monitor(s) de pingtijd uit -- dat "+
"maakt per stuk 6 byte vrij -- of verwijder een monitor.",0);
return}
post("monitor","name="+encodeURIComponent(f["name"].value)+
"&host="+encodeURIComponent(f["host"].value)+
"&int="+encodeURIComponent(f["int"].value))};

/* ---- de twee knoppen van het simulatiedeel ----
 *
 * Het TESTBERICHT vraagt eerst. Niet omdat het gevaarlijk is, maar omdat het
 * zendtijd kost op een band die met anderen gedeeld wordt, en omdat het bij
 * iedereen met het alarmrecht een melding op de telefoon geeft. Wie dat per
 * ongeluk doet, heeft een paar mensen wakker gemaakt voor niets. */
document.getElementById("tgo").onclick=function(){
if(!confirm("Testbericht sturen?\n\nEr gaat een ECHT bericht over het mesh naar "+
"iedereen met het alarmrecht -- gemarkeerd als test, maar het geeft wel een "+
"melding op hun telefoon. Het kost zendtijd op een gedeelde band, en het gaat de "+
"deur uit bij de volgende leesronde (hoogstens 60 s).\n\nDoorgaan?")){return}
ssay("bezig...",1);
psim("alert/test","")}

document.getElementById("sclr").onclick=function(){psim("sim/clear","")}

/* De herstelmelding gaat over de CLI en niet over een eigen route, om dezelfde
   reden als de monitorvelden: daar zit de keuring en daar zit het wegschrijven
   naar flash. Een tweede schrijfpad zou een tweede keuring zijn.

   ALLEEN WAT VERANDERD IS, want elke gelukte 'sensor set' zet een
   flashschrijving in de wacht. WASREC houdt de laatst ontvangen stand bij. */
var WASREC={on:null,hold:null};
document.getElementById("rgo").onclick=function(){
var on=document.getElementById("recon").checked?1:0;
var h=parseInt(document.getElementById("rhold").value,10);
if(!(h>=0&&h<=3600)){ssay("rust: 0 t/m 3600 s",0);return}
var cmds=[];
if(WASREC.on===null||on!=WASREC.on){cmds.push("sensor set alert.recover "+on)}
if(WASREC.hold===null||h!=WASREC.hold){cmds.push("sensor set alert.rhold "+h)}
if(!cmds.length){ssay("niets veranderd",1);return}
ssay("bezig...",1);
cliSeq(cmds,function(good,bad,last){
if(bad){ssay(bad+" van de "+(good+bad)+" geweigerd: "+last.trim(),0)}
else{ssay("herstelmelding "+(on?"aan":"uit")+", rust "+h+" s",1)}
u2()})}

/* De twee velden bijwerken uit status.json -- maar NIET terwijl iemand erin
   bezig is. Daarom alleen als de waarde nog overeenkomt met wat er het laatst
   uit de node kwam; wie iets getypt heeft, houdt zijn tekst. Dezelfde regel als
   bij het bewerken van een tabelregel, en om dezelfde reden. */
function recui(d){
var r=d.rec;if(!r){return}
var cb=document.getElementById("recon"),hf=document.getElementById("rhold");
if(WASREC.on===null||cb.checked==(WASREC.on==1)){cb.checked=r.on==1}
if(WASREC.hold===null||hf.value==""+WASREC.hold){hf.value=r.hold}
WASREC.on=r.on;WASREC.hold=r.hold;
document.getElementById("rholdcur").textContent=
"nu: "+(r.on?"aan":"uit")+", rust "+r.hold+" s"+(r.hold?"":" (meteen melden)")}

document.getElementById("ka").onsubmit=function(ev){ev.preventDefault();
var f=ev.target.elements;
post3("acl","key="+encodeURIComponent(f["key"].value.trim())+
"&rd="+(f["rd"].checked?1:0)+"&alerts="+(f["alerts"].checked?1:0)+
"&admin="+(f["admin"].checked?1:0)).then(function(){f["key"].value=""})};

/* DRIE tempo's, met opzet. Metingen veranderen per minuut, dus die elke 5 s.
   De toegangslijst verandert alleen als er iemand klikt; de buurtlijst groeit
   met de tussenruimte van adverts (minuten). Elke 20 s is daarvoor ruim, en het
   scheelt 4 kB per verversing over de wifi.

   DE INSTELLINGEN VERVERSEN NIET OP EEN KLOK. Ze veranderen alleen als iemand
   hier op Opslaan drukt of aan de seriële console zit, en een formulier dat
   onder je handen wordt bijgewerkt terwijl je erin typt is een formulier dat je
   tekst weggooit. Eén keer bij het laden -- het kanaalbudget staat op het eerste
   tabblad en moet er meteen staan -- en daarna bij het openen van het
   beheertabblad en na elke gelukte wijziging. */
u2();setInterval(u2,5000);
u3();setInterval(u3,20000);
cfg();
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

  /* De stand van de kanaaltoewijzer uit /monitors.cfg. EEN keer, hier, en nooit
   * in een verzoekpad: dit is dezelfde afspraak als bij loadWifiConfig() -- een
   * File-object alloceert intern, en dat mag bij het opstarten en niet per
   * verzoek.
   *
   * De MonitorCfg staat op de stapel en niet statisch. Hij is ongeveer 500 byte
   * en hij leeft één functieaanroep; begin() wordt uit setup() geroepen en daar
   * is die ruimte er. Hem statisch maken zou 500 byte RAM kosten voor een waarde
   * waar één byte van gebruikt wordt.
   *
   * Mislukt het lezen, dan blijft het masker 0 en vult cfg.json zich met wat er
   * nu in gebruik is. Dat is een ONDERschatting van wat vergeven is, en dat is de
   * goede kant om fout te zitten: de pagina belooft dan niet meer ruimte dan er
   * is. Zij zegt er ook bij waar het getal op berust. */
  {
    MonitorCfg cfg;
    MonitorStore::setDefaults(cfg);
    if (MonitorStore::load(SPIFFS, cfg)) g_ever_mask = cfg.ch_ever_used;
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
  /* Toegangsbeheer. Om dezelfde reden alleen POST, en hier nog een graadje
   * scherper: een GET die leesrecht uitdeelt is een link die iemand kan sturen,
   * en een browser die hem voorlaadt deelt het recht uit zonder dat er iemand
   * geklikt heeft. */
  _server->on("/acl.json", HTTP_GET, web_route_acljson);
  _server->on("/acl", HTTP_POST, web_route_aclset);
  _server->on("/acl/del", HTTP_POST, web_route_acldel);
  _server->on("/acl/strict", HTTP_POST, web_route_aclstrict);
  /* Nodebeheer. De leeskant (/cfg.json) is een GET en verandert niets; de
   * schrijfkant is EEN route, en die is POST-only. Dat is hier geen formaliteit:
   * een GET /cli?cmd=erase zou een link zijn die de hele opslag wist zodra een
   * browser hem voorlaadt, en dat is de ergste knop die dit apparaat heeft. */
  _server->on("/cfg.json", HTTP_GET, web_route_cfgjson);
  _server->on("/cli", HTTP_POST, web_route_cli);
  /* Simuleren en testen. POST-only, en hier om twee redenen: een GET die een
   * sensor forceert is een link waarmee iemand de bewaking van een kanaal
   * uitzet, en een GET die een testbericht stuurt is een link die zendtijd kost
   * bij elke prefetch. Beide gebeuren zonder dat er iemand geklikt heeft. */
  _server->on("/sim", HTTP_POST, web_route_sim);
  _server->on("/sim/clear", HTTP_POST, web_route_simclear);
  _server->on("/alert/test", HTTP_POST, web_route_alerttest);
  _server->onNotFound([]() { g_server.send(404, "text/plain", "niet gevonden"); });
}

void WebTask::loop() {
  /* De uitgestelde opdracht eerst, en BUITEN de wifi-controle: hij is al
   * aangenomen en al beantwoord, dus of er nu nog een netwerk is doet niet meer
   * mee. Een aangevraagde herstart die niet doorgaat omdat de wifi tussendoor
   * wegviel, is precies de soort halve toestand waar deze node niet in mag
   * blijven staan. */
  runDeferred();

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

/* De naam van een simulatiestand in JSON. Eén plek, want de pagina vergelijkt op
 * deze woorden en de server schrijft ze -- twee lijstjes die uiteen kunnen lopen
 * is precies hoe een tabel stil de verkeerde kleur krijgt. */
static const char* simModeName(MonitorSensors::SimMode m) {
  switch (m) {
    case MonitorSensors::SIM_UP:   return "up";
    case MonitorSensors::SIM_DOWN: return "down";
    default:                       return "off";
  }
}

static const char* testStateName(MonitorSensors::TestState s) {
  switch (s) {
    case MonitorSensors::TEST_PENDING: return "wacht";
    case MonitorSensors::TEST_SENDING: return "bezig";
    case MonitorSensors::TEST_DONE:    return "klaar";
    default:                           return "niets";
  }
}

void WebTask::handleStatus() {
  if (!requireAuth()) return;

  IPAddress ip = _wifi->isApMode() ? WiFi.softAPIP() : WiFi.localIP();
  char esc[80];
  jsonEscape(g_ssid_shown, esc, sizeof(esc));

  /* HET SIMULATIE- EN TESTBLOK. Apart samengesteld en niet in de grote snprintf
   * hieronder: zonder sensorlaag bestaat het niet, en een veld dat er soms wel en
   * soms niet is, moet de pagina per keer kunnen zien. Een leeg blok is hier het
   * eerlijke antwoord -- de banner blijft dan weg omdat er niets te melden is.
   *
   * "rc" is het aantal ontvangers van de TEST en "rcnow" hoeveel er nu zijn. Die
   * twee staan er beide, want ze kunnen verschillen: wie tussen twee tests een
   * alarmrecht weghaalt, moet niet denken dat de oude uitslag nog geldt. */
  /* HET BYTEBUDGET, in elk status.json en niet alleen bij het toevoegen.
   *
   * Het hoort LIVE te zijn omdat het live verandert: een monitor die op komt gaat
   * van 3 naar 9 byte, en iemand die GPS aanzet verliest er in één keer 11. Een
   * getal dat alleen bij het aanmaken berekend wordt, klopt precies niet op het
   * moment dat het telt. "meas" zegt of het vaste deel al echt gemeten is; tot de
   * eerste leesronde staat er de bekende ondergrens en zegt de pagina dat erbij. */
  char budblk[200];
  budblk[0] = 0;
  if (_mon != nullptr) {
    MonitorSensors::TelemBudget b;
    _mon->telemBudget(b);
    snprintf(budblk, sizeof(budblk),
        "\"tb\":{\"total\":%u,\"base\":%u,\"fixed\":%u,\"mons\":%u,\"used\":%u,"
        "\"left\":%u,\"nms\":%u,\"drop\":%u,\"meas\":%d,\"sw\":%u,\"gen\":%u},",
        (unsigned)b.total, (unsigned)b.base, (unsigned)b.fixed, (unsigned)b.mons,
        (unsigned)b.used, (unsigned)b.left, (unsigned)b.num_ms, (unsigned)b.dropped,
        b.measured ? 1 : 0,
        (unsigned)MonitorSensors::TELEM_BYTES_SWITCH_PUB,
        (unsigned)MonitorSensors::TELEM_BYTES_GENERIC_PUB);
  }

  char simblk[260];
  simblk[0] = 0;
  if (_mon != nullptr) {
    snprintf(simblk, sizeof(simblk),
        "\"sim\":{\"n\":%u,\"max\":%u,\"secs\":%u,\"min\":%u,\"lim\":%u},"
        "\"rec\":{\"on\":%d,\"hold\":%u},"
        "\"test\":{\"st\":\"%s\",\"seq\":%u,\"rc\":%u,\"ack\":%u,\"age\":%lu,"
        "\"wait\":%lu,\"rcnow\":%u},",
        (unsigned)_mon->simActiveCount(),
        (unsigned)MonitorSensors::MAX_SIM_ACTIVE,
        (unsigned)MonitorSensors::SIM_SECS_DEFAULT,
        (unsigned)MonitorSensors::SIM_SECS_MIN,
        (unsigned)MonitorSensors::SIM_SECS_MAX,
        _mon->recoverEnabled() ? 1 : 0,
        (unsigned)_mon->recoverHoldSecs(),
        testStateName(_mon->testState()),
        (unsigned)_mon->testSeq(),
        (unsigned)_mon->testRecipients(),
        (unsigned)_mon->testAcks(),
        (unsigned long)_mon->testAgeSecs(),
        (unsigned long)_mon->testWaitSecs(),
        (unsigned)countAlertRecipients());
  }

  int n = snprintf(g_json, sizeof(g_json),
      "{\"fw\":\"%s\",\"wifi\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"rssi\":%d,"
      "\"reason\":%u,\"reconnects\":%lu,\"resets\":%lu,\"uptime\":%lu,"
      "\"heap\":%lu,\"largest\":%lu,\"ssid\":\"%s\","
      "\"mains\":%d,\"volts\":\"%.3f\",\"paused\":%d,%s%s\"mon\":[",
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
      _mon != nullptr && _mon->monitorsPaused() ? 1 : 0,
      budblk, simblk);

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

    /* DE SIMULATIEVELDEN, op ELKE regel.
     *
     *   "si" = sensornummer voor POST /sim; -1 betekent "hier valt niets te
     *          forceren" en dat geldt alleen voor kanaal 1, dat van SensorMesh
     *          zelf is en niet van ons.
     *   "sm" = off / up / down
     *   "sl" = seconden tot het van zichzelf vervalt
     *
     * Kanaal 2 en 3 dragen HETZELFDE sensornummer, en dat is geen slordigheid:
     * het is één meting met twee namen (zie MonitorSensors.h). Beide regels
     * kleuren daardoor samen amber, en een klik op de een doet hetzelfde als een
     * klik op de ander -- wat klopt, want "netvoeding weg" en "op batterij" zijn
     * hetzelfde feit. */
    const MonitorSensors::SimMode pw_sim = _mon->simMode(MonitorSensors::SIM_POWER);
    const MonitorSensors::SimMode wf_sim = _mon->simMode(MonitorSensors::SIM_WIFI);

    n += snprintf(buf + n, len - n,
        /* tms == -1 op een vast kanaal: daar valt geen pingtijd uit te zetten.
         * De velden staan er toch, zodat de pagina niet per regel hoeft te
         * kijken of ze bestaan -- dezelfde afspraak als bij de andere velden van
         * de vaste vier. "tb" is wel echt: die 4 en die 3 byte gaan van hetzelfde
         * budget af als een monitor. */
        "{\"ch\":1,\"n\":\"spanning\",\"h\":\"batterij\",\"i\":0,\"st\":\"%s\","
        "\"ms\":0,\"f\":0,\"c\":0,\"k\":\"vast\",\"age\":0,\"sev\":\"unk\","
        "\"si\":-1,\"sm\":\"off\",\"sl\":0,\"tms\":-1,\"tb\":4,\"drop\":0},"
        "{\"ch\":%u,\"n\":\"netvoeding\",\"h\":\"klemspanning\",\"i\":0,\"st\":\"%s\","
        "\"ms\":0,\"f\":0,\"c\":0,\"k\":\"vast\",\"age\":0,\"sev\":\"%s\","
        "\"si\":%u,\"sm\":\"%s\",\"sl\":%lu,\"tms\":-1,\"tb\":3,\"drop\":0},"
        "{\"ch\":%u,\"n\":\"batterijvoeding\",\"h\":\"klemspanning\",\"i\":0,\"st\":\"%s\","
        "\"ms\":0,\"f\":0,\"c\":0,\"k\":\"vast\",\"age\":0,\"sev\":\"%s\","
        "\"si\":%u,\"sm\":\"%s\",\"sl\":%lu,\"tms\":-1,\"tb\":3,\"drop\":0},"
        "{\"ch\":%u,\"n\":\"wifi\",\"h\":\"deze node\",\"i\":0,\"st\":\"%s\","
        "\"ms\":0,\"f\":0,\"c\":0,\"k\":\"vast\",\"age\":0,\"sev\":\"%s\","
        "\"si\":%u,\"sm\":\"%s\",\"sl\":%lu,\"tms\":-1,\"tb\":3,\"drop\":0}",
        volts,
        (unsigned)MonitorSensors::CH_MAINS,   _mon->isMains() ? "aan" : "uit", pw_sev,
        (unsigned)MonitorSensors::SIM_POWER, simModeName(pw_sim),
        (unsigned long)_mon->simSecsLeft(MonitorSensors::SIM_POWER),
        (unsigned)MonitorSensors::CH_BATTERY, _mon->isMains() ? "uit" : "aan", pw_sev,
        (unsigned)MonitorSensors::SIM_POWER, simModeName(pw_sim),
        (unsigned long)_mon->simSecsLeft(MonitorSensors::SIM_POWER),
        (unsigned)MonitorSensors::CH_WIFI,    _mon->isWifiOnline() ? "online" : "weg",
        _mon->isWifiOnline() ? "ok" : "bad",
        (unsigned)MonitorSensors::SIM_WIFI, simModeName(wf_sim),
        (unsigned long)_mon->simSecsLeft(MonitorSensors::SIM_WIFI));

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
      const uint8_t sidx = (uint8_t)MonitorSensors::simIndexOfSlot(i);
      const MonitorSensors::SimMode sm = _mon->simMode(sidx);

      const char* st;
      const char* sev;
      /* EEN GEFORCEERD VAKJE GAAT VOOR OP DE PAUZE, en om dezelfde reden als in
       * monitorAlert(): "pauze" is een uitspraak over een MEETPOGING, en er wordt
       * hier niet gemeten maar beweerd. Zou de pauze voorgaan, dan zou een
       * simulatie op een node met wifi-problemen als "pauze" op de pagina staan
       * terwijl er wel degelijk een waarschuwing de deur uit is -- de tabel zou
       * dan iets anders zeggen dan het bericht op de telefoon. */
      if (sm != MonitorSensors::SIM_OFF) {
        st  = (sm == MonitorSensors::SIM_UP) ? "op" : "neer";
        sev = (sm == MonitorSensors::SIM_UP) ? "ok" : "bad";
      }
      else if (paused)                  { st = "pauze"; sev = "warn"; }
      else if (_mon->monitorIsStale(i)) { st = "stil";  sev = "warn"; }
      else if (!_mon->monitorSeeded(i)) { st = "?";     sev = "unk";  }
      else if (_mon->monitorIsUp(i))    { st = "op";    sev = "ok";   }
      else                              { st = "neer";  sev = "bad";  }

      n += snprintf(buf + n, len - n,
          ",{\"ch\":%u,\"n\":\"%s\",\"h\":\"%s\",\"i\":%u,\"st\":\"%s\","
          "\"ms\":%lu,\"f\":%lu,\"c\":%lu,\"k\":\"%s\",\"age\":%lu,\"sev\":\"%s\","
          "\"si\":%u,\"sm\":\"%s\",\"sl\":%lu,\"tms\":%d,\"tb\":%u,\"drop\":%d}",
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
          sev,
          (unsigned)sidx, simModeName(sm),
          (unsigned long)_mon->simSecsLeft(sidx),
          /* tms = gaat de pingtijd de ether in, tb = wat dit vakje in het pakket
           * kost, drop = viel hij bij de laatste uitlezing BUITEN het pakket.
           * Dat laatste is het veld waar het om gaat: een monitor die stil uit de
           * telemetrie verdwijnt, is de fout die dit project al twee keer gekost
           * heeft. Nu staat hij in het antwoord en dus op de pagina. */
          _mon->monitorSendsMs(i) ? 1 : 0,
          (unsigned)_mon->monitorTelemBytes(i),
          _mon->monitorDropped(i) ? 1 : 0);
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

/* ======================= simuleren en testen ==========================
 *
 * Waarom dit bestaat staat in MonitorSensors.h bij SIMULEREN. Kort: de
 * waarschuwingen zijn gebouwd maar nog nooit afgegaan, en een node waarvan
 * niemand weet of zijn bericht aankomt, ontdekt dat op het slechtste moment.
 *
 * Wat hier NIET staat is een tweede verzendweg. Een forcering verandert wat de
 * sensorlaag TERUGGEEFT, en alertIf() in main.cpp doet daarna gewoon zijn werk --
 * echt pakket, echte contactkeuze, echte ACK's. Zou hier een nepbericht
 * samengesteld worden, dan testte deze pagina zichzelf.
 */

/* Antwoord op een SimResult. 400 voor verkeerde invoer, 409 voor "er loopt al
 * iets" en 429 voor de rem -- die code betekent letterlijk "te veel verzoeken",
 * en dat is precies wat de rem tegenhoudt. Een script dat hierop een 429 ziet
 * weet dat het later moet terugkomen; op een 400 zou het opnieuw dezelfde fout
 * maken. */
static int httpCodeFor(MonitorSensors::SimResult r) {
  switch (r) {
    case MonitorSensors::SIM_OK:        return 200;
    case MonitorSensors::SIM_ERR_BUSY:  return 409;
    case MonitorSensors::SIM_ERR_FULL:
    case MonitorSensors::SIM_ERR_GAP:   return 429;
    default:                            return 400;
  }
}

uint8_t WebTask::countAlertRecipients() const {
  if (_acl == nullptr) return 0;
  uint8_t n = 0;
  const int cnt = _acl->getAclCount();
  for (int i = 0; i < cnt; i++) {
    ClientInfo* c = _acl->getAclEntry(i);
    /* PERM_RECV_ALERTS_LO en niet _HI: monitorwaarschuwingen en het testbericht
     * gaan als LOW_PRI_ALERT de deur uit (zie main.cpp), en alertIf() kiest zijn
     * contacten op precies dit bit. Wie hier op _HI zou tellen, zou een getal
     * tonen dat niets met de aflevering te maken heeft. */
    if (c != nullptr && (c->permissions & PERM_RECV_ALERTS_LO)) n++;
  }
  return n;
}

/* POST /sim  (i=<sensornummer>, m=off|up|down, secs=<30..3600>)
 *
 * Eén sensor forceren of vrijgeven. De vervaltijd is verplicht in de zin dat er
 * altijd één is: geen waarde betekent SIM_SECS_DEFAULT, en er bestaat geen stand
 * "voor altijd". Zie de header van MonitorSensors: een forcering die blijft
 * staan zet een monitor stil uit, en dat is erger dan geen testknop hebben.
 */
void WebTask::handleSim() {
  if (!requireAuth()) return;

  if (_mon == nullptr) {
    _server->send(503, "text/plain", "sensorlaag niet gekoppeld");
    return;
  }

  unsigned long idx = 0;
  if (!getUInt(*_server, "i", 0, MonitorSensors::SIM_COUNT - 1, 0, &idx)) {
    _server->send(400, "text/plain",
                  MonitorSensors::simResultText(MonitorSensors::SIM_ERR_INDEX));
    return;
  }

  char mode[8];
  getArg(*_server, "m", mode, sizeof(mode));
  MonitorSensors::SimMode m;
  if (strcmp(mode, "up") == 0)        m = MonitorSensors::SIM_UP;
  else if (strcmp(mode, "down") == 0) m = MonitorSensors::SIM_DOWN;
  else if (strcmp(mode, "off") == 0)  m = MonitorSensors::SIM_OFF;
  else {
    _server->send(400, "text/plain", "m: up, down of off\n");
    return;
  }

  unsigned long secs = 0;
  if (!getUInt(*_server, "secs", MonitorSensors::SIM_SECS_MIN,
               MonitorSensors::SIM_SECS_MAX,
               MonitorSensors::SIM_SECS_DEFAULT, &secs)) {
    _server->send(400, "text/plain",
                  MonitorSensors::simResultText(MonitorSensors::SIM_ERR_SECS));
    return;
  }

  MonitorSensors::SimResult r = _mon->simSet((uint8_t)idx, m, (uint16_t)secs);
  if (r != MonitorSensors::SIM_OK) {
    _server->send(httpCodeFor(r), "text/plain", MonitorSensors::simResultText(r));
    return;
  }

  char msg[160];
  if (m == MonitorSensors::SIM_OFF) {
    snprintf(msg, sizeof(msg), "sensor %lu terug op de meting\n", idx);
  } else {
    /* De vervaltijd staat IN het antwoord, en niet alleen op de pagina. Wie dit
     * met een script doet moet ook weten wanneer het van zichzelf ophoudt. */
    snprintf(msg, sizeof(msg),
             "sensor %lu geforceerd op '%s' voor %lus; daarna vanzelf terug naar "
             "de meting. De waarschuwing gaat bij de volgende leesronde uit "
             "(hoogstens 60s).\n",
             idx, m == MonitorSensors::SIM_UP ? "op" : "neer", secs);
  }
  _server->send(200, "text/plain", msg);
}

/* POST /sim/clear -- alles in één keer terug naar de meting.
 *
 * Deze knop is er omdat hij er moet zijn: wie twijfelt of er nog iets geforceerd
 * staat, hoort dat met één klik zeker te kunnen weten in plaats van tien regels
 * na te lopen. Er zit geen rem op en geen bevestiging: terug naar de waarheid is
 * de handeling die je nooit wil vertragen. */
void WebTask::handleSimClear() {
  if (!requireAuth()) return;

  if (_mon == nullptr) {
    _server->send(503, "text/plain", "sensorlaag niet gekoppeld");
    return;
  }

  const uint8_t was = _mon->simActiveCount();
  _mon->simClearAll();

  char msg[96];
  snprintf(msg, sizeof(msg), "%u forcering(en) opgeheven; alles staat weer op de "
                             "meting\n", (unsigned)was);
  _server->send(200, "text/plain", msg);
}

/* POST /alert/test -- één kort testbericht naar de alarmontvangers.
 *
 * VERSTUURD IS NIET AANGEKOMEN. Het antwoord zegt daarom naar hoeveel ontvangers
 * het gaat en niet dat het gelukt is; wat er van de bezorging bekend is komt
 * daarna in /status.json binnen, want ACK's hebben tijd nodig. Die twee dingen
 * door elkaar halen is precies waarom er op ACK's gelet wordt.
 */
void WebTask::handleAlertTest() {
  if (!requireAuth()) return;

  if (_mon == nullptr) {
    _server->send(503, "text/plain", "sensorlaag niet gekoppeld");
    return;
  }
  if (_acl == nullptr) {
    _server->send(503, "text/plain",
        "meshlaag niet gekoppeld: voeg in main.cpp setup() toe: "
        "web_task.setAcl(&the_mesh);\n");
    return;
  }

  const uint8_t rc = countAlertRecipients();

  /* GEEN ONTVANGERS IS EEN FOUT EN GEEN GELUKTE TEST. Zonder deze tak zou de
   * knop "verstuurd naar 0 ontvangers" melden en dat leest als succes -- terwijl
   * het antwoord op de vraag "komen mijn waarschuwingen aan" dan nee is, en de
   * oorzaak precies hier zichtbaar was. De zendtijd wordt ook niet verbruikt. */
  if (rc == 0) {
    _server->send(412, "text/plain",
        "geen enkele ingang heeft het alarmrecht, dus een waarschuwing van deze "
        "node komt nergens aan. Zet op het tabblad 'toegang' het vinkje 'alarm' "
        "aan bij minstens een sleutel.\n");
    return;
  }

  MonitorSensors::SimResult r = _mon->testRequest(rc);
  if (r != MonitorSensors::SIM_OK) {
    char msg[160];
    snprintf(msg, sizeof(msg), "%s (nog %lus te wachten)\n",
             MonitorSensors::simResultText(r),
             (unsigned long)_mon->testWaitSecs());
    _server->send(httpCodeFor(r), "text/plain", msg);
    return;
  }

  char msg[200];
  snprintf(msg, sizeof(msg),
           "testbericht #%u aangevraagd voor %u ontvanger(s). Hij gaat de deur "
           "uit bij de volgende leesronde (hoogstens 60s) -- dat is geen "
           "vertraging maar hetzelfde pad als een echte waarschuwing. Bevestigde "
           "aflevering komt hieronder te staan.\n",
           (unsigned)_mon->testSeq(), (unsigned)rc);
  _server->send(200, "text/plain", msg);
}

/* ----------------------------- toegangsbeheer ----------------------------- */

/* De naam gaat ontsnapt EN AFGEKAPT in het antwoord. Ontsnapt omdat een naam uit
 * een advert komt en dus alles kan bevatten -- een enkel aanhalingsteken in een
 * nodenaam zou het hele document ongeldig maken, en dan blijft de pagina leeg
 * zonder dat er iets in de logs staat. Afgekapt omdat de maat van g_acl anders
 * niet na te rekenen is: jsonEscape() kan een teken tot zes tekens maken. */
#define NB_JSON_NAME  48

/* Een publieke sleutel uit een tekstveld. Geeft het aantal BYTES terug, of 0 bij
 * afkeuring.
 *
 * DE KEURING IS STREKER DAN mesh::Utils::fromHex, met reden: die controleert
 * alleen de LENGTE en rekent een niet-hexteken stil om naar nul (zie hexVal in
 * src/Utils.cpp). Een sleutel met een typefout zou dus stil een ANDERE sleutel
 * worden -- en dan staat er een ingang in de lijst die op niemand past en waarvan
 * niemand begrijpt waarom hij niets doet. Utils::isHexChar is wel de zeef van
 * upstream; die gebruiken we per teken.
 *
 * Een oneven aantal tekens is een fout en geen halve byte: iemand die 63 tekens
 * plakt heeft één teken verloren, en dan is stil afronden het verkeerde antwoord.
 */
static int parseHexKey(const char* hex, uint8_t* out, int out_max) {
  int len = strlen(hex);
  if (len == 0 || (len & 1) || len > out_max * 2) return 0;
  for (int i = 0; i < len; i++) {
    if (!mesh::Utils::isHexChar(hex[i])) return 0;
  }
  if (!mesh::Utils::fromHex(out, len / 2, hex)) return 0;
  return len / 2;
}

/* Ouderdom in seconden, of -1 voor "nooit".
 *
 * last_activity in ClientInfo is VLUCHTIG -- ClientACL::load() zet hem niet
 * terug, want hij staat niet in /s_contacts. Na een herstart is hij dus 0 voor
 * elke ingang, en dat is precies "sinds de herstart niets gedaan". Dat als "50
 * jaar geleden" tonen zou een leugen zijn; -1 wordt op de pagina "nooit". */
static long ageOf(uint32_t stamp, uint32_t now) {
  if (stamp == 0) return -1;
  if (now <= stamp) return 0;   // klok is verzet, of net gebeurd
  return (long)(now - stamp);
}

void WebTask::handleAclJson() {
  if (!requireAuth()) return;

  if (_acl == nullptr) {
    _server->send(503, "text/plain",
        "meshlaag niet gekoppeld: voeg in main.cpp setup() toe: "
        "web_task.setAcl(&the_mesh);\n");
    return;
  }

  const NeighbourList& nb = _acl->getNeighbours();
  /* De klok van de MESH en niet millis(): last_activity en heard_at staan in
   * RTC-seconden, en die twee mag je niet met elkaar verrekenen. */
  const uint32_t now = _acl->getRTCClock()->getCurrentTime();

  int n = snprintf(g_acl, sizeof(g_acl),
      "{\"strict\":%d,\"max\":%d,\"nbmax\":%d,\"acl\":[",
      _acl->getAclStrict() ? 1 : 0, MAX_CLIENTS, MAX_NEIGHBOURS);

  char key[PUB_KEY_SIZE*2 + 1];
  char esc[NB_JSON_NAME];
  bool first = true;

  for (int i = 0; i < _acl->getAclCount(); i++) {
    ClientInfo* c = _acl->getAclEntry(i);
    if (c->permissions == 0) continue;   // verwijderde ingang
    if ((size_t)n > sizeof(g_acl) - ACL_TAIL) break;

    /* toHex() schrijft 2 tekens per byte PLUS een afsluitende nul, dus precies
     * de 65 byte van 'key'. */
    mesh::Utils::toHex(key, c->id.pub_key, PUB_KEY_SIZE);

    /* De NAAM komt uit de buurtlijst, want ClientInfo draagt er geen. Dat is
     * geen gemak: zonder naam is deze tabel een rij van 64-tekenige hexsleutels
     * en dan kiest niemand de juiste. Staat de node niet (meer) in de buurt,
     * dan blijft de naam leeg -- en dat is de waarheid: wij weten hem niet. */
    const NeighbourEntry* e = nb.find(c->id.pub_key, PUB_KEY_SIZE);
    jsonEscape(e != NULL ? e->name : "", esc, sizeof(esc));

    n += snprintf(g_acl + n, sizeof(g_acl) - n,
        "%s{\"k\":\"%s\",\"n\":\"%s\",\"p\":%u,\"a\":%ld}",
        first ? "" : ",", key, esc, (unsigned)c->permissions,
        ageOf(c->last_activity, now));
    first = false;
  }

  n += snprintf(g_acl + n, sizeof(g_acl) - n, "],\"nb\":[");
  first = true;

  for (int i = 0; i < nb.getNumEntries(); i++) {
    const NeighbourEntry* e = nb.getEntryByIdx(i);
    if ((size_t)n > sizeof(g_acl) - ACL_TAIL) break;

    mesh::Utils::toHex(key, e->pub_key, PUB_KEY_SIZE);
    jsonEscape(e->name, esc, sizeof(esc));

    /* "in": staat deze buur al in de toegangslijst? De pagina kan dat niet zelf
     * bepalen zonder beide lijsten te vergelijken, en dan zou zij de knop
     * "leesrecht geven" aanbieden voor een node die het al heeft. */
    n += snprintf(g_acl + n, sizeof(g_acl) - n,
        "%s{\"k\":\"%s\",\"n\":\"%s\",\"t\":%u,\"h\":%u,\"s\":\"%.1f\","
        "\"a\":%ld,\"c\":%lu,\"in\":%d}",
        first ? "" : ",", key, esc, (unsigned)e->adv_type, (unsigned)e->hops,
        ((float)e->snr4) / 4.0f,
        ageOf(e->heard_at, now), (unsigned long)e->count,
        _acl->aclCountMatching(e->pub_key, PUB_KEY_SIZE) > 0 ? 1 : 0);
    first = false;
  }

  strlcat(g_acl, "]}", sizeof(g_acl));

  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_acl);
}

/* POST /acl  (key, rd, alerts, admin)
 *
 * DE VOLLEDIGE SLEUTEL IS HIER VERPLICHT, en dat is rekenkunde en geen
 * strengheid. De gedeelde sleutel waarmee deze node met de tegenpartij praat
 * wordt uit de VOLLE publieke sleutel berekend (calcSharedSecret); uit een
 * prefix valt hij niet te berekenen, dus een ingang op een prefix zou een ingang
 * zijn waarmee niet te praten is. ClientACL::applyPermissions weigert een
 * gedeeltelijke sleutel om precies die reden ook zelf.
 *
 * En er is een tweede reden, die ook zou gelden als het rekenkundig wél kon: een
 * prefix is te vervalsen. Wie sleutelparen blijft aanmaken tot er een is waarvan
 * de eerste zes byte overeenkomen met de prefix in jouw lijst, ERFT het recht dat
 * jij aan iemand anders gaf. Zes byte is daarvoor niet genoeg werk. Bij
 * VERWIJDEREN mag een prefix wel -- daar is de fout omkeerbaar en daar weigeren
 * we bovendien als de prefix op meer dan één ingang past.
 */
void WebTask::handleAclSet() {
  if (!requireAuth()) return;

  if (_acl == nullptr) {
    _server->send(503, "text/plain", "meshlaag niet gekoppeld");
    return;
  }

  char hex[PUB_KEY_SIZE*2 + 8];   // ruim, zodat te lang wordt AFGEKEURD en niet stil afgekapt
  if (!getArg(*_server, "key", hex, sizeof(hex)) || hex[0] == 0) {
    _server->send(400, "text/plain", "sleutel ontbreekt\n");
    return;
  }

  uint8_t pubkey[PUB_KEY_SIZE];
  int key_len = parseHexKey(hex, pubkey, PUB_KEY_SIZE);
  if (key_len == 0) {
    _server->send(400, "text/plain",
        "sleutel: alleen hextekens, en een even aantal (hoogstens 64)\n");
    return;
  }
  if (key_len != PUB_KEY_SIZE) {
    char msg[128];
    snprintf(msg, sizeof(msg),
        "sleutel: %d van de %d byte; toevoegen vraagt de VOLLEDIGE sleutel "
        "(64 hextekens)\n", key_len, PUB_KEY_SIZE);
    _server->send(400, "text/plain", msg);
    return;
  }

  /* De rol is een getal en geen verzameling vinkjes: PERM_ACL_ROLE_MASK zijn
   * twee bits met vier standen. Beheerder mag alles wat lezen mag, dus is
   * 'admin' zonder 'rd' geen tegenstrijdigheid maar gewoon de hoogste rol. */
  char v[4];
  const bool rd     = getArg(*_server, "rd", v, sizeof(v))     && v[0] == '1';
  const bool admin  = getArg(*_server, "admin", v, sizeof(v))  && v[0] == '1';
  const bool alerts = getArg(*_server, "alerts", v, sizeof(v)) && v[0] == '1';

  uint8_t perms = admin ? PERM_ACL_ADMIN : (rd ? PERM_ACL_READ_ONLY : PERM_ACL_GUEST);
  if (alerts) perms |= (PERM_RECV_ALERTS_LO | PERM_RECV_ALERTS_HI);

  if (perms == 0) {
    /* Geen enkel recht is geen ingang. Dat zou aclSetPerms() als verwijderen
     * uitvoeren, en stil iets weggooien terwijl iemand "opslaan" bedoelde is
     * precies het soort verrassing dat hier niet hoort. */
    _server->send(400, "text/plain",
        "geen enkel recht aangevinkt; gebruik 'wis' om een ingang te "
        "verwijderen\n");
    return;
  }

  if (!_acl->aclSetPerms(pubkey, perms)) {
    _server->send(503, "text/plain", "toegangslijst vol of sleutel geweigerd\n");
    return;
  }

  char msg[96];
  snprintf(msg, sizeof(msg), "ok rechten %02X gezet\n", (unsigned)perms);
  _server->send(200, "text/plain", msg);
}

/* POST /acl/del  (key)
 *
 * Een prefix mag hier, vanaf 6 byte (12 hextekens). Dat is de lengte die MeshCore
 * zelf over het mesh teruggeeft bij REQ_TYPE_GET_ACCESS_LIST, dus het is de
 * lengte waarmee iemand hier redelijkerwijs aankomt. EEN PREFIX IS MINDER VEILIG
 * dan een volle sleutel -- twee sleutels kunnen op hun eerste zes byte
 * overeenkomen -- en daarom weigert aclRemove() zodra de prefix op meer dan één
 * ingang past. Verwijderen is bovendien de omkeerbare kant: opnieuw toevoegen kan,
 * en dat vraagt dan wél de volle sleutel.
 */
void WebTask::handleAclDel() {
  if (!requireAuth()) return;

  if (_acl == nullptr) {
    _server->send(503, "text/plain", "meshlaag niet gekoppeld");
    return;
  }

  char hex[PUB_KEY_SIZE*2 + 8];
  if (!getArg(*_server, "key", hex, sizeof(hex)) || hex[0] == 0) {
    _server->send(400, "text/plain", "sleutel ontbreekt\n");
    return;
  }

  uint8_t pubkey[PUB_KEY_SIZE];
  int key_len = parseHexKey(hex, pubkey, PUB_KEY_SIZE);
  if (key_len < 6) {
    _server->send(400, "text/plain",
        "sleutel: hextekens, even aantal, minstens 12 (6 byte)\n");
    return;
  }

  int matches = _acl->aclCountMatching(pubkey, key_len);
  if (matches == 0) {
    _server->send(404, "text/plain", "geen ingang met die sleutel\n");
    return;
  }
  if (matches > 1) {
    char msg[112];
    snprintf(msg, sizeof(msg),
        "%d ingangen beginnen met deze %d byte; geef meer tekens\n",
        matches, key_len);
    _server->send(409, "text/plain", msg);
    return;
  }

  if (!_acl->aclRemove(pubkey, key_len)) {
    _server->send(500, "text/plain", "verwijderen mislukt\n");
    return;
  }
  _server->send(200, "text/plain", "ok ingang verwijderd\n");
}

/* POST /acl/strict  (on=0|1)
 *
 * Het slot zelf. Het antwoord zegt met zoveel woorden wat de nieuwe stand
 * betekent, want dit is de ene knop op deze pagina waarvan de gevolgen niet in
 * de tabel eronder te zien zijn.
 */
void WebTask::handleAclStrict() {
  if (!requireAuth()) return;

  if (_acl == nullptr) {
    _server->send(503, "text/plain", "meshlaag niet gekoppeld");
    return;
  }

  char on[4];
  if (!getArg(*_server, "on", on, sizeof(on)) || (on[0] != '0' && on[0] != '1')) {
    _server->send(400, "text/plain", "on: 0 of 1\n");
    return;
  }

  _acl->setAclStrict(on[0] == '1');

  if (on[0] == '1') {
    /* Het aantal ingangen MET leesrecht erbij, want dat is het getal waarop het
     * misgaat: het slot dichtzetten met een lege lijst betekent dat er niemand
     * meer kan uitlezen, en dat merk je pas als je het probeert. */
    int readers = 0;
    for (int i = 0; i < _acl->getAclCount(); i++) {
      ClientInfo* c = _acl->getAclEntry(i);
      if (c->permissions == 0) continue;
      if ((c->permissions & PERM_ACL_ROLE_MASK) >= PERM_ACL_READ_ONLY) readers++;
    }
    char msg[128];
    snprintf(msg, sizeof(msg),
        "ok slot AAN; %d node(s) met leesrecht, de rest krijgt geen antwoord\n",
        readers);
    _server->send(200, "text/plain", msg);
  } else {
    _server->send(200, "text/plain",
        "ok slot UIT; elke node op het mesh mag de sensoren uitlezen\n");
  }
}

/* ================================ nodebeheer ==============================
 *
 * WAAROM DIT EEN CONSOLE IS EN GEEN VERZAMELING ROUTES.
 *
 * Deze node is niet met de MeshCore-companion-app te beheren: er zit geen BLE op
 * en deze firmware spreekt het companion-clientprotocol niet. Wat zij WEL heeft is
 * haar CLI, en dat is dezelfde laag waarmee die app een repeater beheert. Eén
 * route die een opdrachtregel doorgeeft levert daarom in één keer ALLES wat de app
 * kan -- en, belangrijker, precies wat de seriële console kan, met dezelfde
 * keuring en dezelfde grenzen. Een tweede schrijfpad met eigen keuring erlangs
 * bouwen zou een tweede waarheid zijn, en twee waarheden lopen uiteen.
 *
 * De knoppen en formulieren op de pagina stellen niets anders samen dan zo'n
 * regel. Dat is geen omweg maar het punt: wat het formulier doet, is na te lezen
 * in de console eronder, en wie het formulier niet vertrouwt typt het zelf.
 *
 * sender_timestamp = 0, net als de seriële console in main.cpp. CommonCLI leest
 * dat als "de console" en niet als "iemand op afstand", en laat er een handvol
 * opdrachten op door die over het mesh geweigerd worden (set freq, erase, get acl,
 * log, stats-*). Dat is hier de goede kant van de streep: deze webinterface hangt
 * aan hetzelfde eigen netwerk als de USB-kabel. Wij zetten zelf een streep op een
 * kleiner rijtje, hieronder, en met de reden in het antwoord.
 */

#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD "password"
#endif

/* De ontsnapte teksten voor cfg.json, STATISCH en niet op de stapel. Deze handler
 * loopt in dezelfde taak als the_mesh.loop(); daar hoort geen driekwart kilobyte
 * aan buffers op de stapel, en op dit project is eerder al een stapeloverloop in
 * upstream opgeruimd. Eén taak leest ze en één handler schrijft ze, dus statisch
 * is hier ook veilig.
 *
 * DE MAAT IS CFG_TXT_MAX EN DE AFKAPPING GEBEURT IN jsonEscape ZELF. Die lus stopt
 * met schrijven zolang er minder dan zeven byte over is, dus hij kapt nooit midden
 * in een \u00xx-reeks af en laat nooit een losse backslash achter. Zelf achteraf
 * op maat afkappen zou precies dat wel doen, en dan is het hele document ongeldig
 * en blijft het tabblad leeg zonder dat er iets in de logs staat. */
static char g_cfg_name[CFG_TXT_MAX];
static char g_cfg_owner[CFG_TXT_MAX];
static char g_cfg_raw[128];       /* owner_info (120 byte) met '\n' -> '|' */

/* Prefix-vergelijking, en met opzet dezelfde losheid als CommonCLI zelf: dat doet
 * memcmp op een vaste lengte, dus "rebootnu" herstart ook. Een strengere zeef hier
 * zou opdrachten weigeren die de node wél zou uitvoeren -- en dan weigert de ene
 * kant wat de andere doet. */
static bool cmdIs(const char* cmd, const char* prefix) {
  return memcmp(cmd, prefix, strlen(prefix)) == 0;
}

/* Een getal netjes, met achterste nullen eraf: 869.618 en niet 869.618000, 62.5 en
 * niet 62.500.
 *
 * DIT IS GEEN COSMETICA. De pagina vergelijkt de huidige waarde als TEKST met de
 * gebakken waarde om te laten zien waarvan deze node afwijkt. Zou de ene kant
 * "62.5" schrijven en de andere "62.500", dan meldt de pagina een afwijking die er
 * niet is -- en een waarschuwing die vals is, maakt de volgende ook niets meer
 * waard. Beide kanten gaan daarom door deze ene functie.
 */
static void fmtNum(char* out, size_t len, double v, int dec) {
  snprintf(out, len, "%.*f", dec, v);
  char* dot = strchr(out, '.');
  if (dot == NULL) return;
  char* e = out + strlen(out);
  while (e > dot && e[-1] == '0') *--e = 0;
  if (e > out && e[-1] == '.') *--e = 0;
}

static const char* locPolicyName(uint8_t p) {
  switch (p) {
    case ADVERT_LOC_NONE:  return "none";
    case ADVERT_LOC_SHARE: return "share";
    case ADVERT_LOC_PREFS: return "prefs";
  }
  return "prefs";
}

static const char* loopDetectName(uint8_t l) {
  switch (l) {
    case LOOP_DETECT_OFF:      return "off";
    case LOOP_DETECT_MINIMAL:  return "minimal";
    case LOOP_DETECT_MODERATE: return "moderate";
  }
  return "strict";
}

/* GET /cfg.json -- de hele stand van NodePrefs in één antwoord.
 *
 * WAAROM NIET DERTIG KEER "get <veld>" OVER DE CLI. Dat zou het net zo goed doen
 * en het zou zelfs consequenter zijn, maar het zijn dertig HTTP-verzoeken op een
 * node die tussendoor een radio bedient, en elk verzoek gaat door dezelfde loop()
 * als het meshwerk. De LEESkant mag daarom rechtstreeks in NodePrefs kijken; de
 * SCHRIJFkant gaat wél over de CLI, want daar zit de keuring en het wegschrijven
 * naar flash. Lezen kan niets stukmaken, schrijven wel.
 *
 * Er verandert hier niets, dus dit is de enige beheerroute die een GET mag zijn.
 */
void WebTask::handleCfgJson() {
  if (!requireAuth()) return;

  if (_acl == nullptr) {
    _server->send(503, "text/plain",
        "meshlaag niet gekoppeld: voeg in main.cpp setup() toe: "
        "web_task.setAcl(&the_mesh);\n");
    return;
  }

  NodePrefs* p = _acl->getNodePrefs();

  /* Ontsnappen is hier niet netheid maar noodzaak: een nodenaam en een
   * eigenaarsregel mogen alles bevatten, en één aanhalingsteken maakt het hele
   * document ongeldig. */
  jsonEscape(p->node_name, g_cfg_name, sizeof(g_cfg_name));

  /* owner_info draagt echte regeleindes; de CLI toont en accepteert ze als '|'.
   * Hier dezelfde omzetting, zodat wat de pagina voorvult ook terug te sturen is.
   * Zonder die omzetting zou het formulier bij opslaan een opdracht met een
   * regeleinde erin sturen -- en dat is geen opdrachtREGEL meer. */
  {
    size_t i = 0;
    for (; p->owner_info[i] && i < sizeof(g_cfg_raw) - 1; i++) {
      g_cfg_raw[i] = (p->owner_info[i] == '\n') ? '|' : p->owner_info[i];
    }
    g_cfg_raw[i] = 0;
  }
  jsonEscape(g_cfg_raw, g_cfg_owner, sizeof(g_cfg_owner));

  char freq[16], bw[16], af[16], lat[20], lon[20];
  char rxd[16], txd[16], dtxd[16], adc[16], bfreq[16], bbw[16];
  fmtNum(freq, sizeof(freq), p->freq, 3);
  fmtNum(bw,   sizeof(bw),   p->bw,   3);
  fmtNum(af,   sizeof(af),   p->airtime_factor, 3);
  fmtNum(lat,  sizeof(lat),  p->node_lat, 6);
  fmtNum(lon,  sizeof(lon),  p->node_lon, 6);
  fmtNum(rxd,  sizeof(rxd),  p->rx_delay_base, 3);
  fmtNum(txd,  sizeof(txd),  p->tx_delay_factor, 3);
  fmtNum(dtxd, sizeof(dtxd), p->direct_tx_delay_factor, 3);
  fmtNum(adc,  sizeof(adc),  p->adc_multiplier, 3);
  /* Door DEZELFDE functie als de huidige waarden -- zie de noot bij fmtNum(). */
  fmtNum(bfreq, sizeof(bfreq), (double)LORA_FREQ, 3);
  fmtNum(bbw,   sizeof(bbw),   (double)LORA_BW,   3);

  char pubkey[PUB_KEY_SIZE*2 + 2];
  mesh::Utils::toHex(pubkey, _acl->getSelfId().pub_key, PUB_KEY_SIZE);

  /* HET KANAALBUDGET. g_ever_mask krijgt de kanalen die NU in gebruik zijn erbij:
   * die zijn per definitie vergeven, en zo valt het gat dicht tussen een net
   * aangemaakte monitor en de flashschrijving die twee seconden later komt. Bits
   * gaan er alleen BIJ, nooit af -- net als bij ch_ever_used zelf. */
  int mon_used = 0;
  if (_mon != nullptr) {
    mon_used = (int)_mon->getNumMonitors();
    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) {
      uint8_t ch = _mon->monitorChannel(i);
      if (ch >= MonitorSensors::CH_MONITOR_FIRST && ch <= MonitorSensors::CH_MONITOR_LAST) {
        g_ever_mask |= ((uint32_t)1 << (ch - MonitorSensors::CH_MONITOR_FIRST));
      }
    }
  }
  int ever = 0;
  for (int b = 0; b < MonitorSensors::MAX_MONITORS; b++) {
    /* (uint32_t)1 en niet 1: met 32 kanalen is bit 31 de laatste, en 1 << 31 op
     * een int is ongedefinieerd gedrag. Dat compileert zonder klacht en levert
     * daarna stil een verkeerde telling op. */
    if (g_ever_mask & ((uint32_t)1 << b)) ever++;
  }

  /* Twee aparte vragen en niet één. "Leeg" en "nog de gebakken waarde" zijn twee
   * verschillende fouten met dezelfde uitkomst, en de pagina zegt ze ook
   * verschillend -- want wie een leeg wachtwoord ziet staan denkt aan een fout in
   * het opslaan, en wie "password" ziet staan denkt aan zichzelf. */
  const bool pw_empty = (p->password[0] == 0);
  const bool pw_def   = (strcmp(p->password, ADMIN_PASSWORD) == 0);

  int n = snprintf(g_cfg, sizeof(g_cfg),
      "{\"name\":\"%s\",\"owner\":\"%s\",\"pubkey\":\"%s\",\"role\":\"%s\","
      "\"freq\":\"%s\",\"bw\":\"%s\",\"sf\":%u,\"cr\":%u,"
      "\"tx\":%d,\"af\":\"%s\",\"agc\":%u,\"rxgain\":\"%s\",\"femrx\":\"%s\","
      "\"lat\":\"%s\",\"lon\":\"%s\",\"advint\":%u,\"fadvint\":%u,"
      "\"advloc\":\"%s\","
      "\"repeat\":\"%s\",\"fmax\":%u,\"fmaxuns\":%u,\"fmaxadv\":%u,"
      "\"loopd\":\"%s\",\"rxdelay\":\"%s\",\"txdelay\":\"%s\","
      "\"dtxdelay\":\"%s\",\"multiack\":%u,\"hashmode\":%u,"
      "\"cad\":\"%s\",\"intthr\":%u,\"rdonly\":\"%s\",\"adcmult\":\"%s\","
      "\"pwdef\":%d,\"pwempty\":%d,"
      "\"mon_used\":%d,\"mon_max\":%u,\"ch_first\":%u,\"ch_last\":%u,"
      "\"ch_ever\":%d,\"ch_free\":%d,"
      "\"baked\":{\"freq\":\"%s\",\"bw\":\"%s\",\"sf\":%u,\"cr\":%u}}",
      g_cfg_name, g_cfg_owner, pubkey, _acl->getRole(),
      freq, bw, (unsigned)p->sf, (unsigned)p->cr,
      (int)p->tx_power_dbm, af, (unsigned)p->agc_reset_interval * 4,
      p->rx_boosted_gain ? "on" : "off",
      p->radio_fem_rxgain ? "on" : "off",
      lat, lon,
      (unsigned)p->advert_interval * 2, (unsigned)p->flood_advert_interval,
      locPolicyName(p->advert_loc_policy),
      p->disable_fwd ? "off" : "on",
      (unsigned)p->flood_max, (unsigned)p->flood_max_unscoped,
      (unsigned)p->flood_max_advert,
      loopDetectName(p->loop_detect), rxd, txd, dtxd,
      (unsigned)p->multi_acks, (unsigned)p->path_hash_mode,
      p->cad_enabled ? "on" : "off", (unsigned)p->interference_threshold,
      p->allow_read_only ? "on" : "off", adc,
      pw_def ? 1 : 0, pw_empty ? 1 : 0,
      mon_used, (unsigned)MonitorSensors::MAX_MONITORS,
      (unsigned)MonitorSensors::CH_MONITOR_FIRST,
      (unsigned)MonitorSensors::CH_MONITOR_LAST,
      ever, (int)MonitorSensors::MAX_MONITORS - ever,
      bfreq, bbw, (unsigned)LORA_SF, (unsigned)LORA_CR);

  if (n < 0 || (size_t)n >= sizeof(g_cfg)) {   /* kan niet; vangnet */
    _server->send(500, "text/plain", "antwoord te groot");
    return;
  }

  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_cfg);
}

/* POST /cli   cmd=<opdracht>[&confirm=<teken>]
 *
 * DRIE OPDRACHTEN KOMEN HIER NIET DOOR, met de reden in het antwoord en niet stil:
 *
 *  - alles met prv.key. Dat is de PRIVESLEUTEL, en die gaat hier over HTTP zonder
 *    TLS met een wachtwoord in base64 ernaast. Wie hem meeleest IS voortaan deze
 *    node, op elke node die haar kent -- dat is niet iets wat je terugdraait door
 *    een instelling te wijzigen. Over serieel mag het wel; daar leest niemand mee.
 *  - start ota. Op ESP32 opent die een EIGEN accesspoint ("MeshCore-OTA") en een
 *    TWEEDE webserver op poort 80. Precies deze pagina valt daar onder weg, en dan
 *    is de weg waarlangs je hem startte weg. Aan de kabel is dat geen probleem.
 *  - poweroff en shutdown. Diepe slaap zonder wektijd: alleen een fysieke reset
 *    haalt de node daaruit, en dat kan niemand van hier. Een knop die het apparaat
 *    onbereikbaar maakt hoort niet op een pagina die je alleen via dat apparaat
 *    bereikt.
 *
 * En twee hebben een bevestiging in de POST nodig:
 *
 *  - set radio / set freq: confirm=radio. De pagina vraagt het al twee keer (een
 *    vinkje en een confirm met de oude naast de nieuwe waarde), maar dat is de
 *    BROWSER. Dit is de server, en dat is het slot dat een losse fetch, een
 *    bookmarklet of een voorgeladen link niet omzeilt.
 *  - erase: confirm=erase. Dat wist de monitorlijst, de toegangslijst, de
 *    wifi-instelling en het kanaalgeheugen in één keer.
 *
 * Wat hier verder langskomt gaat ONGEWIJZIGD naar handleCommand. Er wordt niets
 * herschreven, aangevuld of "verbeterd": wat je typt is wat de node krijgt, en dat
 * is de enige manier waarop de console eronder een eerlijke weergave is van wat er
 * gebeurd is.
 */
void WebTask::handleCli() {
  if (!requireAuth()) return;

  if (_acl == nullptr) {
    _server->send(503, "text/plain",
        "meshlaag niet gekoppeld: voeg in main.cpp setup() toe: "
        "web_task.setAcl(&the_mesh);\n");
    return;
  }

  if (!getArg(*_server, "cmd", g_cmd, sizeof(g_cmd)) || g_cmd[0] == 0) {
    _server->send(400, "text/plain", "geen opdracht\n");
    return;
  }

  /* Regeleindes eraf: een <input> geeft ze niet, maar een script of curl wel, en
   * handleCommand vergelijkt met memcmp -- "reboot\n" zou dan gewoon werken en
   * "get radio\n" zou een veldnaam met een regeleinde erin opzoeken. */
  for (char* q = g_cmd; *q; q++) if (*q == '\r' || *q == '\n') { *q = 0; break; }

  /* Voorloopspaties eraf vóór onze eigen zeef. handleCommand doet dat zelf ook,
   * maar dan zou "  erase" onze zeef langslopen en zijn zeef wél raken -- en dan
   * weigert deze route iets dat de node alsnog uitvoert. */
  char* cmd = g_cmd;
  while (*cmd == ' ') cmd++;
  if (*cmd == 0) {
    _server->send(400, "text/plain", "geen opdracht\n");
    return;
  }

  /* Dezelfde grens als de seriële console in main.cpp. Wat daar past hoort hier te
   * passen en omgekeerd, anders neemt het ene pad een opdracht aan die het andere
   * afkapt. */
  if (strlen(cmd) > CLI_CMD_MAX) {
    char msg[104];
    snprintf(msg, sizeof(msg),
        "opdracht te lang: %u tekens, hoogstens %d (net als de seriele "
        "console)\n", (unsigned)strlen(cmd), CLI_CMD_MAX);
    _server->send(400, "text/plain", msg);
    return;
  }

  char cf[12];
  if (!getArg(*_server, "confirm", cf, sizeof(cf))) cf[0] = 0;

  /* ------------------------------ de weigeringen -------------------------- */

  if (strstr(cmd, "prv.key") != NULL) {
    _server->send(403, "text/plain",
        "geweigerd: de privesleutel gaat hier over HTTP zonder TLS, met het "
        "wachtwoord in base64 ernaast. Wie hem meeleest IS voortaan deze node. "
        "Doe dit over de seriele console.\n");
    return;
  }
  if (cmdIs(cmd, "start ota")) {
    _server->send(403, "text/plain",
        "geweigerd: 'start ota' opent een eigen accesspoint en een TWEEDE "
        "webserver op poort 80 -- deze pagina valt daar onder weg. Doe dit over "
        "de seriele console.\n");
    return;
  }
  if (cmdIs(cmd, "poweroff") || cmdIs(cmd, "shutdown")) {
    _server->send(403, "text/plain",
        "geweigerd: diepe slaap zonder wektijd. Alleen een fysieke reset haalt de "
        "node daaruit, en dat kan niemand van hier. Gebruik 'reboot'.\n");
    return;
  }
  if (strcmp(cmd, "erase") == 0 && strcmp(cf, "erase") != 0) {
    _server->send(409, "text/plain",
        "geweigerd: 'erase' wist de monitorlijst, de toegangslijst, de "
        "wifi-instelling en het kanaalgeheugen. Stuur confirm=erase mee.\n");
    return;
  }
  if ((cmdIs(cmd, "set radio ") || cmdIs(cmd, "set freq "))
      && strcmp(cf, "radio") != 0) {
    _server->send(409, "text/plain",
        "geweigerd: freq/bw/sf/cr bepalen of deze node nog op hetzelfde mesh zit. "
        "Een verkeerd getal en niemand hoort hem nog over LoRa. Stuur "
        "confirm=radio mee, of gebruik het formulier op het beheertabblad -- dat "
        "zet de huidige en de gebakken waarde ernaast.\n");
    return;
  }

  /* DE BUFFERGRENS VAN 'sensor set'. CommonCLI doet daar strcpy(tmp, &command[11])
   * in een buffer van PRV_KEY_SIZE*2+4 = 68 byte. Dat is een ongebonden kopie in
   * upstream en dus onze rand om dicht te zetten: 59 laat marge en is ruim genoeg
   * voor het langste dat wij nodig hebben ("mon.12.host " plus een adres van 40
   * tekens is 52). Weigeren aan deze kant is beter dan de node in een panic laten
   * lopen op een waarde die wij zelf gestuurd hebben. */
  if (cmdIs(cmd, "sensor set ")) {
    size_t rest = strlen(cmd + 11);
    if (rest > 59) {
      char msg[112];
      snprintf(msg, sizeof(msg),
          "geweigerd: %u tekens achter 'sensor set', hoogstens 59 -- de "
          "opdrachtbuffer van CommonCLI is 68 byte\n", (unsigned)rest);
      _server->send(400, "text/plain", msg);
      return;
    }
  }

  /* ------------------------- de uitgestelde opdracht ---------------------- */

  /* reboot en clkreboot KOMEN NIET TERUG uit handleCommand. Ze hier uitvoeren zou
   * de verbinding afbreken vóór het antwoord verstuurd is, en dan weet niemand of
   * de node herstart is of is omgevallen -- op een bewakingsnode is dat precies
   * het verschil dat je wilt weten. Dus: eerst antwoorden, dan loop() een halve
   * seconde later de opdracht laten uitvoeren. Geen delay(), alleen een tijdstip. */
  if (cmdIs(cmd, "reboot") || cmdIs(cmd, "clkreboot")) {
    strlcpy(g_deferred, cmd, sizeof(g_deferred));
    g_deferred_at = millis() + 600;
    _server->send(200, "text/plain",
        "ok herstart aangevraagd; de node is over ongeveer 20 s weer bereikbaar. "
        "De gemeten toestanden beginnen daarna weer op '?'.\n");
    return;
  }

  /* ------------------------------- uitvoeren ------------------------------ */

  /* handleCommand SCHRIJFT IN de opdracht (hij hakt hem op de spaties in stukken
   * en kaatst een 'xx|'-prefix terug), vandaar een muteerbare buffer en geen
   * String.
   *
   * Dat dit kan blokkeren is bekend en aanvaard: bijna elke 'set' doet
   * savePrefs(), en een flashschrijving kost tientallen milliseconden. Dat is
   * precies wat de seriële console ook doet, en het is een handeling die iemand
   * met een klik aanvraagt -- niet iets dat per ronde gebeurt. De radio staat
   * daarmee even stil; de alternatieven (een wachtrij, een tweede taak) zouden
   * betekenen dat het antwoord niet meer bij de opdracht hoort, en dat is op een
   * console erger dan een paar gemiste milliseconden. */
  g_reply[0] = 0;
  _acl->handleCommand(0, cmd, g_reply);

  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "text/plain", g_reply);
}

/* De uitgestelde opdracht, uit loop(). Komt niet terug bij een herstart -- dat is
 * precies de bedoeling. */
void WebTask::runDeferred() {
  if (g_deferred_at == 0) return;
  /* Getekend verschil, zodat het ook klopt als millis() na 49 dagen overloopt. */
  if ((long)(millis() - g_deferred_at) < 0) return;

  g_deferred_at = 0;
  if (_acl == nullptr || g_deferred[0] == 0) { g_deferred[0] = 0; return; }

  /* Een eigen kopie, en g_deferred meteen leeg: handleCommand schrijft in de
   * opdracht, en mocht hij tóch terugkomen (een onbekende opdracht) dan mag hij
   * geen tweede keer uitgevoerd worden. */
  char cmd[sizeof(g_deferred)];
  strlcpy(cmd, g_deferred, sizeof(cmd));
  g_deferred[0] = 0;

  g_reply[0] = 0;
  _acl->handleCommand(0, cmd, g_reply);
}
