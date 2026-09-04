#pragma once

#include <Arduino.h>

#include "MonitorSensors.h"   /* MonitorEvent, MonitorEventSink, de instellingen */
#include "RepeaterCli.h"      /* RepeaterStatus -- de status-momentopname (v2.7.0) */

class WifiTask;

/* PushTask -- gebeurtenissen en heartbeats naar de statsserver.
 *
 * WAAROM DIT BESTAAT
 *
 * Zonder push bereikt een storing de server pas bij de volgende IP-poll (tot
 * 60 s), en betekent stilte van de node alleen "poll mislukt". Met push draagt
 * de node de gebeurtenis op het moment zelf (~1 s na het armen van de melding),
 * en maakt het beloofde heartbeat-interval van elke stilte een betekenisvol
 * signaal: wie langer dan hb_s niets hoort, weet dat er iets is -- niet
 * misschien iets. Het antwoord van de server lost bovendien de
 * ack-synchronisatie op: wie op de site bevestigt, stopt daarmee ook het
 * DM-herhalen op de node (confirmAlertChannel), en wie op de node "ok" stuurt,
 * gaat in "acked" van de volgende push mee.
 *
 * HET CONTRACT (vastgespijkerd met de serverkant; hier niet van afwijken)
 *
 *   POST {push.url}/api/sensorpush
 *   Authorization: Bearer {push.token} ; Content-Type: application/json
 *   {"node":"<pubkey-prefix, 12 hex kleine letters>",
 *    "seq":<uint32, telt per verzending op>,
 *    "boot":<uint32, willekeurig per opstart>,
 *    "hb_s":<uint16, beloofd heartbeat-interval>,
 *    "events":[{"ch":..,"kind":"neer"|"op","text":"..","sev":"hoog"|"laag","sim":0|1},..],
 *    "acked":[<kanaal>,..]}
 *   Antwoord 200: {"ok":1,"ack":[<kanaal>,..]}
 *
 * NIET-BLOKKEREND, EN DAT IS DE HELE ONTWERPEIS. De LoRa-radio wordt uit
 * dezelfde loop() bediend en gaat voor. Daarom een toestandsmachine over
 * loop()-rondes op een non-blocking lwIP-socket -- hetzelfde recept als de
 * ping-machine in MonitorSensors: elke ronde wordt er iets GESTART of
 * NAGEKEKEN, nooit gewacht. De naamsopzoeking gaat langs dezelfde weg als daar
 * (dns_gethostbyname via tcpip_try_callback, antwoord per callback), want
 * getaddrinfo() en WiFi.hostByName() wachten tot seconden op een DNS-server
 * die niet antwoordt. Het duurste dat één ronde kan kosten is de overdracht
 * van een gevulde verzendbuffer aan de lwIP-taak: enkele milliseconden als die
 * taak druk is, microseconden anders. Er is geen pad dat op het netwerk wacht.
 *
 * WAT ER GEBEURT ALS HET MISLUKT -- de kern van dit onderdeel, want stil
 * verlies is precies de fout die dit project bestrijdt:
 *
 *  - de gebeurtenissen staan in een statische ring van PUSH_RING_SIZE (8) en
 *    blijven daar tot een POST met antwoord 200 ze afgeleverd heeft;
 *  - loopt de ring over, dan valt de OUDSTE eruit en telt lostCount() dat --
 *    zichtbaar, niet stil;
 *  - een verbindingsfout probeert het na PUSH_RETRY_MS opnieuw (de heartbeat-
 *    klok loopt gewoon door);
 *  - een HTTP-antwoord dat geen 200 is (401 token fout, 404 node onbekend)
 *    wordt gelogd en dan wordt er tot de volgende heartbeat gewacht --
 *    opnieuw proberen tegen een server die "nee" zegt is spam, geen ijver;
 *  - de op de node bevestigde kanalen (het acked-masker) blijven hier staan
 *    tot ze in een gelukte POST meegingen, dus ook die overleven een storing.
 *
 * Passen niet alle gebeurtenissen in de bodybuffer (~1 kB), dan gaan er zoveel
 * mee als er passen en volgt voor de rest DIRECT een nieuwe POST -- de ring
 * leegt zich dus vanzelf, in volgorde.
 */
