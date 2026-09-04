#include "PushTask.h"
#include "WifiTask.h"

/* Dezelfde includes en dezelfde redenen als de ping-machine in
 * MonitorSensors.cpp: dns_gethostbyname geeft zijn antwoord via een callback en
 * mag alleen uit de lwIP-taak komen (tcpip_try_callback zet hem daar neer
 * zonder te wachten). De sockets zijn de gewone lwIP-BSD-laag -- WiFiClient
 * gebruikt onderwater exact dezelfde -- maar dan met O_NONBLOCK, zodat geen
 * enkele aanroep op het netwerk wacht. */
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "lwip/ip_addr.h"
#include "lwip/inet.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <esp_system.h>   /* esp_random() voor het boot-nummer */

#include <helpers/TxtDataHelpers.h>   /* StrHelper::strncpy */

/* ================== werkgeheugen van de lopende poging ==================
 *
 * In bestandsbereik en niet in de klasse, om dezelfde reden als s_ping in
 * MonitorSensors.cpp: de DNS-velden worden door de lwIP-taak geschreven via een
 * C-callback, en er is precies één PushTask met precies één poging tegelijk.
 * De buffers staan hier ook: één verzoek van ~1,4 kB als klasse-lid zou elke
 * houder van een MonitorSensors-header meeslepen, en statisch is statisch. */

static struct {
  volatile uint8_t  state;   /* 0 bezig, 1 gelukt, 2 mislukt */
  volatile uint32_t addr;    /* IPv4 in netwerk-volgorde */
  char host[MON_PUSH_URL_LEN];
} s_pdns = { 0, 0, {0} };

/* ---- callbacks; deze lopen NIET in de hoofdtaak. Alleen een waarde wegzetten,
 * verder niets -- zelfde huisregel als bij de ping-machine. Een callback die NA
 * onze deadline nog binnenkomt schrijft in velden waar niemand meer op kijkt;
 * dat is onschadelijk, want elke nieuwe opzoeking zet state eerst op 0. ---- */

static void push_dns_found(const char* name, const ip_addr_t* ipaddr, void* arg) {
  if (ipaddr != NULL && IP_IS_V4(ipaddr)) {
    s_pdns.addr  = ip4_addr_get_u32(ip_2_ip4(ipaddr));
    s_pdns.state = 1;      /* als laatste: hierop kijkt de lezer */
  } else {
    s_pdns.state = 2;      /* niet gevonden, of alleen IPv6 */
  }
}

/* Loopt in de lwIP-taak, neergezet door tcpip_try_callback(). */
static void push_do_resolve(void* ctx) {
  ip_addr_t addr;
  err_t e = dns_gethostbyname(s_pdns.host, &addr, push_dns_found, NULL);
  if (e == ERR_OK) {                 /* stond al in de cache van lwIP */
    push_dns_found(s_pdns.host, &addr, NULL);
  } else if (e != ERR_INPROGRESS) {
    s_pdns.state = 2;
  }
}

/* De ontlede url van de lopende poging. Elke poging ontleedt opnieuw, want de
 * instelling kan tussendoor veranderd zijn -- de cache van het opgeloste adres
 * (_addr_host in de klasse) vangt de gewone herhaling af. */
static char     s_host[MON_PUSH_URL_LEN];
/* prefix + het langste contract-pad. Dat is sinds v2.6 niet meer
 * "/api/sensorpush" (15) maar "/api/v1/repeater_settings" (25); +32 houdt daar
 * marge boven. Te krap zou snprintf het pad stil AFKAPPEN en dan post de node
 * naar een endpoint dat niet bestaat -- een 404 waarvan de oorzaak nergens te
 * zien is. */
static char     s_path[MON_PUSH_URL_LEN + 32];
static uint16_t s_port = 80;

/* Het volledige verzoek (koppen + body) en het antwoord. Nagerekend: de koppen
 * zijn hoogstens ~350 byte (pad tot ~113, token tot 40), de body is begrensd op
 * PUSH_BODY_MAX. Het antwoord van dit contract is {"ok":1,"ack":[...]} plus
 * koppen; 512 is daar een veelvoud van, en wat er niet in past wordt geteld
 * maar niet bewaard -- de statusregel en de ack-lijst staan vooraan. */
#define PUSH_BODY_MAX 1024
static char   s_req[PUSH_BODY_MAX + 416];
static size_t s_req_len = 0;
static size_t s_req_off = 0;      /* hoeveel er al de socket in is */
/* Het antwoord. 512 volstond voor de pushes ({"ok":1,"ack":[...]}), maar de
 * POLL-GET van /api/v1/commands geeft de HELE opdrachtwachtrij terug -- tot 40
 * parameters per repeater -- en die moet in zijn geheel gelezen worden om te
 * ontleden. 2560 dekt een sweep voor een handvol repeaters ruim; wat er toch
 * niet in past wordt geteld/gemeld (de Poller ziet aan een niet-afgesloten JSON
 * dat er iets ontbrak) i.p.v. stil verminkt. */
static char   s_resp[2560];
static size_t s_resp_len = 0;

