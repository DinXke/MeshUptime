#include "TakTask.h"
#include "TimeFmt.h"   /* TIME_FLOOR */

#include <string.h>
#include <math.h>
#include <time.h>
#include <helpers/AdvertDataHelpers.h>   /* ADV_TYPE_* */

#ifdef ESP32
  #include <WiFi.h>
  #include "lwip/sockets.h"
  #include "lwip/inet.h"
  #include <errno.h>
  #include <fcntl.h>
#endif

/* Acht plaatsen: een advertgolf na een herstart van de dakrepeater levert er
 * een tiental tegelijk, en de meeste daarvan hebben geen lat/lon. */
#define TAK_RING          8
#define TAK_OUT_BUF       2048          /* positie + eventuele noodmelding   */
#define TAK_CONNECT_MS    4000UL
#define TAK_RETRY_MIN_MS  5000UL
#define TAK_RETRY_MAX_MS  60000UL
#define TAK_RX_CHUNK      512
#define TAK_RX_ROUNDS     4             /* hoogstens 2 kB weglezen per ronde */

struct TakPos {
  uint8_t  pub[6];
  char     name[32];
  uint8_t  src, adv_type;
  bool     sos;
  int8_t   hops;
  int16_t  batt, snr4;
  int32_t  lat_e6, lon_e6;
  uint32_t ts;
};

enum TakState : uint8_t { TAK_IDLE, TAK_CONNECTING, TAK_UP };

static fs::FS*  _fs = nullptr;

/* instellingen */
static bool     _on = false;
static char     _host[16] = "";
static uint16_t _port = TAK_PORT_DEFAULT;
static bool     _adverts = true;
static uint16_t _stale_min = TAK_STALE_DEFAULT;

/* ring */
static TakPos   _ring[TAK_RING];
static uint8_t  _head = 0, _count = 0;

/* verbinding */
static TakState      _st = TAK_IDLE;
static int           _sock = -1;
static unsigned long _deadline = 0, _retry_at = 0;
static unsigned long _backoff = TAK_RETRY_MIN_MS;
static char          _out[TAK_OUT_BUF];
static uint16_t      _out_len = 0, _out_off = 0;

/* stand */
static uint32_t _sent = 0, _lost = 0, _connects = 0;
static char     _note[40] = "";

// ------------------------------------------------------------------ opslag
static void takSave() {
  if (_fs == nullptr) return;
  fs::File f = _fs->open(TAK_CFG_PATH, "w", true);
  if (!f) return;
  f.printf("on %d\nhost %s\nport %u\nadv %d\nstale %u\n",
           _on ? 1 : 0, _host, _port, _adverts ? 1 : 0, _stale_min);
  f.close();
}

static bool validIp(const char* s) {
#ifdef ESP32
  struct in_addr a;
  return s && *s && inet_aton(s, &a) != 0;
#else
  return s && *s;
#endif
}

void tak_begin(fs::FS& fs) {
  _fs = &fs;
  if (!fs.exists(TAK_CFG_PATH)) return;
  fs::File f = fs.open(TAK_CFG_PATH, "r");
  if (!f) return;
  char line[48];
  while (f.available()) {
    size_t n = 0;
    while (f.available() && n < sizeof(line) - 1) {
      int ch = f.read();
      if (ch < 0 || ch == '\n') break;
      if (ch != '\r') line[n++] = (char)ch;
    }
    line[n] = 0;
    char* v = strchr(line, ' ');
    if (v == nullptr) continue;
    *v++ = 0;
    /* Een ontbrekende of rare regel laat de standaard staan. */
    if      (strcmp(line, "on") == 0)    _on = atoi(v) != 0;
    else if (strcmp(line, "host") == 0)  { if (validIp(v)) { strncpy(_host, v, sizeof(_host) - 1); _host[sizeof(_host) - 1] = 0; } }
    else if (strcmp(line, "port") == 0)  { int p = atoi(v); if (p > 0 && p < 65536) _port = (uint16_t)p; }
    else if (strcmp(line, "adv") == 0)   _adverts = atoi(v) != 0;
    else if (strcmp(line, "stale") == 0) { int s = atoi(v); if (s >= TAK_STALE_MIN && s <= TAK_STALE_MAX) _stale_min = (uint16_t)s; }
  }
  f.close();
}

bool tak_enabled() { return _on && _host[0] != 0; }

