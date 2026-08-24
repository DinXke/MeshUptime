#include "WebTask.h"
#include "WifiTask.h"
#include "MonitorSensors.h"
#include "MonitorStore.h"
#include "SensorMesh.h"
#include "IWebNode.h"
#include "TimeFmt.h"

#include <WebServer.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <esp_heap_caps.h>
#include <esp_random.h>   /* esp_random() voor de sessietokens; hardware-RNG */

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
#define TIME_CFG_PATH   "/time.cfg"

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
static char g_json[14336];
#define JSON_TAIL  400      /* ruimte die altijd vrij blijft voor één regel + "]}" */

/* Dwingt de rekensom hierboven af bij het compileren in plaats van bij het eerste
 * stil afgekapte antwoord op een node in het veld -- net als de static_assert bij
 * g_acl. MON_MAX_MONITORS is met een bouwvlag te verhogen; dan moet deze buffer
 * mee. */
/* 820 voor de kopblokken: vaste velden ~300 + budget ~200 + ad-hoc ~300 (de
 * ping-uitslagtekst) + sim/rec/rep/test ~320 -- ruim afgerond. */
/* 360 per monitor: bovenop "am"/"rm"/"sn"/"msv" draagt een SNMP-monitor nu ook
 * "knd", "itp" en de OID (tot 80 tekens) -- ~110 byte extra in het duurste geval. */