/* Begrensd aanplakken. Geeft false zodra het niet meer paste; de aanroeper
 * beslist dan zelf of dat een fout is (koppen) of een natuurlijke grens (de
 * zoveelste gebeurtenis). */
static bool appendf(char* buf, size_t cap, size_t& len, const char* fmt, ...) {
  if (len >= cap) return false;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + len, cap - len, fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= cap - len) return false;
  len += (size_t)n;
  return true;
}

/* Een tekst JSON-veilig maken. Onze alertteksten komen uit eigen snprintf's en
 * bevatten geen aanhalingstekens, maar een zeef die op de goede wil van de
 * bron vertrouwt is geen zeef. Stuurcodes worden een spatie: het zijn er nul
 * in de praktijk en \u-ontsnappingen zijn de bytes niet waard. */
static bool appendJsonText(char* buf, size_t cap, size_t& len, const char* s) {
  for (const char* p = s; *p; p++) {
    char c = *p;
    if (c == '"' || c == '\\') {
      if (len + 2 >= cap) return false;
      buf[len++] = '\\';
      buf[len++] = c;
    } else {
      if ((unsigned char)c < 0x20) c = ' ';
      if (len + 1 >= cap) return false;
      buf[len++] = c;
    }
  }
  buf[len] = 0;
  return true;
}

void PushTask::begin(WifiTask* wifi, MonitorSensors* sensors, const uint8_t* pub_key) {
  _wifi    = wifi;
  _sensors = sensors;

  /* De node-naam uit het contract: de eerste 6 bytes van de publieke sleutel
   * als 12 hex kleine letters -- dezelfde prefix waarmee de server de node al
   * kent van de IP-poll. */
  for (int i = 0; i < 6; i++) {
    snprintf(&_node_id[i * 2], 3, "%02x", (unsigned)pub_key[i]);
  }

  /* Willekeurig per opstart, zodat de server een herstart ziet: een nieuwe
   * boot met seq terug op nul is anders niet te onderscheiden van een node
   * die heel lang niets te melden had. */
  _boot = (uint32_t)esp_random();

  _next_hb  = 0;   /* de eerste push gaat zodra er wifi is: die meldt de boot */
  _retry_at = 0;
}

bool PushTask::enabled() const {
  return _sensors != NULL && _sensors->pushUrl()[0] != 0;
}

uint32_t PushTask::lastOkAgeSecs() const {
  return _last_ok ? (millis() - _last_ok) / 1000 : 0;
}

void PushTask::onMonitorEvent(const MonitorEvent& ev) {
  /* Push uit = niets bewaren. Dit is geen verlies maar een stand: wie geen url
   * zet, heeft geen server om iets aan kwijt te raken. */
  if (!enabled()) return;

  if (_ring_count >= PUSH_RING_SIZE) {
    /* Vol: de OUDSTE valt eruit en dat wordt GETELD. Stil verlies is de fout
     * die dit hele project bestrijdt; de teller staat in de status. Zat de
     * weggevallen plaats in de lopende POST, dan telt hij daar niet meer mee --
     * anders zou het opruimen na de 200 een niet-verzonden gebeurtenis
     * weggooien. */
    _ring_tail = (uint8_t)((_ring_tail + 1) % PUSH_RING_SIZE);
    _ring_count--;
    if (_inflight > 0) _inflight--;
    _lost++;
    MESH_DEBUG_PRINTLN("PushTask: ring vol, oudste gebeurtenis vervallen (totaal verloren: %lu)",
                       (unsigned long)_lost);
  }
  _ring[(_ring_tail + _ring_count) % PUSH_RING_SIZE] = ev;
  _ring_count++;
}

/* v2.5.1 -- een companion-locatie/val in de eigen ring zetten. Zelfde overloop-
 * regel als de sensorring: vol -> de OUDSTE valt eruit en wordt geteld (_lost),
 * niet stil. dueNow() wordt hierdoor waar en de loop() pikt het op; hetzelfde
 * niet-blokkerende pad, alleen naar /api/companion. */
void PushTask::queueCompanion(const uint8_t* pub_key, bool has_loc, float lat, float lon,
                              uint32_t seen, uint32_t fall_ts, uint8_t fall_kind, int16_t batt) {
  if (!enabled() || pub_key == NULL) return;

  if (_cring_count >= COMP_RING_SIZE) {
    _cring_tail = (uint8_t)((_cring_tail + 1) % COMP_RING_SIZE);
    _cring_count--;
    if (_comp_inflight > 0) _comp_inflight--;
    _lost++;
    MESH_DEBUG_PRINTLN("PushTask: companion-ring vol, oudste vervallen (verloren: %lu)",
                       (unsigned long)_lost);
  }
  CompanionPush& c = _cring[(_cring_tail + _cring_count) % COMP_RING_SIZE];
  memcpy(c.pub_key, pub_key, PUB_KEY_SIZE);
  c.has_loc   = has_loc;
  c.lat       = lat;
  c.lon       = lon;
  c.seen      = seen;
  c.fall_ts   = fall_ts;
  c.fall_kind = fall_kind;
  c.batt      = batt;
  _cring_count++;
}

