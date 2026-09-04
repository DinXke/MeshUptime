#include "RepeaterCli.h"

/* helpers/BaseChatMesh.h definieert dit, maar die header sleept de hele chat-
 * client-basisklasse mee (MAX_CONTACTS-contactentabel). Lokaal herhalen is wat
 * SensorMesh.cpp en RoomMesh.cpp ook doen; alle drie dezelfde naam zodat een grep
 * ze vindt als upstream de waarde ooit wijzigt. */
#ifndef RESP_SERVER_LOGIN_OK
  #define RESP_SERVER_LOGIN_OK   0
#endif

/* ------------------------------------------------------------------------
 * isMutating -- welke opdrachten VERANDEREN iets aan de tegenkant?
 *
 * Twee dingen leunen hierop, allebei omdat een herhaling het commando OPNIEUW
 * uitvoert (zie de valkuil bovenaan de header): de herhaalrem (een muterend
 * commando krijgt EEN poging) en de bevestigings-/weigerregels in de webroute en
 * in de poller.
 *
 * De lijst is BEWUST ruim: "wat niet zeker lezen is, geldt als schrijven". Een
 * leescommando dat per ongeluk als muterend geldt kost hoogstens een gemiste
 * herhaling; een schrijfcommando dat per ongeluk als lezen geldt kan een node op
 * een dak twee keer herstarten. Die twee fouten zijn niet gelijkwaardig.
 * ------------------------------------------------------------------------ */