static_assert(300 + 820 + 4 * 210 + MON_MAX_MONITORS * 360 + 130 + JSON_TAIL
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
/* WEERGAVE-plafond voor de buurt-/contactenlijst in JSON (zie de uitleg bij de
 * tweede definitie verderop). De lijst zelf is groot (MAX_NEIGHBOURS=200, voor
 * naamresolutie); /acl.json en /contacts.json tonen hoogstens NB_JSON_MAX. Hier
 * VÓÓR g_acl gedefinieerd omdat de static_assert eronder hem gebruikt. */
#ifndef NB_JSON_MAX
  #define NB_JSON_MAX  64
#endif

static char g_acl[16384];
#define ACL_TAIL  320       /* ruimte die altijd vrij blijft voor één regel + "]}" */

/* De rekensom hierboven is geen decoratie. MAX_CLIENTS (20) en MAX_NEIGHBOURS
 * (12) zijn met een bouwvlag te verhogen -- platformio.local.ini zet nu al
 * MAX_CONTACTS=32 -- en dan moet g_acl mee. Dit dwingt dat af bij het compileren
 * in plaats van bij het eerste stil afgekapte antwoord op een node in het veld. */
static_assert(80 + MAX_CLIENTS * 165 + NB_JSON_MAX * 185 + ACL_TAIL <= sizeof(g_acl),
              "g_acl te klein voor MAX_CLIENTS/NB_JSON_MAX -- zie de rekensom hierboven");

static char g_ssid_shown[33] = {0};   // alleen om te tonen; nooit het wachtwoord
/* NTP-server + POSIX-TZ, één keer bij begin() uit /time.cfg gelezen en bij /time
 * bijgewerkt -- zodat cfg.json ze zonder flash-lees per verzoek kan tonen. */
static char g_ntp_shown[48] = {0};
static char g_tz_shown[48]  = {0};

/* DE WEB-INLOGGEGEVENS DIE NU GELDEN.
 *
 * Bij begin() geladen uit /web.cfg; staat daar niets bruikbaars, dan de gebakken
 * WEB_USER/WEB_PASS. Zelfde "opgeslagen wint van gebakken" als bij de wifi-
 * instelling, en om dezelfde reden: een node die roteert of verhuist hoeft niet
 * opnieuw geflasht te worden. requireAuth() toetst HIERTEGEN, niet tegen de
 * macro's.
 *
 * Het wachtwoord staat in RAM omdat Basic-auth het bij ELK verzoek nodig heeft;
 * het wordt nooit in cfg.json gezet, nooit teruggestuurd en nooit in de console
 * getoond -- net als de wifi-pass. g_web_custom is de vlag "eigen credential
 * gezet": false betekent dat deze node nog op de gebakken, vlootbrede standaard
 * staat, en dat is een zwakte die de pagina zichtbaar hoort te maken. */
static char g_web_user[WEB_USER_LEN];
static char g_web_pass[WEB_PASS_LEN];
static bool g_web_custom = false;

/* ----------------------- de sessie-login voor mensen ---------------------- */

/* WAAROM EEN STATISCHE SESSIETABEL EN NIET EEN ONDERTEKENDE STATELESS TOKEN.
 *
 * De opdracht liet de keuze en vroeg om een beargumentering. Het werd een kleine
 * statische tabel in RAM met willekeurige tokens, en dat is hier de betere keuze:
 *
 *  - GEEN device-secret om te maken, te bewaren en te beschermen. Een HMAC-token
 *    is maar zo geheim als de sleutel op SPIFFS, en SPIFFS is op dit bord niet
 *    versleuteld. Een tabel heeft niets te lekken: het token IS de sessie.
 *  - GEEN afhankelijkheid van een kloktijd. Een stateless token draagt zijn
 *    vervaltijd als absolute tijd, en die moet je kunnen toetsen -- maar de RTC van
 *    deze node kan ongezet zijn (geen GPS, geen NTP gegarandeerd). De tabel toetst
 *    op millis(), een monotone teller die altijd klopt; overloop na ~49 dagen wordt
 *    met een SIGNED verschil opgevangen.
 *  - EEN HERSTART WIST DE SESSIES, en dat is voor een terugval-beheertoegang een
 *    kenmerk en geen gebrek: na een reboot log je opnieuw in. Een gestolen cookie
 *    overleeft de reboot niet.
 *  - LEAN. Vier sessies kosten hieronder ~150 byte RAM, geen heap, geen crypto-
 *    bibliotheek in flash. Dat past bij een node waarvan het echte beheer over
 *    LoRa-DM's loopt en het web de terugval is.
 *
 * De prijs -- de tabel is er maar EEN (deze node) en de sessies leven niet over
 * herstarts -- is precies wat je voor een terugval-login wil.
 *
 * VEILIGHEID, EERLIJK: het token gaat als cookie over ONVERSLEUTELD HTTP. Wie het
 * LAN kan meelezen, leest de cookie en kan hem overnemen -- net als bij Basic-auth.
 * Dit VERVANGT de popup, niet TLS. De cookie is HttpOnly (geen JS-diefstal via XSS)
 * en SameSite=Strict (geen CSRF vanaf een andere site), maar er staat met opzet
 * GEEN 'Secure' op, want dan zou de node over gewoon HTTP geen enkele cookie meer
 * zetten. Zet deze node niet open naar buiten; gebruik een VPN of een TLS-proxy als
 * het van buiten moet. */
#define WEB_COOKIE_NAME  "mu_sess"
#define WEB_SESS_MAX     4                          /* gelijktijdige sessies */
#define WEB_SESS_HEX     32                         /* 16 willekeurige bytes -> 32 hex */
#define WEB_SESS_TTL_MS  (12UL * 60UL * 60UL * 1000UL)   /* 12 uur */

struct WebSession {
  char          tok[WEB_SESS_HEX + 1];
  unsigned long expires_ms;
  bool          used;
};
static WebSession g_sess[WEB_SESS_MAX] = {};

/* DE ANTI-BRUTE-FORCE-REM. Een groeiende WACHTTIJD tussen mislukte pogingen, en met
 * opzet NIET een delay() in het verzoekpad: dit is de synchrone server die de
 * LoRa-timing deelt, en een halve seconde blokkeren daar is precies wat niet mag.
 * We onthouden dus alleen een TIJDSTIP waarvoor de volgende poging geweigerd wordt;
 * de server blijft ondertussen gewoon draaien. De teller en het tijdstip leven in
 * RAM (een herstart of een geslaagde login wist ze), want dit is een rem tegen
 * raden op afstand en geen boekhouding die de reboot hoeft te overleven. */
static uint8_t       g_login_fails = 0;
static unsigned long g_login_next_ms = 0;

/* Twee hex-tekens uit een byte. */
static void webHexByte(uint8_t b, char* o) {
  static const char* H = "0123456789abcdef";
  o[0] = H[b >> 4];
  o[1] = H[b & 0x0f];
}

/* Vergelijking in (vrijwel) constante tijd: geen vroege uitstap op het eerste
 * verschillende teken, zodat het antwoordtijdstip niets over de inhoud verraadt.
 * Voor de willekeurige 128-bits tokens is dit ruim overbodig, voor het wachtwoord
 * is het netjes. Vergelijkt tot de NUL van beide en telt een lengteverschil mee. */
static bool webCtEqual(const char* a, const char* b) {
  size_t la = strlen(a), lb = strlen(b);
  size_t n = la < lb ? la : lb;
  unsigned char d = (unsigned char)(la ^ lb);
  for (size_t i = 0; i < n; i++) d |= (unsigned char)(a[i] ^ b[i]);
  return d == 0;
}

/* millis()-overloop-veilig: is 'deadline' al voorbij? */
static bool webPast(unsigned long deadline) {
  return (long)(millis() - deadline) >= 0;
}

/* Het token uit de Cookie-kop halen naar out. Vereist dat collectHeaders("Cookie")
 * in routes() is aangeroepen. Kopieert de kop eerst naar een STATISCHE buffer (niet
 * op de gedeelde loop-stapel) en ontleedt daar op zijn plaats -- en dat kopiëren
 * gebeurt BINNEN de strlcpy-expressie, dus zonder de tijdelijke-String-val die
 * getArgStrict de das omdeed. Geeft false als er geen mu_sess-cookie is. */
static char g_cookie_buf[320];
static bool webCookieToken(WebServer& s, char* out, size_t out_len) {
  out[0] = 0;
  if (!s.hasHeader("Cookie")) return false;
  strlcpy(g_cookie_buf, s.header("Cookie").c_str(), sizeof(g_cookie_buf));

  const char* key = WEB_COOKIE_NAME "=";
  size_t keylen = strlen(key);
  char* p = strstr(g_cookie_buf, key);
  /* Alleen op een cookie-grens matchen (begin, of na "; "), zodat een cookie met de
   * naam "xmu_sess" niet per ongeluk raak is. */
  while (p) {
    if (p == g_cookie_buf || p[-1] == ' ' || p[-1] == ';') break;
    p = strstr(p + 1, key);
  }
  if (!p) return false;
  p += keylen;

  size_t o = 0;
  while (*p && *p != ';' && *p != ' ' && o + 1 < out_len) out[o++] = *p++;
  out[o] = 0;
  return o > 0;
}

/* Verlopen sessies opruimen -- lui, bij elke toets. */
static void webSessionSweep() {
  for (int i = 0; i < WEB_SESS_MAX; i++) {
    if (g_sess[i].used && webPast(g_sess[i].expires_ms)) g_sess[i].used = false;
  }
}

/* Een nieuwe sessie maken en het token (32 hex + NUL) in out zetten. Neemt een
 * vrij/verlopen vakje, en anders het vakje dat het eerst zou verlopen (dan wint de
 * verste sessie -- oudere gebruikers worden verdrongen, niet nieuwere geweigerd). */
static void webSessionCreate(char* out /* [WEB_SESS_HEX+1] */) {
  uint8_t r[WEB_SESS_HEX / 2];
  for (size_t i = 0; i < sizeof(r); i++) r[i] = (uint8_t)(esp_random() & 0xff);
  for (size_t i = 0; i < sizeof(r); i++) webHexByte(r[i], out + i * 2);
  out[WEB_SESS_HEX] = 0;

  int slot = -1;
  for (int i = 0; i < WEB_SESS_MAX; i++) {
    if (!g_sess[i].used || webPast(g_sess[i].expires_ms)) { slot = i; break; }
  }
  if (slot < 0) {
    slot = 0;
    for (int i = 1; i < WEB_SESS_MAX; i++) {
      if ((long)(g_sess[i].expires_ms - g_sess[slot].expires_ms) < 0) slot = i;
    }
  }
  strlcpy(g_sess[slot].tok, out, sizeof(g_sess[slot].tok));
  g_sess[slot].expires_ms = millis() + WEB_SESS_TTL_MS;
  g_sess[slot].used = true;
}

/* De sessie bij dit token weggooien (afmelden). */
static void webSessionDestroy(const char* tok) {
  for (int i = 0; i < WEB_SESS_MAX; i++) {
    if (g_sess[i].used && webCtEqual(g_sess[i].tok, tok)) g_sess[i].used = false;
  }
}

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
 *   de web-gebruiker (32), ontsnapt, plus de "webcustom"-vlag        ~  60
 *                                                                 ----------
 *                                                                  ~ 1335
 *
 * 1792 geeft daar ruim 450 byte marge boven. */
static char g_cfg[2200];   /* incl. de tijd-velden (ntp/tz/lokale tijd/sync-status) */

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
void web_route_time()   { if (g_self) g_self->handleTime(); }
void web_route_hook()   { if (g_self) g_self->handleHook(); }
void web_route_monadd() { if (g_self) g_self->handleMonAdd(); }
void web_route_mondel() { if (g_self) g_self->handleMonDel(); }
void web_route_acljson()   { if (g_self) g_self->handleAclJson(); }
void web_route_aclset()    { if (g_self) g_self->handleAclSet(); }
void web_route_acldel()    { if (g_self) g_self->handleAclDel(); }
void web_route_aclstrict() { if (g_self) g_self->handleAclStrict(); }
void web_route_cli()       { if (g_self) g_self->handleCli(); }
void web_route_cfgjson()   { if (g_self) g_self->handleCfgJson(); }
void web_route_webcred()   { if (g_self) g_self->handleWebCred(); }
void web_route_credreset() { if (g_self) g_self->handleWebCredReset(); }
void web_route_login()     { if (g_self) g_self->handleLogin(); }
void web_route_loginpost() { if (g_self) g_self->handleLoginPost(); }
void web_route_logout()    { if (g_self) g_self->handleLogout(); }
void web_route_sim()       { if (g_self) g_self->handleSim(); }
void web_route_simclear()  { if (g_self) g_self->handleSimClear(); }
void web_route_alerttest() { if (g_self) g_self->handleAlertTest(); }
void web_route_roomsjson()    { if (g_self) g_self->handleRoomsJson(); }
void web_route_roomadd()      { if (g_self) g_self->handleRoomAdd(); }
void web_route_roomedit()     { if (g_self) g_self->handleRoomEdit(); }
void web_route_roomdel()      { if (g_self) g_self->handleRoomDel(); }
void web_route_roomsbackup()  { if (g_self) g_self->handleRoomsBackup(); }
void web_route_roomsrestore() { if (g_self) g_self->handleRoomsRestore(); }
void web_route_monalarm()     { if (g_self) g_self->handleMonAlarm(); }
void web_route_snodeadd()     { if (g_self) g_self->handleSNodeAdd(); }
void web_route_snodeedit()    { if (g_self) g_self->handleSNodeEdit(); }
void web_route_snodedel()     { if (g_self) g_self->handleSNodeDel(); }
void web_route_roomacl()      { if (g_self) g_self->handleRoomAcl(); }
void web_route_snodeacl()     { if (g_self) g_self->handleSNodeAcl(); }
void web_route_roomadvert()   { if (g_self) g_self->handleRoomAdvert(); }
void web_route_snodeadvert()  { if (g_self) g_self->handleSNodeAdvert(); }
void web_route_monsnmp()      { if (g_self) g_self->handleMonSnmp(); }
void web_route_contactsjson() { if (g_self) g_self->handleContactsJson(); }
void web_route_botjson()      { if (g_self) g_self->handleBotJson(); }
void web_route_botsjson()     { if (g_self) g_self->handleBotsJson(); }
void web_route_botmanage()    { if (g_self) g_self->handleBotManage(); }
void web_route_botrecip()     { if (g_self) g_self->handleBotRecip(); }
void web_route_botdiag()      { if (g_self) g_self->handleBotDiag(); }
void web_route_botadvert()    { if (g_self) g_self->handleBotAdvert(); }
void web_route_botsendto()    { if (g_self) g_self->handleBotSendto(); }
void web_route_botpost()      { if (g_self) g_self->handleBotPost(); }
void web_route_channelsjson() { if (g_self) g_self->handleChannelsJson(); }
void web_route_channeladd()   { if (g_self) g_self->handleChannelAdd(); }
void web_route_channeldel()   { if (g_self) g_self->handleChannelDel(); }
void web_route_channeltoggle(){ if (g_self) g_self->handleChannelToggle(); }
void web_route_companionsjson(){ if (g_self) g_self->handleCompanionsJson(); }
void web_route_companion()    { if (g_self) g_self->handleCompanion(); }
void web_route_messagesjson() { if (g_self) g_self->handleMessagesJson(); }

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

/* Als getArg, maar ZONDER stil af te kappen. Geeft -1 als het argument ontbreekt,
 * -2 als de waarde niet in out past (die zou getArg afkappen), anders de lengte --
 * en dan is out gevuld. Voor de web-credential: een afgekapt wachtwoord is een
 * ander wachtwoord, en dat wil je weigeren en niet stil bewaren. Er komt geen eigen
 * String bij; s.arg() is dezelfde die de bibliotheek toch al aanmaakt, precies als
 * in getArg(). */
static int getArgStrict(WebServer& s, const char* name, char* out, size_t out_len) {
  out[0] = 0;
  int n = s.args();
  for (int i = 0; i < n; i++) {
    if (strcmp(s.argName(i).c_str(), name) == 0) {
      /* DE BUG DIE /web/cred KAPOT MAAKTE -- EN WAAROM getArg() HET WEL DEED.
       *
       * Hier stond:
       *     const char* v = s.arg(i).c_str();
       *     size_t len = strlen(v);
       *     ... strlcpy(out, v, out_len);
       *
       * s.arg(i) geeft een String TERUG PER WAARDE -- een tijdelijk object. Zijn
       * .c_str() wijst naar de INTERNE buffer van dat tijdelijke object. De
       * ESP32-String heeft "small string optimisation": voor korte waarden
       * (o.a. "admin", "meshcore") zit die buffer IN het object zelf (sso.buff),
       * niet op de heap. Aan het einde van de statement (de puntkomma) wordt het
       * tijdelijke object vernietigd en komt zijn stapelruimte vrij; 'v' bengelt
       * dan naar geheugen dat de eerstvolgende regel (strlen, de vergelijking, de
       * strlcpy-oproep) meteen hergebruikt. strlen(v) las dus overschreven bytes en
       * gaf een lengte die niet klopte -- bij korte credentials deterministisch 0.
       * Vandaar dat handleWebCred "user ontbreekt of is leeg" teruggaf, OOK met
       * user/pass in de querystring: het lag niet aan het lezen van de argumenten
       * (dat werkte) maar aan use-after-free NA het lezen.
       *
       * getArg() ontsnapte hieraan puur door de vorm: daar staat
       *     strlcpy(out, s.arg(i).c_str(), out_len);
       * en dan leeft het tijdelijke String-object tot NA de strlcpy (het einde van
       * DIE volledige expressie), dus tijdens het kopiëren is de buffer nog geldig.
       * Zelfde bibliotheek, zelfde s.args()/argName()/arg() -- het enige verschil
       * was dat getArgStrict de pointer over een puntkomma heen bewaarde.
       *
       * DE FIX: nooit een .c_str() van een tijdelijke String in een pointer
       * bewaren. We meten de lengte en kopiëren, elk BINNEN zijn eigen volledige
       * expressie (elke s.arg(i) is een eigen, kortlevende tijdelijke), en meten
       * daarna de definitieve lengte uit de VASTE uitvoerbuffer. */
      if (s.arg(i).length() >= out_len) return -2;   /* zou afkappen -> weigeren */
      strlcpy(out, s.arg(i).c_str(), out_len);
      return (int)strlen(out);
    }
  }
  return -1;
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

/* Schrijft ,"acl":[{"pub":"<64hex>","level":N}, ...] voor een room/snode-slot in
 * buf achter positie n. kind = ACL_KIND_ROOM(0)/ACL_KIND_SNODE(1). NOOIT geheimen
 * -- alleen de PUBLIEKE sleutel + het niveau (1 read, 2 readwrite, 3 admin). */
static int appendAclJson(IWebNode* acl, char* buf, size_t cap, int n, int kind, int slot) {
  n += snprintf(buf + n, cap - n, ",\"acl\":[");
  int cnt = acl->webAclCount(kind, slot);
  char pub[PUB_KEY_SIZE * 2 + 1];
  int level = 0;
  for (int i = 0; i < cnt; i++) {
    if ((size_t)n > cap - 120) break;
    if (!acl->webAclGet(kind, slot, i, pub, sizeof(pub), &level)) continue;
    n += snprintf(buf + n, cap - n, "%s{\"pub\":\"%s\",\"level\":%d}", i ? "," : "", pub, level);
  }
  n += snprintf(buf + n, cap - n, "]");
  return n;
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
@keyframes pulse{50%{box-shadow:0 0 3px var(--accent)}}@media(prefers-reduced-motion:reduce){*{animation-duration:.001ms!important;animation-iteration-count:1!important;transition-duration:.001ms!important}}
.st{font-family:var(--mono);font-size:.85rem;white-space:nowrap}
.c-on{color:var(--accent)}.c-off{color:var(--red)}.c-warn{color:var(--amber)}
.c-unk{color:var(--muted)}
.note{font-size:.84rem;color:var(--muted);margin:.6rem 0 0}
.note b{color:var(--text);font-weight:600}
.why{border-left:3px solid var(--cyan);background:linear-gradient(90deg,
rgba(76,201,240,.07),transparent 70%);padding:.7rem .9rem;border-radius:0 8px 8px 0;
font-size:.87rem;margin:0 0 .8rem}
.why b{color:var(--cyan)}
/* Compacte "?"-help: elke uitleg (.why/.note) wordt bij het laden achter een
   klein "?"-schijfje gevouwen (zie het declutter-script). Instellingen eerst,
   kennis een klik weg -- niets gaat verloren. */
details.help{display:block;margin:.3rem 0}
details.help>summary{list-style:none;cursor:help;display:inline-flex;
align-items:center;justify-content:center;width:1.15rem;height:1.15rem;
border:1px solid var(--border);border-radius:50%;color:var(--muted);
font-size:.72rem;font-weight:700;line-height:1;user-select:none}
details.help>summary::-webkit-details-marker{display:none}
details.help>summary::marker{content:""}
details.help>summary:hover,details.help[open]>summary{color:var(--accent);border-color:var(--accent)}
details.help[open]>summary{margin-bottom:.35rem}
details.help>.why,details.help>.note{margin:.2rem 0 .2rem}
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
<button id="lo" title="Sessie afmelden">afmelden</button>
</div>

<nav class="tabs">
<button class="on" data-p="1">bewaking</button>
<button data-p="2">toegang</button>
<button id="tabrooms" data-p="4" hidden>rooms</button>
<button id="tabsnodes" data-p="5" hidden>sensor-nodes</button>
<button id="tabbot" data-p="6" hidden>bot</button>
<button id="tabcomp" data-p="7" hidden>companions</button>
<button data-p="3">node</button>
</nav>

<section id="p1">

<div id="simban"></div>
<div class="tilegrid" id="t"></div>
<div id="pingres"></div>

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

<p class="note"><b>De velden achter <code>bewerk</code>:</b>
<b>a</b> = alarmroute (<i>dm</i>/<i>room</i>/<i>both</i>),
<b>r</b> = room-set voor de post-tekst (bv <code>0,1</code>),
<b>s</b> = op welke sensor-nodes dit kanaal als telemetrie verschijnt, en
<b>e</b> = <b>ernst</b>. De ernst zet de emoji vooraan de storings-DM &mdash;
🔴&nbsp;hoog, 🟠&nbsp;midden, 🟢&nbsp;laag &mdash; en een companion (T1000-E) kiest
daarop zijn buzzer-tune. <b>Herstelmeldingen</b> (&quot;weer bereikbaar&quot;) zijn
altijd 🟢&nbsp;groen, ongeacht de ingestelde ernst. Alleen de <b>DM</b> krijgt de
emoji; de room-post blijft schoon.</p>

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
<b>Reken op enkele seconden tot de eerste zendpoging</b>, daarna de gewone
bezorgtijd over het mesh. Waarschuwingen worden bij de periodieke leesronde
beoordeeld (elke 60&nbsp;s), maar een klik op deze knoppen <i>trekt die ronde
meteen naar voren</i> &mdash; alleen het moment, niet het pad. Het blijft dus
precies de weg die een echte waarschuwing ook loopt.</p>

<div class="card">
<div class="row">
<label>Vervaltijd van een forcering (s)<input id="simsecs" type="number"
min="30" max="3600" value="600"><span class="cur">30 t/m 3600; een forcering
kan niet blijven staan</span></label>
<label>Rust voor een herstelmelding (s)<input id="rhold" type="number"
min="0" max="3600" value="120"><span class="cur" id="rholdcur">nu: –</span></label>
<label>Herhalen tot bevestiging (s)<input id="arep" type="number"
min="0" max="3600" value="300"><span class="cur" id="arepcur">nu: –</span></label>
</div>
<div class="row" style="margin-top:.6rem">
<label class="cb"><input type="checkbox" id="recon"> ook melden als iets weer
<b>werkt</b></label>
</div>
<div class="quick">
<button id="rgo">alarminstellingen opslaan</button>
<button id="tgo">testbericht sturen</button>
<button id="sclr">alles vrijgeven</button>
</div>
<div id="nagban"></div>
<div id="smsg"></div>
<div class="deliv" id="deliv"></div>
<p class="note"><b>Herhalen tot bevestiging &mdash; als een pieper.</b> Standaard
herhaalt een storingsmelding elke <b>herhaalperiode</b> tot een companion een DM
met <code>ok</code> (of <code>ack</code>) terugstuurt. Dat is met opzet <i>niet</i>
de transport-ACK: die bewijst alleen dat een pakket aankwam, niet dat een
<b>mens</b> keek. Alleen een contact met het <b>alarm</b>-recht kan bevestigen;
één &quot;ok&quot; stopt alle op dat moment openstaande meldingen tegelijk. Zet
de periode op <b>0</b> voor het oude gedrag (één melding en klaar). De ondergrens
is 60&nbsp;s, want elke herhaling is zendtijd op een gedeelde band en de
meldingen worden toch maar eens per leesronde beoordeeld.<br>
<b>Er zit een harde bovengrens op</b> van een handvol herhalingen: een node die
niemand binnen die tijd bevestigt, heeft een groter probleem dan een gemiste
melding, en doorgaan vult alleen de band. Bij het bereiken ervan stopt het
herhalen (de monitor blijft gewoon down in beeld) en zie je dat hieronder.
Herhalingen tellen mee in dezelfde grens van twee gelijktijdige meldingen, dus
een golf storingen verdringt elkaar netjes in plaats van de band vol te
zetten.<br>
<b>Herstelmeldingen &mdash; waarom die er horen te zijn.</b>
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

<h2>Reactietijd &mdash; hoe snel een netvoeding/wifi-alert komt</h2>
<p class="why"><b>Waarom dit er is:</b> een netvoeding- of wifi-alert duurde vroeger
~1&nbsp;minuut, omdat de detectie met opzet traag was en hardgecodeerd. Deze marges
zijn nu instelbaar. <b>De afweging:</b> sneller reageren = een kleinere marge tegen
een <i>valse</i> flap (een korte spanning-dip of blip die geen echte
stroomonderbreking is). De hi/lo-drempels bij &quot;Instellingen&quot; blijven de
eigenlijke ontruising; dit zijn alleen de reactietijd-marges. Met de standaarden
hieronder komt een alert in ~6&ndash;10&nbsp;s.</p>
<div class="card">
<div class="row">
<label>Meet-tik voeding (s)<input id="tim-samp" type="number" min="1" max="60"
value="2"><span class="cur" id="tim-sampcur">nu: &ndash;</span></label>
<label>Bevestigende metingen<input id="tim-conf" type="number" min="1" max="10"
value="2"><span class="cur" id="tim-confcur">nu: &ndash;</span></label>
<label>Rust na overgang (s)<input id="tim-set" type="number" min="0" max="300"
value="8"><span class="cur" id="tim-setcur">nu: &ndash;</span></label>
</div>
<div class="row" style="margin-top:.6rem">
<label>Leesronde / alert-eval (s)<input id="tim-read" type="number" min="1" max="60"
value="3"><span class="cur" id="tim-readcur">nu: &ndash;</span></label>
<label>Settle vaste kanalen (s)<input id="tim-deb" type="number" min="0" max="3600"
value="2"><span class="cur" id="tim-debcur">nu: &ndash;</span></label>
</div>
<div class="quick"><button id="tim-go">reactietijd opslaan</button></div>
<div id="tim-msg"></div>
<p class="note"><b>Meet-tik voeding &mdash; <code>power.sample</code> (1&ndash;60&nbsp;s).</b>
Hoe vaak de node de accuspanning meet. Sneller meten = sneller merken dat de stekker
eruit is, maar elke meting valt binnen dezelfde ruis, dus te snel zonder genoeg
bevestigingen laat een dip harder doorwerken. Standaard 2&nbsp;s (was 10&nbsp;s).</p>
<p class="note"><b>Bevestigende metingen &mdash; <code>power.confirm</code> (1&ndash;10).</b>
Zoveel metingen aan de andere kant achter elkaar voordat de toestand kantelt. Hoger =
zekerder maar trager (reactietijd &asymp; meet-tik &times; dit getal). Standaard 2
(was 3).</p>
<p class="note"><b>Rust na overgang &mdash; <code>power.settle</code> (0&ndash;300&nbsp;s).</b>
Na een overgang &eacute;n vlak na een reboot mag de voedingstoestand deze tijd niet
wisselen &mdash; dempt de spanning-transient bij het uittrekken/opstarten. Dit was met
60&nbsp;s de grootste boosdoener vlak na een herstart. Standaard 8&nbsp;s. 0 = geen
rust.</p>
<p class="note"><b>Leesronde / alert-eval &mdash; <code>read.interval</code> (1&ndash;60&nbsp;s).</b>
Hoe vaak de node de sensoren leest en de waarschuwingen beoordeelt. Ook een snel
gedetecteerde onderbreking wacht tot de volgende leesronde voordat het bericht de deur
uit gaat. Standaard 3&nbsp;s.</p>
<p class="note"><b>Settle vaste kanalen &mdash; <code>alert.debounce</code> (0&ndash;3600&nbsp;s).</b>
Extra korte settle op de vaste kanalen (wifi/netvoeding) bovenop de meet-bevestiging
hierboven; laag mag dus. Standaard 2&nbsp;s (was 5&nbsp;s). De harde grendel tegen een
spook-&quot;terug&quot; blijft altijd gelden, ook op 0.</p>
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

<details><summary>SNMP-monitor toevoegen</summary><div>
<form id="asnmp">
<div class="row">
<label>Naam<input name="name" maxlength="16" required placeholder="dak-repeater-in"></label>
<label>Doel-IP<input name="host" maxlength="40" required placeholder="10.10.30.1"></label>
<label>Interval (s)<input name="int" type="number" min="10" max="3600" value="60" required></label>
</div>
<div class="row">
<label>Community<input name="community" maxlength="23" type="password"
autocomplete="new-password" placeholder="public"></label>
<label>OID<input name="oid" maxlength="79" required spellcheck="false"
placeholder="1.3.6.1.2.1.2.2.1.10.2"></label>
</div>
<div class="row">
<label>Interpretatie<select name="interp">
<option value="numeric">numeric (gauge/integer)</option>
<option value="rate">rate (counter -&gt; per seconde)</option>
<option value="status">status (down als != waarde)</option></select></label>
<label>Status-waarde<input name="snmparg" type="number" value="1" title="voor interp=status: de 'up'-waarde, bv 1"></label>
</div>
<button>SNMP-monitor toevoegen</button>
<div id="smsg"></div>
<p class="note">De node doet zelf een niet-blokkerende <b>SNMP-GET</b> (v2c) op
UDP:161. De waarde stroomt als telemetrie over het mesh (zelfde kanaal/rooms/sensor-
nodes als een gewone monitor); geen antwoord = down (alert). <b>numeric</b>: de
waarde zelf (gauge/integer/timeticks). <b>rate</b>: het tempo van een counter
(bv. bytes/s -&gt; verkeer). <b>status</b>: down zodra de waarde ongelijk is aan de
status-waarde (bv. ifOperStatus, 1 = up). Het community-wachtwoord wordt
geobfuskeerd opgeslagen en nooit teruggegeven.</p>
</form></div></details>

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
<code>mon.&lt;kan&gt;.int</code> <code>mon.&lt;kan&gt;.state</code>
<code>mon.&lt;kan&gt;.ms</code>
<code>alert.recover</code> <code>alert.rhold</code> <code>alert.repeat</code>
<code>alert.debounce</code>, en de reactietijd
<code>power.sample</code> <code>power.confirm</code> <code>power.settle</code>
<code>read.interval</code>. En
<code>sensor list</code> voor de hele lijst.<br>
<b>Ad-hoc ping:</b> <code>ping &lt;adres&gt; [n]</code> pingt een vrij op te
geven adres (n keer, standaard 3, hoogstens 5). Niet-blokkerend: de console
antwoordt &quot;gestart&quot; en de uitslag verschijnt onder de tegels op het
tabblad <b>bewaking</b>. Over een DM werkt hetzelfde commando, met de uitslag als
DM terug. De monitor-pingmachine wordt gedeeld; de ad-hoc krijgt voorrang en de
monitorronde schuift een tel op.<br>
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

<div id="webal"></div>
<details><summary>Web-login &mdash; de eigen inlog van deze node</summary><div>
<p class="note">Dit is de login van <b>deze webpagina</b> &mdash; waarmee je via
/login een sessie krijgt, en waarmee de MeshManager-server met Basic-auth headless
binnenkomt. Iets anders dan het beheerderswachtwoord hierboven, want dat is de
login over het <i>mesh</i>. Elke node hoort hier zijn <b>eigen</b> gebruiker en
wachtwoord te hebben: staan ze nog op de gebakken standaard, dan is het dezelfde
login als op elke andere node en de repeater, en opent &eacute;&eacute;n gelekte
credential de hele vloot. Opgeslagen wint van gebakken, net als bij WiFi.<br>
<b>Na opslaan</b> blijf je gewoon ingelogd: je sessiecookie staat los van de
gebruiker/wachtwoord, dus die verandert niet mee. Pas bij het VOLGENDE inloggen (of
voor de MeshManager-server, die met Basic-auth komt) geldt de nieuwe login. Het
wachtwoord wordt nooit getoond of teruggelezen, en <b>leeg laten kan niet</b> (dat
zou de node openzetten).<br>
<b>Terug naar de gebakken standaard</b> (admin/meshcore) kan met de knop hieronder:
die verwijdert de opgeslagen login (/web.cfg), waarna de gebakken waarden weer
gelden. Handig na een flash om van een geroteerde login af te komen.<br>
<b>Eerlijk over de grens:</b> Basic-auth gaat over onversleuteld HTTP, dus ook een
eigen wachtwoord gaat leesbaar (base64) over het LAN. Dit verkleint de schade van
&eacute;&eacute;n lek tot deze ene node, maar het <b>vervangt geen TLS</b> of een
beheer-VLAN. Zet deze node niet open naar buiten.</p>
<div class="row">
<label>Gebruiker<input id="wcu" maxlength="32" spellcheck="false"
autocomplete="username"></label>
<label>Wachtwoord<input id="wcp" type="password" maxlength="64"
autocomplete="new-password" placeholder="nieuw"></label>
</div>
<div class="quick">
<button id="wcgo">Opslaan</button>
<button id="wcreset" class="dng">terug naar standaard (admin/meshcore)</button>
</div>
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

<details><summary>Tijd &mdash; NTP + tijdzone</summary><div>
<div class="row">
<label>NTP-server<input id="ntp" maxlength="47" spellcheck="false" placeholder="pool.ntp.org"></label>
<label>Tijdzone (POSIX-TZ)<input id="tz" maxlength="47" spellcheck="false" placeholder="CET-1CEST,M3.5.0/2,M10.5.0/3"></label>
</div>
<div class="quick"><button type="button" id="tsave">Opslaan + nu syncen</button>
<span id="tnow" style="align-self:center;color:var(--muted);font-size:.85rem"></span></div>
<div id="tmsg"></div>
<p class="note">De RTC en de MeshCore-<b>protocoltijd</b> (adverts, berichtvolgorde)
blijven <b>UTC</b>; alleen de menselijke <b>weergave</b> &mdash; bv. de bot-<code>path</code>
&quot;Received at&quot; &mdash; is lokaal via de ingestelde tijdzone (met de
zone-afkorting, CET/CEST). Standaard is Europe/Brussels, DST-bewust. Een
<b>LAN-tijdserver</b> mag ook: handig als de node zelf geen internet heeft maar de
router wel een klok. Vóór de eerste geslaagde sync toont de node geen tijd maar
&quot;niet gesynct&quot;. Laatste sync: <b id="tsync">&mdash;</b>.</p>
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

<section id="p4" hidden>
<h2>Rooms &mdash; de MeshCore room-servers op deze node</h2>
<p class="why"><b>Wat dit is:</b> deze node draagt tot vier virtuele
<b>room-servers</b> tegelijk, elk met een eigen sleutelpaar en naam. De
MeshCore-app ziet ze als losse rooms. <b>Room&nbsp;0</b> is de hoofdidentiteit van
deze node en kan niet verwijderd worden. Een room wordt joinbaar door hem als
contact toe te voegen &mdash; scan de <b>QR</b> of plak de <b>join-link</b> (die
draagt naam&nbsp;+&nbsp;publieke sleutel, <i>niet</i> het wachtwoord). Staat er een
<b>gastwachtwoord</b>, dan is dat nog steeds nodig om te lezen/schrijven; een
<b>stealth</b>-room adverteert niet en is alleen via QR/link te vinden.</p>
<div class="card pad0"><table id="rl"></table></div>
<div id="rmsg"></div>

<!-- Deel-paneel: QR + kopieerbare join-link -->
<div id="rshare" class="card" hidden>
<div style="display:flex;justify-content:space-between;align-items:center">
<h3 id="rshare-t" style="margin:0">Deel room</h3>
<button type="button" class="sec" onclick="roomShareClose()">sluiten</button></div>
<div style="text-align:center;margin:.7rem 0"><canvas id="rqr"></canvas></div>
<label>Join-link<input id="rshare-uri" readonly spellcheck="false"></label>
<div class="quick"><button type="button" id="rcopy">kopieer link</button></div>
<p class="note">Scan met de MeshCore-app (Contact toevoegen &rarr; scannen) of plak
de link. De link bevat de room-naam en de <b>publieke</b> sleutel &mdash; nooit een
wachtwoord.</p></div>

<!-- Bewerk-paneel -->
<div id="redit" class="card" hidden>
<h3 id="redit-t" style="margin:0 0 .4rem">Room bewerken</h3>
<div class="row">
<label>Naam<input id="re-name" maxlength="23" spellcheck="false"></label>
<label>Beheerderswachtwoord<input id="re-pass" type="password" maxlength="15"
autocomplete="new-password" placeholder="ongewijzigd"></label>
<label>Gastwachtwoord<input id="re-guest" type="password" maxlength="15"
autocomplete="new-password" placeholder="ongewijzigd"></label></div>
<label class="cb" style="margin-top:.5rem"><input type="checkbox" id="re-stealth">
stealth (niet adverteren)</label>
<label class="cb"><input type="checkbox" id="re-guestclear"> gastwachtwoord
<b>wissen</b> (room weer open volgens de leesregels)</label>
<div class="quick"><button type="button" id="re-save">Opslaan</button>
<button type="button" class="sec" onclick="roomEditClose()">annuleer</button></div>
<div class="quick"><span style="align-self:center;color:var(--muted);font-size:.8rem">Advert nu:</span>
<button type="button" onclick="nodeAdvert(0,REI,1)">flood</button>
<button type="button" onclick="nodeAdvert(0,REI,0)">zero-hop</button></div>
<p class="note">Lege velden laten <b>naam</b>, <b>beheerder</b> en <b>gast</b>
ongewijzigd &mdash; de wachtwoorden worden nooit teruggelezen, dus ze staan hier
leeg. Vul alleen in wat je wilt veranderen; vink <b>gastwachtwoord wissen</b> aan om
het weg te halen. Een nieuwe naam gaat direct het advert in.</p>
<h3 style="margin:.6rem 0 .3rem">Toegang &mdash; wachtwoordloos per sleutel</h3>
<div class="card pad0"><table id="racl"></table></div>
<div class="frow"><select id="racl-pick" style="width:auto" title="kies uit gehoorde contacten"><option value="">&mdash; gehoorde contacten &mdash;</option></select>
<input id="racl-pub" placeholder="of plak volledige pubkey (64 hex)"
maxlength="64" spellcheck="false" style="flex:1;min-width:10rem">
<select id="racl-lvl" style="width:auto"><option value="read">read</option>
<option value="readwrite">readwrite</option><option value="admin">admin</option></select>
<button type="button" id="racl-add">grant</button></div>
<div id="raclmsg"></div>
<p class="note">Een sleutel in deze lijst krijgt <b>zonder wachtwoord</b> toegang op
het gekozen niveau (read = lezen/joinen, readwrite = + posten, admin = + beheer van
deze room). <b>Toevoegen</b> vraagt de volledige pubkey (64 hex); <b>verwijderen</b>
mag op een prefix (min. 12 hex).</p>
<h3 style="margin:.6rem 0 .3rem">Kanalen &mdash; welke sensoren in deze room posten</h3>
<div class="card pad0"><table id="rchan"></table></div>
<div class="frow"><input id="rchan-name" placeholder="naam" maxlength="16" spellcheck="false" style="width:8rem">
<input id="rchan-host" placeholder="adres (of - voor gemeld)" maxlength="40" spellcheck="false" style="flex:1;min-width:8rem">
<input id="rchan-int" type="number" min="10" max="3600" value="60" style="width:5rem" title="interval s">
<button type="button" id="rchan-add">nieuw + koppel</button></div>
<div id="rchanmsg"></div>
<p class="note">Vinkje = deze sensor hoort bij deze room (de <b>rm</b>-bit). Uitvinken
<b>ontkoppelt</b> alleen (de monitor blijft bestaan). <b>bewerk</b> wijzigt naam/adres/
interval. Een monitor <b>globaal</b> verwijderen doe je op het tabblad bewaking.</p></div>

<h2>Room toevoegen</h2>
<div class="card"><form id="radd">
<label>Naam<input name="name" maxlength="23" required spellcheck="false"
placeholder="bv. Telemetrie"></label>
<button>Toevoegen</button>
<div id="raddmsg"></div>
<p class="note">Een nieuwe room krijgt een <b>eigen sleutelpaar</b> en neemt het
huidige beheerderswachtwoord van deze node over. Daarna te delen via de
<b>Deel</b>-knop.</p></form></div>

<h2>Backup &amp; restore</h2>
<div class="card">
<p class="note">De backup is een JSON-bestand met de <b>volledige</b> room-config
&mdash; namen, stealth, <b>wachtwoorden en sleutelparen</b>. Daarmee is elke room
exact te herstellen (zelfde publieke sleutel, dus bestaande QR-codes en contacten
blijven geldig). <b>Bewaar hem veilig</b>: wie hem heeft, heeft de room-identiteiten.
Room&nbsp;0 (hoofdidentiteit) wordt bij restore standaard <b>niet</b> overschreven.</p>
<div class="quick">
<button type="button" id="rbackup">Backup downloaden</button>
<label class="btn sec" style="cursor:pointer;line-height:1.4">Restore uploaden&hellip;<input
type="file" id="rrestore" accept="application/json,.json" hidden></label></div>
<label class="cb" style="margin-top:.6rem"><input type="checkbox" id="rov">
bij restore <b>ook room&nbsp;0</b> (hoofdidentiteit) overschrijven</label>
<p class="note">De backup omvat ook de <b>virtuele sensor-nodes</b> (namen + sleutels);
restore zet ze samen met de rooms terug.</p>
<div id="rrmsg"></div></div>
</section>

<section id="p5" hidden>
<h2>Sensor-nodes &mdash; virtuele telemetrie-nodes</h2>
<p class="why"><b>Waarom dit bestaat:</b> de MeshCore-app toont telemetrie alleen
voor contacten van het <b>sensor</b>-type; onze rooms zijn van het room-type. Een
virtuele sensor-node is een aparte identiteit die als <b>sensor</b> adverteert, dus
de app toont er telemetrie voor. Je kunt er meerdere hebben en per sensor kiezen op
<b>welke</b> sensor-nodes hij verschijnt (tabblad <b>bewaking</b>, kolom
<i>rooms/nodes</i>) &mdash; zo verdeel je de kanalen over meerdere nodes en til je
het totaal voorbij de limiet van &eacute;&eacute;n CayenneLPP-pakket. Elke node
heeft een eigen sleutelpaar; delen gaat via de QR/join-link (contacttype
<i>sensor</i>).</p>
<div class="card pad0"><table id="snl"></table></div>
<div id="snmsg"></div>

<h2>Sensor-node toevoegen</h2>
<div class="card"><form id="snadd">
<label>Naam<input name="name" maxlength="23" required spellcheck="false"
placeholder="bv. BE-HSS-DinX-Up2"></label>
<button>Toevoegen</button>
<div id="snaddmsg"></div>
<p class="note">Een nieuwe sensor-node krijgt een <b>eigen sleutelpaar</b> en neemt
het beheerderswachtwoord van deze node over. Koppel er daarna sensoren aan op het
tabblad <b>bewaking</b>.</p></form></div>

<div id="snedit" class="card" hidden>
<h3 id="snedit-t" style="margin:0 0 .4rem">Sensor-node bewerken</h3>
<div class="row"><label>Naam<input id="sne-name" maxlength="23" spellcheck="false"></label></div>
<label class="cb" style="margin-top:.5rem"><input type="checkbox" id="sne-stealth">
stealth (niet adverteren)</label>
<div class="quick"><button type="button" id="sne-save">Opslaan</button>
<button type="button" class="sec" onclick="snodeEditClose()">annuleer</button></div>
<div class="quick"><span style="align-self:center;color:var(--muted);font-size:.8rem">Advert nu:</span>
<button type="button" onclick="nodeAdvert(1,SNEI,1)">flood</button>
<button type="button" onclick="nodeAdvert(1,SNEI,0)">zero-hop</button></div>
<h3 style="margin:.6rem 0 .3rem">Toegang &mdash; wachtwoordloos per sleutel</h3>
<div class="card pad0"><table id="sacl"></table></div>
<div class="frow"><select id="sacl-pick" style="width:auto" title="kies uit gehoorde contacten"><option value="">&mdash; gehoorde contacten &mdash;</option></select>
<input id="sacl-pub" placeholder="of plak volledige pubkey (64 hex)"
maxlength="64" spellcheck="false" style="flex:1;min-width:10rem">
<select id="sacl-lvl" style="width:auto"><option value="read">read</option>
<option value="readwrite">readwrite</option><option value="admin">admin</option></select>
<button type="button" id="sacl-add">grant</button></div>
<div id="saclmsg"></div>
<p class="note">Een sleutel hier krijgt <b>zonder wachtwoord</b> toegang tot de
telemetrie van deze sensor-node op het gekozen niveau. Toevoegen: volledige pubkey;
verwijderen mag op prefix.</p>
<h3 style="margin:.6rem 0 .3rem">Kanalen &mdash; welke sensoren op deze sensor-node</h3>
<div class="card pad0"><table id="schan"></table></div>
<div class="frow"><input id="schan-name" placeholder="naam" maxlength="16" spellcheck="false" style="width:8rem">
<input id="schan-host" placeholder="adres (of - voor gemeld)" maxlength="40" spellcheck="false" style="flex:1;min-width:8rem">
<input id="schan-int" type="number" min="10" max="3600" value="60" style="width:5rem" title="interval s">
<button type="button" id="schan-add">nieuw + koppel</button></div>
<div id="schanmsg"></div>
<p class="note">Vinkje = deze sensor verschijnt als telemetrie op deze sensor-node (de
<b>sn</b>-bit). Uitvinken <b>ontkoppelt</b> alleen; de monitor blijft bestaan.
Globaal verwijderen: tabblad bewaking.</p></div>
</section>

<section id="p6" hidden>
<h2>Bots &mdash; meerdere chat-identiteiten</h2>
<p class="why"><b>Waarom meerdere bots:</b> de node draagt N onafhankelijke
<b>chat</b>-contacten (type&nbsp;1), elk met een eigen sleutel, naam en
ontvangerslijst. Eén bot draagt de <b>alert-rol</b> (de per-sensor
<i>dm</i>/<i>both</i>-alerts en flash-meldingen); een tweede kan bv.
<b>companion-MANAGEMENT</b>-verkeer dragen. Kies hieronder welke bot je beheert;
de <b>*</b> markeert de alert-bot.</p>

<div class="card pad0"><table id="botsl"></table></div>
<div class="frow" style="margin-top:.4rem">
<input id="bot-new-name" placeholder="naam van een nieuwe bot" maxlength="23" spellcheck="false" style="flex:1;min-width:10rem">
<button type="button" id="bot-new">+ bot (genereert sleutel)</button></div>
<div id="botsmsg"></div>
<p class="note">De <b>alert-bot</b> is de zendweg voor alarmen; hem uitzetten of wissen
kan niet. Een nieuwe bot krijgt een <b>nieuw sleutelpaar</b> en adverteert meteen.
Wissen laat de sleutel op de node staan (heraanmaak van hetzelfde slot geeft dezelfde
identiteit terug). MeshManager leest alle bots via <code>/bots.json</code>.</p>

<h2>Beheer van: <span id="bot-cur-name" style="color:var(--accent)">&hellip;</span></h2>
<p class="why">Alle onderstaande instellingen (join-link, ontvangers, zend-diagnose,
handmatige DM's) gelden voor de <b>gekozen</b> bot. Klik een bot in de tabel hierboven.</p>

<div class="card" id="botcard">
<div class="row"><b id="bot-name">&hellip;</b>
<span class="key" id="bot-pub" title="volledige pubkey (klik om te kopiëren)"></span></div>
<div class="frow" style="margin-top:.4rem">
<input id="bot-uri" readonly spellcheck="false" style="flex:1;min-width:12rem">
<button type="button" id="bot-copy">kopieer</button></div>
<canvas id="bqr" style="margin-top:.5rem;max-width:100%;image-rendering:pixelated"></canvas>
<div class="quick"><span style="align-self:center;color:var(--muted);font-size:.8rem">Advert nu:</span>
<button type="button" onclick="botAdvert(1)">flood</button>
<button type="button" onclick="botAdvert(0)">zero-hop</button></div>
<div id="botmsg"></div></div>

<h2>Zend-diagnose &mdash; hoe de afzender zond</h2>
<p class="why"><b>Wat het doet:</b> achter een bot-antwoord komt hoe het binnenkomende
pakket verstuurd was: <code>2-byte</code>&#128077; of <code>1-byte</code>&#128542;
(pad-hashgrootte) en <code>scoped</code>&#128077; of <code>geen scope</code>&#128542;.
Twee losse oordelen, dus mengelingen komen voor. In een kanaal leest iedereen mee dat
1-byte en ongescoped niet meer de bedoeling zijn.</p>
<div class="card">
<div class="quick"><span style="align-self:center;color:var(--muted);font-size:.8rem">Toon bij:</span>
<label><input type="checkbox" id="dg-ping"> ping</label>
<label><input type="checkbox" id="dg-test"> test</label>
<label><input type="checkbox" id="dg-path"> path</label></div>
<div class="frow" style="margin-top:.45rem">
<select id="dg-urlmode" style="width:auto">
<option value="0">geen uitleg-URL</option>
<option value="1">URL inline, tussen haakjes</option>
<option value="2">URL als apart bericht in het kanaal</option></select>
<input id="dg-url" placeholder="https://&hellip;" spellcheck="false" style="flex:1;min-width:12rem"></div>
<div id="dg-fit" class="note" style="margin-top:.3rem"></div>
<div class="quick" style="margin-top:.35rem"><button type="button" id="dg-save">bewaren</button></div>
<div id="dgmsg"></div>
<p class="note">De URL verschijnt alleen bij een <b>ongescopet</b> pakket. <b>Inline</b>
past hem tussen haakjes achter <code>geen scope</code>, maar een antwoord mag hoogstens
160&nbsp;tekens zijn &mdash; past de link er niet helemaal in, dan laat de bot hem
<i>volledig</i> weg (nooit een halve link). <b>Apart bericht</b> heeft die last niet en
werkt dus ook bij lange <code>path</code>-antwoorden; dat gaat als losse post naar het
kanaal waar het commando vandaan kwam.</p></div>

<h2>Ontvangers &mdash; wie de DM's krijgt</h2>
<div class="card pad0"><table id="botrl"></table></div>
<div class="frow" style="margin-top:.4rem">
<select id="bot-pick" style="width:auto" title="kies uit gehoorde contacten"><option value="">&mdash; uit gehoorde contacten &mdash;</option></select>
<input id="bot-pub-in" placeholder="of plak volledige pubkey (64 hex)" maxlength="64"
spellcheck="false" style="flex:1;min-width:12rem">
<button type="button" id="bot-add">toevoegen</button></div>
<div id="botaddmsg"></div>
<p class="note">Toevoegen vraagt de <b>volledige</b> pubkey (het gedeelde geheim
wordt eruit berekend); kies uit de <b>gehoorde contacten</b> of plak hem. Verwijderen
mag op een prefix (&ge;12 hex).</p>

<h2>Handmatig een DM sturen</h2>
<div class="card">
<div class="frow">
<select id="bot-sto-pick" style="width:auto" title="kies uit gehoorde contacten"><option value="">&mdash; gehoorde contacten &mdash;</option></select>
<input id="bot-sto-key" placeholder="pubkey (64 hex)" maxlength="64" spellcheck="false" style="flex:1;min-width:10rem"></div>
<div class="frow" style="margin-top:.35rem">
<input id="bot-sto-msg" placeholder="bericht" maxlength="150" spellcheck="false" style="flex:1;min-width:12rem">
<button type="button" id="bot-sto-go">stuur DM</button></div>
<div class="quick" style="margin-top:.35rem">
<input id="bot-post-msg" placeholder="bericht naar de HELE lijst" maxlength="150" spellcheck="false" style="flex:1;min-width:12rem">
<button type="button" id="bot-post-go">post naar allen</button></div>
<div id="botsendmsg"></div>
<p class="note">Eén DM (<b>stuur DM</b>) of naar iedereen op de lijst
(<b>post naar allen</b>). Beide zijn beheer-acties.</p></div>

<h2>Hashtag-kanalen &mdash; meeluisteren en antwoorden</h2>
<div class="card pad0"><table id="chl"></table></div>
<div class="frow" style="margin-top:.4rem">
<input id="ch-name" placeholder="kanaalnaam (bv. #test of Public)" maxlength="23" spellcheck="false" style="width:11rem">
<input id="ch-secret" placeholder="secret leeg = afgeleid uit naam (of 32/64 hex)" maxlength="64" spellcheck="false" style="flex:1;min-width:12rem">
<button type="button" id="ch-add">toevoegen</button></div>
<div id="chmsg"></div>
<p class="note">De bot leest de <b>ingeschakelde</b> kanalen mee en antwoordt IN het
kanaal op <code>ping</code>, <code>test</code> (signaalrapport) en <code>path</code>
(route met repeater-namen). <b>Secret leeg laten</b> = een <b>hashtag-kanaal</b>: de
sleutel wordt dan uit de naam afgeleid (eerste 16 byte van <code>sha256(naam)</code>),
exact zoals de MeshCore-app &mdash; zelfde naam is zelfde kanaal. De naam
<b>Public</b> (met of zonder <code>#</code>) is een speciaal geval: zonder secret
gebruikt de node de <b>vaste publieke sleutel</b> (<code>8b3387e9c5cdea6ac9e5edbaa115cd72</code>),
zodat je op het ECHTE publieke kanaal uitkomt. Geef je wél een secret op (32 hex =
128-bit of 64 hex = 256-bit), dan gebruikt de node die (die wint altijd). Een
afgeleide sleutel is <b>niet geheim</b> (wie de naam kent leidt hem af); een eigen
secret wordt hier niet teruggetoond. Zet een kanaal op <b>uit</b> om te stoppen met
meelezen zonder het te wissen.</p>
</section>

<section id="p7" hidden>
<h2>Companions &mdash; beheer + laatst bekende locatie</h2>
<p class="why"><b>Waarom op de node:</b> companions (T1000-E e.d.) worden hier
rechtstreeks op de node beheerd, zodat aansturen en locatie-opvolging blijven
werken <i>ook als MeshManager plat ligt</i>. De knoppen sturen het bijhorende
<code>!</code>-commando als schone bot-DM naar de companion (via dezelfde weg als
&lsquo;stuur DM&rsquo;). De companion antwoordt op <code>!loc</code>/SOS/val met een
DM die begint met <code>#LOC&nbsp;&lt;lat&gt;,&lt;lon&gt;</code>; de node vangt dat
op, werkt de laatst bekende locatie bij en bounced niet meer met
&lsquo;onbekend commando&rsquo;.</p>

<div class="card pad0"><table id="cml"></table></div>
<div class="frow" style="margin-top:.4rem">
<select id="cm-pick" style="width:auto" title="kies uit gehoorde contacten"><option value="">&mdash; uit gehoorde contacten &mdash;</option></select>
<input id="cm-name" placeholder="naam" maxlength="23" spellcheck="false" style="width:9rem">
<input id="cm-pub" placeholder="volledige pubkey (64 hex)" maxlength="64" spellcheck="false" style="flex:1;min-width:12rem">
<button type="button" id="cm-add">opslaan</button></div>
<div id="cmaddmsg"></div>
<p class="note">Toevoegen/wijzigen vraagt de <b>volledige</b> pubkey (het gedeelde
geheim wordt eruit berekend) en een naam; kies uit de <b>gehoorde contacten</b> of
plak de sleutel. Een bestaande companion (zelfde pubkey) wordt bijgewerkt, de
locatie blijft. Verwijderen mag op een prefix (&ge;12 hex). Cap: 16 companions.</p>

<h2>Inkomende berichten &mdash; antwoorden van companions</h2>
<div class="card pad0"><table id="cmmsgs"></table></div>
<p class="note">Álle inkomende companion-DM's (antwoorden op commando's zoals
<code>!status</code>/<code>!ping</code>/<code>!cfg</code> en <code>#LOC</code>-rapporten),
nieuwste eerst. Leest <code>/messages.json</code> (RAM-ringbuffer, laatste 24 &mdash;
overleeft geen herstart). Ook opvraagbaar door MeshManager.</p>

<h2>Commando's &mdash; naar de gekozen companion</h2>
<div class="card">
<div class="frow"><label style="align-self:center;color:var(--muted);font-size:.85rem">Companion:</label>
<select id="cm-cmd-pick" style="flex:1;min-width:12rem"><option value="">&mdash; kies een companion &mdash;</option></select></div>
<div class="quick" style="margin-top:.5rem">
<button type="button" onclick="cmCmd('!find')">Find</button>
<button type="button" onclick="cmCmd('!findstop')">Stop-find</button>
<button type="button" onclick="cmCmd('!loc')">Locate</button>
<button type="button" onclick="cmCmd('!ping')">Ping</button>
<button type="button" onclick="cmCmd('!cfg')">Config</button>
<button type="button" onclick="cmCmd('!mute on')">Mute aan</button>
<button type="button" onclick="cmCmd('!mute off')">Mute uit</button></div>
<div class="quick" style="margin-top:.4rem">
<button type="button" onclick="cmCmd('!status')">Status</button>
<button type="button" onclick="cmCmd('!tunes')">Tunes</button>
<button type="button" onclick="cmCmd('!rxps')">Rx-stats</button>
<button type="button" onclick="cmCmd('!gps on')">GPS aan</button>
<button type="button" onclick="cmCmd('!gps off')">GPS uit</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Config-preset<select id="cm-preset" style="width:auto;margin-left:.3rem"><option>1</option><option>2</option><option>3</option></select></label>
<button type="button" onclick="cmCmd('!preset '+document.getElementById('cm-preset').value)">stuur preset</button></div>
<div class="frow" style="margin-top:.5rem">
<label style="align-self:center">Volume<select id="cm-vol" style="width:auto;margin-left:.3rem"><option>0</option><option>1</option><option>2</option><option>3</option></select></label>
<button type="button" onclick="cmCmd('!vol '+document.getElementById('cm-vol').value)">stuur vol</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Vol per slot<select id="cm-vslot" style="width:auto;margin-left:.3rem"><option value="H">H (hoog)</option><option value="M">M (midden)</option><option value="L">L (laag)</option></select></label>
<select id="cm-vslotval" style="width:auto"><option>0</option><option>1</option><option>2</option><option>3</option></select>
<button type="button" onclick="cmCmd('!vol '+document.getElementById('cm-vslot').value+' '+document.getElementById('cm-vslotval').value)">stuur slot-vol</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Play<input id="cm-play" placeholder="preset" maxlength="20" style="width:8rem;margin-left:.3rem"></label>
<button type="button" onclick="cmCmd('!play '+document.getElementById('cm-play').value.trim())">stuur play</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Tune<select id="cm-tsev" style="width:auto;margin-left:.3rem"><option value="H">H (hoog)</option><option value="M">M (midden)</option><option value="L">L (laag)</option></select></label>
<input id="cm-tpreset" placeholder="preset-naam" maxlength="20" style="width:8rem">
<button type="button" onclick="cmCmd('!tune '+document.getElementById('cm-tsev').value+' preset '+document.getElementById('cm-tpreset').value.trim())">stuur tune</button></div>
<div class="frow" style="margin-top:.4rem">
<input id="cm-quiet" placeholder="quiet-argument (bv. 22:00-07:00)" maxlength="30" style="flex:1;min-width:10rem">
<button type="button" onclick="cmCmd('!quiet '+document.getElementById('cm-quiet').value.trim())">stuur quiet</button></div>
<div class="frow" style="margin-top:.4rem">
<select id="cm-allow-pick" style="width:auto" title="kies uit gehoorde contacten"><option value="">&mdash; gehoorde contacten &mdash;</option></select>
<input id="cm-allow" placeholder="allow-pubkey (64 hex; prefix &ge;12 voor verwijderen)" maxlength="64" spellcheck="false" style="flex:1;min-width:10rem">
<button type="button" onclick="cmAllowAdd()">allow +</button>
<button type="button" onclick="cmAllowDel()">allow &minus;</button>
<button type="button" onclick="cmCmd('!allow list')">allow-lijst</button></div>

<h3 style="margin:.7rem 0 .2rem">Valdetectie</h3>
<div class="quick">
<button type="button" onclick="cmCmd('!fall on')">Fall aan</button>
<button type="button" onclick="cmCmd('!fall off')">Fall uit</button>
<button type="button" onclick="cmCmd('!fall status')">status</button>
<button type="button" onclick="cmFallTest()">test (pre-alarm)</button></div>
<div class="quick" style="margin-top:.4rem">
<span style="align-self:center;color:var(--muted);font-size:.8rem">MeshManager-koppeling:</span>
<button type="button" onclick="cmCmd('!fall mm on')">mm aan</button>
<button type="button" onclick="cmCmd('!fall mm off')">mm uit</button></div>
<div class="frow" style="margin-top:.45rem">
<label style="align-self:center">Gevoeligheid<select id="cm-fsens" style="width:auto;margin-left:.3rem"><option value="low">laag</option><option value="med" selected>midden</option><option value="high">hoog</option></select></label>
<button type="button" onclick="cmCmd('!fall sens '+document.getElementById('cm-fsens').value)">stuur gevoeligheid</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Geen-beweging<input id="cm-fnomotion" type="number" min="0" max="1440" value="0" style="width:5rem;margin-left:.3rem" title="minuten; 0 = uit"> min</label>
<button type="button" onclick="cmCmd('!fall nomotion '+(parseInt(document.getElementById('cm-fnomotion').value,10)||0))">stuur dead-man</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Pre-alarm<input id="cm-fprealarm" type="number" min="0" max="300" value="30" style="width:5rem;margin-left:.3rem"> s</label>
<button type="button" onclick="cmCmd('!fall prealarm '+(parseInt(document.getElementById('cm-fprealarm').value,10)||0))">stuur pre-alarm</button></div>
<div class="frow" style="margin-top:.4rem">
<select id="cm-ftarget-pick" style="width:auto" title="kies uit gehoorde contacten"><option value="">&mdash; gehoorde contacten &mdash;</option></select>
<input id="cm-ftarget" placeholder="doel-pubkey (64 hex; prefix &ge;12 voor verwijderen)" maxlength="64" spellcheck="false" style="flex:1;min-width:10rem">
<button type="button" onclick="cmFallTargetAdd()">doel +</button>
<button type="button" onclick="cmFallTargetDel()">doel &minus;</button>
<button type="button" onclick="cmCmd('!fall target list')">doel-lijst</button></div>
<div id="cmcmdmsg"></div>
<p class="note">Elke knop stuurt het <code>!</code>-commando als bot-DM naar de
gekozen companion. <b>Find</b>/<b>Stop-find</b> laten de companion piepen/knipperen,
<b>Locate</b> vraagt een <code>#LOC</code>-antwoord (dat hierboven de locatie
bijwerkt), <b>Vol</b> 0&ndash;3, <b>Tune</b> koppelt een buzzer-preset aan een
ernst (H/M/L), <b>Play</b> speelt een preset af.</p>
<p class="note"><b>Valdetectie</b> stuurt de <code>!fall</code>-subcommando's:
<b>aan</b>/<b>uit</b> (<code>!fall on|off</code>), <b>MeshManager-koppeling</b>
(<code>!fall mm on|off</code> &mdash; laat de companion z'n val/SOS ook naar de
MeshManager-server melden), <b>gevoeligheid</b> (<code>!fall sens low|med|high</code>),
<b>geen-beweging</b> (dead-man, <code>!fall nomotion &lt;min&gt;</code>, 0 = uit),
<b>pre-alarm</b> (annuleervenster vóór de SOS gaat, <code>!fall prealarm &lt;sec&gt;</code>).
De <b>doel-lijst</b> is waar de val/SOS-melding heen gaat: <b>doel +</b>
(<code>!fall target add &lt;64hex&gt;</code>) voegt toe, <b>doel &minus;</b>
(<code>!fall target del &lt;prefix&gt;</code>, &ge;12 hex) verwijdert,
<b>doel-lijst</b> (<code>!fall target list</code>) laat de companion z'n huidige
lijst terugmelden; kies uit de gehoorde contacten of plak de sleutel. <b>test</b>
(<code>!fall test</code>) start de pre-alarm nu (annuleerbaar, zonder echt te vallen)
en <b>status</b> (<code>!fall status</code>) laat de companion z'n val-config
terugmelden als DM. Een <code>#LOC</code>-rapport met <code>(val)</code>,
<code>(geen beweging)</code> of <code>(SOS)</code> erin legt bovendien een
<b>val-event</b> vast (zichtbaar in <code>/companions.json</code> als
<code>fall_ts</code>/<code>fall_kind</code>, voor de MeshManager-escalatie). Wat de
companion precies begrijpt hangt van z'n eigen firmware af &mdash; de node stuurt
het commando alleen door.</p>

<h3 style="margin:.9rem 0 .2rem">Radio &mdash; alleen als je weet wat je doet</h3>
<div class="rw"><b>Let op:</b> een radio-instelling <b>kan de companion van de mesh
doen vallen (fysieke seriële recovery nodig)</b>. Freq/BW/SF/CR/tx-power moeten aan
BEIDE kanten gelijk zijn; één verkeerd getal en de companion hoort niemand meer over
LoRa. Elke knop hieronder vraagt eerst een expliciete bevestiging en stuurt dan
<code>!radio &lt;veld&gt; &lt;waarde&gt; confirm</code>. <b>radio show</b> leest alleen.</div>
<div class="quick" style="margin-top:.2rem">
<button type="button" onclick="cmCmd('!radio show')">radio show (lezen)</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Freq (MHz)<input id="cm-rf-freq" placeholder="bv. 869.525" maxlength="12" style="width:7rem;margin-left:.3rem"></label>
<button type="button" class="dng" onclick="cmRadio('freq','cm-rf-freq')">zet freq</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Bandbreedte (kHz)<input id="cm-rf-bw" placeholder="bv. 250" maxlength="8" style="width:6rem;margin-left:.3rem"></label>
<button type="button" class="dng" onclick="cmRadio('bw','cm-rf-bw')">zet bw</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Spreading factor<input id="cm-rf-sf" placeholder="7&ndash;12" maxlength="4" style="width:4rem;margin-left:.3rem"></label>
<button type="button" class="dng" onclick="cmRadio('sf','cm-rf-sf')">zet sf</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Coding rate<input id="cm-rf-cr" placeholder="5&ndash;8" maxlength="4" style="width:4rem;margin-left:.3rem"></label>
<button type="button" class="dng" onclick="cmRadio('cr','cm-rf-cr')">zet cr</button></div>
<div class="frow" style="margin-top:.4rem">
<label style="align-self:center">Tx-power (dBm)<input id="cm-rf-tx" placeholder="bv. 22" maxlength="4" style="width:4rem;margin-left:.3rem"></label>
<button type="button" class="dng" onclick="cmRadio('tx','cm-rf-tx')">zet tx-power</button></div>
<div id="cmradiomsg"></div>
<p class="note"><b>Parity-commando's</b> (naast de knoppen hierboven): <b>Status</b>
(<code>!status</code>), <b>Tunes</b> (<code>!tunes</code>, lijst), <b>Rx-stats</b>
(<code>!rxps</code>), <b>GPS aan/uit</b> (<code>!gps on|off</code>),
<b>Config-preset</b> (<code>!preset 1|2|3</code>), <b>Vol per slot</b>
(<code>!vol H|M|L &lt;0-3&gt;</code> naast de globale <code>!vol &lt;0-3&gt;</code>),
en <b>allow</b> (<code>!allow add &lt;64hex&gt;</code> / <code>!allow del &lt;prefix&gt;</code>,
&ge;12 hex / <code>!allow list</code>). Alle companion-management-DM's gaan via de
<b>MGMT-bot</b> (<code>BE-HSS-DinX-MGMT</code>) als die bestaat, anders via de alert-bot.</p>

<h2>Kaart &mdash; laatst bekende posities</h2>
<div id="cm-map" style="height:18rem;border-radius:8px;overflow:hidden;display:none"></div>
<div id="cm-maptext"></div>
<p class="note">De kaart (OpenStreetMap via Leaflet) verschijnt zodra minstens één
companion een locatie heeft. Leaflet komt van een CDN en werkt dus alleen als de
browser internet heeft; zonder internet valt dit terug op <code>lat,lon</code> als
tekst met een OpenStreetMap-link per companion.</p>
</section>

<!-- QR-generator: qrcode-generator van Kazuhiko Arase (MIT), getrimd tot de
     encoder-kern en geminifieerd. Client-side, geen externe asset, geen C-lib in de
     flash. De matrix is byte-identiek geverifieerd t.o.v. de volledige lib. -->
<script>
var qrcode=function(){var r=function(r,t){var n=r,e=c[t],o=null,u=0,a=null,f=[],i={},g=function(r,t){o=function(r){for(var t=new Array(r),n=0;n<r;n+=1){t[n]=new Array(r);for(var e=0;e<r;e+=1)t[n][e]=null}return t}(u=4*n+17),h(0,0),h(u-7,0),h(0,u-7),s(),l(),d(r,t),n>=7&&v(r),null==a&&(a=p(n,e,f)),w(a,t)},h=function(r,t){for(var n=-1;n<=7;n+=1)if(!(r+n<=-1||u<=r+n))for(var e=-1;e<=7;e+=1)t+e<=-1||u<=t+e||(o[r+n][t+e]=0<=n&&n<=6&&(0==e||6==e)||0<=e&&e<=6&&(0==n||6==n)||2<=n&&n<=4&&2<=e&&e<=4)},l=function(){for(var r=8;r<u-8;r+=1)null==o[r][6]&&(o[r][6]=r%2==0);for(var t=8;t<u-8;t+=1)null==o[6][t]&&(o[6][t]=t%2==0)},s=function(){for(var r=A.getPatternPosition(n),t=0;t<r.length;t+=1)for(var e=0;e<r.length;e+=1){var u=r[t],a=r[e];if(null==o[u][a])for(var f=-2;f<=2;f+=1)for(var i=-2;i<=2;i+=1)o[u+f][a+i]=-2==f||2==f||-2==i||2==i||0==f&&0==i}},v=function(r){for(var t=A.getBCHTypeNumber(n),e=0;e<18;e+=1){var a=!r&&1==(t>>e&1);o[Math.floor(e/3)][e%3+u-8-3]=a}for(e=0;e<18;e+=1){a=!r&&1==(t>>e&1);o[e%3+u-8-3][Math.floor(e/3)]=a}},d=function(r,t){for(var n=e<<3|t,a=A.getBCHTypeInfo(n),f=0;f<15;f+=1){var i=!r&&1==(a>>f&1);f<6?o[f][8]=i:f<8?o[f+1][8]=i:o[u-15+f][8]=i}for(f=0;f<15;f+=1){i=!r&&1==(a>>f&1);f<8?o[8][u-f-1]=i:f<9?o[8][15-f-1+1]=i:o[8][15-f-1]=i}o[u-8][8]=!r},w=function(r,t){for(var n=-1,e=u-1,a=7,f=0,i=A.getMaskFunction(t),g=u-1;g>0;g-=2)for(6==g&&(g-=1);;){for(var c=0;c<2;c+=1)if(null==o[e][g-c]){var h=!1;f<r.length&&(h=1==(r[f]>>>a&1)),i(e,g-c)&&(h=!h),o[e][g-c]=h,-1==(a-=1)&&(f+=1,a=7)}if((e+=n)<0||u<=e){e-=n,n=-n;break}}},p=function(r,t,n){for(var e=B.getRSBlocks(r,t),o=C(),u=0;u<n.length;u+=1){var a=n[u];o.put(a.getMode(),4),o.put(a.getLength(),A.getLengthInBits(a.getMode(),r)),a.write(o)}var f=0;for(u=0;u<e.length;u+=1)f+=e[u].dataCount;if(o.getLengthInBits()>8*f)throw"code length overflow. ("+o.getLengthInBits()+">"+8*f+")";for(o.getLengthInBits()+4<=8*f&&o.put(0,4);o.getLengthInBits()%8!=0;)o.putBit(!1);for(;!(o.getLengthInBits()>=8*f||(o.put(236,8),o.getLengthInBits()>=8*f));)o.put(17,8);return function(r,t){for(var n=0,e=0,o=0,u=new Array(t.length),a=new Array(t.length),f=0;f<t.length;f+=1){var i=t[f].dataCount,g=t[f].totalCount-i;e=Math.max(e,i),o=Math.max(o,g),u[f]=new Array(i);for(var c=0;c<u[f].length;c+=1)u[f][c]=255&r.getBuffer()[c+n];n+=i;var h=A.getErrorCorrectPolynomial(g),l=y(u[f],h.getLength()-1).mod(h);for(a[f]=new Array(h.getLength()-1),c=0;c<a[f].length;c+=1){var s=c+l.getLength()-a[f].length;a[f][c]=s>=0?l.getAt(s):0}}var v=0;for(c=0;c<t.length;c+=1)v+=t[c].totalCount;var d=new Array(v),w=0;for(c=0;c<e;c+=1)for(f=0;f<t.length;f+=1)c<u[f].length&&(d[w]=u[f][c],w+=1);for(c=0;c<o;c+=1)for(f=0;f<t.length;f+=1)c<a[f].length&&(d[w]=a[f][c],w+=1);return d}(o,e)};i.addData=function(r,t){var n=null;switch(t=t||"Byte"){case"Numeric":n=D(r);break;case"Alphanumeric":n=M(r);break;case"Byte":n=m(r);break;case"Kanji":n=b(r);break;default:throw"mode:"+t}f.push(n),a=null},i.isDark=function(r,t){if(r<0||u<=r||t<0||u<=t)throw r+","+t;return o[r][t]},i.getModuleCount=function(){return u},i.make=function(){if(n<1){for(var r=1;r<40;r++){for(var t=B.getRSBlocks(r,e),o=C(),u=0;u<f.length;u++){var a=f[u];o.put(a.getMode(),4),o.put(a.getLength(),A.getLengthInBits(a.getMode(),r)),a.write(o)}var c=0;for(u=0;u<t.length;u++)c+=t[u].dataCount;if(o.getLengthInBits()<=8*c)break}n=r}g(!1,function(){for(var r=0,t=0,n=0;n<8;n+=1){g(!0,n);var e=A.getLostPoint(i);(0==n||r>e)&&(r=e,t=n)}return t}())};return i};r.stringToBytes=(r.stringToBytesFuncs={default:function(r){for(var t=[],n=0;n<r.length;n+=1){var e=r.charCodeAt(n);t.push(255&e)}return t}}).default;var t,n,e,o,u,a=1,f=2,i=4,g=8,c={L:1,M:0,Q:3,H:2},h=0,l=1,s=2,v=3,d=4,w=5,p=6,k=7,A=(t=[[],[6,18],[6,22],[6,26],[6,30],[6,34],[6,22,38],[6,24,42],[6,26,46],[6,28,50],[6,30,54],[6,32,58],[6,34,62],[6,26,46,66],[6,26,48,70],[6,26,50,74],[6,30,54,78],[6,30,56,82],[6,30,58,86],[6,34,62,90],[6,28,50,72,94],[6,26,50,74,98],[6,30,54,78,102],[6,28,54,80,106],[6,32,58,84,110],[6,30,58,86,114],[6,34,62,90,118],[6,26,50,74,98,122],[6,30,54,78,102,126],[6,26,52,78,104,130],[6,30,56,82,108,134],[6,34,60,86,112,138],[6,30,58,86,114,142],[6,34,62,90,118,146],[6,30,54,78,102,126,150],[6,24,50,76,102,128,154],[6,28,54,80,106,132,158],[6,32,58,84,110,136,162],[6,26,54,82,110,138,166],[6,30,58,86,114,142,170]],n=1335,e=7973,u=function(r){for(var t=0;0!=r;)t+=1,r>>>=1;return t},(o={}).getBCHTypeInfo=function(r){for(var t=r<<10;u(t)-u(n)>=0;)t^=n<<u(t)-u(n);return 21522^(r<<10|t)},o.getBCHTypeNumber=function(r){for(var t=r<<12;u(t)-u(e)>=0;)t^=e<<u(t)-u(e);return r<<12|t},o.getPatternPosition=function(r){return t[r-1]},o.getMaskFunction=function(r){switch(r){case h:return function(r,t){return(r+t)%2==0};case l:return function(r,t){return r%2==0};case s:return function(r,t){return t%3==0};case v:return function(r,t){return(r+t)%3==0};case d:return function(r,t){return(Math.floor(r/2)+Math.floor(t/3))%2==0};case w:return function(r,t){return r*t%2+r*t%3==0};case p:return function(r,t){return(r*t%2+r*t%3)%2==0};case k:return function(r,t){return(r*t%3+(r+t)%2)%2==0};default:throw"bad maskPattern:"+r}},o.getErrorCorrectPolynomial=function(r){for(var t=y([1],0),n=0;n<r;n+=1)t=t.multiply(y([1,L.gexp(n)],0));return t},o.getLengthInBits=function(r,t){if(1<=t&&t<10)switch(r){case a:return 10;case f:return 9;case i:case g:return 8;default:throw"mode:"+r}else if(t<27)switch(r){case a:return 12;case f:return 11;case i:return 16;case g:return 10;default:throw"mode:"+r}else{if(!(t<41))throw"type:"+t;switch(r){case a:return 14;case f:return 13;case i:return 16;case g:return 12;default:throw"mode:"+r}}},o.getLostPoint=function(r){for(var t=r.getModuleCount(),n=0,e=0;e<t;e+=1)for(var o=0;o<t;o+=1){for(var u=0,a=r.isDark(e,o),f=-1;f<=1;f+=1)if(!(e+f<0||t<=e+f))for(var i=-1;i<=1;i+=1)o+i<0||t<=o+i||0==f&&0==i||a==r.isDark(e+f,o+i)&&(u+=1);u>5&&(n+=3+u-5)}for(e=0;e<t-1;e+=1)for(o=0;o<t-1;o+=1){var g=0;r.isDark(e,o)&&(g+=1),r.isDark(e+1,o)&&(g+=1),r.isDark(e,o+1)&&(g+=1),r.isDark(e+1,o+1)&&(g+=1),0!=g&&4!=g||(n+=3)}for(e=0;e<t;e+=1)for(o=0;o<t-6;o+=1)r.isDark(e,o)&&!r.isDark(e,o+1)&&r.isDark(e,o+2)&&r.isDark(e,o+3)&&r.isDark(e,o+4)&&!r.isDark(e,o+5)&&r.isDark(e,o+6)&&(n+=40);for(o=0;o<t;o+=1)for(e=0;e<t-6;e+=1)r.isDark(e,o)&&!r.isDark(e+1,o)&&r.isDark(e+2,o)&&r.isDark(e+3,o)&&r.isDark(e+4,o)&&!r.isDark(e+5,o)&&r.isDark(e+6,o)&&(n+=40);var c=0;for(o=0;o<t;o+=1)for(e=0;e<t;e+=1)r.isDark(e,o)&&(c+=1);return n+=Math.abs(100*c/t/t-50)/5*10},o),L=function(){for(var r=new Array(256),t=new Array(256),n=0;n<8;n+=1)r[n]=1<<n;for(n=8;n<256;n+=1)r[n]=r[n-4]^r[n-5]^r[n-6]^r[n-8];for(n=0;n<255;n+=1)t[r[n]]=n;var e={glog:function(r){if(r<1)throw"glog("+r+")";return t[r]},gexp:function(t){for(;t<0;)t+=255;for(;t>=256;)t-=255;return r[t]}};return e}();function y(r,t){if(void 0===r.length)throw r.length+"/"+t;var n=function(){for(var n=0;n<r.length&&0==r[n];)n+=1;for(var e=new Array(r.length-n+t),o=0;o<r.length-n;o+=1)e[o]=r[o+n];return e}(),e={getAt:function(r){return n[r]},getLength:function(){return n.length},multiply:function(r){for(var t=new Array(e.getLength()+r.getLength()-1),n=0;n<e.getLength();n+=1)for(var o=0;o<r.getLength();o+=1)t[n+o]^=L.gexp(L.glog(e.getAt(n))+L.glog(r.getAt(o)));return y(t,0)},mod:function(r){if(e.getLength()-r.getLength()<0)return e;for(var t=L.glog(e.getAt(0))-L.glog(r.getAt(0)),n=new Array(e.getLength()),o=0;o<e.getLength();o+=1)n[o]=e.getAt(o);for(o=0;o<r.getLength();o+=1)n[o]^=L.gexp(L.glog(r.getAt(o))+t);return y(n,0).mod(r)}};return e}var B=function(){var r=[[1,26,19],[1,26,16],[1,26,13],[1,26,9],[1,44,34],[1,44,28],[1,44,22],[1,44,16],[1,70,55],[1,70,44],[2,35,17],[2,35,13],[1,100,80],[2,50,32],[2,50,24],[4,25,9],[1,134,108],[2,67,43],[2,33,15,2,34,16],[2,33,11,2,34,12],[2,86,68],[4,43,27],[4,43,19],[4,43,15],[2,98,78],[4,49,31],[2,32,14,4,33,15],[4,39,13,1,40,14],[2,121,97],[2,60,38,2,61,39],[4,40,18,2,41,19],[4,40,14,2,41,15],[2,146,116],[3,58,36,2,59,37],[4,36,16,4,37,17],[4,36,12,4,37,13],[2,86,68,2,87,69],[4,69,43,1,70,44],[6,43,19,2,44,20],[6,43,15,2,44,16],[4,101,81],[1,80,50,4,81,51],[4,50,22,4,51,23],[3,36,12,8,37,13],[2,116,92,2,117,93],[6,58,36,2,59,37],[4,46,20,6,47,21],[7,42,14,4,43,15],[4,133,107],[8,59,37,1,60,38],[8,44,20,4,45,21],[12,33,11,4,34,12],[3,145,115,1,146,116],[4,64,40,5,65,41],[11,36,16,5,37,17],[11,36,12,5,37,13],[5,109,87,1,110,88],[5,65,41,5,66,42],[5,54,24,7,55,25],[11,36,12,7,37,13],[5,122,98,1,123,99],[7,73,45,3,74,46],[15,43,19,2,44,20],[3,45,15,13,46,16],[1,135,107,5,136,108],[10,74,46,1,75,47],[1,50,22,15,51,23],[2,42,14,17,43,15],[5,150,120,1,151,121],[9,69,43,4,70,44],[17,50,22,1,51,23],[2,42,14,19,43,15],[3,141,113,4,142,114],[3,70,44,11,71,45],[17,47,21,4,48,22],[9,39,13,16,40,14],[3,135,107,5,136,108],[3,67,41,13,68,42],[15,54,24,5,55,25],[15,43,15,10,44,16],[4,144,116,4,145,117],[17,68,42],[17,50,22,6,51,23],[19,46,16,6,47,17],[2,139,111,7,140,112],[17,74,46],[7,54,24,16,55,25],[34,37,13],[4,151,121,5,152,122],[4,75,47,14,76,48],[11,54,24,14,55,25],[16,45,15,14,46,16],[6,147,117,4,148,118],[6,73,45,14,74,46],[11,54,24,16,55,25],[30,46,16,2,47,17],[8,132,106,4,133,107],[8,75,47,13,76,48],[7,54,24,22,55,25],[22,45,15,13,46,16],[10,142,114,2,143,115],[19,74,46,4,75,47],[28,50,22,6,51,23],[33,46,16,4,47,17],[8,152,122,4,153,123],[22,73,45,3,74,46],[8,53,23,26,54,24],[12,45,15,28,46,16],[3,147,117,10,148,118],[3,73,45,23,74,46],[4,54,24,31,55,25],[11,45,15,31,46,16],[7,146,116,7,147,117],[21,73,45,7,74,46],[1,53,23,37,54,24],[19,45,15,26,46,16],[5,145,115,10,146,116],[19,75,47,10,76,48],[15,54,24,25,55,25],[23,45,15,25,46,16],[13,145,115,3,146,116],[2,74,46,29,75,47],[42,54,24,1,55,25],[23,45,15,28,46,16],[17,145,115],[10,74,46,23,75,47],[10,54,24,35,55,25],[19,45,15,35,46,16],[17,145,115,1,146,116],[14,74,46,21,75,47],[29,54,24,19,55,25],[11,45,15,46,46,16],[13,145,115,6,146,116],[14,74,46,23,75,47],[44,54,24,7,55,25],[59,46,16,1,47,17],[12,151,121,7,152,122],[12,75,47,26,76,48],[39,54,24,14,55,25],[22,45,15,41,46,16],[6,151,121,14,152,122],[6,75,47,34,76,48],[46,54,24,10,55,25],[2,45,15,64,46,16],[17,152,122,4,153,123],[29,74,46,14,75,47],[49,54,24,10,55,25],[24,45,15,46,46,16],[4,152,122,18,153,123],[13,74,46,32,75,47],[48,54,24,14,55,25],[42,45,15,32,46,16],[20,147,117,4,148,118],[40,75,47,7,76,48],[43,54,24,22,55,25],[10,45,15,67,46,16],[19,148,118,6,149,119],[18,75,47,31,76,48],[34,54,24,34,55,25],[20,45,15,61,46,16]],t=function(r,t){var n={};return n.totalCount=r,n.dataCount=t,n},n={};return n.getRSBlocks=function(n,e){var o=function(t,n){switch(n){case c.L:return r[4*(t-1)+0];case c.M:return r[4*(t-1)+1];case c.Q:return r[4*(t-1)+2];case c.H:return r[4*(t-1)+3];default:return}}(n,e);if(void 0===o)throw"bad rs block @ typeNumber:"+n+"/errorCorrectionLevel:"+e;for(var u=o.length/3,a=[],f=0;f<u;f+=1)for(var i=o[3*f+0],g=o[3*f+1],h=o[3*f+2],l=0;l<i;l+=1)a.push(t(g,h));return a},n}(),C=function(){var r=[],t=0,n={getBuffer:function(){return r},getAt:function(t){var n=Math.floor(t/8);return 1==(r[n]>>>7-t%8&1)},put:function(r,t){for(var e=0;e<t;e+=1)n.putBit(1==(r>>>t-e-1&1))},getLengthInBits:function(){return t},putBit:function(n){var e=Math.floor(t/8);r.length<=e&&r.push(0),n&&(r[e]|=128>>>t%8),t+=1}};return n},D=function(r){var t=a,n=r,e={getMode:function(){return t},getLength:function(r){return n.length},write:function(r){for(var t=n,e=0;e+2<t.length;)r.put(o(t.substring(e,e+3)),10),e+=3;e<t.length&&(t.length-e==1?r.put(o(t.substring(e,e+1)),4):t.length-e==2&&r.put(o(t.substring(e,e+2)),7))}},o=function(r){for(var t=0,n=0;n<r.length;n+=1)t=10*t+u(r.charAt(n));return t},u=function(r){if("0"<=r&&r<="9")return r.charCodeAt(0)-"0".charCodeAt(0);throw"illegal char :"+r};return e},M=function(r){var t=f,n=r,e={getMode:function(){return t},getLength:function(r){return n.length},write:function(r){for(var t=n,e=0;e+1<t.length;)r.put(45*o(t.charAt(e))+o(t.charAt(e+1)),11),e+=2;e<t.length&&r.put(o(t.charAt(e)),6)}},o=function(r){if("0"<=r&&r<="9")return r.charCodeAt(0)-"0".charCodeAt(0);if("A"<=r&&r<="Z")return r.charCodeAt(0)-"A".charCodeAt(0)+10;switch(r){case" ":return 36;case"$":return 37;case"%":return 38;case"*":return 39;case"+":return 40;case"-":return 41;case".":return 42;case"/":return 43;case":":return 44;default:throw"illegal char :"+r}};return e},m=function(t){var n=i,e=r.stringToBytes(t),o={getMode:function(){return n},getLength:function(r){return e.length},write:function(r){for(var t=0;t<e.length;t+=1)r.put(e[t],8)}};return o},b=function(t){var n=g,e=r.stringToBytesFuncs.SJIS;if(!e)throw"sjis not supported.";!function(){var r=e("友");if(2!=r.length||38726!=(r[0]<<8|r[1]))throw"sjis not supported."}();var o=e(t),u={getMode:function(){return n},getLength:function(r){return~~(o.length/2)},write:function(r){for(var t=o,n=0;n+1<t.length;){var e=(255&t[n])<<8|255&t[n+1];if(33088<=e&&e<=40956)e-=33088;else{if(!(57408<=e&&e<=60351))throw"illegal char at "+(n+1)+"/"+e;e-=49472}e=192*(e>>>8&255)+(255&e),r.put(e,13),n+=2}if(n<t.length)throw"illegal char at "+(n+1)}};return u};return r}();"undefined"!=typeof window&&(window.qrcode=qrcode);
</script>

<script>
/* ===================== sessie: 401 -> naar de login =======================
 *
 * De pagina praat alleen via fetch() met de node, en een fetch() opent GEEN
 * inlogpopup bij een 401 (anders dan een navigatie of een <img>). Als een sessie
 * verloopt terwijl de pagina openstaat, komt dat dus terug als een 401 in elke
 * verversing. We vangen dat op EEN plek op door fetch() eenmalig te omwikkelen: bij
 * een 401 sturen we de browser naar de eigen inlogpagina in plaats van de gebruiker
 * met stille fouten te laten zitten. Alle bestaande aanroepen (status.json, cli,
 * acl.json, cfg.json, web/cred, ...) lopen hier vanzelf doorheen. */
(function(){var of=window.fetch;window.fetch=function(u,o){
return of(u,o).then(function(r){if(r.status==401){location.replace("login")}return r})}})();

/* Afmelden: de sessie wissen (POST /logout wist ze ook aan de nodekant) en naar de
   inlogpagina. Faalt de POST, dan toch naar /login -- de cookie is dan hoogstens nog
   lokaal geldig en de login vraagt sowieso opnieuw. */
document.getElementById("lo").onclick=function(){
fetch("logout",{method:"POST"}).then(function(){location.replace("login")})
.catch(function(){location.replace("login")})};

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

/* DE AD-HOC PING-UITSLAG. Verschijnt onder de tegels zodra een `ping <adres>` uit
   de CLI-console klaar is (of terwijl hij loopt: "bezig"). Cyaan zoals de andere
   'waarom'-kaders, want dit is informatie en geen alarm. De uitslag komt uit
   /status.json -- dezelfde die de DM-variant vult -- dus de console hoefde er niet
   op te wachten. */
function pingres(d){
var e=document.getElementById("pingres");var a=d.adhoc;
if(!a||a.st=="niets"){e.className="";e.innerHTML="";return}
e.className="why";
if(a.st=="klaar"){e.innerHTML="<b>ping-uitslag:</b> "+a.txt}
else{e.innerHTML="<b>ping bezig</b> naar "+(a.host||"?")+
" &mdash; de uitslag verschijnt hier zodra alle pings klaar zijn"}}

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

/* Alarm-route (dm/room/both) + room-set (bv "0,1"). Alleen zinvol op de
   room-variant; op een sensor-node worden ze bewaard maar genegeerd. am/rm komen
   uit status.json. */
function m2l(mk){var a=[];for(var b=0;b<16;b++){if(mk&(1<<b))a.push(b)}return a.join(",")}
function l2m(s){var m=0;(""+s).split(",").forEach(function(x){var n=parseInt(x,10);if(n>=0&&n<16)m|=(1<<n)});return m}
var iA=document.createElement("select");iA.className="n3";
[["1","dm"],["2","room"],["3","both"]].forEach(function(o){var op=document.createElement("option");op.value=o[0];op.textContent=o[1];if(o[0]===String(m.am||3))op.selected=true;iA.appendChild(op)});
var iR=document.createElement("input");iR.className="n3";iR.maxLength=12;iR.value=m2l(m.rm||1);iR.title="rooms (post-tekst), bv 0,1";
/* sn = op welke sensor-nodes dit kanaal als telemetrie verschijnt (bv 0,1). */
var iS=document.createElement("input");iS.className="n3";iS.maxLength=12;iS.value=m2l(m.sn||1);iS.title="sensor-nodes (telemetrie), bv 0,1";
/* e = ernst -> de emoji vooraan de storings-DM waarop de T1000-E-companion zijn
   buzzer-tune kiest. 0 hoog (rood), 1 midden (oranje), 2 laag (groen). */
var iE=document.createElement("select");iE.className="n3";iE.title="ernst: emoji vooraan de storings-DM (companion-buzzer). Herstel is altijd groen.";
[["0","🔴 hoog"],["1","🟠 midden"],["2","🟢 laag"]].forEach(function(o){var op=document.createElement("option");op.value=o[0];op.textContent=o[1];if(o[0]===String(m.msv||0))op.selected=true;iE.appendChild(op)});
cells[3].appendChild(document.createTextNode(" a:"));cells[3].appendChild(iA);
cells[3].appendChild(document.createTextNode(" r:"));cells[3].appendChild(iR);
cells[3].appendChild(document.createTextNode(" s:"));cells[3].appendChild(iS);
cells[3].appendChild(document.createTextNode(" e:"));cells[3].appendChild(iE);

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

/* Alarm-route (am) + room-set (rm) + sensor-node-set (sn) gaan NIET via 'sensor
   set' maar via /mon/alarm (op het stabiele kanaal m.ch). Alleen versturen als er
   iets aan veranderd is. */
var nam=parseInt(iA.value,10),nrm=l2m(iR.value),nsn=l2m(iS.value),nsv=parseInt(iE.value,10);
var alarmChg=(nam!=(m.am||3))||(nrm!=(m.rm||1))||(nsn!=(m.sn||1))||(nsv!=(m.msv||0));
function saveAlarm(cb){
if(!alarmChg){if(cb)cb();return}
fetch("mon/alarm",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"ch="+m.ch+"&am="+nam+"&rm="+nrm+"&sn="+nsn+"&sev="+nsv})
.then(function(r){return r.json().catch(function(){return{ok:r.ok}})})
.then(function(j){if(!j.ok)tsay("alarm/koppeling kanaal "+m.ch+" geweigerd: "+(j.error||""),0);
if(cb)cb()}).catch(function(){tsay("alarm/koppeling niet opgeslagen",0);if(cb)cb()})}

if(!cmds.length){
if(alarmChg){ok.disabled=true;no.disabled=true;
saveAlarm(function(){EDIT=0;u2();cfg();tsay("alarm/koppeling van kanaal "+m.ch+" opgeslagen",1)})}
else{tsay("niets veranderd aan kanaal "+m.ch,1);no.onclick()}
return}

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
EDIT=0;rowsayClear(r);saveAlarm();u2();cfg();
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

var MONS=[];   // laatste status.json-monitors, voor het kanaalbeheer-panel
function u2(){return fetch("status.json").then(function(r){return r.json()})
.then(function(d){MONS=d.mon||[];tiles(d);simban(d);pingres(d);table(d);tbud(d);deliv(d);recui(d);timui(d);
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
for(var k=1;k<=7;k++){document.getElementById("p"+k).hidden=(""+k)!=p}
if(p=="3"){cfg()}
if(p=="4"){roomsLoad();refreshPickers()}
if(p=="5"){snodesLoad();refreshPickers()}
if(p=="6"){botLoad()}
if(p=="7"){companionsLoad()}}}

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
/* Getypte 'ping' gaat als 'icmp' de draad op: een IPS onderweg (zie de route in
   handleCli) doodt HTTP-bodies met kleine-letters ping+spatie. Zelfde lengte,
   zelfde engine; alleen het woord op de draad verschilt. */
if(/^\s*ping(\s|$)/i.test(cmd)){cmd=cmd.replace(/^\s*ping/i,"icmp")}
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
document.getElementById("wcu").value=d.webuser||"";
pwalarm(d);webal(d);budget(d);timeui(d)})})}

