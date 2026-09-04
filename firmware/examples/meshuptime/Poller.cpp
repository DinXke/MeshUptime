#include "Poller.h"
#include "PushTask.h"

/* Poller -- zie de uitgebreide uitleg in Poller.h. Dit bestand is de mechaniek:
 * timing, het ontleden van de poll, de doeltabel en het starten van de jobs. */

/* ------------------------------------------------------------------------
 * Kleine bestandshelper: één regel lezen, '\r'/'\n' eraf. -1 = einde.
 * Eigen kopie (MonitorStore's readLine is daar file-static); klein genoeg.
 * ------------------------------------------------------------------------ */
static int readLine(fs::File& f, char* dest, size_t max) {
  if (max == 0) return -1;
  size_t i = 0;
  bool any = false;
  while (f.available()) {
    int c = f.read();
    if (c < 0) break;
    any = true;
    if (c == '\n') break;
    if (c == '\r') continue;
    if (i < max - 1) dest[i++] = (char)c;
  }
  dest[i] = 0;
  return any ? (int)i : -1;
}

/* ------------------------------------------------------------------------
 * param -> commando (exact zoals RepeaterCli en de oude HA-pusher):
 *   "cmd:X" -> X letterlijk ;  anders P -> "get P".
 * Hier apart zodat de queue-zeef (paramAllowedFromQueue) op het ECHTE commando
 * kan toetsen en niet op de parameternaam.
 * ------------------------------------------------------------------------ */
static void deriveCommand(const char* param, char* out, size_t out_len) {
  while (*param == ' ') param++;
  if (strncmp(param, "cmd:", 4) == 0) {
    const char* c = param + 4;
    while (*c == ' ') c++;
    StrHelper::strncpy(out, c, out_len);
  } else {
    snprintf(out, out_len, "get %s", param);
  }
}

static bool startsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

/* Mag dit commando uit de WACHTRIJ de lucht in? Reads (get ...) altijd. Geweigerd
 * wordt de "brick"-set: alles wat een node op een dak onbereikbaar of
 * onherstelbaar kan maken. Op afstand is daar geen bevestiging voor -- vandaar dat
 * ze via /cli/remote een expliciete confirm eisen, en uit de wachtrij (waar geen
 * confirm bestaat) helemaal niet mogen. Andere muterende commando's (set name/tx,
 * filter count) mogen wel, maar worden door RepeaterCli niet herhaald. */
bool Poller::paramAllowedFromQueue(const char* param) {
  char cmd[RCLI_CMD_MAX + 1];
  deriveCommand(param, cmd, sizeof(cmd));
  const char* c = cmd;
  while (*c == ' ') c++;

  if (strstr(c, "prv.key") != nullptr) return false;   // privésleutel: nooit

  static const char* const BRICK[] = {
    "clkreboot", "reboot", "erase", "poweroff", "shutdown",
    "start ota", "start", "set radio", "set freq", "factory",
    "rollback", "flash", "ota", nullptr
  };
  for (int i = 0; BRICK[i] != nullptr; i++) {
    if (startsWith(c, BRICK[i])) return false;
  }
  return true;
}

/* ------------------------------------------------------------------------
 * Boekhouding
 * ------------------------------------------------------------------------ */
void Poller::reset() {
  _fs = nullptr; _push = nullptr; _rcli = nullptr;
  _on = false;
  _poll_secs = POLL_SECS_DEFAULT;
  _next_poll = 0;
  _last_poll = 0;
  _ntargets = 0;
  _default_pass[0] = 0;
  memset(_targets, 0, sizeof(_targets));
  memset(_pending, 0, sizeof(_pending));
  _pending_head = 0;
  _pending_count = 0;
  _processed = 0;
  _dropped = 0;
  _last_refresh_dropped = 0;
  StrHelper::strncpy(_note, "nog niet gepold", sizeof(_note));
}

