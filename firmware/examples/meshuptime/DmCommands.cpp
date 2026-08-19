#include "DmCommands.h"

/* Alleen voor PERM_RECV_ALERTS_LO/HI. Die staan in SensorMesh.h en niet in
 * ClientACL.h, en ze hier overtypen zou stil scheef gaan lopen zodra iemand ze
 * verschuift. Een include is dan de veiligere afhankelijkheid. */
#include "SensorMesh.h"

#include <string.h>

/* ======================= WAT MAIN.CPP MOET AANLEVEREN =======================
 *
 * MyMesh in main.cpp is de enige plek die de sensorlaag, de wifi-taak EN deze
 * module kent. Daar hoort dus de koppeling, en niet in een global in een
 * header. Nodig zijn:
 *
 *   1. #include "DmCommands.h"
 *
 *   2. een klasse die DmDataSource implementeert, met de drie methodes. Die
 *      leest MonitorSensors via zijn PUBLIEKE methodes:
 *
 *        vast, kanaal 2/3/4 : isMains(), isWifiOnline(), lastVolts(),
 *                             secsInPowerState(), CH_MAINS/CH_BATTERY/CH_WIFI
 *        ping-monitors      : MAX_MONITORS, monitorUsed(slot),
 *                             monitorChannel(slot), monitorName(slot),
 *                             monitorHost(slot), monitorIsUp(slot),
 *                             monitorSeeded(slot), monitorPingMs(slot),
 *                             monitorChecks(slot), monitorFails(slot)
 *
 *      dmSensorCount() geeft dan 3 + MonitorSensors::MAX_MONITORS, en
 *      dmSensorAt() geeft false voor een vakje waar monitorUsed() false is --
 *      DmCommands slaat zo'n vakje over. Zo blijft de nummering van de vakjes
 *      gelijk aan die van MonitorSensors en hoeft er niets te schuiven als er
 *      een monitor verdwijnt.
 *
 *      Deze module raakt MonitorSensors NERGENS zelf aan: die klasse is nog in
 *      beweging, en de enige afspraak die hier telt is DmDataSource.
 *
 *   3. in MyMesh:
 *        bool handleIncomingMsg(ClientInfo& from, uint32_t timestamp,
 *                               uint8_t* data, uint8_t flags, size_t len) override {
 *          return dm.handleDm(from, timestamp, data, len);
 *        }
 *        void onAckRecv(mesh::Packet* packet, uint32_t ack_crc) override {
 *          if (dm.onAck(ack_crc)) { packet->markDoNotRetransmit(); return; }
 *          SensorMesh::onAckRecv(packet, ack_crc);
 *        }
 *
 *   4. in setup():  dm.begin(&the_mesh, &dm_source);
 *                   dm.setPathHashSize(the_mesh.getNodePrefs()->path_hash_mode + 1);
 *
 *   5. in loop():   dm.loop();
 *
 * LET OP -- ZONDER EEN WIJZIGING BUITEN DEZE MODULE KOMT ALLEEN EEN ADMIN HIER.
 * SensorMesh::onPeerDataRecv laat tekstberichten door met:
 *
 *     } else if (type == PAYLOAD_TYPE_TXT_MSG && len > 5 && from->isAdmin()) {
 *
 * handleIncomingMsg() wordt dus nooit aangeroepen voor een gewone leescontact.
 * De rechtentoets hieronder is daarmee juist maar in de praktijk nog niet
 * zichtbaar; wie de opdrachten voor leescontacten wil openzetten, moet daar
 * `&& from->isAdmin()` weghalen en de toets aan isAllowed() laten. Dat is een
 * wijziging in SensorMesh.cpp en die valt buiten deze opdracht.
 * =========================================================================== */

void DmCommands::begin(mesh::Mesh* mesh, DmDataSource* data) {
  _mesh = mesh;
  _data = data;
  _boot_time = (mesh != NULL) ? mesh->getRTCClock()->getCurrentTime() : 0;
  reset();
}

void DmCommands::reset() {
  _text[0] = 0;
  _tail[0] = 0;
  _num_chunks = 0;
  _next_chunk = 0;
  _attempt = 0;
  _gen_omitted = 0;
  _expected_ack = 0;
  _msg_timestamp = 0;
  _next_action = millis();
  _to_path_len = OUT_PATH_UNKNOWN;
}

