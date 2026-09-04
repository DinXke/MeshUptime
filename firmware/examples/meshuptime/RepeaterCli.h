#pragma once

#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include <helpers/TxtDataHelpers.h>

/* "geen pad bekend, stuur dit als flood". Upstream definieert deze waarde twee
 * keer met dezelfde inhoud (helpers/ClientACL.h en helpers/ContactInfo.h) --
 * allebei headers die deze module verder NIET nodig heeft: de een sleept een
 * toegangslijst mee, de ander de contactentabel van de chat-client. Eén byte
 * overnemen achter een guard is goedkoper dan een van die twee binnenhalen, en
 * de guard zorgt dat wie ze wél al includeerde (RoomMesh.h doet dat) hier geen
 * dubbele definitie krijgt. */
#ifndef OUT_PATH_UNKNOWN
  #define OUT_PATH_UNKNOWN  0xFF
#endif

/* Het statusverzoek (v2.7.0). Zelfde waarde als upstream:
 * simple_repeater/MyMesh.cpp:46 `#define REQ_TYPE_GET_STATUS 0x01 // same as
 * _GET_STATS`, en helpers/BaseChatMesh.h:18 draagt hem onder dezelfde naam.
 * Lokaal herhaald achter een guard, om dezelfde reden als RESP_SERVER_LOGIN_OK:
 * de headers die hem definieren slepen elk een basisklasse mee die deze module
 * niet nodig heeft. */
#ifndef REQ_TYPE_GET_STATUS
  #define REQ_TYPE_GET_STATUS  0x01
#endif

/* ============================================================================
 * DE DRAADVORM VAN HET STATUSANTWOORD -- OVERGENOMEN, NIET ONTHOUDEN
 *
 * Bron: `struct RepeaterStats` in vendor/MeshCore/examples/simple_repeater/
 * MyMesh.h:43, gevuld in MyMesh::handleRequest() (~regel 216) en verstuurd als
 * `return 4 + sizeof(stats)` -- dus: [0..3] de GEREFLECTEERDE sender_timestamp
 * (onze tag) en daarna de struct.
 *
 * De struct draagt GEEN packed-attribuut, maar elk veld staat op zijn natuurlijke
 * uitlijning, dus er zit geen padding in en sizeof == 56. Nagerekend veld voor
 * veld (de offsets hieronder zijn de compileruitkomst, niet een schatting):
 *
 *    0  uint16 batt_milli_volts        24  uint32 n_sent_flood
 *    2  uint16 curr_tx_queue_len       28  uint32 n_sent_direct
 *    4  int16  noise_floor             32  uint32 n_recv_flood
 *    6  int16  last_rssi               36  uint32 n_recv_direct
 *    8  uint32 n_packets_recv          40  uint16 err_events  (was n_full_events)
 *   12  uint32 n_packets_sent          42  int16  last_snr    (x 4)
 *   16  uint32 total_air_time_secs     44  uint16 n_direct_dups
 *   20  uint32 total_up_time_secs      46  uint16 n_flood_dups
 *                                      48  uint32 total_rx_air_time_secs
 *                                      52  uint32 n_recv_errors
 *
 * WIJ LEZEN MET memcpy OP EXPLICIETE OFFSETS en niet met een struct-cast. Twee
 * redenen: de ontvangen buffer is niet gegarandeerd 4-byte uitgelijnd (een
 * unaligned struct-load is op ESP32 een uitzondering), en een cast zou stil
 * meeveranderen als onze compiler ooit anders padt dan de tegenkant. De offsets
 * hierboven zijn het contract; als upstream de struct wijzigt, wijzigt dit mee.
 *
 * EN DAAROM DE PLAUSIBILITEITSTOETS. De doelrepeater hoeft niet dezelfde build te
 * draaien (JessaZH draait dutchmeshcore v1.17.1-PS+filter+rollback). Een fork die
 * een veld TOEVOEGT aan het eind is onschadelijk -- wij lezen de eerste 56 byte en
 * negeren de rest. Een fork die een veld INVOEGT of HERORDENT is dat niet: dan
 * lezen we getallen op de verkeerde plaats. Dat is niet aan de bytes te zien, dus
 * toetst parseStatus() de velden waarvan we het BEREIK kennen (accuspanning,
 * uptime, airtime <= uptime, RSSI/SNR/ruisvloer) en verwerpt het HELE antwoord als
 * er iets niet plausibel is. Geen half gevulde meting, geen verzonnen nul. */