class PushTask : public MonitorEventSink {
public:
  /* De ring. 8 is genoeg: gebeurtenissen ontstaan hoogstens twee per leesronde
   * (MAX_MONITOR_ALERTS) en de ring leegt zich zodra de server bereikbaar is;
   * acht plaatsen overbruggen dus ruim een half uur serverstoring aan
   * overgangen. Wat er toch uit valt, telt de verliesteller. */
  static const uint8_t PUSH_RING_SIZE = 8;

  /* Tijdsbudgetten van de toestandsmachine, per fase (millis). Ruim boven wat
   * een gezond LAN nodig heeft, ruim onder wat een gebruiker "hangt" noemt. */
  static const uint32_t PUSH_DNS_MS     = 5000;
  static const uint32_t PUSH_CONNECT_MS = 4000;
  static const uint32_t PUSH_SEND_MS    = 3000;
  static const uint32_t PUSH_RECV_MS    = 5000;
  /* Zelfde gedachte als DNS_TTL_MS bij de monitors: de server verhuist niet
   * elk uur, en elke opzoeking is verkeer en wachttijd. */
  static const uint32_t PUSH_DNS_TTL_MS = 10UL * 60 * 1000;
  /* Na een verbindingsfout: niet hameren, wel snel genoeg terugkomen dat een
   * gebeurtenis geen minuten blijft liggen. */
  static const uint32_t PUSH_RETRY_MS   = 15000;

  /* pub_key: de publieke sleutel van de node (the_mesh.self_id.pub_key); de
   * eerste 6 bytes worden het "node"-veld (12 hex kleine letters). Wordt
   * gekopieerd, dus de aanroeper hoeft niets vast te houden. */
  void begin(WifiTask* wifi, MonitorSensors* sensors, const uint8_t* pub_key);
  void loop();

  /* De haak van MonitorSensors (zie setEventSink). Loopt in de hoofdtaak. */
  void onMonitorEvent(const MonitorEvent& ev) override;

  /* v2.5.1 -- INSTANT-PUSH van een companion-locatie/val naar MeshManager.
   * RoomMesh roept dit aan zodra een companion-#LOC/val ontvangen ÉN bewaard is
   * (companionUpdateLoc / companionRecordFall): de companion-stand gaat DAN
   * METEEN de deur uit i.p.v. te wachten tot MeshManager /companions.json polt
   * (tot ~1 min oud). Hergebruikt dezelfde host/token/DNS-cache/socket-machine
   * als de sensorpush, maar met een NIEUW pad (POST {push.url}/api/companion) en
   * een eigen kleine ring, zodat een val een transiënte netwerk-/serverfout
   * overleeft (retry/queue, net als de sensorpush). Body:
   *   {"companions":[{"pubkey":"<64hex>","lat":<f>,"lon":<f>,"batt":<pct>,"seen":<u>,
   *                   "fall_ts":<u>,"fall_kind":"val|nomotion|sos|"}]}
   * has_loc=false -> lat/lon worden WEGGELATEN; batt<0 (onbekend) -> batt WEGGELATEN;
   * fall_ts=0/fall_kind=0 -> geen val (fall_ts:0, fall_kind:""). Push uit (geen url)
   * -> stil laten vallen. */
  void queueCompanion(const uint8_t* pub_key, bool has_loc, float lat, float lon,
                      uint32_t seen, uint32_t fall_ts, uint8_t fall_kind, int16_t batt);

