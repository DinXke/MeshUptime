#pragma once

#include <Arduino.h>
#include <FS.h>
#ifdef ESP32
  #include <WiFi.h>
#endif

/* ============================================================================
 * OpenHopTask -- deze node als RADIO voor een openHop-host, zonder iets in te
 * leveren (v2.21.0).
 *
 * WAT OPENHOP IS. openHop (openhop-dev) is een Python-HERIMPLEMENTATIE van
 * MeshCore: zelfde protocol, zelfde mesh. De repeater-daemon draait op een
 * Linux-host en praat met zijn radio over een klein binair protocol -- over USB
 * of over TCP, poort 5055. Hun eigen firmware (`openhop_modem`) maakt van een
 * Heltec V3 een DOMME modem: alles wat deze node verder is -- room-server, bots,
 * IRC, de poller, de web-GUI, de bewakingen, de companion-hub -- is dan weg.
 *
 * DIT IS DE ANDERE KANT VAN DIE RUIL. Wij spreken hun modemprotocol, maar
 * blijven zelf de repeater. De openHop-host wordt een PASSAGIER op onze radio:
 * hij hoort alles wat wij horen en mag zenden via onze wachtrij. Dat kan alleen
 * omdat we hetzelfde protocol spreken -- het zijn dezelfde pakketten op dezelfde
 * frequentie; er valt niets te vertalen.
 *
 * DE DRIE REGELS DIE DEZE BRUG VEILIG HOUDEN
 *
 * 1. EEN GAST VERZET ONZE RADIO NIET. `SET_CONFIG` mag frequentie, bandbreedte,
 *    spreiding en codering niet veranderen: dat zou deze node uit zijn eigen
 *    mesh tillen omdat er aan de overkant een daemon start. Komt er een andere
 *    stand binnen dan de onze, dan antwoorden we ERR_INVALID_CONFIG met onze
 *    werkelijke waarden in CONFIG_RESP ernaast. Zendvermogen, syncword en
 *    preamble nemen we aan zonder ze toe te passen -- ook die horen bij ons.
 *
 * 2. EEN GAST DRINGT NIET VOOR. Wat de host wil zenden gaat in DEZELFDE
 *    wachtrij als ons eigen verkeer, met de laagste prioriteit en binnen
 *    hetzelfde zendtijdbudget. Is de wachtrij vol, dan zeggen we dat (TX_FAIL)
 *    in plaats van te doen alsof: een "verzonden" dat nooit de lucht in ging is
 *    precies de leugen waar de rest van deze firmware tegen gebouwd is.
 *
 * 3. EEN GAST HERCONFIGUREERT ONS NIET. De wifi- en OTA-commando's van hun
 *    protocol beantwoorden we met ERR_INVALID_CMD. Deze node heeft daar zijn
 *    eigen weg voor, achter een login; een TCP-poort op het LAN is niet de plek
 *    om het netwerk van een node om te gooien.
 *
 * WAT DE EIGENAAR ZELF MOET WETEN. Laat de openHop-daemon NIET ook repeteren.
 * Hij zit op onze antenne; als hij zijn eigen doorstuurlogica aanzet gaat elk
 * floodpakket er twee keer uit -- een keer van ons en een keer van hem. Voor
 * meeluisteren, dashboards, plugins en MQTT is dat geen bezwaar; voor repeteren
 * wel. Dit is een keuze aan zijn kant en niet iets dat wij kunnen afdwingen.
 *
 * DE DRAADVORM (openhop_core/hardware/protocol_constants.py, v0.7):
 *
 *   SYNC 0xAA | CMD (1) | LEN (2, LE) | PAYLOAD | CRC-16/CCITT-FALSE (2, LE)
 *
 * De CRC loopt over CMD+LEN+PAYLOAD, niet over de SYNC. Polynoom 0x1021, start
 * 0xFFFF, niet gespiegeld, geen xor-out.
 * ==========================================================================*/

#define OPENHOP_CFG_PATH      "/openhop.cfg"
#define OPENHOP_PORT_DEFAULT  5055
#define OPENHOP_TOKEN_MAX     33      /* 32 tekens + afsluiter */

/* FAILOVER (v2.22.0). Hoe lang de gast weg moet zijn voordat de node zelf
 * weer gaat repeteren, en hoe lang hij terug moet zijn voordat de node het
 * weer uit handen geeft. Een wifi-hik van een paar tellen hoort geen
 * omschakeling te worden; een minuut is lang genoeg om dat te filteren en
 * kort genoeg om een echte uitval niet te laten liggen. */