static bool startsWith(const char* s, const char* prefix) {
  return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool RepeaterCli::isMutating(const char* cmd) {
  if (cmd == nullptr) return true;   // onbekend = het onveilige antwoord
  while (*cmd == ' ') cmd++;

  /* Een KALE 'region' (zonder argument) is een LEES-commando: het toont de
   * huidige regio. Het staat als "cmd:region" in DEFAULT_CLI_PARAMS en wordt dus
   * bij elke sweep gepold; het als muterend behandelen zou betekenen dat een
   * verloren antwoord nooit herhaald wordt. 'region <iets>' zet wél en blijft
   * hieronder muterend. */
  if (strcmp(cmd, "region") == 0) return false;

  static const char* const MUT[] = {
    "set ", "erase", "reboot", "clkreboot", "time ", "password",
    "setperm", "remove", "add", "import", "start", "region ",
    "poweroff", "shutdown", "factory", "clear", "save", "filter set",
    "filter clear", "filter reset", "rollback", "ota", "flash", nullptr
  };
  for (int i = 0; MUT[i] != nullptr; i++) {
    if (startsWith(cmd, MUT[i])) return true;
  }
  if (strcmp(cmd, "set") == 0 || strcmp(cmd, "time") == 0) return true;
  return false;
}

/* ------------------------------------------------------------------------
 * Boekhouding
 * ------------------------------------------------------------------------ */
void RepeaterCli::reset() {
  _state            = RCLI_IDLE;
  _attempt          = 0;
  _cmd_max_attempts = RCLI_MAX_ATTEMPTS;
  _path_len         = OUT_PATH_UNKNOWN;
  _started_at       = 0;
  _next_send        = 0;
  _deadline         = 0;
  _job_n            = 0;
  _job_i            = 0;
  _job_kind         = RCLI_JOB_CLI;
  _status_tag       = 0;
  _answer[0]        = 0;
  _nchunks          = 0;
  _answer_used      = 0;
  _collect_until    = 0;
  _error[0]         = 0;
  _cmd[0]           = 0;
  _cur_param[0]     = 0;
  _pass[0]          = 0;
  _target_hex[0]    = 0;
  _job[0]           = 0;
  memset(_target_pub, 0, sizeof(_target_pub));
  memset(_secret, 0, sizeof(_secret));
}

/* De i-de parameter uit de job. Zie RCLI_JOB_BUF: bij één commando is dat de HELE
 * buffer (een handmatige opdracht mag komma's bevatten), anders het i-de komma-
 * veld. */
bool RepeaterCli::nthParam(uint8_t i, char* out, size_t out_len) const {
  if (i >= _job_n) { if (out_len) out[0] = 0; return false; }

  if (_job_n == 1) { StrHelper::strncpy(out, _job, out_len); return true; }

  const char* p = _job;
  for (uint8_t k = 0; k < i; k++) {
    p = strchr(p, ',');
    if (p == nullptr) { if (out_len) out[0] = 0; return false; }
    p++;
  }
  const char* end = strchr(p, ',');
  size_t n = end ? (size_t)(end - p) : strlen(p);
  if (n >= out_len) n = out_len - 1;
  memcpy(out, p, n);
  out[n] = 0;
  return true;
}

/* ------------------------------------------------------------------------
 * Job starten
 * ------------------------------------------------------------------------ */
RepeaterCli::Enq RepeaterCli::startJob(const char* pubkey_hex, const char* password) {
  if (_mesh == nullptr || _host == nullptr) return RCLI_NO_HOST;
  if (busy()) return RCLI_BUSY;
  if (password == nullptr || strlen(password) > RCLI_PASS_MAX - 1) return RCLI_BAD_ARG;
  if (_job_n == 0 || _job[0] == 0) return RCLI_BAD_ARG;

  const size_t hlen = pubkey_hex ? strlen(pubkey_hex) : 0;
  if (hlen < 12 || hlen > PUB_KEY_SIZE * 2 || (hlen & 1)) return RCLI_BAD_KEY;

  uint8_t given[PUB_KEY_SIZE];
  memset(given, 0, sizeof(given));
  if (!mesh::Utils::fromHex(given, (int)(hlen / 2), pubkey_hex)) return RCLI_BAD_KEY;

  uint8_t full[PUB_KEY_SIZE];
  if (hlen == PUB_KEY_SIZE * 2) {
    memcpy(full, given, PUB_KEY_SIZE);
  } else if (!_host->rcliResolvePubKey(given, (int)(hlen / 2), full)) {
    return RCLI_BAD_KEY;   // prefix onbekend: de aanroeper moet de 64 hex leveren
  }

  memcpy(_target_pub, full, PUB_KEY_SIZE);
  mesh::Utils::toHex(_target_hex, _target_pub, 6);
  StrHelper::strncpy(_pass, password, sizeof(_pass));

  /* Het gedeelde geheim EEN keer -- ECDH is duur en fillSharedSecret() draait
   * straks vanuit de ontvangstlus. */
  const mesh::LocalIdentity& me = _host->rcliUseClientIdentity();
  me.calcSharedSecret(_secret, _target_pub);

  /* Een pad dat we van deze repeater leerden opnieuw gebruiken (RAM-cache): dan
   * hoeft de eerste zending niet het hele mesh wakker te maken. */
  if (_have_cached_path && memcmp(_cached_pub, _target_pub, PUB_KEY_SIZE) == 0) {
    memcpy(_path, _cached_path, sizeof(_path));
    _path_len = _cached_path_len;
  } else {
    _path_len = OUT_PATH_UNKNOWN;
  }

  _state      = RCLI_LOGIN;
  _attempt    = 0;
  _job_i      = 0;
  _answer[0]  = 0;
  _nchunks    = 0;
  _answer_used = 0;
  _error[0]   = 0;
  _started_at = millis();
  _next_send  = _started_at;   // de login mag meteen

  /* Het eerste commando alvast klaarzetten, puur zodat command() al iets zinnigs
   * toont zolang de login loopt (de statuspagina). De zending gebeurt pas na een
   * gelukte login; dit stuurt niets. Een statusjob heeft geen CLI-tekst: daar
   * zetten we alleen een leesbaar label. */
  if (_job_kind == RCLI_JOB_STATUS) {
    StrHelper::strncpy(_cur_param, "status", sizeof(_cur_param));
    StrHelper::strncpy(_cmd, "(statusverzoek)", sizeof(_cmd));
    _cmd_max_attempts = RCLI_MAX_ATTEMPTS;   // een leesactie: gewone drie pogingen
  } else {
    loadCurrentParam();
  }
  return RCLI_OK;
}

RepeaterCli::Enq RepeaterCli::queue(const char* pubkey_hex, const char* password,
                                    const char* command) {
  if (command == nullptr) return RCLI_BAD_ARG;
  while (*command == ' ') command++;
  const size_t clen = strlen(command);
  if (clen == 0 || clen > RCLI_CMD_MAX) return RCLI_BAD_ARG;

  if (busy()) return RCLI_BUSY;   // vóór reset(): een lopende job niet wegvegen
  reset();

  /* Handmatig = een job van EEN, met de opdracht als "cmd:<command>". Zo levert de
   * param->commando-vertaling ("cmd:" -> strip) precies de opdracht op, en gaat het
   * antwoord terug onder "cmd:<command>" -- de vorm die MeshManager al kent. */
  int n = snprintf(_job, sizeof(_job), "cmd:%s", command);
  if (n < 0 || (size_t)n >= sizeof(_job)) return RCLI_BAD_ARG;
  _job_n = 1;

  return startJob(pubkey_hex, password);
}

RepeaterCli::Enq RepeaterCli::queueJob(const char* pubkey_hex, const char* password,
                                       const char* params_csv) {
  if (params_csv == nullptr || params_csv[0] == 0) return RCLI_BAD_ARG;

  if (busy()) return RCLI_BUSY;
  reset();

  StrHelper::strncpy(_job, params_csv, sizeof(_job));

  /* Tellen: komma-velden, met minstens EEN. Boven RCLI_MAX_JOB knippen we -- de
   * server zou nooit meer dan 40 mogen sturen, maar een grens die je zelf trekt is
   * er een die niemand kan overschrijden. */
  uint8_t n = 1;
  for (const char* p = _job; *p; p++) if (*p == ',') n++;
  if (n > RCLI_MAX_JOB) n = RCLI_MAX_JOB;
  _job_n = n;

  return startJob(pubkey_hex, password);
}

/* Een STATUSVERZOEK (v2.7.0). De job is EEN stap ("status"), puur zodat de
 * bestaande job-boekhouding (pogingen, tussenpauzes, bovengrens) ongewijzigd
 * geldt. De parameternaam wordt niet gebruikt -- het antwoord gaat via de
 * stats-callback en niet via de per-param-weg. */
RepeaterCli::Enq RepeaterCli::queueStatus(const char* pubkey_hex, const char* password) {
  if (busy()) return RCLI_BUSY;
  reset();

  StrHelper::strncpy(_job, "status", sizeof(_job));
  _job_n    = 1;
  _job_kind = RCLI_JOB_STATUS;

  return startJob(pubkey_hex, password);
}

/* ------------------------------------------------------------------------
 * De job doorlopen
 * ------------------------------------------------------------------------ */
bool RepeaterCli::loadCurrentParam() {
  if (!nthParam(_job_i, _cur_param, sizeof(_cur_param))) return false;

  /* param -> commando, exact zoals de oude HA-pusher (pusher.py ~393):
   *   "cmd:X" -> X letterlijk ;  anders P -> "get P". */
  const char* p = _cur_param;
  while (*p == ' ') p++;
  if (strncmp(p, "cmd:", 4) == 0) {
    const char* c = p + 4;
    while (*c == ' ') c++;
    StrHelper::strncpy(_cmd, c, sizeof(_cmd));
  } else {
    snprintf(_cmd, sizeof(_cmd), "get %s", p);
  }
  /* Muteert dit lopende commando? Dan EEN poging, geen herhaling. */
  _cmd_max_attempts = isMutating(_cmd) ? 1 : RCLI_MAX_ATTEMPTS;
  return _cmd[0] != 0;
}

void RepeaterCli::deliverCurrent(const char* value) {
  if (_result_fn != nullptr && _cur_param[0] != 0) {
    _result_fn(_result_ctx, _target_hex, _cur_param, value);   // value==nullptr = geen antwoord
  }
}

void RepeaterCli::beginNextCommand() {
  _job_i++;
  if (_job_i >= _job_n) {
    memset(_pass, 0, sizeof(_pass));   // stond hier alleen om te kunnen inloggen
    _state = RCLI_DONE;
    return;
  }
  _state     = RCLI_CMD;
  _attempt   = 0;
  _answer[0] = 0;
  _nchunks   = 0;
  _answer_used = 0;
  if (!loadCurrentParam()) {
    /* Onleesbare parameter (kan niet, maar dan niet stil): als null afleveren en
     * door. */
    deliverCurrent(nullptr);
    beginNextCommand();
  }
}

void RepeaterCli::failJob(const char* why) {
  StrHelper::strncpy(_error, why, sizeof(_error));

  if (_job_kind == RCLI_JOB_STATUS) {
    /* EEN MISLUKTE STATUSRONDE MELDT NIETS. Bij een settings-job is een null-
     * antwoord informatie ("gevraagd, geen antwoord") omdat de server per parameter
     * bijhoudt wat er gevraagd is. Bij een statusverzoek bestaat die boekhouding
     * niet: het endpoint is /api/v1/ingest en dat neemt METINGEN aan. Een lege of
     * half gevulde meting daar neerzetten is erger dan een lege pagina -- die zou
     * als echte waarde in de reeksen en de grafieken belanden. Dus: alleen loggen
     * en laten vallen. */
    MESH_DEBUG_PRINTLN("RepeaterCli: statusronde voor %s mislukt (%s); GEEN metingen gemeld",
                       _target_hex, _error);
    memset(_pass, 0, sizeof(_pass));
    _state = RCLI_FAILED;
    return;
  }

  /* Alle NOG NIET afgehandelde commando's als "geen antwoord" (null) afleveren, ook
   * als de LOGIN faalde en er dus geen enkel commando de deur uit is geweest. Zo
   * ziet MeshManager "gevraagd, geen antwoord" i.p.v. stilte. */
  for (uint8_t i = _job_i; i < _job_n; i++) {
    if (nthParam(i, _cur_param, sizeof(_cur_param))) deliverCurrent(nullptr);
  }
  memset(_pass, 0, sizeof(_pass));
  _state = RCLI_FAILED;
}

/* ------------------------------------------------------------------------
 * Zenden
 * ------------------------------------------------------------------------ */
void RepeaterCli::sendLogin() {
  uint8_t temp[4 + RCLI_PASS_MAX];
  uint32_t now = _mesh->getRTCClock()->getCurrentTimeUnique();
  memcpy(temp, &now, 4);

  int plen = (int)strlen(_pass);
  if (plen > 15) plen = 15;   // zoals BaseChatMesh::sendLogin
  memcpy(&temp[4], _pass, plen);

  const mesh::LocalIdentity& me = _host->rcliUseClientIdentity();
  mesh::Identity dest(_target_pub);
  mesh::Packet* pkt = _mesh->createAnonDatagram(PAYLOAD_TYPE_ANON_REQ, me, dest, _secret,
                                                temp, 4 + plen);
  if (pkt == nullptr) { _next_send = millis() + 1000; return; }   // pool leeg: straks

  _host->rcliSend(pkt, _path, _path_len);
  _attempt++;
  _next_send = millis() + RCLI_MIN_GAP_MS;
  _deadline  = millis() + RCLI_STEP_TIMEOUT_MS;
}

void RepeaterCli::sendCommand() {
  const int tlen = (int)strlen(_cmd);

  uint8_t temp[5 + RCLI_CMD_MAX + 1];
  /* ELKE poging een NIEUWE tijdstempel -- anders ziet de repeater een gelijke
   * tijdstempel als 'is_retry' en antwoordt hij met stilte (zie de valkuil). */
  uint32_t now = _mesh->getRTCClock()->getCurrentTimeUnique();
  memcpy(temp, &now, 4);
  temp[4] = (uint8_t)((_attempt & 3) | (TXT_TYPE_CLI_DATA << 2));
  memcpy(&temp[5], _cmd, tlen + 1);

  _host->rcliUseClientIdentity();   // createDatagram schrijft de afzenderhash uit self_id
  mesh::Identity dest(_target_pub);
  mesh::Packet* pkt = _mesh->createDatagram(PAYLOAD_TYPE_TXT_MSG, dest, _secret, temp, 5 + tlen);
  if (pkt == nullptr) { _next_send = millis() + 1000; return; }

  _host->rcliSend(pkt, _path, _path_len);
  _attempt++;
  _next_send = millis() + RCLI_MIN_GAP_MS;
  _deadline  = millis() + RCLI_STEP_TIMEOUT_MS;
}

/* Het statusverzoek. De vorm is LETTERLIJK die van BaseChatMesh::sendRequest(
 * recipient, req_type, ...) uit de gepinde submodule (BaseChatMesh.cpp ~659):
 *
 *   uint8_t temp[13];
 *   temp[0..3]  = tag (getCurrentTimeUnique)
 *   temp[4]     = req_type
 *   temp[5..8]  = 0            // reserved (mogelijk voor een 'since'-param)
 *   temp[9..12] = random       // maakt de packet-hash uniek
 *   createDatagram(PAYLOAD_TYPE_REQ, ...)
 *
 * De tag is niet decoratief: MyMesh::handleRequest kaatst de sender_timestamp terug
 * in reply_data[0..3], dus we kunnen het antwoord aan ONS verzoek koppelen. Dat is
 * hier belangrijker dan bij de CLI-weg, want een loginantwoord komt met hetzelfde
 * PAYLOAD_TYPE_RESPONSE binnen.
 *
 * En net als bij de CLI: de repeater toetst `timestamp > client->last_timestamp`
 * (STRIKT), dus elke poging krijgt een nieuwe tag. getCurrentTimeUnique() loopt
 * altijd op, ook als de RTC stilstaat. */
void RepeaterCli::sendStatusReq() {
  uint8_t temp[13];
  _status_tag = _mesh->getRTCClock()->getCurrentTimeUnique();
  memcpy(temp, &_status_tag, 4);
  temp[4] = REQ_TYPE_GET_STATUS;
  memset(&temp[5], 0, 4);
  _mesh->getRNG()->random(&temp[9], 4);

  _host->rcliUseClientIdentity();   // createDatagram schrijft de afzenderhash uit self_id
  mesh::Identity dest(_target_pub);
  mesh::Packet* pkt = _mesh->createDatagram(PAYLOAD_TYPE_REQ, dest, _secret, temp, sizeof(temp));
  if (pkt == nullptr) { _next_send = millis() + 1000; return; }

  _host->rcliSend(pkt, _path, _path_len);
  _attempt++;
  _next_send = millis() + RCLI_MIN_GAP_MS;
  _deadline  = millis() + RCLI_STEP_TIMEOUT_MS;
}

/* ------------------------------------------------------------------------
 * loop -- hoogstens EEN zending per ronde
 * ------------------------------------------------------------------------ */
void RepeaterCli::loop() {
  if (!busy()) return;

  const unsigned long now = millis();

  /* Harde bovengrens voor de HELE job, geschaald met het aantal commando's. */
  if ((long)(now - (_started_at + RCLI_JOB_MAX_MS(_job_n))) >= 0) {
    failJob("tijd op (bovengrens van de job bereikt)");
    return;
  }

  /* Stukken van een antwoord aan het verzamelen? Dan niets zenden en zeker niet
   * herhalen (een herhaling zou het commando op de tegenkant opnieuw uitvoeren);
   * na RCLI_CHUNK_GAP_MS stilte is het antwoord compleet. */
  if (_state == RCLI_CMD && _nchunks > 0) {
    if ((long)(now - _collect_until) >= 0) finishAnswer();
    return;
  }

  /* Nog aan het wachten op het antwoord van de huidige poging? */
  if (_attempt > 0 && (long)(now - _deadline) < 0) return;

  /* De poging is verlopen. */
  if (_state == RCLI_LOGIN) {
    if (_attempt >= RCLI_MAX_ATTEMPTS) {
      /* Een FOUT wachtwoord geeft op de tegenkant geen foutantwoord maar STILTE
       * (handleLoginReq geeft reply_len=0), dus "geen antwoord" en "verkeerd
       * wachtwoord" zien er van hier af hetzelfde uit -- dat expliciet zeggen. */
      failJob("geen loginantwoord na 3 pogingen (fout wachtwoord geeft ook stilte)");
      return;
    }
  } else if (_state == RCLI_STATUS) {
    if (_attempt >= RCLI_MAX_ATTEMPTS) {
      /* Drie pogingen gehad. Herhalen MAG hier: een statusverzoek is een LEESactie
       * (isMutating is er niet op van toepassing), dus dit is het verschil met een
       * muterend CLI-commando. Blijft het stil, dan melden we NIETS -- zie de
       * status-tak in failJob(). */
      failJob("ingelogd, maar geen statusantwoord na 3 pogingen");
      return;
    }
  } else {   // RCLI_CMD
    if (_attempt >= _cmd_max_attempts) {
      /* Dit COMMANDO opgegeven -> als "geen antwoord" (null) afleveren en DOOR naar
       * het volgende. Eén stil commando breekt de hele sweep niet af; dat is het
       * verschil met een login-fout. */
      deliverCurrent(nullptr);
      beginNextCommand();
      _next_send = now + RCLI_MIN_GAP_MS;
      return;
    }
  }

  /* Minimumafstand tussen twee zendingen -- ook tussen twee commando's. */
  if ((long)(now - _next_send) < 0) return;

  /* Een bewaard pad dat niet meer klopt geeft stilte; vanaf de tweede poging dus
   * flood, die de node hoe dan ook vindt en meteen een vers pad oplevert. */
  if (_attempt > 0 && _path_len != OUT_PATH_UNKNOWN) {
    _path_len = OUT_PATH_UNKNOWN;
    _have_cached_path = false;
  }

  if (_state == RCLI_LOGIN)       sendLogin();
  else if (_state == RCLI_STATUS) sendStatusReq();
  else                            sendCommand();
}

/* De verzamelde stukken op tijdstempel sorteren (hoogstens RCLI_CHUNK_MAX, dus
 * insertion sort), met één spatie aaneenrijgen, afleveren en door naar het
 * volgende commando. De tegenkant geeft elk pakket een oplopende
 * getCurrentTimeUnique(); zijn ze gelijk, dan blijft de aankomstvolgorde. */
void RepeaterCli::finishAnswer() {
  uint8_t order[RCLI_CHUNK_MAX];
  for (uint8_t i = 0; i < _nchunks; i++) order[i] = i;
  for (uint8_t i = 1; i < _nchunks; i++) {
    uint8_t k = order[i];
    int j = (int)i - 1;
    while (j >= 0 && (int32_t)(_chunk_ts[order[j]] - _chunk_ts[k]) > 0) { order[j + 1] = order[j]; j--; }
    order[j + 1] = k;
  }
  char out[RCLI_ANSWER_MAX];
  size_t o = 0;
  for (uint8_t i = 0; i < _nchunks; i++) {
    uint8_t k = order[i];
    if (o > 0 && o < RCLI_ANSWER_MAX - 1) out[o++] = ' ';
    size_t l = _chunk_len[k];
    if (o + l > RCLI_ANSWER_MAX - 1) l = RCLI_ANSWER_MAX - 1 - o;
    memcpy(out + o, _answer + _chunk_off[k], l);
    o += l;
  }
  out[o] = 0;
  memcpy(_answer, out, o + 1);
  _nchunks = 0;
  _answer_used = 0;
  deliverCurrent(o ? _answer : nullptr);
  beginNextCommand();
  _next_send = millis() + RCLI_MIN_GAP_MS;
}

/* ------------------------------------------------------------------------
 * Inkomend
 * ------------------------------------------------------------------------ */
bool RepeaterCli::onPeerData(uint8_t type, const uint8_t* data, size_t len) {
  if (!busy()) return false;

  if (_state == RCLI_LOGIN && type == PAYLOAD_TYPE_RESPONSE && len >= 7) {
    if (data[4] != RESP_SERVER_LOGIN_OK) {
      failJob("login geweigerd (verkeerd wachtwoord?)");
      return true;
    }
    if (data[6] == 0) {
      /* Ingelogd, maar niet als BEHEERDER. De repeater voert CLI-tekst alleen uit
       * voor client->isAdmin() en gooit hem anders STIL weg -- hier afvangen
       * scheelt N commando's de lucht in duwen voor niets. */
      failJob("ingelogd zonder beheerdersrecht; dit wachtwoord mag geen CLI");
      return true;
    }
    /* Login OK. _next_send staat al op login-tijd + gap, dus de volgende zending
     * wacht die afstand netjes uit.
     *
     * LET OP HET ONDERSCHEID MET HET STATUSANTWOORD: dat komt met hetzelfde
     * PAYLOAD_TYPE_RESPONSE binnen als dit loginantwoord. Ze worden uitsluitend
     * door de STAAT gescheiden (RCLI_LOGIN vs RCLI_STATUS), en het statuspad toetst
     * daarnaast de teruggekaatste tag. Zonder dat onderscheid zou data[4] van een
     * stats-antwoord (de lage byte van batt_milli_volts) toevallig als
     * RESP_SERVER_LOGIN_OK gelezen kunnen worden. */
    _attempt   = 0;
    _job_i     = 0;
    _answer[0] = 0;
    _nchunks   = 0;
    _answer_used = 0;
    if (_job_kind == RCLI_JOB_STATUS) {
      _state = RCLI_STATUS;
      return true;
    }
    _state = RCLI_CMD;
    if (!loadCurrentParam()) { deliverCurrent(nullptr); beginNextCommand(); }
    return true;
  }

  /* HET STATUSANTWOORD. Zelfde PAYLOAD_TYPE_RESPONSE als de login, dus alleen de
   * staat (en de tag) scheiden ze. Zie de draadvorm bovenaan RepeaterCli.h. */
  if (_state == RCLI_STATUS && type == PAYLOAD_TYPE_RESPONSE) {
    if (_attempt == 0) return false;   // nog niets gevraagd; zie de CMD-tak hieronder

    if (len < RCLI_STATUS_RESP_MIN) {
      /* Te kort voor een RepeaterStats. Dit is GEEN antwoord dat we half mogen
       * gebruiken: laten liggen en de gewone herhaling zijn werk laten doen. Komt
       * het na drie pogingen niet, dan melden we niets. */
      MESH_DEBUG_PRINTLN("RepeaterCli: statusantwoord van %s te kort (%u byte, >= %u nodig)",
                         _target_hex, (unsigned)len, (unsigned)RCLI_STATUS_RESP_MIN);
      return false;
    }

    /* De teruggekaatste tag: MyMesh::handleRequest zet onze sender_timestamp in
     * reply_data[0..3]. Klopt hij niet, dan is dit het antwoord op een ANDER (ouder)
     * verzoek en horen die cijfers niet bij deze ronde. */
    uint32_t tag;
    memcpy(&tag, data, 4);
    if (tag != _status_tag) {
      MESH_DEBUG_PRINTLN("RepeaterCli: statusantwoord van %s met vreemde tag; genegeerd",
                         _target_hex);
      return false;
    }

    RepeaterStatus st;
    if (!parseStatus(data, len, st)) {
      /* Ontleed maar NIET plausibel -> vermoedelijk een andere structuurindeling
       * (een fork die een veld invoegde of herordende). Dan is elk getal verdacht,
       * dus we melden niets en zeggen waarom. Opnieuw proberen heeft geen zin: de
       * volgende poging levert dezelfde bytes. */
      failJob("statusantwoord onverwacht van vorm; geen metingen gemeld");
      return true;
    }

    if (_stats_fn != nullptr) _stats_fn(_stats_ctx, _target_hex, st);
    snprintf(_answer, sizeof(_answer),
             "status ok: %u mV, uptime %lus, airtime %lus",
             (unsigned)st.batt_milli_volts, (unsigned long)st.total_up_time_secs,
             (unsigned long)st.total_air_time_secs);
    memset(_pass, 0, sizeof(_pass));
    _state = RCLI_DONE;
    return true;
  }

  if (_state == RCLI_CMD && type == PAYLOAD_TYPE_TXT_MSG && len > 5) {
    /* Alleen aannemen als het LOPENDE commando ook echt de lucht in is (_attempt>0).
     * Tussen twee commando's van een job staat de staat al op RCLI_CMD met
     * _attempt==0 terwijl we de tussenpauze uitwachten; een late/dubbele reactie op
     * het VORIGE commando zou dan aan het VOLGENDE (nog niet verstuurde) commando
     * toegeschreven worden -- een antwoord onder de verkeerde parameternaam. */
    if (_attempt == 0) return false;
    const uint8_t flags = (uint8_t)(data[4] >> 2);
    if (flags != TXT_TYPE_CLI_DATA && flags != TXT_TYPE_PLAIN) return false;

    /* EEN STUK van het antwoord. Bewaren met de tijdstempel uit het pakket
     * (data[0..3]); afleveren gebeurt pas na RCLI_CHUNK_GAP_MS stilte in loop()
     * (finishAnswer), zodat een antwoord dat de repeater in meerdere pakketten
     * stuurt volledig en in de juiste volgorde bij MeshManager aankomt. De eerste
     * versie leverde het EERSTE pakket meteen af: van Jessa's `filter count` kwam
     * dan alleen de limiettabel binnen en nooit de kopregel met de tellers. */
    uint32_t ts; memcpy(&ts, data, 4);
    const char* src = (const char*)&data[5];
    const size_t avail = len - 5;
    size_t n = 0;
    if (_nchunks < RCLI_CHUNK_MAX && _answer_used < RCLI_ANSWER_MAX - 1) {
      char* dst = _answer + _answer_used;
      const size_t room = RCLI_ANSWER_MAX - 1 - _answer_used;
      while (n < avail && n < room && src[n] != 0) {
        char c = src[n];
        dst[n] = ((unsigned char)c < 0x20) ? ' ' : c;   // regeleindes -> spatie
        n++;
      }
      while (n > 0 && dst[n - 1] == ' ') n--;
      if (n > 0) {
        _chunk_ts[_nchunks]  = ts;
        _chunk_off[_nchunks] = _answer_used;
        _chunk_len[_nchunks] = (uint16_t)n;
        _nchunks++;
        _answer_used += (uint16_t)n;
      }
    }

    if (n == 0 && _nchunks == 0) {
      /* Leeg antwoord = de repeater zag onze zending als herhaling (kan haast niet,
       * elke poging heeft een nieuwe tijdstempel) of het is een onbekende opdracht/
       * rechtenkwestie. Als "geen antwoord" (null) afleveren en door. */
      deliverCurrent(nullptr);
      beginNextCommand();
      _next_send = millis() + RCLI_MIN_GAP_MS;
      return true;
    }

    _collect_until = millis() + RCLI_CHUNK_GAP_MS;
    /* Vol (stukken of tekens): dan heeft wachten geen zin meer. */
    if (_nchunks >= RCLI_CHUNK_MAX || _answer_used >= RCLI_ANSWER_MAX - 1) finishAnswer();
    return true;
  }

  return false;
}

void RepeaterCli::onPath(const uint8_t* path, uint8_t path_len, uint8_t extra_type,
                         const uint8_t* extra, uint8_t extra_len) {
  if (!busy()) return;

  _path_len = mesh::Packet::copyPath(_path, path, path_len);
  memcpy(_cached_path, _path, sizeof(_cached_path));
  _cached_path_len  = _path_len;
  memcpy(_cached_pub, _target_pub, PUB_KEY_SIZE);
  _have_cached_path = true;

  /* Op een FLOOD-login zit het loginantwoord VERPAKT in deze PATH (createPathReturn
   * met extra=RESPONSE). Wie dat mist, wacht op elke eerste login tot de time-out. */
  if (extra_type == PAYLOAD_TYPE_RESPONSE && extra_len > 0) {
    onPeerData(PAYLOAD_TYPE_RESPONSE, extra, extra_len);
  }
}

/* ------------------------------------------------------------------------
 * parseStatus -- de bytes lezen EN toetsen
 *
 * Lezen met memcpy op expliciete offsets (zie de tabel bovenaan RepeaterCli.h):
 * de buffer is niet gegarandeerd uitgelijnd en een struct-cast zou stil
 * meeveranderen met onze eigen padding.
 *
 * De TOETS is het belangrijkste deel van deze functie. De doelrepeater hoeft niet
 * onze build te draaien -- JessaZH draait dutchmeshcore v1.17.1-PS+filter+rollback
 * -- en een fork die een veld INVOEGT of HERORDENT levert bytes die er volkomen
 * geldig uitzien maar op de verkeerde plaats staan. Dat is aan de bytes zelf niet
 * te zien, dus toetsen we de velden waarvan we het fysieke bereik kennen. Faalt er
 * een, dan verwerpen we het HELE antwoord: liever een lege pagina dan een
 * grafiek met een verzonnen getal erin.
 *
 * De grenzen, met hun reden:
 *  - accuspanning: 0 (niet gemeten / geen accu) of 1500..6000 mV. Een 18650 of
 *    LiPo op een LoRa-node zit tussen 3,0 en 4,3 V; 1,5-6,0 V laat elke
 *    plausibele voeding door en verwerpt een teller die daar terechtkwam.
 *  - uptime: < 20 jaar. Een node die langer beweert te lopen dan het protocol
 *    bestaat, leest van de verkeerde offset.
 *  - airtime en rx-airtime: mogen de uptime niet OVERSCHRIJDEN. Een radio kan niet
 *    langer gezonden hebben dan hij aan stond. Dit is de scherpste toets die we
 *    hebben en hij pakt precies de verschuiving die een ingevoegd veld geeft.
 *    (Met uptime 0 -- een node die net op is -- slaan we deze toets over.)
 *  - TX-wachtrij: <= 4096. De pakketpool van een node is enkele tientallen.
 *  - ruisvloer/RSSI: -200..50 dBm; SNR: -50..50 dB. Buiten die banden bestaat
 *    geen radio.
 * ------------------------------------------------------------------------ */
bool RepeaterCli::parseStatus(const uint8_t* data, size_t len, RepeaterStatus& out) {
  if (len < RCLI_STATUS_RESP_MIN) return false;
  const uint8_t* p = data + 4;   // achter de teruggekaatste tag

  memcpy(&out.batt_milli_volts,       p +  0, 2);
  memcpy(&out.curr_tx_queue_len,      p +  2, 2);
  memcpy(&out.noise_floor,            p +  4, 2);
  memcpy(&out.last_rssi,              p +  6, 2);
  memcpy(&out.n_packets_recv,         p +  8, 4);
  memcpy(&out.n_packets_sent,         p + 12, 4);
  memcpy(&out.total_air_time_secs,    p + 16, 4);
  memcpy(&out.total_up_time_secs,     p + 20, 4);
  memcpy(&out.n_sent_flood,           p + 24, 4);
  memcpy(&out.n_sent_direct,          p + 28, 4);
  memcpy(&out.n_recv_flood,           p + 32, 4);
  memcpy(&out.n_recv_direct,          p + 36, 4);
  memcpy(&out.err_events,             p + 40, 2);
  memcpy(&out.last_snr_x4,            p + 42, 2);
  memcpy(&out.n_direct_dups,          p + 44, 2);
  memcpy(&out.n_flood_dups,           p + 46, 2);
  memcpy(&out.total_rx_air_time_secs, p + 48, 4);
  memcpy(&out.n_recv_errors,          p + 52, 4);

  const uint32_t TWENTY_YEARS = 631152000UL;   // 20 * 365,25 dagen in seconden

  if (out.batt_milli_volts != 0 &&
      (out.batt_milli_volts < 1500 || out.batt_milli_volts > 6000)) return false;
  if (out.total_up_time_secs > TWENTY_YEARS) return false;
  if (out.total_up_time_secs > 0) {
    if (out.total_air_time_secs    > out.total_up_time_secs) return false;
    if (out.total_rx_air_time_secs > out.total_up_time_secs) return false;
  }
  if (out.curr_tx_queue_len > 4096) return false;
  if (out.noise_floor < -200 || out.noise_floor > 50) return false;
  if (out.last_rssi   < -200 || out.last_rssi   > 50) return false;
  const int snr_db = out.last_snr_x4 / 4;
  if (snr_db < -50 || snr_db > 50) return false;

  return true;
}