#define RCLI_STATS_WIRE_LEN   56
#define RCLI_STATUS_RESP_MIN  (4 + RCLI_STATS_WIRE_LEN)   /* 60 */

/* Het ontlede statusantwoord. Alleen ruwe velden -- het omrekenen naar de
 * MeshManager-eenheden (volt, dagen, minuten, dB) gebeurt bij het opbouwen van de
 * ingest-body, zodat de omrekening op EEN plek staat. */
struct RepeaterStatus {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood;
  uint32_t n_sent_direct;
  uint32_t n_recv_flood;
  uint32_t n_recv_direct;
  uint16_t err_events;
  int16_t  last_snr_x4;
  uint16_t n_direct_dups;
  uint16_t n_flood_dups;
  uint32_t total_rx_air_time_secs;
  uint32_t n_recv_errors;
};

/* ============================================================================
 * RepeaterCli -- admin-CLI-opdrachten naar een ANDERE repeater over LoRa, en de
 * antwoorden terug. Sinds v2.6.0 een JOB van N commando's in EEN sessie.
 *
 * WAAROM DIT BESTAAT
 *
 * BE-HSS-JessaZH.VIR (e3d3f4d7edd0) hangt op een dak en is alleen over LoRa te
 * bereiken. Tot v2.5.1 liep het uitlezen van zijn pakketfilter-tellers en het
 * rechtzetten van zijn klok via Home Assistant (meshcore.execute_command), dus
 * via een companion-node, een integratie en een automatisering -- drie schakels
 * die alle drie stuk kunnen terwijl deze node er zelf naast staat en het ook
 * kan. v2.6.0 haalt HA volledig uit de keten: de MeshManager-poller (Poller.*)
 * haalt de opdrachtwachtrij zelf op en voert ze uit langs deze module.
 *
 * ------------------------------------------------------------------------
 * WAT ER IN v2.6.0 VERANDERDE: EEN JOB, NIET EEN COMMANDO
 *
 * v2.5.1 deed EEN commando per sessie (login -> commando -> antwoord). Een
 * MeshManager-instellingenopvraging vraagt echter tot 40 parameters van dezelfde
 * repeater in EEN keer, en die veertig keer opnieuw laten inloggen zou veertig
 * ANON_REQ's extra kosten -- zendtijd op een gedeelde band, voor niets. Daarom is
 * de sessie nu een JOB: EENMAAL inloggen, dan de N commando's ACHTER ELKAAR, elk
 * met dezelfde tussenpauze en dezelfde herhaalregels als voorheen. Elk antwoord
 * (of het uitblijven ervan) wordt PER COMMANDO afgeleverd zodra het binnen is.
 *
 * Een handmatige `/cli/remote` is gewoon een job van EEN commando; het pad is
 * identiek, alleen de lengte van de lijst verschilt.
 *
 * ------------------------------------------------------------------------
 * DE ROLVRAAG: WAAROM GEEN BaseChatMesh  (ongewijzigd sinds v2.5.1)
 *
 * BaseChatMesh IS een mesh::Mesh. RoomMesh IS OOK een mesh::Mesh. Een node heeft
 * er precies EEN. RoomMesh onder BaseChatMesh hangen zou betekenen dat elke
 * callback die RoomMesh nu invult (onRecvPacket met de multiroom-dispatch,
 * searchPeersByHash over vier ACL's, onAnonDataRecv met de room-login,
 * onPeerDataRecv met posts/telemetrie/bot) tegen de pure-virtuals van
 * BaseChatMesh gepast moet worden -- een verbouwing van de dragende muur van een
 * node die iemands bewaking draait. In de plaats komt hier de login-/CLI-
 * sessielogica zelf, bovenop wat er al ligt. De prijs, eerlijk: dit is een tweede
 * implementatie van hetzelfde protocol naast BaseChatMesh. Verandert MeshCore de
 * vorm van login of CLI_DATA, dan moet dit mee -- daarom staat bij elk veld waar
 * de tegenkant het schrijft.
 *
 * ------------------------------------------------------------------------
 * HET PROTOCOL, ZOALS DE TEGENKANT HET ECHT DOET
 * (Geverifieerd tegen vendor/MeshCore/examples/simple_repeater/MyMesh.cpp.)
 *
 * 1. INLOGGEN = een ANON_REQ, geen REQ.
 *      payload = [tijdstempel uint32][wachtwoord, HOOGSTENS 15 tekens]
 *    Geen request-type-byte: MyMesh::onAnonDataRecv herkent een login aan
 *    `data[4] == 0 || data[4] >= ' '` -- een drukbaar eerste teken BETEKENT login.
 *    De 15-tekengrens komt van BaseChatMesh::sendLogin().
 *
 * 2. HET ANTWOORD is een PAYLOAD_TYPE_RESPONSE van 13 byte:
 *      [4] RESP_SERVER_LOGIN_OK (0), [6] is_admin (1=beheerder), [7] ACL-rechten.
 *    Kwam onze login als FLOOD binnen, dan komt dit antwoord NIET als los RESPONSE
 *    maar VERPAKT IN een PAYLOAD_TYPE_PATH (createPathReturn, extra=RESPONSE).
 *    Beide wegen worden afgehandeld (onPeerData en onPath).
 *
 * 3. HET COMMANDO is een PAYLOAD_TYPE_TXT_MSG met TXT_TYPE_CLI_DATA (=1). De
 *    repeater voert het alleen uit voor `client->isAdmin()` -- vandaar dat we de
 *    is_admin-byte echt toetsen: zonder recht wordt het commando STIL weggegooid.
 *
 * 4. OP EEN CLI_DATA-ANTWOORD KOMT GEEN ACK. Het ANTWOORD zelf is de bevestiging;
 *    er is niets anders om op te wachten. Dat maakt de toestandsmachine klein.
 *
 * ------------------------------------------------------------------------
 * DE VALKUIL DIE JE MAAR EEN KEER HOEFT TE MISSEN: HERHALEN
 *
 * De repeater beschermt tegen replay met twee ONGELIJKE regels:
 *   login (ANON_REQ):  sender_timestamp <= last_timestamp  -> WEIGEREN
 *   CLI   (TXT_MSG):   sender_timestamp <  last_timestamp  -> WEIGEREN
 *                      sender_timestamp == last_timestamp  -> "is_retry",
 *                                          commando NIET uitvoeren, LEEG antwoord
 * Elke poging krijgt daarom een NIEUWE tijdstempel (getCurrentTimeUnique()).
 *
 * GEVOLG: een herhaling VOERT HET COMMANDO OPNIEUW UIT op de tegenkant. Voor een
 * `get`/`filter count`/`clock` is dat onschadelijk. Voor een commando dat iets
 * VERANDERT niet -- daarom herhaalt deze module zo'n commando NIET (isMutating(),
 * de bovengrens wordt dan 1). Die rem zit HIER, zodat hij ook geldt voor de
 * poller die uit de MeshManager-wachtrij put.
 *
 * ------------------------------------------------------------------------
 * ZENDTIJD IS DE SCHAARSTE
 *
 *  - EEN sessie tegelijk. Geen parallelle jobs. Wie een tweede aanbiedt terwijl
 *    er een loopt, krijgt "bezig" (de poller wacht dan gewoon).
 *  - Per commando hoogstens RCLI_MAX_ATTEMPTS pogingen (1 als het muteert).
 *  - RCLI_MIN_GAP_MS tussen twee zendingen van ons, ALTIJD.
 *  - RCLI_JOB_MAX_MS als harde bovengrens voor de HELE job: schaalt met het
 *    aantal commando's, zodat een sweep van veertig parameters niet halverwege
 *    wordt afgekapt maar een vastgelopen job ook nooit de enige sessie eeuwig
 *    bezet houdt.
 * ==========================================================================*/