/* Het antwoord van een remote admin-CLI-opdracht in de eigen ring zetten. Zelfde
 * overloopregel als de andere twee ringen: vol -> de OUDSTE valt eruit en wordt
 * GETELD (_lost), nooit stil. In de praktijk kan dat hier nauwelijks gebeuren --
 * RepeaterCli levert er hoogstens een per opdracht en doet er een tegelijk. */
void PushTask::queueRepeaterSetting(const char* pubkey_hex12, const char* param,
                                    const char* value) {
  /* value MAG nullptr zijn (= "gevraagd, geen antwoord"); node en param niet. */
  if (!enabled() || pubkey_hex12 == NULL || param == NULL) return;
  if (pubkey_hex12[0] == 0 || param[0] == 0) return;

  if (_rring_count >= RCLI_RING_SIZE) {
    _rring_tail = (uint8_t)((_rring_tail + 1) % RCLI_RING_SIZE);
    _rring_count--;
    if (_rcli_inflight > 0) _rcli_inflight--;
    _lost++;
    MESH_DEBUG_PRINTLN("PushTask: repeater-cli-ring vol, oudste vervallen (verloren: %lu)",
                       (unsigned long)_lost);
  }
  RepCliPush& r = _rring[(_rring_tail + _rring_count) % RCLI_RING_SIZE];
  StrHelper::strncpy(r.node,  pubkey_hex12, sizeof(r.node));
  StrHelper::strncpy(r.param, param,        sizeof(r.param));
  /* value == nullptr -> JSON null. "" is een geldig (leeg) antwoord, null de
   * afwezigheid ervan; de server behandelt die twee anders. */
  r.has_value = (value != nullptr);
  StrHelper::strncpy(r.value, value ? value : "", sizeof(r.value));
  _rring_count++;
}

/* De MeshManager-opdrachtwachtrij pollen: alleen een vlag zetten. De GET zelf
 * draait in loop() zodra er geen dringender push voorligt; er is er hoogstens EEN
 * tegelijk. */
bool PushTask::requestPoll(PollFn cb, void* ctx) {
  if (!enabled() || cb == nullptr) return false;
  if (_poll_requested || _kind == KIND_POLL) return false;   // er loopt/staat er al een
  _poll_cb        = cb;
  _poll_ctx       = ctx;
  _poll_requested = true;
  return true;
}

/* Wanneer moet er een POST uit? Zodra er iets te melden is (gebeurtenissen of
 * node-bevestigingen), en anders op de heartbeat-klok. De retry-rem gaat voor:
 * na een fout wordt er even niet geprobeerd, wat er ook klaarstaat. */
bool PushTask::dueNow(unsigned long now) const {
  if (_wifi == NULL || !_wifi->isOnline()) return false;
  if (_retry_at != 0 && (long)(now - _retry_at) < 0) return false;
  if (_cring_count > 0) return true;   /* companion-push: meteen (val/loc) */
  /* Een CLI-antwoord is per definitie iets waar iemand NU op wacht (er is net
   * met de hand een opdracht naar een repeater gestuurd), dus ook meteen. */
  if (_rring_count > 0) return true;
  /* Een gevraagde poll: zodra het uitkomt. Staat achter de pushes in prioriteit
   * (startAttempt), maar mag de heartbeat niet hoeven af te wachten. */
  if (_poll_requested) return true;
  if (_ring_count > 0 || _acked_pending != 0) return true;
  return (long)(now - _next_hb) >= 0;
}

void PushTask::loop() {
  if (_sensors == NULL) return;

  /* Elke ronde de node-bevestigingen overnemen. Het masker in MonitorSensors
   * wordt daarbij gewist; HIER blijft het staan tot een POST met 200 het
   * gedragen heeft, dus een mislukte push verliest geen bevestiging. */
  _acked_pending |= _sensors->takePushAcked();

  if (!enabled()) {
    /* Push staat uit. Een lopende poging afbreken en de wachtrij legen: wat er
     * ligt was voor een server die er nu niet meer is. */
    if (_state != PUSH_IDLE) { closeSock(); _state = PUSH_IDLE; _inflight = 0; _acked_sent = 0; _comp_inflight = 0; _rcli_inflight = 0; }
    _ring_count = 0;
    _acked_pending = 0;
    _cring_count = 0;
    _rring_count = 0;
    _poll_requested = false;   /* geen server om aan te pollen */
    return;
  }

  const unsigned long now = millis();

  switch (_state) {
    case PUSH_IDLE:
      if (dueNow(now)) startAttempt();
      break;

    case PUSH_RESOLVING: {
      uint8_t st = s_pdns.state;
      if (st == 1) {
        _addr_v4     = s_pdns.addr;
        _addr_expiry = now + PUSH_DNS_TTL_MS;
        StrHelper::strncpy(_addr_host, s_host, sizeof(_addr_host));
        startConnect(_addr_v4, s_port);
      } else if (st == 2) {
        failNet("dns: niet gevonden");
      } else if ((long)(now - _deadline) >= 0) {
        failNet("dns: geen antwoord");
      }
      break;
    }

    case PUSH_CONNECTING:
      stepConnect();
      break;

    case PUSH_SENDING:
      stepSend();
      break;

    case PUSH_RECEIVING:
      stepRecv();
      break;
  }
}

