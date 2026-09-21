#include "OpenHopTask.h"
#ifdef ESP32
  #include <lwip/sockets.h>   /* send() met MSG_DONTWAIT: nooit wachten */
  #include <errno.h>
#endif
#include "RoomMesh.h"
#include "WifiTask.h"
#include "MonitorStore.h"   /* MON_ALERT_*, MON_SEV_* */
#include <string.h>

/* Zie OpenHopTask.h voor het waarom en de draadvorm. Dit bestand is de
 * uitvoering: een TCP-server die het modemprotocol van openHop spreekt, met
 * onze radio eronder in plaats van een lege modem. */

#define OH_DIAG(...) do { Serial.printf("[oh] " __VA_ARGS__); Serial.println(); } while (0)

/* ------------------------------------------------------------------------
 * CRC-16/CCITT-FALSE -- bit voor bit gelijk aan hun crc16_ccitt().
 * ------------------------------------------------------------------------ */
uint16_t OpenHopTask::crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

/* ------------------------------------------------------------------------
 * Boekhouding en persistentie
 * ------------------------------------------------------------------------ */
void OpenHopTask::reset() {
  _fs = nullptr; _wifi = nullptr; _mesh = nullptr;
  _on = false;
  _port = OPENHOP_PORT_DEFAULT;
  _token[0] = 0;
  _listening = false;
  _authed = false;
  _client_ip[0] = 0;
  _in_len = 0;
  _rx_head = _rx_count = 0;
  _rx_pushed = _rx_dropped = _tx_ok = _tx_refused = _sock_full = 0;
  _stall_since = 0;
  _out_head = _out_len = 0;
  _droogte_s = OH_DROOGTE_DEFAULT_S;
  _laatste_tx = 0;
  _rx_sinds_tx = 0;
  _fo_on = false;
  _fo_hold_s = OPENHOP_FO_HOLD_DEFAULT;
  _guest_seen = 0;
  _guest_back_since = 0;
  _guest_gone_since = 0;
  _fo_taken = false;
  _fo_count = 0;
  StrHelper::strncpy(_note, "uit", sizeof(_note));
}

void OpenHopTask::begin(fs::FS* fs, WifiTask* wifi, RoomMesh* mesh) {
  _fs = fs; _wifi = wifi; _mesh = mesh;
  loadConfig();
  if (_on) startServer();
}

void OpenHopTask::loadConfig() {
  if (_fs == nullptr || !_fs->exists(OPENHOP_CFG_PATH)) return;
  fs::File f = _fs->open(OPENHOP_CFG_PATH, "r");
  if (!f) return;
  /* Drie regels: aan/uit, poort, token. Een ontbrekende regel laat de
   * standaardwaarde staan -- zo blijft een bestand van een oudere versie
   * leesbaar en gaat er nooit een instelling verloren die er niet in staat. */
  char line[OPENHOP_TOKEN_MAX + 8];
  int n = 0;
  while (f.available() && n < 6) {
    size_t len = 0;
    while (f.available() && len < sizeof(line) - 1) {
      int ch = f.read();
      if (ch < 0 || ch == 10) break;
      if (ch == 13) continue;
      line[len++] = (char)ch;
    }
    line[len] = 0;
    if (n == 0) _on = (line[0] == '1');
    else if (n == 1) {
      long v = strtol(line, nullptr, 10);
      if (v >= 1 && v <= 65535) _port = (uint16_t)v;
    } else if (n == 2) {
      StrHelper::strncpy(_token, line, sizeof(_token));
    } else if (n == 3) {
      _fo_on = (line[0] == '1');
    } else if (n == 4) {
      long v = strtol(line, nullptr, 10);
      if (v >= OPENHOP_FO_HOLD_MIN && v <= OPENHOP_FO_HOLD_MAX) _fo_hold_s = (uint16_t)v;
    } else {
      /* Regel 6: de droogtetijd. Ontbreekt hij (bestand van voor v2.23.0),
       * dan blijft de standaard staan, en dat is de bedoeling. */
      long v = strtol(line, nullptr, 10);
      if (v == 0 || (v >= OH_DROOGTE_MIN_S && v <= OH_DROOGTE_MAX_S)) _droogte_s = (uint16_t)v;
    }
    n++;
  }
  f.close();
}