/* ---- tijd: NTP-server + tijdzone + sync-status ---- */
function timeui(d){
var ntp=document.getElementById("ntp"),tz=document.getElementById("tz");
if(ntp&&document.activeElement!=ntp)ntp.value=d.ntp||"";
if(tz&&document.activeElement!=tz)tz.value=d.tz||"";
var now=document.getElementById("tnow");if(now)now.textContent=d.tlocal?("nu: "+d.tlocal):"";
var sy=document.getElementById("tsync");
if(sy){if(d.tsync){var a=d.tsyncage||0,ago=a<90?(a+" s"):(a<5400?(Math.round(a/60)+" min"):(Math.round(a/3600)+" u"));
sy.textContent=(d.tsyncmsg||"gesynct")+" ("+ago+" geleden)"}
else sy.textContent=(d.tsyncmsg&&d.tsyncmsg.length)?d.tsyncmsg:"nog niet gesynct"}}
(function(){var b=document.getElementById("tsave");if(!b)return;
b.onclick=function(){var ntp=document.getElementById("ntp").value.trim(),
tz=document.getElementById("tz").value.trim();
fetch("time",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"ntp="+encodeURIComponent(ntp)+"&tz="+encodeURIComponent(tz)})
.then(function(r){return r.json().catch(function(){return{ok:r.ok}})})
.then(function(j){var m=document.getElementById("tmsg");
if(j.ok){m.textContent="opgeslagen; sync aangevraagd";m.className="ok";
setTimeout(cfg,1500);setTimeout(cfg,6000)}
else{m.textContent="mislukt: "+(j.error||"");m.className="x"}
if(m.textContent)setTimeout(function(){if(m.textContent.indexOf("opgeslagen")==0)m.textContent=""},6000)})
.catch(function(){var m=document.getElementById("tmsg");m.textContent="mislukt";m.className="x"})}})();

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

/* HET WEB-LOGIN-ALARM. Rood zolang de web-login (de Basic-auth van deze pagina) nog
   de gebakken, vlootbrede standaard is, en groen zodra deze node een eigen login
   heeft. Zelfde vorm en zelfde reden als het wachtwoordalarm: een node op de
   gedeelde standaard is een zwakte die zichtbaar hoort te zijn, want één lek opent
   dan de hele vloot. De gebruikersnaam gaat NIET rauw in innerHTML (dat zou een
   injectiegat zijn); hij staat veilig in het invoerveld hieronder, gezet in cfg(). */