/* ============================== RECHTEN ==============================
 *
 * De ACL van SensorMesh is de enige lijst; geen eigen tweede lijst ernaast, die
 * loopt uit elkaar zodra iemand `setperm` gebruikt.
 */
bool DmCommands::isAllowed(const ClientInfo& from) const {
  if ((from.permissions & PERM_ACL_ROLE_MASK) >= PERM_ACL_READ_ONLY) return true;

  /* Wie waarschuwingen van deze node ontvangt, krijgt nu al teksten over
   * precies deze sensoren toegestuurd ("Battery is low"). Hem de toestand
   * laten OPVRAGEN legt dus niets bloot wat hij niet ongevraagd al kreeg, en
   * dat is de reden dat deze twee bits hier meetellen en niet alleen de rol. */
  if (from.permissions & (PERM_RECV_ALERTS_LO | PERM_RECV_ALERTS_HI)) return true;

  return false;
}

void DmCommands::startReply(const ClientInfo& to) {
  reset();

  /* De ontvanger wordt GEKOPIEERD. Het antwoord loopt over meerdere
   * loop()-rondes, en in die tijd kan acl.putClient() of acl.load() de
   * ClientInfo achter deze referentie overschrijven. */
  _to_id = to.id;
  memcpy(_to_secret, to.shared_secret, PUB_KEY_SIZE);
  if (to.out_path_len != OUT_PATH_UNKNOWN && to.out_path_len <= MAX_PATH_SIZE) {
    memcpy(_to_path, to.out_path, to.out_path_len);
    _to_path_len = to.out_path_len;
  } else {
    _to_path_len = OUT_PATH_UNKNOWN;
  }
}

bool DmCommands::appendText(const char* s) {
  size_t cur = strlen(_text);
  size_t add = strlen(s);
  if (cur + add + 1 > sizeof(_text)) return false;
  memcpy(&_text[cur], s, add + 1);
  return true;
}

/* ============================== OPDRACHTEN ============================== */

/* `list` -- de enige weg waarlangs een naam over het mesh gaat; zie de uitleg
 * bovenaan DmCommands.h. De vorm is "kanaal:naam=toestand", zodat de vrager in
 * EEN antwoord de koppeling kanaalnummer -> naam krijgt die CayenneLPP zelf
 * niet kan dragen. Verander die vorm niet zonder te bedenken wat er aan de
 * andere kant op geparst wordt. */
void DmCommands::buildList() {
  const int n = _data->dmSensorCount();
  int written = 0;

  for (int i = 0; i < n; i++) {
    if (!_data->dmSensorAt(i, _si)) continue;

    char line[DM_NAME_MAX + DM_STATE_MAX + 12];
    if (_si.channel != 0) {
      snprintf(line, sizeof(line), "%s%u:%s=%s", written ? "\n" : "", (unsigned) _si.channel, _si.name, _si.state);
    } else {
      snprintf(line, sizeof(line), "%s%s=%s", written ? "\n" : "", _si.name, _si.state);
    }

    if (!appendText(line)) {
      /* De werkbuffer is vol. Dat kan alleen als er meer dan 768 byte tekst is,
       * en dat is ruim meer dan de vier stukken samen kwijt kunnen; de knipper
       * houdt dus altijd iets over en de slotregel komt er zeker. */
      _gen_omitted = n - i;
      break;
    }
    written++;
  }

  if (written == 0) appendText("geen sensoren bekend");
}

void DmCommands::buildGet(const char* name) {
  const int n = _data->dmSensorCount();
  int found = -1;

  // eerst een volledige naam, kleine/hoofdletters maken niet uit
  for (int i = 0; i < n && found < 0; i++) {
    if (_data->dmSensorAt(i, _si) && strcasecmp(_si.name, name) == 0) found = i;
  }

  /* Anders een beginstuk, maar ALLEEN als dat er precies een oplevert. Bij
   * twee treffers de eerste teruggeven zou de vrager stilzwijgend de verkeerde
   * sensor geven, en dat is erger dan hem opnieuw laten typen. */
  if (found < 0) {
    const size_t len = strlen(name);
    int hits = 0;
    for (int i = 0; i < n; i++) {
      if (_data->dmSensorAt(i, _si) && strncasecmp(_si.name, name, len) == 0) { hits++; found = i; }
    }
    if (hits != 1) found = -1;
  }

  if (found < 0 || !_data->dmSensorAt(found, _si)) {
    char line[DM_NAME_MAX + 40];
    snprintf(line, sizeof(line), "geen sensor '%.*s'; stuur 'list'", DM_NAME_MAX, name);
    appendText(line);
    return;
  }

  char line[DM_NAME_MAX + DM_STATE_MAX + 24];
  if (_si.channel != 0) {
    snprintf(line, sizeof(line), "%s (kanaal %u) = %s", _si.name, (unsigned) _si.channel, _si.state);
  } else {
    snprintf(line, sizeof(line), "%s = %s", _si.name, _si.state);
  }
  appendText(line);

  if (_si.detail[0]) {
    appendText("\n");
    appendText(_si.detail);
  }
}

