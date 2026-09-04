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
  _last_refresh_seen = 0;
  _status_ok = 0;
  _status_fail = 0;
  _status_wait = false;
  _status_got = false;
  _clockfix_ok = 0;
  _clockfix_fail = 0;
  _clockfix_last[0] = 0;
  StrHelper::strncpy(_note, "nog niet gepold", sizeof(_note));
}

void Poller::begin(fs::FS* fs, PushTask* push, RepeaterCli* rcli) {
  _fs = fs; _push = push; _rcli = rcli;
  /* De statusuitslag komt via RepeaterCli terug; de CLI-antwoorden lopen via de
   * result-callback die main_room al zet. Twee wegen, twee endpoints. */
  if (_rcli) _rcli->setStatsCallback(Poller::statsThunk, this);
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
  /* Uitzetten midden in een statusronde: de afsluit-toets in loop() draait dan
   * niet meer, en een blijvend gezette vlag zou bij het weer aanzetten een
   * mislukking tellen die niet bij die ronde hoort. De ronde zelf loopt in
   * RepeaterCli gewoon af (die kent geen aan/uit) -- er gaat alleen geen nieuwe
   * meer starten. */
  if (!_on) { _status_wait = false; _status_got = false; }
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
  int got_clockfix = 0;

  /* --- refresh: STATUSVERZOEKEN (v2.7.0). Elke prefix wordt een eigen sessie:
   * login + REQ_TYPE_GET_STATUS. Ze gaan in DEZELFDE wachtrij als de
   * instellingenopvragingen, zodat er nooit twee sessies tegelijk lopen en de
   * clear-on-read-regel voor beide geldt. --- */
  _last_refresh_seen = 0;
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
        _last_refresh_seen++;
        if (!pushPending(hex, "", PEND_STATUS)) {
          /* pushPending telt de overloop zelf (_dropped) en logt hem. Hier niets
           * extra's: clear-on-read betekent dat dit verzoek weg is, en dat staat
           * dan in het logboek en in de teller. */
        }
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
        q = a2;

        /* "cmd:clockfix" is GEEN CLI-tekst maar een eigen job (zie
         * POLLER_CLOCKFIX_PARAM). Hem in de CSV laten staan zou het woord
         * "clockfix" naar de repeater sturen, en dat kent die niet. */
        if (strcmp(one, POLLER_CLOCKFIX_PARAM) == 0) {
          if (pushPending(prefix, "", PEND_CLOCKFIX)) got_clockfix++;
          continue;
        }

        size_t l = strlen(one);
        if (o + l + 2 < sizeof(csv)) {
          if (o > 0) csv[o++] = ',';
          memcpy(csv + o, one, l); o += l;
        }
      }
      csv[o] = 0;

      if (o > 0) {
        if (pushPending(prefix, csv, PEND_SETTINGS)) got_settings++;
        /* pushPending telt zelf de overloop (_dropped). */
      }
      p = rb + 1;   // door naar de volgende settings-ingang
    }
  }

  snprintf(_note, sizeof(_note), "gepold: %d instellingen, %d status, %d klok",
           got_settings, _last_refresh_seen, got_clockfix);
  MESH_DEBUG_PRINTLN("Poller: %s", _note);
}

void Poller::pollThunk(void* ctx, const char* body) {
  static_cast<Poller*>(ctx)->onPollBody(body);
}