function webal(d){
var e=document.getElementById("webal");if(!e){return}
if(!d.webcustom){
e.className="alarm";
e.innerHTML="<b>de web-login staat nog op de gebakken standaard</b>"+
"De ingebouwde gebruiker en het ingebouwde wachtwoord zijn dezelfde vlootbrede "+
"login als op elke andere node en de repeater — één lek opent ze "+
"allemaal. Geef deze node onder <i>Web-login</i> hieronder een eigen gebruiker en "+
"wachtwoord."}
else{e.className="alarm ok";
e.innerHTML="<b>eigen web-login gezet</b>Deze node heeft een eigen gebruiker en "+
"wachtwoord, los van de rest van de vloot. Het wachtwoord wordt nooit getoond."}}

/* De web-login zetten: POST /web/cred, form-urlencoded, precies het contract dat de
   statsserver ook gebruikt. Na succes vraagt de browser bij het volgende verzoek om
   de nieuwe inlog; we wissen het wachtwoordveld en verversen cfg(). */
document.getElementById("wcgo").onclick=function(){
var u=document.getElementById("wcu").value.trim();
var p=document.getElementById("wcp").value;
if(!u){logline("(web-login)","gebruiker mag niet leeg zijn",0);return}
if(!p){logline("(web-login)","wachtwoord mag niet leeg zijn — een lege pass "+
"zet de node open",0);return}
if(!confirm("Web-login van DEZE node wijzigen?\n\nGebruiker: "+u+"\n\nJe blijft "+
"ingelogd (je sessiecookie verandert niet mee); de nieuwe login geldt bij het "+
"volgende inloggen. Het beheerderswachtwoord (login over het mesh) verandert hier "+
"NIET mee.")){return}
fetch("web/cred",{method:"POST",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"user="+encodeURIComponent(u)+"&pass="+encodeURIComponent(p)})
.then(function(r){return r.text().then(function(t){
if(r.ok){logline("(web-login)","opgeslagen; geldt bij het volgende inloggen",1);
document.getElementById("wcp").value="";cfg()}
else{logline("(web-login)",t.trim()||("fout "+r.status),0)}})})
.catch(function(){logline("(web-login)","geen verbinding met de node",0)})}

/* Terug naar de gebakken standaard (admin/meshcore): POST /web/cred/reset wist
   /web.cfg. Twee keer bevestigen zit er niet op, maar de tekst is duidelijk over
   het gevolg. Je blijft ingelogd; de standaard geldt bij het volgende inloggen. */
document.getElementById("wcreset").onclick=function(){
if(!confirm("Web-login terugzetten op de GEBAKKEN standaard admin/meshcore?\n\nDe "+
"opgeslagen eigen login (/web.cfg) wordt verwijderd. Dat is de vlootbrede standaard "+
"-- geef deze node daarna weer een eigen login. Doorgaan?")){return}
fetch("web/cred/reset",{method:"POST"})
.then(function(r){return r.text().then(function(t){
if(r.ok){logline("(web-login)","teruggezet op admin/meshcore; geldt bij het "+
"volgende inloggen",1);cfg()}
else{logline("(web-login)",t.trim()||("fout "+r.status),0)}})})
.catch(function(){logline("(web-login)","geen verbinding met de node",0)})}

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

/* SNMP-monitor toevoegen: eigen endpoint /monitor/snmp. */
document.getElementById("asnmp").onsubmit=function(ev){ev.preventDefault();
var f=ev.target.elements;
var sm=document.getElementById("smsg");
fetch("monitor/snmp",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"name="+encodeURIComponent(f["name"].value)+
"&host="+encodeURIComponent(f["host"].value)+
"&int="+encodeURIComponent(f["int"].value)+
"&community="+encodeURIComponent(f["community"].value)+
"&oid="+encodeURIComponent(f["oid"].value)+
"&interp="+encodeURIComponent(f["interp"].value)+
"&snmparg="+encodeURIComponent(f["snmparg"].value)})
.then(function(r){return r.text().then(function(t){
sm.className=r.ok?"ok":"bad";sm.textContent=t.trim();
if(r.ok){f["name"].value="";f["oid"].value="";f["community"].value="";u2();cfg()}})})
.catch(function(){sm.className="bad";sm.textContent="verbindingsfout"})};

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
var WASREC={on:null,hold:null,rep:null};
document.getElementById("rgo").onclick=function(){
var on=document.getElementById("recon").checked?1:0;
var h=parseInt(document.getElementById("rhold").value,10);
var rp=parseInt(document.getElementById("arep").value,10);
if(!(h>=0&&h<=3600)){ssay("rust: 0 t/m 3600 s",0);return}
if(!(rp===0||(rp>=60&&rp<=3600))){ssay("herhalen: 0 (uit) of 60 t/m 3600 s",0);return}
var cmds=[];
if(WASREC.on===null||on!=WASREC.on){cmds.push("sensor set alert.recover "+on)}
if(WASREC.hold===null||h!=WASREC.hold){cmds.push("sensor set alert.rhold "+h)}
if(WASREC.rep===null||rp!=WASREC.rep){cmds.push("sensor set alert.repeat "+rp)}
if(!cmds.length){ssay("niets veranderd",1);return}
ssay("bezig...",1);
cliSeq(cmds,function(good,bad,last){
if(bad){ssay(bad+" van de "+(good+bad)+" geweigerd: "+last.trim(),0)}
else{ssay("opgeslagen: herstel "+(on?"aan":"uit")+", rust "+h+" s, herhalen "+
(rp?rp+" s":"uit"),1)}
u2()})}

/* De velden bijwerken uit status.json -- maar NIET terwijl iemand erin bezig is.
   Daarom alleen als de waarde nog overeenkomt met wat er het laatst uit de node
   kwam; wie iets getypt heeft, houdt zijn tekst. Dezelfde regel als bij het
   bewerken van een tabelregel, en om dezelfde reden. */
function recui(d){
var r=d.rec,rep=d.rep;if(!r){return}
var cb=document.getElementById("recon"),hf=document.getElementById("rhold");
if(WASREC.on===null||cb.checked==(WASREC.on==1)){cb.checked=r.on==1}
if(WASREC.hold===null||hf.value==""+WASREC.hold){hf.value=r.hold}
WASREC.on=r.on;WASREC.hold=r.hold;
document.getElementById("rholdcur").textContent=
"nu: "+(r.on?"aan":"uit")+", rust "+r.hold+" s"+(r.hold?"":" (meteen melden)");
if(rep){
var af=document.getElementById("arep");
if(WASREC.rep===null||af.value==""+WASREC.rep){af.value=rep.secs}
WASREC.rep=rep.secs;
document.getElementById("arepcur").textContent=
"nu: "+(rep.secs?"elke "+rep.secs+" s (tot 'ok'), max "+rep.cap+"x":"uit");
/* DE PIEP-BANNER: hoeveel meldingen nu ONBEVESTIGD herhaald worden, en hoeveel er
   de bovengrens raakten. Amber zolang er iets nagt, want dat is een openstaande
   storing die op een mens wacht; dit is precies wat je in één oogopslag wil zien. */
var nb=document.getElementById("nagban");
if(rep.nag||rep.maxed){
nb.className="sb";
nb.innerHTML='<div class="t"><b>'+
(rep.nag?rep.nag+" melding(en) herhalen tot bevestiging":"")+
(rep.nag&&rep.maxed?" &middot; ":"")+
(rep.maxed?rep.maxed+" op de bovengrens gestopt":"")+"</b>"+
"Een companion met alarmrecht stopt ze door <code>ok</code> per DM te sturen. "+
(rep.maxed?"De gestopte meldingen blijven op de pagina als storing zichtbaar; "+
"alleen het herhalen hield op.":"")+"</div>"}
else{nb.className="";nb.innerHTML=""}}}

/* v2.3.6 REACTIETIJD. Zelfde weg als de alarminstellingen: alles over de CLI
   ('sensor set ...'), één zeef en één opslag, en ALLEEN wat veranderd is. WASTIM
   houdt de laatst ontvangen stand bij, zodat wie iets typt zijn waarde houdt tot
   hij opslaat. */
var WASTIM={samp:null,conf:null,set:null,read:null,deb:null};
function tmsg(t,ok){var m=document.getElementById("tim-msg");
m.textContent=t;m.className=ok?"ok":"bad"}
document.getElementById("tim-go").onclick=function(){
var sp=parseInt(document.getElementById("tim-samp").value,10);
var cf=parseInt(document.getElementById("tim-conf").value,10);
var st=parseInt(document.getElementById("tim-set").value,10);
var rd=parseInt(document.getElementById("tim-read").value,10);
var db=parseInt(document.getElementById("tim-deb").value,10);
if(!(sp>=1&&sp<=60)){tmsg("meet-tik: 1 t/m 60 s",0);return}
if(!(cf>=1&&cf<=10)){tmsg("bevestigingen: 1 t/m 10",0);return}
if(!(st>=0&&st<=300)){tmsg("rust: 0 t/m 300 s",0);return}
if(!(rd>=1&&rd<=60)){tmsg("leesronde: 1 t/m 60 s",0);return}
if(!(db>=0&&db<=3600)){tmsg("settle: 0 t/m 3600 s",0);return}
var cmds=[];
if(WASTIM.samp===null||sp!=WASTIM.samp){cmds.push("sensor set power.sample "+sp)}
if(WASTIM.conf===null||cf!=WASTIM.conf){cmds.push("sensor set power.confirm "+cf)}
if(WASTIM.set===null||st!=WASTIM.set){cmds.push("sensor set power.settle "+st)}
if(WASTIM.read===null||rd!=WASTIM.read){cmds.push("sensor set read.interval "+rd)}
if(WASTIM.deb===null||db!=WASTIM.deb){cmds.push("sensor set alert.debounce "+db)}
if(!cmds.length){tmsg("niets veranderd",1);return}
tmsg("bezig...",1);
cliSeq(cmds,function(good,bad,last){
if(bad){tmsg(bad+" van de "+(good+bad)+" geweigerd: "+last.trim(),0)}
else{tmsg("opgeslagen -- werkt meteen (alert nu ~6-10 s)",1)}
u2()})};

/* De velden bijwerken uit status.json, maar niet terwijl iemand erin bezig is --
   dezelfde regel als recui(). */
function timui(d){
var t=d.timing;if(!t){return}
function fld(id,k,val,unit){
var f=document.getElementById(id);
if(f&&(WASTIM[k]===null||f.value==""+WASTIM[k])){f.value=val}
WASTIM[k]=val;
var c=document.getElementById(id+"cur");
if(c){c.textContent="nu: "+val+(unit||"")}}
fld("tim-samp","samp",t.psample," s");
fld("tim-conf","conf",t.pconfirm,"");
fld("tim-set","set",t.psettle," s");
fld("tim-read","read",t.read," s");
fld("tim-deb","deb",t.deb," s")}

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
/* =============================== rooms ==================================
   Alles praat met /rooms.json en de /room/* + /rooms/* endpoints. De QR wordt
   client-side getekend uit de join-URI (qrcode-lib hierboven); niets verlaat deze
   node. Op de sensor-variant geeft /rooms.json 501 -> tab blijft verborgen. */
function rmsg(id,t,ok){var e=document.getElementById(id);if(!e)return;
e.textContent=t||"";e.className=ok?"ok":"x";e.style.margin=t?".4rem 0":"0";
if(t)setTimeout(function(){if(e.textContent==t)e.textContent=""},6000)}

var RM=[];
function roomsGet(){return fetch("rooms.json",{credentials:"include"})
.then(function(r){if(r.status==401){location="/login";throw 0}
if(r.status==501)return null;if(!r.ok)throw 0;return r.json()})}

/* Bij het laden: is dit een room-node? Zo ja, toon de tab. */
function roomsProbe(){roomsGet().then(function(d){
var t=document.getElementById("tabrooms");if(t)t.hidden=!(d&&d.max>0);
var ts=document.getElementById("tabsnodes");if(ts)ts.hidden=!(d&&d.snode_max>0);
var tb=document.getElementById("tabbot");if(tb)tb.hidden=!(d&&d.max>0);
var tc=document.getElementById("tabcomp");if(tc)tc.hidden=!(d&&d.max>0)})
.catch(function(){})}

function roomsLoad(){roomsGet().then(function(d){
var e=document.getElementById("rl");
if(!d){e.innerHTML="<tr><td>Deze node kent geen rooms (sensor-variant).</td></tr>";return}
RM=d.rooms||[];roomsRender(d)}).catch(function(){rmsg("rmsg","kon rooms niet laden",0)})}

function roomsRender(d){var e=document.getElementById("rl");e.innerHTML="";
var h=e.insertRow();["#","naam","stealth","gast","posts",""].forEach(function(t){
var th=document.createElement("th");th.textContent=t;h.appendChild(th)});
d.rooms.forEach(function(rm){var r=e.insertRow();
var c=r.insertCell();c.className="num";c.textContent=rm.idx;
r.insertCell().textContent=rm.name;
c=r.insertCell();c.textContent=rm.stealth?"aan":"–";if(rm.stealth)c.className="c-warn";
c=r.insertCell();c.textContent=rm.guest?"ja":"nee";
c=r.insertCell();c.className="num";c.textContent=rm.posts;
c=r.insertCell();c.className="acts";
var bs=document.createElement("button");bs.textContent="deel";bs.className="go";
bs.onclick=function(){roomShare(rm)};c.appendChild(bs);
var be=document.createElement("button");be.textContent="bewerk";
be.onclick=function(){roomEdit(rm)};c.appendChild(be);
if(rm.idx>0){var bd=document.createElement("button");bd.textContent="wis";
bd.onclick=function(){roomDel(rm)};c.appendChild(bd)}})}

/* ---- delen: QR + join-link ---- */
function drawQR(uri){var qr=qrcode(0,"M");qr.addData(uri);qr.make();
var n=qr.getModuleCount(),s=5,q=4,cv=document.getElementById("rqr");
cv.width=cv.height=(n+q*2)*s;var g=cv.getContext("2d");
g.fillStyle="#fff";g.fillRect(0,0,cv.width,cv.height);g.fillStyle="#000";
for(var y=0;y<n;y+=1)for(var x=0;x<n;x+=1)if(qr.isDark(y,x))g.fillRect((x+q)*s,(y+q)*s,s,s)}
function roomShare(rm){var p=document.getElementById("rshare");p.hidden=false;
document.getElementById("rshare-t").textContent="Deel: "+rm.name;
document.getElementById("rshare-uri").value=rm.uri||"";
try{drawQR(rm.uri)}catch(e){rmsg("rmsg","QR tekenen mislukt",0)}
p.scrollIntoView({block:"nearest"})}
function roomShareClose(){document.getElementById("rshare").hidden=true}
document.getElementById("rcopy").onclick=function(){
var i=document.getElementById("rshare-uri");i.focus();i.select();
if(navigator.clipboard&&navigator.clipboard.writeText){
navigator.clipboard.writeText(i.value).then(function(){rmsg("rmsg","join-link gekopieerd",1)},
function(){rmsg("rmsg","kopiëren niet gelukt — selecteer handmatig",0)})}
else{try{document.execCommand("copy");rmsg("rmsg","join-link gekopieerd",1)}
catch(e){rmsg("rmsg","kopiëren niet gelukt — selecteer handmatig",0)}}};

/* ---- toevoegen ---- */
document.getElementById("radd").onsubmit=function(ev){ev.preventDefault();
var f=ev.target.elements,nm=f["name"].value.trim();if(!nm)return;
fetch("room/add",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"name="+encodeURIComponent(nm)}).then(function(r){return r.json()})
.then(function(j){if(j.ok){f["name"].value="";
rmsg("raddmsg","room "+j.idx+" toegevoegd",1);roomsLoad()}
else rmsg("raddmsg","toevoegen mislukt: "+(j.error||""),0)})
.catch(function(){rmsg("raddmsg","toevoegen mislukt",0)})};

/* ---- bewerken ---- */
var REI=-1;
function roomEdit(rm){REI=rm.idx;document.getElementById("redit").hidden=false;
document.getElementById("redit-t").textContent="Bewerken: room "+rm.idx+" ("+rm.name+")";
document.getElementById("re-name").value=rm.name;
document.getElementById("re-pass").value="";
document.getElementById("re-guest").value="";
document.getElementById("re-guestclear").checked=false;
document.getElementById("re-stealth").checked=!!rm.stealth;
renderAcl("racl",0,rm.idx,rm.acl);
renderChannels("rchan",0,rm.idx);
document.getElementById("redit").scrollIntoView({block:"nearest"})}
function roomEditClose(){document.getElementById("redit").hidden=true;REI=-1}
document.getElementById("re-save").onclick=function(){if(REI<0)return;
var b="idx="+REI,nm=document.getElementById("re-name").value.trim(),
pw=document.getElementById("re-pass").value,
gv=document.getElementById("re-guest").value,
gc=document.getElementById("re-guestclear").checked,
st=document.getElementById("re-stealth").checked?1:0;
if(nm)b+="&name="+encodeURIComponent(nm);
if(pw)b+="&pass="+encodeURIComponent(pw);
if(gc)b+="&guest_clear=1";else if(gv)b+="&guest="+encodeURIComponent(gv);
b+="&stealth="+st;
fetch("room/edit",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){rmsg("rmsg","room "+REI+" opgeslagen",1);roomEditClose();roomsLoad()}
else rmsg("rmsg","opslaan mislukt: "+(j.error||""),0)})
.catch(function(){rmsg("rmsg","opslaan mislukt",0)})};

/* ---- verwijderen ---- */
function roomDel(rm){if(rm.idx<=0)return;
if(!confirm("Room "+rm.idx+" ('"+rm.name+"') verwijderen? De sleutel gaat verloren; "+
"bestaande contacten en QR-codes worden ongeldig."))return;
fetch("room/del",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"idx="+rm.idx})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){rmsg("rmsg","room "+rm.idx+" verwijderd",1);roomEditClose();roomShareClose();roomsLoad()}
else rmsg("rmsg","verwijderen mislukt: "+(j.error||""),0)})
.catch(function(){rmsg("rmsg","verwijderen mislukt",0)})}

/* ---- backup / restore ---- */
document.getElementById("rbackup").onclick=function(){
fetch("rooms/backup",{credentials:"include"}).then(function(r){
if(!r.ok)throw 0;return r.text()}).then(function(t){
var bl=new Blob([t],{type:"application/json"}),u=URL.createObjectURL(bl),
a=document.createElement("a");a.href=u;a.download="meshuptime-rooms-backup.json";
document.body.appendChild(a);a.click();a.remove();URL.revokeObjectURL(u);
rmsg("rrmsg","backup gedownload — bewaar veilig, hij bevat de sleutels",1)})
.catch(function(){rmsg("rrmsg","backup mislukt",0)})};
document.getElementById("rrestore").onchange=function(ev){
var f=ev.target.files[0];if(!f)return;var rd=new FileReader();
rd.onload=function(){var txt=rd.result,ov=document.getElementById("rov").checked;
if(ov)txt=txt.replace("{",'{"overwrite_main":"1",');
if(!confirm("Rooms herstellen uit deze backup? Dit overschrijft de huidige "+
"room-config"+(ov?" INCLUSIEF room 0 (hoofdidentiteit)":"")+".")){ev.target.value="";return}
fetch("rooms/restore",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/json"},body:txt})
.then(function(r){return r.json().catch(function(){return{ok:r.ok}})})
.then(function(j){if(j.ok){rmsg("rrmsg","hersteld",1);roomsLoad()}
else rmsg("rrmsg","restore mislukt: "+(j.error||"onbekend"),0)})
.catch(function(){rmsg("rrmsg","restore mislukt",0)});ev.target.value=""};
rd.readAsText(f)};

/* ============================ sensor-nodes ==============================
   Hergebruikt roomShare()/drawQR() (QR + deel) en rmsg(). De lijst komt uit de
   "snodes"-array van /rooms.json. */
function snodesLoad(){roomsGet().then(function(d){
var e=document.getElementById("snl");
if(!d){e.innerHTML="<tr><td>Deze node kent geen sensor-nodes.</td></tr>";return}
snodesRender(d.snodes||[])}).catch(function(){rmsg("snmsg","kon sensor-nodes niet laden",0)})}

function snodesRender(list){var e=document.getElementById("snl");e.innerHTML="";
var h=e.insertRow();["#","naam","stealth","kanalen",""].forEach(function(t){
var th=document.createElement("th");th.textContent=t;h.appendChild(th)});
list.forEach(function(sn){var r=e.insertRow();
var c=r.insertCell();c.className="num";c.textContent=sn.idx;
r.insertCell().textContent=sn.name;
c=r.insertCell();c.textContent=sn.stealth?"aan":"–";if(sn.stealth)c.className="c-warn";
c=r.insertCell();c.className="num";c.textContent=(sn.channels&&sn.channels.length)?sn.channels.join(","):"–";
c=r.insertCell();c.className="acts";
var bs=document.createElement("button");bs.textContent="deel";bs.className="go";
bs.onclick=function(){roomShare(sn)};c.appendChild(bs);
var be=document.createElement("button");be.textContent="bewerk";
be.onclick=function(){snodeEdit(sn)};c.appendChild(be);
var bd=document.createElement("button");bd.textContent="wis";
bd.onclick=function(){snodeDel(sn)};c.appendChild(bd)})}

document.getElementById("snadd").onsubmit=function(ev){ev.preventDefault();
var f=ev.target.elements,nm=f["name"].value.trim();if(!nm)return;
fetch("snode/add",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"name="+encodeURIComponent(nm)}).then(function(r){return r.json()})
.then(function(j){if(j.ok){f["name"].value="";
rmsg("snaddmsg","sensor-node "+j.idx+" toegevoegd",1);snodesLoad()}
else rmsg("snaddmsg","toevoegen mislukt: "+(j.error||""),0)})
.catch(function(){rmsg("snaddmsg","toevoegen mislukt",0)})};

var SNEI=-1;
function snodeEdit(sn){SNEI=sn.idx;document.getElementById("snedit").hidden=false;
document.getElementById("snedit-t").textContent="Bewerken: sensor-node "+sn.idx+" ("+sn.name+")";
document.getElementById("sne-name").value=sn.name;
document.getElementById("sne-stealth").checked=!!sn.stealth;
renderAcl("sacl",1,sn.idx,sn.acl);
renderChannels("schan",1,sn.idx);
document.getElementById("snedit").scrollIntoView({block:"nearest"})}
function snodeEditClose(){document.getElementById("snedit").hidden=true;SNEI=-1}
document.getElementById("sne-save").onclick=function(){if(SNEI<0)return;
var b="idx="+SNEI,nm=document.getElementById("sne-name").value.trim(),
st=document.getElementById("sne-stealth").checked?1:0;
if(nm)b+="&name="+encodeURIComponent(nm);
b+="&stealth="+st;
fetch("snode/edit",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:b})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){rmsg("snmsg","sensor-node "+SNEI+" opgeslagen",1);snodeEditClose();snodesLoad()}
else rmsg("snmsg","opslaan mislukt: "+(j.error||""),0)})
.catch(function(){rmsg("snmsg","opslaan mislukt",0)})};

function snodeDel(sn){
if(!confirm("Sensor-node "+sn.idx+" ('"+sn.name+"') verwijderen? De sleutel gaat "+
"verloren; bestaande contacten en QR-codes worden ongeldig."))return;
fetch("snode/del",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"idx="+sn.idx})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){rmsg("snmsg","sensor-node "+sn.idx+" verwijderd",1);snodeEditClose();roomShareClose();snodesLoad()}
else rmsg("snmsg","verwijderen mislukt: "+(j.error||""),0)})
.catch(function(){rmsg("snmsg","verwijderen mislukt",0)})}

/* ===================== ACL: wachtwoordloze grants ======================
   Per room (kind 0) en sensor-node (kind 1) een lijst (pubkey -> niveau). De data
   komt uit /rooms.json (rooms[].acl / snodes[].acl). Endpoints /room/acl en
   /snode/acl. */
function aclLevelName(l){return l==3?"admin":l==2?"readwrite":l==1?"read":"?"}
function renderAcl(tblId,kind,idx,list){var e=document.getElementById(tblId);if(!e)return;e.innerHTML="";
var h=e.insertRow();["sleutel","niveau",""].forEach(function(t){
var th=document.createElement("th");th.textContent=t;h.appendChild(th)});
(list||[]).forEach(function(g){var r=e.insertRow();
var c=r.insertCell();c.className="key";c.textContent=g.pub.slice(0,8)+"…"+g.pub.slice(-4);
c.title=g.pub+"  (klik om te kopieren)";
c.onclick=function(){if(navigator.clipboard)navigator.clipboard.writeText(g.pub)};
r.insertCell().textContent=aclLevelName(g.level);
c=r.insertCell();c.className="acts";var b=document.createElement("button");b.textContent="wis";
b.onclick=function(){aclDel(kind,idx,g.pub)};c.appendChild(b)})}
function aclRefresh(kind){roomsGet().then(function(d){if(!d)return;
if(kind){var sn=(d.snodes||[]).filter(function(x){return x.idx==SNEI})[0];
renderAcl("sacl",1,SNEI,sn?sn.acl:[])}
else{var rm=(d.rooms||[]).filter(function(x){return x.idx==REI})[0];
renderAcl("racl",0,REI,rm?rm.acl:[])}}).catch(function(){})}
function aclSet(kind,idx,pub,level,msgId,after){
fetch(kind?"snode/acl":"room/acl",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"idx="+idx+"&pubkey="+encodeURIComponent(pub)+"&level="+encodeURIComponent(level)})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){rmsg(msgId,"grant gezet ("+aclLevelName(j.level)+")",1);if(after)after()}
else rmsg(msgId,"mislukt: "+(j.error||""),0)}).catch(function(){rmsg(msgId,"mislukt",0)})}
function aclDel(kind,idx,pub){
var msgId=kind?"saclmsg":"raclmsg";
if(!confirm("Grant voor "+pub.slice(0,8)+"… van "+(kind?"sensor-node":"room")+" "+idx+" verwijderen?"))return;
fetch(kind?"snode/acl":"room/acl",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"idx="+idx+"&pubkey="+encodeURIComponent(pub)+"&del=1"})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){rmsg(msgId,"grant weg",1);aclRefresh(kind)}else rmsg(msgId,"mislukt: "+(j.error||""),0)})
.catch(function(){rmsg(msgId,"mislukt",0)})}
document.getElementById("racl-add").onclick=function(){
var pub=document.getElementById("racl-pub").value.trim(),lvl=document.getElementById("racl-lvl").value;
if(REI<0||!pub)return;
aclSet(0,REI,pub,lvl,"raclmsg",function(){document.getElementById("racl-pub").value="";aclRefresh(0)})};
document.getElementById("sacl-add").onclick=function(){
var pub=document.getElementById("sacl-pub").value.trim(),lvl=document.getElementById("sacl-lvl").value;
if(SNEI<0||!pub)return;
aclSet(1,SNEI,pub,lvl,"saclmsg",function(){document.getElementById("sacl-pub").value="";aclRefresh(1)})};

/* ============ gehoorde contacten: kiezer (bot-ontvangers + ACL-grants) =========
   /contacts.json draagt de buurtlijst met VOLLEDIGE pubkey (nodig voor het gedeelde
   geheim). De kiezer vult een pubkey-invoerveld; handmatig plakken kan ook. */
var CONTACTS=[];
function contactsGet(){return fetch("contacts.json",{credentials:"include"})
.then(function(r){if(!r.ok)throw 0;return r.json()})
.then(function(d){CONTACTS=(d&&d.contacts)||[];return CONTACTS}).catch(function(){return[]})}
function fillPicker(id){var sel=document.getElementById(id);if(!sel)return;
while(sel.options.length>1)sel.remove(1);
CONTACTS.forEach(function(c){var o=document.createElement("option");o.value=c.k;
var nm=c.n&&c.n.length?c.n:(c.k.slice(0,8)+"…");
o.textContent=nm+" · "+c.k.slice(0,6)+"… ("+c.t+","+c.h+"h)";sel.appendChild(o)})}
function bindPicker(pickId,inId){var sel=document.getElementById(pickId);if(!sel)return;
sel.onchange=function(){var v=sel.value;if(v){var i=document.getElementById(inId);if(i)i.value=v}}}
function refreshPickers(){contactsGet().then(function(){
["racl-pick","sacl-pick","bot-pick","bot-sto-pick","cm-pick","cm-ftarget-pick","cm-allow-pick"].forEach(fillPicker)})}
bindPicker("racl-pick","racl-pub");bindPicker("sacl-pick","sacl-pub");
bindPicker("bot-pick","bot-pub-in");bindPicker("bot-sto-pick","bot-sto-key");
bindPicker("cm-pick","cm-pub");bindPicker("cm-ftarget-pick","cm-ftarget");
bindPicker("cm-allow-pick","cm-allow");

/* ============================== bot (chat/notifier) =========================
   Alles praat met /bot.json en de /bot/* endpoints. Zichtbaar op room-nodes. */
function bmsg(id,t,ok){rmsg(id,t,ok)}
function drawQRon(cvId,uri){var qr=qrcode(0,"M");qr.addData(uri);qr.make();
var n=qr.getModuleCount(),s=4,q=4,cv=document.getElementById(cvId);if(!cv)return;
cv.width=cv.height=(n+q*2)*s;var g=cv.getContext("2d");
g.fillStyle="#fff";g.fillRect(0,0,cv.width,cv.height);g.fillStyle="#000";
for(var y=0;y<n;y+=1)for(var x=0;x<n;x+=1)if(qr.isDark(y,x))g.fillRect((x+q)*s,(y+q)*s,s,s)}
var BOT=null;
/* v2.5.0: welke bot beheren we? "" = de alert-bot (default). MGMTBOT = de eerste
   niet-alert-bot (companion-MANAGEMENT); ALERTBOT = index van de alert-bot. */