/* De opdrachttekst die de tegenkant krijgt. Zelfde grens als de mesh-tekstlaag
 * (BaseChatMesh MAX_TEXT_LEN, 10 cipherblokken van 16). */
#define RCLI_CMD_MAX      160

/* Het ANTWOORD. De stock-repeater schrijft het in `uint8_t temp[166]` met de
 * tekst op offset 5 (simple_repeater/MyMesh.cpp): EEN pakket, hoogstens 160
 * tekens. De dutchmeshcore/EasySkyMesh-fork op JessaZH doet dat NIET: die knipt
 * een lang antwoord (`filter count` = kopregel + limiettabel, ~210 tekens) in
 * meerdere TXT_MSG-pakketten, die over een flood niet per se in volgorde
 * aankomen. Daarom RCLI_CHUNK_MAX stukken verzamelen, op hun tijdstempel
 * sorteren en pas na RCLI_CHUNK_GAP_MS stilte afleveren. 400 is de som van vier
 * volle stukken min de marge die nooit gehaald wordt; PushTask::value volgt. */
#define RCLI_ANSWER_MAX   400
#define RCLI_CHUNK_MAX    4
#define RCLI_CHUNK_GAP_MS 3000UL

/* Het wachtwoord. 16: sendLogin() kapt af op 15, dus meer komt nooit aan. */
#define RCLI_PASS_MAX     16