void Poller::begin(fs::FS* fs, PushTask* push, RepeaterCli* rcli) {
  _fs = fs; _push = push; _rcli = rcli;
  loadConfig();
  loadTargets();
  /* De eerste poll na een korte genadetijd (wifi/tijd/advert eerst). */
  _next_poll = millis() + POLLER_FIRST_DELAY_MS;
}

/* ------------------------------------------------------------------------
 * Config + doeltabel persistentie
 * ------------------------------------------------------------------------ */
void Poller::loadConfig() {
  if (_fs == nullptr || !_fs->exists(POLLER_CFG_PATH)) return;
  fs::File f = _fs->open(POLLER_CFG_PATH, "r");
  if (!f) return;
  char line[16];
  if (readLine(f, line, sizeof(line)) >= 0) _on = (line[0] == '1');
  if (readLine(f, line, sizeof(line)) >= 0) {
    long v = strtol(line, nullptr, 10);
    if (v < POLL_SECS_MIN) v = POLL_SECS_MIN;
    if (v > POLL_SECS_MAX) v = POLL_SECS_MAX;
    _poll_secs = (uint16_t)v;
  }
  f.close();
}

void Poller::saveConfig() {
  if (_fs == nullptr) return;
  fs::File f = _fs->open(POLLER_CFG_PATH, "w");
  if (!f) return;
  f.printf("%d\n%u\n", _on ? 1 : 0, (unsigned)_poll_secs);
  f.close();
}

void Poller::loadTargets() {
  _ntargets = 0;
  _default_pass[0] = 0;
  if (_fs == nullptr || !_fs->exists(POLLER_TARGETS_PATH)) return;
  fs::File f = _fs->open(POLLER_TARGETS_PATH, "r");
  if (!f) return;

  /* Regel 1 = het standaardwachtwoord (mag leeg zijn). */
  char line[POLLER_PREFIX_MAX + RCLI_PASS_MAX + 4];
  if (readLine(f, line, sizeof(line)) >= 0) StrHelper::strncpy(_default_pass, line, sizeof(_default_pass));

  /* Daarna "prefix wachtwoord" per regel. */
  while (readLine(f, line, sizeof(line)) >= 0 && _ntargets < POLLER_MAX_TARGETS) {
    char* sp = strchr(line, ' ');
    if (sp == nullptr) continue;
    *sp = 0;
    const char* pass = sp + 1;
    if (line[0] == 0 || pass[0] == 0) continue;
    StrHelper::strncpy(_targets[_ntargets].prefix, line, sizeof(_targets[_ntargets].prefix));
    StrHelper::strncpy(_targets[_ntargets].pass, pass, sizeof(_targets[_ntargets].pass));
    _ntargets++;
  }
  f.close();
}

void Poller::saveTargets() {
  if (_fs == nullptr) return;
  fs::File f = _fs->open(POLLER_TARGETS_PATH, "w");
  if (!f) return;
  f.printf("%s\n", _default_pass);
  for (int i = 0; i < _ntargets; i++) {
    f.printf("%s %s\n", _targets[i].prefix, _targets[i].pass);
  }
  f.close();
}

/* ------------------------------------------------------------------------
 * Config-setters (web-GUI)
 * ------------------------------------------------------------------------ */
void Poller::setEnabled(bool on) {
  if (_on == on) return;
  _on = on;
  saveConfig();
  if (_on) _next_poll = millis() + 1000;   // net aangezet: bijna meteen pollen
}

void Poller::setPollSecs(uint16_t s) {
  if (s < POLL_SECS_MIN) s = POLL_SECS_MIN;
  if (s > POLL_SECS_MAX) s = POLL_SECS_MAX;
  _poll_secs = s;
  saveConfig();
}

/* ------------------------------------------------------------------------
 * Doeltabel (web-GUI)
 * ------------------------------------------------------------------------ */
