#pragma once

#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include <helpers/ClientACL.h>
#include <helpers/TxtDataHelpers.h>

/* DmCommands -- de sensoren opvragen met een gewoon tekstbericht over het mesh.
 *
 * WAAROM DIT BESTAAT, EN WAAROM HET BELANGRIJKER IS DAN HET LIJKT
 *
 * De telemetrie van deze node gaat als CayenneLPP over de radio. Dat formaat
 * draagt UITSLUITEND kanaalnummers: er is geen naamveld, in geen enkele versie.
 * Wie kanaal 7 leest, krijgt een 0 of een 1 en verder niets. De namen die in de
 * webinterface staan ("dak-repeater", "hoas") komen nooit voorbij de radio.
 *
 * Deze DM-interface is dus de ENIGE weg waarlangs een naam over het mesh gaat.
 * Daarmee is `list` geen gemak maar de sleutel bij de telemetrie: zonder die
 * lijst weet een ontvanger niet wat kanaal 7 betekent, en met een lijst die
 * verouderd is, leest hij verkeerde cijfers zonder foutmelding. Wie hier iets
 * verandert aan de vorm van `list`, verandert de enige naamdrager die er is.
 *
 * HOE DEZE CODE AAN DE SENSOREN KOMT
 *
 * Niet via MonitorSensors. Die klasse is in beweging (de ping-monitors op
 * kanaal 5..12 worden er nu ingebouwd) en haar interne opbouw is geen contract.
 * In de plaats daarvan staat hieronder DmDataSource: drie methodes, door
 * main.cpp geimplementeerd, want main.cpp is de enige plek die zowel de
 * sensorlaag als de wifi-taak als dit bestand kent. Zie DmCommands.cpp voor de
 * volledige beschrijving van wat main.cpp moet aanleveren.
 */

/* Vaste maten voor een sensorbeschrijving. Vaste buffers en geen pointers naar
 * geheugen van de aanroeper: zo hoeft de bron (main.cpp) niets in leven te
 * houden na het antwoord, en is er geen enkele allocatie nodig. */
#define DM_NAME_MAX     20
#define DM_STATE_MAX    16
#define DM_DETAIL_MAX   72

struct DmSensorInfo {
  uint8_t channel;                 // CayenneLPP-kanaal; 0 = geen kanaal
  char    name[DM_NAME_MAX];       // naam zoals in de webinterface
  char    state[DM_STATE_MAX];     // korte toestand, bv "op" / "neer" / "net"
  char    detail[DM_DETAIL_MAX];   // extra regel voor `get`; mag leeg zijn
};

/* De hele afhankelijkheid van deze module op de rest van MeshUptime. Klein
 * gehouden met opzet: elke methode erbij is een methode die de andere agent in
 * MonitorSensors moet blijven ondersteunen. */
class DmDataSource {
public:
  /* Hoeveel sensoren er zijn. Mag tussen twee aanroepen wijzigen. */
  virtual int dmSensorCount() = 0;

  /* Vult 'dest' voor sensor idx (0 .. dmSensorCount()-1). false = sla over.
   * De implementatie MOET alle velden zetten, ook de lege als "" . */
  virtual bool dmSensorAt(int idx, DmSensorInfo& dest) = 0;

  /* Een regel met de toestand van de node zelf: voeding en wifi, in de
   * woorden van de toepassing. Uptime en vrije heap zet DmCommands er zelf
   * bij -- die kent het bord, niet de toepassing. */
  virtual void dmStatusLine(char* dest, size_t max_len) = 0;

  /* Uitbreidingen voor sensorbeheer, bevestiging en ad-hoc ping over DM.
   * Standaard leeg (default-implementatie), zodat een DmDataSource die ze niet
   * kent gewoon list/get/status/help houdt. main.cpp's MonitorDmSource schakelt
   * ze door naar MonitorSensors. */
  virtual bool        dmIsAck(const uint8_t* data, size_t len) { (void)data; (void)len; return false; }
  virtual uint8_t     dmConfirmAlerts() { return 0; }
  virtual const char* dmMonCommand(const char* line) { (void)line; return nullptr; }
  virtual int         dmAdhocState() { return 0; }   /* 0 = ADHOC_NONE */
  virtual bool        dmAdhocReady() { return false; }
  virtual const char* dmAdhocResult() { return ""; }
  virtual void        dmAdhocClear() {}
  virtual const char* dmHelpExtra() { return nullptr; }