#define OPENHOP_FO_HOLD_DEFAULT  60
#define OPENHOP_FO_HOLD_MIN      10
#define OPENHOP_FO_HOLD_MAX      3600
/* Een VERBONDEN host die niets zegt is normaal: hun driver praat alleen als
 * er iets te zenden valt. Pas na dit veelvoud van de wachttijd zonder enig
 * frame noemen we hem vastgelopen -- dan is het geen rustige minuut meer. */
#define OPENHOP_FO_STALL_MULT    10

/* Het protocol. Namen letterlijk uit protocol_constants.py, zodat ze naast
 * elkaar te leggen zijn zonder te hoeven vertalen. */
#define OH_SYNC                 0xAA
/* host -> modem */
#define OH_CMD_TX_REQUEST       0x01
#define OH_CMD_SET_CONFIG       0x10
#define OH_CMD_GET_CONFIG       0x11
#define OH_CMD_STATUS_REQ       0x20
#define OH_CMD_NOISE_REQ        0x22
#define OH_CMD_CAD_REQUEST      0x30
#define OH_CMD_RX_START         0x31
#define OH_CMD_SET_CAD_PARAMS   0x34
#define OH_CMD_SET_WIFI         0x41
#define OH_CMD_AUTH             0x50
#define OH_CMD_WIFI_RESET       0x60
#define OH_CMD_GET_WIFI         0x61
#define OH_CMD_GET_VERSION      0x70
#define OH_CMD_PING             0xFF
/* modem -> host */
#define OH_CMD_TX_DONE          0x02
#define OH_CMD_TX_FAIL          0x03
#define OH_CMD_RX_PACKET        0x04
#define OH_CMD_CONFIG_RESP      0x12
#define OH_CMD_STATUS_RESP      0x21
#define OH_CMD_NOISE_RESP       0x23
#define OH_CMD_CAD_RESP         0x32
#define OH_CMD_RX_STARTED       0x33
#define OH_CMD_CAD_PARAMS_RESP  0x35
#define OH_CMD_AUTH_OK          0x51
#define OH_CMD_VERSION_RESP     0x71
#define OH_CMD_ERROR            0xFE
#define OH_CMD_PONG             0xFF
/* foutcodes (payload[0] bij OH_CMD_ERROR) */
#define OH_ERR_INVALID_CMD      0x02
#define OH_ERR_RADIO_BUSY       0x03
#define OH_ERR_PAYLOAD_TOO_BIG  0x05
#define OH_ERR_INVALID_CONFIG   0x06
#define OH_ERR_UNAUTHORIZED     0x09

/* Een LoRa-pakket is hoogstens 255 byte; met de zes byte signaalkop erbij is dit
 * de grootste frame-inhoud die we ooit sturen of ontvangen. */
#define OH_MAX_PAYLOAD          264
/* De ontvangstring. logRxRaw() draait in de meshlus; daar willen we niet op een
 * socket wachten, dus wordt er gekopieerd en pas in loop() geschreven. Zes
 * plaatsen is ruim een seconde aan druk verkeer en kost ~1,6 kB. */
#define OH_RX_RING              6
/* Zolang de host niets aanneemt: na dit stilstaan de ring weggooien (en dat
 * eerlijk tellen), en na zes keer zo lang de verbinding loslaten zodat hij
 * opnieuw kan verbinden. */
#define OH_STALL_DROP_MS        5000UL

class RoomMesh;
class WifiTask;

class OpenHopTask {
public:
  OpenHopTask() { reset(); }

  void begin(fs::FS* fs, WifiTask* wifi, RoomMesh* mesh);
  void loop();

  /* Elk RUW ontvangen pakket, rechtstreeks uit Dispatcher::logRxRaw(). Kopieert
   * alleen; het schrijven gebeurt in loop(). Veilig als de brug uit staat of er
   * geen client is -- dan valt het meteen weg. */
  void onRawRx(float snr, float rssi, const uint8_t* raw, int len);