var BOTSEL="",MGMTBOT="",ALERTBOT=0;
function botSelQ(pre){return (BOTSEL!==""?pre+"bot="+encodeURIComponent(BOTSEL):"")}
function botGet(){var q=BOTSEL!==""?("?bot="+encodeURIComponent(BOTSEL)):"";
return fetch("bot.json"+q,{credentials:"include"})
.then(function(r){if(r.status==501)return null;if(!r.ok)throw 0;return r.json()})}
/* Overzichtstabel van alle bots + selectie + beheerknoppen. */
function botsLoad(){return fetch("bots.json",{credentials:"include"})
.then(function(r){if(r.status==501)return null;if(!r.ok)throw 0;return r.json()})
.then(function(d){if(!d)return;ALERTBOT=d.alert;var bots=d.bots||[];
MGMTBOT="";for(var i=0;i<bots.length;i++){if(!bots[i].alert){MGMTBOT=String(bots[i].idx);break}}
botsRender(bots)}).catch(function(){bmsg("botsmsg","kon botlijst niet laden",0)})}
function botsRender(bots){var e=document.getElementById("botsl");if(!e)return;e.innerHTML="";
var h=e.insertRow();["bot","pubkey","#ontv.","rol",""].forEach(function(t){
var th=document.createElement("th");th.textContent=t;h.appendChild(th)});
bots.forEach(function(b){var r=e.insertRow();
if(String(b.idx)===String(BOTSEL)||(BOTSEL===""&&b.idx===ALERTBOT))r.style.background="var(--sel,rgba(127,127,127,.15))";
var c=r.insertCell();var a=document.createElement("a");a.href="#";a.textContent=b.name;
a.onclick=function(ev){ev.preventDefault();BOTSEL=String(b.idx);botLoad()};c.appendChild(a);
c=r.insertCell();c.className="key";c.textContent=b.pub.slice(0,8)+"…"+b.pub.slice(-4);
c=r.insertCell();c.textContent=b.nrecips;
c=r.insertCell();c.textContent=(b.alert?"alert *":"")+(b.enabled?"":" (uit)");
c=r.insertCell();c.className="acts";
var mk=function(lbl,fn){var x=document.createElement("button");x.textContent=lbl;x.onclick=fn;x.style.marginLeft=".2rem";return x};
c.appendChild(mk("naam",function(){botRename(b.idx,b.name)}));
if(!b.alert)c.appendChild(mk(b.enabled?"uit":"aan",function(){botEnable(b.idx,!b.enabled)}));
if(!b.alert)c.appendChild(mk("alert",function(){botSetAlert(b.idx)}));
if(!b.alert&&b.idx!==0)c.appendChild(mk("wis",function(){botDelSlot(b.idx,b.name)}))});
if(!bots.length){var r=e.insertRow();var c=r.insertCell();c.colSpan=5;c.textContent="(geen bots)";c.style.color="var(--muted)"}}
function botManage(body,okmsg){return fetch("bot/manage",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:body})
.then(function(r){return r.json()}).then(function(j){
bmsg("botsmsg",j.ok?okmsg:("mislukt: "+(j.error||"")),j.ok?1:0);
if(j.ok){if(j.idx!==undefined)BOTSEL=String(j.idx);botLoad()}return j})
.catch(function(){bmsg("botsmsg","mislukt",0)})}
function botNew(){var n=document.getElementById("bot-new-name").value.trim();
botManage("op=add&name="+encodeURIComponent(n),"bot toegevoegd").then(function(){
document.getElementById("bot-new-name").value=""})}
function botRename(idx,cur){var n=prompt("Nieuwe naam voor bot:",cur);if(n===null)return;n=n.trim();
if(!n)return;botManage("op=rename&bot="+idx+"&name="+encodeURIComponent(n),"hernoemd")}
function botSetAlert(idx){if(!confirm("Deze bot de alert-rol geven?\n\nAlarmen gaan dan via deze bot."))return;
botManage("op=setalert&bot="+idx,"alert-bot gewijzigd")}
function botEnable(idx,en){botManage("op=enable&bot="+idx+"&en="+(en?1:0),en?"bot aangezet":"bot uitgezet")}
function botDelSlot(idx,name){if(!confirm("Bot '"+name+"' wissen?\n\nDe ontvangerslijst gaat weg (de sleutel blijft)."))return;
botManage("op=del&bot="+idx,"bot gewist").then(function(j){if(j&&j.ok){BOTSEL="";botLoad()}})}
function botLoad(){refreshPickers();botsLoad();botGet().then(function(d){
if(!d){document.getElementById("bot-name").textContent="geen bot op deze node";return}
BOT=d;
var cn=document.getElementById("bot-cur-name");if(cn)cn.textContent=d.name+(d.alert?" (alert-bot)":"");
document.getElementById("bot-name").textContent=d.name;
var kp=document.getElementById("bot-pub");kp.textContent=d.pub.slice(0,8)+"…"+d.pub.slice(-4);
kp.title=d.pub+"  (klik om te kopiëren)";kp.onclick=function(){if(navigator.clipboard)navigator.clipboard.writeText(d.pub)};
document.getElementById("bot-uri").value=d.uri||"";
var m=d.diag||0;
document.getElementById("dg-ping").checked=!!(m&1);
document.getElementById("dg-test").checked=!!(m&2);
document.getElementById("dg-path").checked=!!(m&4);
document.getElementById("dg-urlmode").value=String(d.durlmode||0);
document.getElementById("dg-url").value=d.durl||"";
DFIT=d.dfit||[0,0,0];DURLMAX=d.durlmax||0;dgFit();
try{drawQRon("bqr",d.uri)}catch(e){}
botRender(d.recips||[],d.max)}).catch(function(){bmsg("botmsg","kon bot niet laden",0)});
channelsLoad()}
function botRender(list,max){var e=document.getElementById("botrl");e.innerHTML="";
var h=e.insertRow();["ontvanger","",""].forEach(function(t){
var th=document.createElement("th");th.textContent=t;h.appendChild(th)});
list.forEach(function(g){var r=e.insertRow();
var c=r.insertCell();c.className="key";c.textContent=g.k.slice(0,8)+"…"+g.k.slice(-4);
c.title=g.k+"  (klik om te kopiëren)";
c.onclick=function(){if(navigator.clipboard)navigator.clipboard.writeText(g.k)};
r.insertCell().textContent="";
c=r.insertCell();c.className="acts";var b=document.createElement("button");b.textContent="wis";
b.onclick=function(){botDel(g.k)};c.appendChild(b)});
if(!list.length){var r=e.insertRow();var c=r.insertCell();c.colSpan=3;
c.textContent="(nog geen ontvangers — voeg jezelf toe)";c.style.color="var(--muted)"}}
function botAdd(){var pub=document.getElementById("bot-pub-in").value.trim();
if(!pub)return;
fetch("bot/recipient",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"key="+encodeURIComponent(pub)+botSelQ("&")})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){document.getElementById("bot-pub-in").value="";
var pk=document.getElementById("bot-pick");if(pk)pk.value="";
bmsg("botaddmsg","ontvanger toegevoegd",1);botLoad()}
else bmsg("botaddmsg","mislukt: "+(j.error||""),0)}).catch(function(){bmsg("botaddmsg","mislukt",0)})}
function botDel(pub){if(!confirm("Ontvanger "+pub.slice(0,8)+"… verwijderen?"))return;
fetch("bot/recipient",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"del="+encodeURIComponent(pub)+botSelQ("&")})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){bmsg("botaddmsg","ontvanger weg",1);botLoad()}
else bmsg("botaddmsg","mislukt: "+(j.error||""),0)}).catch(function(){bmsg("botaddmsg","mislukt",0)})}
function botAdvert(flood){fetch("bot/advert",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"flood="+(flood?1:0)+botSelQ("&")})
.then(function(r){return r.json()}).then(function(j){
bmsg("botmsg",j.ok?("bot-advert verstuurd ("+(flood?"flood":"zero-hop")+")"):"mislukt",j.ok?1:0)})
.catch(function(){bmsg("botmsg","mislukt",0)})}
document.getElementById("bot-add").onclick=botAdd;
document.getElementById("bot-new").onclick=botNew;

/* ---- zend-diagnose: vinkjes, URL-modus en het live ruimte-tellertje ---- */
var DFIT=[0,0,0],DURLMAX=0;
function dgFit(){var e=document.getElementById("dg-fit");if(!e)return;
var u=document.getElementById("dg-url").value.trim();
var mode=document.getElementById("dg-urlmode").value;
if(mode=="0"){e.textContent="Geen URL in de antwoorden.";e.style.color="var(--muted)";return}
if(mode=="2"){e.textContent="Apart bericht: lengte speelt geen rol (past altijd). "
+u.length+" van max "+DURLMAX+" tekens gebruikt.";
e.style.color=(u.length>DURLMAX?"var(--red)":"var(--muted)");return}
/* inline: per commando tonen hoeveel er nog in past */
var names=["ping","test","path"],fits=[],miss=[];
for(var i=0;i<3;i++){if(u.length<=DFIT[i])fits.push(names[i]+" (≤"+DFIT[i]+")");
else miss.push(names[i]+" (≤"+DFIT[i]+")")}
var t="URL is "+u.length+" tekens. Past inline bij: "+(fits.length?fits.join(", "):"geen enkel commando");
if(miss.length)t+=" — weggelaten bij: "+miss.join(", ");
e.textContent=t;e.style.color=miss.length?"var(--amber)":"var(--accent)"}
document.getElementById("dg-url").oninput=dgFit;
document.getElementById("dg-urlmode").onchange=dgFit;
document.getElementById("dg-save").onclick=function(){
var m=(document.getElementById("dg-ping").checked?1:0)
|(document.getElementById("dg-test").checked?2:0)
|(document.getElementById("dg-path").checked?4:0);
var mode=document.getElementById("dg-urlmode").value;
var u=document.getElementById("dg-url").value.trim();
if(u.length>DURLMAX){bmsg("dgmsg","URL te lang (max "+DURLMAX+" tekens)",0);return}
fetch("bot/diag",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"mask="+m+"&urlmode="+mode+"&url="+encodeURIComponent(u)+botSelQ("&")})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){DFIT=j.dfit||DFIT;dgFit();bmsg("dgmsg","bewaard",1)}
else bmsg("dgmsg","mislukt: "+(j.error||""),0)})
.catch(function(){bmsg("dgmsg","mislukt",0)})};
document.getElementById("bot-copy").onclick=function(){
var i=document.getElementById("bot-uri");i.focus();i.select();
if(navigator.clipboard&&navigator.clipboard.writeText)
navigator.clipboard.writeText(i.value).then(function(){bmsg("botmsg","join-link gekopieerd",1)},function(){});
else try{document.execCommand("copy");bmsg("botmsg","join-link gekopieerd",1)}catch(e){}};
document.getElementById("bot-sto-go").onclick=function(){
var k=document.getElementById("bot-sto-key").value.trim(),m=document.getElementById("bot-sto-msg").value;
if(!k||!m){bmsg("botsendmsg","pubkey en bericht nodig",0);return}
fetch("bot/sendto",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"key="+encodeURIComponent(k)+"&msg="+encodeURIComponent(m)+botSelQ("&")})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){document.getElementById("bot-sto-msg").value="";bmsg("botsendmsg","DM verstuurd",1)}
else bmsg("botsendmsg","mislukt: "+(j.error||""),0)}).catch(function(){bmsg("botsendmsg","mislukt",0)})};
document.getElementById("bot-post-go").onclick=function(){
var m=document.getElementById("bot-post-msg").value;
if(!m){bmsg("botsendmsg","bericht nodig",0);return}
fetch("bot/post",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"msg="+encodeURIComponent(m)+botSelQ("&")})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){document.getElementById("bot-post-msg").value="";bmsg("botsendmsg","gepost naar "+j.sent+" ontvanger(s)",1)}
else bmsg("botsendmsg","mislukt: "+(j.error||""),0)}).catch(function(){bmsg("botsendmsg","mislukt",0)})};

/* ---- hashtag-kanalen: lijst + toevoegen/aan-uit/wissen ---- */
function channelsLoad(){fetch("channels.json",{credentials:"include"})
.then(function(r){if(r.status==501)return null;if(!r.ok)throw 0;return r.json()})
.then(function(d){if(!d)return;channelsRender(d.channels||[])})
.catch(function(){bmsg("chmsg","kon kanalen niet laden",0)})}
function channelsRender(list){var e=document.getElementById("chl");if(!e)return;e.innerHTML="";
var h=e.insertRow();["kanaal","sleutel","aan",""].forEach(function(t){
var th=document.createElement("th");th.textContent=t;h.appendChild(th)});
list.forEach(function(c){var r=e.insertRow();
r.insertCell().textContent=c.n;
var sc=r.insertCell();sc.className="num";sc.textContent=c.bits+"-bit #"+c.h+(c.pub?" (publiek)":(c.drv?" (afgeleid)":" (eigen)"));
var ac=r.insertCell();var cb=document.createElement("input");cb.type="checkbox";cb.checked=!!c.en;
cb.onchange=function(){channelToggle(c.n,cb.checked)};ac.appendChild(cb);
var dc=r.insertCell();dc.className="acts";var b=document.createElement("button");b.textContent="wis";
b.onclick=function(){channelDel(c.n)};dc.appendChild(b)});
if(!list.length){var r=e.insertRow();var c=r.insertCell();c.colSpan=4;
c.textContent="(geen kanalen — voeg er een toe, bv. Public)";c.style.color="var(--muted)"}}
function channelToggle(name,en){fetch("channel/toggle",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"name="+encodeURIComponent(name)+"&enabled="+(en?1:0)})
.then(function(r){return r.json()}).then(function(j){
bmsg("chmsg",j.ok?("kanaal "+name+(en?" aan":" uit")):"mislukt: "+(j.error||""),j.ok?1:0);channelsLoad()})
.catch(function(){bmsg("chmsg","mislukt",0)})}
function channelDel(name){if(!confirm("Kanaal '"+name+"' verwijderen?"))return;
fetch("channel/del",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"name="+encodeURIComponent(name)})
.then(function(r){return r.json()}).then(function(j){
bmsg("chmsg",j.ok?"kanaal weg":"mislukt: "+(j.error||""),j.ok?1:0);channelsLoad()})
.catch(function(){bmsg("chmsg","mislukt",0)})}
document.getElementById("ch-add").onclick=function(){
var name=document.getElementById("ch-name").value.trim(),
sec=document.getElementById("ch-secret").value.trim();
if(!name){bmsg("chmsg","naam nodig (secret mag leeg = afgeleid uit naam)",0);return}
fetch("channel/add",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"name="+encodeURIComponent(name)+"&secret="+encodeURIComponent(sec)+"&enabled=1"})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){document.getElementById("ch-name").value="";document.getElementById("ch-secret").value="";
bmsg("chmsg","kanaal toegevoegd - "+(j.public?"publiek kanaal (vaste sleutel)":(j.derived?"sleutel afgeleid uit naam":"eigen sleutel"))+" ("+j.bits+"-bit #"+j.h+")",1);channelsLoad()}
else bmsg("chmsg","mislukt: "+(j.error||""),0)}).catch(function(){bmsg("chmsg","mislukt",0)})};

/* ===================== kanaalbeheer (node-centrisch) ===================
   Dezelfde rm/sn-maskers als per-sensor, maar PER room/sensor-node getoond.
   Koppelen/ontkoppelen = de rm- resp. sn-bit zetten/wissen via /mon/alarm (ch).
   Nieuw kanaal = monitor aanmaken (/monitor) + meteen aan deze node koppelen. */
function chanMask(m,kind){return kind?(m.sn||0):(m.rm||0)}
function renderChannels(tblId,kind,idx){var e=document.getElementById(tblId);if(!e)return;e.innerHTML="";
var h=e.insertRow();["kan","naam","aan",""].forEach(function(t){
var th=document.createElement("th");th.textContent=t;h.appendChild(th)});
MONS.forEach(function(m){if(m.ch<5)return;   // vaste kanalen 1-4 hebben geen rm/sn
var r=e.insertRow();
var c=r.insertCell();c.className="num";c.textContent=m.ch;
r.insertCell().textContent=m.n;
c=r.insertCell();var cb=document.createElement("input");cb.type="checkbox";
cb.checked=!!(chanMask(m,kind)&(1<<idx));
cb.onchange=function(){coupleChannel(kind,idx,m,cb.checked)};c.appendChild(cb);
c=r.insertCell();c.className="acts";
var be=document.createElement("button");be.textContent="bewerk";
be.onclick=function(){chanEdit(m,kind,idx)};c.appendChild(be)})}
function coupleChannel(kind,idx,m,on){
var mask=chanMask(m,kind);mask=on?(mask|(1<<idx)):(mask&~(1<<idx));
var body="ch="+m.ch+"&am="+(m.am||3)+"&rm="+(kind?(m.rm||0):mask)+"&sn="+(kind?mask:(m.sn||1));
var mid=kind?"schanmsg":"rchanmsg";
fetch("mon/alarm",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:body})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){rmsg(mid,"kanaal "+m.ch+(on?" gekoppeld":" ontkoppeld"),1);
if(kind)m.sn=mask;else m.rm=mask}
else{rmsg(mid,"mislukt: "+(j.error||""),0)}}).catch(function(){rmsg(mid,"mislukt",0)})}
function chanEdit(m,kind,idx){
var nn=prompt("Naam voor kanaal "+m.ch+":",m.n);if(nn===null)return;
var nh=prompt("Adres voor kanaal "+m.ch+" (of - voor een gemelde dienst):",m.h);if(nh===null)return;
var ni=prompt("Interval (s) voor kanaal "+m.ch+":",""+m.i);if(ni===null)return;
var cmds=[];
if(nn&&nn!=m.n)cmds.push("sensor set mon."+m.ch+".name "+nn);
if(nh&&nh!=m.h)cmds.push("sensor set mon."+m.ch+".host "+nh);
if(ni&&parseInt(ni,10)!=m.i)cmds.push("sensor set mon."+m.ch+".int "+parseInt(ni,10));
var mid=kind?"schanmsg":"rchanmsg";
if(!cmds.length){rmsg(mid,"niets gewijzigd",1);return}
cliSeq(cmds,function(good,bad,last){rmsg(mid,bad?("deels geweigerd: "+last.trim()):"kanaal bijgewerkt",!bad);
u2().then(function(){renderChannels(kind?"schan":"rchan",kind,idx)})})}
function chanAdd(kind,idx,pfx){if(idx<0)return;var mid=pfx+"msg";
var name=document.getElementById(pfx+"-name").value.trim(),
host=document.getElementById(pfx+"-host").value.trim(),
iv=document.getElementById(pfx+"-int").value;
if(!name||!host){rmsg(mid,"naam en adres verplicht",0);return}
fetch("monitor",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"name="+encodeURIComponent(name)+"&host="+encodeURIComponent(host)+"&int="+encodeURIComponent(iv)})
.then(function(r){return r.text()}).then(function(t){
var mm=t.match(/kanaal (\d+)/);
if(!mm){rmsg(mid,"aanmaken mislukt: "+t.trim(),0);return}
var ch=parseInt(mm[1],10);
var body=kind?("ch="+ch+"&am=3&rm=1&sn="+(1<<idx)):("ch="+ch+"&am=3&rm="+(1<<idx));
fetch("mon/alarm",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:body})
.then(function(r){return r.json()}).then(function(j){
rmsg(mid,j.ok?("kanaal "+ch+" aangemaakt + gekoppeld"):("koppelen mislukt: "+(j.error||"")),j.ok);
document.getElementById(pfx+"-name").value="";document.getElementById(pfx+"-host").value="";
u2().then(function(){renderChannels(pfx,kind,idx)})})})
.catch(function(){rmsg(mid,"aanmaken mislukt",0)})}
document.getElementById("rchan-add").onclick=function(){chanAdd(0,REI,"rchan")};
document.getElementById("schan-add").onclick=function(){chanAdd(1,SNEI,"schan")};

/* Handmatig advert (kind 0=room, 1=snode; flood 1/0). */
function nodeAdvert(kind,idx,flood){if(idx<0)return;var mid=kind?"snmsg":"rmsg";
fetch((kind?"snode":"room")+"/advert",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"idx="+idx+"&flood="+flood})
.then(function(r){return r.json()}).then(function(j){
rmsg(mid,j.ok?("advert verstuurd ("+(flood?"flood":"zero-hop")+")"):("advert mislukt: "+(j.error||"")),j.ok)})
.catch(function(){rmsg(mid,"advert mislukt",0)})}

u2();setInterval(u2,5000);
u3();setInterval(u3,20000);
cfg();
roomsProbe();

/* ============================== companions (v2.4.0) =======================
   Beheer + laatst bekende locatie. Praat met /companions.json en /companion;
   commando's gaan via /bot/sendto (dezelfde weg als de bot-DM). Room-nodes. */
var CMP=[],CMMAP=null,CMLAYER=null,CMLEAFLET=0,CMSIG="";
function cmMsg(id,t,ok){rmsg(id,t,ok)}
function companionsGet(){return fetch("companions.json",{credentials:"include"})
.then(function(r){if(r.status==501)return null;if(!r.ok)throw 0;return r.json()})}
/* Alleen /companions.json opnieuw ophalen + tekenen (GEEN pickers): gebruikt na
   elke actie en door de lichte poll onderaan. companionsLoad() doet dit PLUS de
   pickers en draait bij het openen van het tabblad. */
function companionsRefresh(){return companionsGet().then(function(d){
if(!d)return;CMP=d.companions||[];cmRender();cmFillCmdPick();cmMap();cmMsgsRefresh()})
.catch(function(){cmMsg("cmaddmsg","kon companions niet laden",0)})}
/* Inkomende-berichten-inbox: /messages.json ophalen + tekenen (nieuwste eerst). */
function cmMsgsRefresh(){fetch("messages.json",{credentials:"include"})
.then(function(r){if(!r.ok)throw 0;return r.json()}).then(function(d){
var e=document.getElementById("cmmsgs");if(!e)return;e.innerHTML="";
var h=e.insertRow();["tijd","companion","bericht"].forEach(function(t){
var th=document.createElement("th");th.textContent=t;h.appendChild(th)});
var M=d.messages||[];
M.forEach(function(m){var r=e.insertRow();
var c=r.insertCell();c.textContent=cmAge(m.ts);c.style.whiteSpace="nowrap";c.style.color="var(--muted)";c.title=m.ts?new Date(m.ts*1000).toLocaleString():"";
c=r.insertCell();c.textContent=m.name||(m.pubkey||"").slice(0,8);c.title=m.pubkey||"";
c=r.insertCell();c.textContent=m.text||"";c.style.wordBreak="break-word"});
if(!M.length){var r=e.insertRow();var c=r.insertCell();c.colSpan=3;
c.textContent="(nog geen berichten ontvangen)";c.style.color="var(--muted)"}})
.catch(function(){})}
function companionsLoad(){refreshPickers();botsLoad();return companionsRefresh()}
function cmAge(s){if(!s)return"—";var now=Math.floor(Date.now()/1000);var a=now-s;if(a<0)a=0;
if(a<90)return a+"s";if(a<5400)return Math.round(a/60)+"m";if(a<172800)return Math.round(a/3600)+"u";return Math.round(a/86400)+"d"}
function cmRender(){var e=document.getElementById("cml");e.innerHTML="";
var h=e.insertRow();["naam","pubkey","locatie","",""].forEach(function(t){
var th=document.createElement("th");th.textContent=t;h.appendChild(th)});
CMP.forEach(function(g){var r=e.insertRow();
var nc=r.insertCell();nc.textContent=g.name||"—";
/* Val-badge: alleen als er een val-event in /companions.json staat. Rood en
   opvallend -- dit is het enige wat je hier meteen moet zien. */
if(g.fall_ts){var bd=document.createElement("span");
bd.textContent=" ⚠ "+g.fall_kind+" "+cmAge(g.fall_ts);
bd.style.cssText="color:var(--red);font-weight:600";
bd.title="val-event ontvangen ("+g.fall_kind+"), "+cmAge(g.fall_ts)+" geleden";nc.appendChild(bd)}
var c=r.insertCell();c.className="key";c.textContent=g.pubkey.slice(0,8)+"…"+g.pubkey.slice(-4);
c.title=g.pubkey+"  (klik om te kopiëren)";
c.onclick=function(){if(navigator.clipboard)navigator.clipboard.writeText(g.pubkey)};
c=r.insertCell();
if(g.lat!=null&&g.lon!=null){var a=document.createElement("a");
a.href="https://www.openstreetmap.org/?mlat="+g.lat+"&mlon="+g.lon+"#map=16/"+g.lat+"/"+g.lon;
a.target="_blank";a.rel="noopener";a.textContent=g.lat.toFixed(5)+","+g.lon.toFixed(5);
c.appendChild(a);c.appendChild(document.createTextNode(" · "+cmAge(g.seen)))}
else{c.textContent="(geen)";c.style.color="var(--muted)"}
c=r.insertCell();c.className="acts";var b=document.createElement("button");b.textContent="cmd";
b.title="kies in het commandopaneel";b.onclick=function(){
var p=document.getElementById("cm-cmd-pick");p.value=g.pubkey;
cmMsg("cmcmdmsg","companion gekozen: "+(g.name||g.pubkey.slice(0,8)),1)};c.appendChild(b);
c=r.insertCell();c.className="acts";var d2=document.createElement("button");d2.textContent="wis";
d2.onclick=function(){cmDel(g.pubkey)};c.appendChild(d2)});
if(!CMP.length){var r=e.insertRow();var c=r.insertCell();c.colSpan=5;
c.textContent="(nog geen companions — voeg er een toe)";c.style.color="var(--muted)"}}
function cmFillCmdPick(){var s=document.getElementById("cm-cmd-pick");if(!s)return;
var cur=s.value;while(s.options.length>1)s.remove(1);
CMP.forEach(function(g){var o=document.createElement("option");o.value=g.pubkey;
o.textContent=(g.name||g.pubkey.slice(0,8))+" · "+g.pubkey.slice(0,6)+"…";s.appendChild(o)});
s.value=cur}
function cmAdd(){var pub=document.getElementById("cm-pub").value.trim();
var nm=document.getElementById("cm-name").value.trim();
if(pub.length!=64){cmMsg("cmaddmsg","volledige pubkey (64 hex) nodig",0);return}
fetch("companion",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"key="+encodeURIComponent(pub)+"&name="+encodeURIComponent(nm)})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){document.getElementById("cm-pub").value="";document.getElementById("cm-name").value="";
var pk=document.getElementById("cm-pick");if(pk)pk.value="";
cmMsg("cmaddmsg","opgeslagen",1);companionsLoad()}
else cmMsg("cmaddmsg","mislukt: "+(j.error||""),0)}).catch(function(){cmMsg("cmaddmsg","mislukt",0)})}
function cmDel(pub){if(!confirm("Companion "+pub.slice(0,8)+"… verwijderen?"))return;
fetch("companion",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},body:"del="+encodeURIComponent(pub)})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){cmMsg("cmaddmsg","companion weg",1);companionsLoad()}
else cmMsg("cmaddmsg","mislukt: "+(j.error||""),0)}).catch(function(){cmMsg("cmaddmsg","mislukt",0)})}
function cmCmd(cmd){var pub=document.getElementById("cm-cmd-pick").value;
if(!pub){cmMsg("cmcmdmsg","kies eerst een companion",0);return}
if(!cmd||!cmd.trim()){cmMsg("cmcmdmsg","leeg commando",0);return}
/* Companion-MANAGEMENT gaat via de MGMT-bot (indien aanwezig; anders de alert-bot). */
var mgb=(typeof MGMTBOT!=="undefined"&&MGMTBOT!=="")?("&bot="+encodeURIComponent(MGMTBOT)):"";
return fetch("bot/sendto",{method:"POST",credentials:"include",
headers:{"Content-Type":"application/x-www-form-urlencoded"},
body:"key="+encodeURIComponent(pub)+"&msg="+encodeURIComponent(cmd)+mgb})
.then(function(r){return r.json()}).then(function(j){
if(j.ok){cmMsg("cmcmdmsg","verzonden over de mesh: "+cmd+" — antwoord/effect volgt async",1);
companionsRefresh()}
else cmMsg("cmcmdmsg","mislukt: "+(j.error||""),0)})
.catch(function(){cmMsg("cmcmdmsg","mislukt",0)})}
function cmFallTest(){if(!confirm("Val-TEST sturen?\n\nDe companion start nu z'n "+
"pre-alarm (aftellen). Dat is annuleerbaar op het toestel en stuurt geen echte "+
"SOS zolang niemand valt — maar hij gaat wel piepen."))return;cmCmd('!fall test')}
function cmFallTargetAdd(){var t=document.getElementById("cm-ftarget").value.trim();
if(t.length!=64){cmMsg("cmcmdmsg","doel toevoegen: volledige pubkey (64 hex) nodig",0);return}
cmCmd('!fall target add '+t)}
function cmFallTargetDel(){var t=document.getElementById("cm-ftarget").value.trim();
if(t.length<12||t.length%2){cmMsg("cmcmdmsg","doel verwijderen: prefix van min. 12 hex (even) nodig",0);return}
cmCmd('!fall target del '+t)}
/* allow-lijst (wie de companion mag aansturen): add op volledige pubkey,
   del op een prefix (>=12 hex). Zelfde validatie als de fall-doellijst. */
function cmAllowAdd(){var t=document.getElementById("cm-allow").value.trim();
if(t.length!=64){cmMsg("cmcmdmsg","allow toevoegen: volledige pubkey (64 hex) nodig",0);return}
cmCmd('!allow add '+t)}
function cmAllowDel(){var t=document.getElementById("cm-allow").value.trim();
if(t.length<12||t.length%2){cmMsg("cmcmdmsg","allow verwijderen: prefix van min. 12 hex (even) nodig",0);return}
cmCmd('!allow del '+t)}
/* RADIO: elk veld apart, ACHTER een expliciete confirm met de waarschuwing dat
   dit de companion van de mesh kan halen. Stuurt '!radio <veld> <waarde> confirm'
   (de companion-firmware eist de confirm-staart). Leeg veld -> niets sturen. */
function cmRadio(field,inId){var el=document.getElementById(inId);
var v=el?el.value.trim():"";
if(!v){cmMsg("cmradiomsg","radio "+field+": vul eerst een waarde in",0);return}
if(!confirm("RADIO "+field.toUpperCase()+" = "+v+" naar de companion sturen?\n\n"+
"WAARSCHUWING: dit kan de companion van de mesh doen vallen (fysieke seriële "+
"recovery nodig). Freq/BW/SF/CR/tx-power moeten aan beide kanten gelijk zijn.\n\n"+
"Doorgaan?"))return;
cmMsg("cmradiomsg","radio "+field+" "+v+" verzonden (confirm) — effect volgt async",1);
cmCmd('!radio '+field+' '+v+' confirm')}
document.getElementById("cm-add").onclick=cmAdd;
/* Lichte auto-verversing: alleen als het tabblad openstaat (p7 zichtbaar), zelfde
   stijl als u2/u3 maar we slaan de fetch over als niemand kijkt. Locatie/seen/val
   komen async over de mesh binnen en de node bewaart ze -- 15s is ruim genoeg. */
setInterval(function(){var p=document.getElementById("p7");
if(p&&!p.hidden)companionsRefresh()},15000);
/* Leaflet lui laden van de CDN; faalt dat (offline), dan blijft de tekstlijst. */
function cmLoadLeaflet(cb){if(window.L){cb(true);return}
if(CMLEAFLET==2){cb(false);return}
if(CMLEAFLET==1){setTimeout(function(){cmLoadLeaflet(cb)},300);return}
CMLEAFLET=1;
var css=document.createElement("link");css.rel="stylesheet";
css.href="https://unpkg.com/leaflet@1.9.4/dist/leaflet.css";document.head.appendChild(css);
var s=document.createElement("script");s.src="https://unpkg.com/leaflet@1.9.4/dist/leaflet.js";
s.onload=function(){CMLEAFLET=0;cb(!!window.L)};
s.onerror=function(){CMLEAFLET=2;cb(false)};document.head.appendChild(s)}
function cmMap(){var locs=CMP.filter(function(g){return g.lat!=null&&g.lon!=null});
var sig=locs.map(function(g){return g.lat+","+g.lon}).join("|");
var md=document.getElementById("cm-map"),mt=document.getElementById("cm-maptext");
if(!locs.length){md.style.display="none";mt.textContent="(nog geen locaties)";mt.style.color="var(--muted)";return}
mt.textContent="";mt.style.color="";
cmLoadLeaflet(function(ok){
if(!ok||!window.L){md.style.display="none";var html="";
locs.forEach(function(g){html+='<div style="margin:.15rem 0">'+(g.name||g.pubkey.slice(0,8))+
': <a target="_blank" rel="noopener" href="https://www.openstreetmap.org/?mlat='+g.lat+'&mlon='+g.lon+
'#map=16/'+g.lat+'/'+g.lon+'">'+g.lat.toFixed(5)+','+g.lon.toFixed(5)+'</a> · '+cmAge(g.seen)+'</div>'});
mt.innerHTML=html;return}
md.style.display="block";
if(!CMMAP){CMMAP=L.map("cm-map");L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
{maxZoom:19,attribution:"© OpenStreetMap"}).addTo(CMMAP)}
if(CMLAYER)CMMAP.removeLayer(CMLAYER);
CMLAYER=L.layerGroup().addTo(CMMAP);var pts=[];
locs.forEach(function(g){var m=L.marker([g.lat,g.lon]).addTo(CMLAYER);
m.bindPopup((g.name||g.pubkey.slice(0,8))+"<br>"+g.lat.toFixed(5)+","+g.lon.toFixed(5)+" · "+cmAge(g.seen));
pts.push([g.lat,g.lon])});
/* Alleen bij een GEWIJZIGDE locatieset het beeld verzetten -- zo laat de 15s-poll
   het pannen/zoomen van de gebruiker met rust. */
if(sig!=CMSIG){CMSIG=sig;
if(pts.length==1)CMMAP.setView(pts[0],15);else CMMAP.fitBounds(pts,{padding:[30,30]})}
setTimeout(function(){CMMAP.invalidateSize()},120)})}

/* ===================== declutter: uitleg achter een "?" =====================
   CONSISTENT over ALLE panelen: elke .why/.note-uitleg wordt bij het laden in
   een compacte, dichtgeklapte "?"-help gestopt. Instellingen staan zo vooraan en
   compact; de tekst (de KENNIS) blijft, één klik weg. Eén keer bij het laden --
   alle uitleg staat statisch in de HTML, dus dynamisch gegenereerde tabellen
   raken dit niet. */
(function(){var ps=document.querySelectorAll("p.why,p.note");
for(var i=0;i<ps.length;i++){var p=ps[i];
if(p.parentNode&&p.parentNode.classList&&p.parentNode.classList.contains("help"))continue;
var d=document.createElement("details");d.className="help";
var s=document.createElement("summary");s.textContent="?";
s.title=p.classList.contains("why")?"waarom dit zo is":"toelichting";
p.parentNode.insertBefore(d,p);d.appendChild(s);d.appendChild(p)}})();
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

/* Tijd-config: regel 1 = NTP-server(host), regel 2 = POSIX-TZ. Ontbreekt het
 * bestand of een regel, dan gelden DEFAULT_NTP / DEFAULT_TZ. Zelfde discipline als
 * de wifi-config: File-alloc alleen bij boot en bij opslaan, nooit per verzoek. */
bool loadTimeConfig(char* ntp, size_t ntp_len, char* tz, size_t tz_len) {
  if (ntp && ntp_len) strlcpy(ntp, DEFAULT_NTP, ntp_len);
  if (tz && tz_len)   strlcpy(tz, DEFAULT_TZ, tz_len);
  File f = SPIFFS.open(TIME_CFG_PATH, FILE_READ);
  if (!f) return false;   // standaarden staan al
  if (ntp && ntp_len > 1) {
    size_t n = f.readBytesUntil('\n', ntp, ntp_len - 1);
    ntp[n] = 0;
    for (char* p = ntp; *p; p++) if (*p == '\r') { *p = 0; break; }
    if (ntp[0] == 0) strlcpy(ntp, DEFAULT_NTP, ntp_len);
  }
  if (tz && tz_len > 1) {
    size_t n = f.readBytesUntil('\n', tz, tz_len - 1);
    tz[n] = 0;
    for (char* p = tz; *p; p++) if (*p == '\r') { *p = 0; break; }
    if (tz[0] == 0) strlcpy(tz, DEFAULT_TZ, tz_len);
  }
  f.close();
  return true;
}