  /* NODE-/NETWERK-/BEDIEN-COMMANDO'S (batch). Schrijft het antwoord in 'out' en
   * geeft de lengte terug, of 0 als de regel geen node-commando is (dan valt
   * DmCommands door naar list/get/status/help/mon). `role` = de ACL-rol van de
   * afzender (PERM_ACL_*): read=1, readwrite=2, admin=3 -- de implementatie
   * handhaaft per commando het vereiste niveau. Sync commando's leveren meteen
   * tekst; een deferred commando (bv. ad-hoc net-taak) start iets en zet de adhoc-
   * state, waarna DmCommands de uitslag via dezelfde weg als ping bezorgt. */
  virtual int         dmNodeCommand(const char* line, uint8_t role, char* out, size_t out_len)
                        { (void)line; (void)role; (void)out; (void)out_len; return 0; }
};

/* ================== DE MAAT VAN EEN BERICHT ==================
 *
 * MAX_TEXT_LEN is 160 (helpers/BaseChatMesh.h:8, 10 * CIPHER_BLOCK_SIZE), maar
 * BaseChatMesh::composeMsgPacket weigert werk boven 158:
 *
 *     if (text_len > MAX_TEXT_LEN) return NULL;
 *     if (attempt > 3 && text_len > MAX_TEXT_LEN-2) return NULL;
 *
 * Een tekst van 159 of 160 byte gaat er de eerste keer wel uit, maar kan na de
 * vierde poging NIET MEER opnieuw verstuurd worden -- de tweede regel geeft dan
 * NULL terug. Zo'n bericht is dus eenmalig: komt de ACK niet, dan is het
 * antwoord definitief weg.
 *
 * Daarom knippen wij op 158 en NIET op 160. Dat kost twee byte per stuk en
 * levert op dat elk stuk onbeperkt herhaalbaar blijft -- ook door de
 * herhaallus verderop in dit bestand, en ook door elke client aan de andere
 * kant die dezelfde routine gebruikt. ZET DIT NIET TERUG NAAR 160.
 */
#define DM_CHUNK_MAX        158

/* Een stuknummer "[2/5] " is 6 byte, en BaseChatMesh::sendGroupMessage laat
 * zien wat er met een voorvoegsel gebeurt dat niet past:
 *
 *     if (text_len + prefix_len > MAX_TEXT_LEN) text_len = MAX_TEXT_LEN - prefix_len;
 *
 * STIL afkappen, zonder foutmelding. Wij rekenen die 6 byte dus vooraf van het
 * budget af in plaats van erop te vertrouwen dat het wel past: 152 byte tekst
 * per stuk. Bij een antwoord van EEN stuk zetten we geen voorvoegsel -- dat is
 * 6 byte zendtijd voor niets en "[1/1] " leest ook slechter -- en dan mag de
 * tekst de volle 158 byte lang zijn. */
#define DM_CHUNK_PREFIX_LEN 6
#define DM_CHUNK_TEXT_MAX   (DM_CHUNK_MAX - DM_CHUNK_PREFIX_LEN)

/* BOVENGRENS OP HET AANTAL STUKKEN.
 *
 * Vier. Niet omdat vier mooi is, maar omdat dit een gedeelde band is: iemand
 * die `list` typt, moet niet twintig berichten door het mesh kunnen duwen. Bij
 * SF11 kost een vol bericht ongeveer een seconde zendtijd, en die seconde is
 * van iedereen -- ook van de repeaters die hem doorgeven, waar hij dus nog
 * eens meetelt. Vier stukken is ~600 byte tekst; genoeg voor twaalf sensoren
 * met naam en toestand.
 *
 * Wat niet past, wordt NIET stil weggelaten: het laatste stuk eindigt met een
 * slotregel die zegt hoeveel regels er niet in gingen. Een afgekapt antwoord
 * dat er compleet uitziet, is erger dan geen antwoord. */
#define DM_MAX_CHUNKS       4

/* Ruimte die in het laatste toegestane stuk vrij blijft voor die slotregel. */
#define DM_TAIL_MAX         40