/* EEN PARAMETER (de MeshManager-sleutel, bv. "name", "cmd:filter count"). De
 * server splitst de parameterlijst op komma's, dus een parameternaam bevat NOOIT
 * een komma -- daarop leunt de CSV-opslag hieronder. 56 dekt de langste die er in
 * de praktijk voorkomt met marge. */
#define RCLI_PARAM_MAX    56

/* HOEVEEL COMMANDO'S EEN JOB DRAAGT. Veertig, want dat is wat MeshManager als
 * parameterlijst uitdeelt (routes_admin.py kapt de lijst op 40). */
#define RCLI_MAX_JOB      40

/* De JOB-BUFFER: de parameters als komma-lijst (poller) OF een enkele opdracht
 * (handmatig, met "cmd:" ervoor). Bij de poller: tot 40 parameters van gemiddeld
 * ~14 tekens = ~560; bij handmatig: "cmd:" + tot 160 = 164. 640 dekt beide.
 *
 * WAAROM CSV EN GEEN ARRAY VAN 40 x RCLI_PARAM_MAX: dat zou 2240 byte zijn tegen
 * 640 nu, en op een node zonder PSRAM telt elke kB. Een parameternaam bevat geen
 * komma (de server splitst er zelf op), dus splitsen op komma is verliesvrij --
 * BEHALVE voor een handmatige opdracht die WEL een komma kan hebben ("set radio
 * 869,250,11,5"). Die staat daarom altijd als een job van EEN, en dan wordt de
 * buffer nooit gesplitst (zie nthParam). */
#define RCLI_JOB_BUF      640

/* POGINGEN per commando. Drie (1 als het muteert), zoals DM_MAX_ATTEMPTS. */
#define RCLI_MAX_ATTEMPTS 3

/* WACHTEN OP EEN ANTWOORD, per poging. Met de companion-formules (MyMesh.cpp:851):
 * flood 500 + 16*airtime; op SF11 (~700 ms in de lucht) is dat ~11,7 s heen, en
 * het antwoord komt langs dezelfde weg terug plus de vertragingen die de repeater
 * er zelf voorzet. 25 s dekt heen en terug met marge. */
#define RCLI_STEP_TIMEOUT_MS  25000UL

/* MINIMUMAFSTAND tussen twee zendingen van ons -- ook tussen twee commando's van
 * dezelfde job. Zes seconden is ruim boven de doorgeeftijd van een enkel pakket
 * door het mesh en ruim onder wat "hangt" voelt. */
#define RCLI_MIN_GAP_MS       6000UL

/* HARDE BOVENGRENS voor de HELE job. Per commando kost het ergste geval 3 x 25 s
 * plus de tussenpauze; de login komt daar eenmalig bij. We schalen mee met het
 * aantal commando's: base voor de login + per commando een royaal budget. Dit is
 * een VANGNET tegen een vastgelopen job, niet de normale werking -- de per-
 * commando-deadlines regelen het echte tempo. */
#define RCLI_JOB_BASE_MS      40000UL
#define RCLI_JOB_PERCMD_MS    90000UL
#define RCLI_JOB_MAX_MS(n)    (RCLI_JOB_BASE_MS + (uint32_t)(n) * RCLI_JOB_PERCMD_MS)