bool saveTimeConfig(const char* ntp, const char* tz) {
  File f = SPIFFS.open(TIME_CFG_PATH, FILE_WRITE);
  if (!f) return false;
  f.printf("%s\n%s\n", (ntp && ntp[0]) ? ntp : DEFAULT_NTP,
                       (tz && tz[0]) ? tz : DEFAULT_TZ);
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

  /* NTP-server + tijdzone voor het formulier (opgeslagen of de standaard). Eén keer
   * hier gelezen; cfg.json toont daarna deze RAM-buffers. */
  loadTimeConfig(g_ntp_shown, sizeof(g_ntp_shown), g_tz_shown, sizeof(g_tz_shown));

  /* De eigen web-login, langs dezelfde weg en om dezelfde reden: opgeslagen wint
   * van gebakken. Eén keer hier lezen (een File-object alloceert intern), nooit in
   * een verzoekpad -- requireAuth() leest daarna alleen deze RAM-buffers. Staat er
   * geen bruikbare login opgeslagen, dan de gebakken vlaggen, zodat een verse node
   * meteen bereikbaar is; g_web_custom=false maakt op de pagina zichtbaar dat hij
   * nog op die vlootbrede standaard staat. */
  if (MonitorStore::loadWebCred(SPIFFS, g_web_user, sizeof(g_web_user),
                                g_web_pass, sizeof(g_web_pass))) {
    g_web_custom = true;
  } else {
    strlcpy(g_web_user, WEB_USER, sizeof(g_web_user));
    strlcpy(g_web_pass, WEB_PASS, sizeof(g_web_pass));
    g_web_custom = false;
  }

  /* De stand van de kanaaltoewijzer uit /monitors.cfg. EEN keer, hier, en nooit
   * in een verzoekpad: dit is dezelfde afspraak als bij loadWifiConfig() -- een
   * File-object alloceert intern, en dat mag bij het opstarten en niet per
   * verzoek.
   *
   * De MonitorCfg staat STATIC en niet op de stapel. Sinds de SNMP-velden
   * (snmp_oid[80] + snmp_community[24] per ingang x MON_MAX_MONITORS) is een
   * MonitorCfg ~6 kB; op de stapel liet dat samen met MonitorStore::load de 8 kB
   * loopTask-stack overlopen bij boot. begin() draait eenmalig uit setup();
   * setDefaults() herinitialiseert, dus static is veilig. ~6 kB .bss.
   *
   * Mislukt het lezen, dan blijft het masker 0 en vult cfg.json zich met wat er
   * nu in gebruik is. Dat is een ONDERschatting van wat vergeven is, en dat is de
   * goede kant om fout te zitten: de pagina belooft dan niet meer ruimte dan er
   * is. Zij zegt er ook bij waar het getal op berust. */
  {
    static MonitorCfg cfg;
    MonitorStore::setDefaults(cfg);
    if (MonitorStore::load(SPIFFS, cfg)) g_ever_mask = cfg.ch_ever_used;
  }

  routes();
}

void WebTask::routes() {
  /* De Cookie-kop MOET verzameld worden voordat handleClient() draait, anders geeft
   * header("Cookie") altijd leeg terug en ziet geen enkele sessie er ooit uit als
   * geldig. Authorization (Basic) wordt altijd al gelezen; deze regel is alleen voor
   * de sessiecookie. Eenmalig bij het opstarten. */
  static const char* COLLECT_HEADERS[] = { "Cookie" };
  _server->collectHeaders(COLLECT_HEADERS, 1);

  _server->on("/", HTTP_GET, web_route_root);
  /* De eigen inlogpagina en het afmelden -- de mens-gerichte kant van de auth.
   * /login is met opzet NIET achter de auth (anders kon je nooit inloggen); de POST
   * heeft zijn eigen brute-force-rem. /logout wist de sessie. */
  _server->on("/login", HTTP_GET, web_route_login);
  _server->on("/login", HTTP_POST, web_route_loginpost);
  _server->on("/logout", HTTP_POST, web_route_logout);
  _server->on("/status.json", HTTP_GET, web_route_status);
  _server->on("/wifi", HTTP_POST, web_route_wifi);
  _server->on("/time", HTTP_POST, web_route_time);
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
  /* De eigen web-login. POST-only, en achter dezelfde Basic-auth als de rest: dit
   * is de route waarmee de statsserver de credential roteert. Een GET zou een link
   * zijn die een browser of een linkchecker kan volgen, en dit verandert het
   * wachtwoord van de node. */
  _server->on("/web/cred", HTTP_POST, web_route_webcred);
  /* De reset naar de gebakken standaard (admin/meshcore). POST-only en achter de
   * auth: de eigenaar roept hem NA het flashen aan met de HUIDIGE (geroteerde)
   * login. Zie de uitleg boven handleWebCredReset(). */
  _server->on("/web/cred/reset", HTTP_POST, web_route_credreset);
  /* Simuleren en testen. POST-only, en hier om twee redenen: een GET die een
   * sensor forceert is een link waarmee iemand de bewaking van een kanaal
   * uitzet, en een GET die een testbericht stuurt is een link die zendtijd kost
   * bij elke prefetch. Beide gebeuren zonder dat er iemand geklikt heeft. */
  _server->on("/sim", HTTP_POST, web_route_sim);
  _server->on("/sim/clear", HTTP_POST, web_route_simclear);
  _server->on("/alert/test", HTTP_POST, web_route_alerttest);
  /* Room-beheer. De leeskant (/rooms.json, /rooms/backup) is GET; alles wat een
   * room aanmaakt, wijzigt, verwijdert of terugzet is POST -- om dezelfde reden als
   * bij de monitors en de toegangslijst: een GET die een room wist of de config
   * terugzet is een link die een browser of linkchecker kan volgen. /rooms/backup
   * is een GET (het is een download en verandert niets) maar draagt sleutels, dus
   * hij zit achter dezelfde auth. */
  _server->on("/rooms.json", HTTP_GET, web_route_roomsjson);
  _server->on("/room/add", HTTP_POST, web_route_roomadd);
  _server->on("/room/edit", HTTP_POST, web_route_roomedit);
  _server->on("/room/del", HTTP_POST, web_route_roomdel);
  _server->on("/rooms/backup", HTTP_GET, web_route_roomsbackup);
  _server->on("/rooms/restore", HTTP_POST, web_route_roomsrestore);
  /* Per-sensor alarmroute (am) + room-set (rm). POST-only: hij zet de bewaking van
   * een kanaal op een andere bezorgweg, dus geen GET-link die een browser volgt. */
  _server->on("/mon/alarm", HTTP_POST, web_route_monalarm);
  /* Virtuele sensor-nodes: symmetrisch met /room/*. Lijst via /rooms.json. */
  _server->on("/snode/add", HTTP_POST, web_route_snodeadd);
  _server->on("/snode/edit", HTTP_POST, web_route_snodeedit);
  _server->on("/snode/del", HTTP_POST, web_route_snodedel);
  /* Per-sleutel toegangsgrants (wachtwoordloos) op een room/snode-slot. */
  _server->on("/room/acl", HTTP_POST, web_route_roomacl);
  _server->on("/snode/acl", HTTP_POST, web_route_snodeacl);
  /* Handmatig advert (flood/zero-hop) per room/snode. */
  _server->on("/room/advert", HTTP_POST, web_route_roomadvert);
  _server->on("/snode/advert", HTTP_POST, web_route_snodeadvert);
  /* SNMP-monitor aanmaken. POST-only, net als /monitor. */
  _server->on("/monitor/snmp", HTTP_POST, web_route_monsnmp);
  /* Ontdekte contacten (buurtlijst met VOLLEDIGE pubkey) -- de kiezer voor de
   * bot-ontvangers en de ACL-grants leest deze lijst. GET, alleen publieke sleutels. */
  _server->on("/contacts.json", HTTP_GET, web_route_contactsjson);
  /* Bots (CHAT/notifier): leeskant /bot.json + /bots.json (GET), mutaties POST.
   * De POST-endpoints nemen een optionele `bot=<idx-of-naam>` (default: alert-bot). */
  _server->on("/bot.json", HTTP_GET, web_route_botjson);
  _server->on("/bots.json", HTTP_GET, web_route_botsjson);
  _server->on("/bot/manage", HTTP_POST, web_route_botmanage);
  _server->on("/bot/recipient", HTTP_POST, web_route_botrecip);
  _server->on("/bot/advert", HTTP_POST, web_route_botadvert);
  _server->on("/bot/sendto", HTTP_POST, web_route_botsendto);
  _server->on("/bot/post", HTTP_POST, web_route_botpost);
  _server->on("/bot/diag", HTTP_POST, web_route_botdiag);
  _server->on("/channels.json", HTTP_GET, web_route_channelsjson);
  _server->on("/channel/add", HTTP_POST, web_route_channeladd);
  _server->on("/channel/del", HTTP_POST, web_route_channeldel);
  _server->on("/channel/toggle", HTTP_POST, web_route_channeltoggle);
  /* Companions (v2.4.0): leeskant /companions.json (GET, ook voor MeshManager),
   * mutaties via /companion (POST: key+name toevoegen/wijzigen, of del=prefix). */
  _server->on("/companions.json", HTTP_GET, web_route_companionsjson);
  _server->on("/companion", HTTP_POST, web_route_companion);
  _server->on("/messages.json", HTTP_GET, web_route_messagesjson);
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
/* Draagt dit verzoek een geldige sessiecookie? Alleen de cookie -- Basic-auth zit
 * in authOk() ernaast. */
bool WebTask::sessionValid() {
  char tok[WEB_SESS_HEX + 1];
  if (!webCookieToken(*_server, tok, sizeof(tok))) return false;
  if (strlen(tok) != WEB_SESS_HEX) return false;
  webSessionSweep();
  for (int i = 0; i < WEB_SESS_MAX; i++) {
    if (g_sess[i].used && webCtEqual(g_sess[i].tok, tok)) return true;
  }
  return false;
}

/* De twee wegen samen: cookie (de mens) OF Basic-auth (de server). De cookie eerst,
 * want die is goedkoop en het gewone geval voor een browser; anders de Basic-toets
 * tegen de OPGESLAGEN credential (g_web_user/g_web_pass -- niet de gebakken macro's;
 * die zijn enkel de terugval bij een verse node, zie begin()). */
bool WebTask::authOk() {
  if (sessionValid()) return true;
  return _server->authenticate(g_web_user, g_web_pass);
}

bool WebTask::requireAuth() {
  /* De API-poort. Geldig via cookie of Basic -> door. Anders een 401 met de
   * Basic-uitdaging: de MeshManager-server (die Basic vooraf meestuurt) werkt zo
   * gewoon door, en een browser-fetch() krijgt de 401 zonder dat er een inlogpopup
   * opent -- de pagina stuurt zichzelf dan naar /login (zie de fetch-wikkel in
   * PAGE_HTML). Zo blijft de machineweg heel terwijl de popup voor mensen weg is. */
  if (authOk()) return true;
  _server->requestAuthentication(BASIC_AUTH, WEB_REALM);
  return false;
}

void WebTask::handleRoot() {
  /* De MENS-gerichte pagina. Zonder geldige sessie/Basic NIET met een 401 antwoorden
   * (dat opent de lelijke popup bij een navigatie) maar netjes doorsturen naar de
   * eigen inlogpagina. */
  if (!authOk()) {
    _server->sendHeader("Location", "/login");
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(302, "text/plain", "");
    return;
  }
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

static const char* adhocStateName(MonitorSensors::AdhocState s) {
  switch (s) {
    case MonitorSensors::ADHOC_PENDING: return "wacht";
    case MonitorSensors::ADHOC_BUSY:    return "bezig";
    case MonitorSensors::ADHOC_DONE:    return "klaar";
    default:                            return "niets";
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

  /* De ad-hoc ping apart, want de uitslagtekst is lang en hoort niet in de marge
   * van simblk te hoeven passen. Alleen de tekst als hij klaar is; anders leeg.
   * host en tekst bevatten alleen tekens uit validHost en onze eigen woorden, dus
   * niets om te ontsnappen. */
  char adhocblk[300];
  adhocblk[0] = 0;
  if (_mon != nullptr) {
    const MonitorSensors::AdhocState as = _mon->adhocState();
    snprintf(adhocblk, sizeof(adhocblk),
        "\"adhoc\":{\"st\":\"%s\",\"host\":\"%s\",\"txt\":\"%s\"},",
        adhocStateName(as), _mon->adhocHost(),
        as == MonitorSensors::ADHOC_DONE ? _mon->adhocResultText() : "");
  }

  char simblk[320];
  simblk[0] = 0;
  if (_mon != nullptr) {
    uint8_t nag = 0, capped = 0;
    _mon->repeatStatus(nag, capped);
    snprintf(simblk, sizeof(simblk),
        "\"sim\":{\"n\":%u,\"max\":%u,\"secs\":%u,\"min\":%u,\"lim\":%u},"
        "\"rec\":{\"on\":%d,\"hold\":%u},"
        "\"rep\":{\"secs\":%u,\"min\":%u,\"lim\":%u,\"cap\":%u,\"nag\":%u,\"maxed\":%u},"
        "\"test\":{\"st\":\"%s\",\"seq\":%u,\"rc\":%u,\"ack\":%u,\"age\":%lu,"
        "\"wait\":%lu,\"rcnow\":%u},",
        (unsigned)_mon->simActiveCount(),
        (unsigned)MonitorSensors::MAX_SIM_ACTIVE,
        (unsigned)MonitorSensors::SIM_SECS_DEFAULT,
        (unsigned)MonitorSensors::SIM_SECS_MIN,
        (unsigned)MonitorSensors::SIM_SECS_MAX,
        _mon->recoverEnabled() ? 1 : 0,
        (unsigned)_mon->recoverHoldSecs(),
        (unsigned)_mon->alertRepeatSecs(),
        (unsigned)MON_AREPEAT_MIN,
        (unsigned)MON_AREPEAT_MAX,
        (unsigned)MonitorSensors::MAX_ALERT_REPEATS,
        (unsigned)nag, (unsigned)capped,
        testStateName(_mon->testState()),
        (unsigned)_mon->testSeq(),
        (unsigned)_mon->testRecipients(),
        (unsigned)_mon->testAcks(),
        (unsigned long)_mon->testAgeSecs(),
        (unsigned long)_mon->testWaitSecs(),
        (unsigned)countAlertRecipients());
  }

  /* v2.3.6 REACTIETIJD -- de runtime-instelbare detectie-marges + hun grenzen.
   * Zo kan de MeshManager-server ze tonen (met sliders die de grenzen kennen) en
   * later ook zetten via "sensor set power.sample|power.confirm|power.settle|
   * read.interval". CONTRACT: velden psample/pconfirm/psettle/read/deb (huidige
   * waarden) + *_min/*_max per veld. */
  char timblk[360];
  timblk[0] = 0;
  if (_mon != nullptr) {
    snprintf(timblk, sizeof(timblk),
        "\"timing\":{\"psample\":%u,\"pconfirm\":%u,\"psettle\":%u,\"read\":%u,\"deb\":%u,"
        "\"psample_min\":%u,\"psample_max\":%u,\"pconfirm_min\":%u,\"pconfirm_max\":%u,"
        "\"psettle_min\":%u,\"psettle_max\":%u,\"read_min\":%u,\"read_max\":%u,"
        "\"deb_min\":%u,\"deb_max\":%u},",
        (unsigned)_mon->powerSampleSecs(), (unsigned)_mon->powerConfirm(),
        (unsigned)_mon->powerSettleSecs(), (unsigned)_mon->readIntervalSecs(),
        (unsigned)_mon->fixedDebounceSecs(),
        (unsigned)MON_PSAMPLE_MIN, (unsigned)MON_PSAMPLE_MAX,
        (unsigned)MON_PCONFIRM_MIN, (unsigned)MON_PCONFIRM_MAX,
        (unsigned)MON_PSETTLE_MIN, (unsigned)MON_PSETTLE_MAX,
        (unsigned)MON_READ_MIN, (unsigned)MON_READ_MAX,
        (unsigned)MON_FDEB_MIN, (unsigned)MON_FDEB_MAX);
  }

  int n = snprintf(g_json, sizeof(g_json),
      "{\"fw\":\"%s\",\"wifi\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"rssi\":%d,"
      "\"reason\":%u,\"reconnects\":%lu,\"resets\":%lu,\"uptime\":%lu,"
      "\"heap\":%lu,\"largest\":%lu,\"ssid\":\"%s\","
      "\"mains\":%d,\"volts\":\"%.3f\",\"paused\":%d,%s%s%s%s\"mon\":[",
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
      budblk, adhocblk, simblk, timblk);

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
          "\"si\":%u,\"sm\":\"%s\",\"sl\":%lu,\"tms\":%d,\"tb\":%u,\"drop\":%d,"
          "\"am\":%u,\"rm\":%u,\"sn\":%u,\"msv\":%u,\"knd\":%u,\"itp\":%u,\"oid\":\"%s\"}",
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
          _mon->monitorDropped(i) ? 1 : 0,
          /* am = alarm-route (1=dm,2=room,3=both), rm = room-set bitmasker,
           * sn = sensor-node-set bitmasker (welke virtuele sensor-nodes dit kanaal
           * als telemetrie tonen), msv = ingestelde ernst (0 hoog,1 midden,2 laag)
           * voor de ernst-emoji vooraan de storings-DM. Room-variant. */
          (unsigned)_mon->monitorAlertMode(i),
          (unsigned)_mon->monitorRoomsMask(i),
          (unsigned)_mon->monitorSensorNodesMask(i),
          (unsigned)_mon->monitorSeverity(i),
          /* knd = soort (0 host, 1 snmp), itp = SNMP-interpretatie, oid = de OID
           * (het community wordt NOOIT geexposeerd). */
          (unsigned)_mon->monitorKind(i),
          (unsigned)_mon->monitorSnmpInterp(i),
          _mon->monitorSnmpOid(i));
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

/* POST /time  (ntp, tz) -- NTP-server + tijdzone (POSIX-TZ) bewaren en METEEN
 * toepassen: de TZ op het proces (weergave) en de server aan WifiTask, gevolgd door
 * een her-sync. RTC en de MeshCore-PROTOCOLtijd blijven UTC; alleen de menselijke
 * WEERGAVE wordt lokaal. Anders dan /wifi hoeft dit GEEN herstart. */
void WebTask::handleTime() {
  if (!requireAuth()) return;

  char ntp[48], tz[48];
  if (!getArg(*_server, "ntp", ntp, sizeof(ntp)) || ntp[0] == 0)
    strlcpy(ntp, DEFAULT_NTP, sizeof(ntp));
  if (!getArg(*_server, "tz", tz, sizeof(tz)) || tz[0] == 0)
    strlcpy(tz, DEFAULT_TZ, sizeof(tz));

  if (!saveTimeConfig(ntp, tz)) {
    _server->send(500, "application/json", "{\"ok\":false,\"error\":\"opslaan mislukt\"}");
    return;
  }
  strlcpy(g_ntp_shown, ntp, sizeof(g_ntp_shown));
  strlcpy(g_tz_shown, tz, sizeof(g_tz_shown));

  applyTimeZone(tz);                     // weergave meteen lokaal
  if (_wifi) {
    _wifi->setNtpServer(ntp);
    _wifi->syncNow();                    // her-sync met de nieuwe server (indien online)
  }
  _server->send(200, "application/json", "{\"ok\":true}");
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

/* POST /monitor/snmp  (name, host, int, community, oid, interp, snmparg)
 * Maakt een SNMP-monitor: eerst een gewone monitor (naam/host/interval), daarna de
 * SNMP-velden via de bestaande setSettingValue-weg. Host = doel-IP (of naam). */
void WebTask::handleMonSnmp() {
  if (!requireAuth()) return;
  if (_mon == nullptr) { _server->send(503, "text/plain", "sensorlaag niet gekoppeld"); return; }

  char name[MON_NAME_LEN + 4], host[MON_HOST_LEN + 4], comm[24], oid[80], interp[12], sarg[16];
  getArg(*_server, "name", name, sizeof(name));
  getArg(*_server, "host", host, sizeof(host));
  getArg(*_server, "community", comm, sizeof(comm));
  getArg(*_server, "oid", oid, sizeof(oid));
  getArg(*_server, "interp", interp, sizeof(interp));
  getArg(*_server, "snmparg", sarg, sizeof(sarg));
  unsigned long ivl = 0;
  if (!getUInt(*_server, "int", MON_INTERVAL_MIN, MON_INTERVAL_MAX, MON_INTERVAL_DEFAULT, &ivl)) {
    _server->send(400, "text/plain", MonitorSensors::monResultText(MonitorSensors::MON_ERR_INTERVAL));
    return;
  }
  if (!name[0] || !host[0] || !oid[0]) {
    _server->send(400, "text/plain", "naam, host/ip en oid zijn verplicht\n");
    return;
  }

  uint8_t ch = 0;
  MonitorSensors::MonResult r = _mon->createMonitor(name, host, (uint16_t)ivl, &ch);
  if (r != MonitorSensors::MON_OK) {
    _server->send(httpCodeFor(r), "text/plain", MonitorSensors::monResultText(r));
    return;
  }
  char key[24];
  snprintf(key, sizeof(key), "mon.%u.type", (unsigned)ch); _mon->setSettingValue(key, "snmp");
  snprintf(key, sizeof(key), "mon.%u.oid", (unsigned)ch);
  if (!_mon->setSettingValue(key, oid)) {
    /* Ongeldige OID -> de monitor is al gemaakt; laat 'm als host staan, meld het. */
    snprintf(key, sizeof(key), "mon.%u.type", (unsigned)ch); _mon->setSettingValue(key, "host");
    _server->send(400, "text/plain", "ongeldige OID (alleen cijfers en punten)\n");
    return;
  }
  if (comm[0])   { snprintf(key, sizeof(key), "mon.%u.community", (unsigned)ch); _mon->setSettingValue(key, comm); }
  if (interp[0]) { snprintf(key, sizeof(key), "mon.%u.interp", (unsigned)ch);    _mon->setSettingValue(key, interp); }
  if (sarg[0])   { snprintf(key, sizeof(key), "mon.%u.snmparg", (unsigned)ch);   _mon->setSettingValue(key, sarg); }

  char msg[80];
  snprintf(msg, sizeof(msg), "ok snmp %s -> kanaal %u\n", name, (unsigned)ch);
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

  /* De klik belooft een onmiddellijk gevolg, dus trek de leesronde naar voren.
   * Zonder dit werkt de forcering wel meteen op monitorAlert(), maar wacht het
   * BERICHT tot de gewone ronde -- gemiddeld 30, hoogstens 60 s -- en dan denkt
   * wie op 'neer' klikt dat de knop stuk is. Ook bij OPHEFFEN (SIM_OFF): een
   * vroegtijdig herstel hoort net zo goed meteen zichtbaar te zijn. */
  if (_acl != nullptr) _acl->requestSensorReadNow();

  char msg[160];
  if (m == MonitorSensors::SIM_OFF) {
    snprintf(msg, sizeof(msg), "sensor %lu terug op de meting\n", idx);
  } else {
    /* De vervaltijd staat IN het antwoord, en niet alleen op de pagina. Wie dit
     * met een script doet moet ook weten wanneer het van zichzelf ophoudt. */
    snprintf(msg, sizeof(msg),
             "sensor %lu geforceerd op '%s' voor %lus; daarna vanzelf terug naar "
             "de meting. De eerste zendpoging volgt binnen enkele seconden.\n",
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

  /* Ook hier de ronde naar voren: alles terug op de meting hoort meteen te
   * kloppen, niet pas na een leesinterval. */
  if (_acl != nullptr) _acl->requestSensorReadNow();

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

  /* De leesronde naar voren: het testbericht gaat langs het echte alertpad
   * (onSensorDataRead -> alertIf), en dat pad draait normaal pas bij de volgende
   * ronde. Met deze duw is de eerste zendpoging er binnen enkele seconden in
   * plaats van na maximaal een minuut -- zonder aan het pad zelf iets te
   * veranderen. */
  if (_acl != nullptr) _acl->requestSensorReadNow();

  char msg[200];
  snprintf(msg, sizeof(msg),
           "testbericht #%u aangevraagd voor %u ontvanger(s). De eerste "
           "zendpoging volgt binnen enkele seconden langs hetzelfde pad als een "
           "echte waarschuwing; daarna de gewone bezorgtijd over het mesh. "
           "Bevestigde aflevering komt hieronder te staan.\n",
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

/* WEERGAVE-plafond voor de buurt-/contactenlijst in JSON. De lijst zelf is groot
 * (MAX_NEIGHBOURS=200, voor NAAMRESOLUTIE), maar /acl.json en /contacts.json tonen
 * hoogstens de eerste NB_JSON_MAX ingangen -- genoeg voor de kiezer, en het houdt
 * g_acl/g_json klein (een JSON van 200 contacten is onbruikbaar in de pagina). */
#ifndef NB_JSON_MAX
  #define NB_JSON_MAX  64
#endif

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
  const uint32_t now = _acl->nowSecs();

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

  for (int i = 0; i < nb.getNumEntries() && i < NB_JSON_MAX; i++) {
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

/* ================================ room-beheer =============================
 *
 * Alle mutaties lopen via de IWebNode-room-API (webRoomAdd/Edit/Del/…), en die
 * loopt in RoomMesh op zijn beurt via handleRoomCommand -- dezelfde keuring,
 * dezelfde persistentie en dezelfde adverts als de CLI. WebTask bouwt hier dus
 * GEEN room-logica na; het vertaalt alleen HTTP <-> die API. De join-URI wordt
 * door de node opgebouwd (die kent de room-pubkey), niet hier.
 *
 * webRoomMax() == 0 betekent "deze node kent geen rooms" -- dat is de sensor-
 * variant, die IWebNode's standaarden laat staan. Dan antwoorden we netjes met
 * 501 i.p.v. te doen alsof er rooms zijn. */

/* Kleine helper: een geheel-getal-argument uit het formulier. Geeft def terug als
 * het ontbreekt of geen cijfer is. */
static int getArgInt(WebServer& s, const char* name, int def) {
  char buf[12];
  if (!getArg(s, name, buf, sizeof(buf)) || buf[0] == 0) return def;
  return atoi(buf);
}

/* true als deze node rooms kent EN de meshlaag gekoppeld is; anders is het
 * antwoord al verstuurd (503/501). */
bool WebTask::roomsAvailable() {
  if (_acl == nullptr) {
    _server->send(503, "text/plain", "meshlaag niet gekoppeld");
    return false;
  }
  if (_acl->webRoomMax() <= 0) {
    _server->send(501, "application/json",
        "{\"ok\":false,\"error\":\"deze node kent geen rooms (sensor-variant)\"}");
    return false;
  }
  return true;
}

/* GET /rooms.json -- de hele room-stand, incl. de join-URI per room. `guest` is
 * een boolean (of er een gastwachtwoord gezet is) -- NOOIT het wachtwoord zelf. */
void WebTask::handleRoomsJson() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;

  const int max = _acl->webRoomMax();
  int n = snprintf(g_json, sizeof(g_json),
      "{\"max\":%d,\"active\":%d,\"rooms\":[", max, _acl->webRoomActiveCount());

  char esc[NB_JSON_NAME];
  char pub[PUB_KEY_SIZE * 2 + 1];
  char uri[200];
  char euri[300];
  bool first = true;
  for (int i = 0; i < max; i++) {
    if (!_acl->webRoomActive(i)) continue;
    if ((size_t)n > sizeof(g_json) - 640) break;   // marge; kap niet halverwege af
    jsonEscape(_acl->webRoomName(i), esc, sizeof(esc));
    pub[0] = 0;  _acl->webRoomPubHex(i, pub, sizeof(pub));
    uri[0] = 0;  _acl->webRoomJoinUri(i, uri, sizeof(uri));
    jsonEscape(uri, euri, sizeof(euri));
    n += snprintf(g_json + n, sizeof(g_json) - n,
        "%s{\"idx\":%d,\"name\":\"%s\",\"stealth\":%s,\"guest\":%s,\"posts\":%d,"
        "\"pub\":\"%s\",\"uri\":\"%s\",\"kind\":\"room\"",
        first ? "" : ",", i, esc,
        _acl->webRoomStealth(i) ? "true" : "false",
        _acl->webRoomHasGuest(i) ? "true" : "false",
        _acl->webRoomPosts(i), pub, euri);
    n = appendAclJson(_acl, g_json, sizeof(g_json), n, 0, i);   // 0 = room
    n += snprintf(g_json + n, sizeof(g_json) - n, "}");
    first = false;
  }

  /* Virtuele sensor-nodes: aparte array, symmetrisch met "rooms". Per node de
   * kanalen die eraan hangen (via het sensornodes-masker) zodat MeshManager de
   * groepering kan tonen. */
  const int smax = _acl->webSNodeMax();
  n += snprintf(g_json + n, sizeof(g_json) - n,
      "],\"snode_max\":%d,\"snode_active\":%d,\"snodes\":[", smax, _acl->webSNodeActiveCount());
  first = true;
  for (int i = 0; i < smax; i++) {
    if (!_acl->webSNodeActive(i)) continue;
    if ((size_t)n > sizeof(g_json) - 700) break;
    jsonEscape(_acl->webSNodeName(i), esc, sizeof(esc));
    pub[0] = 0;  _acl->webSNodePubHex(i, pub, sizeof(pub));
    uri[0] = 0;  _acl->webSNodeJoinUri(i, uri, sizeof(uri));
    jsonEscape(uri, euri, sizeof(euri));
    n += snprintf(g_json + n, sizeof(g_json) - n,
        "%s{\"idx\":%d,\"name\":\"%s\",\"stealth\":%s,\"pub\":\"%s\",\"uri\":\"%s\","
        "\"kind\":\"sensor\",\"channels\":[",
        first ? "" : ",", i, esc,
        _acl->webSNodeStealth(i) ? "true" : "false", pub, euri);
    first = false;
    /* De kanalen die aan deze sensor-node hangen. */
    if (_mon != nullptr) {
      bool fc = true;
      /* Vaste bronnen (kanaal 2/3/4) volgen hun eigen snode-masker. */
      if (_mon->fixedSensorNodesMask(MON_FA_MAINS) & (1u << i)) {
        n += snprintf(g_json + n, sizeof(g_json) - n, "%s%u,%u", fc ? "" : ",",
                      (unsigned)MonitorSensors::CH_MAINS, (unsigned)MonitorSensors::CH_BATTERY); fc = false;
      }
      if (_mon->fixedSensorNodesMask(MON_FA_WIFI) & (1u << i)) {
        n += snprintf(g_json + n, sizeof(g_json) - n, "%s%u", fc ? "" : ",",
                      (unsigned)MonitorSensors::CH_WIFI); fc = false;
      }
      for (int s = 0; s < MonitorSensors::MAX_MONITORS; s++) {
        if (!_mon->monitorUsed(s)) continue;
        if (!(_mon->monitorSensorNodesMask(s) & (1u << i))) continue;
        if ((size_t)n > sizeof(g_json) - 40) break;
        n += snprintf(g_json + n, sizeof(g_json) - n, "%s%u", fc ? "" : ",",
                      (unsigned)_mon->monitorChannel(s)); fc = false;
      }
    }
    n += snprintf(g_json + n, sizeof(g_json) - n, "]");   // sluit channels
    n = appendAclJson(_acl, g_json, sizeof(g_json), n, 1, i);   // 1 = sensor-node
    n += snprintf(g_json + n, sizeof(g_json) - n, "}");
  }
  strlcat(g_json, "]}", sizeof(g_json));

  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_json);
}

/* POST /room/add  (name) -> {"ok":true,"idx":N,"uri":"..."} */
void WebTask::handleRoomAdd() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;

  char name[24];
  if (!getArg(*_server, "name", name, sizeof(name)) || name[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"naam ontbreekt\"}");
    return;
  }
  int idx = _acl->webRoomAdd(name);
  if (idx < 0) {
    _server->send(503, "application/json",
        "{\"ok\":false,\"error\":\"geen vrije room-slot\"}");
    return;
  }
  char uri[200] = {0};
  _acl->webRoomJoinUri(idx, uri, sizeof(uri));
  char euri[300]; jsonEscape(uri, euri, sizeof(euri));
  char msg[360];
  snprintf(msg, sizeof(msg), "{\"ok\":true,\"idx\":%d,\"uri\":\"%s\"}", idx, euri);
  _server->send(200, "application/json", msg);
}

/* POST /room/edit  (idx, optioneel name,pass,guest,stealth=0/1) -> {"ok":true} */
void WebTask::handleRoomEdit() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;

  int idx = getArgInt(*_server, "idx", -1);
  if (idx < 0 || idx >= _acl->webRoomMax() || !_acl->webRoomActive(idx)) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"ongeldige idx\"}");
    return;
  }

  char name[24], pass[16], guest[16], st[4], gc[4];
  bool has_name  = getArg(*_server, "name",  name,  sizeof(name));
  bool has_pass  = getArg(*_server, "pass",  pass,  sizeof(pass));
  bool has_guest = getArg(*_server, "guest", guest, sizeof(guest));
  bool has_st    = getArg(*_server, "stealth", st, sizeof(st));
  bool clear_guest = getArg(*_server, "guest_clear", gc, sizeof(gc)) && gc[0] == '1';

  /* CONTRACT: lege/ontbrekende velden = ONGEWIJZIGD (zo stuurt de MeshManager-
   * server ze). Dat geldt ook voor 'guest' -- een lege waarde wist het gast-
   * wachtwoord dus NIET (dat zou de server per ongeluk doen). Wissen is een
   * expliciete handeling: 'guest_clear=1' (de "wis gast"-knop in de eigen GUI). */
  const char* namep  = (has_name  && name[0])  ? name  : nullptr;
  const char* passp  = (has_pass  && pass[0])  ? pass  : nullptr;
  const char* guestp = clear_guest ? "" : ((has_guest && guest[0]) ? guest : nullptr);
  int stealth = has_st ? (st[0] == '1' ? 1 : 0) : -1;

  if (!_acl->webRoomEdit(idx, namep, passp, guestp, stealth)) {
    _server->send(503, "application/json", "{\"ok\":false,\"error\":\"bewerken mislukt\"}");
    return;
  }
  _server->send(200, "application/json", "{\"ok\":true}");
}

/* POST /room/del  (idx) -> {"ok":true}. Room 0 mag niet weg. */
void WebTask::handleRoomDel() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;

  int idx = getArgInt(*_server, "idx", -1);
  if (idx <= 0) {
    _server->send(400, "application/json",
        "{\"ok\":false,\"error\":\"room 0 (hoofdidentiteit) kan niet weg\"}");
    return;
  }
  if (!_acl->webRoomDel(idx)) {
    _server->send(400, "application/json",
        "{\"ok\":false,\"error\":\"ongeldige of inactieve room\"}");
    return;
  }
  _server->send(200, "application/json", "{\"ok\":true}");
}

/* GET /rooms/backup -- VOLLEDIGE room-config INCL. sleutels, als download.
 * GEVOELIG: alleen achter auth (requireAuth). Wordt niet gelogd. */
void WebTask::handleRoomsBackup() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;

  int len = _acl->webRoomsBackup(g_json, sizeof(g_json));
  if (len <= 0) {
    _server->send(503, "application/json",
        "{\"ok\":false,\"error\":\"backup kon niet worden opgebouwd\"}");
    return;
  }
  _server->sendHeader("Cache-Control", "no-store");
  _server->sendHeader("Content-Disposition",
                      "attachment; filename=\"meshuptime-rooms-backup.json\"");
  _server->send(200, "application/json", g_json);
}

/* POST /rooms/restore  (JSON-body zoals de backup) -> {"ok":true}. Streng: alleen
 * op de eigen backup-marker; room 0 blijft tenzij "overwrite_main":"1". */
void WebTask::handleRoomsRestore() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;

  /* De ruwe body zit bij een niet-form POST in het argument "plain". We lezen hem
   * in g_json (ruim) i.p.v. op de stapel: een backup kan ~1 kB zijn en de
   * loop-taak deelt zijn stapel met de mesh. */
  if (!getArg(*_server, "plain", g_json, sizeof(g_json)) || g_json[0] == 0) {
    _server->send(400, "application/json",
        "{\"ok\":false,\"error\":\"lege body -- stuur de backup-JSON\"}");
    return;
  }
  if (!_acl->webRoomsRestore(g_json)) {
    _server->send(400, "application/json",
        "{\"ok\":false,\"error\":\"ongeldige backup (verkeerde marker of vorm)\"}");
    return;
  }
  _server->send(200, "application/json", "{\"ok\":true}");
}