/* De url ontleden, het verzoek bouwen en de eerste stap zetten. De url wordt
 * bij ELKE poging opnieuw ontleed: de instelling kan veranderd zijn, en de
 * adrescache (op hostnaam) vangt de gewone herhaling af. */
void PushTask::startAttempt() {
  /* Voorrang: companion (een val mag nooit wachten), dan het antwoord van een
   * remote CLI-opdracht (daar staat iemand met een browser op te wachten), dan de
   * poll (nieuwe opdrachten ophalen), dan pas de gewone sensorpush/heartbeat. De
   * rest van het pad (DNS/connect/send/recv) is voor alle vier identiek. */
  _kind = (_cring_count > 0)  ? KIND_COMPANION
        : (_rring_count > 0)  ? KIND_REPCLI
        : (_poll_requested)   ? KIND_POLL
                              : KIND_SENSOR;

  const char* url = _sensors->pushUrl();

  /* "http://host[:poort][/voorvoegsel]" -- het schema is door de zeef in
   * setSettingValue() al afgedwongen. */
  const char* p = url + 7;
  size_t hl = 0;
  while (p[hl] && p[hl] != ':' && p[hl] != '/' && hl < sizeof(s_host) - 1) hl++;
  memcpy(s_host, p, hl);
  s_host[hl] = 0;
  p += hl;

  s_port = 80;
  if (*p == ':') {
    char* endp = NULL;
    long v = strtol(p + 1, &endp, 10);
    if (v < 1 || v > 65535) { failNet("url: poort"); return; }
    s_port = (uint16_t)v;
    p = endp;
  }
  /* Wat er nog staat is het padvoorvoegsel (of niets). Het contract-pad hangt af
   * van het soort push -- zelfde host/token, ander endpoint. */
  snprintf(s_path, sizeof(s_path), "%s%s", *p ? p : "",
           _kind == KIND_COMPANION ? "/api/companion" :
           _kind == KIND_REPCLI    ? "/api/v1/repeater_settings" :
           /* ?caps= zegt de server WAT deze poller waarmaakt. Wij voeren
            * instellingenopvragingen uit en laten statusverzoeken vallen (die
            * gaan over REQ_TYPE_GET_STATUS, een ander protocol -- zie
            * Poller::parse). Zonder die opgave biedt de beheerpagina een knop
            * "status opvragen" aan die een verzoek in de wachtrij legt dat hier
            * gegarandeerd weggegooid wordt. Komt refresh er ooit bij, dan is dit
            * de ene plek die mee moet. */
           _kind == KIND_POLL      ? "/api/v1/commands?caps=settings" : "/api/sensorpush");

  if (s_host[0] == 0) { failNet("url: geen host"); return; }

  if (!buildRequest(s_host, s_path)) { failNet("verzoek past niet"); return; }

  /* Staat de host er als IPv4-adres, dan is er niets op te zoeken. */
  ip4_addr_t a4;
  if (ip4addr_aton(s_host, &a4)) {
    startConnect(ip4_addr_get_u32(&a4), s_port);
    return;
  }

  /* Adrescache: zelfde host en nog niet verlopen. */
  const unsigned long now = millis();
  if (_addr_v4 != 0 && strcmp(_addr_host, s_host) == 0
      && (long)(now - _addr_expiry) < 0) {
    startConnect(_addr_v4, s_port);
    return;
  }

  /* Opzoeken, langs dezelfde weg als de ping-machine. */
  s_pdns.state = 0;
  s_pdns.addr  = 0;
  StrHelper::strncpy(s_pdns.host, s_host, sizeof(s_pdns.host));
  if (tcpip_try_callback(push_do_resolve, NULL) != ERR_OK) {
    failNet("dns: lwip-wachtrij vol");
    return;
  }
  _state    = PUSH_RESOLVING;
  _deadline = now + PUSH_DNS_MS;
}

/* Het verzoek in zijn geheel klaarzetten: koppen plus body in één buffer, dan
 * kan de verzendfase domweg "wat er nog ligt" de socket in schuiven.
 *
 * De gebeurtenissen gaan er in RINGVOLGORDE in, zoveel als er in de body
 * passen; de rest blijft in de ring en de push die DIRECT hierna volgt (de ring
 * is dan nog niet leeg, dus dueNow blijft waar) neemt ze mee. _inflight en
 * _acked_sent leggen vast wat er in DEZE post zit, zodat finishOk() precies
 * dat opruimt en niets anders. */