bool Poller::pushPending(const char* prefix, const char* params_csv, PendKind kind) {
  if (_pending_count >= POLLER_PENDING_MAX) {
    _dropped++;
    MESH_DEBUG_PRINTLN("Poller: wachtrij vol, verzoek voor %s VERVALLEN (clear-on-read; verloren: %lu)",
                       prefix, (unsigned long)_dropped);
    return false;
  }
  Pending& e = _pending[(_pending_head + _pending_count) % POLLER_PENDING_MAX];
  StrHelper::strncpy(e.prefix, prefix, sizeof(e.prefix));
  StrHelper::strncpy(e.params, params_csv ? params_csv : "", sizeof(e.params));
  e.kind = kind;
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

  /* STATUSVERZOEK? Dan een heel ander vervolg na dezelfde login: geen CLI-tekst
   * maar een REQ_TYPE_GET_STATUS, en geen per-param-nullen bij mislukking. */
  if (e.kind == PEND_STATUS || e.kind == PEND_CLOCKFIX) {
    Pending copy = e;                 /* kopie: we halen hem hieronder uit de ring */
    const PendKind k = e.kind;
    _pending_head = (uint8_t)((_pending_head + 1) % POLLER_PENDING_MAX);
    _pending_count--;
    _processed++;
    if (k == PEND_STATUS) startStatus(copy); else startClockFix(copy);
    return;
  }

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
 * Een STATUSVERZOEK starten (v2.7.0)
 *
 * Lukt het niet, dan MELDEN WE NIETS. Bij een instellingenopvraging is een
 * null-antwoord informatie ("gevraagd, geen antwoord"), want de server houdt per
 * parameter bij wat er gevraagd is. Hier gaat het naar /api/v1/ingest, en dat
 * neemt METINGEN aan: een lege of half gevulde meting zou daar als echte waarde in
 * de reeksen en grafieken belanden. Dus alleen een logregel en een teller.
 * ------------------------------------------------------------------------ */
void Poller::startStatus(const Pending& e) {
  const char* pass = passwordFor(e.prefix);
  if (pass == nullptr) {
    _status_fail++;
    snprintf(_note, sizeof(_note), "%s: statusverzoek zonder wachtwoord, niets gemeld", e.prefix);
    MESH_DEBUG_PRINTLN("Poller: %s", _note);
    return;
  }

  /* keyFor() en niet e.prefix: staat de VOLLE 64-hex sleutel in de doeltabel, dan
   * gebruiken we die. Anders moet RepeaterCli de prefix uit de buurtlijst oplossen,
   * en dat lukt alleen als deze node ooit een advert van dat doel hoorde. Zelfde
   * keuze als bij een settings-job (zie startNextPending). */
  RepeaterCli::Enq r = _rcli->queueStatus(keyFor(e.prefix), pass);
  if (r == RepeaterCli::RCLI_OK) {
    _status_wait = true;
    _status_got  = false;
    snprintf(_note, sizeof(_note), "%s: statusronde gestart", e.prefix);
  } else if (r == RepeaterCli::RCLI_BAD_KEY) {
    _status_fail++;
    snprintf(_note, sizeof(_note), "%s: sleutel onbekend (nooit gehoord?); status niet gevraagd",
             e.prefix);
    MESH_DEBUG_PRINTLN("Poller: %s", _note);
  } else {
    _status_fail++;
    snprintf(_note, sizeof(_note), "%s: statusronde NIET gestart (rc=%d)", e.prefix, (int)r);
    MESH_DEBUG_PRINTLN("Poller: %s", _note);
  }
}

/* De uitslag van een geslaagde, PLAUSIBELE statusronde. RepeaterCli roept dit
 * alleen aan als het antwoord volledig ontleed en getoetst is -- er komt hier dus
 * nooit een half gevulde meting binnen. Alleen doorzetten naar de PushTask-ring
 * (kopieren, geen I/O: dit draait in de ontvangstlus van de mesh). */
void Poller::onStats(const char* pubkey_hex12, const RepeaterStatus& st) {
  _status_ok++;
  _status_got = true;
  if (_push) _push->queueRepeaterStats(pubkey_hex12, st);
  snprintf(_note, sizeof(_note), "%s: status gemeld (%u mV, uptime %lu d)",
           pubkey_hex12, (unsigned)st.batt_milli_volts,
           (unsigned long)(st.total_up_time_secs / 86400UL));
  MESH_DEBUG_PRINTLN("Poller: %s", _note);
}

void Poller::statsThunk(void* ctx, const char* pubkey_hex12, const RepeaterStatus& st) {
  static_cast<Poller*>(ctx)->onStats(pubkey_hex12, st);
}

/* ------------------------------------------------------------------------
 * De KLOK rechtzetten (v2.8.0)
 *
 * Deze job kan MINUTEN duren: bij een node die vóórloopt zit er een clkreboot en
 * een herstart in. Dat is aanvaard -- de job houdt de enige sessie bezet, dus een
 * ander pollerverzoek wacht (prima), maar de BEWAKING loopt gewoon door: alles
 * gebeurt stapsgewijs vanuit loop() en er wordt nergens gewacht.
 * ------------------------------------------------------------------------ */
void Poller::startClockFix(const Pending& e) {
  const char* pass = passwordFor(e.prefix);
  if (pass == nullptr) {
    _clockfix_fail++;
    StrHelper::strncpy(_clockfix_last, "MISLUKT - geen wachtwoord voor dit doel",
                       sizeof(_clockfix_last));
    /* Wél terugmelden onder "cmd:clockfix": de beheerpagina hoort te zien waarom er
     * niets gebeurde, niet een knop die stil niets doet. */
    if (_push) _push->queueRepeaterSetting(e.prefix, POLLER_CLOCKFIX_PARAM, _clockfix_last);
    snprintf(_note, sizeof(_note), "%s: klok-job zonder wachtwoord", e.prefix);
    MESH_DEBUG_PRINTLN("Poller: %s", _note);
    return;
  }

  RepeaterCli::Enq r = _rcli->queueClockFix(keyFor(e.prefix), pass);
  if (r == RepeaterCli::RCLI_OK) {
    snprintf(_note, sizeof(_note), "%s: klok-job gestart", e.prefix);
    MESH_DEBUG_PRINTLN("Poller: %s", _note);
    return;
  }

  _clockfix_fail++;
  if (r == RepeaterCli::RCLI_BAD_KEY) {
    StrHelper::strncpy(_clockfix_last,
                       "MISLUKT - sleutel onbekend; geef de volle 64-hex in de doeltabel",
                       sizeof(_clockfix_last));
  } else {
    snprintf(_clockfix_last, sizeof(_clockfix_last), "MISLUKT - job niet gestart (rc=%d)", (int)r);
  }
  if (_push) _push->queueRepeaterSetting(e.prefix, POLLER_CLOCKFIX_PARAM, _clockfix_last);
  snprintf(_note, sizeof(_note), "%s: klok-job niet gestart", e.prefix);
  MESH_DEBUG_PRINTLN("Poller: %s", _note);
}

/* De uitkomst van een klok-job, voor de eigen boekhouding. Het antwoord zelf is al
 * onderweg naar MeshManager via de gewone result-callback; dit is wat /poller.json
 * en het seriële logboek erover kunnen zeggen. */
void Poller::noteClockFix(const char* answer) {
  if (answer == nullptr) answer = "";
  StrHelper::strncpy(_clockfix_last, answer, sizeof(_clockfix_last));
  if (strncmp(answer, "OK", 2) == 0) _clockfix_ok++; else _clockfix_fail++;
  snprintf(_note, sizeof(_note), "klok: %.70s", answer);
  MESH_DEBUG_PRINTLN("Poller: klok-job klaar -> %s", answer);
}

/* ------------------------------------------------------------------------
 * loop
 * ------------------------------------------------------------------------ */
void Poller::loop() {
  if (!_on || _push == nullptr) return;

  /* Een gestarte statusronde afsluiten zodra RepeaterCli weer vrij is. Kwam er geen
   * meting uit, dan is dat een MISLUKTE ronde en die hoort geteld te worden -- zie
   * de uitleg bij _status_wait. De reden zelf staat in RepeaterCli::error() en in
   * het seriële logboek; hier houden we alleen het aantal bij. */
  if (_status_wait && _rcli != nullptr && !_rcli->busy()) {
    if (!_status_got) {
      _status_fail++;
      snprintf(_note, sizeof(_note), "statusronde zonder meting: %s", _rcli->error());
      MESH_DEBUG_PRINTLN("Poller: %s", _note);
    }
    _status_wait = false;
  }

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