/* ---- VIRTUELE SENSOR-NODES (symmetrisch met /room/*) ---- */

/* POST /snode/add (name) -> {"ok":true,"idx":N,"uri":"..."} */
void WebTask::handleSNodeAdd() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;
  char name[24];
  if (!getArg(*_server, "name", name, sizeof(name)) || name[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"naam ontbreekt\"}");
    return;
  }
  int idx = _acl->webSNodeAdd(name);
  if (idx < 0) {
    _server->send(503, "application/json", "{\"ok\":false,\"error\":\"geen vrije sensor-node-slot\"}");
    return;
  }
  char uri[200] = {0};
  _acl->webSNodeJoinUri(idx, uri, sizeof(uri));
  char euri[300]; jsonEscape(uri, euri, sizeof(euri));
  char msg[360];
  snprintf(msg, sizeof(msg), "{\"ok\":true,\"idx\":%d,\"uri\":\"%s\"}", idx, euri);
  _server->send(200, "application/json", msg);
}

/* POST /snode/edit (idx, optioneel name, stealth=0/1) -> {"ok":true} */
void WebTask::handleSNodeEdit() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;
  int idx = getArgInt(*_server, "idx", -1);
  if (idx < 0 || idx >= _acl->webSNodeMax() || !_acl->webSNodeActive(idx)) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"ongeldige idx\"}");
    return;
  }
  char name[24], st[4];
  bool has_name = getArg(*_server, "name", name, sizeof(name));
  bool has_st   = getArg(*_server, "stealth", st, sizeof(st));
  const char* namep = (has_name && name[0]) ? name : nullptr;
  int stealth = has_st ? (st[0] == '1' ? 1 : 0) : -1;
  if (!_acl->webSNodeEdit(idx, namep, stealth)) {
    _server->send(503, "application/json", "{\"ok\":false,\"error\":\"bewerken mislukt\"}");
    return;
  }
  _server->send(200, "application/json", "{\"ok\":true}");
}

/* POST /snode/del (idx) -> {"ok":true} */
void WebTask::handleSNodeDel() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;
  int idx = getArgInt(*_server, "idx", -1);
  if (idx < 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"ongeldige idx\"}");
    return;
  }
  if (!_acl->webSNodeDel(idx)) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"ongeldige of inactieve sensor-node\"}");
    return;
  }
  _server->send(200, "application/json", "{\"ok\":true}");
}

/* read/readwrite/admin -> 1/2/3; 0 = onbekend. */
static int aclLevelWord(const char* w) {
  if (!strcasecmp(w, "read") || !strcasecmp(w, "ro") || !strcasecmp(w, "readonly")) return 1;
  if (!strcasecmp(w, "readwrite") || !strcasecmp(w, "rw") || !strcasecmp(w, "write")) return 2;
  if (!strcasecmp(w, "admin")) return 3;
  int n = atoi(w);
  return (n >= 1 && n <= 3) ? n : 0;
}

/* POST /room/acl of /snode/acl. Velden: idx, pubkey, level (read|readwrite|admin)
 * OF del=1 (dan pubkey = prefix). TOEVOEGEN vraagt de VOLLEDIGE pubkey (64 hex);
 * VERWIJDEREN mag op een prefix (>=12 hex), geweigerd bij >1 treffer. */
void WebTask::handleAclEndpoint(int kind) {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;

  int idx = getArgInt(*_server, "idx", -1);
  if (idx < 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"idx ontbreekt\"}");
    return;
  }
  char pk[PUB_KEY_SIZE * 2 + 8];
  if (!getArg(*_server, "pubkey", pk, sizeof(pk)) || pk[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"pubkey ontbreekt\"}");
    return;
  }
  char del[4];
  bool isdel = getArg(*_server, "del", del, sizeof(del)) && del[0] == '1';

  if (isdel) {
    int rc = _acl->webAclDel(kind, idx, pk);
    if (rc == 1) { _server->send(200, "application/json", "{\"ok\":true}"); return; }
    const char* err = (rc == -3) ? "prefix past op meerdere ingangen"
                    : (rc == -2) ? "niet gevonden of ongeldige prefix (min. 12 hex)"
                    : "ongeldig slot";
    char msg[128]; snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"%s\"}", err);
    _server->send(400, "application/json", msg);
    return;
  }

  char lvlw[16];
  getArg(*_server, "level", lvlw, sizeof(lvlw));
  int level = aclLevelWord(lvlw);
  if (level == 0) {
    _server->send(400, "application/json",
        "{\"ok\":false,\"error\":\"level: read|readwrite|admin\"}");
    return;
  }
  int rc = _acl->webAclSet(kind, idx, pk, level);
  if (rc == 0) {
    char msg[64]; snprintf(msg, sizeof(msg), "{\"ok\":true,\"level\":%d}", level);
    _server->send(200, "application/json", msg);
    return;
  }
  const char* err = (rc == -3) ? "grant-tabel vol"
                  : (rc == -2) ? "toevoegen vraagt de VOLLEDIGE pubkey (64 hex)"
                  : "ongeldig slot";
  char msg[128]; snprintf(msg, sizeof(msg), "{\"ok\":false,\"error\":\"%s\"}", err);
  _server->send(400, "application/json", msg);
}

void WebTask::handleRoomAcl()  { handleAclEndpoint(0); }
void WebTask::handleSNodeAcl() { handleAclEndpoint(1); }

/* POST /room/advert of /snode/advert (idx, flood=0/1). */
void WebTask::handleRoomAdvert() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;
  int idx = getArgInt(*_server, "idx", -1);
  char fl[4]; bool flood = getArg(*_server, "flood", fl, sizeof(fl)) && fl[0] == '1';
  if (idx < 0 || !_acl->webRoomAdvert(idx, flood)) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"ongeldige of inactieve room\"}");
    return;
  }
  char msg[64]; snprintf(msg, sizeof(msg), "{\"ok\":true,\"flood\":%d}", flood ? 1 : 0);
  _server->send(200, "application/json", msg);
}
void WebTask::handleSNodeAdvert() {
  if (!requireAuth()) return;
  if (!roomsAvailable()) return;
  int idx = getArgInt(*_server, "idx", -1);
  char fl[4]; bool flood = getArg(*_server, "flood", fl, sizeof(fl)) && fl[0] == '1';
  if (idx < 0 || !_acl->webSNodeAdvert(idx, flood)) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"ongeldige of inactieve sensor-node\"}");
    return;
  }
  char msg[64]; snprintf(msg, sizeof(msg), "{\"ok\":true,\"flood\":%d}", flood ? 1 : 0);
  _server->send(200, "application/json", msg);
}

/* ================================================================== */
/*  Ontdekte contacten + bot (CHAT/notifier)                           */
/* ================================================================== */

/* GET /contacts.json -- de buurtlijst met VOLLEDIGE pubkey, voor de kiezers (bot-
 * ontvangers + ACL-grants). Alleen publieke gegevens. */
void WebTask::handleContactsJson() {
  if (!requireAuth()) return;
  if (_acl == nullptr) { _server->send(503, "text/plain", "meshlaag niet gekoppeld"); return; }

  const NeighbourList& nb = _acl->getNeighbours();
  const uint32_t now = _acl->nowSecs();
  char key[PUB_KEY_SIZE * 2 + 1];
  char esc[NB_JSON_NAME];

  int n = snprintf(g_json, sizeof(g_json), "{\"max\":%d,\"shown\":%d,\"contacts\":[",
                   MAX_NEIGHBOURS, NB_JSON_MAX);
  bool first = true;
  for (int i = 0; i < nb.getNumEntries() && i < NB_JSON_MAX; i++) {
    const NeighbourEntry* e = nb.getEntryByIdx(i);
    if ((size_t)n > sizeof(g_json) - 160) break;
    mesh::Utils::toHex(key, e->pub_key, PUB_KEY_SIZE);
    jsonEscape(e->name, esc, sizeof(esc));
    n += snprintf(g_json + n, sizeof(g_json) - n,
        "%s{\"k\":\"%s\",\"n\":\"%s\",\"t\":%u,\"h\":%u,\"s\":\"%.1f\",\"a\":%ld,\"c\":%lu}",
        first ? "" : ",", key, esc, (unsigned)e->adv_type, (unsigned)e->hops,
        ((float)e->snr4) / 4.0f, ageOf(e->heard_at, now), (unsigned long)e->count);
    first = false;
  }
  strlcat(g_json, "]}", sizeof(g_json));
  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_json);
}

/* GET /companions.json -- {max, companions:[{name,pubkey,lat,lon,seen}...]}.
 * seen = RTC-tijd (s) van het laatste #LOC-rapport (0 = nooit); lat/lon ontbreken
 * zolang er geen locatie is. Bedoeld voor de GUI én voor MeshManager om te pollen.
 * Auth zoals de andere endpoints; alleen publieke pubkeys, nooit geheimen. */
void WebTask::handleCompanionsJson() {
  if (!requireAuth()) return;
  if (_acl == nullptr) { _server->send(503, "text/plain", "meshlaag niet gekoppeld"); return; }

  int max = _acl->webCompanionMax();
  int n = snprintf(g_json, sizeof(g_json), "{\"max\":%d,\"companions\":[", max);
  int cnt = _acl->webCompanionCount();
  char pub[PUB_KEY_SIZE * 2 + 1];
  char nm[24], esc[64];
  float lat = NAN, lon = NAN; uint32_t seen = 0; bool hasloc = false;
  uint32_t fall_ts = 0; int fall_kind = 0;
  static const char* kFallKind[] = { "", "val", "nomotion", "sos" };
  bool first = true;
  for (int i = 0; i < cnt; i++) {
    if ((size_t)n > sizeof(g_json) - 200) break;
    if (!_acl->webCompanionGet(i, nm, sizeof(nm), pub, sizeof(pub), &lat, &lon, &seen, &hasloc,
                               &fall_ts, &fall_kind)) continue;
    jsonEscape(nm, esc, sizeof(esc));
    n += snprintf(g_json + n, sizeof(g_json) - n, "%s{\"name\":\"%s\",\"pubkey\":\"%s\",\"seen\":%u",
                  first ? "" : ",", esc, pub, (unsigned)seen);
    if (hasloc)
      n += snprintf(g_json + n, sizeof(g_json) - n, ",\"lat\":%.6f,\"lon\":%.6f", lat, lon);
    /* Val-event alleen als er een is (fall_ts != 0). fall_kind 1..3 -> tekst. */
    if (fall_ts != 0 && fall_kind >= 1 && fall_kind <= 3)
      n += snprintf(g_json + n, sizeof(g_json) - n, ",\"fall_ts\":%u,\"fall_kind\":\"%s\"",
                    (unsigned)fall_ts, kFallKind[fall_kind]);
    n += snprintf(g_json + n, sizeof(g_json) - n, "}");
    first = false;
  }
  strlcat(g_json, "]}", sizeof(g_json));
  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_json);
}

/* GET /messages.json -- inkomende-berichten-inbox: alle inkomende companion-DM's
 * (commando-antwoorden, #LOC-rapporten, enz.), nieuwste eerst. Leesbaar in de
 * node-GUI en opvraagbaar door MeshManager. */
void WebTask::handleMessagesJson() {
  if (!requireAuth()) return;
  if (_acl == nullptr) { _server->send(503, "text/plain", "meshlaag niet gekoppeld"); return; }
  int cnt = _acl->webMsgCount();
  int n = snprintf(g_json, sizeof(g_json), "{\"messages\":[");
  char pub[PUB_KEY_SIZE * 2 + 1];
  char nm[24], txt[140], esc_nm[64], esc_tx[300];
  uint32_t ts = 0;
  bool first = true;
  for (int i = 0; i < cnt; i++) {
    if ((size_t)n > sizeof(g_json) - 420) break;   // laat ruimte voor deze entry + "]}"
    if (!_acl->webMsgGet(i, nm, sizeof(nm), pub, sizeof(pub), txt, sizeof(txt), &ts)) continue;
    jsonEscape(nm, esc_nm, sizeof(esc_nm));
    jsonEscape(txt, esc_tx, sizeof(esc_tx));
    n += snprintf(g_json + n, sizeof(g_json) - n,
                  "%s{\"name\":\"%s\",\"pubkey\":\"%s\",\"ts\":%u,\"text\":\"%s\"}",
                  first ? "" : ",", esc_nm, pub, (unsigned)ts, esc_tx);
    first = false;
  }
  strlcat(g_json, "]}", sizeof(g_json));
  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_json);
}

/* POST /companion -- toevoegen/wijzigen (key=64hex + name) of verwijderen
 * (del=prefix>=12hex). Companions leven alleen op de room-server. */
void WebTask::handleCompanion() {
  if (!requireAuth()) return;
  if (_acl == nullptr) { _server->send(503, "text/plain", "meshlaag niet gekoppeld"); return; }
  if (_acl->webCompanionMax() == 0) {
    _server->send(501, "application/json",
        "{\"ok\":false,\"error\":\"deze node kent geen companions (sensor-variant)\"}");
    return;
  }

  char del[PUB_KEY_SIZE * 2 + 1];
  if (getArg(*_server, "del", del, sizeof(del)) && del[0]) {
    int r = _acl->webCompanionDel(del);
    if (r == 1) { _server->send(200, "application/json", "{\"ok\":true}"); return; }
    _server->send(400, "application/json",
        r == -3 ? "{\"ok\":false,\"error\":\"prefix past op meerdere\"}"
                : "{\"ok\":false,\"error\":\"niet gevonden of ongeldige prefix\"}");
    return;
  }
  char key[PUB_KEY_SIZE * 2 + 1];
  if (!getArg(*_server, "key", key, sizeof(key)) || strlen(key) != PUB_KEY_SIZE * 2) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"volledige pubkey (64 hex) nodig\"}");
    return;
  }
  char name[24];
  if (!getArg(*_server, "name", name, sizeof(name))) name[0] = 0;
  int r = _acl->webCompanionSet(key, name);
  if (r == 0) { _server->send(200, "application/json", "{\"ok\":true}"); return; }
  _server->send(400, "application/json",
      r == -3 ? "{\"ok\":false,\"error\":\"companionlijst vol\"}"
              : "{\"ok\":false,\"error\":\"volledige pubkey (64 hex) nodig\"}");
}

/* Kleine guard: bestaat er een bot op deze node? (SensorMesh: nee.) */
bool WebTask::botAvailable() {
  if (_acl == nullptr) { _server->send(503, "text/plain", "meshlaag niet gekoppeld"); return false; }
  if (!_acl->webBotActive()) {
    _server->send(501, "application/json",
        "{\"ok\":false,\"error\":\"deze node kent geen bot (sensor-variant)\"}");
    return false;
  }
  return true;
}

/* De `bot=`-selector (idx of naam) uit de request oplossen naar een slot; leeg
 * -> de alert-bot. -1 als onbekend. */
int WebTask::botArgIndex() {
  char sel[24]; sel[0] = 0;
  getArg(*_server, "bot", sel, sizeof(sel));
  return _acl->webBotResolve(sel);   // "" -> alert-bot
}

/* Eén bot-slot als JSON-VELDEN (zonder omhullende { }) naar buf; retour = lengte.
 * De aanroeper zet zelf de accolades eromheen. NOOIT geheimen. */
static int botSlotJson(IWebNode* acl, int i, char* buf, size_t cap, bool with_recips) {
  char name[48]; jsonEscape(acl->webBotSlotName(i), name, sizeof(name));
  char pub[PUB_KEY_SIZE * 2 + 1] = {0}; acl->webBotSlotPubHex(i, pub, sizeof(pub));
  char uri[200] = {0}; acl->webBotSlotJoinUri(i, uri, sizeof(uri));
  char euri[300]; jsonEscape(uri, euri, sizeof(euri));
  char durl[220]; jsonEscape(acl->webBotSlotDiagUrl(i), durl, sizeof(durl));
  int n = snprintf(buf, cap,
      "\"idx\":%d,\"name\":\"%s\",\"pub\":\"%s\",\"uri\":\"%s\","
      "\"alert\":%s,\"enabled\":%s,\"nrecips\":%d,\"max\":%d,"
      "\"diag\":%d,\"durlmode\":%d,\"durl\":\"%s\",\"durlmax\":%d,\"dfit\":[%d,%d,%d]",
      i, name, pub, euri,
      acl->webBotSlotIsAlert(i) ? "true" : "false",
      acl->webBotSlotEnabled(i) ? "true" : "false",
      acl->webBotSlotRecipCount(i), acl->webBotRecipMax(),
      acl->webBotSlotDiagMask(i), acl->webBotSlotDiagUrlMode(i), durl,
      acl->webBotDiagUrlMax(),
      acl->webBotSlotDiagUrlBudget(i, 0), acl->webBotSlotDiagUrlBudget(i, 1), acl->webBotSlotDiagUrlBudget(i, 2));
  if (with_recips && (size_t)n < cap - 12) {
    n += snprintf(buf + n, cap - n, ",\"recips\":[");
    int cnt = acl->webBotSlotRecipCount(i);
    char rk[PUB_KEY_SIZE * 2 + 1];
    int emitted = 0;
    for (int j = 0; j < cnt; j++) {
      if ((size_t)n > cap - 90) break;
      int lvl = 1;
      if (!acl->webBotSlotRecipGet(i, j, rk, sizeof(rk), &lvl)) continue;
      n += snprintf(buf + n, cap - n, "%s{\"k\":\"%s\",\"l\":%d}", emitted ? "," : "", rk, lvl);
      emitted++;
    }
    n += snprintf(buf + n, cap - n, "]");
  }
  return n;
}

/* GET /bot.json[?bot=<idx-of-naam>] -- botstatus + join-uri + ontvangerslijst van
 * ÉÉN bot (default: de alert-bot). Vorm compatibel met v2.4.0 (bestaande GUI +
 * MeshManager blijven werken), met extra velden idx/alert erbij. NOOIT geheimen. */
void WebTask::handleBotJson() {
  if (!requireAuth()) return;
  if (!botAvailable()) return;
  int i = botArgIndex();
  if (i < 0) { _server->send(400, "application/json", "{\"active\":false,\"error\":\"onbekende bot\"}"); return; }
  int n = snprintf(g_json, sizeof(g_json), "{\"active\":true,");
  n += botSlotJson(_acl, i, g_json + n, sizeof(g_json) - n, true);
  strlcat(g_json, "}", sizeof(g_json));
  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_json);
}

/* GET /bots.json -- overzicht van ALLE bot-slots (naam/pub/rol/#recips), zodat
 * MeshManager de MGMT-bot z'n pubkey kan oppikken. NOOIT geheimen. */
void WebTask::handleBotsJson() {
  if (!requireAuth()) return;
  if (!botAvailable()) return;
  int n = snprintf(g_json, sizeof(g_json),
      "{\"max\":%d,\"alert\":%d,\"bots\":[", _acl->webBotSlotMax(), _acl->webBotAlertIdx());
  bool first = true;
  for (int i = 0; i < _acl->webBotSlotMax(); i++) {
    if (!_acl->webBotSlotUsed(i)) continue;
    if ((size_t)n > sizeof(g_json) - 420) break;
    n += snprintf(g_json + n, sizeof(g_json) - n, "%s{", first ? "" : ",");
    n += botSlotJson(_acl, i, g_json + n, sizeof(g_json) - n, false);
    n += snprintf(g_json + n, sizeof(g_json) - n, "}");
    first = false;
  }
  strlcat(g_json, "]}", sizeof(g_json));
  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_json);
}

/* POST /bot/manage -- add | rename | enable | del | setalert.
 *   op=add     name=<naam>              -> nieuwe bot (genereert sleutel)
 *   op=rename  bot=<sel> name=<naam>
 *   op=enable  bot=<sel> en=0/1
 *   op=del     bot=<sel>
 *   op=setalert bot=<sel> */
void WebTask::handleBotManage() {
  if (!requireAuth()) return;
  if (!botAvailable()) return;
  char op[16]; if (!getArg(*_server, "op", op, sizeof(op)) || op[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"op ontbreekt\"}"); return;
  }
  char name[24]; bool have_name = getArg(*_server, "name", name, sizeof(name));

  if (strcmp(op, "add") == 0) {
    int idx = _acl->webBotAdd(have_name && name[0] ? name : NULL);
    if (idx >= 0) { char out[48]; snprintf(out, sizeof(out), "{\"ok\":true,\"idx\":%d}", idx);
                    _server->send(200, "application/json", out); return; }
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"geen vrij slot\"}");
    return;
  }

  int i = botArgIndex();
  if (i < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"onbekende bot\"}"); return; }

  bool ok = false;
  if (strcmp(op, "rename") == 0)        ok = have_name && _acl->webBotRename(i, name);
  else if (strcmp(op, "enable") == 0)   { char e[4]; getArg(*_server, "en", e, sizeof(e)); ok = _acl->webBotEnable(i, e[0] == '1'); }
  else if (strcmp(op, "del") == 0)      ok = _acl->webBotDel(i);
  else if (strcmp(op, "setalert") == 0) ok = _acl->webBotSetAlert(i);
  else { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"onbekende op\"}"); return; }

  _server->send(ok ? 200 : 400, "application/json",
      ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"actie geweigerd (laatste/alert-bot?)\"}");
}

/* POST /bot/recipient[?bot=] -- toevoegen (key=64hex) of verwijderen (del=prefix>=12hex). */
void WebTask::handleBotRecip() {
  if (!requireAuth()) return;
  if (!botAvailable()) return;
  int i = botArgIndex();
  if (i < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"onbekende bot\"}"); return; }

  char del[PUB_KEY_SIZE * 2 + 1];
  if (getArg(*_server, "del", del, sizeof(del)) && del[0]) {
    int r = _acl->webBotSlotRecipDel(i, del);
    if (r == 1) { _server->send(200, "application/json", "{\"ok\":true}"); return; }
    _server->send(400, "application/json",
        r == -3 ? "{\"ok\":false,\"error\":\"prefix past op meerdere\"}"
                : "{\"ok\":false,\"error\":\"niet gevonden of ongeldige prefix\"}");
    return;
  }
  char key[PUB_KEY_SIZE * 2 + 1];
  if (!getArg(*_server, "key", key, sizeof(key)) || key[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"key ontbreekt\"}");
    return;
  }
  int r = _acl->webBotSlotRecipSet(i, key, 1);
  if (r == 0) { _server->send(200, "application/json", "{\"ok\":true}"); return; }
  _server->send(400, "application/json",
      r == -3 ? "{\"ok\":false,\"error\":\"ontvangerslijst vol\"}"
              : "{\"ok\":false,\"error\":\"volledige pubkey (64 hex) nodig\"}");
}

/* POST /bot/advert[?bot=]  (flood=0/1). */
void WebTask::handleBotAdvert() {
  if (!requireAuth()) return;
  if (!botAvailable()) return;
  int i = botArgIndex();
  if (i < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"onbekende bot\"}"); return; }
  char fl[4]; bool flood = getArg(*_server, "flood", fl, sizeof(fl)) && fl[0] == '1';
  if (!_acl->webBotSlotAdvert(i, flood)) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"bot niet actief\"}");
    return;
  }
  char msg[64]; snprintf(msg, sizeof(msg), "{\"ok\":true,\"flood\":%d}", flood ? 1 : 0);
  _server->send(200, "application/json", msg);
}

/* POST /bot/sendto[?bot=]  (key=64hex, msg) -- ad-hoc schone DM (flash-melding).
 * De companion-MANAGEMENT-GUI geeft hier de MGMT-bot mee. */
void WebTask::handleBotSendto() {
  if (!requireAuth()) return;
  if (!botAvailable()) return;
  int i = botArgIndex();
  if (i < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"onbekende bot\"}"); return; }
  char key[PUB_KEY_SIZE * 2 + 1];
  if (!getArg(*_server, "key", key, sizeof(key)) || strlen(key) != PUB_KEY_SIZE * 2) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"volledige pubkey (64 hex) nodig\"}");
    return;
  }
  char msg[200];
  if (!getArg(*_server, "msg", msg, sizeof(msg)) || msg[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"bericht ontbreekt\"}");
    return;
  }
  int r = _acl->webBotSlotSendTo(i, key, msg);
  if (r == 0) { _server->send(200, "application/json", "{\"ok\":true}"); return; }
  _server->send(400, "application/json", "{\"ok\":false,\"error\":\"versturen mislukt\"}");
}

/* POST /bot/post[?bot=]  (msg) -- DM de hele ontvangerslijst van die bot. */
void WebTask::handleBotPost() {
  if (!requireAuth()) return;
  if (!botAvailable()) return;
  int i = botArgIndex();
  if (i < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"onbekende bot\"}"); return; }
  char msg[200];
  if (!getArg(*_server, "msg", msg, sizeof(msg)) || msg[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"bericht ontbreekt\"}");
    return;
  }
  int r = _acl->webBotSlotPost(i, msg);
  if (r >= 0) {
    char out[64]; snprintf(out, sizeof(out), "{\"ok\":true,\"sent\":%d}", r);
    _server->send(200, "application/json", out);
    return;
  }
  _server->send(400, "application/json", "{\"ok\":false,\"error\":\"wachtrij vol of geen ontvangers\"}");
}

/* POST /bot/diag[?bot=] -- zend-diagnose instellen; persistent op de node.
 *   mask=<0-7>    bit0 ping, bit1 test, bit2 path (0 = helemaal uit)
 *   urlmode=0..2  0=geen URL, 1=inline tussen haakjes, 2=apart kanaalbericht
 *   url=<tekst>   de URL zelf (leeg = ongewijzigd laten)
 * Elk veld is optioneel; wat ontbreekt blijft staan. */
void WebTask::handleBotDiag() {
  if (!requireAuth()) return;
  if (!botAvailable()) return;
  int i = botArgIndex();
  if (i < 0) { _server->send(400, "application/json", "{\"ok\":false,\"error\":\"onbekende bot\"}"); return; }

  bool any = false;
  char buf[8];
  if (getArg(*_server, "mask", buf, sizeof(buf)) && buf[0]) {
    _acl->webBotSlotSetDiagMask(i, atoi(buf));
    any = true;
  }
  char url[256];
  bool have_url = getArg(*_server, "url", url, sizeof(url));
  if (getArg(*_server, "urlmode", buf, sizeof(buf)) && buf[0]) {
    _acl->webBotSlotSetDiagUrl(i, atoi(buf), have_url && url[0] ? url : NULL);
    any = true;
  } else if (have_url && url[0]) {
    _acl->webBotSlotSetDiagUrl(i, _acl->webBotSlotDiagUrlMode(i), url);
    any = true;
  }
  if (!any) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"niets op te geven\"}");
    return;
  }

  char eurl[220]; jsonEscape(_acl->webBotSlotDiagUrl(i), eurl, sizeof(eurl));
  char out[320];
  snprintf(out, sizeof(out),
      "{\"ok\":true,\"diag\":%d,\"durlmode\":%d,\"durl\":\"%s\",\"dfit\":[%d,%d,%d]}",
      _acl->webBotSlotDiagMask(i), _acl->webBotSlotDiagUrlMode(i), eurl,
      _acl->webBotSlotDiagUrlBudget(i, 0), _acl->webBotSlotDiagUrlBudget(i, 1), _acl->webBotSlotDiagUrlBudget(i, 2));
  _server->send(200, "application/json", out);
}

/* ================================================================== */
/*  Hashtag-/publieke kanalen (web)                                    */
/* ================================================================== */

bool WebTask::channelsAvailable() {
  if (_acl == nullptr) { _server->send(503, "text/plain", "meshlaag niet gekoppeld"); return false; }
  if (_acl->webChannelMax() <= 0) {
    _server->send(501, "application/json",
        "{\"ok\":false,\"error\":\"deze node kent geen kanalen (sensor-variant)\"}");
    return false;
  }
  return true;
}

/* GET /channels.json -- de kanalenlijst (naam, sleutellengte, aan/uit, hash).
 * Het SECRET komt hier NOOIT in voor (schrijf-alleen). */
void WebTask::handleChannelsJson() {
  if (!requireAuth()) return;
  if (!channelsAvailable()) return;

  int n = snprintf(g_json, sizeof(g_json), "{\"max\":%d,\"channels\":[", _acl->webChannelMax());
  int cnt = _acl->webChannelCount();
  char nm[24], nesc[24 * 6 + 1], hh[4];
  for (int i = 0; i < cnt; i++) {
    if ((size_t)n > sizeof(g_json) - 140) break;
    int bits = 0; bool en = false, drv = false, pub = false;
    if (!_acl->webChannelGet(i, nm, sizeof(nm), &bits, &en, hh, &drv, &pub)) continue;
    jsonEscape(nm, nesc, sizeof(nesc));
    n += snprintf(g_json + n, sizeof(g_json) - n,
                  "%s{\"n\":\"%s\",\"bits\":%d,\"en\":%s,\"h\":\"%s\",\"drv\":%s,\"pub\":%s}",
                  i == 0 ? "" : ",", nesc, bits, en ? "true" : "false", hh,
                  drv ? "true" : "false", pub ? "true" : "false");
  }
  strlcat(g_json, "]}", sizeof(g_json));
  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", g_json);
}

/* POST /channel/add  (name, secret [leeg=hashtag/afgeleid, of 32/64 hex], enabled=0|1)
 * Zoals de MeshCore-app: geen secret -> HASHTAG-kanaal, sleutel = sha256(naam)[:16].
 * Het antwoord meldt of de sleutel is afgeleid + de kanaal-hash, zodat de GUI kan
 * tonen wat er gebeurde. */
void WebTask::handleChannelAdd() {
  if (!requireAuth()) return;
  if (!channelsAvailable()) return;
  char name[24], secret[80], en[4];
  if (!getArg(*_server, "name", name, sizeof(name)) || name[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"naam ontbreekt\"}"); return;
  }
  secret[0] = 0;
  getArg(*_server, "secret", secret, sizeof(secret));   // leeg mag: hashtag-kanaal
  bool enabled = !(getArg(*_server, "enabled", en, sizeof(en)) && en[0] == '0');   // standaard aan
  int r = _acl->webChannelAdd(name, secret[0] ? secret : "", enabled ? 1 : 0);
  if (r != 0) {
    _server->send(400, "application/json",
        r == -3 ? "{\"ok\":false,\"error\":\"kanalenlijst vol\"}"
                : "{\"ok\":false,\"error\":\"secret moet 32 of 64 hextekens zijn (of laat leeg)\"}");
    return;
  }
  /* Terugmelden wat er gebeurde: publiek/afgeleid/eigen sleutel + de kanaal-hash.
   * De lijst vinden op naam om de definitieve status/hash te lezen. */
  int cnt = _acl->webChannelCount();
  char nm[24], hh[4]; int bits = 0; bool cen = false, drv = false, pub = false;
  for (int i = 0; i < cnt; i++) {
    if (_acl->webChannelGet(i, nm, sizeof(nm), &bits, &cen, hh, &drv, &pub) && strcmp(nm, name) == 0) break;
  }
  char out[110];
  snprintf(out, sizeof(out), "{\"ok\":true,\"derived\":%s,\"public\":%s,\"bits\":%d,\"h\":\"%s\"}",
           drv ? "true" : "false", pub ? "true" : "false", bits, hh);
  _server->send(200, "application/json", out);
}

/* POST /channel/del  (name) */
void WebTask::handleChannelDel() {
  if (!requireAuth()) return;
  if (!channelsAvailable()) return;
  char name[24];
  if (!getArg(*_server, "name", name, sizeof(name)) || name[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"naam ontbreekt\"}"); return;
  }
  int r = _acl->webChannelDel(name);
  _server->send(r == 1 ? 200 : 400, "application/json",
                r == 1 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"niet gevonden\"}");
}