static bool validHexPrefix(const char* p, size_t& len_out) {
  size_t n = strlen(p);
  if (n < 12 || n > 64 || (n & 1)) return false;
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  len_out = n;
  return true;
}

/* Kleine-letters-kopie van een hex-prefix: de wachtrij levert kleine letters, dus
 * de vergelijking moet daar niet op struikelen. */
static void lowerHex(const char* in, char* out, size_t out_len) {
  size_t i = 0;
  for (; in[i] && i < out_len - 1; i++) {
    char c = in[i];
    out[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
  }
  out[i] = 0;
}

bool Poller::setTarget(const char* prefix_hex, const char* password) {
  if (prefix_hex == nullptr) return false;
  size_t plen;
  char prefix[POLLER_PREFIX_MAX];
  lowerHex(prefix_hex, prefix, sizeof(prefix));
  if (!validHexPrefix(prefix, plen)) return false;

  /* Leeg wachtwoord = verwijderen. */
  if (password == nullptr || password[0] == 0) return delTarget(prefix);
  if (strlen(password) > RCLI_PASS_MAX - 1) return false;

  /* Bestaande prefix bijwerken? */
  for (int i = 0; i < _ntargets; i++) {
    if (strcmp(_targets[i].prefix, prefix) == 0) {
      StrHelper::strncpy(_targets[i].pass, password, sizeof(_targets[i].pass));
      saveTargets();
      return true;
    }
  }
  if (_ntargets >= POLLER_MAX_TARGETS) return false;   // vol
  StrHelper::strncpy(_targets[_ntargets].prefix, prefix, sizeof(_targets[_ntargets].prefix));
  StrHelper::strncpy(_targets[_ntargets].pass, password, sizeof(_targets[_ntargets].pass));
  _ntargets++;
  saveTargets();
  return true;
}

bool Poller::delTarget(const char* prefix_hex) {
  if (prefix_hex == nullptr) return false;
  char prefix[POLLER_PREFIX_MAX];
  lowerHex(prefix_hex, prefix, sizeof(prefix));
  for (int i = 0; i < _ntargets; i++) {
    if (strcmp(_targets[i].prefix, prefix) == 0) {
      for (int j = i; j + 1 < _ntargets; j++) _targets[j] = _targets[j + 1];
      _ntargets--;
      saveTargets();
      return true;
    }
  }
  return false;
}

void Poller::setDefaultPass(const char* password) {
  if (password == nullptr) password = "";
  if (strlen(password) > RCLI_PASS_MAX - 1) return;
  StrHelper::strncpy(_default_pass, password, sizeof(_default_pass));
  saveTargets();
}

bool Poller::targetAt(int idx, char* prefix_out, size_t out_len) const {
  if (idx < 0 || idx >= _ntargets) { if (out_len) prefix_out[0] = 0; return false; }
  StrHelper::strncpy(prefix_out, _targets[idx].prefix, out_len);
  return true;
}

/* prefix -> wachtwoord: een tabelingang waarvan de opgeslagen prefix een prefix
 * IS van (of gelijk aan) de gevraagde sleutel, anders de standaard. De
 * prefix-match werkt beide kanten op: MeshManager stuurt 12 hex, maar iemand kan
 * de volle 64 hebben opgeslagen -- dan moet die 64-ingang de 12-vraag dekken en
 * omgekeerd. We vergelijken daarom op de KORTSTE van de twee lengtes. */
const char* Poller::passwordFor(const char* prefix_hex) const {
  char want[POLLER_PREFIX_MAX];
  lowerHex(prefix_hex, want, sizeof(want));
  size_t wl = strlen(want);
  for (int i = 0; i < _ntargets; i++) {
    size_t tl = strlen(_targets[i].prefix);
    size_t n = wl < tl ? wl : tl;
    if (n >= 12 && strncmp(_targets[i].prefix, want, n) == 0) return _targets[i].pass;
  }
  return _default_pass[0] ? _default_pass : nullptr;
}

/* prefix -> de sleutel om mee te WERKEN: de volle 64 hex uit de doeltabel als
 * die er staat, anders de gevraagde prefix zelf. MeshManager stuurt altijd 12
 * hex, en RepeaterCli lost die alleen op via de buurtlijst -- die na elke
 * herstart leeg is tot de repeater weer adverteert (Jessa: om de ~2 uur). Wie de
 * volle sleutel in de doeltabel zet, hoort daar niet op te hoeven wachten; dat
 * is precies waarom die tabel 64 tekens aankan. */
const char* Poller::keyFor(const char* prefix_hex) const {
  char want[POLLER_PREFIX_MAX];
  lowerHex(prefix_hex, want, sizeof(want));
  size_t wl = strlen(want);
  for (int i = 0; i < _ntargets; i++) {
    size_t tl = strlen(_targets[i].prefix);
    size_t n = wl < tl ? wl : tl;
    if (n >= 12 && tl == 64 && strncmp(_targets[i].prefix, want, n) == 0) return _targets[i].prefix;
  }
  return prefix_hex;
}

/* ------------------------------------------------------------------------
 * De poll ontleden
 *
 * Een tolerante, met de hand geschreven scanner voor de bekende vorm
 * {"refresh":[...],"settings":[{"prefix":"..","params":["..",..]},..]}. Geen
 * JSON-lib (geen nieuwe library, en de vorm is vast). refresh staat VÓÓR settings
 * (routes_api.commands), dus zoeken naar "prefix" NÁ "settings" pikt nooit een
 * refresh-veld mee.
 * ------------------------------------------------------------------------ */

/* Lees een JSON-string vanaf de eerste " op of na p. out krijgt de inhoud
 * (met \" en \\ ontsnapt terug), *after wijst achter de sluit-". nullptr als er
 * geen string meer is vóór 'stop'. */
static const char* readQuoted(const char* p, const char* stop, char* out, size_t out_len,
                              const char** after) {
  while (p < stop && *p != '"') p++;
  if (p >= stop || *p != '"') return nullptr;
  p++;   // achter de open-"
  size_t o = 0;
  while (p < stop && *p != '"') {
    char c = *p;
    if (c == '\\' && p + 1 < stop) { p++; c = *p; }   // \" of \\ -> letterlijk
    if (o < out_len - 1) out[o++] = c;
    p++;
  }
  out[o] = 0;
  if (p < stop && *p == '"') p++;
  if (after) *after = p;
  return out;
}

void Poller::onPollBody(const char* body) {
  _last_poll = millis();
  const char* end = body + strlen(body);
  int got_settings = 0;

  /* --- refresh: NIET ondersteund; tellen en logboeken, dan laten vallen. --- */
  _last_refresh_dropped = 0;
  const char* rp = strstr(body, "\"refresh\"");
  if (rp) {
    rp = strchr(rp, '[');
    if (rp) {
      rp++;
      const char* rend = strchr(rp, ']');
      if (rend == nullptr) rend = end;
      const char* q = rp;
      char hex[POLLER_PREFIX_MAX];
      while (q < rend) {
        const char* after = nullptr;
        if (readQuoted(q, rend, hex, sizeof(hex), &after) == nullptr) break;
        _last_refresh_dropped++;
        MESH_DEBUG_PRINTLN("Poller: refresh-verzoek voor %s NIET ondersteund (statusverzoek); genegeerd", hex);
        q = after;
      }
    }
  }

  /* --- settings: elk {"prefix":..,"params":[..]} in _pending. --- */
  const char* sp = strstr(body, "\"settings\"");
  if (sp) {
    const char* p = sp;
    while (true) {
      const char* pk = strstr(p, "\"prefix\"");
      if (pk == nullptr) break;
      char prefix[POLLER_PREFIX_MAX];
      const char* after = nullptr;
      if (readQuoted(pk + 8, end, prefix, sizeof(prefix), &after) == nullptr) break;

      /* De params-array van DEZE ingang. Hij hoort na de prefix te komen; zoek de
       * eerstvolgende "params" en zijn '['. */
      const char* mk = strstr(after, "\"params\"");
      if (mk == nullptr) break;
      const char* lb = strchr(mk, '[');
      if (lb == nullptr) break;
      const char* rb = strchr(lb, ']');
      if (rb == nullptr) rb = end;

      char csv[RCLI_JOB_BUF];
      size_t o = 0;
      const char* q = lb + 1;
      while (q < rb) {
        char one[RCLI_PARAM_MAX];
        const char* a2 = nullptr;
        if (readQuoted(q, rb, one, sizeof(one), &a2) == nullptr) break;
        size_t l = strlen(one);
        if (o + l + 2 < sizeof(csv)) {
          if (o > 0) csv[o++] = ',';
          memcpy(csv + o, one, l); o += l;
        }
        q = a2;
      }
      csv[o] = 0;

      if (o > 0) {
        if (pushPending(prefix, csv)) got_settings++;
        /* pushPending telt zelf de overloop (_dropped). */
      }
      p = rb + 1;   // door naar de volgende settings-ingang
    }
  }

  snprintf(_note, sizeof(_note), "gepold: %d verzoek(en), %d refresh genegeerd",
           got_settings, _last_refresh_dropped);
  MESH_DEBUG_PRINTLN("Poller: %s", _note);
}

void Poller::pollThunk(void* ctx, const char* body) {
  static_cast<Poller*>(ctx)->onPollBody(body);
}

bool Poller::pushPending(const char* prefix, const char* params_csv) {
  if (_pending_count >= POLLER_PENDING_MAX) {
    _dropped++;
    MESH_DEBUG_PRINTLN("Poller: wachtrij vol, verzoek voor %s VERVALLEN (clear-on-read; verloren: %lu)",
                       prefix, (unsigned long)_dropped);
    return false;
  }
  Pending& e = _pending[(_pending_head + _pending_count) % POLLER_PENDING_MAX];
  StrHelper::strncpy(e.prefix, prefix, sizeof(e.prefix));
  StrHelper::strncpy(e.params, params_csv, sizeof(e.params));
  _pending_count++;
  return true;
}

/* ------------------------------------------------------------------------
 * Een wachtend verzoek starten
 * ------------------------------------------------------------------------ */
void Poller::startNextPending() {
  if (_pending_count == 0) return;
  if (_rcli == nullptr || _rcli->busy()) return;   // EEN sessie tegelijk

  Pending& e = _pending[_pending_head];

  const char* pass = passwordFor(e.prefix);

  /* De params splitsen in "veilig" (mag de lucht in) en "geweigerd/geen
   * wachtwoord" (meteen als null terug, zodat MeshManager "gevraagd, geen
   * antwoord" ziet i.p.v. stilte). */
  char safe[RCLI_JOB_BUF]; size_t so = 0; safe[0] = 0;
  int refused = 0, nulled = 0;

  const char* q = e.params;
  while (*q) {
    char one[RCLI_PARAM_MAX];
    size_t l = 0;
    while (q[l] && q[l] != ',' && l < sizeof(one) - 1) { one[l] = q[l]; l++; }
    one[l] = 0;
    q += l;
    if (*q == ',') q++;
    if (one[0] == 0) continue;

    bool allowed = paramAllowedFromQueue(one);
    if (!allowed) {
      refused++;
      if (_push) _push->queueRepeaterSetting(e.prefix, one, nullptr);   // geweigerd -> null
      MESH_DEBUG_PRINTLN("Poller: param '%s' voor %s GEWEIGERD uit de wachtrij (gevaarlijk); null gepusht",
                         one, e.prefix);
      continue;
    }
    if (pass == nullptr) {
      /* Geen wachtwoord: alles als null terug (we kunnen niet inloggen). */
      nulled++;
      if (_push) _push->queueRepeaterSetting(e.prefix, one, nullptr);
      continue;
    }
    if (so + l + 2 < sizeof(safe)) {
      if (so > 0) safe[so++] = ',';
      memcpy(safe + so, one, l); so += l;
    }
  }
  safe[so] = 0;

  /* Uit de wachtrij halen: verwerkt of geweigerd, in beide gevallen weg. */
  _pending_head = (uint8_t)((_pending_head + 1) % POLLER_PENDING_MAX);
  _pending_count--;
  _processed++;

  if (pass == nullptr) {
    snprintf(_note, sizeof(_note), "%s: geen wachtwoord, %d param(s) als null gemeld", e.prefix, nulled);
    MESH_DEBUG_PRINTLN("Poller: %s", _note);
    return;
  }
  if (so == 0) {
    snprintf(_note, sizeof(_note), "%s: %d param(s) geweigerd, niets te sturen", e.prefix, refused);
    return;
  }

  /* De volle sleutel uit de doeltabel als die er is: dan hangt de sessie niet
   * van een gehoorde advert af (zie keyFor). */
  RepeaterCli::Enq r = _rcli->queueJob(keyFor(e.prefix), pass, safe);
  if (r == RepeaterCli::RCLI_OK) {
    snprintf(_note, sizeof(_note), "%s: sessie gestart (%s%s)", e.prefix, safe,
             refused ? ", enkele geweigerd" : "");
  } else if (r == RepeaterCli::RCLI_BAD_KEY) {
    /* Prefix niet op te lossen (nooit een advert gehoord, geen volle sleutel):
     * alles als null terug zodat de beheerpagina het ziet. */
    const char* p2 = safe;
    while (*p2) {
      char one[RCLI_PARAM_MAX]; size_t l = 0;
      while (p2[l] && p2[l] != ',' && l < sizeof(one) - 1) { one[l] = p2[l]; l++; }
      one[l] = 0; p2 += l; if (*p2 == ',') p2++;
      if (one[0] && _push) _push->queueRepeaterSetting(e.prefix, one, nullptr);
    }
    snprintf(_note, sizeof(_note), "%s: sleutel onbekend (nooit gehoord?); null gemeld", e.prefix);
    MESH_DEBUG_PRINTLN("Poller: %s", _note);
  } else {
    snprintf(_note, sizeof(_note), "%s: sessie NIET gestart (rc=%d)", e.prefix, (int)r);
    MESH_DEBUG_PRINTLN("Poller: %s", _note);
  }
}

/* ------------------------------------------------------------------------
 * loop
 * ------------------------------------------------------------------------ */
void Poller::loop() {
  if (!_on || _push == nullptr) return;

  /* Een wachtend verzoek starten zodra RepeaterCli vrij is. Dit VOOR het pollen,
   * zodat de wachtrij leegloopt voordat we nieuwe ophalen (clear-on-read: pollen
   * met een volle wachtrij zou verzoeken meteen kwijtmaken). */
  startNextPending();

  const unsigned long now = millis();
  if ((long)(now - _next_poll) < 0) return;

  /* Alleen pollen als er PLAATS is om te bewaren wat we ophalen: clear-on-read.
   * Is de wachtrij niet leeg, dan stellen we de poll kort uit i.p.v. verzoeken te
   * riskeren. */
  if (_pending_count > 0) { _next_poll = now + 2000; return; }

  /* De poll aanvragen. requestPoll faalt stil als er al een loopt of als de push
   * uitstaat (geen url); dan proberen we het de volgende ronde weer. De GET zelf
   * vuurt pas als er wifi is en geen dringender push voorligt (PushTask). */
  if (_push->requestPoll(Poller::pollThunk, this)) {
    _next_poll = now + (unsigned long)_poll_secs * 1000UL;
  } else {
    _next_poll = now + 3000;   // kort opnieuw; push bezig of (nog) geen url
  }
}