/* Uptime als tekst. Dagen erbij zodra ze er zijn: een node die maanden hangt is
 * het interessante geval, en "1512u" leest niemand goed. */
static void formatUptime(char* dest, size_t max_len, uint32_t secs) {
  const uint32_t d = secs / 86400UL;  secs %= 86400UL;
  const uint32_t h = secs / 3600UL;   secs %= 3600UL;
  const uint32_t m = secs / 60UL;

  if (d > 0) {
    snprintf(dest, max_len, "%ud%uu%02um", (unsigned) d, (unsigned) h, (unsigned) m);
  } else {
    snprintf(dest, max_len, "%uu%02um", (unsigned) h, (unsigned) m);
  }
}

void DmCommands::buildStatus() {
  /* Voeding en wifi in de woorden van de toepassing: die kent de drempels en de
   * wifi-taak, deze module niet. Uptime en vrije heap komen van hier, want die
   * horen bij het bord en niet bij de sensorlaag. */
  char line[96];
  line[0] = 0;
  _data->dmStatusLine(line, sizeof(line));
  appendText(line);

  /* Bij voorkeur uit de RTC, want millis() loopt na ~49 dagen om en juist een
   * node die lang hangt wil je hier eerlijk zien. Staat de klok niet (nog geen
   * tijdsync), dan is millis() het enige dat er is. */
  const uint32_t now = _mesh->getRTCClock()->getCurrentTime();
  const uint32_t up  = (_boot_time != 0 && now > _boot_time) ? (now - _boot_time) : (uint32_t)(millis() / 1000UL);

  char ut[16];
  formatUptime(ut, sizeof(ut), up);

  unsigned long heap = 0;
#if defined(ESP32)
  heap = (unsigned long) ESP.getFreeHeap();
#endif

  char extra[64];
  snprintf(extra, sizeof(extra), "\nuptime %s\nvrij %lu B", ut, heap);
  appendText(extra);
}

void DmCommands::buildHelp() {
  appendText("list: alle sensoren als kanaal:naam=toestand\n"
             "get <naam>: een sensor met detail\n"
             "status: voeding, wifi, uptime, vrij geheugen\n"
             "help: deze lijst");
}

/* ============================== OPKNIPPEN ============================== */