/* POST /channel/toggle  (name, enabled=0|1) */
void WebTask::handleChannelToggle() {
  if (!requireAuth()) return;
  if (!channelsAvailable()) return;
  char name[24], en[4];
  if (!getArg(*_server, "name", name, sizeof(name)) || name[0] == 0) {
    _server->send(400, "application/json", "{\"ok\":false,\"error\":\"naam ontbreekt\"}"); return;
  }
  bool enabled = getArg(*_server, "enabled", en, sizeof(en)) && en[0] == '1';
  int r = _acl->webChannelToggle(name, enabled ? 1 : 0);
  _server->send(r == 1 ? 200 : 400, "application/json",
                r == 1 ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"niet gevonden\"}");
}

/* POST /mon/alarm  (idx | ch, am, rm [, sn] [, sev])  -- per-sensor alarmroute +
 * room-set + sensor-node-set + ernst. sev (0 hoog/1 midden/2 laag) is optioneel en
 * bepaalt de ernst-emoji vooraan de storings-DM; afwezig = ongewijzigd.
 *
 * DE IDENTIFICATIE. De MeshManager-server nam `idx` = de POSITIE in de mon[]-array
 * van /status.json. Die array begint met VIER vaste cellen (kanaal 1 'spanning',
 * netvoeding, batterij, wifi) die GEEN am/rm dragen; de reguliere monitors -- de
 * enige met am/rm -- staan er dus vanaf positie MON_FIXED_PREFIX (4) achter, in
 * slotvolgorde en alleen de gebruikte. We rekenen `idx` terug naar het kanaal via
 * exact diezelfde volgorde als appendMonitors.
 *
 * Positie is BROOS: hij verschuift als er een monitor bij/af gaat, en de eerste
 * vier zijn vaste bronnen zonder am/rm. Daarom accepteren we OOK het stabiele veld
 * `ch` (dat al in elke mon[]-cel staat als "ch"); geef je `ch` mee, dan wint dat.
 * Zie het eindrapport voor de aanbeveling aan de serverkant.
 *
 * GEEN tweede schrijfpad: we sturen 'sensor set mon.<ch>.alert' en '.rooms' langs
 * dezelfde CLI als de rest, en verifiëren daarna via de getters (definitief, geen
 * broze tekstuitslag-parsing). */
void WebTask::handleMonAlarm() {
  if (!requireAuth()) return;
  if (_mon == nullptr) {
    _server->send(503, "application/json",
        "{\"ok\":false,\"error\":\"sensorlaag niet gekoppeld\"}");
    return;
  }
  if (_acl == nullptr) {
    _server->send(503, "application/json", "{\"ok\":false,\"error\":\"meshlaag niet gekoppeld\"}");
    return;
  }

  const int am = getArgInt(*_server, "am", 0);
  const int rm = getArgInt(*_server, "rm", -1);
  if (am < 1 || am > 3) {
    _server->send(400, "application/json",
        "{\"ok\":false,\"error\":\"am: 1=dm, 2=room, 3=both\"}");
    return;
  }
  if (rm < 0) {
    _server->send(400, "application/json",
        "{\"ok\":false,\"error\":\"rm ontbreekt (room-bitmasker, 0 = geen room)\"}");
    return;
  }

  /* mon[] = [MON_FIXED_PREFIX vaste cellen] + [gebruikte reguliere monitors]. */
  const int MON_FIXED_PREFIX = 4;
  int slot = -1;
  char chbuf[8];
  if (getArg(*_server, "ch", chbuf, sizeof(chbuf)) && chbuf[0]) {
    int ch = atoi(chbuf);
    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) {
      if (_mon->monitorUsed(i) && (int)_mon->monitorChannel(i) == ch) { slot = i; break; }
    }
    if (slot < 0) {
      _server->send(404, "application/json",
          "{\"ok\":false,\"error\":\"geen (gebruikt) kanaal met dat ch\"}");
      return;
    }
  } else {
    int idx = getArgInt(*_server, "idx", -1);
    int rel = idx - MON_FIXED_PREFIX;
    if (idx < 0 || rel < 0) {
      _server->send(400, "application/json",
          "{\"ok\":false,\"error\":\"idx wijst op een vast kanaal (0-3) of ontbreekt; "
          "reguliere monitors beginnen op positie 4 -- of stuur 'ch'\"}");
      return;
    }
    int seen = 0;
    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) {
      if (!_mon->monitorUsed(i)) continue;
      if (seen == rel) { slot = i; break; }
      seen++;
    }
    if (slot < 0) {
      _server->send(404, "application/json",
          "{\"ok\":false,\"error\":\"geen monitor op die mon[]-positie\"}");
      return;
    }
  }

  const int ch = (int)_mon->monitorChannel(slot);

  /* rm-bitmasker -> komma-lijst van room-indexen ("0,2"), of "none" voor 0. */
  char rooms[48];
  int ro = 0;
  for (int b = 0; b < 16; b++) {
    if (rm & (1 << b)) {
      if (ro) rooms[ro++] = ',';
      ro += snprintf(rooms + ro, sizeof(rooms) - ro, "%d", b);
    }
  }
  if (ro == 0) strcpy(rooms, "none"); else rooms[ro] = 0;

  /* Optioneel: `sn` = sensor-node-bitmasker (welke virtuele sensor-nodes deze
   * sensor als telemetrie-kanaal tonen). Afwezig -> ongewijzigd. */
  const int sn = getArgInt(*_server, "sn", -1);
  char snlist[48];
  if (sn >= 0) {
    int so = 0;
    for (int b = 0; b < 16; b++) {
      if (sn & (1 << b)) {
        if (so) snlist[so++] = ',';
        so += snprintf(snlist + so, sizeof(snlist) - so, "%d", b);
      }
    }
    if (so == 0) strcpy(snlist, "none"); else snlist[so] = 0;
  }

  /* Optioneel: `sev` = ernst (0 hoog, 1 midden, 2 laag) voor de ernst-emoji vooraan
   * de storings-DM. Afwezig of -1 -> ongewijzigd. */
  const int sev = getArgInt(*_server, "sev", -1);
  if (sev > MON_SEV_LOW) {
    _server->send(400, "application/json",
        "{\"ok\":false,\"error\":\"sev: 0=hoog, 1=midden, 2=laag\"}");
    return;
  }

  /* am gaat als getal direct door (parseAlertMode accepteert 1/2/3). */
  snprintf(g_cmd, sizeof(g_cmd), "sensor set mon.%d.alert %d", ch, am);
  _acl->handleCommandWeb(0, g_cmd, g_reply);
  snprintf(g_cmd, sizeof(g_cmd), "sensor set mon.%d.rooms %s", ch, rooms);
  _acl->handleCommandWeb(0, g_cmd, g_reply);
  if (sn >= 0) {
    snprintf(g_cmd, sizeof(g_cmd), "sensor set mon.%d.snodes %s", ch, snlist);
    _acl->handleCommandWeb(0, g_cmd, g_reply);
  }
  if (sev >= 0) {
    snprintf(g_cmd, sizeof(g_cmd), "sensor set mon.%d.sev %d", ch, sev);
    _acl->handleCommandWeb(0, g_cmd, g_reply);
  }

  /* Definitieve controle via de getters -- niet de CLI-tekst parsen. */
  bool ok = ((int)_mon->monitorAlertMode(slot) == am) &&
            ((int)_mon->monitorRoomsMask(slot) == rm) &&
            (sn < 0 || (int)_mon->monitorSensorNodesMask(slot) == sn) &&
            (sev < 0 || (int)_mon->monitorSeverity(slot) == sev);
  if (!ok) {
    char msg[192];
    snprintf(msg, sizeof(msg),
        "{\"ok\":false,\"error\":\"node nam am/rm/sn niet over (am=%u rm=%u sn=%u)\",\"ch\":%d}",
        (unsigned)_mon->monitorAlertMode(slot), (unsigned)_mon->monitorRoomsMask(slot),
        (unsigned)_mon->monitorSensorNodesMask(slot), ch);
    _server->send(500, "application/json", msg);
    return;
  }
  char msg[96];
  snprintf(msg, sizeof(msg), "{\"ok\":true,\"ch\":%d,\"am\":%d,\"rm\":%d,\"sn\":%d}",
           ch, am, rm, (int)_mon->monitorSensorNodesMask(slot));
  _server->send(200, "application/json", msg);
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
  mesh::Utils::toHex(pubkey, _acl->getSelfPubKey(), PUB_KEY_SIZE);

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

  /* De web-GEBRUIKER mag getoond worden, het wachtwoord NOOIT -- net als bij de
   * wifi-instelling. Ontsnappen is nodig want de gebruiker mag (via /web/cred) elk
   * teken bevatten. g_web_custom is de vlag "eigen credential gezet": staat hij op
   * false, dan draait deze node nog op de gebakken, vlootbrede login. */
  char webuser_esc[WEB_USER_LEN * 6 + 1];
  jsonEscape(g_web_user, webuser_esc, sizeof(webuser_esc));

  /* TIJD: NTP-server + TZ (voor het formulier) en de sync-status (voor de weergave).
   * De lokale tijd wordt uit de RTC (UTC) omgezet via de ingestelde TZ. De
   * escape-buffers zijn STATIC (niet op de loopTask-stapel): handleCfgJson draait
   * op één web-verzoek tegelijk, dus dat is veilig -- en het houdt het frame klein
   * (les van v2.2.0). */
  static char ntp_esc[sizeof(g_ntp_shown) * 6 + 1], tz_esc[sizeof(g_tz_shown) * 6 + 1];
  jsonEscape(g_ntp_shown, ntp_esc, sizeof(ntp_esc));
  jsonEscape(g_tz_shown, tz_esc, sizeof(tz_esc));
  static char tlocal[40]; fmtLocalDateTime(_acl->nowSecs(), tlocal, sizeof(tlocal));
  static char tlocal_esc[80]; jsonEscape(tlocal, tlocal_esc, sizeof(tlocal_esc));
  static char tsyncmsg_esc[64 * 6 + 1];
  jsonEscape(_wifi ? _wifi->lastSyncMsg() : "", tsyncmsg_esc, sizeof(tsyncmsg_esc));
  const int  t_synced = (_wifi && _wifi->timeSynced()) ? 1 : 0;
  const unsigned long t_age = _wifi ? _wifi->secsSinceSync() : 0;

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
      "\"webuser\":\"%s\",\"webcustom\":%d,"
      "\"mon_used\":%d,\"mon_max\":%u,\"ch_first\":%u,\"ch_last\":%u,"
      "\"ch_ever\":%d,\"ch_free\":%d,"
      "\"ntp\":\"%s\",\"tz\":\"%s\",\"tlocal\":\"%s\",\"tsync\":%d,"
      "\"tsyncmsg\":\"%s\",\"tsyncage\":%lu,"
      "\"baked\":{\"freq\":\"%s\",\"bw\":\"%s\",\"sf\":%u,\"cr\":%u}}",
      g_cfg_name, g_cfg_owner, pubkey, _acl->getRoleName(),
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
      webuser_esc, g_web_custom ? 1 : 0,
      mon_used, (unsigned)MonitorSensors::MAX_MONITORS,
      (unsigned)MonitorSensors::CH_MONITOR_FIRST,
      (unsigned)MonitorSensors::CH_MONITOR_LAST,
      ever, (int)MonitorSensors::MAX_MONITORS - ever,
      ntp_esc, tz_esc, tlocal_esc, t_synced, tsyncmsg_esc, t_age,
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

  /* ------------------------------ ad-hoc ping ----------------------------- */
  /* 'ping <adres> [n]' is GEEN mesh-CLI-opdracht (die zou "Unknown command"
   * geven), dus hier onderscheppen we hem en sturen hem naar de ad-hoc
   * pingmachine van de sensorlaag -- dezelfde die de DM-variant gebruikt, dus
   * geen tweede weg. De webconsole is synchroon en mag niet blokkeren: we
   * antwoorden "gestart" en de uitslag verschijnt daarna in /status.json, precies
   * zoals de pagina hem toont. Wie geen sensorlaag heeft, valt door naar de
   * gewone afhandeling (die dan "Unknown command" geeft). */
  /* OOK 'icmp' als draad-alias, en dit is GEMETEN noodzaak en geen sier: een
   * IPS op het netwerk van de gebruiker (UDM Pro, Intrusion Prevention) reset
   * elke HTTP-verbinding waarvan de body exact kleine-letters "ping" plus
   * witruimte bevat -- de klassieke command-injection-signatuur. Getest op
   * 20-8-2026: 'pinx 8.8.8.8' en 'PING 8.8.8.8' bereikten de node (200),
   * 'ping 8.8.8.8' werd op het pad gedood; vanaf een ander netwerksegment kwam
   * dezelfde opdracht wel aan. De console-JS herschrijft een getypte ping
   * daarom naar icmp voordat hij de draad op gaat; beide woorden zijn 4 tekens,
   * dus cmd+4 blijft kloppen. Over de DM (LoRa, versleuteld) blijft het gewoon
   * 'ping' -- daar kijkt geen middlebox mee. */
  if ((cmdIs(cmd, "ping ") || strcmp(cmd, "ping") == 0 ||
       cmdIs(cmd, "icmp ") || strcmp(cmd, "icmp") == 0 ||
       cmdIs(cmd, "Ping ") || cmdIs(cmd, "PING ")) && _mon != nullptr) {
    const char* arg = cmd + 4;
    while (*arg == ' ') arg++;
    char host[MON_HOST_LEN + 4]; host[0] = 0;
    unsigned long n = 0;
    /* "<adres> [n]" met sscanf-vrije, eenvoudige splitsing: geen String, geen
     * allocatie. */
    {
      int hi = 0;
      while (arg[hi] && arg[hi] != ' ' && hi < (int)sizeof(host) - 1) { host[hi] = arg[hi]; hi++; }
      host[hi] = 0;
      const char* np = arg + hi;
      while (*np == ' ') np++;
      if (*np) n = strtoul(np, nullptr, 10);
    }
    if (host[0] == 0) {
      _server->send(400, "text/plain", "ping <adres> [n]\n");
      return;
    }
    if (!_mon->isWifiOnline()) {
      _server->send(200, "text/plain", "geen wifi, niet gepingd\n");
      return;
    }
    MonitorSensors::SimResult r = _mon->startAdhocPing(host, (uint8_t)n);
    char msg[160];
    if (r == MonitorSensors::SIM_ERR_BUSY) {
      snprintf(msg, sizeof(msg), "bezig met %s, probeer zo opnieuw\n", _mon->adhocHost());
      _server->send(200, "text/plain", msg);
    } else if (r != MonitorSensors::SIM_OK) {
      _server->send(200, "text/plain",
          "adres: 1-40 tekens uit a-z A-Z 0-9 . - _ (geen IPv6)\n");
    } else {
      snprintf(msg, sizeof(msg),
          "ping naar %s gestart; de uitslag verschijnt zo bij 'bewaking' "
          "(onder de tegels). Niet-blokkerend, dus deze console wacht er niet "
          "op.\n", host);
      _server->send(200, "text/plain", msg);
    }
    return;
  }

  /* --------------------- de eigen web-login via de console ---------------- */
  /* 'sensor set web.user <naam>' en 'sensor set web.pass <ww>' zetten de EIGEN
   * web-login van deze node (/web.cfg). We onderscheppen ze hier -- net als 'ping'
   * -- en sturen ze NIET door: de sleutel 'web.*' is van ons en leeft niet in
   * MonitorSensors. 'sensor get web.user' toont de gebruiker en de vlag; het
   * wachtwoord wordt NOOIT teruggelezen, net als de wifi-pass.
   *
   * LET OP -- dit is de weg langs DEZE (web)console. De seriële console en een DM
   * gaan rechtstreeks naar SensorMesh/MonitorSensors, en daar is 'web.*' een
   * onbekende sleutel; wie het ook daar wil, voegt één tak toe in
   * MonitorSensors::setSettingValue (buiten deze opdracht gehouden). De route
   * POST /web/cred en het formulier op het beheertabblad dekken de gevraagde twee
   * zetwegen volledig. Deze onderschepping raakt CommonCLI's opdrachtbuffer niet
   * eens, dus de ~119-tekengrens speelt hier niet; we toetsen zelf op de
   * afgesproken maten (user 32, pass 64). */
  if (cmdIs(cmd, "sensor set web.user ") || cmdIs(cmd, "sensor set web.pass ")) {
    bool is_user = cmdIs(cmd, "sensor set web.user ");
    const char* val = cmd + 20;   /* "sensor set web.user "/"...pass " zijn beide 20 tekens */
    while (*val == ' ') val++;
    if (val[0] == 0) {
      _server->send(400, "text/plain", is_user ? "web.user <naam>\n" : "web.pass <ww>\n");
      return;
    }
    size_t maxlen = is_user ? (size_t)(WEB_USER_LEN - 1) : (size_t)(WEB_PASS_LEN - 1);
    if (strlen(val) > maxlen) {
      snprintf(g_reply, sizeof(g_reply),
          "geweigerd: web.%s hoogstens %u tekens\n",
          is_user ? "user" : "pass", (unsigned)maxlen);
      _server->send(400, "text/plain", g_reply);
      return;
    }

    /* Het niet-gewijzigde veld houdt zijn huidige waarde. Zet iemand alleen
     * web.pass terwijl de user nog de gebakken standaard is, dan bewaren we die
     * user -- maar nooit een LEGE pass (dat weigert saveWebCred sowieso). */
    char nu[WEB_USER_LEN], np[WEB_PASS_LEN];
    strlcpy(nu, is_user ? val : g_web_user, sizeof(nu));
    strlcpy(np, is_user ? g_web_pass : val, sizeof(np));
    if (np[0] == 0) {
      _server->send(400, "text/plain",
          "er is nog geen wachtwoord; zet eerst 'sensor set web.pass <ww>'\n");
      return;
    }
    if (!MonitorStore::saveWebCred(SPIFFS, nu, np)) {
      _server->send(500, "text/plain", "opslaan mislukt\n");
      return;
    }
    strlcpy(g_web_user, nu, sizeof(g_web_user));
    strlcpy(g_web_pass, np, sizeof(g_web_pass));
    g_web_custom = true;
    snprintf(g_reply, sizeof(g_reply),
        "ok web.%s gezet; geldt bij het volgende verzoek\n", is_user ? "user" : "pass");
    _server->send(200, "text/plain", g_reply);
    return;
  }
  if (strcmp(cmd, "sensor get web.user") == 0) {
    snprintf(g_reply, sizeof(g_reply),
        "web.user=%s (eigen credential: %s)\n", g_web_user,
        g_web_custom ? "ja" : "nee, nog de gebakken standaard");
    _server->send(200, "text/plain", g_reply);
    return;
  }
  if (strcmp(cmd, "sensor get web.pass") == 0) {
    _server->send(200, "text/plain",
        "verborgen -- het wachtwoord wordt nooit teruggelezen\n");
    return;
  }

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
  _acl->handleCommandWeb(0, cmd, g_reply);

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
  _acl->handleCommandWeb(0, cmd, g_reply);
}

/* POST /web/cred   user=<nieuw>&pass=<nieuw>   (form-urlencoded)
 *
 * DE ROTATIEROUTE -- het CONTRACT met de statsserver, vastgespijkerd:
 *
 *   - body is form-urlencoded (net als elke andere POST op deze server), met de
 *     velden 'user' en 'pass';
 *   - ontbreekt of leeg één van beide  -> 400. Een LEGE pass mag nooit: dat zou de
 *     node openzetten;
 *   - te lang (user > 32, pass > 64)   -> 400. Afkappen zou een ander wachtwoord
 *     bewaren dan verstuurd is;
 *   - bij succes 200 met body {"ok":1}. De server bewaart daarna de nieuwe
 *     credential aan zijn kant.
 *
 * De server roept dit aan MET de HUIDIGE Basic-auth (requireAuth hieronder toetst
 * nog tegen de oude waarde). De NIEUWE credential geldt pas bij het VOLGENDE
 * verzoek: g_web_user/g_web_pass worden hier pas NA de auth en NA het opstellen van
 * het antwoord gezet, dus de lopende sessie wordt niet mid-request omgegooid.
 *
 * VEILIGHEIDSNOOT, en die hoort hier eerlijk te staan: dit is Basic-auth over
 * onversleuteld HTTP. Ook een per-node credential gaat leesbaar (base64) over het
 * LAN bij elk verzoek. Dit verkleint de schade van één lek -- niet langer de hele
 * vloot, alleen deze node -- maar het VERVANGT GEEN TLS of beheer-VLAN. Roteer over
 * een net waar niemand meeleest, en zet deze node niet open naar buiten.
 */
void WebTask::handleWebCred() {
  if (!requireAuth()) return;

  char user[WEB_USER_LEN], pass[WEB_PASS_LEN];

  int ur = getArgStrict(*_server, "user", user, sizeof(user));
  if (ur == -2) {
    _server->send(400, "text/plain", "user te lang (hoogstens 32 tekens)\n");
    return;
  }
  if (ur <= 0) {   /* -1 afwezig of 0 leeg */
    _server->send(400, "text/plain", "user ontbreekt of is leeg\n");
    return;
  }

  int pr = getArgStrict(*_server, "pass", pass, sizeof(pass));
  if (pr == -2) {
    _server->send(400, "text/plain", "pass te lang (hoogstens 64 tekens)\n");
    return;
  }
  if (pr <= 0) {
    /* Een lege pass zou de node openzetten -- weigeren, ook al vraagt de server erom. */
    _server->send(400, "text/plain", "pass ontbreekt of is leeg\n");
    return;
  }

  if (!MonitorStore::saveWebCred(SPIFFS, user, pass)) {
    _server->send(500, "text/plain", "opslaan mislukt\n");
    return;
  }

  /* Pas NU laten gelden: het antwoord hieronder gaat nog over de oude sessie. */
  strlcpy(g_web_user, user, sizeof(g_web_user));
  strlcpy(g_web_pass, pass, sizeof(g_web_pass));
  g_web_custom = true;

  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", "{\"ok\":1}");
}

/* ============================ de sessie-login ============================= */

/* De inlogpagina. Zelfstandig (geen CDN, geen externe fonts -- de node heeft geen
 * internet), en in dezelfde vormtaal als de hoofdpagina: dezelfde MeshManager-
 * kleurtokens, dezelfde drie themastanden (systeem/licht/donker), dezelfde mono-
 * en sans-stapels. Het formulier POST't gewoon (geen fetch), zodat inloggen ook
 * zonder JavaScript werkt; de 302 van de server regelt de rest. Een klein stukje JS
 * toont alleen de foutmelding uit de querystring (?bad of ?wait=N) -- de server
 * schrijft geen tekst in deze pagina, dus hij blijft een statisch PROGMEM-document.
 *
 * De EERLIJKE veiligheidsnoot staat op de pagina zelf: cookie + login over
 * onversleuteld HTTP is geen TLS. */
static const char LOGIN_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="nl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>MeshUptime &middot; inloggen</title>
<script>
try{var t=localStorage.getItem("mu-theme");
if(t=="light"||t=="dark"){document.documentElement.setAttribute("data-theme",t)}}
catch(e){}
</script>
<style>
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
body{font-family:var(--sans);font-size:15px;line-height:1.45;margin:0;
min-height:100vh;display:flex;align-items:center;justify-content:center;
padding:1.2rem;color:var(--text);background:var(--bg)}
.box{width:100%;max-width:22rem}
h1{font-size:1.15rem;letter-spacing:.02em;margin:0 0 .15rem}
h1 span{font-family:var(--mono);font-size:.66rem;text-transform:uppercase;
letter-spacing:.16em;color:var(--muted);display:block;margin-top:.25rem}
.card{background:linear-gradient(180deg,rgba(255,255,255,.025),transparent 55%),
var(--card);border:1px solid var(--border);border-radius:10px;padding:1.1rem;
margin-top:1rem}
label{display:block;font-family:var(--mono);font-size:.64rem;text-transform:uppercase;
letter-spacing:.12em;color:var(--muted);margin:.7rem 0 0}
label:first-child{margin-top:0}
input{width:100%;margin-top:.3rem;padding:.5rem .6rem;font-family:var(--mono);
font-size:.95rem;color:var(--text);background:var(--bg);border:1px solid var(--border);
border-radius:6px}
input:focus{outline:none;border-color:var(--cyan)}
button{margin-top:1rem;width:100%;padding:.55rem 1.1rem;font-family:var(--mono);
font-size:.72rem;text-transform:uppercase;letter-spacing:.12em;cursor:pointer;
color:var(--bg);background:var(--accent);border:1px solid var(--accent);border-radius:6px}
button:hover{filter:brightness(1.1)}
#err{font-family:var(--mono);font-size:.8rem;min-height:1.2rem;margin-top:.7rem;
color:var(--red)}
.note{font-size:.8rem;color:var(--muted);margin:1rem 0 0}
.note b{color:var(--text);font-weight:600}
.thm{margin-top:.9rem;text-align:right}
.thm button{width:auto;margin:0;padding:.3rem .7rem;background:transparent;
color:var(--muted);border-color:var(--border);text-transform:none;letter-spacing:0}
.thm button:hover{color:var(--cyan);border-color:var(--cyan);filter:none}
</style></head><body>
<div class="box">
<h1>MeshUptime<span>bewaking &middot; inloggen</span></h1>
<div class="card">
<form method="post" action="/login" autocomplete="on">
<label>Gebruiker<input name="user" autocomplete="username" autofocus
spellcheck="false"></label>
<label>Wachtwoord<input name="pass" type="password" autocomplete="current-password"></label>
<button type="submit">inloggen</button>
</form>
<div id="err"></div>
<p class="note"><b>Dit is de lokale terugval-beheertoegang van deze node.</b> Het
echte beheer loopt over LoRa-DM's; dit web is er voor als je erbij moet zonder mesh.
Na inloggen krijg je een sessiecookie (12&nbsp;uur geldig); afmelden kan op de
pagina.</p>
<p class="note"><b>Eerlijk over de grens:</b> deze login en de cookie gaan over
<b>onversleuteld HTTP</b>. Wie het LAN kan meelezen, leest ze mee &mdash; dit
vervangt de inlogpopup, niet TLS. Zet deze node niet open naar buiten; gebruik een
VPN of een TLS-proxy als het van buiten moet.</p>
<div class="thm"><button id="thm" type="button">thema</button></div>
</div>
</div>
<script>
/* De foutmelding uit de querystring (?bad of ?wait=N). De server schrijft niets in
   deze pagina; dit houdt hem statisch en injectievrij. */
(function(){var q=location.search,e=document.getElementById("err");
var m=q.match(/[?&]wait=(\d+)/);
if(m){e.textContent="Te veel mislukte pogingen. Wacht "+m[1]+" s en probeer opnieuw."}
else if(/[?&]bad/.test(q)){e.textContent="Gebruiker of wachtwoord onjuist."}})();
/* Zelfde themaknop-logica als de hoofdpagina, in het klein. */
var THEMES=["system","light","dark"],THNAME={system:"systeem",light:"licht",dark:"donker"};
function thGet(){try{var t=localStorage.getItem("mu-theme");
return(t=="light"||t=="dark")?t:"system"}catch(e){return"system"}}
function thApply(t){try{if(t=="system"){localStorage.removeItem("mu-theme");
document.documentElement.removeAttribute("data-theme")}
else{localStorage.setItem("mu-theme",t);
document.documentElement.setAttribute("data-theme",t)}}catch(e){}
document.getElementById("thm").textContent="thema: "+THNAME[t]}
document.getElementById("thm").onclick=function(){
var i=THEMES.indexOf(thGet());thApply(THEMES[(i+1)%THEMES.length])};
thApply(thGet());
</script>
</body></html>)HTML";

/* De groeiende wachttijd tussen mislukte inlogpogingen. Pas vanaf de derde fout
 * (twee vertypen mag zonder straf), daarna 5 s per extra fout, tot ten hoogste
 * 300 s. Niet-blokkerend: dit is de wachttijd die de POST-handler AFDWINGT via een
 * tijdstip, niet een delay(). */
static unsigned long webBruteWaitMs(uint8_t fails) {
  if (fails < 3) return 0;
  unsigned long secs = (unsigned long)(fails - 2) * 5UL;
  if (secs > 300) secs = 300;
  return secs * 1000UL;
}

void WebTask::handleLogin() {
  /* Al ingelogd? Dan niet nog eens het formulier tonen, maar door naar de pagina. */
  if (authOk()) {
    _server->sendHeader("Location", "/");
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(302, "text/plain", "");
    return;
  }
  _server->sendHeader("Cache-Control", "no-store");
  _server->send_P(200, "text/html", LOGIN_HTML);
}

void WebTask::handleLoginPost() {
  /* De brute-force-rem: staat de klok nog vóór het vrijgavetijdstip, dan weigeren we
   * zonder zelfs maar te toetsen, met de resterende wachttijd in de melding. */
  unsigned long now = millis();
  if (!webPast(g_login_next_ms)) {
    unsigned long left = (g_login_next_ms - now + 999) / 1000;
    char loc[48];
    snprintf(loc, sizeof(loc), "/login?wait=%lu", left);
    _server->sendHeader("Location", loc);
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(302, "text/plain", "");
    return;
  }

  /* De ingevoerde login lezen met de VEILIGE lezer (getArg -- geen bengelende
   * pointer). Afkappen is hier onschuldig: een afgekapt wachtwoord is gewoon een
   * verkeerd wachtwoord en faalt de toets. */
  char user[WEB_USER_LEN], pass[WEB_PASS_LEN];
  if (!getArg(*_server, "user", user, sizeof(user))) user[0] = 0;
  if (!getArg(*_server, "pass", pass, sizeof(pass))) pass[0] = 0;

  bool ok = user[0] && pass[0] &&
            webCtEqual(user, g_web_user) && webCtEqual(pass, g_web_pass);

  if (ok) {
    g_login_fails = 0;
    g_login_next_ms = millis();
    char tok[WEB_SESS_HEX + 1];
    webSessionCreate(tok);
    char cookie[128];
    /* Geen 'Secure' (zie de noot bij de sessietabel: dat zou over HTTP elke cookie
     * onderdrukken). HttpOnly + SameSite=Strict + Path=/ + Max-Age. */
    snprintf(cookie, sizeof(cookie),
             "%s=%s; Max-Age=%lu; Path=/; HttpOnly; SameSite=Strict",
             WEB_COOKIE_NAME, tok, (unsigned long)(WEB_SESS_TTL_MS / 1000UL));
    _server->sendHeader("Set-Cookie", cookie);
    _server->sendHeader("Location", "/");
    _server->sendHeader("Cache-Control", "no-store");
    _server->send(302, "text/plain", "");
    return;
  }

  /* Mislukt: teller op, groeiende wachttijd zetten, terug naar het formulier met een
   * nette melding. */
  if (g_login_fails < 255) g_login_fails++;
  g_login_next_ms = millis() + webBruteWaitMs(g_login_fails);
  _server->sendHeader("Location", "/login?bad=1");
  _server->sendHeader("Cache-Control", "no-store");
  _server->send(302, "text/plain", "");
}

void WebTask::handleLogout() {
  /* Geen auth nodig: afmelden mag altijd, en meer dan de eigen sessie wissen kan het
   * niet. Beide kanten: het vakje in de tabel en de cookie in de browser. */
  char tok[WEB_SESS_HEX + 1];
  if (webCookieToken(*_server, tok, sizeof(tok))) webSessionDestroy(tok);
  _server->sendHeader("Set-Cookie",
      WEB_COOKIE_NAME "=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict");
  _server->sendHeader("Location", "/login");
  _server->sendHeader("Cache-Control", "no-store");
  _server->send(302, "text/plain", "");
}

/* POST /web/cred/reset -- terug naar de GEBAKKEN standaard (admin/meshcore).
 *
 * HOE DE EIGENAAR admin/meshcore TERUGKRIJGT:
 *   SPIFFS overleeft een flash van de firmware. De node staat nu op een geroteerde
 *   login (mm-...) in /web.cfg, en die blijft dus staan na het flashen van deze
 *   firmware. Deze route verwijdert /web.cfg; daardoor valt begin() (en elke
 *   requireAuth erna) terug op de gebakken WEB_USER/WEB_PASS = admin/meshcore.
 *
 * Roep hem NA het flashen aan MET DE HUIDIGE (geroteerde) login, bv.:
 *   curl -u mm-USER:mm-PASS -X POST http://<node-ip>/web/cred/reset
 * Antwoord {"ok":1}; daarna is de login admin/meshcore. (Ook via de eigen sessie te
 * doen: log in met de huidige login en de knop 'terug naar standaard' onder
 * Web-login op het node-tabblad doet hetzelfde.)
 *
 * Ben je de huidige login kwijt, dan is de terugval een volledige flash-wis
 * (`pio run -e meshuptime -t erase` en opnieuw flashen): dat wist heel SPIFFS, dus
 * ook /web.cfg -- maar ook de monitors en de wifi-instelling. Daarom is deze route
 * de schone weg en de erase de noodrem.
 *
 * De lopende sessies laten we staan: de credential verandert, maar wie nu is
 * ingelogd is dezelfde beheerder en hoeft er niet uit gegooid te worden. */
void WebTask::handleWebCredReset() {
  if (!requireAuth()) return;

  if (!MonitorStore::clearWebCred(SPIFFS)) {
    _server->send(500, "text/plain", "kon /web.cfg niet verwijderen\n");
    return;
  }
  strlcpy(g_web_user, WEB_USER, sizeof(g_web_user));
  strlcpy(g_web_pass, WEB_PASS, sizeof(g_web_pass));
  g_web_custom = false;

  _server->sendHeader("Cache-Control", "no-store");
  _server->send(200, "application/json", "{\"ok\":1,\"reset\":1}");
}