bool PushTask::buildRequest(const char* host, const char* path) {
  /* De POLL is een GET zonder body: aparte, korte weg. Zelfde token, zelfde
   * Connection: close (recv leest tot de server sluit). */
  if (_kind == KIND_POLL) {
    s_req_len = 0;
    s_req_off = 0;
    return appendf(s_req, sizeof(s_req), s_req_len,
                   "GET %s HTTP/1.1\r\n"
                   "Host: %s\r\n"
                   "Authorization: Bearer %s\r\n"
                   "Connection: close\r\n\r\n",
                   path, host, _sensors->pushToken());
  }

  static char body[PUSH_BODY_MAX];
  size_t blen = 0;

  if (_kind == KIND_COMPANION) {
    if (!buildCompanionBody(body, sizeof(body), blen)) return false;
  } else if (_kind == KIND_REPCLI) {
    if (!buildRepCliBody(body, sizeof(body), blen)) return false;
  } else {
    if (!buildSensorBody(body, sizeof(body), blen)) return false;
  }

  /* De koppen erbij. Connection: close is de afspraak waarmee het EINDE van het
   * antwoord herkenbaar is zonder chunked-parser: de server sluit, recv geeft
   * 0, klaar. Zelfde koppen voor beide soorten; alleen pad + body verschillen. */
  s_req_len = 0;
  s_req_off = 0;
  if (!appendf(s_req, sizeof(s_req), s_req_len,
               "POST %s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "Authorization: Bearer %s\r\n"
               "Content-Type: application/json\r\n"
               "Content-Length: %u\r\n"
               "Connection: close\r\n\r\n",
               path, host, _sensors->pushToken(), (unsigned)blen)) return false;
  if (s_req_len + blen >= sizeof(s_req)) return false;
  memcpy(s_req + s_req_len, body, blen + 1);
  s_req_len += blen;
  return true;
}

/* KIND_POLL heeft geen body om te bouwen; deze bestaat alleen zodat de
 * declaratie in de header een definitie heeft en het pad symmetrisch leest. */
bool PushTask::buildPollRequest() { return true; }

/* v2.5.1 -- de companion-body. Alle wachtende companion-plaatsen gaan in één
 * POST (ze delen host/token); _comp_inflight legt vast hoeveel, zodat finishOk()
 * precies dat opruimt. lat/lon worden WEGGELATEN als has_loc onwaar is; fall
 * krijgt fall_ts:0 en een lege fall_kind als er geen val-event is. */
bool PushTask::buildCompanionBody(char* body, size_t cap, size_t& blen) {
  blen = 0;
  if (!appendf(body, cap, blen, "{\"companions\":[")) return false;

  char hex[PUB_KEY_SIZE * 2 + 1];
  _comp_inflight = 0;
  for (uint8_t i = 0; i < _cring_count; i++) {
    const CompanionPush& c = _cring[(_cring_tail + i) % COMP_RING_SIZE];

    /* Eerst apart opbouwen: past dit stuk niet meer, dan blijft de body geldig
     * en gaat de rest met de VOLGENDE post mee (de ring is dan nog niet leeg). */
    char piece[220];
    size_t plen = 0;
    for (int j = 0; j < PUB_KEY_SIZE; j++) snprintf(&hex[j * 2], 3, "%02x", (unsigned)c.pub_key[j]);
    const char* fk = c.fall_kind == 1 ? "val" :
                     c.fall_kind == 2 ? "nomotion" :
                     c.fall_kind == 3 ? "sos" : "";
    if (!appendf(piece, sizeof(piece), plen, "%s{\"pubkey\":\"%s\"",
                 _comp_inflight == 0 ? "" : ",", hex)) return false;
    if (c.has_loc) {
      if (!appendf(piece, sizeof(piece), plen, ",\"lat\":%.6f,\"lon\":%.6f",
                   c.lat, c.lon)) return false;
    }
    if (c.batt >= 0) {
      if (!appendf(piece, sizeof(piece), plen, ",\"batt\":%d", (int)c.batt)) return false;
    }
    if (!appendf(piece, sizeof(piece), plen,
                 ",\"seen\":%lu,\"fall_ts\":%lu,\"fall_kind\":\"%s\"}",
                 (unsigned long)c.seen, (unsigned long)c.fall_ts, fk)) return false;

    if (blen + plen + 2 >= cap) break;   /* rest in de volgende post ("]}" past nog) */
    memcpy(body + blen, piece, plen + 1);
    blen += plen;
    _comp_inflight++;
  }

  if (!appendf(body, cap, blen, "]}")) return false;
  return true;
}

/* De body van een remote-CLI-antwoord. EEN per POST en niet meerdere: het
 * contract met MeshManager draagt een enkele repeater met een enkele
 * settings-map, en RepeaterCli levert er toch maar een tegelijk. Bleef er een
 * tweede in de ring staan, dan houdt dueNow() de volgende post meteen waar.
 *
 * De paramNAAM gaat door dezelfde ontsnapping als de waarde. Dat is geen
 * overdaad: de naam is "cmd:" plus een opdracht die iemand in een webformulier
 * getypt heeft, dus daar KAN een aanhalingsteken in zitten -- en een JSON die
 * daarop breekt zou de hele post laten mislukken zonder dat er iets te zien is. */
bool PushTask::buildRepCliBody(char* body, size_t cap, size_t& blen) {
  blen = 0;
  _rcli_inflight = 0;
  if (_rring_count == 0) return false;

  const RepCliPush& r = _rring[_rring_tail];

  if (!appendf(body, cap, blen, "{\"repeater\":{\"pubkey_prefix\":\"%s\"},\"settings\":{\"",
               r.node)) return false;
  if (!appendJsonText(body, cap, blen, r.param)) return false;
  /* value: een string "..." OF JSON null. null (has_value==false) = "gevraagd,
   * geen antwoord"; de server telt die parameter als onbeantwoord. */
  if (r.has_value) {
    if (!appendf(body, cap, blen, "\":\"")) return false;
    if (!appendJsonText(body, cap, blen, r.value)) return false;
    if (!appendf(body, cap, blen, "\"}}")) return false;
  } else {
    if (!appendf(body, cap, blen, "\":null}}")) return false;
  }

  _rcli_inflight = 1;
  return true;
}