  /* ---- instellingen (web-GUI) ---- */
  bool     enabled() const      { return _on; }
  void     setEnabled(bool on);
  uint16_t port() const         { return _port; }
  void     setPort(uint16_t p);
  bool     tokenSet() const     { return _token[0] != 0; }
  /* ---- failover ---- */
  bool     failover() const     { return _fo_on; }
  void     setFailover(bool on);
  uint16_t failoverHold() const { return _fo_hold_s; }
  void     setFailoverHold(uint16_t s);
  /* Heeft de node het repeteren NU van de gast overgenomen? */
  bool     failoverActive() const { return _fo_taken; }
  uint32_t failoverCount() const  { return _fo_count; }
  /* De huidige doorstuurstand van de node, zodat de GUI kan tonen wie er nu
   * repeteert zonder zelf de mesh te hoeven kennen. */
  bool     nodeForwarding() const;
  void     setToken(const char* t);

  /* ---- stand (/openhop.json) ---- */
  bool        clientConnected() const;
  const char* clientIp() const     { return _client_ip; }
  uint32_t    rxPushed() const     { return _rx_pushed; }
  uint32_t    rxDropped() const    { return _rx_dropped; }
  uint32_t    txAccepted() const   { return _tx_ok; }
  uint32_t    txRefused() const    { return _tx_refused; }
  /* Frames die we lieten vallen omdat de zendbuffer vol zat. Zie sendFrame:
   * wachten zou de hele node stilzetten. */
  uint32_t    sockFull() const     { return _sock_full; }
  const char* lastNote() const     { return _note; }

private:
  fs::FS*   _fs;
  WifiTask* _wifi;
  RoomMesh* _mesh;

  bool      _on;
  uint16_t  _port;
  char      _token[OPENHOP_TOKEN_MAX];

#ifdef ESP32
  WiFiServer _server;
  WiFiClient _cl;
#endif
  bool      _listening;
  bool      _authed;        /* AUTH gedaan (of niet nodig omdat er geen token staat) */
  char      _client_ip[16];

  /* De inkomende frame-assemblage. Een TCP-stroom levert stukjes, geen frames. */
  uint8_t   _in[OH_MAX_PAYLOAD + 8];
  size_t    _in_len;

  /* De ontvangstring: [len:2][rssi:2][snr10:2][data...] per plaats. */
  uint8_t   _rx[OH_RX_RING][OH_MAX_PAYLOAD];
  uint16_t  _rx_len[OH_RX_RING];
  uint8_t   _rx_head, _rx_count;

  uint32_t  _rx_pushed, _rx_dropped, _tx_ok, _tx_refused, _sock_full;
  unsigned long _stall_since;   /* sinds wanneer neemt hij niets meer aan? */

  /* De failover. _guest_seen is het laatste LEVENSTEKEN van de host: elk
   * geldig frame telt, en hun driver stuurt uit zichzelf PING's. Zo valt een
   * daemon die nog verbonden is maar vastgelopen, ook op.
   * _fo_taken onthoudt dat WIJ het aanzetten: de failover geeft alleen terug
   * wat hij zelf omzette, zodat hij nooit vecht met een keuze van de
   * eigenaar. */
  bool      _fo_on;
  uint16_t  _fo_hold_s;
  unsigned long _guest_seen;
  unsigned long _guest_back_since;
  unsigned long _guest_gone_since;   /* sinds wanneer missen we hem? */
  bool      _fo_taken;
  uint32_t  _fo_count;
  char      _note[80];

  void reset();
  void loadConfig();
  void saveConfig();

  void startServer();
  void stopServer();
  void dropClient(const char* waarom);

  /* Een frame de deur uit. false = er kon niet geschreven worden. */
  /* alleen_als_plaats: voor de RX-stroom. Antwoorden (PONG, AUTH_OK,
   * CONFIG_RESP, TX_DONE) moeten er gewoon uit -- zonder die hoort hun
   * driver niets terug en komt de verbinding nooit tot stand. */
  bool sendFrame(uint8_t cmd, const uint8_t* payload, size_t len,
                 bool alleen_als_plaats = false);
  void sendError(uint8_t code);

  void readSocket();
  void handleFrame(uint8_t cmd, const uint8_t* payload, size_t len);
  void flushRxRing();
  /* Elke ronde: is de gast er nog, en moeten we iets doen? */
  void failoverTick();

  void cmdSetConfig(const uint8_t* payload, size_t len);
  void cmdGetConfig();
  void cmdStatus();
  void cmdNoise();
  void cmdTx(const uint8_t* payload, size_t len);

  static uint16_t crc16(const uint8_t* data, size_t len);
};