void OpenHopTask::saveConfig() {
  if (_fs == nullptr) return;
  fs::File f = _fs->open(OPENHOP_CFG_PATH, "w", true);
  if (!f) return;
  f.println(_on ? "1" : "0");
  f.println((int)_port);
  f.println(_token);
  f.println(_fo_on ? "1" : "0");
  f.println((int)_fo_hold_s);
  f.println((int)_droogte_s);
  f.close();
}

void OpenHopTask::setEnabled(bool on) {
  if (on == _on) return;
  _on = on;
  saveConfig();
  if (_on) startServer(); else stopServer();
}

void OpenHopTask::setPort(uint16_t p) {
  if (p < 1 || p == _port) return;
  _port = p;
  saveConfig();
  /* De luisteraar moet om: een WiFiServer bindt zijn poort bij het starten. */
  if (_on) { stopServer(); startServer(); }
}

void OpenHopTask::setToken(const char* t) {
  StrHelper::strncpy(_token, t ? t : "", sizeof(_token));
  saveConfig();
  /* Een lopende sessie is aangegaan onder de OUDE regels. Wie het wachtwoord
   * verandert, verwacht dat het meteen geldt -- ook voor wie al binnen is. */
  dropClient("token gewijzigd");
}

/* ------------------------------------------------------------------------
 * De luisteraar
 * ------------------------------------------------------------------------ */
void OpenHopTask::startServer() {
#ifdef ESP32
  if (_listening) return;
  _server = WiFiServer(_port);
  _server.begin();
  _server.setNoDelay(true);
  _listening = true;
  snprintf(_note, sizeof(_note), "luistert op poort %u", (unsigned)_port);
  OH_DIAG("brug aan, poort %u", (unsigned)_port);
#endif
}

void OpenHopTask::stopServer() {
#ifdef ESP32
  dropClient("brug uitgezet");
  if (_listening) { _server.end(); _listening = false; }
  StrHelper::strncpy(_note, "uit", sizeof(_note));
  OH_DIAG("brug uit");
#endif
}

void OpenHopTask::dropClient(const char* waarom) {
#ifdef ESP32
  if (_cl) {
    _cl.stop();
    OH_DIAG("host losgekoppeld: %s", waarom ? waarom : "");
  }
#endif
  _authed = false;
  _client_ip[0] = 0;
  _in_len = 0;
  _rx_head = _rx_count = 0;   /* wat er nog klaarstond hoort bij de vorige sessie */
}

bool OpenHopTask::clientConnected() const {
#ifdef ESP32
  return const_cast<WiFiClient&>(_cl).connected();
#else
  return false;
#endif
}

/* ------------------------------------------------------------------------
 * Frames schrijven
 * ------------------------------------------------------------------------ */