void DmCommands::splitReply() {
  _num_chunks = 0;
  _next_chunk = 0;
  _attempt = 0;
  _tail[0] = 0;

  const uint16_t len = (uint16_t) strlen(_text);
  if (len == 0) return;

  /* Past het in EEN bericht, dan geen "[1/1] " ervoor: dat is 6 byte zendtijd
   * voor niets en het leest slechter. Zonder voorvoegsel mag de volle 158 op. */
  if (len <= DM_CHUNK_MAX) {
    _chunk_off[0] = 0;
    _chunk_len[0] = (uint8_t) len;
    _num_chunks = 1;
    return;
  }

  uint16_t pos = 0;
  while (pos < len && _num_chunks < DM_MAX_CHUNKS) {
    uint16_t limit = DM_CHUNK_TEXT_MAX;

    /* In het laatste toegestane stuk ruimte vrijhouden voor de slotregel, maar
     * alleen als er echt iets overblijft om over te melden. */
    if (_num_chunks == DM_MAX_CHUNKS - 1 && (uint16_t)(len - pos) > limit) {
      limit -= DM_TAIL_MAX;
    }

    uint16_t take = len - pos;
    uint16_t skip = 0;   // scheidingsteken dat na het knippen overgeslagen wordt

    if (take > limit) {
      /* Knippen op regelgrens als dat kan -- een halve regel "5:dak-repea" is
       * onbruikbaar -- anders op woordgrens, en pas hard afkappen als een
       * enkel woord langer is dan een heel stuk. */
      uint16_t w = limit;
      while (w > 0 && _text[pos + w] != '\n') w--;
      if (w == 0) {
        w = limit;
        while (w > 0 && _text[pos + w] != ' ') w--;
      }
      if (w > 0) { take = w; skip = 1; } else { take = limit; }
    }

    _chunk_off[_num_chunks] = pos;
    _chunk_len[_num_chunks] = (uint8_t) take;
    _num_chunks++;
    pos += take + skip;
  }

  /* De slotregel: wat er NIET meegaat, staat erin. Een afgekapt antwoord dat er
   * compleet uitziet is erger dan een kort antwoord met een eerlijke staart. */
  if (pos < len || _gen_omitted > 0) {
    int lines = _gen_omitted;
    if (pos < len) {
      lines++;   // de rest vanaf pos is minstens een regel
      for (uint16_t i = pos; i < len; i++) {
        if (_text[i] == '\n') lines++;
      }
    }

    /* De slotregel komt ACHTER het laatste stuk en moet daar dus in passen. In
     * de gewone gang van zaken is dat al geregeld -- de lus hierboven houdt in
     * het laatste toegestane stuk DM_TAIL_MAX vrij -- maar niet in twee andere
     * gevallen: een antwoord dat in MINDER dan DM_MAX_CHUNKS stukken past en
     * toch een slotregel nodig heeft omdat de opbouw zelf al regels liet
     * vallen, en het antwoord van precies EEN stuk van 158 byte. Zonder deze
     * nacontrole werd dat stilzwijgend een bericht van 184 byte: langer dan
     * MAX_TEXT_LEN, en dus een bericht dat de radio nooit uit krijgt.
     *
     * Er wordt met DM_TAIL_MAX gerekend en niet met strlen(_tail), zodat het
     * opmaken hieronder de slotregel nog een cijfer langer mag maken zonder dat
     * de rekensom omvalt. In het gewone geval verandert deze toets niets. */
    const uint8_t room = (_num_chunks == 1) ? DM_CHUNK_MAX : DM_CHUNK_TEXT_MAX;
    const uint8_t last = _num_chunks - 1;

    if (_chunk_len[last] + DM_TAIL_MAX > room) {
      const uint16_t off = _chunk_off[last];
      const uint16_t max_len = room - DM_TAIL_MAX;

      uint16_t w = max_len;
      while (w > 0 && _text[off + w] != '\n') w--;
      if (w == 0) {
        w = max_len;
        while (w > 0 && _text[off + w] != ' ') w--;
      }
      if (w == 0) w = max_len;   // een enkel woord langer dan een stuk

      // wat we nu extra laten vallen, hoort ook in de telling
      lines++;
      for (uint16_t i = off + w; i < off + _chunk_len[last]; i++) {
        if (_text[i] == '\n') lines++;
      }
      _chunk_len[last] = (uint8_t) w;
    }

    snprintf(_tail, sizeof(_tail), " (+%d regels weg, zie web)", lines);
  }
}

/* ============================== VERZENDEN ==============================
 *
 * TXT_TYPE_PLAIN en NIET TXT_TYPE_CLI_DATA. In BaseChatMesh::onPeerDataRecv
 * staat het met zoveel woorden:
 *
 *     } else if (flags == TXT_TYPE_CLI_DATA) {
 *       onCommandDataRecv(...);
 *       // NOTE: no ack expected for CLI_DATA replies
 *
 * Voor een CLI_DATA-antwoord stuurt de ontvanger dus GEEN ACK terug. Dan weet
 * deze node nooit of het antwoord is aangekomen, en dat is precies wat je bij
 * een antwoord in vier stukken wel wilt weten. PLAIN levert een ACK op, en op
 * die ACK loopt de herhaallus hieronder.
 */