class RepeaterCli;

/* ------------------------------------------------------------------------
 * DE HAAK NAAR DE MESHKLASSE (ongewijzigd sinds v2.5.1)
 *
 * Drie methoden. Twee van de drie dingen die deze module nodig heeft zijn niet
 * publiek op mesh::Mesh: een flood MET de regio-transportcode (sendFloodScoped is
 * protected) en welke identiteit als CLIENT optreedt (dat weet alleen de
 * meshklasse). De scope is geen luxe: de packetfilter van een repeater in een
 * regio gooit ONGESCOPEDE floods weg (drop.hash), en dan verdwijnt de login zonder
 * spoor. Zie de uitleg bij floodScoped in SensorMesh.h.
 * ------------------------------------------------------------------------ */
class RepeaterCliHost {
public:
  /* Kies de client-identiteit, ZET HAAR ACTIEF (self_id) en geef haar terug. Op
   * deze node wisselt self_id per ontvangen pakket (onRecvPacket); wie hier niet
   * eerst kiest, stuurt vanaf een willekeurige identiteit. Dezelfde regel die
   * RoomMesh::sendAlertDM al zet. */
  virtual const mesh::LocalIdentity& rcliUseClientIdentity() = 0;

  /* Verstuur een klaargemaakt pakket: DIRECT met pad, anders flood MET scope. */
  virtual void rcliSend(mesh::Packet* pkt, const uint8_t* path, uint8_t path_len) = 0;

  /* Prefix (>=6 byte) -> volle 32-byte sleutel. Nodig voor het ECDH-geheim; met
   * alleen een prefix kan deze node eenvoudig niet met de tegenkant praten.
   * RoomMesh lost dit op uit de buurtlijst. false = onbekend of niet uniek. */
  virtual bool rcliResolvePubKey(const uint8_t* prefix, int prefix_len, uint8_t* out_full) = 0;
};

class RepeaterCli {
public:
  enum State : uint8_t {
    RCLI_IDLE = 0,   // vrij
    RCLI_LOGIN,      // login onderweg, wachten op RESP_SERVER_LOGIN_OK
    RCLI_CMD,        // een commando van de job onderweg, wachten op CLI_DATA
    RCLI_STATUS,     // REQ_TYPE_GET_STATUS onderweg, wachten op de stats-RESPONSE
    RCLI_DONE,       // job klaar (alle commando's afgehandeld of overgeslagen)
    RCLI_FAILED      // job afgebroken (login mislukt / bovengrens); error() zegt waarom
  };

  /* WAT VOOR JOB. Beide soorten delen de LOGIN (die is bij een statusverzoek net
   * zo verplicht: MyMesh::handleRequest wordt alleen bereikt via onPeerDataRecv,
   * en dat vereist dat de afzender in de ACL staat -- de anonieme weg kent alleen
   * LOGIN/REGIONS/OWNER/CLOCK). Ze verschillen in wat er NA de login gebeurt:
   * CLI-tekst (TXT_MSG met TXT_TYPE_CLI_DATA) of een REQ (PAYLOAD_TYPE_REQ met
   * REQ_TYPE_GET_STATUS). */
  enum JobKind : uint8_t { RCLI_JOB_CLI = 0, RCLI_JOB_STATUS };

  /* Wat een job-start teruggeeft. */
  enum Enq : uint8_t {
    RCLI_OK = 0,
    RCLI_BUSY,        // er loopt er al een
    RCLI_BAD_KEY,     // geen geldige hex, of prefix niet op te lossen
    RCLI_BAD_ARG,     // leeg/te lang commando of wachtwoord, of lege job
    RCLI_NO_HOST      // begin() is nooit aangeroepen (sensor-variant)
  };

  /* Uitslag PER COMMANDO. value == nullptr betekent GEEN ANTWOORD -- dat is geen
   * fout maar informatie: MeshManager wil "gevraagd, geen antwoord" (null) zien in
   * plaats van stilte (zie routes_api.repeater_settings, waar None -> onbeantwoord).
   *
   * param is de MeshManager-sleutel zoals die terug moet ("name", "cmd:filter
   * count"); value de ruwe antwoordregel of nullptr. Bewust een callback en geen
   * directe PushTask-aanroep: deze module hoort niets van HTTP of tokens te weten.
   * De callback mag alleen KOPIEREN -- hij draait vanuit de ontvangstlus. */
  typedef void (*ResultFn)(void* ctx, const char* pubkey_hex12, const char* param,
                           const char* value);