bool OpenHopTask::sendFrame(uint8_t cmd, const uint8_t* payload, size_t len,
                            bool alleen_als_plaats) {
#ifdef ESP32
  if (!clientConnected()) return false;

  /* DE HOOFDLUS WACHT NIET. WiFiClient::write() blokkeert tot de bytes weg
   * kunnen; leest de host even niet, dan staat deze node stil -- geen
   * webserver, geen mesh. Een socket-timeout maakt dat korter, niet goed: twee
   * seconden per frame was genoeg om de node zichzelf als "stil" te laten
   * melden en openHop zijn TX_DONE te laten missen.
   *
   * Dus schrijven we hier niets. We zetten het frame in de uitgaande buffer;
   * pumpTx() leegt die elke ronde met MSG_DONTWAIT. */
  const size_t nodig = 4 + len + 2;
  if (nodig > OH_OUT_BUF) return false;          /* kan nooit passen */

  if (!outRoom(nodig)) {
    if (alleen_als_plaats) {
      /* Een RX_PACKET mag vallen: de host mist een pakket, en dat is beter dan
       * een node die wacht. */
      _sock_full++;
      return false;
    }
    /* Een antwoord (PONG, AUTH_OK, CONFIG_RESP, TX_DONE) is klein en
     * noodzakelijk -- zonder TX_DONE blijft openHop eindeloos opnieuw vragen.
     * Daarvoor maken we plaats door de buffer te laten vallen: die bevat dan
     * toch vooral RX-frames die de host niet aankan. */
    _rx_dropped += (uint32_t)(_out_len / 64);    /* ruwe schatting, eerlijk laag */
    _out_head = 0;
    _out_len = 0;
    _sock_full++;
    if (!outRoom(nodig)) return false;
  }

  uint8_t hdr[4];
  hdr[0] = OH_SYNC;
  hdr[1] = cmd;
  hdr[2] = (uint8_t)(len & 0xFF);
  hdr[3] = (uint8_t)((len >> 8) & 0xFF);
  uint16_t crc = crc16(&hdr[1], 3);
  /* De CRC loopt over CMD+LEN+PAYLOAD; het tweede stuk hier. */
  if (payload && len) {
    uint16_t c = crc;
    for (size_t i = 0; i < len; i++) {
      c ^= (uint16_t)payload[i] << 8;
      for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    crc = c;
  }
  uint8_t tail[2] = { (uint8_t)(crc & 0xFF), (uint8_t)(crc >> 8) };

  outPush(hdr, 4);
  if (payload && len) outPush(payload, len);
  outPush(tail, 2);
  pumpTx();                 /* meteen proberen; wat niet kan, blijft staan */
  return true;
#else
  return false;
#endif
}

/* Bytes achteraan de ringloze buffer. De aanroeper heeft de plaats al
 * gecontroleerd met outRoom(). */
void OpenHopTask::outPush(const uint8_t* p, size_t n) {
#ifdef ESP32
  if (_out_head > 0 && (size_t)(_out_head + _out_len + n) > OH_OUT_BUF) {
    memmove(_out, _out + _out_head, _out_len);   /* schuif naar voren */
    _out_head = 0;
  }
  memcpy(_out + _out_head + _out_len, p, n);
  _out_len = (uint16_t)(_out_len + n);
#endif
}

/* Wat weg kan, gaat weg. Meer niet. */
void OpenHopTask::pumpTx() {
#ifdef ESP32
  if (_out_len == 0) { _stall_since = 0; return; }
  if (!clientConnected()) { _out_head = _out_len = 0; return; }

  int fd = _cl.fd();
  if (fd < 0) return;

  while (_out_len > 0) {
    int n = ::send(fd, _out + _out_head, _out_len, MSG_DONTWAIT);
    if (n > 0) {
      _out_head = (uint16_t)(_out_head + n);
      _out_len  = (uint16_t)(_out_len - n);
      if (_out_len == 0) { _out_head = 0; _stall_since = 0; }
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      /* Zendvenster vol. Niet wachten -- volgende ronde opnieuw. */
      if (_stall_since == 0) _stall_since = millis();
      else if (millis() - _stall_since >= OH_STALL_DROP_MS * 6) {
        dropClient("host neemt niets meer aan");
      }
      return;
    }
    /* Echte fout op de socket. */
    dropClient("schrijffout op de socket");
    return;
  }
#endif
}


void OpenHopTask::sendError(uint8_t code) {
  sendFrame(OH_CMD_ERROR, &code, 1);
}

/* ------------------------------------------------------------------------
 * Ontvangen pakketten: kopieren in de meshlus, schrijven in onze eigen lus
 * ------------------------------------------------------------------------ */
void OpenHopTask::onRawRx(float snr, float rssi, const uint8_t* raw, int len) {
  if (!_on || !_authed || len <= 0 || len > (int)(OH_MAX_PAYLOAD - 6)) return;
  if (!clientConnected()) return;
  if (_rx_count >= OH_RX_RING) {
    /* Vol. De OUDSTE laten vallen: bij een mesh is het verse pakket het
     * bruikbare, en een teller vertelt achteraf dat het gebeurd is. */
    _rx_head = (uint8_t)((_rx_head + 1) % OH_RX_RING);
    _rx_count--;
    _rx_dropped++;
  }
  uint8_t slot = (uint8_t)((_rx_head + _rx_count) % OH_RX_RING);
  uint8_t* p = _rx[slot];
  int16_t rssi_i = (int16_t)rssi;
  int16_t snr10  = (int16_t)(snr * 10.0f);
  /* Hun vorm: rssi(i16) | snr x10(i16) | signaal-rssi(i16) | de ruwe bytes.
   * Een aparte signaal-RSSI meet deze radio niet; dan is de gewone RSSI de
   * eerlijkste waarde -- een verzonnen tweede getal zou daar niet beter van
   * worden. */
  memcpy(&p[0], &rssi_i, 2);
  memcpy(&p[2], &snr10, 2);
  memcpy(&p[4], &rssi_i, 2);
  memcpy(&p[6], raw, len);
  _rx_len[slot] = (uint16_t)(len + 6);
  _rx_count++;
}

void OpenHopTask::flushRxRing() {
  while (_rx_count > 0) {
    if (!sendFrame(OH_CMD_RX_PACKET, _rx[_rx_head], _rx_len[_rx_head], true)) {
      /* Past niet. Deze ronde niet blijven duwen -- anders staat de hele ring
       * achter een frame te wachten dat er nu eenmaal niet in gaat. */
      const unsigned long nu = millis();
      if (_stall_since == 0) { _stall_since = nu; return; }
      if (nu - _stall_since < OH_STALL_DROP_MS) return;

      /* Lang genoeg dicht: de ring weg, en EEN verlies per frame tellen. Zo
       * blijft het getal betekenen wat het zegt en loopt de doorstroom weer. */
      _rx_dropped += _rx_count;
      _rx_head = _rx_count = 0;
      if (nu - _stall_since >= OH_STALL_DROP_MS * 6) {
        /* Hij leest al een halve minuut niets. Dan is de verbinding er wel maar
         * de host niet, en laten we hem los zodat hij schoon kan terugkomen --
         * meteen ook het signaal waar de failover op wacht. */
        dropClient("host neemt niets meer aan");
      }
      return;
    }
    _stall_since = 0;
    _rx_head = (uint8_t)((_rx_head + 1) % OH_RX_RING);
    _rx_count--;
    _rx_pushed++;
    _rx_sinds_tx++;
  }
  _stall_since = 0;
}

/* ------------------------------------------------------------------------
 * De commando's
 * ------------------------------------------------------------------------ */
void OpenHopTask::cmdGetConfig() {
  uint32_t freq, bw; uint8_t sf, cr; int8_t pwr;
  _mesh->ohRadioParams(freq, bw, sf, cr, pwr);
  uint8_t p[14];
  memcpy(&p[0], &freq, 4);
  memcpy(&p[4], &bw, 4);
  p[8] = sf; p[9] = cr; p[10] = (uint8_t)pwr;
  uint16_t sync = 0x12;          /* MeshCore-syncword */
  memcpy(&p[11], &sync, 2);
  p[13] = 16;                    /* preamble */
  sendFrame(OH_CMD_CONFIG_RESP, p, sizeof(p));
}

void OpenHopTask::cmdSetConfig(const uint8_t* payload, size_t len) {
  if (len < 14) { sendError(OH_ERR_INVALID_CONFIG); return; }
  uint32_t freq, bw; uint8_t sf, cr;
  memcpy(&freq, &payload[0], 4);
  memcpy(&bw, &payload[4], 4);
  sf = payload[8]; cr = payload[9];

  uint32_t ofreq, obw; uint8_t osf, ocr; int8_t opwr;
  _mesh->ohRadioParams(ofreq, obw, osf, ocr, opwr);

  /* EEN GAST VERZET ONZE RADIO NIET. Frequentie, bandbreedte, spreiding en
   * codering bepalen in welk mesh deze node staat; die laten verzetten door een
   * daemon die net opstartte zou de repeater uit zijn eigen netwerk tillen. We
   * vergelijken met een kleine marge op de frequentie, want de host rekent in
   * hele hertz en wij bewaren een float. */
  uint32_t verschil = (freq > ofreq) ? (freq - ofreq) : (ofreq - freq);
  if (verschil > 2000 || bw != obw || sf != osf || cr != ocr) {
    snprintf(_note, sizeof(_note), "SET_CONFIG geweigerd (%lu/%lu/%u/%u gevraagd)",
             (unsigned long)freq, (unsigned long)bw, (unsigned)sf, (unsigned)cr);
    OH_DIAG("%s -- onze stand: %lu/%lu/%u/%u", _note,
            (unsigned long)ofreq, (unsigned long)obw, (unsigned)osf, (unsigned)ocr);
    sendError(OH_ERR_INVALID_CONFIG);
    /* En meteen zeggen wat het WEL is, zodat de overkant het kan rechtzetten
     * zonder dat iemand in een logboek hoeft te duiken. */
    cmdGetConfig();
    return;
  }
  /* Zendvermogen, syncword en preamble nemen we aan zonder ze toe te passen:
   * ook die horen bij ons, maar ze zetten deze node niet buiten het mesh. */
  cmdGetConfig();
}

void OpenHopTask::cmdStatus() {
  /* Hun StatusResp: uptime | rx | tx | crc_errors | last_rssi | snr x10 |
   * noise x10 | temp | radio_state  ("<IIIIhhhbB", 24 byte). */
  uint8_t p[24];
  uint32_t uptime = millis() / 1000;
  uint32_t rx = _mesh->getNumRecvFlood() + _mesh->getNumRecvDirect();
  uint32_t tx = _mesh->getNumSentFlood() + _mesh->getNumSentDirect();
  uint32_t crc_err = 0;
  int16_t rssi = (int16_t)_mesh->ohLastRssi();
  int16_t snr10 = (int16_t)(_mesh->ohLastSnr() * 10.0f);
  int16_t noise10 = (int16_t)(_mesh->ohNoiseFloor() * 10);
  int8_t temp = 0;
  uint8_t state = _mesh->ohRadioBusy() ? 1 : 0;
  memcpy(&p[0], &uptime, 4);
  memcpy(&p[4], &rx, 4);
  memcpy(&p[8], &tx, 4);
  memcpy(&p[12], &crc_err, 4);
  memcpy(&p[16], &rssi, 2);
  memcpy(&p[18], &snr10, 2);
  memcpy(&p[20], &noise10, 2);
  p[22] = (uint8_t)temp;
  p[23] = state;
  sendFrame(OH_CMD_STATUS_RESP, p, sizeof(p));
}

void OpenHopTask::cmdNoise() {
  int16_t noise10 = (int16_t)(_mesh->ohNoiseFloor() * 10);
  sendFrame(OH_CMD_NOISE_RESP, (uint8_t*)&noise10, 2);
}

void OpenHopTask::cmdTx(const uint8_t* payload, size_t len) {
  /* Het VERZOEK is het levensteken voor de droogtemeting, niet pas de
   * geslaagde zending: een weigering omdat de lucht bezet is betekent nog
   * altijd dat openHop aan het repeteren is. */
  _laatste_tx = millis();
  _rx_sinds_tx = 0;
  if (len == 0 || len > 255) { _tx_refused++; sendError(OH_ERR_PAYLOAD_TOO_BIG); return; }
  uint32_t airtime = 0;
  if (!_mesh->ohInjectRaw(payload, (int)len, &airtime)) {
    /* De wachtrij zat vol of het was geen leesbaar pakket. TX_FAIL en niet
     * TX_DONE: "verzonden" zeggen over iets dat nooit de lucht in gaat is de
     * ene fout die deze hele firmware probeert te vermijden. */
    _tx_refused++;
    snprintf(_note, sizeof(_note), "TX geweigerd (%u byte)", (unsigned)len);
    sendFrame(OH_CMD_TX_FAIL, nullptr, 0);
    return;
  }
  _tx_ok++;
  uint32_t us = airtime * 1000UL;
  sendFrame(OH_CMD_TX_DONE, (uint8_t*)&us, 4);
}

void OpenHopTask::handleFrame(uint8_t cmd, const uint8_t* payload, size_t len) {
  /* De poort staat open op het LAN. Zolang er een token ingesteld is, mag er
   * niets gebeuren voordat dat token gezien is -- ook niet PING, want dat is
   * een gratis manier om te weten of hier iets luistert. */
  if (_token[0] != 0 && !_authed && cmd != OH_CMD_AUTH) {
    sendError(OH_ERR_UNAUTHORIZED);
    return;
  }

  switch (cmd) {
    case OH_CMD_AUTH: {
      char aangeboden[OPENHOP_TOKEN_MAX];
      size_t n = len < sizeof(aangeboden) - 1 ? len : sizeof(aangeboden) - 1;
      memcpy(aangeboden, payload, n);
      aangeboden[n] = 0;
      if (_token[0] == 0 || strcmp(aangeboden, _token) == 0) {
        _authed = true;
        sendFrame(OH_CMD_AUTH_OK, nullptr, 0);
        OH_DIAG("host aangemeld (%s)", _client_ip);
      } else {
        sendError(OH_ERR_UNAUTHORIZED);
        dropClient("verkeerd token");
      }
      break;
    }
    case OH_CMD_PING:
      /* PING en PONG hebben allebei 0xFF; het verschil zit in de richting. */
      sendFrame(OH_CMD_PONG, nullptr, 0);
      break;
    case OH_CMD_GET_VERSION: {
      const char* v = MESHUPTIME_VERSION " (MeshUptime-brug)";
      sendFrame(OH_CMD_VERSION_RESP, (const uint8_t*)v, strlen(v));
      break;
    }
    case OH_CMD_GET_CONFIG:   cmdGetConfig(); break;
    case OH_CMD_SET_CONFIG:   cmdSetConfig(payload, len); break;
    case OH_CMD_STATUS_REQ:   cmdStatus(); break;
    case OH_CMD_NOISE_REQ:    cmdNoise(); break;
    case OH_CMD_TX_REQUEST:   cmdTx(payload, len); break;
    case OH_CMD_RX_START:
      /* Deze radio staat altijd in ontvangst -- daar is hij een repeater voor.
       * Bevestigen is dus eerlijk; er valt niets te starten. */
      sendFrame(OH_CMD_RX_STARTED, nullptr, 0);
      break;
    case OH_CMD_CAD_REQUEST: {
      /* Geen echte CAD: die zou de ontvangst onderbreken van een node die voor
       * het hele mesh luistert. Wat we wel eerlijk kunnen zeggen is of onze
       * radio op dit ogenblik bezet is. De host gebruikt dit als
       * Listen-Before-Talk en mag dat weten. */
      uint8_t bezet = _mesh->ohRadioBusy() ? 1 : 0;
      sendFrame(OH_CMD_CAD_RESP, &bezet, 1);
      break;
    }
    case OH_CMD_SET_CAD_PARAMS: {
      /* Aangenomen, niet toegepast: onze zendlus doet zijn eigen afweging. */
      uint8_t ok[4] = {0, 0, 0, 0};
      sendFrame(OH_CMD_CAD_PARAMS_RESP, ok, sizeof(ok));
      break;
    }
    case OH_CMD_SET_WIFI:
    case OH_CMD_WIFI_RESET:
    case OH_CMD_GET_WIFI:
      /* NIET. Deze node heeft zijn eigen weg voor netwerkinstellingen, achter
       * een login. Een TCP-poort op het LAN is niet de plek om de wifi van een
       * draaiende repeater om te gooien. */
      OH_DIAG("wifi-commando 0x%02X geweigerd", cmd);
      sendError(OH_ERR_INVALID_CMD);
      break;
    default:
      sendError(OH_ERR_INVALID_CMD);
      break;
  }
}

/* ------------------------------------------------------------------------
 * De lus
 * ------------------------------------------------------------------------ */
void OpenHopTask::readSocket() {
#ifdef ESP32
  /* Een begrensde hap per ronde: de meshlus mag hier niet op wachten. */
  int budget = 512;
  while (_cl.available() > 0 && budget-- > 0) {
    if (_in_len >= sizeof(_in)) { _in_len = 0; }        /* kan niet, maar dan niet vastlopen */
    int b = _cl.read();
    if (b < 0) break;
    /* Op zoek naar de SYNC: alles ervoor is rommel of een half frame van een
     * vorige sessie. */
    if (_in_len == 0 && (uint8_t)b != OH_SYNC) continue;
    _in[_in_len++] = (uint8_t)b;

    if (_in_len < 4) continue;                          /* SYNC + CMD + LEN */
    uint16_t plen = (uint16_t)_in[2] | ((uint16_t)_in[3] << 8);
    if (plen > OH_MAX_PAYLOAD) {                        /* onzin; opnieuw uitlijnen */
      _in_len = 0;
      sendError(OH_ERR_PAYLOAD_TOO_BIG);
      continue;
    }
    if (_in_len < (size_t)(4 + plen + 2)) continue;     /* nog niet compleet */

    uint16_t gekregen = (uint16_t)_in[4 + plen] | ((uint16_t)_in[5 + plen] << 8);
    uint16_t verwacht = crc16(&_in[1], 3 + plen);
    if (gekregen == verwacht) {
      _guest_seen = millis();   /* levensteken voor de failover */
      _laatste_tx = _guest_seen;   /* krediet: hij mag eerst nog beginnen */
      _rx_sinds_tx = 0;
      handleFrame(_in[1], &_in[4], plen);
    } else {
      OH_DIAG("frame met foute CRC (cmd 0x%02X, %u byte)", _in[1], (unsigned)plen);
      sendError(0x01 /* ERR_CRC_MISMATCH */);
    }
    _in_len = 0;
  }
#endif
}


/* ------------------------------------------------------------------------
 * DE FAILOVER (v2.22.0)
 *
 * WAAROM DIT OP DE NODE ZIT EN NIET OP DE SERVER. Juist als openHop wegvalt is
 * er vaak meer weg: de container, het LAN, de wifi. Een failover die zelf over
 * het netwerk moet praten faalt dan mee. Deze kijkt alleen naar wat hij van hier
 * kan zien, en dat is genoeg.
 *
 * WAT "WEG" BETEKENT. Niet "de socket is dicht" alleen -- ook een daemon die nog
 * verbonden is maar vastgelopen. Elk geldig frame van de host is een levensteken,
 * en hun driver stuurt uit zichzelf PING's; blijft het OPENHOP_FO_HOLD_DEFAULT
 * seconden helemaal stil, dan is hij weg.
 *
 * DE FAALRICHTING IS DE VEILIGE. Raakt deze node zijn eigen netwerk kwijt, dan
 * ziet hij "gast weg" en gaat hij repeteren. Dat is precies wat je wil als de
 * slimme helft onbereikbaar is.
 *
 * WAT HIJ NOOIT DOET: iets terugzetten dat hij niet zelf omzette. Stond het
 * doorsturen al aan, dan valt er niets over te nemen en gebeurt er niets. En de
 * omzetting gaat ALLEEN in RAM: na een herstart staat de node op de stand die de
 * eigenaar koos en beslist de failover opnieuw. Een overname die stilletjes
 * blijvend wordt, is een instelling die niemand meer kan navertellen.
 * ------------------------------------------------------------------------ */
bool OpenHopTask::nodeForwarding() const {
  return _mesh != nullptr && _mesh->ohForwarding();
}

void OpenHopTask::setFailover(bool on) {
  if (on == _fo_on) return;
  _fo_on = on;
  /* Uitzetten terwijl we het overgenomen hadden: netjes teruggeven, anders
   * blijft de node repeteren om een reden die niet meer bestaat. */
  if (!_fo_on && _fo_taken && _mesh) {
    _mesh->ohSetForwarding(false);
    _fo_taken = false;
    OH_DIAG("failover uit; doorsturen weer aan de gast gelaten");
  }
  saveConfig();
}

void OpenHopTask::setDroogte(uint16_t s) {
  if (s != 0) {
    if (s < OH_DROOGTE_MIN_S) s = OH_DROOGTE_MIN_S;
    if (s > OH_DROOGTE_MAX_S) s = OH_DROOGTE_MAX_S;
  }
  _droogte_s = s;
  _laatste_tx = millis();   /* opnieuw krediet geven */
  _rx_sinds_tx = 0;
  saveConfig();
}

void OpenHopTask::setFailoverHold(uint16_t s) {
  if (s < OPENHOP_FO_HOLD_MIN) s = OPENHOP_FO_HOLD_MIN;
  if (s > OPENHOP_FO_HOLD_MAX) s = OPENHOP_FO_HOLD_MAX;
  _fo_hold_s = s;
  saveConfig();
}

void OpenHopTask::failoverTick() {
  if (!_fo_on || _mesh == nullptr) return;
  const unsigned long nu = millis();
  const unsigned long hold = (unsigned long)_fo_hold_s * 1000UL;

  /* De VERBINDING is het levensteken. Een host die alleen luistert is stil, en
   * dat is geen storing; pas een verbonden host die een veelvoud van de
   * wachttijd geen byte meer stuurde, is vastgelopen. */
  const bool verbonden = clientConnected();
  const bool vastgelopen = verbonden && _guest_seen != 0 &&
      (nu - _guest_seen) > hold * OPENHOP_FO_STALL_MULT;
  /* DROOG: verbonden, wij reiken hem pakketten aan, en er komt al die tijd
   * geen enkel zendverzoek terug. Dan repeteert hij niet -- bijvoorbeeld omdat
   * zijn andere kop weg is en hij in bridge-stand alles daarheen stuurt. */
  const bool droog = verbonden && _droogte_s > 0 &&
      _rx_sinds_tx >= OH_DROOGTE_MIN_RX && _laatste_tx != 0 &&
      (nu - _laatste_tx) >= (unsigned long)_droogte_s * 1000UL;

  const bool levend = verbonden && !vastgelopen && !droog;

  if (levend) {
    _guest_gone_since = 0;
    if (_guest_back_since == 0) _guest_back_since = nu;
  } else {
    _guest_back_since = 0;
    if (_guest_gone_since == 0) _guest_gone_since = nu;
  }

  /* Pas overnemen als hij ONAFGEBROKEN lang genoeg weg is. */
  if (!levend && !_fo_taken && (nu - _guest_gone_since) >= hold) {
    if (_mesh->ohForwarding()) return;      /* stond al aan; niets over te nemen */
    _mesh->ohSetForwarding(true);
    _fo_taken = true;
    _fo_count++;
    snprintf(_note, sizeof(_note), "FAILOVER: %s, node repeteert nu zelf",
             droog ? "gast zendt niet meer" : "gast weg");
    OH_DIAG("%s (na %u s stilte)", _note, (unsigned)_fo_hold_s);
    _mesh->dispatchAlert(MON_ALERT_BOTH, 0xFFFF, true,
                         "openHop weg -- deze node neemt het repeteren over",
                         MON_SEV_HIGH);
    return;
  }

  if (levend && _fo_taken && (nu - _guest_back_since) >= hold) {
    _mesh->ohSetForwarding(false);
    _fo_taken = false;
    snprintf(_note, sizeof(_note), "gast terug; repeteren weer aan openHop");
    OH_DIAG("%s", _note);
    /* Herstel is altijd laag/groen -- zelfde regel als bij de bewakingen. */
    _mesh->dispatchAlert(MON_ALERT_BOTH, 0xFFFF, false,
                         "openHop is terug -- deze node stopt weer met repeteren",
                         MON_SEV_LOW);
  }
}

void OpenHopTask::loop() {
#ifdef ESP32
  /* De failover kijkt ook als de brug uit staat NIET: zonder brug is er geen
   * gast en valt er niets over te nemen. Wel voor de wifi-controle hieronder,
   * want een node zonder netwerk heeft ook geen host. */
  if (!_on) return;
  /* Eerst de uitgaande buffer: die moet ook leeglopen als er even geen
   * nieuw verkeer is, anders blijft een TX_DONE hangen tot het volgende
   * pakket toevallig langskomt. */
  pumpTx();
  failoverTick();
  if (_wifi == nullptr || !_wifi->isOnline()) {
    /* Zonder netwerk valt er niets te bedienen. De luisteraar blijft staan;
     * WiFiServer komt vanzelf terug als de verbinding er weer is. */
    return;
  }
  if (!_listening) startServer();

  /* Een nieuwe host. Een tweede verbinding vervangt de eerste: een daemon die
   * herstart laat zijn oude socket soms hangen, en dan zou de nieuwe er nooit
   * in komen. */
  WiFiClient nieuw = _server.available();
  if (nieuw) {
    if (clientConnected()) dropClient("nieuwe host meldt zich");
    _cl = nieuw;
    _cl.setNoDelay(true);
    /* Tweede vangnet: mocht een write toch blijven hangen, dan is de schade
     * seconden in plaats van minuten. */
    /* Geen setTimeout meer nodig: er wordt hier nooit blokkerend
     * geschreven. pumpTx() gebruikt MSG_DONTWAIT. */
    _out_head = _out_len = 0;
    _in_len = 0;
    _authed = (_token[0] == 0);          /* geen token = meteen binnen */
    snprintf(_client_ip, sizeof(_client_ip), "%s", _cl.remoteIP().toString().c_str());
    _guest_seen = millis();
    snprintf(_note, sizeof(_note), "host %s verbonden", _client_ip);
    OH_DIAG("%s", _note);
  }

  if (!clientConnected()) {
    if (_client_ip[0]) { dropClient("verbinding weg"); }
    return;
  }

  readSocket();
  flushRxRing();
#endif
}