/* Werkbuffer voor het volledige antwoord voor het opknippen. Ruimer dan
 * DM_MAX_CHUNKS * DM_CHUNK_TEXT_MAX (608), omdat knippen op regelgrens per
 * stuk wat ruimte laat liggen; dan is de knipper de begrenzer en niet deze
 * buffer, en klopt de telling in de slotregel. */
#define DM_TEXT_BUF_LEN     768

/* Herhalen tot de ACK komt. Zie de opmerking bij TXT_TYPE_PLAIN in de .cpp:
 * we hebben PLAIN juist gekozen omdat het een ACK oplevert, en die kennis is
 * alleen iets waard als we er ook naar handelen. Drie pogingen per stuk; de
 * pogingteller is 2 bits breed (data[4] & 3), dus vier is het maximum dat het
 * formaat kan dragen en drie laat er een over. */
#define DM_MAX_ATTEMPTS     3

/* Tijden. Alles ruim: de radio gaat voor op een snel antwoord.
 *  - FIRST_SEND: de gewone ACK op het binnengekomen bericht mag eerst weg
 *  - ACK_TIMEOUT: wachten op de ACK van een stuk voor we het herhalen
 *  - SPACING: rust tussen twee opeenvolgende stukken */
#define DM_FIRST_SEND_MS    2000
#define DM_ACK_TIMEOUT_MS   20000
#define DM_SPACING_MS       4000

/* Hoe lang een afzender zonder rechten geen tweede weigering krijgt. Zie de
 * uitleg bij isAllowed() in de .cpp. */
#define DM_DENY_COOLDOWN_MS (10UL*60UL*1000UL)

class DmCommands {
public:
  DmCommands() { reset(); _data = NULL; _mesh = NULL; _path_hash_size = 1; _boot_time = 0;
                 memset(_deny_key, 0, sizeof(_deny_key)); _deny_until = 0; }

  /* mesh: voor createDatagram()/sendDirect()/sendFlood() en self_id -- alle
   * drie publiek op mesh::Mesh, dus hier is geen vriendschap met SensorMesh
   * nodig. data: de sensorlijst, door main.cpp aangeleverd. */
  void begin(mesh::Mesh* mesh, DmDataSource* data);

  /* Uit MyMesh::handleIncomingMsg(). true = behandeld, waarop SensorMesh de
   * gewone ACK stuurt; het eigenlijke antwoord gaat in stukken uit loop(). */
  bool handleDm(const ClientInfo& from, uint32_t timestamp, const uint8_t* text, size_t len);

  /* Uit MyMesh::onAckRecv(). true = dit was de ACK op ons stuk, en dan hoort
   * de aanroeper packet->markDoNotRetransmit() te doen. */
  bool onAck(uint32_t ack_crc);

  /* Uit de gewone loop(). Verstuurt HOOGSTENS EEN stuk per ronde. */
  void loop();

  /* ROOM-VARIANT: bouw ALLEEN de antwoordtekst op voor een room-commando, zonder
   * iets te versturen. Hergebruikt buildList/buildGet/buildStatus/buildHelp en de
   * mon-commando's (add/edit/del/ping) via dezelfde DmDataSource. Geeft de lengte
   * van de tekst in 'out' terug, of 0 als de regel geen herkend commando is (dan
   * is het een gewone room-post en hoeft er niets terug). RoomMesh knipt 'out'
   * daarna in room-posts. Verstuurt NIETS zelf -- de bezorging loopt via de
   * room-post-synchronisatie, niet via het DM-ACK-pad. */
  int  renderReply(const ClientInfo& from, int room_idx, const char* line, char* out, size_t out_len);

  bool isBusy() const { return _num_chunks > 0; }

  /* ROOM-OORSPRONG VOOR UITGESTELDE RESULTATEN. Een ad-hoc ping (en later andere
   * uitgestelde commando's) die IN een room gevraagd is, hoort zijn resultaat IN
   * die room terug te krijgen -- niet als DM. main_room.cpp geeft hier een callback
   * die de tekst met addServerPost naar de oorsprong-room post. Zonder callback valt
   * het terug op het DM-pad (het oude gedrag). */
  typedef void (*RoomPostFn)(void* ctx, int room_idx, const char* text);
  void setRoomPostCallback(RoomPostFn fn, void* ctx) { _room_post_fn = fn; _room_post_ctx = ctx; }