  /* HET ANTWOORD VAN EEN REMOTE ADMIN-CLI-OPDRACHT naar MeshManager.
   *
   * Zelfde host/token/DNS-cache/socketmachine als de andere twee soorten -- alleen
   * het pad en de body verschillen, precies zoals /api/companion er in v2.5.1
   * naast kwam. Er is bewust GEEN tweede HTTP-client bijgekomen: die stack past
   * niet naast mesh, wifi en de webserver (zie de uitleg bovenaan StatsPublisher
   * in MeshCore over waarom een tweede TLS-sessie deze klasse node liet crashen).
   *
   *   POST {push.url}/api/v1/repeater_settings
   *   Authorization: Bearer {push.token} ; Content-Type: application/json
   *   {"repeater":{"pubkey_prefix":"<12 hex>"},"settings":{"<param>":"<waarde>"}}
   *
   * pubkey_prefix is de eerste 6 byte van de DOELrepeater (niet van deze node!) --
   * dezelfde prefix waarmee MeshManager die repeater al kent.
   *
   * param is een CONTRACT met de serverkant: "cmd:filter count" is de vorm die
   * pfstock.apply_cli_filter() herkent en in tegels en grafieken omzet. Wie hem
   * anders schrijft, krijgt een instelling die de server bewaart en niemand
   * gebruikt. RepeaterCli stelt hem samen als "cmd:" + de verstuurde opdracht.
   *
   * value == nullptr  -> JSON `null`: de server (routes_api.repeater_settings)
   * telt zo'n parameter als "gevraagd, geen antwoord" i.p.v. beantwoord. DAT IS
   * BELANGRIJK: een sweep die de helft niet uit de lucht kreeg hoort de
   * beheerpagina "gevraagd, geen antwoord" te tonen, niet stilte -- precies de
   * fout die dit project bestrijdt. RepeaterCli levert een verlopen commando met
   * nullptr af.
   *
   * Push uit (geen url) -> stil laten vallen: geen server om iets aan kwijt te
   * raken is een stand en geen verlies. */
  void queueRepeaterSetting(const char* pubkey_hex12, const char* param, const char* value);

  /* ------------------------------------------------------------------------
   * DE MESHMANAGER-OPDRACHTWACHTRIJ POLLEN (v2.6.0) -- de kern waarmee Home
   * Assistant uit de keten valt.
   *
   *   GET {push.url}/api/v1/commands
   *   Authorization: Bearer {push.token}
   *
   * De Poller (Poller.*) beslist WANNEER er gepold wordt (elke poll_secs) en WAT
   * er met het antwoord gebeurt; PushTask levert alleen de niet-blokkerende GET,
   * langs exact dezelfde DNS-cache/connect/send/recv-machine als de pushes. Zo is
   * er EEN gehard socketpad en geen tweede HTTP-client naast mesh/wifi/webserver.
   *
   * requestPoll() zet een vlag; de eigenlijke GET draait zodra loop() aan de beurt
   * is en er geen dringender push (companion, cli-antwoord) voorligt. Het rauwe
   * antwoord-BODY gaat via de callback terug naar de Poller, die het ontleedt --
   * PushTask kent de vorm van de wachtrij niet en hoeft dat niet.
   *
   * Er is er hoogstens EEN tegelijk: een tweede requestPoll() terwijl er een loopt
   * of klaarstaat doet niets (false terug). De heartbeat-klok wordt door een poll
   * NIET verzet -- pollen is geen meten. */
  /* ------------------------------------------------------------------------
   * EEN STATUS-MOMENTOPNAME VAN EEN ANDERE REPEATER (v2.7.0).
   *
   *   POST {push.url}/api/v1/ingest
   *   Authorization: Bearer {push.token} ; Content-Type: application/json
   *   {"repeater":{"pubkey_prefix":"<12 hex>"},"metrics":{...}}
   *
   * Een DERDE endpoint naast /api/sensorpush en /api/v1/repeater_settings, in
   * dezelfde stijl: eigen KIND_, eigen kleine ring, eigen body-bouwer, maar
   * dezelfde host/token/DNS-cache/socketmachine. Geen `ts` meegeven -- dan neemt de
   * server zijn eigen tijd, en dat is hier de betrouwbaardere klok (de doelrepeater
   * z'n klok kan ver weglopen; dat is juist een van de dingen die we willen kunnen
   * repareren).
   *
   * DE METRIEKNAMEN ZIJN EEN CONTRACT met de serverkant (server/app/metrics.py);
   * wat daar niet in staat wordt genegeerd. De omrekening naar de eenheden die daar
   * gedocumenteerd staan (V, dagen, minuten, dB) gebeurt in buildIngestBody(), zodat
   * ze op EEN plek staat.
   *
   * WAT DE FIRMWARE NIET MELDT, LATEN WE WEG. Geen nul verzinnen: op de server IS
   * een nul een meting, en een verzonnen nul wordt een punt in een grafiek. */
  void queueRepeaterStats(const char* pubkey_hex12, const RepeaterStatus& st);