void DmCommands::sendChunk() {
  const uint8_t idx = _next_chunk;
  const bool is_last = (idx + 1 == _num_chunks);

  uint8_t data[5 + DM_CHUNK_MAX + 1];
  char* out = (char *) &data[5];
  int n = 0;

  /* Met DM_MAX_CHUNKS = 4 is "[n/m] " altijd precies DM_CHUNK_PREFIX_LEN lang;
   * gaat die grens ooit boven 9, dan moet DM_CHUNK_PREFIX_LEN mee omhoog. */
  if (_num_chunks > 1) {
    n = snprintf(out, DM_CHUNK_PREFIX_LEN + 1, "[%u/%u] ", (unsigned)(idx + 1), (unsigned) _num_chunks);
  }

  memcpy(&out[n], &_text[_chunk_off[idx]], _chunk_len[idx]);
  n += _chunk_len[idx];

  if (is_last && _tail[0]) {
    const int t = (int) strlen(_tail);
    memcpy(&out[n], _tail, t);
    n += t;
  }

  if (n > DM_CHUNK_MAX) n = DM_CHUNK_MAX;   // kan niet, maar knipt liever dan schrijft buiten data[]
  out[n] = 0;

  /* Een herhaling moet dezelfde tekst EN dezelfde tijdstempel hebben, anders
   * ziet de ontvanger twee losse berichten in plaats van een tweede poging. */
  if (_attempt == 0) _msg_timestamp = _mesh->getRTCClock()->getCurrentTimeUnique();

  memcpy(data, &_msg_timestamp, 4);
  data[4] = (TXT_TYPE_PLAIN << 2) | (_attempt & 3);   // lage 2 bits: pogingnummer

  // de ACK die we hierop verwachten, op dezelfde manier berekend als SensorMesh::sendAlert
  mesh::Utils::sha256((uint8_t *) &_expected_ack, 4, data, 5 + n, _mesh->self_id.pub_key, PUB_KEY_SIZE);

  mesh::Packet* pkt = _mesh->createDatagram(PAYLOAD_TYPE_TXT_MSG, _to_id, _to_secret, data, 5 + n);
  if (pkt) {
    if (_to_path_len != OUT_PATH_UNKNOWN) {
      _mesh->sendDirect(pkt, _to_path, _to_path_len);
    } else {
      _mesh->sendFlood(pkt, 0, _path_hash_size);
    }
  }

  _next_action = millis() + DM_ACK_TIMEOUT_MS;
}

bool DmCommands::onAck(uint32_t ack_crc) {
  if (_num_chunks == 0 || _expected_ack == 0) return false;
  if (ack_crc != _expected_ack) return false;

  _expected_ack = 0;
  _attempt = 0;
  _next_chunk++;

  if (_next_chunk >= _num_chunks) {   // hele antwoord bezorgd
    reset();
    return true;
  }

  /* Rust tussen twee stukken. Niet omdat het moet, maar omdat het antwoord op
   * een DM geen reden is om de band vol te zetten. */
  _next_action = millis() + DM_SPACING_MS;
  return true;
}

/* Hoogstens EEN bericht per ronde de wachtrij in. Vier stukken achter elkaar
 * inschuiven zou de radio voor seconden bezet zetten voor iets wat niemand
 * gevraagd heeft behalve een enkele afzender; de radio gaat voor. */
void DmCommands::loop() {
  if (_num_chunks == 0) return;
  if ((long)(millis() - _next_action) < 0) return;

  if (_expected_ack != 0) {
    // de ACK op het vorige stuk is niet binnen DM_ACK_TIMEOUT_MS gekomen
    _expected_ack = 0;
    if (++_attempt >= DM_MAX_ATTEMPTS) {
      /* Opgeven, en de rest ook laten zitten: als stuk 2 van 4 niet aankomt, is
       * stuk 3 en 4 versturen zendtijd voor een antwoord dat toch een gat
       * heeft. De afzender kan opnieuw vragen. */
      MESH_DEBUG_PRINTLN("DmCommands: geen ACK op stuk %d/%d, antwoord afgebroken",
                         (uint32_t)(_next_chunk + 1), (uint32_t) _num_chunks);
      reset();
      return;
    }
  }

  sendChunk();
}

/* ============================== BINNENKOMEND ============================== */