// -------------------------------------------------------------------- ring
void tak_queue_pos(const uint8_t* pub, const char* name, TakSrc src, uint8_t adv_type,
                   double lat, double lon, int batt, int snr4, int hops,
                   bool sos, uint32_t ts) {
  if (!tak_enabled() || pub == nullptr) return;
  if (src == TAK_SRC_ADVERT && !_adverts) return;
  if (ts < TIME_FLOOR) return;                              /* regel 4 */
  if (lat == 0.0 && lon == 0.0) return;                     /* "niet ingesteld" */
  if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;

  /* Regel 2: staat deze afzender al in de ring, dan wordt dat de nieuwe. */
  TakPos* p = nullptr;
  for (uint8_t i = 0; i < _count; i++) {
    TakPos& q = _ring[(_head + i) % TAK_RING];
    if (memcmp(q.pub, pub, 6) == 0) {
      /* Een SOS die nog niet weg is blijft een SOS, ook als er intussen een
       * gewone positie achteraan komt. */
      sos = sos || q.sos;
      p = &q;
      break;
    }
  }
  if (p == nullptr) {
    if (_count == TAK_RING) {                               /* regel 1 */
      _head = (_head + 1) % TAK_RING;
      _count--;
      _lost++;
    }
    p = &_ring[(_head + _count) % TAK_RING];
    _count++;
  }

  memcpy(p->pub, pub, 6);
  p->name[0] = 0;
  if (name) { strncpy(p->name, name, sizeof(p->name) - 1); p->name[sizeof(p->name) - 1] = 0; }
  p->src      = (uint8_t)src;
  p->adv_type = adv_type;
  p->sos      = sos;
  p->hops     = (int8_t)(hops < -1 ? -1 : (hops > 127 ? 127 : hops));
  p->batt     = (int16_t)batt;
  p->snr4     = (int16_t)snr4;
  p->lat_e6   = (int32_t)lrint(lat * 1e6);
  p->lon_e6   = (int32_t)lrint(lon * 1e6);
  p->ts       = ts;
}

// ------------------------------------------------------------ CoT bouwen
static size_t xmlEsc(char* dst, size_t max, const char* s) {
  size_t n = 0;
  if (max == 0) return 0;
  for (; s && *s && n + 7 < max; s++) {
    const char* e = nullptr;
    switch (*s) {
      case '&':  e = "&amp;";  break;
      case '<':  e = "&lt;";   break;
      case '>':  e = "&gt;";   break;
      case '"':  e = "&quot;"; break;
      case '\'': e = "&apos;"; break;
    }
    if (e) { size_t l = strlen(e); memcpy(dst + n, e, l); n += l; }
    else if ((unsigned char)*s >= 0x20) dst[n++] = *s;
  }
  dst[n] = 0;
  return n;
}