  typedef void (*PollFn)(void* ctx, const char* body);
  bool requestPoll(PollFn cb, void* ctx);
  bool pollInFlight() const { return _poll_requested || _kind == KIND_POLL; }

  /* Voor de statuspagina en het rapport. lostCount() is de belangrijkste:
   * gebeurtenissen die uit de ring gevallen zijn zonder afgeleverd te worden. */
  bool     enabled() const;              /* url gezet? */
  uint32_t sentCount() const     { return _sent_ok; }
  uint32_t failNetCount() const  { return _fail_net; }
  uint32_t failHttpCount() const { return _fail_http; }
  uint32_t lostCount() const     { return _lost; }
  int      lastHttpStatus() const { return _last_status; }
  uint8_t  queuedCount() const   { return _ring_count; }
  uint32_t lastOkAgeSecs() const;        /* sinds de laatste gelukte push; 0 = nooit */

private:
  enum State : uint8_t {
    PUSH_IDLE = 0,
    PUSH_RESOLVING,    /* wacht op de DNS-callback */
    PUSH_CONNECTING,   /* non-blocking connect loopt */
    PUSH_SENDING,      /* verzoek gaat de socket in, mogelijk in stukken */
    PUSH_RECEIVING     /* antwoord komt binnen tot de server sluit */
  };

  /* Welk SOORT de LOPENDE poging is. De socket-machine (DNS/connect/send/recv)
   * is voor beide identiek; alleen het pad en de body-opbouw verschillen, en
   * finishOk()/failXxx() ruimen de bijhorende ring op. Companion-pushes gaan
   * VOOR (een val mag niet achter een heartbeat aansluiten). */
  enum PushKind : uint8_t { KIND_SENSOR = 0, KIND_COMPANION, KIND_REPCLI, KIND_POLL,
                           KIND_INGEST };
  PushKind _kind = KIND_SENSOR;

  /* De poll (GET /api/v1/commands). _poll_requested = de Poller wil pollen zodra
   * het uitkomt; het antwoord-BODY gaat via _poll_cb terug. Geen ring: er is er
   * altijd hoogstens een, en het antwoord is vluchtig (de Poller kopieert wat hij
   * nodig heeft). */
  bool    _poll_requested = false;
  PollFn  _poll_cb        = nullptr;
  void*   _poll_ctx       = nullptr;

  /* De ring voor CLI-antwoorden van andere repeaters. TWEE plaatsen, en dat is
   * geen zuinigheid maar een afspiegeling van de bron: RepeaterCli doet EEN
   * opdracht tegelijk, dus er kan er hoogstens een tegelijk klaar zijn. De tweede
   * plaats is er alleen zodat een antwoord dat binnenkomt terwijl de vorige nog
   * de socket in gaat niet stil verdwijnt. Elke plaats kost ~250 byte. */
  static const uint8_t RCLI_RING_SIZE = 2;
  struct RepCliPush {
    char node[13];    /* pubkey-prefix van de DOELrepeater, 12 hex + afsluiter */
    char param[168];  /* "cmd:" + de opdracht (RCLI_CMD_MAX = 160) + afsluiter   */
    char value[400];  /* de antwoordregel (= RCLI_ANSWER_MAX: meerdere pakketten aaneengeregen) */
    bool has_value;   /* false -> JSON null ("gevraagd, geen antwoord")          */
  };
  RepCliPush _rring[RCLI_RING_SIZE];
  uint8_t _rring_tail    = 0;
  uint8_t _rring_count   = 0;
  uint8_t _rcli_inflight = 0;

  /* De ring voor status-momentopnames van andere repeaters. TWEE plaatsen, om
   * dezelfde reden als de cli-ring: RepeaterCli doet een sessie tegelijk, dus er
   * kan er hoogstens een klaar zijn; de tweede vangt op dat er een binnenkomt
   * terwijl de vorige de socket in gaat. Elke plaats kost ~72 byte. */
  static const uint8_t ING_RING_SIZE = 2;
  struct IngestPush {
    char           node[13];   /* pubkey-prefix van de DOELrepeater, 12 hex */
    RepeaterStatus st;
  };
  IngestPush _iring[ING_RING_SIZE];
  uint8_t _iring_tail   = 0;
  uint8_t _iring_count  = 0;
  uint8_t _ing_inflight = 0;