bool DmCommands::handleDm(const ClientInfo& from, uint32_t timestamp, const uint8_t* text, size_t len) {
  if (_mesh == NULL || _data == NULL) return false;

  /* Zelf een C-string maken. De PLAIN-tak van SensorMesh::onPeerDataRecv zet
   * GEEN afsluitende nul (dat doet alleen de CLI_DATA-tak) maar rekent er
   * verderop wel op; wat er staat is de opvulling met nullen uit het
   * ontsleutelen. Daar niet op vertrouwen. */
  char cmd[64];
  const size_t n = (len < sizeof(cmd) - 1) ? len : sizeof(cmd) - 1;
  memcpy(cmd, text, n);
  cmd[n] = 0;

  char* p = cmd;
  while (*p == ' ' || *p == '\t') p++;
  char* e = p + strlen(p);
  while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;

  if (*p == 0) return false;   // leeg bericht: niets om te behandelen, dus ook geen ACK

  /* ---- afzender zonder rechten ----
   *
   * DE KEUZE: EEN korte weigering zonder enig detail, hoogstens eens per
   * DM_DENY_COOLDOWN_MS per afzender.
   *
   * Stil negeren is afgevallen omdat de afzender dan niet weet of de node stuk
   * is, buiten bereik is of hem weigert -- daar gaat hij van herhalen, en dat
   * kost meer zendtijd dan een antwoord. Wel antwoorden met de sensorlijst is
   * ook afgevallen: dat is precies wat de rechten moeten tegenhouden. De
   * weigering noemt geen naam, geen kanaal en geen toestand, en verraadt dus
   * niets: de afzender staat al in de ACL van deze node en weet dus al dat hij
   * bestaat.
   *
   * De wachttijd zit erop zodat de weigering geen zendtijdversterker wordt: een
   * afzender die tien keer `list` stuurt, krijgt een keer antwoord. Er is EEN
   * plek onthouden en geen lijst -- dit is de uitzonderingsweg, geen boekhouding. */
  if (!isAllowed(from)) {
    const bool same = (memcmp(_deny_key, from.id.pub_key, sizeof(_deny_key)) == 0);
    if (same && (long)(millis() - _deny_until) < 0) {
      /* Wel true, zodat SensorMesh de gewone ACK stuurt: de afzender weet dat
       * zijn bericht is aangekomen en hoeft niet te herhalen. Alleen geen
       * tekst, want die heeft hij net gehad. */
      return true;
    }
    memcpy(_deny_key, from.id.pub_key, sizeof(_deny_key));
    _deny_until = millis() + DM_DENY_COOLDOWN_MS;

    startReply(from);
    appendText("Geen leesrechten op deze node. Vraag de beheerder om toegang.");
    splitReply();
    _next_action = millis() + DM_FIRST_SEND_MS;
    return true;
  }

  /* ---- er is nog een antwoord onderweg ---- */
  if (isBusy() && memcmp(_to_id.pub_key, from.id.pub_key, PUB_KEY_SIZE) != 0) {
    /* Van iemand anders. false teruggeven betekent GEEN ACK, waarop de client
     * van deze afzender "niet bezorgd" meldt en hij het zelf opnieuw doet --
     * tegen die tijd is de vorige beurt klaar. Dat kost geen enkele byte
     * zendtijd, en een wachtrij aanleggen zou betekenen dat een handvol
     * afzenders samen alsnog twintig berichten kunnen laten uitzenden. */
    MESH_DEBUG_PRINTLN("DmCommands: bezig met een ander antwoord, deze DM niet behandeld");
    return false;
  }
  // van dezelfde afzender: de nieuwe vraag vervangt de vorige (hij vroeg opnieuw)

  startReply(from);

  if (strcasecmp(p, "list") == 0) {
    buildList();
  } else if (strcasecmp(p, "status") == 0) {
    buildStatus();
  } else if (strcasecmp(p, "help") == 0 || strcmp(p, "?") == 0) {
    buildHelp();
  } else if (strncasecmp(p, "get", 3) == 0 && (p[3] == ' ' || p[3] == 0)) {
    char* arg = p + 3;
    while (*arg == ' ') arg++;
    if (*arg == 0) {
      appendText("gebruik: get <naam>");
    } else {
      buildGet(arg);
    }
  } else {
    /* Niet gokken en niet zwijgen. Een verwijzing naar `help` is korter dan de
     * hulptekst zelf, en zo betaalt een typefout geen vier berichten. */
    appendText("onbekende opdracht; stuur 'help'");
  }

  splitReply();
  if (_num_chunks == 0) {
    /* Kan alleen als een build-methode niets schreef. Toch true: het bericht is
     * begrepen, en de ACK is dan het enige eerlijke antwoord. */
    return true;
  }

  /* Niet meteen: de gewone ACK op dit binnengekomen bericht mag eerst weg. */
  _next_action = millis() + DM_FIRST_SEND_MS;
  return true;
}