/* De sensorpush-body (ongewijzigd t.o.v. v2.5.0; enkel losgemaakt van de koppen
 * zodat companion en sensor dezelfde headerbouw delen). */
bool PushTask::buildSensorBody(char* body, size_t cap, size_t& blen) {
  blen = 0;

  /* De staart eerst (de acked-lijst en de sluithaken): dan is bij elke
   * gebeurtenis exact bekend hoeveel ruimte er gereserveerd moet blijven. */
  char tail[200];
  size_t tlen = 0;
  tail[0] = 0;
  if (!appendf(tail, sizeof(tail), tlen, "],\"acked\":[")) return false;
  bool first_ack = true;
  for (uint8_t ch = 0; ch < 64; ch++) {
    if (!(_acked_pending & ((uint64_t)1 << ch))) continue;
    if (!appendf(tail, sizeof(tail), tlen, first_ack ? "%u" : ",%u", (unsigned)ch)) return false;
    first_ack = false;
  }
  if (!appendf(tail, sizeof(tail), tlen, "]}")) return false;

  /* seq telt per VERZENDING op, ook bij een herkansing van dezelfde
   * gebeurtenissen: zo ziet de server elk gat. */
  _seq++;

  if (!appendf(body, cap, blen,
               "{\"node\":\"%s\",\"seq\":%lu,\"boot\":%lu,\"hb_s\":%u,\"events\":[",
               _node_id, (unsigned long)_seq, (unsigned long)_boot,
               (unsigned)_sensors->pushHbSecs())) return false;

  _inflight = 0;
  for (uint8_t i = 0; i < _ring_count; i++) {
    const MonitorEvent& ev = _ring[(_ring_tail + i) % PUSH_RING_SIZE];

    /* Het stuk eerst apart opbouwen: past het niet meer, dan blijft de body
     * geldig en gaat dit stuk met de VOLGENDE post mee. */
    char piece[MON_EVENT_TEXT_LEN * 2 + 64];
    size_t plen = 0;
    if (!appendf(piece, sizeof(piece), plen, "%s{\"ch\":%u,\"kind\":\"%s\",\"text\":\"",
                 i == 0 ? "" : ",", (unsigned)ev.ch, ev.up ? "op" : "neer")) return false;
    if (!appendJsonText(piece, sizeof(piece), plen, ev.text)) return false;
    if (!appendf(piece, sizeof(piece), plen, "\",\"sev\":\"%s\",\"sim\":%u}",
                 ev.sev_high ? "hoog" : "laag", (unsigned)ev.sim)) return false;

    if (blen + plen + tlen >= cap) break;   /* rest in de volgende post */
    memcpy(body + blen, piece, plen + 1);
    blen += plen;
    _inflight++;
  }

  if (blen + tlen >= cap) return false;   /* kan niet: staart is gereserveerd */
  memcpy(body + blen, tail, tlen + 1);
  blen += tlen;

  _acked_sent = _acked_pending;
  return true;
}

void PushTask::startConnect(uint32_t addr_v4, uint16_t port) {
  _sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (_sock < 0) { failNet("socket"); return; }

  /* O_NONBLOCK vóór connect: vanaf hier wacht geen enkele aanroep. */
  int fl = lwip_fcntl(_sock, F_GETFL, 0);
  lwip_fcntl(_sock, F_SETFL, fl | O_NONBLOCK);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family      = AF_INET;
  sa.sin_port        = htons(port);
  sa.sin_addr.s_addr = addr_v4;

  int r = lwip_connect(_sock, (struct sockaddr*)&sa, sizeof(sa));
  if (r == 0) {
    /* Meteen verbonden (kan op een lokaal net): door naar verzenden. */
    _state    = PUSH_SENDING;
    _deadline = millis() + PUSH_SEND_MS;
    return;
  }
  if (errno != EINPROGRESS) { failNet("connect"); return; }

  _state    = PUSH_CONNECTING;
  _deadline = millis() + PUSH_CONNECT_MS;
}

/* Kijken (niet wachten) of de verbinding er is: select met tijdsduur nul, en
 * bij schrijfbaarheid het echte oordeel uit SO_ERROR -- schrijfbaar betekent
 * "de poging is klaar", niet "hij is gelukt". */
void PushTask::stepConnect() {
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(_sock, &wfds);
  struct timeval tv = { 0, 0 };
  int r = lwip_select(_sock + 1, NULL, &wfds, NULL, &tv);
  if (r < 0) { failNet("select"); return; }
  if (r > 0 && FD_ISSET(_sock, &wfds)) {
    int err = 0;
    socklen_t elen = sizeof(err);
    lwip_getsockopt(_sock, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) { failNet("connect geweigerd"); return; }
    _state    = PUSH_SENDING;
    _deadline = millis() + PUSH_SEND_MS;
    return;
  }
  if ((long)(millis() - _deadline) >= 0) failNet("connect: tijd op");
}