  /* Uitslag van een STATUSverzoek. Wordt ALLEEN aangeroepen als het antwoord
   * volledig ontleed EN plausibel was; mislukt de ronde, dan komt er niets --
   * geen half gevulde meting, geen verzonnen nul (zie parseStatus). */
  typedef void (*StatsFn)(void* ctx, const char* pubkey_hex12, const RepeaterStatus& st);

  RepeaterCli() { _host = nullptr; _mesh = nullptr; reset(); _have_cached_path = false;
                  _result_fn = nullptr; _result_ctx = nullptr;
                  _stats_fn = nullptr; _stats_ctx = nullptr; }

  void begin(mesh::Mesh* mesh, RepeaterCliHost* host) { _mesh = mesh; _host = host; }
  void setResultCallback(ResultFn fn, void* ctx) { _result_fn = fn; _result_ctx = ctx; }
  void setStatsCallback(StatsFn fn, void* ctx)   { _stats_fn = fn; _stats_ctx = ctx; }

  /* HANDMATIG (/cli/remote): EEN opdracht.
   * pubkey_hex 12..64 hex; password <=15; command letterlijk. Wordt intern een job
   * van EEN, afgeleverd onder de sleutel "cmd:<command>". */
  Enq queue(const char* pubkey_hex, const char* password, const char* command);

  /* POLLER: een JOB van N parameters uit de MeshManager-wachtrij.
   * params_csv is de komma-lijst zoals de server hem gaf ("name,role,cmd:region").
   * Elke parameter P wordt een commando: begint P met "cmd:" -> stuur P+4 letterlijk;
   * anders -> "get P". Het antwoord wordt afgeleverd onder de OORSPRONKELIJKE
   * parameternaam P. Dit is exact de vertaling die de oude HA-pusher deed. */
  Enq queueJob(const char* pubkey_hex, const char* password, const char* params_csv);

  /* POLLER: een STATUSVERZOEK (v2.7.0). Eenmaal inloggen, dan EEN
   * REQ_TYPE_GET_STATUS, en het antwoord via de stats-callback. Een leesactie, dus
   * de gewone drie pogingen (isMutating is hier niet van toepassing). */
  Enq queueStatus(const char* pubkey_hex, const char* password);

  /* Uit de gewone loop(). Doet hoogstens EEN zending per ronde. */
  void loop();

  /* ---- inkomend, aangeroepen door de meshklasse ---- */
  bool matchesSrcHash(const uint8_t* hash) const {
    return (_state == RCLI_LOGIN || _state == RCLI_CMD || _state == RCLI_STATUS)
           && _target_pub[0] == hash[0];
  }
  void fillSharedSecret(uint8_t* dest) const { memcpy(dest, _secret, PUB_KEY_SIZE); }
  bool onPeerData(uint8_t type, const uint8_t* data, size_t len);
  void onPath(const uint8_t* path, uint8_t path_len, uint8_t extra_type,
              const uint8_t* extra, uint8_t extra_len);

  /* ---- uitlezen (webinterface, statuspagina) ---- */
  State       state() const   { return _state; }
  bool        busy() const    { return _state == RCLI_LOGIN || _state == RCLI_CMD
                                    || _state == RCLI_STATUS; }
  const char* error() const   { return _error; }
  const char* command() const { return _cmd; }         // het LOPENDE commando
  const char* answer() const  { return _answer; }       // antwoord op het lopende cmd
  const char* targetHex() const { return _target_hex; }
  uint8_t     attempt() const { return _attempt; }
  uint8_t     jobIndex() const { return _job_i; }
  uint8_t     jobCount() const { return _job_n; }
  uint32_t    elapsedSecs() const {
    return busy() ? (uint32_t)((millis() - _started_at) / 1000) : 0;
  }

  /* Herkent een opdracht die iets VERANDERT. Publiek: de webroute en de poller
   * baseren er hun bevestigings-/weigerregels op, en er mag maar EEN lijst zijn. */
  static bool isMutating(const char* cmd);

private:
  mesh::Mesh*      _mesh;
  RepeaterCliHost* _host;
  ResultFn         _result_fn;
  void*            _result_ctx;
  StatsFn          _stats_fn;
  void*            _stats_ctx;