  /* De companion-ring. Klein: val/loc-events zijn zeldzaam en de ring leegt zich
   * zodra de server bereikbaar is. Loopt hij toch over, dan valt de OUDSTE eruit
   * (geteld in _lost, net als de sensorring). */
  static const uint8_t COMP_RING_SIZE = 4;
  struct CompanionPush {
    uint8_t  pub_key[PUB_KEY_SIZE];
    float    lat, lon;
    bool     has_loc;
    uint32_t seen;
    uint32_t fall_ts;
    uint8_t  fall_kind;
    int16_t  batt;          /* accu-% (0..100); -1 = onbekend -> weggelaten */
  };
  CompanionPush _cring[COMP_RING_SIZE];
  uint8_t _cring_tail    = 0;
  uint8_t _cring_count   = 0;
  uint8_t _comp_inflight = 0;   /* hoeveel companion-plaatsen in de lopende POST */

  WifiTask*       _wifi    = NULL;
  MonitorSensors* _sensors = NULL;
  char            _node_id[13] = {0};   /* 12 hex + afsluiter */

  State         _state = PUSH_IDLE;
  int           _sock = -1;
  unsigned long _deadline = 0;
  unsigned long _next_hb = 0;          /* 0 = meteen (eerste push meldt de boot) */
  unsigned long _retry_at = 0;         /* niet vóór dit moment opnieuw proberen */
  unsigned long _last_ok = 0;          /* millis van de laatste 200 */

  uint32_t _seq  = 0;
  uint32_t _boot = 0;                  /* willekeurig per opstart (esp_random) */

  /* De ring. tail = oudste; de POST neemt altijd vanaf de oudste, zodat de
   * volgorde op de server klopt met de volgorde hier. */
  MonitorEvent _ring[PUSH_RING_SIZE];
  uint8_t  _ring_tail  = 0;
  uint8_t  _ring_count = 0;
  /* Hoeveel ringplaatsen (vanaf tail) in de LOPENDE POST zitten. Valt tijdens
   * die POST de oudste uit de ring (overloop), dan telt die niet meer als
   * onderweg -- anders zou het opruimen na de 200 een niet-verzonden
   * gebeurtenis weggooien. */
  uint8_t  _inflight = 0;

  /* Op de node bevestigde kanalen (bitmasker, bit n = kanaal n), overgenomen
   * uit MonitorSensors en hier vastgehouden tot een POST met 200 ze droeg.
   * _acked_sent is de momentopname die in de lopende POST zit. */
  uint64_t _acked_pending = 0;
  uint64_t _acked_sent    = 0;

  /* Opgeloste server (IPv4, netwerk-volgorde) met vervaltijd, plus de host
   * waar dat adres bij hoort -- verandert de url, dan telt de cache niet. */
  uint32_t      _addr_v4 = 0;
  unsigned long _addr_expiry = 0;
  char          _addr_host[MON_PUSH_URL_LEN] = {0};

  /* Tellers voor status en rapport. */
  uint32_t _sent_ok = 0, _fail_net = 0, _fail_http = 0, _lost = 0;
  int      _last_status = 0;
  int      _last_logged_status = -1;   /* tegen dezelfde regel elke heartbeat */

  bool dueNow(unsigned long now) const;
  void startAttempt();                 /* url ontleden, DNS of connect starten */
  void startConnect(uint32_t addr_v4, uint16_t port);
  bool buildRequest(const char* host, const char* path);
  bool buildSensorBody(char* body, size_t cap, size_t& blen);
  bool buildCompanionBody(char* body, size_t cap, size_t& blen);
  bool buildRepCliBody(char* body, size_t cap, size_t& blen);
  bool buildIngestBody(char* body, size_t cap, size_t& blen);
  bool buildPollRequest();   /* KIND_POLL: een GET zonder body */
  void stepConnect();
  void stepSend();
  void stepRecv();
  void finishOk();
  void failNet(const char* why);
  void failHttp(int status);
  void closeSock();
};