  /* Optioneel: main.cpp kan getNodePrefs()->path_hash_mode + 1 doorgeven, zoals
   * SensorMesh zelf doet bij het verzenden van waarschuwingen. Zonder deze
   * aanroep geldt 1, de standaard van sendFlood(). */
  void setPathHashSize(uint8_t n) { _path_hash_size = n; }

  /* Een net juist door renderReply() gestart uitgesteld resultaat NIET via loop()
   * naar een room laten afleveren. Gebruikt door het bot-DM-commandopad: dat kwam
   * niet uit een room (room_idx=-1) en levert de uitslag zelf af via botAdhocPoll().
   * Raakt alleen de routing-vlaggen; de onderliggende adhoc-toestand blijft staan. */
  void cancelPendingAdhocRouting() { _ping_wait = false; _ping_from_room = false; }

private:
  mesh::Mesh*   _mesh;
  DmDataSource* _data;
  uint8_t       _path_hash_size;
  uint32_t      _boot_time;      // RTC-tijd bij begin(), voor de uptime

  /* De ontvanger, GEKOPIEERD en niet als pointer of referentie bewaard: het
   * antwoord loopt over meerdere loop()-rondes, en in die tijd kan de ACL
   * herschreven worden (putClient/applyPermissions/acl.load). */
  mesh::Identity _to_id;
  /* De vrager van een ad-hoc ping, gekopieerd zoals _to_* -- het antwoord komt
   * pas rondes later en de ClientInfo achter de referentie kan dan overschreven
   * zijn. _ping_wait zegt of er een uitslag verwacht wordt. */
  bool          _ping_wait = false;
  /* Kwam de lopende ping uit een ROOM? Dan gaat het resultaat via de callback naar
   * die room (_ping_room_idx), niet als DM. */
  bool          _ping_from_room = false;
  int           _ping_room_idx = -1;
  mesh::Identity _ping_id;
  uint8_t       _ping_secret[PUB_KEY_SIZE];
  uint8_t       _ping_path[MAX_PATH_SIZE];
  uint8_t       _ping_path_len = 0;

  RoomPostFn    _room_post_fn = nullptr;
  void*         _room_post_ctx = nullptr;
  uint8_t        _to_secret[PUB_KEY_SIZE];
  uint8_t        _to_path[MAX_PATH_SIZE];
  uint8_t        _to_path_len;

  /* Het volledige antwoord, en de stukken als index erin. */
  char     _text[DM_TEXT_BUF_LEN];
  char     _tail[DM_TAIL_MAX];      // slotregel; leeg als alles paste
  uint16_t _chunk_off[DM_MAX_CHUNKS];
  uint8_t  _chunk_len[DM_MAX_CHUNKS];
  uint8_t  _num_chunks;
  uint8_t  _next_chunk;
  uint8_t  _attempt;
  int      _gen_omitted;            // regels die al bij het opbouwen niet pasten

  uint32_t      _msg_timestamp;     // vast per stuk, zodat een herhaling er een is
  uint32_t      _expected_ack;      // 0 = niet op een ACK aan het wachten
  unsigned long _next_action;       // millis(); wanneer loop() weer iets mag

  DmSensorInfo  _si;                // hergebruikt; houdt de stapel klein

  /* Wachttijd voor de weigering aan een afzender zonder rechten. Vier byte van
   * de publieke sleutel is genoeg om "dezelfde als vorige keer" te herkennen;
   * dit is een rem tegen zendtijdverspilling, geen beveiliging. */
  uint8_t       _deny_key[4];
  unsigned long _deny_until;

  /* Rem op ad-hoc ping vanuit een room (renderReply): niet vaker dan ~elke 20 s,
   * want een ping is zendtijd op een gedeelde band. */
  unsigned long _room_ping_until = 0;

  void reset();
  bool isAllowed(const ClientInfo& from) const;
  void startReply(const ClientInfo& to);
  /* Zoals startReply maar vanuit los bewaarde velden -- voor het uitgestelde
   * ping-antwoord, waar geen ClientInfo meer voorhanden is. */
  void startReplyStored(const mesh::Identity& id, const uint8_t* secret,
                        const uint8_t* path, uint8_t path_len);

  bool appendText(const char* s);
  void buildList();
  void buildGet(const char* name);
  void buildStatus();
  void buildHelp();

  void splitReply();
  void sendChunk();
};