void PushTask::stepSend() {
  /* Zoveel schuiven als de zendbuffer aanneemt. Het verzoek is ~1,4 kB en de
   * lwIP-zendbuffer standaard groter, dus meestal is dit één ronde -- maar
   * daar wordt niet op gerekend. */
  ssize_t n = lwip_send(_sock, s_req + s_req_off, s_req_len - s_req_off, 0);
  if (n > 0) {
    s_req_off += (size_t)n;
    if (s_req_off >= s_req_len) {
      _state      = PUSH_RECEIVING;
      _deadline   = millis() + PUSH_RECV_MS;
      s_resp_len  = 0;
      s_resp[0]   = 0;
    }
    return;
  }
  if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
    if ((long)(millis() - _deadline) >= 0) failNet("send: tijd op");
    return;
  }
  failNet("send");
}

/* Het antwoord ontleden en afronden. Wordt aangeroepen als de server sluit of
 * als het antwoord er zichtbaar helemaal is. */
static int parseStatus(const char* resp) {
  /* "HTTP/1.1 200 ..." -- het getal na de eerste spatie. */
  const char* sp = strchr(resp, ' ');
  return sp ? atoi(sp + 1) : 0;
}

void PushTask::stepRecv() {
  /* Lezen wat er ligt; wat niet meer in de buffer past wordt gelezen en
   * weggegooid (de statusregel en de ack-lijst staan vooraan). */
  char sink[128];
  char*  dst = (s_resp_len < sizeof(s_resp) - 1) ? s_resp + s_resp_len : sink;
  size_t cap = (s_resp_len < sizeof(s_resp) - 1) ? sizeof(s_resp) - 1 - s_resp_len : sizeof(sink);

  ssize_t n = lwip_recv(_sock, dst, cap, 0);
  if (n > 0) {
    if (dst != sink) {
      s_resp_len += (size_t)n;
      s_resp[s_resp_len] = 0;
    }
    /* Al compleet zonder op de sluiting te wachten? Het contract-antwoord van een
     * PUSH is één plat JSON-object; staat er na de koppen een '}', dan is hij
     * binnen. Voor de POLL geldt dat NIET: /api/v1/commands is genest
     * ({"settings":[{...}]}), dus de eerste '}' is een BINNENSTE en zou het
     * antwoord te vroeg afronden. Daarom leest de poll altijd tot de server sluit
     * (Connection: close, dus n==0) of tot de deadline. */
    const char* hdr_end = strstr(s_resp, "\r\n\r\n");
    if (_kind != KIND_POLL && hdr_end != NULL && strchr(hdr_end + 4, '}') != NULL) {
      /* doorvallen naar afronden */
    } else {
      /* Nog niet compleet. De deadline geldt ook hier: een server die eindeloos
       * druppelt zonder ooit af te ronden mag deze fase niet openhouden. */
      if ((long)(millis() - _deadline) >= 0) { failNet("recv: tijd op"); }
      return;
    }
  } else if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
    if ((long)(millis() - _deadline) >= 0) { failNet("recv: tijd op"); }
    return;
  } else if (n < 0) {
    failNet("recv");
    return;
  }
  /* n == 0 (server sloot) of het antwoord is compleet: afronden. */

  const int status = parseStatus(s_resp);
  if (status < 100) {
    /* Geen herkenbare statusregel: dit is een kapotte verbinding of een
     * niet-HTTP-antwoord, geen weigering van de server. Kort opnieuw proberen
     * dus, en niet een hele heartbeat wachten. */
    failNet("antwoord onleesbaar");
    return;
  }
  if (status != 200) { failHttp(status); return; }

  const char* hdr_end = strstr(s_resp, "\r\n\r\n");
  const char* body    = hdr_end ? hdr_end + 4 : s_resp;

  /* DE POLL: het BODY naar de Poller, die het ontleedt. PushTask kent de vorm van
   * de opdrachtwachtrij niet. De callback mag alleen kopieren (hij draait in
   * loop(), niet in een radio-callback). */
  if (_kind == KIND_POLL) {
    if (_poll_cb != NULL) _poll_cb(_poll_ctx, body);
    finishOk();
    return;
  }

  /* De server-bevestigingen: "ack":[kanaal,...] -> per kanaal het herhalen
   * stoppen, hetzelfde effect als een ok-DM maar dan gericht. Alleen zinvol op
   * de sensorpush; /api/companion kent geen ack-lijst. */
  const char* ap      = (_kind == KIND_SENSOR) ? strstr(body, "\"ack\"") : NULL;
  if (ap != NULL) {
    ap = strchr(ap, '[');
    if (ap != NULL) {
      ap++;
      for (int guard = 0; guard < 64 && *ap && *ap != ']'; guard++) {
        while (*ap == ' ' || *ap == ',') ap++;
        if (*ap == ']' || *ap == 0) break;
        char* end = NULL;
        long ch = strtol(ap, &end, 10);
        if (end == ap) break;   /* geen getal: contract geschonden, stoppen */
        if (ch >= 0 && ch <= 63) _sensors->confirmAlertChannel((uint8_t)ch);
        ap = end;
      }
    }
  }

  finishOk();
}