  JobKind  _job_kind;
  uint32_t _status_tag;   // de tag die de repeater in zijn antwoord terugkaatst

  State    _state;
  uint8_t  _attempt;          // pogingen aan het LOPENDE commando (of de login)
  uint8_t  _cmd_max_attempts; // 1 bij een muterend lopend commando, anders RCLI_MAX_ATTEMPTS

  uint8_t  _target_pub[PUB_KEY_SIZE];
  char     _target_hex[13];   // eerste 6 byte als 12 hex: het MeshManager-"node"-veld
  uint8_t  _secret[PUB_KEY_SIZE];
  char     _pass[RCLI_PASS_MAX];

  /* De JOB: de parameters/commando's als CSV (of een enkele opdracht), plus de
   * cursor. Zie RCLI_JOB_BUF voor waarom CSV. */
  char     _job[RCLI_JOB_BUF];
  uint8_t  _job_n;            // aantal commando's in de job
  uint8_t  _job_i;            // huidige (0.._job_n-1)

  char     _cur_param[RCLI_PARAM_MAX];  // de MeshManager-sleutel van het lopende cmd
  char     _cmd[RCLI_CMD_MAX + 1];      // het afgeleide, lopende commando
  char     _answer[RCLI_ANSWER_MAX];
  char     _error[80];

  /* Stukken van het lopende antwoord: elk stuk staat achter elkaar in _answer
   * (offset/lengte) met de tijdstempel uit zijn pakket; finishAnswer() sorteert
   * en rijgt aaneen. Zie RCLI_CHUNK_MAX voor waarom dit bestaat. */
  uint8_t  _nchunks;
  uint32_t _chunk_ts[RCLI_CHUNK_MAX];
  uint16_t _chunk_off[RCLI_CHUNK_MAX];
  uint16_t _chunk_len[RCLI_CHUNK_MAX];
  uint16_t _answer_used;
  unsigned long _collect_until;   // na dit moment zonder nieuw stuk: afleveren

  /* Het pad NAAR het doel, geleerd uit de PATH die de repeater op onze flood
   * terugstuurt. Bewaard over commando's EN over sessies heen (alleen RAM), puur
   * zendtijdwinst: na de eerste flood gaan alle volgende commando's direct. */
  uint8_t  _path[MAX_PATH_SIZE];
  uint8_t  _path_len;
  uint8_t  _cached_path[MAX_PATH_SIZE];
  uint8_t  _cached_path_len;
  uint8_t  _cached_pub[PUB_KEY_SIZE];
  bool     _have_cached_path;

  unsigned long _started_at;   // millis van de job-start
  unsigned long _next_send;    // niet zenden vóór dit moment (RCLI_MIN_GAP_MS)
  unsigned long _deadline;     // wanneer de huidige poging opgegeven wordt

  void reset();
  void failJob(const char* why);          // login/bovengrens: rest als null afleveren
  void deliverCurrent(const char* value); // huidig cmd afleveren (value of nullptr)
  void finishAnswer();                    // verzamelde stukken sorteren, afleveren, door
  bool loadCurrentParam();                // _cur_param + _cmd voor _job_i klaarzetten
  void beginNextCommand();                // naar het volgende commando (of DONE)
  void sendLogin();
  void sendCommand();
  void sendStatusReq();
  /* Ontleedt + toetst een stats-antwoord. false = niet plausibel of te kort; dan
   * wordt er NIETS gemeld. */
  static bool parseStatus(const uint8_t* data, size_t len, RepeaterStatus& out);

  /* De i-de parameter uit de job. Bij _job_n==1 is dat de HELE buffer (zodat een
   * handmatige opdracht met komma's heel blijft); anders het i-de komma-veld. */
  bool nthParam(uint8_t i, char* out, size_t out_len) const;

  /* Gemeenschappelijke job-start: sleutel oplossen, geheim rekenen, velden zetten.
   * commandMode: bij true is _job een enkele opdracht ("cmd:"+command met n=1). */
  Enq startJob(const char* pubkey_hex, const char* password);
};