static void isoTime(char* dst, size_t max, uint32_t t) {
  time_t tt = (time_t)t;
  struct tm tm;
  gmtime_r(&tt, &tm);
  strftime(dst, max, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

/* CoT-type volgens MIL-STD-2525 zoals ATAK het leest: een persoon met een
 * tracker is een eenheid (U-C), een repeater of room-server een installatie,
 * een sensornode uitrusting. Onbekende soorten worden in ATAK een gewone stip. */
static const char* cotType(const TakPos& p) {
  if (p.src == TAK_SRC_COMPANION) return "a-f-G-U-C";
  switch (p.adv_type) {
    case ADV_TYPE_CHAT:     return "a-f-G-U-C";
    case ADV_TYPE_REPEATER: return "a-f-G-I-U-T";
    case ADV_TYPE_ROOM:     return "a-f-G-I-U-T";
    case ADV_TYPE_SENSOR:   return "a-f-G-E-S";
    default:                return "a-f-G";
  }
}

static const char* srcText(const TakPos& p) {
  if (p.src == TAK_SRC_COMPANION) return "companion";
  if (p.src == TAK_SRC_TEST)      return "test";
  switch (p.adv_type) {
    case ADV_TYPE_CHAT:     return "chat";
    case ADV_TYPE_REPEATER: return "repeater";
    case ADV_TYPE_ROOM:     return "room";
    case ADV_TYPE_SENSOR:   return "sensor";
    default:                return "node";
  }
}

/* Zet één positie (en bij een SOS ook de noodmelding) in _out. */
static void buildCot(const TakPos& p) {
  char uid[20], call[100], tnow[24], tstale[24], rem[96];
  snprintf(uid, sizeof(uid), "MC-%02x%02x%02x%02x%02x%02x",
           p.pub[0], p.pub[1], p.pub[2], p.pub[3], p.pub[4], p.pub[5]);
  if (p.name[0]) xmlEsc(call, sizeof(call), p.name);
  else           snprintf(call, sizeof(call), "%s", uid);
  isoTime(tnow, sizeof(tnow), p.ts);
  isoTime(tstale, sizeof(tstale), p.ts + (uint32_t)_stale_min * 60UL);

  /* Een advert draagt de INGESTELDE plaats van een node, geen gps-meting;
   * een companion stuurt zijn gps-fix. ATAK toont dat verschil via 'how'. */
  bool gps = (p.src == TAK_SRC_COMPANION);
  double lat = p.lat_e6 / 1e6, lon = p.lon_e6 / 1e6;

  size_t r = 0;
  if (p.snr4 != INT16_MIN && p.hops >= 0)
    r = snprintf(rem, sizeof(rem), "MeshCore %s, SNR %.1f dB, %d hop%s, via MeshUptime",
                 srcText(p), p.snr4 / 4.0, p.hops, p.hops == 1 ? "" : "s");
  else
    r = snprintf(rem, sizeof(rem), "MeshCore %s, via MeshUptime", srcText(p));
  (void)r;

  int n = snprintf(_out, sizeof(_out),
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
    "<event version=\"2.0\" uid=\"%s\" type=\"%s\" how=\"%s\" time=\"%s\" start=\"%s\" stale=\"%s\">"
    "<point lat=\"%.6f\" lon=\"%.6f\" hae=\"9999999.0\" ce=\"%s\" le=\"9999999.0\"/>"
    "<detail><contact callsign=\"%s\"/>%s",
    uid, cotType(p), gps ? "m-g" : "h-e", tnow, tnow, tstale,
    lat, lon, gps ? "10.0" : "9999999.0",
    call, p.src == TAK_SRC_COMPANION ? "<__group name=\"Cyan\" role=\"Team Member\"/>" : "");
  if (n < 0 || n >= (int)sizeof(_out)) { _out_len = 0; return; }

  if (p.batt >= 0 && p.batt <= 100)
    n += snprintf(_out + n, sizeof(_out) - n, "<status battery=\"%d\"/>", p.batt);
  if (n < (int)sizeof(_out))
    n += snprintf(_out + n, sizeof(_out) - n, "<remarks>%s</remarks></detail></event>", rem);

  /* De noodmelding is een eigen event met een vaste uid per persoon: ATAK
   * toont dan de rode banner en vervangt hem bij een herhaling, in plaats van
   * er elke keer een nieuwe bij te zetten. */
  if (p.sos && n < (int)sizeof(_out)) {
    n += snprintf(_out + n, sizeof(_out) - n,
      "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
      "<event version=\"2.0\" uid=\"%s-9-1-1\" type=\"b-a-o-tbl\" how=\"m-g\" time=\"%s\" start=\"%s\" stale=\"%s\">"
      "<point lat=\"%.6f\" lon=\"%.6f\" hae=\"9999999.0\" ce=\"10.0\" le=\"9999999.0\"/>"
      "<detail><link uid=\"%s\" type=\"%s\" relation=\"p-p\"/>"
      "<contact callsign=\"%s-Alert\"/><emergency type=\"911 Alert\">%s</emergency></detail></event>",
      uid, tnow, tnow, tstale, lat, lon, uid, cotType(p), call, call);
  }

  if (n < 0 || n >= (int)sizeof(_out)) { _out_len = 0; return; }   /* te lang: niet half sturen */
  _out_len = (uint16_t)n;
  _out_off = 0;
}

// ------------------------------------------------------------ verbinding
#ifdef ESP32
static void takClose() {
  if (_sock >= 0) lwip_close(_sock);
  _sock = -1;
  _st = TAK_IDLE;
  _out_len = _out_off = 0;       /* een half verstuurd bericht is weg */
}

static void takFail(const char* why) {
  snprintf(_note, sizeof(_note), "%s", why);
  takClose();
  _retry_at = millis() + _backoff;
  _backoff = _backoff * 2 > TAK_RETRY_MAX_MS ? TAK_RETRY_MAX_MS : _backoff * 2;
}

static void takStartConnect() {
  struct in_addr a;
  if (inet_aton(_host, &a) == 0) { takFail("ongeldig adres"); return; }

  _sock = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (_sock < 0) { takFail("socket"); return; }
  int fl = lwip_fcntl(_sock, F_GETFL, 0);
  lwip_fcntl(_sock, F_SETFL, fl | O_NONBLOCK);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family      = AF_INET;
  sa.sin_port        = htons(_port);
  sa.sin_addr.s_addr = a.s_addr;

  int r = lwip_connect(_sock, (struct sockaddr*)&sa, sizeof(sa));
  if (r == 0) { _st = TAK_UP; _connects++; _backoff = TAK_RETRY_MIN_MS; _note[0] = 0; return; }
  if (errno != EINPROGRESS) { takFail("connect"); return; }
  _st = TAK_CONNECTING;
  _deadline = millis() + TAK_CONNECT_MS;
}

/* Schrijfbaar betekent "de poging is klaar", niet "gelukt": het oordeel staat
 * in SO_ERROR. */
static void takStepConnect() {
  fd_set wfds;
  FD_ZERO(&wfds);
  FD_SET(_sock, &wfds);
  struct timeval tv = { 0, 0 };
  int r = lwip_select(_sock + 1, NULL, &wfds, NULL, &tv);
  if (r < 0) { takFail("select"); return; }
  if (r > 0 && FD_ISSET(_sock, &wfds)) {
    int err = 0;
    socklen_t elen = sizeof(err);
    lwip_getsockopt(_sock, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err != 0) { takFail("connect geweigerd"); return; }
    _st = TAK_UP;
    _connects++;
    _backoff = TAK_RETRY_MIN_MS;
    _note[0] = 0;
    return;
  }
  if ((long)(millis() - _deadline) >= 0) takFail("connect: tijd op");
}

/* Regel 3: wat de server ons stuurt, lezen we weg. 0 = hij sloot. */
static bool takDrain() {
  static char junk[TAK_RX_CHUNK];
  for (int i = 0; i < TAK_RX_ROUNDS; i++) {
    int r = lwip_recv(_sock, junk, sizeof(junk), MSG_DONTWAIT);
    if (r > 0) continue;
    if (r == 0) { takFail("server sloot"); return false; }
    if (errno == EWOULDBLOCK || errno == EAGAIN) return true;
    takFail("recv");
    return false;
  }
  return true;
}

static void takPump() {
  if (_out_len == 0 || _out_off >= _out_len) return;
  int r = lwip_send(_sock, _out + _out_off, _out_len - _out_off, MSG_DONTWAIT);
  if (r > 0) {
    _out_off += r;
    if (_out_off >= _out_len) { _out_len = _out_off = 0; _sent++; }
    return;
  }
  if (r < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) return;   /* volgende ronde */
  takFail("send");
}
#endif

void tak_loop() {
#ifdef ESP32
  if (!tak_enabled() || WiFi.status() != WL_CONNECTED) {
    if (_sock >= 0) takClose();
    return;
  }
  switch (_st) {
    case TAK_IDLE:
      if ((long)(millis() - _retry_at) >= 0) takStartConnect();
      break;
    case TAK_CONNECTING:
      takStepConnect();
      break;
    case TAK_UP:
      if (!takDrain()) break;
      if (_out_len == 0 && _count > 0) {
        buildCot(_ring[_head]);
        _head = (_head + 1) % TAK_RING;
        _count--;
      }
      takPump();
      break;
  }
#endif
}

// ------------------------------------------------------------------- CLI
static bool onOff(const char* a, bool* out) {
  if (strcmp(a, "on") == 0 || strcmp(a, "aan") == 0 || strcmp(a, "1") == 0) { *out = true; return true; }
  if (strcmp(a, "off") == 0 || strcmp(a, "uit") == 0 || strcmp(a, "0") == 0) { *out = false; return true; }
  return false;
}

static void takStatus(char* reply, size_t max) {
  const char* st = !_on ? "uit" : (_host[0] == 0 ? "geen host" :
                   _st == TAK_UP ? "verbonden" : _st == TAK_CONNECTING ? "verbinden" : "wacht");
  snprintf(reply, max, "TAK %s %s:%u, %lu verstuurd, %lu weg, %u in rij, adverts %s, stale %um%s%s",
           st, _host[0] ? _host : "-", _port, (unsigned long)_sent, (unsigned long)_lost,
           _count, _adverts ? "aan" : "uit", _stale_min, _note[0] ? " - " : "", _note);
}

/* Na elke wijziging opnieuw verbinden, met de nieuwe instellingen. */
static void takApply() {
#ifdef ESP32
  if (_sock >= 0) takClose();
#endif
  _retry_at = 0;
  _backoff = TAK_RETRY_MIN_MS;
  _note[0] = 0;
  takSave();
}

bool tak_handle_command(const char* cmd, char* reply, size_t reply_max, uint32_t now) {
  if (cmd == nullptr || memcmp(cmd, "tak", 3) != 0) return false;
  if (cmd[3] != 0 && cmd[3] != ' ') return false;

  const char* a = cmd + 3;
  while (*a == ' ') a++;
  if (*a == 0) { takStatus(reply, reply_max); return true; }

  bool b;
  if (onOff(a, &b)) {
    _on = b;
    takApply();
    if (_on && _host[0] == 0) snprintf(reply, reply_max, "OK - aan, maar nog geen host (tak host <ip>[:poort])");
    else takStatus(reply, reply_max);
    return true;
  }

  if (strncmp(a, "host ", 5) == 0) {
    char h[24];
    const char* v = a + 5;
    while (*v == ' ') v++;
    strncpy(h, v, sizeof(h) - 1);
    h[sizeof(h) - 1] = 0;
    char* c = strchr(h, ':');
    if (c) {
      *c = 0;
      int p = atoi(c + 1);
      if (p <= 0 || p > 65535) { snprintf(reply, reply_max, "Err - poort 1-65535"); return true; }
      _port = (uint16_t)p;
    }
    if (!validIp(h)) { snprintf(reply, reply_max, "Err - IPv4-adres verwacht (geen hostnaam)"); return true; }
    strncpy(_host, h, sizeof(_host) - 1);
    _host[sizeof(_host) - 1] = 0;
    takApply();
    takStatus(reply, reply_max);
    return true;
  }

  if (strncmp(a, "port ", 5) == 0) {
    int p = atoi(a + 5);
    if (p <= 0 || p > 65535) { snprintf(reply, reply_max, "Err - poort 1-65535"); return true; }
    _port = (uint16_t)p;
    takApply();
    takStatus(reply, reply_max);
    return true;
  }

  if (strncmp(a, "adverts ", 8) == 0) {
    if (!onOff(a + 8, &b)) { snprintf(reply, reply_max, "Err - tak adverts on|off"); return true; }
    _adverts = b;
    takSave();
    takStatus(reply, reply_max);
    return true;
  }

  if (strncmp(a, "stale ", 6) == 0) {
    int s = atoi(a + 6);
    if (s < TAK_STALE_MIN || s > TAK_STALE_MAX) {
      snprintf(reply, reply_max, "Err - stale %d-%d minuten", TAK_STALE_MIN, TAK_STALE_MAX);
      return true;
    }
    _stale_min = (uint16_t)s;
    takSave();
    takStatus(reply, reply_max);
    return true;
  }

  if (strncmp(a, "test", 4) == 0 && (a[4] == 0 || a[4] == ' ')) {
    double lat = 0, lon = 0;
    if (sscanf(a + 4, " %lf %lf", &lat, &lon) != 2) {
      snprintf(reply, reply_max, "tak test <lat> <lon>  (zet een testpunt op de kaart)");
      return true;
    }
    if (!tak_enabled())   { snprintf(reply, reply_max, "Err - TAK staat uit of heeft geen host"); return true; }
    if (now < TIME_FLOOR) { snprintf(reply, reply_max, "Err - klok niet gesynct, CoT zou al vervallen zijn"); return true; }
    static const uint8_t test_pub[6] = { 0x7e, 0x57, 0x7a, 0x4b, 0x00, 0x01 };
    tak_queue_pos(test_pub, "MeshUptime-test", TAK_SRC_TEST, ADV_TYPE_NONE,
                  lat, lon, -1, INT16_MIN, -1, false, now);
    snprintf(reply, reply_max, "OK - testpunt %.5f,%.5f in de rij", lat, lon);
    return true;
  }

  snprintf(reply, reply_max, "tak [on|off|host <ip>[:poort]|port <n>|adverts on|off|stale <min>|test <lat> <lon>]");
  return true;
}