void PushTask::finishOk() {
  closeSock();

  if (_kind == KIND_POLL) {
    /* De poll is af (de callback heeft het body al verwerkt). Niets op te ruimen,
     * geen ring, en de heartbeat-klok blijft met rust: pollen is geen meten. */
    _poll_requested = false;
  } else if (_kind == KIND_COMPANION) {
    /* Alleen de companion-ring opruimen; de sensorring, het acked-masker en de
     * heartbeat-klok blijven met rust. Bleef er iets in de ring (overloop of
     * body vol), dan houdt dueNow() de volgende post meteen waar. */
    _cring_tail    = (uint8_t)((_cring_tail + _comp_inflight) % COMP_RING_SIZE);
    _cring_count   = (uint8_t)(_cring_count - _comp_inflight);
    _comp_inflight = 0;
  } else if (_kind == KIND_REPCLI) {
    /* Alleen de eigen ring opruimen. De heartbeat-klok blijft met rust: een
     * CLI-antwoord is buiten de cadans om en mag de belofte "je hoort me elke
     * hb_s" niet vooruitschuiven -- dan zou een handmatige opdracht de betekenis
     * van de stilte tussen twee heartbeats veranderen. */
    _rring_tail    = (uint8_t)((_rring_tail + _rcli_inflight) % RCLI_RING_SIZE);
    _rring_count   = (uint8_t)(_rring_count - _rcli_inflight);
    _rcli_inflight = 0;
  } else {
    /* Precies opruimen wat deze post droeg: _inflight plaatsen vanaf de staart
     * (onMonitorEvent heeft _inflight al verlaagd als de overloop er een van
     * opat) en de bevestigingen uit de momentopname. Wat er ondertussen bij
     * kwam blijft staan en maakt dueNow() meteen weer waar. */
    _ring_tail   = (uint8_t)((_ring_tail + _inflight) % PUSH_RING_SIZE);
    _ring_count  = (uint8_t)(_ring_count - _inflight);
    _inflight    = 0;
    _acked_pending &= ~_acked_sent;
    _acked_sent  = 0;
    /* De heartbeat-klok alleen na een SENSORpush verzetten: een companion-push
     * is buiten de cadans om en mag de belofte niet vooruitschuiven. */
    _next_hb     = millis() + (unsigned long)_sensors->pushHbSecs() * 1000UL;
  }

  _sent_ok++;
  _last_ok     = millis();
  _last_status = 200;
  _last_logged_status = -1;   /* een volgende fout is weer een nieuwe melding waard */
  _retry_at    = 0;
  _state       = PUSH_IDLE;
}

void PushTask::failNet(const char* why) {
  closeSock();
  _fail_net++;
  _inflight      = 0;
  _comp_inflight = 0;
  _rcli_inflight = 0;
  _acked_sent    = 0;
  /* Een mislukte POLL niet in de retry-lus vasthouden: de Poller vraagt op zijn
   * eigen cadans (poll_secs) een nieuwe. Zo hamert een netwerkhapering de
   * opdrachtwachtrij niet, en houdt poll_secs zijn betekenis. */
  _poll_requested = false;
  /* Niet hameren: even wachten en dan gewoon opnieuw -- de gebeurtenissen staan
   * veilig in de ring en de bevestigingen in het masker. */
  _retry_at = millis() + PUSH_RETRY_MS;
  _state    = PUSH_IDLE;
  MESH_DEBUG_PRINTLN("PushTask: mislukt (%s), opnieuw over %lus", why,
                     (unsigned long)(PUSH_RETRY_MS / 1000));
}

void PushTask::failHttp(int status) {
  closeSock();
  _fail_http++;
  _inflight      = 0;
  _comp_inflight = 0;
  _rcli_inflight = 0;
  _acked_sent    = 0;
  _poll_requested = false;   /* zie failNet: de Poller vraagt op zijn eigen cadans */
  _last_status = status;

  /* Loggen, niet spammen: één regel per nieuwe status, en dan wachten tot de
   * volgende heartbeat. Een 401 (token) of 404 (node onbekend) gaat niet
   * vanzelf over; er elke seconde tegenaan lopen maakt alleen de logs vol. */
  if (status != _last_logged_status) {
    MESH_DEBUG_PRINTLN("PushTask: server antwoordde %d%s; wacht tot de volgende heartbeat",
                       status,
                       status == 401 ? " (token fout)" :
                       status == 404 ? " (node onbekend)" : "");
    _last_logged_status = status;
  }
  const unsigned long wait = (unsigned long)_sensors->pushHbSecs() * 1000UL;
  _retry_at = millis() + wait;
  /* De heartbeat-klok alleen bij een SENSORpush vooruitzetten. Een geweigerde
   * companion-push mag de sensor-heartbeatbelofte niet uitstellen; _retry_at
   * remt het hameren al af. */
  if (_kind == KIND_SENSOR) _next_hb = millis() + wait;
  _state    = PUSH_IDLE;
}

void PushTask::closeSock() {
  if (_sock >= 0) {
    lwip_close(_sock);
    _sock = -1;
  }
}
