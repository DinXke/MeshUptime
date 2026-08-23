#include "RoomMesh.h"
#include "TimeFmt.h"

/* === KANAAL-COMMANDO-DIAGNOSE (v2.3.7) ======================================
 * ALTIJD-AAN seriële logging (MESH_DEBUG staat uit), prefix "[chan]" -- net als
 * [fixdiag]/[fixedge]. Zo is via COM4 te zien of een gemist kanaal-commando een
 * PARSE-probleem is (verkeerd geknipt/getrimd), een ONLEESBAAR pakket (verkeerde
 * sleutel door een 1-byte kanaal-hash-botsing) of gewoon RF-verlies (de LoRa-laag
 * mag pakketten missen -- dat is inherent, geen bug: dan verschijnt hier NIETS). */
#define CHAN_DIAG(...) do { Serial.printf("[chan] " __VA_ARGS__); Serial.println(); } while (0)

/* ============================================================================
 * RoomMesh -- implementatie. Zie RoomMesh.h voor het waarom.
 *
 * De room-mechaniek (login, post-opslag, client-sync, adverts) is die van
 * MeshCore's simple_room_server (v1.17.0), veralgemeend naar N room-slots met een
 * gedeelde post-pool. De multiroom-verdeling (welke room een pakket toebehoort)
 * gebeurt in onRecvPacket door self_id + _active_slot om te zetten -- het patroon
 * uit SIREN.
 * ==========================================================================*/

#define REPLY_DELAY_MILLIS          1500
#define PUSH_NOTIFY_DELAY_MILLIS    2000
#define SYNC_PUSH_INTERVAL          1200

#define PUSH_ACK_TIMEOUT_FLOOD      12000
#define PUSH_TIMEOUT_BASE           4000
#define PUSH_ACK_TIMEOUT_FACTOR     2000

#define POST_SYNC_DELAY_SECS        6
#define FIRMWARE_VER_LEVEL          1

#define REQ_TYPE_GET_STATUS         0x01
#define REQ_TYPE_KEEP_ALIVE         0x02
#define REQ_TYPE_GET_TELEMETRY_DATA 0x03
#define REQ_TYPE_GET_ACCESS_LIST    0x05

#define RESP_SERVER_LOGIN_OK        0

#define LAZY_CONTACTS_WRITE_DELAY   5000
#define ALERT_ACK_EXPIRY_MILLIS     8000
#define SENSOR_READ_INTERVAL_SECS_DEF  60

#ifndef SENSOR_READ_INTERVAL_SECS
  #define SENSOR_READ_INTERVAL_SECS  SENSOR_READ_INTERVAL_SECS_DEF
#endif

#define FLOOD_ADVERT_INTERVAL_MS    (47UL * 60UL * 60UL * 1000UL)

/* Bestandsnamen voor persistentie. Room 0's identiteit staat in "_main" (de
 * bestaande hoofdsleutel); extra rooms krijgen "/room_id_N". */
#define ROOM_CFG_PATH   "/room_cfg"
/* Sensor-node-config (namen/actief/stealth) en identiteiten (/snode_id_N). */
#define SNODE_CFG_PATH  "/snode_cfg"
/* Bot: identiteit via IdentityStore ("/bot_id"); de DM-ontvangerslijst als tekst. */
#define BOT_ID_NAME      "/bot_id"
#define BOT_RECIPS_PATH  "/bot_recips"
#define CHANNELS_CFG_PATH "/channels.cfg"
/* De vaste, publiek bekende sleutel van het MeshCore-standaardkanaal "Public"
 * (docs/faq.md + qr_codes.md). Een naam-only kanaal "public" krijgt DEZE sleutel
 * (niet sha256("public")), zodat de node op het ECHTE publieke kanaal uitkomt. */
#define PUBLIC_GROUP_SECRET_HEX  "8b3387e9c5cdea6ac9e5edbaa115cd72"
/* Eigenaar-seed: de lijst begint met deze pubkey op een verse node. */
#define BOT_OWNER_SEED_HEX  "2cb0c5eb473757805eab00f9dd0594c229d6e50b2acec6b57403b6259c9e126f"

/* Vooruit-declaratie: de join-URI-encoder staat lager in dit bestand. */
static void roomUrlEncode(const char* in, char* out, size_t out_len);

struct ServerStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;
  int16_t  last_snr;
  uint16_t n_direct_dups, n_flood_dups;
  uint16_t n_posted, n_post_push;
};

/* ------------------------------------------------------------------ */
/*  Constructor / begin                                                 */
/* ------------------------------------------------------------------ */
RoomMesh::RoomMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms,
                   mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      region_map(key_store), temp_map(key_store),
      cli_acl(rooms[0].acl),
      _cli(board, rtc, sensors, region_map, cli_acl, &_prefs, this),
      telemetry(MAX_PACKET_PAYLOAD - 4)
{
  _fs = NULL;
  _num_active_rooms = 0;
  _active_slot = 0;
  _num_active_snodes = 0;
  _active_snode = -1;
  last_millis = 0;
  uptime_millis = 0;
  _logging = false;
  region_load_active = false;
  set_radio_at = revert_radio_at = 0;
  recv_pkt_region = NULL;
  last_read_time = 0;
  num_alert_tasks = 0;
  _post_cb = NULL;
  _post_cb_ctx = NULL;
  _bot_active = false;
  _bot_diag = true;   // verklikker standaard AAN; persistente stand komt uit loadBotRecips()
  _bot_name[0] = 0;
  _bot_next_local_advert = _bot_next_flood_advert = 0;
  memset(_bot_recips, 0, sizeof(_bot_recips));
  _active_is_bot = false;
  _bot_match_n = 0;
  memset(_channels, 0, sizeof(_channels));

  memset(rooms, 0, sizeof(rooms));
  for (int i = 0; i < MAX_ROOMS; i++) {
    rooms[i].next_client_idx = 0;
    rooms[i].stealth = false;
  }
  memset(snodes, 0, sizeof(snodes));
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    snodes[i].next_client_idx = 0;
    snodes[i].stealth = false;
  }
  memset(_grants, 0, sizeof(_grants));   // level 0 = vrij
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    memset(&_post_pool[i], 0, sizeof(PostInfo));
    _post_pool[i].room_idx = 0xFF;
  }

  // radio / mesh defaults (gelijk aan de room-server)
  _prefs.airtime_factor = 1.0;
  _prefs.rx_delay_base = 0.0f;
  _prefs.tx_delay_factor = 0.5f;
  _prefs.direct_tx_delay_factor = 0.2f;
  StrHelper::strncpy(_prefs.node_name, "MeshUptime", sizeof(_prefs.node_name));
  _prefs.node_lat = ADVERT_LAT;
  _prefs.node_lon = ADVERT_LON;
  StrHelper::strncpy(_prefs.password, ADMIN_PASSWORD, sizeof(_prefs.password));
  _prefs.freq = LORA_FREQ;
  _prefs.sf = LORA_SF;
  _prefs.bw = LORA_BW;
  _prefs.cr = LORA_CR;
  _prefs.tx_power_dbm = LORA_TX_POWER;
  _prefs.disable_fwd = 1;
  _prefs.advert_interval = 1;         // 2 minuten
  _prefs.flood_advert_interval = 47;  // uren
  _prefs.flood_max = 64;
  _prefs.interference_threshold = 0;
  _prefs.cad_enabled = 0;
  _prefs.gps_enabled = 0;
  _prefs.gps_interval = 0;
  _prefs.advert_loc_policy = ADVERT_LOC_PREFS;
  _prefs.radio_fem_rxgain = 1;
  _prefs.rx_boosted_gain = 1;   // SX1262 op de Heltec V3; standaard aan

  memset(default_scope.key, 0, sizeof(default_scope.key));
}

void RoomMesh::begin(FILESYSTEM* fs) {
  mesh::Mesh::begin();
  _fs = fs;
  _cli.loadPrefs(_fs);

  bool had_room_cfg = (_fs != NULL) && _fs->exists(ROOM_CFG_PATH);
  loadRoomConfig();   // namen/wachtwoorden/stealth + welke slots actief zijn

  /* Room 0 is de hoofdidentiteit: setRoom0Identity() heeft rooms[0].id al gezet
   * met de sleutel uit "_main" (main_room.cpp). We maken hem alleen actief en
   * geven hem een standaardnaam als er nog geen config was. */
  if (!rooms[0].active) {
    rooms[0].active = true;
    _num_active_rooms++;
  }
  if (rooms[0].name[0] == 0) StrHelper::strncpy(rooms[0].name, "Storingen", sizeof(rooms[0].name));
  if (rooms[0].password[0] == 0) StrHelper::strncpy(rooms[0].password, _prefs.password, sizeof(rooms[0].password));

  /* EERSTE KEER (geen room-config op schijf): een zinvolle standaardindeling.
   * Room 0 = "Storingen" (de hoofdidentiteit, keys behouden), room 1 =
   * "Telemetrie" met een eigen sleutelpaar. Daarna herconfigureerbaar via de
   * room-CLI en bewaard. */
  if (!had_room_cfg && MAX_ROOMS >= 2 && !rooms[1].active) {
    rooms[1].active = true;
    _num_active_rooms++;
    StrHelper::strncpy(rooms[1].name, "Telemetrie", sizeof(rooms[1].name));
    StrHelper::strncpy(rooms[1].password, _prefs.password, sizeof(rooms[1].password));
    rooms[1].guest_password[0] = 0;
    rooms[1].stealth = false;
  }

  /* Extra rooms: eigen sleutelpaar laden of aanmaken. */
  for (int i = 1; i < MAX_ROOMS; i++) {
    if (rooms[i].active) loadOrCreateRoomIdentity(i);
  }

  if (!had_room_cfg) saveRoomConfig();

  /* ---- VIRTUELE SENSOR-NODES ----
   * Config (namen/actief/stealth) laden; bij een verse node één actieve sensor-
   * node met de vertrouwde uptime-naam. Elke actieve node krijgt een eigen
   * sleutelpaar (persistent, /snode_id_N) en het beheerderswachtwoord van deze
   * node, zodat de MeshCore-app na inloggen de telemetrie ziet. */
  bool had_snode_cfg = (_fs != NULL) && _fs->exists(SNODE_CFG_PATH);
  loadSensorNodeConfig();
  if (!had_snode_cfg && !snodes[0].active) {
    snodes[0].active = true;
    _num_active_snodes++;
    StrHelper::strncpy(snodes[0].name, "BE-HSS-DinX-Up", sizeof(snodes[0].name));
  }
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    if (!snodes[i].active) continue;
    loadOrCreateSensorNodeIdentity(i);
    if (snodes[i].password[0] == 0)
      StrHelper::strncpy(snodes[i].password, _prefs.password, sizeof(snodes[i].password));
    snodes[i].guest_password[0] = 0;
  }
  if (!had_snode_cfg) saveSensorNodeConfig();

  /* Per-sleutel toegangsgrants laden en op de (nu actieve) slots toepassen. Moet
   * NA het activeren van rooms + sensor-nodes gebeuren, want applyPermissions
   * gebruikt de slot-identiteit voor het gedeelde geheim. */
  loadAclGrants();

  /* ---- BOT: CHAT/notifier-identiteit ----
   * Eigen persistent sleutelpaar (/bot_id) + DM-ontvangerslijst (/bot_recips). Op
   * een verse node wordt de lijst met de eigenaar-pubkey geseed, zodat dm/both-
   * alerts meteen ergens aankomen. De bot is altijd actief (niet-stealth). */
  bool had_bot_recips = (_fs != NULL) && _fs->exists(BOT_RECIPS_PATH);
  loadOrCreateBotIdentity();
  _bot_active = true;
  if (_bot_name[0] == 0) StrHelper::strncpy(_bot_name, BOT_NAME_DEFAULT, sizeof(_bot_name));
  loadBotRecips();
  if (!had_bot_recips) {
    uint8_t seed[PUB_KEY_SIZE];
    if (mesh::Utils::fromHex(seed, PUB_KEY_SIZE, BOT_OWNER_SEED_HEX)) botRecipAdd(seed);
    saveBotRecips();
  }

  /* Hashtag-/publieke kanalen die de bot meeleest (persistent, /channels.cfg). */
  loadChannels();

  /* Alleen room 0's ACL wordt bewaard (in het bestaande contacts-bestand); de
   * ACL's van de extra rooms en de sensor-nodes leven in RAM en worden opnieuw
   * opgebouwd zodra een client inlogt -- zoals in SIREN's fase 1. */
  rooms[0].acl.load(_fs, rooms[0].id);

  region_map.load(_fs);
  {
    RegionEntry* r = region_map.getDefaultRegion();
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
#ifdef DEFAULT_FLOOD_SCOPE_NAME
      r = region_map.findByName(DEFAULT_FLOOD_SCOPE_NAME);
      if (r == NULL) {
        r = region_map.putRegion(DEFAULT_FLOOD_SCOPE_NAME, 0);
        if (r) r->flags = 0;
      }
      if (r) {
        region_map.setDefaultRegion(r);
        region_map.getTransportKeysFor(*r, &default_scope, 1);
      }
#endif
    }
  }

  radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  radio_driver.setTxPower(_prefs.tx_power_dbm);
  radio_driver.setRxBoostedGainMode(_prefs.rx_boosted_gain);
  board.setLoRaFemLnaEnabled(_prefs.radio_fem_rxgain);

  updateAdvertTimer();
  updateFloodAdvertTimer();
  board.setAdcMultiplier(_prefs.adc_multiplier);

#if ENV_INCLUDE_GPS == 1
  applyGpsPrefs();
#endif
}

/* ------------------------------------------------------------------ */
/*  Identiteit + config persistentie                                    */
/* ------------------------------------------------------------------ */
void RoomMesh::saveRoomIdentity(int idx) {
  if (_fs == NULL || idx <= 0 || idx >= MAX_ROOMS) return;   // idx 0 = "_main", niet hier
  char path[24];
  snprintf(path, sizeof(path), "/room_id_%d", idx);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#else
  IdentityStore store(*_fs, "/identity");
#endif
  store.save(path, rooms[idx].id);
}

void RoomMesh::loadOrCreateRoomIdentity(int idx) {
  if (_fs == NULL || idx <= 0 || idx >= MAX_ROOMS) return;
  char path[24];
  snprintf(path, sizeof(path), "/room_id_%d", idx);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#else
  IdentityStore store(*_fs, "/identity");
#endif
  if (!store.load(path, rooms[idx].id)) {
    rooms[idx].id = radio_new_identity();
    int count = 0;
    while (count < 10 && (rooms[idx].id.pub_key[0] == 0x00 || rooms[idx].id.pub_key[0] == 0xFF)) {
      rooms[idx].id = radio_new_identity(); count++;
    }
    store.save(path, rooms[idx].id);
  }
}

/* Room-config als tekst, regelgebaseerd. Eén regel per actieve room:
 *   r <idx> <stealth> <naam>\t<guest_password>
 * Wachtwoorden met spaties worden niet ondersteund; naam en guest gescheiden met
 * een TAB zodat een naam met spaties toch mag. */
void RoomMesh::saveRoomConfig() {
  if (_fs == NULL) return;
  File f = _fs->open(ROOM_CFG_PATH, "w", true);
  if (!f) return;
  f.printf("#MUROOM1\n");
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    f.printf("r %d %d %s\t%s\t%s\n", i, rooms[i].stealth ? 1 : 0,
             rooms[i].name, rooms[i].guest_password, rooms[i].password);
  }
  f.printf(".\n");
  f.close();
}

void RoomMesh::loadRoomConfig() {
  if (_fs == NULL || !_fs->exists(ROOM_CFG_PATH)) return;
  File f = _fs->open(ROOM_CFG_PATH, "r");
  if (!f) return;

  char line[160];
  bool first = true;
  while (f.available()) {
    size_t len = 0;
    while (f.available() && len < sizeof(line) - 1) {
      int c = f.read();
      if (c < 0 || c == '\n') break;
      if (c == '\r') continue;
      line[len++] = (char)c;
    }
    line[len] = 0;
    if (first) { first = false; continue; }   // kenregel overslaan
    if (line[0] == '.' || line[0] == 0) continue;
    if (line[0] != 'r') continue;

    // r <idx> <stealth> <naam>\t<guest>\t<pass>
    char* p = line + 1;
    while (*p == ' ') p++;
    int idx = atoi(p);
    while (*p && *p != ' ') p++;   // idx
    while (*p == ' ') p++;
    int stealth = atoi(p);
    while (*p && *p != ' ') p++;   // stealth
    while (*p == ' ') p++;
    char* name = p;
    char* guest = strchr(p, '\t');
    char* pass = NULL;
    if (guest) { *guest++ = 0; pass = strchr(guest, '\t'); if (pass) *pass++ = 0; }

    if (idx < 0 || idx >= MAX_ROOMS) continue;
    if (!rooms[idx].active) { rooms[idx].active = true; _num_active_rooms++; }
    rooms[idx].stealth = stealth ? true : false;
    StrHelper::strncpy(rooms[idx].name, name, sizeof(rooms[idx].name));
    if (guest) StrHelper::strncpy(rooms[idx].guest_password, guest, sizeof(rooms[idx].guest_password));
    if (pass && pass[0]) StrHelper::strncpy(rooms[idx].password, pass, sizeof(rooms[idx].password));
  }
  f.close();
}

/* ------------------------------------------------------------------ */
/*  Sensor-node identiteit + config persistentie                       */
/* ------------------------------------------------------------------ */
void RoomMesh::saveSensorNodeIdentity(int idx) {
  if (_fs == NULL || idx < 0 || idx >= MAX_SENSOR_NODES) return;
  char path[24];
  snprintf(path, sizeof(path), "/snode_id_%d", idx);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#else
  IdentityStore store(*_fs, "/identity");
#endif
  store.save(path, snodes[idx].id);
}

void RoomMesh::loadOrCreateSensorNodeIdentity(int idx) {
  if (_fs == NULL || idx < 0 || idx >= MAX_SENSOR_NODES) return;
  char path[24];
  snprintf(path, sizeof(path), "/snode_id_%d", idx);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#else
  IdentityStore store(*_fs, "/identity");
#endif
  if (!store.load(path, snodes[idx].id)) {
    snodes[idx].id = radio_new_identity();
    int count = 0;
    while (count < 10 && (snodes[idx].id.pub_key[0] == 0x00 || snodes[idx].id.pub_key[0] == 0xFF)) {
      snodes[idx].id = radio_new_identity(); count++;
    }
    store.save(path, snodes[idx].id);
  }
}

/* Sensor-node-config als tekst, één regel per actieve node:
 *   s <idx> <stealth> <naam>
 * (geen wachtwoorden: die nemen de node-standaard over bij begin()). */
void RoomMesh::saveSensorNodeConfig() {
  if (_fs == NULL) return;
  File f = _fs->open(SNODE_CFG_PATH, "w", true);
  if (!f) return;
  f.printf("#MUSNODE1\n");
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    if (!snodes[i].active) continue;
    f.printf("s %d %d %s\n", i, snodes[i].stealth ? 1 : 0, snodes[i].name);
  }
  f.printf(".\n");
  f.close();
}

void RoomMesh::loadSensorNodeConfig() {
  if (_fs == NULL || !_fs->exists(SNODE_CFG_PATH)) return;
  File f = _fs->open(SNODE_CFG_PATH, "r");
  if (!f) return;

  char line[96];
  bool first = true;
  while (f.available()) {
    size_t len = 0;
    while (f.available() && len < sizeof(line) - 1) {
      int c = f.read();
      if (c < 0 || c == '\n') break;
      if (c == '\r') continue;
      line[len++] = (char)c;
    }
    line[len] = 0;
    if (first) { first = false; continue; }   // kenregel overslaan
    if (line[0] == '.' || line[0] == 0) continue;
    if (line[0] != 's') continue;

    // s <idx> <stealth> <naam>
    char* p = line + 1;
    while (*p == ' ') p++;
    int idx = atoi(p);
    while (*p && *p != ' ') p++;   // idx
    while (*p == ' ') p++;
    int stealth = atoi(p);
    while (*p && *p != ' ') p++;   // stealth
    while (*p == ' ') p++;
    char* name = p;                // rest = naam (mag spaties bevatten)

    if (idx < 0 || idx >= MAX_SENSOR_NODES) continue;
    if (!snodes[idx].active) { snodes[idx].active = true; _num_active_snodes++; }
    snodes[idx].stealth = stealth ? true : false;
    if (name[0]) StrHelper::strncpy(snodes[idx].name, name, sizeof(snodes[idx].name));
  }
  f.close();
}

/* ------------------------------------------------------------------ */
/*  Per-sleutel toegangsgrants (wachtwoordloze toegang op basis van key) */
/* ------------------------------------------------------------------ */
#define ACL_GRANTS_PATH  "/acl_grants"

RoomSlot* RoomMesh::slotRef(int kind, int idx) {
  if (kind == ACL_KIND_ROOM) {
    if (idx >= 0 && idx < MAX_ROOMS && rooms[idx].active) return &rooms[idx];
  } else if (kind == ACL_KIND_SNODE) {
    if (idx >= 0 && idx < MAX_SENSOR_NODES && snodes[idx].active) return &snodes[idx];
  }
  return NULL;
}

uint8_t RoomMesh::grantLookup(int kind, int slot, const uint8_t* pubkey) {
  for (int i = 0; i < MAX_ACL_GRANTS; i++) {
    if (_grants[i].level == 0) continue;
    if (_grants[i].kind == kind && _grants[i].slot == slot &&
        memcmp(_grants[i].pub_key, pubkey, PUB_KEY_SIZE) == 0) {
      return _grants[i].level;
    }
  }
  return 0;
}

int RoomMesh::aclGrantSet(int kind, int slot, const uint8_t* pubkey, int key_len, uint8_t level) {
  if (slotRef(kind, slot) == NULL) return -1;
  if (key_len != PUB_KEY_SIZE) return -2;                 // toevoegen vraagt VOLLEDIGE key
  level &= PERM_ACL_ROLE_MASK;
  if (level < PERM_ACL_READ_ONLY) return -2;              // 1..3
  /* Bestaande grant voor (kind,slot,pubkey) bijwerken, anders een vrije nemen. */
  int free_i = -1;
  for (int i = 0; i < MAX_ACL_GRANTS; i++) {
    if (_grants[i].level == 0) { if (free_i < 0) free_i = i; continue; }
    if (_grants[i].kind == kind && _grants[i].slot == slot &&
        memcmp(_grants[i].pub_key, pubkey, PUB_KEY_SIZE) == 0) {
      _grants[i].level = level;
      RoomSlot* s = slotRef(kind, slot);
      if (s) s->acl.applyPermissions(s->id, pubkey, PUB_KEY_SIZE, level);  // live
      saveAclGrants();
      return 0;
    }
  }
  if (free_i < 0) return -3;                              // tabel vol
  _grants[free_i].kind = (uint8_t)kind;
  _grants[free_i].slot = (uint8_t)slot;
  _grants[free_i].level = level;
  memcpy(_grants[free_i].pub_key, pubkey, PUB_KEY_SIZE);
  RoomSlot* s = slotRef(kind, slot);
  if (s) s->acl.applyPermissions(s->id, pubkey, PUB_KEY_SIZE, level);      // live nu al
  saveAclGrants();
  return 0;
}

int RoomMesh::aclGrantDel(int kind, int slot, const uint8_t* prefix, int key_len) {
  if (slotRef(kind, slot) == NULL) return -1;
  if (key_len < 6 || key_len > PUB_KEY_SIZE) return -2;
  int match = -1, count = 0;
  for (int i = 0; i < MAX_ACL_GRANTS; i++) {
    if (_grants[i].level == 0) continue;
    if (_grants[i].kind == kind && _grants[i].slot == slot &&
        memcmp(_grants[i].pub_key, prefix, key_len) == 0) { match = i; count++; }
  }
  if (count == 0) return -2;
  if (count > 1) return -3;                               // dubbelzinnige prefix
  /* Uit de runtime-ACL halen (applyPermissions met GUEST verwijdert), dan grant vrij. */
  RoomSlot* s = slotRef(kind, slot);
  if (s) s->acl.applyPermissions(s->id, _grants[match].pub_key, PUB_KEY_SIZE, PERM_ACL_GUEST);
  _grants[match].level = 0;
  saveAclGrants();
  return 1;
}

/* Persistentie: één regel per grant  ->  g <kind> <slot> <level> <pubkeyhex> */
void RoomMesh::saveAclGrants() {
  if (_fs == NULL) return;
  File f = _fs->open(ACL_GRANTS_PATH, "w", true);
  if (!f) return;
  f.printf("#MUACL1\n");
  char hex[PUB_KEY_SIZE * 2 + 1];
  for (int i = 0; i < MAX_ACL_GRANTS; i++) {
    if (_grants[i].level == 0) continue;
    mesh::Utils::toHex(hex, _grants[i].pub_key, PUB_KEY_SIZE);
    f.printf("g %d %d %d %s\n", (int)_grants[i].kind, (int)_grants[i].slot,
             (int)_grants[i].level, hex);
  }
  f.printf(".\n");
  f.close();
}

void RoomMesh::loadAclGrants() {
  if (_fs == NULL || !_fs->exists(ACL_GRANTS_PATH)) return;
  File f = _fs->open(ACL_GRANTS_PATH, "r");
  if (!f) return;
  char line[160];
  bool first = true;
  int gi = 0;
  while (f.available() && gi < MAX_ACL_GRANTS) {
    size_t len = 0;
    while (f.available() && len < sizeof(line) - 1) {
      int c = f.read();
      if (c < 0 || c == '\n') break;
      if (c == '\r') continue;
      line[len++] = (char)c;
    }
    line[len] = 0;
    if (first) { first = false; continue; }
    if (line[0] != 'g') continue;
    // g <kind> <slot> <level> <hex>
    char* p = line + 1;
    while (*p == ' ') p++;
    int kind = atoi(p);   while (*p && *p != ' ') p++; while (*p == ' ') p++;
    int slot = atoi(p);   while (*p && *p != ' ') p++; while (*p == ' ') p++;
    int level = atoi(p);  while (*p && *p != ' ') p++; while (*p == ' ') p++;
    char* hex = p;
    if ((kind != ACL_KIND_ROOM && kind != ACL_KIND_SNODE) ||
        level < PERM_ACL_READ_ONLY || level > PERM_ACL_ADMIN) continue;
    uint8_t pubkey[PUB_KEY_SIZE];
    if (strlen(hex) < PUB_KEY_SIZE * 2) continue;
    if (!mesh::Utils::fromHex(pubkey, PUB_KEY_SIZE, hex)) continue;
    _grants[gi].kind = (uint8_t)kind;
    _grants[gi].slot = (uint8_t)slot;
    _grants[gi].level = (uint8_t)level;
    memcpy(_grants[gi].pub_key, pubkey, PUB_KEY_SIZE);
    /* Live in de runtime-ACL zetten als het slot actief is. */
    RoomSlot* s = slotRef(kind, slot);
    if (s) s->acl.applyPermissions(s->id, pubkey, PUB_KEY_SIZE, (uint8_t)level);
    gi++;
  }
  f.close();
}

/* ---- IWebNode: ACL-weergave/-beheer ---- */
int RoomMesh::webAclCount(int kind, int slot) {
  int n = 0;
  for (int i = 0; i < MAX_ACL_GRANTS; i++)
    if (_grants[i].level != 0 && _grants[i].kind == kind && _grants[i].slot == slot) n++;
  return n;
}

bool RoomMesh::webAclGet(int kind, int slot, int idx, char* pub64, size_t out_len, int* level) {
  if (out_len < (size_t)(PUB_KEY_SIZE * 2 + 1)) return false;
  int n = 0;
  for (int i = 0; i < MAX_ACL_GRANTS; i++) {
    if (_grants[i].level == 0 || _grants[i].kind != kind || _grants[i].slot != slot) continue;
    if (n == idx) {
      mesh::Utils::toHex(pub64, _grants[i].pub_key, PUB_KEY_SIZE);
      if (level) *level = _grants[i].level;
      return true;
    }
    n++;
  }
  return false;
}

int RoomMesh::webAclSet(int kind, int slot, const char* pub_hex, int level) {
  uint8_t pubkey[PUB_KEY_SIZE];
  if (!pub_hex || strlen(pub_hex) != PUB_KEY_SIZE * 2) return -2;
  if (!mesh::Utils::fromHex(pubkey, PUB_KEY_SIZE, pub_hex)) return -2;
  return aclGrantSet(kind, slot, pubkey, PUB_KEY_SIZE, (uint8_t)level);
}

int RoomMesh::webAclDel(int kind, int slot, const char* prefix_hex) {
  if (!prefix_hex) return -2;
  int hexlen = (int)strlen(prefix_hex);
  if (hexlen < 12 || (hexlen & 1) || hexlen > PUB_KEY_SIZE * 2) return -2;
  uint8_t prefix[PUB_KEY_SIZE];
  if (!mesh::Utils::fromHex(prefix, hexlen / 2, prefix_hex)) return -2;
  return aclGrantDel(kind, slot, prefix, hexlen / 2);
}

/* ------------------------------------------------------------------ */
/*  onRecvPacket -- multiroom-dispatch (SIREN-patroon)                  */
/* ------------------------------------------------------------------ */
mesh::DispatcherAction RoomMesh::onRecvPacket(mesh::Packet* pkt) {
  // regio-scope bepalen (zoals de room-server)
  if (pkt->getRouteType() == ROUTE_TYPE_TRANSPORT_FLOOD) {
    recv_pkt_region = region_map.findMatch(pkt, REGION_DENY_FLOOD);
  } else if (pkt->getRouteType() == ROUTE_TYPE_FLOOD) {
    if (region_map.getWildcard().flags & REGION_DENY_FLOOD) recv_pkt_region = NULL;
    else recv_pkt_region = &region_map.getWildcard();
  } else {
    recv_pkt_region = NULL;
  }

  uint8_t ptype = pkt->getPayloadType();
  if (pkt->payload_len >= 1 &&
      (ptype == PAYLOAD_TYPE_ANON_REQ || ptype == PAYLOAD_TYPE_PATH ||
       ptype == PAYLOAD_TYPE_REQ || ptype == PAYLOAD_TYPE_RESPONSE ||
       ptype == PAYLOAD_TYPE_TXT_MSG)) {
    uint8_t dest_hash = pkt->payload[0];
    for (int s = 0; s < MAX_ROOMS; s++) {
      if (!rooms[s].active) continue;
      if (rooms[s].id.isHashMatch(&dest_hash)) {
        _active_slot = s;
        _active_snode = -1;
        _active_is_bot = false;
        self_id = rooms[s].id;   // basisklasse ontsleutelt met de juiste sleutel
        return mesh::Mesh::onRecvPacket(pkt);
      }
    }
    /* Geen room-treffer: kijk of het pakket voor een virtuele sensor-node is. */
    for (int s = 0; s < MAX_SENSOR_NODES; s++) {
      if (!snodes[s].active) continue;
      if (snodes[s].id.isHashMatch(&dest_hash)) {
        _active_snode = s;
        _active_slot = 0;        // veilige waarde; activeSlot() kiest op _active_snode
        _active_is_bot = false;
        self_id = snodes[s].id;
        return mesh::Mesh::onRecvPacket(pkt);
      }
    }
    /* Geen room/snode-treffer: is het pakket voor de BOT-identiteit? Dan met de
     * botsleutel ontsleutelen. Zo wordt de bot tweerichting (het ontbrekende stuk
     * dat inkomende DM's naar hem liet vallen naar rooms[0].id). */
    if (_bot_active && _bot_id.isHashMatch(&dest_hash)) {
      _active_slot = 0;
      _active_snode = -1;
      _active_is_bot = true;
      self_id = _bot_id;
      return mesh::Mesh::onRecvPacket(pkt);
    }
    _active_slot = 0;
    _active_snode = -1;
    _active_is_bot = false;
    self_id = rooms[0].id;
    return mesh::Mesh::onRecvPacket(pkt);
  }

  _active_slot = 0;
  _active_snode = -1;
  _active_is_bot = false;
  self_id = rooms[0].id;
  return mesh::Mesh::onRecvPacket(pkt);
}

bool RoomMesh::allowPacketForward(const mesh::Packet* packet) {
  if (_prefs.disable_fwd) return false;
  if (packet->isRouteFlood()) {
    if (packet->getPathHashCount() >= _prefs.flood_max) return false;
  }
  return true;
}

void RoomMesh::onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp,
                            const uint8_t* app_data, size_t app_data_len) {
  AdvertDataParser parser(app_data, app_data_len);
  neighbours.noteAdvert(id.pub_key,
                        (parser.isValid() && parser.hasName()) ? parser.getName() : NULL,
                        parser.isValid() ? parser.getType() : ADV_TYPE_NONE,
                        packet->_snr,
                        packet->getPathHashCount(),
                        getRTCClock()->getCurrentTime());
}

int RoomMesh::searchPeersByHash(const uint8_t* hash) {
  /* BOT: er is geen ACL/login. De afzender-pubkey komt uit de buurtlijst (adverts)
   * en/of de ontvangerslijst; we bewaren de VOLLEDIGE pubkeys zodat
   * getPeerSharedSecret het gedeelde geheim eruit kan berekenen. Zo kan de bot een
   * DM ontsleutelen van iedereen wiens advert hij hoorde (of die op de lijst staat),
   * zonder wachtwoord-login. */
  if (_active_is_bot) {
    _bot_match_n = 0;
    /* Buurtlijst: iedereen wiens advert we hoorden (volledige pubkey bekend). */
    for (int i = 0; i < neighbours.getNumEntries() && _bot_match_n < 4; i++) {
      const NeighbourEntry* e = neighbours.getEntryByIdx(i);
      if (e && e->pub_key[0] == hash[0]) {
        memcpy(_bot_match_pub[_bot_match_n++], e->pub_key, PUB_KEY_SIZE);
      }
    }
    /* Ontvangerslijst: de eigenaar/vertrouwden staan hier met volledige pubkey,
     * ook als hun advert (net) niet in de buurtlijst zit. Dubbels vermijden. */
    for (int r = 0; r < MAX_BOT_RECIPS && _bot_match_n < 4; r++) {
      if (_bot_recips[r].level == 0) continue;
      if (_bot_recips[r].pub_key[0] != hash[0]) continue;
      bool dup = false;
      for (int j = 0; j < _bot_match_n; j++)
        if (memcmp(_bot_match_pub[j], _bot_recips[r].pub_key, PUB_KEY_SIZE) == 0) { dup = true; break; }
      if (!dup) memcpy(_bot_match_pub[_bot_match_n++], _bot_recips[r].pub_key, PUB_KEY_SIZE);
    }
    return _bot_match_n;
  }

  ClientACL& acl = activeSlot().acl;
  int n = 0;
  for (int i = 0; i < acl.getNumClients() && n < MAX_CLIENTS; i++) {
    if (acl.getClientByIdx(i)->id.isHashMatch(hash)) {
      matching_peer_indexes[n++] = i;
    }
  }
  return n;
}

void RoomMesh::getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) {
  if (_active_is_bot) {
    if (peer_idx >= 0 && peer_idx < _bot_match_n)
      _bot_id.calcSharedSecret(dest_secret, _bot_match_pub[peer_idx]);
    return;
  }
  ClientACL& acl = activeSlot().acl;
  int i = matching_peer_indexes[peer_idx];
  if (i >= 0 && i < acl.getNumClients()) {
    memcpy(dest_secret, acl.getClientByIdx(i)->shared_secret, PUB_KEY_SIZE);
  }
}

/* ------------------------------------------------------------------ */
/*  onAnonDataRecv -- room-login                                        */
/* ------------------------------------------------------------------ */
void RoomMesh::onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret,
                              const mesh::Identity& sender, uint8_t* data, size_t len) {
  if (packet->getPayloadType() != PAYLOAD_TYPE_ANON_REQ) return;
  /* De bot kent geen room-login/ACL: een (afwijkende) ANON_REQ aan de bot mag NIET
   * in room 0 belanden. De bot praat alleen het TXT-diagnosepad (onPeerDataRecv). */
  if (_active_is_bot) return;
  if (len < 9) return;   // 4 ts + 4 sync_since + minstens de nul van het wachtwoord

  RoomSlot& slot = activeSlot();   // room OF virtuele sensor-node

  uint32_t sender_timestamp, sender_sync_since;
  memcpy(&sender_timestamp, data, 4);
  memcpy(&sender_sync_since, &data[4], 4);
  data[len] = 0;

  const int a_kind = activeIsSnode() ? ACL_KIND_SNODE : ACL_KIND_ROOM;
  const int a_slot = activeIsSnode() ? _active_snode : _active_slot;

  ClientInfo* client = NULL;

  /* PASSWORD-LESS: staat de afzender-pubkey als GRANT op dit slot? Dan honoreren we
   * dat niveau ZONDER wachtwoord (de ACL wint vóór de wachtwoordcheck). */
  uint8_t grant = grantLookup(a_kind, a_slot, sender.pub_key);
  if (grant >= PERM_ACL_READ_ONLY) {
    client = slot.acl.getClient(sender.pub_key, PUB_KEY_SIZE);
    bool is_new = (client == NULL);
    if (is_new) client = slot.acl.putClient(sender, 0);
    if (client == NULL) return;   // ACL vol
    if (!is_new && sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("slot[%d] possible replay attack (grant)", (uint32_t)a_slot);
      return;
    }
    client->last_timestamp = sender_timestamp;
    if (is_new) client->extra.room.sync_since = sender_sync_since;
    client->extra.room.pending_ack = 0;
    client->extra.room.push_failures = 0;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions = (uint8_t)((client->permissions & ~PERM_ACL_ROLE_MASK) | grant);
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);
  } else if (data[8] == 0) {   // leeg wachtwoord: alleen als de afzender al bekend is
    client = slot.acl.getClient(sender.pub_key, PUB_KEY_SIZE);
  }

  if (client == NULL) {
    uint8_t perm;
    if (strcmp((char*)&data[8], slot.password) == 0) {
      perm = PERM_ACL_ADMIN;
    } else if (slot.guest_password[0] && strcmp((char*)&data[8], slot.guest_password) == 0) {
      perm = PERM_ACL_READ_WRITE;
    } else if (_prefs.allow_read_only) {
      perm = PERM_ACL_GUEST;
    } else {
      MESH_DEBUG_PRINTLN("room[%d] incorrect password", (uint32_t)_active_slot);
      return;
    }

    client = slot.acl.putClient(sender, 0);
    if (sender_timestamp <= client->last_timestamp) {
      MESH_DEBUG_PRINTLN("room[%d] possible replay attack", (uint32_t)_active_slot);
      return;
    }

    client->last_timestamp = sender_timestamp;
    client->extra.room.sync_since = sender_sync_since;
    client->extra.room.pending_ack = 0;
    client->extra.room.push_failures = 0;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->permissions &= ~0x03;
    client->permissions |= perm;
    memcpy(client->shared_secret, secret, PUB_KEY_SIZE);

    /* Alleen room 0 bewaart zijn ACL naar flash; sensor-node-ACL's leven in RAM. */
    if (!activeIsSnode() && _active_slot == 0) slot.dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
  }

  if (packet->isRouteFlood()) client->out_path_len = OUT_PATH_UNKNOWN;

  uint32_t now = getRTCClock()->getCurrentTimeUnique();
  memcpy(reply_data, &now, 4);
  reply_data[4] = RESP_SERVER_LOGIN_OK;
  reply_data[5] = 0;
  reply_data[6] = (client->isAdmin() ? 1 : (client->permissions == 0 ? 2 : 0));
  reply_data[7] = client->permissions;
  getRNG()->random(&reply_data[8], 4);
  reply_data[12] = FIRMWARE_VER_LEVEL;

  slot.next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);

  if (packet->isRouteFlood()) {
    mesh::Packet* path = createPathReturn(sender, client->shared_secret, packet->path, packet->path_len,
                                          PAYLOAD_TYPE_RESPONSE, reply_data, 13);
    if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
  } else {
    mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, sender, client->shared_secret, reply_data, 13);
    if (reply) {
      if (client->out_path_len != OUT_PATH_UNKNOWN) {
        sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
      } else {
        sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/*  onPeerDataRecv -- posts, CLI, KEEP_ALIVE, telemetrie-REQ            */
/* ------------------------------------------------------------------ */
void RoomMesh::onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx,
                              const uint8_t* secret, uint8_t* data, size_t len) {
  /* BOT: het inkomende mesh-diagnose-pad (ping/path/help). Geen ACL, geen posts. */
  if (_active_is_bot) {
    if (type != PAYLOAD_TYPE_TXT_MSG || len <= 5) return;
    uint8_t flags = (data[4] >> 2);
    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) return;
    if (sender_idx < 0 || sender_idx >= _bot_match_n) return;
    data[len] = 0;
    handleBotDm(packet, _bot_match_pub[sender_idx], secret, data, len);
    return;
  }

  RoomSlot& slot = activeSlot();   // room OF virtuele sensor-node
  int i = matching_peer_indexes[sender_idx];
  if (i < 0 || i >= slot.acl.getNumClients()) return;
  ClientInfo* client = slot.acl.getClientByIdx(i);

  if (type == PAYLOAD_TYPE_TXT_MSG && len > 5) {
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4);
    uint8_t flags = (data[4] >> 2);
    if (!(flags == TXT_TYPE_PLAIN || flags == TXT_TYPE_CLI_DATA)) return;

    if (sender_timestamp >= client->last_timestamp) {
      bool is_retry = (sender_timestamp == client->last_timestamp);
      client->last_timestamp = sender_timestamp;
      uint32_t now = getRTCClock()->getCurrentTimeUnique();
      client->last_activity = now;
      client->extra.room.push_failures = 0;
      data[len] = 0;

      uint32_t ack_hash;
      mesh::Utils::sha256((uint8_t*)&ack_hash, 4, data, 5 + strlen((char*)&data[5]),
                          client->id.pub_key, PUB_KEY_SIZE);

      uint8_t temp[166];
      bool send_ack = false;

      if (flags == TXT_TYPE_CLI_DATA) {
        if (client->isAdmin()) {
          if (!is_retry) {
            handleCommand(sender_timestamp, (char*)&data[5], (char*)&temp[5]);
            temp[4] = (TXT_TYPE_CLI_DATA << 2);
          } else temp[5] = 0;
        } else temp[5] = 0;
      } else {   // TXT_TYPE_PLAIN = een room-post
        /* Een sensor-node kent GEEN posts (het is een telemetrie-node, geen room):
         * negeer de tekst stil. Alleen rooms bewaren en beantwoorden posts. */
        if (activeIsSnode() || (client->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
          temp[5] = 0;
        } else {
          if (!is_retry) {
            /* Eerst de post bewaren (zodat de room de vraag toont), daarna kijken
             * of het een commando is en zo ja het antwoord terug in de room posten. */
            addPost(slot, client, (const char*)&data[5]);
            char out[512];
            int rlen = roomCommandReply(client, _active_slot, (const char*)&data[5], out, sizeof(out));
            if (rlen > 0) addServerPost(_active_slot, out);
          }
          temp[5] = 0;
          send_ack = true;
        }
      }

      uint32_t delay_millis = 0;
      if (send_ack) {
        if (client->out_path_len == OUT_PATH_UNKNOWN) {
          mesh::Packet* ack = createAck(ack_hash);
          if (ack) sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());
          delay_millis = TXT_ACK_DELAY + REPLY_DELAY_MILLIS;
        } else {
          uint32_t d = TXT_ACK_DELAY;
          if (getExtraAckTransmitCount() > 0) {
            mesh::Packet* a1 = createMultiAck(ack_hash, 1);
            if (a1) sendDirect(a1, client->out_path, client->out_path_len, d);
            d += 300;
          }
          mesh::Packet* a2 = createAck(ack_hash);
          if (a2) sendDirect(a2, client->out_path, client->out_path_len, d);
          delay_millis = d + REPLY_DELAY_MILLIS;
        }
      }

      int text_len = strlen((char*)&temp[5]);
      if (text_len > 0) {
        if (now == sender_timestamp) now++;
        memcpy(temp, &now, 4);
        auto reply = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, secret, temp, 5 + text_len);
        if (reply) {
          if (client->out_path_len == OUT_PATH_UNKNOWN) {
            sendFloodReply(reply, delay_millis + SERVER_RESPONSE_DELAY, packet->getPathHashSize());
          } else {
            sendDirect(reply, client->out_path, client->out_path_len, delay_millis + SERVER_RESPONSE_DELAY);
          }
        }
      }
    }
  } else if (type == PAYLOAD_TYPE_REQ && len >= 5) {
    uint32_t sender_timestamp;
    memcpy(&sender_timestamp, data, 4);
    if (sender_timestamp < client->last_timestamp) return;
    client->last_timestamp = sender_timestamp;
    client->last_activity = getRTCClock()->getCurrentTime();
    client->extra.room.push_failures = 0;

    if (data[4] == REQ_TYPE_KEEP_ALIVE && packet->isRouteDirect()) {
      uint32_t forceSince = 0;
      if (len >= 9) memcpy(&forceSince, &data[5], 4);
      else memset(&data[5], 0, 4);
      if (forceSince > 0) client->extra.room.sync_since = forceSince;
      client->extra.room.pending_ack = 0;

      if (client->out_path_len != OUT_PATH_UNKNOWN) {
        uint32_t ack_hash;
        mesh::Utils::sha256((uint8_t*)&ack_hash, 4, data, 9, client->id.pub_key, PUB_KEY_SIZE);
        auto reply = createAck(ack_hash);
        if (reply) {
          /* Een sensor-node heeft geen posts -> altijd 0 ongesynchroniseerd (en
           * getUnsyncedCount rekent met &slot - rooms, wat alleen voor rooms geldt). */
          reply->payload[reply->payload_len++] = activeIsSnode() ? 0 : getUnsyncedCount(slot, client);
          sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
        }
      }
    } else {
      int reply_len = handleRequest(slot, client, sender_timestamp, &data[4], len - 4);
      if (reply_len > 0) {
        if (packet->isRouteFlood()) {
          mesh::Packet* path = createPathReturn(client->id, secret, packet->path, packet->path_len,
                                                PAYLOAD_TYPE_RESPONSE, reply_data, reply_len);
          if (path) sendFloodReply(path, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
        } else {
          mesh::Packet* reply = createDatagram(PAYLOAD_TYPE_RESPONSE, client->id, secret, reply_data, reply_len);
          if (reply) {
            if (client->out_path_len != OUT_PATH_UNKNOWN) {
              sendDirect(reply, client->out_path, client->out_path_len, SERVER_RESPONSE_DELAY);
            } else {
              sendFloodReply(reply, SERVER_RESPONSE_DELAY, packet->getPathHashSize());
            }
          }
        }
      }
    }
  }
}

bool RoomMesh::onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret,
                              uint8_t* path, uint8_t path_len, uint8_t extra_type,
                              uint8_t* extra, uint8_t extra_len) {
  /* BOT: geen ACL en geen padopslag (de bot antwoordt geflood). matching_peer_indexes
   * draagt in de bot-dispatch bovendien geen ACL-index -- niet aanraken. */
  if (_active_is_bot) return false;

  RoomSlot& slot = activeSlot();
  int i = matching_peer_indexes[sender_idx];
  if (i >= 0 && i < slot.acl.getNumClients()) {
    ClientInfo* client = slot.acl.getClientByIdx(i);
    client->out_path_len = mesh::Packet::copyPath(client->out_path, path, path_len);
    client->last_activity = getRTCClock()->getCurrentTime();
  }
  if (extra_type == PAYLOAD_TYPE_ACK && extra_len >= 4) processAckForSlot(slot, extra);
  return false;
}

void RoomMesh::onAckRecv(mesh::Packet* packet, uint32_t ack_crc) {
  // 1) post-sync-ACK's van clients (over alle rooms)
  for (int s = 0; s < MAX_ROOMS; s++) {
    if (!rooms[s].active) continue;
    if (processAckForSlot(rooms[s], (uint8_t*)&ack_crc)) {
      packet->markDoNotRetransmit();
      return;
    }
  }
  // 2) ACK's op DM-alarmen
  if (num_alert_tasks > 0) {
    AlertTask* t = &alert_tasks[0];
    for (int i = 0; i < t->attempt; i++) {
      if (ack_crc == t->expected_acks[i]) {
        t->attempt = 4;
        t->send_expiry = 0;
        packet->markDoNotRetransmit();
        return;
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Post-opslag + client-sync                                           */
/* ------------------------------------------------------------------ */
void RoomMesh::storePost(uint8_t room_idx, const mesh::Identity& author, const char* text) {
  if (!text || text[0] == 0) return;
  int quota = MAX_TOTAL_POSTS / (_num_active_rooms > 0 ? _num_active_rooms : 1);

  PostInfo* free_slot = NULL;
  PostInfo* oldest_for_room = NULL;
  int room_count = 0;
  for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
    PostInfo& p = _post_pool[i];
    if (p.room_idx == 0xFF) {
      if (!free_slot) free_slot = &p;
    } else if (p.room_idx == room_idx) {
      room_count++;
      if (!oldest_for_room || p.post_timestamp < oldest_for_room->post_timestamp) oldest_for_room = &p;
    }
  }
  if (room_count >= quota && oldest_for_room) {
    memset(oldest_for_room, 0, sizeof(PostInfo));
    oldest_for_room->room_idx = 0xFF;
    if (!free_slot) free_slot = oldest_for_room;
  }
  if (!free_slot) {
    PostInfo* oldest_global = NULL;
    for (int i = 0; i < MAX_TOTAL_POSTS; i++) {
      if (!oldest_global || _post_pool[i].post_timestamp < oldest_global->post_timestamp) oldest_global = &_post_pool[i];
    }
    memset(oldest_global, 0, sizeof(PostInfo));
    oldest_global->room_idx = 0xFF;
    free_slot = oldest_global;
  }

  free_slot->author = author;
  StrHelper::strncpy(free_slot->text, text, MAX_POST_TEXT_LEN);
  free_slot->post_timestamp = getRTCClock()->getCurrentTimeUnique();
  free_slot->room_idx = room_idx;

  if (room_idx < MAX_ROOMS) {
    rooms[room_idx].next_push = futureMillis(PUSH_NOTIFY_DELAY_MILLIS);
    rooms[room_idx].num_posted++;
  }

  if (_post_cb) _post_cb((int)room_idx, free_slot->post_timestamp, free_slot->author.pub_key, free_slot->text, _post_cb_ctx);
}

void RoomMesh::addPost(RoomSlot& slot, ClientInfo* client, const char* text) {
  storePost((uint8_t)(&slot - rooms), client->id, text);
}

void RoomMesh::addServerPost(int room_idx, const char* text) {
  if (room_idx < 0 || room_idx >= MAX_ROOMS || !rooms[room_idx].active) return;
  if (!text || text[0] == 0) return;

  /* Lange tekst over meerdere posts knippen (op regel-/woordgrens waar mogelijk),
   * elk stuk <= MAX_POST_TEXT_LEN. */
  const char* p = text;
  int remaining = strlen(text);
  int guard = 0;
  while (remaining > 0 && guard++ < 8) {
    int take = remaining;
    if (take > MAX_POST_TEXT_LEN) {
      take = MAX_POST_TEXT_LEN;
      int w = take;
      while (w > 0 && p[w] != '\n' && p[w] != ' ') w--;
      if (w > 0) take = w;
    }
    char chunk[MAX_POST_TEXT_LEN + 1];
    memcpy(chunk, p, take);
    chunk[take] = 0;
    storePost((uint8_t)room_idx, rooms[room_idx].id, chunk);
    p += take;
    remaining -= take;
    while (*p == ' ' || *p == '\n') { p++; remaining--; }
  }
}

void RoomMesh::pushPostToClient(RoomSlot& slot, ClientInfo* client, PostInfo& post) {
  int len = 0;
  memcpy(&reply_data[len], &post.post_timestamp, 4); len += 4;
  uint8_t attempt;
  getRNG()->random(&attempt, 1);
  reply_data[len++] = (TXT_TYPE_SIGNED_PLAIN << 2) | (attempt & 3);
  memcpy(&reply_data[len], post.author.pub_key, 4); len += 4;
  int text_len = strlen(post.text);
  memcpy(&reply_data[len], post.text, text_len); len += text_len;

  mesh::Utils::sha256((uint8_t*)&client->extra.room.pending_ack, 4, reply_data, len, client->id.pub_key, PUB_KEY_SIZE);
  client->extra.room.push_post_timestamp = post.post_timestamp;

  self_id = slot.id;   // teken met de sleutel van deze room
  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, client->id, client->shared_secret, reply_data, len);
  if (pkt) {
    if (client->out_path_len == OUT_PATH_UNKNOWN) {
      sendFloodScoped(default_scope, pkt, 0, _prefs.path_hash_mode + 1);
      client->extra.room.ack_timeout = futureMillis(PUSH_ACK_TIMEOUT_FLOOD);
    } else {
      sendDirect(pkt, client->out_path, client->out_path_len);
      uint8_t hops = client->out_path_len & 63;
      client->extra.room.ack_timeout = futureMillis(PUSH_TIMEOUT_BASE + PUSH_ACK_TIMEOUT_FACTOR * (hops + 1));
    }
    slot.num_post_pushes++;
  } else {
    client->extra.room.pending_ack = 0;
  }
}

uint8_t RoomMesh::getUnsyncedCount(RoomSlot& slot, ClientInfo* client) {
  uint8_t ridx = (uint8_t)(&slot - rooms);
  uint8_t count = 0;
  for (int k = 0; k < MAX_TOTAL_POSTS; k++) {
    const PostInfo& p = _post_pool[k];
    if (p.room_idx == ridx && p.post_timestamp > client->extra.room.sync_since &&
        !p.author.matches(client->id)) count++;
  }
  return count;
}

bool RoomMesh::processAckForSlot(RoomSlot& slot, const uint8_t* data) {
  for (int i = 0; i < slot.acl.getNumClients(); i++) {
    auto c = slot.acl.getClientByIdx(i);
    if (c->extra.room.pending_ack && memcmp(data, &c->extra.room.pending_ack, 4) == 0) {
      c->extra.room.pending_ack = 0;
      c->extra.room.push_failures = 0;
      c->extra.room.sync_since = c->extra.room.push_post_timestamp;
      return true;
    }
  }
  return false;
}

/* ------------------------------------------------------------------ */
/*  Adverts (per room)                                                  */
/* ------------------------------------------------------------------ */
mesh::Packet* RoomMesh::createRoomAdvert(RoomSlot& slot) {
  /* HET STANDAARD MESHCORE-ADVERTFORMAAT, en niet met de hand.
   *
   * Hier stond een handgemaakte opbouw: [ADV_TYPE_ROOM][name_len][naam...]. Die
   * extra name_len-byte hoort NIET in het formaat -- AdvertDataBuilder zet een
   * vlaggenbyte (type | ADV_NAME_MASK | evt. ADV_LATLON_MASK) en dan de naam als
   * de RESTERENDE bytes, zonder lengteprefix. Door die extra byte lazen de
   * MeshCore-app en MeshManager onze roomnaam verkeerd en toonden zij "(unnamed)".
   *
   * We gebruiken bewust NIET _cli.buildAdvertData(): die pakt _prefs.node_name,
   * terwijl elke room hier zijn EIGEN naam heeft. Vandaar AdvertDataBuilder met
   * slot.name (naam-only, net als simple_room_server). De inkomende parser
   * (onAdvertRecv -> AdvertDataParser) verwachtte dit standaardformaat al; alleen
   * onze UITGAANDE adverts waren fout. */
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder builder(ADV_TYPE_ROOM, slot.name);
  uint8_t app_data_len = builder.encodeTo(app_data);

  self_id = slot.id;
  return createAdvert(slot.id, app_data, app_data_len);
}

void RoomMesh::sendRoomAdvertisement(RoomSlot& slot, int delay_millis, bool flood) {
  mesh::Packet* pkt = createRoomAdvert(slot);
  if (!pkt) return;
  if (flood) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
  else sendZeroHop(pkt, delay_millis);
}

/* Sensor-node-advert: identiek aan de room-advert maar met ADV_TYPE_SENSOR, zodat
 * de MeshCore-app het contact als SENSOR ziet en telemetrie toont. Ook hier het
 * standaardformaat via AdvertDataBuilder (nooit met de hand). */
mesh::Packet* RoomMesh::createSensorNodeAdvert(RoomSlot& slot) {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder builder(ADV_TYPE_SENSOR, slot.name);
  uint8_t app_data_len = builder.encodeTo(app_data);

  self_id = slot.id;
  return createAdvert(slot.id, app_data, app_data_len);
}

void RoomMesh::sendSensorNodeAdvertisement(RoomSlot& slot, int delay_millis, bool flood) {
  mesh::Packet* pkt = createSensorNodeAdvert(slot);
  if (!pkt) return;
  if (flood) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
  else sendZeroHop(pkt, delay_millis);
}

void RoomMesh::sendSelfAdvertisement(int delay_millis, bool flood) {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active || rooms[i].stealth) continue;
    sendRoomAdvertisement(rooms[i], delay_millis + (uint32_t)i * 1000, flood);
  }
  /* De virtuele sensor-nodes erachteraan (verschoven in tijd zodat ze niet samen
   * de ether op gaan). */
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    if (!snodes[i].active || snodes[i].stealth) continue;
    sendSensorNodeAdvertisement(snodes[i], delay_millis + (uint32_t)(MAX_ROOMS + i) * 1000, flood);
  }
  if (_bot_active)
    sendBotAdvertisement(delay_millis + (uint32_t)(MAX_ROOMS + MAX_SENSOR_NODES) * 1000, flood);
}

void RoomMesh::updateAdvertTimer() {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active || rooms[i].stealth || _prefs.advert_interval == 0) {
      rooms[i].next_local_advert = 0;
    } else {
      rooms[i].next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000 + (uint32_t)i * 15000);
    }
  }
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    if (!snodes[i].active || snodes[i].stealth || _prefs.advert_interval == 0) {
      snodes[i].next_local_advert = 0;
    } else {
      snodes[i].next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000 + (uint32_t)(MAX_ROOMS + i) * 15000);
    }
  }
  /* Bot erachteraan (verschoven zodat hij niet met de rooms/snodes samenvalt). */
  if (!_bot_active || _prefs.advert_interval == 0) _bot_next_local_advert = 0;
  else _bot_next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000 + (uint32_t)(MAX_ROOMS + MAX_SENSOR_NODES) * 15000);
}

void RoomMesh::updateFloodAdvertTimer() {
  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active || rooms[i].stealth || _prefs.flood_advert_interval == 0) {
      rooms[i].next_flood_advert = 0;
    } else {
      rooms[i].next_flood_advert = futureMillis((uint32_t)_prefs.flood_advert_interval * 60 * 60 * 1000 + (uint32_t)i * 15000);
    }
  }
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    if (!snodes[i].active || snodes[i].stealth || _prefs.flood_advert_interval == 0) {
      snodes[i].next_flood_advert = 0;
    } else {
      snodes[i].next_flood_advert = futureMillis((uint32_t)_prefs.flood_advert_interval * 60 * 60 * 1000 + (uint32_t)(MAX_ROOMS + i) * 15000);
    }
  }
  if (!_bot_active || _prefs.flood_advert_interval == 0) _bot_next_flood_advert = 0;
  else _bot_next_flood_advert = futureMillis((uint32_t)_prefs.flood_advert_interval * 60 * 60 * 1000 + (uint32_t)(MAX_ROOMS + MAX_SENSOR_NODES) * 15000);
}

/* ------------------------------------------------------------------ */
/*  handleRequest -- status + telemetrie + toegangslijst                */
/* ------------------------------------------------------------------ */
int RoomMesh::handleRequest(RoomSlot& slot, ClientInfo* sender, uint32_t sender_timestamp,
                            uint8_t* payload, size_t payload_len) {
  memcpy(reply_data, &sender_timestamp, 4);

  if (payload[0] == REQ_TYPE_GET_STATUS) {
    ServerStats stats;
    stats.batt_milli_volts = board.getBattMilliVolts();
    stats.curr_tx_queue_len = _mgr->getOutboundTotal();
    stats.noise_floor = (int16_t)_radio->getNoiseFloor();
    stats.last_rssi = (int16_t)radio_driver.getLastRSSI();
    stats.n_packets_recv = radio_driver.getPacketsRecv();
    stats.n_packets_sent = radio_driver.getPacketsSent();
    stats.total_air_time_secs = getTotalAirTime() / 1000;
    stats.total_up_time_secs = uptime_millis / 1000;
    stats.n_sent_flood = getNumSentFlood();
    stats.n_sent_direct = getNumSentDirect();
    stats.n_recv_flood = getNumRecvFlood();
    stats.n_recv_direct = getNumRecvDirect();
    stats.err_events = _err_flags;
    stats.last_snr = (int16_t)(radio_driver.getLastSNR() * 4);
    stats.n_direct_dups = ((SimpleMeshTables*)getTables())->getNumDirectDups();
    stats.n_flood_dups = ((SimpleMeshTables*)getTables())->getNumFloodDups();
    stats.n_posted = slot.num_posted;
    stats.n_post_push = slot.num_post_pushes;
    memcpy(&reply_data[4], &stats, sizeof(stats));
    return 4 + sizeof(stats);
  }
  if (payload[0] == REQ_TYPE_GET_TELEMETRY_DATA) {
    uint8_t perm_mask = ~(payload[1]);
    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
    if ((sender->permissions & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) perm_mask = 0x00;
    /* Voor een virtuele sensor-node: alleen de kanalen die aan DIE node gekoppeld
     * zijn (subset), zodat sensoren over meerdere nodes verdeeld kunnen worden.
     * Voor een room: de volledige set (ongewijzigd). */
    if (activeIsSnode()) sensors.querySensorsForNode(_active_snode, perm_mask, telemetry);
    else                 sensors.querySensors(perm_mask, telemetry);
    float temperature = board.getMCUTemperature();
    if (!isnan(temperature)) telemetry.addTemperature(TELEM_CHANNEL_SELF, temperature);
    uint8_t tlen = telemetry.getSize();
    memcpy(&reply_data[4], telemetry.getBuffer(), tlen);
    return 4 + tlen;
  }
  if (payload[0] == REQ_TYPE_GET_ACCESS_LIST && sender->isAdmin()) {
    if (payload[1] == 0 && payload[2] == 0) {
      uint8_t ofs = 4;
      for (int i = 0; i < slot.acl.getNumClients() && ofs + 7 <= (int)sizeof(reply_data) - 4; i++) {
        auto c = slot.acl.getClientByIdx(i);
        if (!c->isAdmin()) continue;
        memcpy(&reply_data[ofs], c->id.pub_key, 6); ofs += 6;
        reply_data[ofs++] = c->permissions;
      }
      return ofs;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/*  Per-room loop (push) + main loop                                    */
/* ------------------------------------------------------------------ */
void RoomMesh::loopSlot(RoomSlot& slot) {
  if (!millisHasNowPassed(slot.next_push)) return;
  if (slot.acl.getNumClients() == 0) { slot.next_push = futureMillis(SYNC_PUSH_INTERVAL); return; }

  for (int i = 0; i < slot.acl.getNumClients(); i++) {
    auto c = slot.acl.getClientByIdx(i);
    if (c->extra.room.pending_ack && millisHasNowPassed(c->extra.room.ack_timeout)) {
      c->extra.room.push_failures++;
      c->extra.room.pending_ack = 0;
    }
  }

  auto client = slot.acl.getClientByIdx(slot.next_client_idx);
  bool did_push = false;
  if (client->extra.room.pending_ack == 0 && client->last_activity != 0 &&
      client->extra.room.push_failures < 3) {
    uint8_t ridx = (uint8_t)(&slot - rooms);
    uint32_t now = getRTCClock()->getCurrentTime();
    PostInfo* oldest = NULL;
    for (int k = 0; k < MAX_TOTAL_POSTS; k++) {
      PostInfo& p = _post_pool[k];
      if (p.room_idx == ridx && now >= p.post_timestamp + POST_SYNC_DELAY_SECS &&
          p.post_timestamp > client->extra.room.sync_since && !p.author.matches(client->id)) {
        if (!oldest || p.post_timestamp < oldest->post_timestamp) oldest = &p;
      }
    }
    if (oldest) { pushPostToClient(slot, client, *oldest); did_push = true; }
  }

  slot.next_client_idx = (slot.next_client_idx + 1) % slot.acl.getNumClients();
  slot.next_push = futureMillis(did_push ? SYNC_PUSH_INTERVAL : SYNC_PUSH_INTERVAL / 8);
}

void RoomMesh::loop() {
  mesh::Mesh::loop();

  for (int i = 0; i < MAX_ROOMS; i++) {
    if (!rooms[i].active) continue;
    RoomSlot& slot = rooms[i];
    loopSlot(slot);

    if (!slot.stealth) {
      if (slot.next_flood_advert && millisHasNowPassed(slot.next_flood_advert)) {
        sendRoomAdvertisement(slot, 0, true);
        slot.next_flood_advert = futureMillis(FLOOD_ADVERT_INTERVAL_MS + (uint32_t)i * 15000);
        slot.next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000 + (uint32_t)i * 15000);
      } else if (slot.next_local_advert && millisHasNowPassed(slot.next_local_advert)) {
        sendRoomAdvertisement(slot, 0, false);
        slot.next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000 + (uint32_t)i * 15000);
      }
    }

    if (slot.dirty_contacts_expiry && millisHasNowPassed(slot.dirty_contacts_expiry)) {
      if (i == 0) rooms[0].acl.save(_fs, RoomMesh::saveFilter);
      slot.dirty_contacts_expiry = 0;
    }
  }

  /* Sensor-node-adverts. GEEN loopSlot: een sensor-node kent geen posts, dus er is
   * niets te pushen; alleen het advert houdt de app-zichtbaarheid in stand. */
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    if (!snodes[i].active || snodes[i].stealth) continue;
    RoomSlot& slot = snodes[i];
    if (slot.next_flood_advert && millisHasNowPassed(slot.next_flood_advert)) {
      sendSensorNodeAdvertisement(slot, 0, true);
      slot.next_flood_advert = futureMillis(FLOOD_ADVERT_INTERVAL_MS + (uint32_t)(MAX_ROOMS + i) * 15000);
      slot.next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000 + (uint32_t)(MAX_ROOMS + i) * 15000);
    } else if (slot.next_local_advert && millisHasNowPassed(slot.next_local_advert)) {
      sendSensorNodeAdvertisement(slot, 0, false);
      slot.next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000 + (uint32_t)(MAX_ROOMS + i) * 15000);
    }
  }

  /* Bot-advert (CHAT). Zelfde timer-discipline; verschoven na de snodes. */
  if (_bot_active) {
    const uint32_t bstag = (uint32_t)(MAX_ROOMS + MAX_SENSOR_NODES) * 15000;
    if (_bot_next_flood_advert && millisHasNowPassed(_bot_next_flood_advert)) {
      sendBotAdvertisement(0, true);
      _bot_next_flood_advert = futureMillis(FLOOD_ADVERT_INTERVAL_MS + bstag);
      _bot_next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000 + bstag);
    } else if (_bot_next_local_advert && millisHasNowPassed(_bot_next_local_advert)) {
      sendBotAdvertisement(0, false);
      _bot_next_local_advert = futureMillis((uint32_t)_prefs.advert_interval * 2 * 60 * 1000 + bstag);
    }
  }

  if (set_radio_at && millisHasNowPassed(set_radio_at)) {
    set_radio_at = 0;
    radio_driver.setParams(pending_freq, pending_bw, pending_sf, pending_cr);
  }
  if (revert_radio_at && millisHasNowPassed(revert_radio_at)) {
    revert_radio_at = 0;
    radio_driver.setParams(_prefs.freq, _prefs.bw, _prefs.sf, _prefs.cr);
  }

  // periodieke sensorleesronde -> app-hook (posts + alarmen)
  // v2.3.6: cadans LIVE uit _cfg (sensors.readIntervalSecs()) i.p.v. de compile-time
  // -D SENSOR_READ_INTERVAL_SECS; die blijft enkel de default-seed (zie MonitorStore.h).
  uint32_t curr = getRTCClock()->getCurrentTime();
  if (curr >= last_read_time + sensors.readIntervalSecs()) {
    telemetry.reset();
    telemetry.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
    sensors.querySensors(0xFF, telemetry);
    onSensorDataRead();
    last_read_time = curr;
  }

  // DM-alarmwachtrij (kop van de rij)
  if (num_alert_tasks > 0) {
    AlertTask* t = &alert_tasks[0];
    if (millisHasNowPassed(t->send_expiry)) {
      /* from_bot: itereer de bot-ontvangerslijst (schone DM vanaf de bot). Anders:
       * de klassieke weg over room-0-contacten met alarm-recht. Beide delen de
       * attempt/ACK-boekhouding en het onAckRecv-pad. */
      int contact_count = t->from_bot ? botRecipCount() : rooms[0].acl.getNumClients();
      if (t->attempt >= 4) {
        t->curr_contact_idx++;
        if (t->curr_contact_idx >= contact_count) {
          t->text[0] = 0;
          num_alert_tasks--;
          for (int i = 0; i < num_alert_tasks; i++) alert_tasks[i] = alert_tasks[i + 1];
        } else if (t->from_bot) {
          uint8_t pub[PUB_KEY_SIZE];
          if (botRecipGetByIdx(t->curr_contact_idx, pub)) {
            t->attempt = t->high_pri ? 0 : 3;
            t->timestamp = getRTCClock()->getCurrentTimeUnique();
            sendBotAlertDM(pub, t);
          }
        } else {
          auto c = rooms[0].acl.getClientByIdx(t->curr_contact_idx);
          uint16_t pri_mask = t->high_pri ? PERM_RECV_ALERTS_HI : PERM_RECV_ALERTS_LO;
          if (c->permissions & pri_mask) {
            t->attempt = t->high_pri ? 0 : 3;
            t->timestamp = getRTCClock()->getCurrentTimeUnique();
            sendAlertDM(c, t);
          }
        }
      } else if (t->curr_contact_idx < contact_count) {
        if (t->from_bot) {
          uint8_t pub[PUB_KEY_SIZE];
          if (botRecipGetByIdx(t->curr_contact_idx, pub)) sendBotAlertDM(pub, t);
          else t->attempt = 4;
        } else {
          auto c = rooms[0].acl.getClientByIdx(t->curr_contact_idx);
          sendAlertDM(c, t);
        }
      } else {
        t->attempt = 4;
      }
    }
  }

  uint32_t now = millis();
  uptime_millis += now - last_millis;
  last_millis = now;
}

/* ------------------------------------------------------------------ */
/*  DM-alarmpad                                                         */
/* ------------------------------------------------------------------ */
void RoomMesh::sendAlertDM(const ClientInfo* c, AlertTask* t) {
  int text_len = strlen(t->text);
  uint8_t data[MAX_PACKET_PAYLOAD];
  memcpy(data, &t->timestamp, 4);
  data[4] = (TXT_TYPE_PLAIN << 2) | t->attempt;
  memcpy(&data[5], t->text, text_len);

  self_id = rooms[0].id;   // DM's komen van de hoofdidentiteit
  mesh::Utils::sha256((uint8_t*)&t->expected_acks[t->attempt], 4, data, 5 + text_len, rooms[0].id.pub_key, PUB_KEY_SIZE);
  t->attempt++;

  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, c->id, c->shared_secret, data, 5 + text_len);
  if (pkt) {
    if (c->out_path_len != OUT_PATH_UNKNOWN) sendDirect(pkt, c->out_path, c->out_path_len);
    else sendFloodScoped(default_scope, pkt, 0, _prefs.path_hash_mode + 1);
  }
  t->send_expiry = futureMillis(ALERT_ACK_EXPIRY_MILLIS);
}

void RoomMesh::dispatchAlert(uint8_t mode, uint16_t room_mask, bool high_pri, const char* text) {
  if (!text || text[0] == 0) return;

  // room-deel: naar elke toegewezen, actieve room
  if (mode & ALERT_MODE_ROOM) {
    for (int i = 0; i < MAX_ROOMS; i++) {
      if ((room_mask & (1 << i)) && rooms[i].active) addServerPost(i, text);
    }
  }

  // DM-deel: SCHONE DM vanaf de bot naar ELKE ontvanger in de bot-lijst
  // (herhaal-tot-ACK per ontvanger). Vervangt de oude room-identiteit-DM zodat
  // dm/both nu als een gewoon chatcontact in de app tonen.
  if ((mode & ALERT_MODE_DM) && _bot_active && botRecipCount() > 0 &&
      num_alert_tasks < ROOM_MAX_ALERTS) {
    AlertTask* t = &alert_tasks[num_alert_tasks];
    StrHelper::strncpy(t->text, text, sizeof(t->text));
    t->high_pri = high_pri;
    t->from_bot = true;
    t->send_expiry = 0;
    t->attempt = 4;
    t->curr_contact_idx = -1;
    num_alert_tasks++;
  }
}

/* ================================================================== */
/*  BOT: CHAT/notifier-identiteit + DM-ontvangerslijst                 */
/* ================================================================== */

void RoomMesh::loadOrCreateBotIdentity() {
  if (_fs == NULL) return;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#else
  IdentityStore store(*_fs, "/identity");
#endif
  if (!store.load(BOT_ID_NAME, _bot_id)) {
    _bot_id = radio_new_identity();
    int count = 0;
    while (count < 10 && (_bot_id.pub_key[0] == 0x00 || _bot_id.pub_key[0] == 0xFF)) {
      _bot_id = radio_new_identity(); count++;
    }
    store.save(BOT_ID_NAME, _bot_id);
  }
}

int RoomMesh::botRecipFindFree() const {
  for (int i = 0; i < MAX_BOT_RECIPS; i++) if (_bot_recips[i].level == 0) return i;
  return -1;
}

int RoomMesh::botRecipCount() const {
  int n = 0;
  for (int i = 0; i < MAX_BOT_RECIPS; i++) if (_bot_recips[i].level != 0) n++;
  return n;
}

bool RoomMesh::botRecipGetByIdx(int idx, uint8_t* pub_out) const {
  int n = 0;
  for (int i = 0; i < MAX_BOT_RECIPS; i++) {
    if (_bot_recips[i].level == 0) continue;
    if (n == idx) { memcpy(pub_out, _bot_recips[i].pub_key, PUB_KEY_SIZE); return true; }
    n++;
  }
  return false;
}

/* Toevoegen: VOLLEDIGE pubkey. 0 ok, -2 al aanwezig (geen fout), -3 vol. */
int RoomMesh::botRecipAdd(const uint8_t* pubkey) {
  for (int i = 0; i < MAX_BOT_RECIPS; i++) {
    if (_bot_recips[i].level != 0 && memcmp(_bot_recips[i].pub_key, pubkey, PUB_KEY_SIZE) == 0)
      return -2;   // al in de lijst
  }
  int f = botRecipFindFree();
  if (f < 0) return -3;
  memcpy(_bot_recips[f].pub_key, pubkey, PUB_KEY_SIZE);
  _bot_recips[f].level = 1;
  return 0;
}

/* Verwijderen op prefix (>= 6 byte). 1 ok, -2 niet gevonden, -3 dubbelzinnig. */
int RoomMesh::botRecipDelPrefix(const uint8_t* prefix, int key_len) {
  if (key_len < 6 || key_len > PUB_KEY_SIZE) return -2;
  int hit = -1, hits = 0;
  for (int i = 0; i < MAX_BOT_RECIPS; i++) {
    if (_bot_recips[i].level == 0) continue;
    if (memcmp(_bot_recips[i].pub_key, prefix, key_len) == 0) { hit = i; hits++; }
  }
  if (hits == 0) return -2;
  if (hits > 1) return -3;
  _bot_recips[hit].level = 0;
  memset(_bot_recips[hit].pub_key, 0, PUB_KEY_SIZE);
  return 1;
}

/* Ontvangerslijst als tekst: één regel per ontvanger  ->  b <level> <pubkeyhex> */
void RoomMesh::saveBotRecips() {
  if (_fs == NULL) return;
  File f = _fs->open(BOT_RECIPS_PATH, "w", true);
  if (!f) return;
  f.printf("#MUBOT1\n");
  /* Verklikker-stand als extra regeltype 'd'; oude parsers slaan 'm gewoon over. */
  f.printf("d %d\n", _bot_diag ? 1 : 0);
  char hex[PUB_KEY_SIZE * 2 + 1];
  for (int i = 0; i < MAX_BOT_RECIPS; i++) {
    if (_bot_recips[i].level == 0) continue;
    mesh::Utils::toHex(hex, _bot_recips[i].pub_key, PUB_KEY_SIZE);
    f.printf("b %d %s\n", (int)_bot_recips[i].level, hex);
  }
  f.printf(".\n");
  f.close();
}

void RoomMesh::loadBotRecips() {
  if (_fs == NULL || !_fs->exists(BOT_RECIPS_PATH)) return;
  File f = _fs->open(BOT_RECIPS_PATH, "r");
  if (!f) return;
  char line[96];
  bool first = true;
  int bi = 0;
  memset(_bot_recips, 0, sizeof(_bot_recips));
  while (f.available() && bi < MAX_BOT_RECIPS) {
    size_t len = 0;
    while (f.available() && len < sizeof(line) - 1) {
      int c = f.read();
      if (c < 0 || c == '\n') break;
      if (c == '\r') continue;
      line[len++] = (char)c;
    }
    line[len] = 0;
    if (first) { first = false; continue; }
    if (line[0] == 'd' && line[1] == ' ') { _bot_diag = atoi(line + 2) != 0; continue; }
    if (line[0] != 'b') continue;
    char* p = line + 1;
    while (*p == ' ') p++;
    int level = atoi(p);  while (*p && *p != ' ') p++; while (*p == ' ') p++;
    char* hex = p;
    if (level <= 0) continue;
    if (strlen(hex) < PUB_KEY_SIZE * 2) continue;
    uint8_t pubkey[PUB_KEY_SIZE];
    if (!mesh::Utils::fromHex(pubkey, PUB_KEY_SIZE, hex)) continue;
    memcpy(_bot_recips[bi].pub_key, pubkey, PUB_KEY_SIZE);
    _bot_recips[bi].level = (uint8_t)level;
    bi++;
  }
  f.close();
}

void RoomMesh::setBotDiag(bool en) {
  _bot_diag = en;
  saveBotRecips();
}

/* Advert als CHAT-contact (type=1), zodat de MeshCore-app de bot als gewoon
 * chatcontact toont. Standaard MeshCore-formaat via AdvertDataBuilder. */
mesh::Packet* RoomMesh::createBotAdvert() {
  uint8_t app_data[MAX_ADVERT_DATA_SIZE];
  AdvertDataBuilder builder(ADV_TYPE_CHAT, _bot_name);
  uint8_t app_data_len = builder.encodeTo(app_data);
  self_id = _bot_id;
  return createAdvert(_bot_id, app_data, app_data_len);
}

void RoomMesh::sendBotAdvertisement(int delay_millis, bool flood) {
  mesh::Packet* pkt = createBotAdvert();
  if (!pkt) return;
  if (flood) sendFloodScoped(default_scope, pkt, delay_millis, _prefs.path_hash_mode + 1);
  else sendZeroHop(pkt, delay_millis);
  self_id = rooms[0].id;   // standaard-identiteit herstellen
}

/* Schone DM vanaf de bot naar een losse pubkey (niet in een ACL). Berekent het
 * gedeelde geheim zelf uit de volledige pubkey; verder identiek aan sendAlertDM
 * zodat ACK-herhaling en onAckRecv gedeeld blijven. */
void RoomMesh::sendBotAlertDM(const uint8_t* pubkey, AlertTask* t) {
  int text_len = strlen(t->text);
  uint8_t data[MAX_PACKET_PAYLOAD];
  memcpy(data, &t->timestamp, 4);
  data[4] = (TXT_TYPE_PLAIN << 2) | t->attempt;
  memcpy(&data[5], t->text, text_len);

  uint8_t secret[PUB_KEY_SIZE];
  _bot_id.calcSharedSecret(secret, pubkey);
  mesh::Identity dest(pubkey);

  self_id = _bot_id;   // de DM komt van de bot
  mesh::Utils::sha256((uint8_t*)&t->expected_acks[t->attempt], 4, data, 5 + text_len, _bot_id.pub_key, PUB_KEY_SIZE);
  t->attempt++;

  auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, dest, secret, data, 5 + text_len);
  if (pkt) sendFloodScoped(default_scope, pkt, 0, _prefs.path_hash_mode + 1);
  t->send_expiry = futureMillis(ALERT_ACK_EXPIRY_MILLIS);
  self_id = rooms[0].id;
}

/* ------------------------------------------------------------------ */
/*  Bot: inkomend mesh-diagnose-pad (ping / path / help)               */
/* ------------------------------------------------------------------ */

/* Zend-diagnose-achtervoegsel voor bot-antwoorden: duim omhoog voor moderne
 * afzenders (2-byte pad-hashes, gescopete flood), droevige smiley voor legacy
 * (1-byte hashes -- worden door o.a. DinX-Home gefilterd -- of een flood zonder
 * scope: ROUTE_TYPE_FLOOD i.p.v. TRANSPORT_FLOOD met transport-codes). Alleen
 * wat uit het pakket ECHT afleidbaar is:
 *  - de hash-grootte zit in de topbits van path_len en wordt ook bij een
 *    zero-hop FLOOD gezet (setPathHashSizeAndCount), maar een zero-hop DIRECT
 *    pakket draagt hem niet -> dan geen oordeel;
 *  - het scope-oordeel geldt alleen voor floods; een DIRECT pakket floodt niet
 *    en heeft dus geen scope nodig. */
static void appendTxDiag(char* reply, size_t cap, const mesh::Packet* packet) {
  size_t o = strlen(reply);
  bool size_known = packet->isRouteFlood() || packet->getPathHashCount() > 0;
  if (size_known && o < cap) {
    if (packet->getPathHashSize() >= 2)
      o += snprintf(reply + o, cap - o, " | %d-byte \xF0\x9F\x91\x8D", (int)packet->getPathHashSize());
    else
      o += snprintf(reply + o, cap - o, " | 1-byte \xF0\x9F\x98\x9E");
  }
  if (packet->isRouteFlood() && o < cap) {
    if (packet->hasTransportCodes())
      snprintf(reply + o, cap - o, " | scoped \xF0\x9F\x91\x8D");
    else
      snprintf(reply + o, cap - o, " | geen scope \xF0\x9F\x98\x9E");
  }
}

/* De bot is GEEN monitoring-console: hij is een mesh-diagnose-responder met een
 * klein eigen setje. self_id is hier al _bot_id (in onRecvPacket gezet), en het
 * gedeelde geheim is al berekend, dus we antwoorden rechtstreeks als schone DM
 * vanaf de bot. De buffers zijn STATIC (niet op de loopTask-stapel), en er is
 * hoogstens één inkomend bot-pakket tegelijk (coöperatief vanuit de mesh-lus). */
void RoomMesh::handleBotDm(mesh::Packet* packet, const uint8_t* sender_pub,
                           const uint8_t* secret, uint8_t* data, size_t len) {
  (void)len;
  const char* text = (const char*)&data[5];
  while (*text == ' ') text++;
  char verb[12]; int vi = 0;
  while (text[vi] && text[vi] != ' ' && vi < (int)sizeof(verb) - 1) { verb[vi] = text[vi]; vi++; }
  verb[vi] = 0;

  /* De lokale kloktijd op ontvangstmoment (RTC = UTC -> lokaal via de TZ), met de
   * zone-afkorting (CET/CEST). Onder TIME_FLOOR: "niet gesynct". */
  uint32_t now_s = getRTCClock()->getCurrentTime();
  char rxt[40]; fmtLocalHMS(now_s, rxt, sizeof(rxt));

  static char reply[200];
  reply[0] = 0;

  if (!strcasecmp(verb, "ping")) {
    snprintf(reply, sizeof(reply), "Pong (%s)", rxt);
  } else if (!strcasecmp(verb, "path")) {
    /* Afzendernaam uit de buurtlijst (adverts); val terug op de pubkey-prefix. */
    char name[24];
    const NeighbourEntry* ne = neighbours.find(sender_pub, PUB_KEY_SIZE);
    if (ne && ne->name[0]) {
      StrHelper::strncpy(name, ne->name, sizeof(name));
    } else {
      char hx[13]; mesh::Utils::toHex(hx, sender_pub, 6); hx[12] = 0;
      snprintf(name, sizeof(name), "%s", hx);
    }
    /* TUSSENLIGGENDE REPEATERS MET NAAM. Elke pad-hash (hsize byte) is afgeleid van
     * de pubkey van de doorgevende node; los hem op via de buurtlijst (hash == de
     * eerste hsize byte van de pubkey) en toon de NAAM. Onbekend -> de hex-hash.
     * Begrensd op ~64 tekens met "…" zodat de regel in één pakket past. */
    uint8_t hsize = packet->getPathHashSize();
    if (hsize == 0) hsize = 1;
    int nhops = (int)packet->getPathHashCount();
    char route[96]; int ro = 0; route[0] = 0;
    for (int h = 0; h < nhops; h++) {
      if (ro > 62) { ro += snprintf(route + ro, sizeof(route) - ro, "%s…", h ? " > " : ""); break; }
      const uint8_t* hh = &packet->path[h * hsize];
      const char* rn = NULL;
      for (int k = 0; k < neighbours.getNumEntries(); k++) {
        const NeighbourEntry* e = neighbours.getEntryByIdx(k);
        if (e && memcmp(e->pub_key, hh, hsize) == 0 && e->name[0]) { rn = e->name; break; }
      }
      char hn[18];
      if (rn) {
        StrHelper::strncpy(hn, rn, sizeof(hn));
      } else {
        int nb = hsize > 4 ? 4 : hsize; char hx[9];
        mesh::Utils::toHex(hx, hh, nb); hx[nb * 2] = 0;
        StrHelper::strncpy(hn, hx, sizeof(hn));
      }
      ro += snprintf(route + ro, sizeof(route) - ro, "%s[%s]", h ? " > " : "", hn);
    }
    /* packet->_snr is SNR x 4 (zoals de buurtlijst het bewaart); RSSI van de radio. */
    float snr_db = ((float)packet->_snr) / 4.0f;
    int rssi = (int)radio_driver.getLastRSSI();
    if (nhops == 0) {
      snprintf(reply, sizeof(reply),
               "ack @[%s] (direct) | SNR: %.1f dB | RSSI: %d dBm | Received at: %s",
               name, snr_db, rssi, rxt);
    } else {
      snprintf(reply, sizeof(reply),
               "ack @[%s] via %s (%d hops) | SNR: %.1f dB | RSSI: %d dBm | Received at: %s",
               name, route, nhops, snr_db, rssi, rxt);
    }
  } else if (!strcasecmp(verb, "help") || verb[0] == '?') {
    snprintf(reply, sizeof(reply),
             "mesh-diagnose-bot: `ping` -> Pong; `path` -> afzender + tussenliggende "
             "repeaters + SNR/RSSI + tijd; `help`");
  } else {
    snprintf(reply, sizeof(reply), "onbekend commando. stuur `ping` of `path`.");
  }

  /* Diagnose-commando's krijgen het verklik-achtervoegsel (1-byte / geen scope),
   * tenzij de verklikker in de GUI uit staat. */
  if (_bot_diag && (!strcasecmp(verb, "ping") || !strcasecmp(verb, "path")))
    appendTxDiag(reply, sizeof(reply), packet);

  /* 1) ACK het inkomende bericht, zodat de app niet blijft herzenden. De ack-hash
   *    gaat over de ORIGINELE berichtbytes + de afzender-pubkey (zoals het room-pad). */
  uint32_t ack_hash;
  mesh::Utils::sha256((uint8_t*)&ack_hash, 4, data, 5 + strlen((char*)&data[5]), sender_pub, PUB_KEY_SIZE);
  mesh::Packet* ack = createAck(ack_hash);
  if (ack) sendFloodReply(ack, TXT_ACK_DELAY, packet->getPathHashSize());

  /* 2) Het antwoord als schone DM vanaf de bot (self_id is al _bot_id). Eén pakket:
   *    ping/path/help passen ruim binnen MAX_PACKET_PAYLOAD; kap voor de zekerheid. */
  static uint8_t out[MAX_PACKET_PAYLOAD];
  uint32_t ts = getRTCClock()->getCurrentTimeUnique();
  memcpy(out, &ts, 4);
  out[4] = (TXT_TYPE_PLAIN << 2);
  int rlen = (int)strlen(reply);
  /* Eén pakket: MeshCore's tekstlimiet is 160 (MAX_TEXT_LEN). Kap voor de zekerheid;
   * ping/help en een normale path-regel passen daar ruim binnen. */
  if (rlen > 160) rlen = 160;
  memcpy(&out[5], reply, rlen);
  mesh::Identity dest(sender_pub);
  mesh::Packet* rpkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, dest, secret, out, 5 + rlen);
  if (rpkt) sendFloodScoped(default_scope, rpkt, TXT_ACK_DELAY + SERVER_RESPONSE_DELAY, _prefs.path_hash_mode + 1);
}

/* ================================================================== */
/*  Hashtag-/publieke kanalen: de bot leest mee en antwoordt          */
/* ================================================================== */

void RoomMesh::channelComputeHash(BotChannel& c) {
  /* Het MeshCore group-channel-formaat: de kanaal-hash = eerste byte van
   * sha256(secret, secret_len). 16-byte (128-bit) of 32-byte (256-bit) sleutel. */
  mesh::Utils::sha256(&c.hash, 1, c.secret, c.secret_len);
}

int RoomMesh::channelFindByName(const char* name) const {
  if (!name) return -1;
  for (int i = 0; i < MAX_CHANNELS; i++)
    if (_channels[i].used && strcasecmp(_channels[i].name, name) == 0) return i;
  return -1;
}

int RoomMesh::channelCount() const {
  int n = 0;
  for (int i = 0; i < MAX_CHANNELS; i++) if (_channels[i].used) n++;
  return n;
}

bool RoomMesh::channelGet(int idx, char* out_name, int* out_bits, bool* out_enabled, uint8_t* out_hash, bool* out_derived, bool* out_public) const {
  int n = 0;
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!_channels[i].used) continue;
    if (n == idx) {
      if (out_name) StrHelper::strncpy(out_name, _channels[i].name, 24);
      if (out_bits) *out_bits = _channels[i].secret_len * 8;
      if (out_enabled) *out_enabled = _channels[i].enabled;
      if (out_hash) *out_hash = _channels[i].hash;
      if (out_derived) *out_derived = _channels[i].derived;
      if (out_public) *out_public = _channels[i].is_public;
      return true;
    }
    n++;
  }
  return false;
}

/* IWebNode-brug: naam + bits + enabled + hash-hex + afgeleid + publiek (nooit het secret). */
bool RoomMesh::webChannelGet(int i, char* name, size_t name_len, int* bits, bool* enabled, char* hashhex, bool* derived, bool* is_public) {
  uint8_t h = 0;
  char nm[24];
  if (!channelGet(i, nm, bits, enabled, &h, derived, is_public)) return false;
  if (name && name_len) StrHelper::strncpy(name, nm, name_len);
  if (hashhex) { mesh::Utils::toHex(hashhex, &h, 1); hashhex[2] = 0; }
  return true;
}

/* Is dit secret (16 byte) de vaste publieke sleutel? */
static bool channelSecretIsPublic(const uint8_t* secret, uint8_t secret_len) {
  if (secret_len != 16) return false;
  uint8_t pub[16];
  if (!mesh::Utils::fromHex(pub, 16, PUBLIC_GROUP_SECRET_HEX)) return false;
  return memcmp(secret, pub, 16) == 0;
}

/* Toevoegen/bijwerken op NAAM.
 *   secret_hex leeg/NULL:
 *     - naam is "public" (case-insensitief, met/zonder #) -> de VASTE publieke
 *       sleutel (8b3387e9...), zodat de node op het ECHTE publieke kanaal uitkomt.
 *     - anders -> HASHTAG-kanaal: sleutel = eerste 16 byte van sha256(naam), EXACT
 *       zoals de MeshCore-app ("#test" -> 9cd8fcf2...). Niet geheim.
 *   secret_hex = 32 of 64 hextekens -> expliciete 128-/256-bit sleutel (wint altijd). */
int RoomMesh::channelAdd(const char* name, const char* secret_hex, bool enabled) {
  if (!name || name[0] == 0) return -2;
  uint8_t secret[PUB_KEY_SIZE];
  memset(secret, 0, sizeof(secret));
  uint8_t secret_len;
  bool derived;
  if (secret_hex == NULL || secret_hex[0] == 0) {
    /* Naam-only. Speciale naam "public" (met of zonder #) -> vaste publieke sleutel. */
    const char* base = name;
    if (*base == '#') base++;
    if (strcasecmp(base, "public") == 0) {
      mesh::Utils::fromHex(secret, 16, PUBLIC_GROUP_SECRET_HEX);
    } else {
      mesh::Utils::sha256(secret, 16, (const uint8_t*)name, strlen(name));   // hashtag
    }
    secret_len = 16;
    derived = true;
  } else {
    int hexlen = (int)strlen(secret_hex);
    if (hexlen != 32 && hexlen != 64) return -2;       // 128- of 256-bit
    if (!mesh::Utils::fromHex(secret, hexlen / 2, secret_hex)) return -2;
    secret_len = (uint8_t)(hexlen / 2);
    derived = false;
  }

  int idx = channelFindByName(name);                   // bijwerken?
  if (idx < 0) {
    for (int i = 0; i < MAX_CHANNELS; i++) if (!_channels[i].used) { idx = i; break; }
    if (idx < 0) return -3;                             // vol
  }
  BotChannel& c = _channels[idx];
  memset(&c, 0, sizeof(c));
  c.used = true;
  c.enabled = enabled;
  c.derived = derived;
  c.is_public = channelSecretIsPublic(secret, secret_len);
  StrHelper::strncpy(c.name, name, sizeof(c.name));
  memcpy(c.secret, secret, PUB_KEY_SIZE);
  c.secret_len = secret_len;
  channelComputeHash(c);
  saveChannels();
  return 0;
}

int RoomMesh::channelDel(const char* name) {
  int idx = channelFindByName(name);
  if (idx < 0) return -2;
  memset(&_channels[idx], 0, sizeof(_channels[idx]));
  saveChannels();
  return 1;
}

int RoomMesh::channelSetEnabled(const char* name, bool en) {
  int idx = channelFindByName(name);
  if (idx < 0) return -2;
  _channels[idx].enabled = en;
  saveChannels();
  return 1;
}

/* Persistentie: één regel per kanaal  ->  c <enabled> <derived> <secrethex> <naam>
 * (naam als rest van de regel zodat spaties mogen; secret als hex). Het secret
 * staat OOK voor een afgeleid (hashtag) kanaal in het bestand -- het is niet
 * geheim en zo hoeft loadChannels() niet opnieuw te hashen. */
void RoomMesh::saveChannels() {
  if (_fs == NULL) return;
  File f = _fs->open(CHANNELS_CFG_PATH, "w", true);
  if (!f) return;
  f.printf("#MUCHAN2\n");
  char hex[PUB_KEY_SIZE * 2 + 1];
  for (int i = 0; i < MAX_CHANNELS; i++) {
    if (!_channels[i].used) continue;
    mesh::Utils::toHex(hex, _channels[i].secret, _channels[i].secret_len);
    hex[_channels[i].secret_len * 2] = 0;
    f.printf("c %d %d %s %s\n", _channels[i].enabled ? 1 : 0,
             _channels[i].derived ? 1 : 0, hex, _channels[i].name);
  }
  f.printf(".\n");
  f.close();
}

void RoomMesh::loadChannels() {
  memset(_channels, 0, sizeof(_channels));
  if (_fs == NULL || !_fs->exists(CHANNELS_CFG_PATH)) return;
  File f = _fs->open(CHANNELS_CFG_PATH, "r");
  if (!f) return;
  char line[160];
  bool first = true;
  int ci = 0;
  while (f.available() && ci < MAX_CHANNELS) {
    size_t len = 0;
    while (f.available() && len < sizeof(line) - 1) {
      int ch = f.read();
      if (ch < 0 || ch == '\n') break;
      if (ch == '\r') continue;
      line[len++] = (char)ch;
    }
    line[len] = 0;
    if (first) { first = false; continue; }
    if (line[0] != 'c') continue;
    // c <enabled> <derived> <secrethex> <naam>
    char* p = line + 1;
    while (*p == ' ') p++;
    int en  = atoi(p);  while (*p && *p != ' ') p++; while (*p == ' ') p++;
    int drv = atoi(p);  while (*p && *p != ' ') p++; while (*p == ' ') p++;
    char* hex = p;      while (*p && *p != ' ') p++; if (*p) { *p = 0; p++; } while (*p == ' ') p++;
    char* name = p;
    int hexlen = (int)strlen(hex);
    if ((hexlen != 32 && hexlen != 64) || name[0] == 0) continue;
    BotChannel& c = _channels[ci];
    memset(&c, 0, sizeof(c));
    if (!mesh::Utils::fromHex(c.secret, hexlen / 2, hex)) continue;
    c.secret_len = (uint8_t)(hexlen / 2);
    c.enabled = en ? true : false;
    c.derived = drv ? true : false;
    c.is_public = channelSecretIsPublic(c.secret, c.secret_len);
    c.used = true;
    StrHelper::strncpy(c.name, name, sizeof(c.name));
    channelComputeHash(c);
    ci++;
  }
  f.close();
}

/* De basisklasse zoekt bij een inkomend group-pakket de kanalen met deze hash;
 * wij leveren de INGESCHAKELDE kanalen die matchen, met hun secret zodat de
 * basisklasse kan ontsleutelen. */
int RoomMesh::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) {
  int n = 0;
  for (int i = 0; i < MAX_CHANNELS && n < max_matches; i++) {
    if (!_channels[i].used || !_channels[i].enabled) continue;
    if (_channels[i].hash != hash[0]) continue;
    channels[n].hash[0] = _channels[i].hash;
    memcpy(channels[n].secret, _channels[i].secret, PUB_KEY_SIZE);
    n++;
  }
  return n;
}

/* Een ontsleutelde group-tekst. Formaat: [ts:4][txt_type:1]["<afzender>: <bericht>"].
 * We knippen de "<afzender>: "-prefix eraf, herkennen ping/test/path en antwoorden
 * IN het kanaal (met de afzendernaam erbij). */
void RoomMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel,
                               uint8_t* data, size_t len) {
  if (type != PAYLOAD_TYPE_GRP_TXT || len < 6) return;
  if ((data[4] >> 2) != 0) return;         // alleen TXT_TYPE_PLAIN
  data[len] = 0;
  handleChannelText(packet, channel, (const char*)&data[5]);
}

/* Whitespace = spatie, TAB, CR of LF (de trimset voor kanaal-commando's). */
static inline bool chanIsWs(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

void RoomMesh::handleChannelText(mesh::Packet* packet, const mesh::GroupChannel& channel, const char* text) {
  if (!text) return;
  CHAN_DIAG("rx ruw=\"%s\" len=%u", text, (unsigned)strlen(text));

  /* MISLUKTE DECODE herkennen (task #4). De kanaal-hash is 1 byte (eerste byte van
   * sha256(secret)); botsen twee gejoynde kanalen daarop, dan ontsleutelt een
   * bericht met de VERKEERDE sleutel tot gorgel. Herken dat aan controltekens
   * (buiten \t\r\n) en log het -- zo is een hash-botsing te onderscheiden van een
   * parse-probleem of RF-verlies. */
  int nonprint = 0, total = 0;
  for (const char* p = text; *p; p++, total++) {
    unsigned char c = (unsigned char)*p;
    if (c != '\t' && c != '\r' && c != '\n' && c < 0x20) nonprint++;
  }
  if (nonprint > 0) {
    CHAN_DIAG("ONLEESBAAR (%d/%d controltekens) -> waarschijnlijk verkeerde sleutel "
              "(1-byte kanaal-hash-botsing?) of corrupt pakket; genegeerd", nonprint, total);
    return;
  }

  /* "<afzender>: <bericht>" scheiden -- TOLERANT (task #2):
   *  - probeer eerst ": ", dan een kale ":";
   *  - geen herkenbare, plausibele naam-prefix -> de HELE tekst is het bericht,
   *    zodat een kaal "ping" (zonder naam) ook werkt.
   * Een ':' zonder plausibel naam-prefix (te lang, of het prefix bevat al
   * whitespace/':' -- denk aan een URL) telt NIET als scheider. */
  char sender[24]; sender[0] = 0;
  const char* msg = text;
  const char* sep = strstr(text, ": ");
  int seplen = 2;
  if (!sep) { sep = strchr(text, ':'); seplen = 1; }
  if (sep) {
    int sl = (int)(sep - text);
    bool plausible = (sl > 0 && sl <= (int)sizeof(sender) - 1);
    for (int i = 0; plausible && i < sl; i++)
      if (chanIsWs(text[i]) || text[i] == ':') plausible = false;   // naam bevat geen ws/':'
    if (plausible) {
      memcpy(sender, text, sl); sender[sl] = 0;
      msg = sep + seplen;
    }
  }
  /* Leading whitespace van het bericht af. */
  while (*msg && chanIsWs(*msg)) msg++;

  /* Niet op onszelf reageren (de bot post ook onder _bot_name). */
  if (sender[0] && strcasecmp(sender, _bot_name) == 0) {
    CHAN_DIAG("van onszelf (%s) -> genegeerd", sender);
    return;
  }

  /* Alleen op een KAAL commando reageren (ping/test/path), evt. met een leading '#',
   * '!' of '/' dat sommige clients toevoegen. Verb = tot de eerste whitespace, dan
   * trailing leestekens eraf (bv. "ping!"/"ping."). Zo blijft het kanaal niet
   * vollopen en breekt een trailing CR/LF de match niet meer. */
  const char* v = msg;
  if (*v == '#' || *v == '!' || *v == '/') v++;
  char verb[12]; int vi = 0;
  while (v[vi] && !chanIsWs(v[vi]) && vi < (int)sizeof(verb) - 1) { verb[vi] = v[vi]; vi++; }
  verb[vi] = 0;
  while (vi > 0) {   // trailing leestekens (niet-alfanumeriek) wegknippen
    char c = verb[vi - 1];
    bool alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    if (alnum) break;
    verb[--vi] = 0;
  }

  bool is_ping = !strcasecmp(verb, "ping");
  bool is_test = !strcasecmp(verb, "test");
  bool is_path = !strcasecmp(verb, "path");
  CHAN_DIAG("geparsed afzender=\"%s\" verb=\"%s\" -> %s", sender, verb,
            (is_ping || is_test || is_path) ? "MATCH" : "geen bot-commando (genegeerd)");
  if (!(is_ping || is_test || is_path)) return;   // geen bot-commando; negeren

  uint32_t now_s = getRTCClock()->getCurrentTime();
  char rxt[40]; fmtLocalHMS(now_s, rxt, sizeof(rxt));
  float snr_db = ((float)packet->_snr) / 4.0f;
  int rssi = (int)radio_driver.getLastRSSI();
  int nhops = (int)packet->getPathHashCount();

  static char reply[200];
  const char* who = sender[0] ? sender : "?";

  if (is_ping) {
    snprintf(reply, sizeof(reply), "@%s Pong (%s)", who, rxt);
  } else if (is_test) {
    /* Signaalrapport: "ik hoor je" met SNR/RSSI/hops. */
    snprintf(reply, sizeof(reply), "@%s gehoord \xE2\x9C\x93 SNR %.1f dB, RSSI %d dBm, %d hops (%s)",
             who, snr_db, rssi, nhops, rxt);
  } else {   // path: route met repeater-namen (zelfde als de DM-path)
    uint8_t hsize = packet->getPathHashSize(); if (hsize == 0) hsize = 1;
    char route[80]; int ro = 0; route[0] = 0;
    for (int h = 0; h < nhops; h++) {
      if (ro > 50) { ro += snprintf(route + ro, sizeof(route) - ro, "%s\xE2\x80\xA6", h ? " > " : ""); break; }
      const uint8_t* hh = &packet->path[h * hsize];
      const char* rn = NULL;
      for (int k = 0; k < neighbours.getNumEntries(); k++) {
        const NeighbourEntry* e = neighbours.getEntryByIdx(k);
        if (e && memcmp(e->pub_key, hh, hsize) == 0 && e->name[0]) { rn = e->name; break; }
      }
      char hn[18];
      if (rn) StrHelper::strncpy(hn, rn, sizeof(hn));
      else { int nb = hsize > 4 ? 4 : hsize; char hx[9]; mesh::Utils::toHex(hx, hh, nb); hx[nb*2]=0; StrHelper::strncpy(hn, hx, sizeof(hn)); }
      ro += snprintf(route + ro, sizeof(route) - ro, "%s[%s]", h ? " > " : "", hn);
    }
    if (nhops == 0)
      snprintf(reply, sizeof(reply), "@%s direct | SNR %.1f dB | RSSI %d dBm | %s", who, snr_db, rssi, rxt);
    else
      snprintf(reply, sizeof(reply), "@%s via %s (%d hops) | SNR %.1f dB | RSSI %d dBm | %s",
               who, route, nhops, snr_db, rssi, rxt);
  }

  /* Kanaal-commando's zijn altijd diagnose -> verklik 1-byte / geen scope,
   * tenzij de verklikker in de GUI uit staat. */
  if (_bot_diag) appendTxDiag(reply, sizeof(reply), packet);

  sendChannelReply(channel, reply);
}

/* "<botnaam>: <reply>" bouwen en IN het kanaal versturen (geflood). */
void RoomMesh::sendChannelReply(const mesh::GroupChannel& channel, const char* reply) {
  static uint8_t temp[MAX_PACKET_PAYLOAD];
  uint32_t ts = getRTCClock()->getCurrentTimeUnique();
  memcpy(temp, &ts, 4);
  temp[4] = 0;   // TXT_TYPE_PLAIN
  int off = 5 + snprintf((char*)&temp[5], MAX_PACKET_PAYLOAD - 6, "%s: %s",
                         _bot_name[0] ? _bot_name : "bot", reply);
  if (off > MAX_PACKET_PAYLOAD - 1) off = MAX_PACKET_PAYLOAD - 1;
  /* self_id op de hoofdidentiteit (group-pakketten worden niet met self_id
   * ondertekend, maar houd de dispatch-toestand netjes). */
  mesh::GroupChannel ch = channel;   // niet-const kopie voor de API
  mesh::Packet* pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_TXT, ch, temp, off);
  if (pkt) sendFloodScoped(default_scope, pkt, SERVER_RESPONSE_DELAY, _prefs.path_hash_mode + 1);
}

/* Ad-hoc schone DM vanaf de bot naar één pubkey (flash-melding). Enqueue als een
 * gerichte AlertTask met een 1-ontvanger-lijst? Nee: hergebruik het AlertTask-pad
 * niet (dat itereert de HELE lijst). We sturen hier direct, best-effort met de
 * standaard flood-herhaling van de mesh. Lange tekst wordt geknipt. */
int RoomMesh::botSendTo(const uint8_t* pubkey, const char* text) {
  if (!_bot_active || !text || text[0] == 0) return -1;
  uint8_t secret[PUB_KEY_SIZE];
  _bot_id.calcSharedSecret(secret, pubkey);
  mesh::Identity dest(pubkey);

  size_t total = strlen(text);
  size_t chunk = MAX_PACKET_PAYLOAD - 6;   // 4 ts + 1 flags + marge
  if (chunk > MAX_POST_TEXT_LEN) chunk = MAX_POST_TEXT_LEN;
  size_t off = 0;
  int sent = 0;
  while (off < total) {
    size_t n = total - off;
    if (n > chunk) n = chunk;
    uint8_t data[MAX_PACKET_PAYLOAD];
    uint32_t ts = getRTCClock()->getCurrentTimeUnique();
    memcpy(data, &ts, 4);
    data[4] = (TXT_TYPE_PLAIN << 2);
    memcpy(&data[5], text + off, n);
    self_id = _bot_id;
    auto pkt = createDatagram(PAYLOAD_TYPE_TXT_MSG, dest, secret, data, 5 + n);
    if (pkt) { sendFloodScoped(default_scope, pkt, (uint32_t)sent * 300, _prefs.path_hash_mode + 1); sent++; }
    off += n;
  }
  self_id = rooms[0].id;
  return sent > 0 ? 0 : -1;
}

/* DM de HELE ontvangerslijst (admin). Via het AlertTask-pad -> herhaal-tot-ACK
 * per ontvanger. Retour = aantal ontvangers, of <0 fout. */
int RoomMesh::botPost(const char* text) {
  if (!_bot_active || !text || text[0] == 0) return -1;
  int rc = botRecipCount();
  if (rc == 0) return 0;
  if (num_alert_tasks >= ROOM_MAX_ALERTS) return -3;   // wachtrij vol
  AlertTask* t = &alert_tasks[num_alert_tasks];
  StrHelper::strncpy(t->text, text, sizeof(t->text));
  t->high_pri = false;
  t->from_bot = true;
  t->send_expiry = 0;
  t->attempt = 4;
  t->curr_contact_idx = -1;
  num_alert_tasks++;
  return rc;
}

/* ---- IWebNode: bot ---- */
bool RoomMesh::webBotPubHex(char* out, size_t out_len) {
  if (!_bot_active || out_len < (size_t)(PUB_KEY_SIZE * 2 + 1)) return false;
  mesh::Utils::toHex(out, _bot_id.pub_key, PUB_KEY_SIZE);
  return true;
}

bool RoomMesh::webBotJoinUri(char* out, size_t out_len) {
  if (!_bot_active) return false;
  char hex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, _bot_id.pub_key, PUB_KEY_SIZE);
  char enc[sizeof(_bot_name) * 3 + 1];
  roomUrlEncode(_bot_name, enc, sizeof(enc));
  /* type=1 = ADV_TYPE_CHAT (Companion). Formaat uit MeshCore docs/qr_codes.md. */
  int n = snprintf(out, out_len,
                   "meshcore://contact/add?name=%s&public_key=%s&type=1", enc, hex);
  return n > 0 && (size_t)n < out_len;
}

bool RoomMesh::webBotRecipGet(int idx, char* pub64, size_t out_len, int* level) {
  if (out_len < (size_t)(PUB_KEY_SIZE * 2 + 1)) return false;
  uint8_t pub[PUB_KEY_SIZE];
  if (!botRecipGetByIdx(idx, pub)) return false;
  mesh::Utils::toHex(pub64, pub, PUB_KEY_SIZE);
  if (level) *level = 1;
  return true;
}

int RoomMesh::webBotRecipSet(const char* pub_hex, int level) {
  (void)level;
  if (!pub_hex || strlen(pub_hex) != PUB_KEY_SIZE * 2) return -2;
  uint8_t pubkey[PUB_KEY_SIZE];
  if (!mesh::Utils::fromHex(pubkey, PUB_KEY_SIZE, pub_hex)) return -2;
  int r = botRecipAdd(pubkey);
  if (r == 0 || r == -2) { saveBotRecips(); return 0; }   // -2 = al aanwezig: idempotent ok
  return r;
}

int RoomMesh::webBotRecipDel(const char* prefix_hex) {
  if (!prefix_hex) return -2;
  int hexlen = (int)strlen(prefix_hex);
  if (hexlen < 12 || (hexlen & 1) || hexlen > PUB_KEY_SIZE * 2) return -2;
  uint8_t prefix[PUB_KEY_SIZE];
  if (!mesh::Utils::fromHex(prefix, hexlen / 2, prefix_hex)) return -2;
  int r = botRecipDelPrefix(prefix, hexlen / 2);
  if (r == 1) saveBotRecips();
  return r;
}

int RoomMesh::webBotSendTo(const char* pub_hex, const char* text) {
  if (!pub_hex || strlen(pub_hex) != PUB_KEY_SIZE * 2) return -2;
  uint8_t pubkey[PUB_KEY_SIZE];
  if (!mesh::Utils::fromHex(pubkey, PUB_KEY_SIZE, pub_hex)) return -2;
  return botSendTo(pubkey, text);
}

/* ------------------------------------------------------------------ */
/*  CLI                                                                 */
/* ------------------------------------------------------------------ */
void RoomMesh::handleCommand(uint32_t sender_timestamp, char* command, char* reply) {
  while (*command == ' ') command++;

  if (strlen(command) > 4 && command[2] == '|') {
    memcpy(reply, command, 3);
    reply += 3;
    command += 3;
  }

  if (memcmp(command, "setperm ", 8) == 0) {
    char* hex = &command[8];
    char* sp = strchr(hex, ' ');
    if (sp == NULL) { strcpy(reply, "Err - bad params"); return; }
    *sp++ = 0;
    uint8_t pubkey[PUB_KEY_SIZE];
    int hex_len = min((int)(sp - hex), PUB_KEY_SIZE * 2);
    if (mesh::Utils::fromHex(pubkey, hex_len / 2, hex)) {
      uint8_t perms = atoi(sp);
      if (rooms[0].acl.applyPermissions(rooms[0].id, pubkey, hex_len / 2, perms)) {
        rooms[0].dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
        strcpy(reply, "OK");
      } else strcpy(reply, "Err - invalid params");
    } else strcpy(reply, "Err - bad pubkey");
  } else if (memcmp(command, "room.post ", 10) == 0 || memcmp(command, "room.post\t", 10) == 0) {
    char* msg = command + 9;
    while (*msg == ' ' || *msg == '\t') msg++;
    if (*msg == 0) strcpy(reply, "ERR lege tekst");
    else { addServerPost(0, msg); strcpy(reply, "OK"); }
  } else if (memcmp(command, "sensornode ", 11) == 0) {
    handleSensorNodeCommand(command + 11, reply);
  } else if (memcmp(command, "bot ", 4) == 0) {
    handleBotCommand(command + 4, reply);
  } else if (memcmp(command, "channel ", 8) == 0) {
    handleChannelCommand(command + 8, reply);
  } else if (memcmp(command, "room ", 5) == 0) {
    handleRoomCommand(command + 5, reply);
  } else if (sender_timestamp == 0 && strcmp(command, "get acl") == 0) {
    Serial.println("ACL (room 0):");
    for (int i = 0; i < rooms[0].acl.getNumClients(); i++) {
      auto c = rooms[0].acl.getClientByIdx(i);
      if (c->permissions == 0) continue;
      Serial.printf("%02X ", c->permissions);
      mesh::Utils::printHex(Serial, c->id.pub_key, PUB_KEY_SIZE);
      Serial.printf("\n");
    }
    reply[0] = 0;
  } else {
    _cli.handleCommand(sender_timestamp, command, reply);
  }
}

/* room list | add <naam> | del <idx> | set name|pass|guest <idx> <waarde> |
 * stealth <idx> on|off */
void RoomMesh::handleRoomCommand(char* args, char* reply) {
  while (*args == ' ') args++;

  if (memcmp(args, "acl", 3) == 0 && (args[3] == ' ' || args[3] == 0)) {
    handleAclSubcommand(ACL_KIND_ROOM, args + 3, reply);
    return;
  }
  if (memcmp(args, "advert ", 7) == 0) {
    char* q = args + 7;
    int idx = atoi(q);
    while (*q && *q != ' ') q++;
    while (*q == ' ') q++;
    bool flood = (memcmp(q, "flood", 5) == 0);
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) { strcpy(reply, "Err - idx"); return; }
    sendRoomAdvertisement(rooms[idx], 0, flood);
    sprintf(reply, "OK advert room %d (%s)", idx, flood ? "flood" : "zero-hop");
    return;
  }
  if (strncmp(args, "list", 4) == 0) {
    int n = 0;
    char* p = reply;
    p += sprintf(p, "rooms:");
    for (int i = 0; i < MAX_ROOMS; i++) {
      if (!rooms[i].active) continue;
      p += sprintf(p, " [%d]%s%s", i, rooms[i].name, rooms[i].stealth ? "(stealth)" : "");
      n++;
    }
    if (n == 0) strcpy(reply, "geen rooms");
    return;
  }
  if (memcmp(args, "add ", 4) == 0) {
    char* name = args + 4;
    while (*name == ' ') name++;
    if (*name == 0) { strcpy(reply, "Err - naam?"); return; }
    int idx = -1;
    for (int i = 1; i < MAX_ROOMS; i++) { if (!rooms[i].active) { idx = i; break; } }
    if (idx < 0) { strcpy(reply, "Err - vol"); return; }
    rooms[idx].active = true;
    _num_active_rooms++;
    StrHelper::strncpy(rooms[idx].name, name, sizeof(rooms[idx].name));
    StrHelper::strncpy(rooms[idx].password, _prefs.password, sizeof(rooms[idx].password));
    rooms[idx].guest_password[0] = 0;
    rooms[idx].stealth = false;
    loadOrCreateRoomIdentity(idx);
    saveRoomConfig();
    updateAdvertTimer();
    updateFloodAdvertTimer();
    sendRoomAdvertisement(rooms[idx], 2000, false);
    sprintf(reply, "OK room %d '%s'", idx, rooms[idx].name);
    return;
  }
  if (memcmp(args, "del ", 4) == 0) {
    int idx = atoi(args + 4);
    if (idx <= 0 || idx >= MAX_ROOMS || !rooms[idx].active) { strcpy(reply, "Err - idx"); return; }
    rooms[idx].active = false;
    _num_active_rooms--;
    for (int k = 0; k < MAX_TOTAL_POSTS; k++) if (_post_pool[k].room_idx == idx) { memset(&_post_pool[k], 0, sizeof(PostInfo)); _post_pool[k].room_idx = 0xFF; }
    saveRoomConfig();
    sprintf(reply, "OK room %d weg", idx);
    return;
  }
  if (memcmp(args, "stealth ", 8) == 0) {
    char* q = args + 8;
    int idx = atoi(q);
    while (*q && *q != ' ') q++;
    while (*q == ' ') q++;
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) { strcpy(reply, "Err - idx"); return; }
    rooms[idx].stealth = (memcmp(q, "on", 2) == 0);
    saveRoomConfig();
    updateAdvertTimer();
    updateFloodAdvertTimer();
    if (!rooms[idx].stealth) sendRoomAdvertisement(rooms[idx], 2000, false);
    sprintf(reply, "OK room %d stealth=%d", idx, rooms[idx].stealth ? 1 : 0);
    return;
  }
  if (memcmp(args, "set ", 4) == 0) {
    char* q = args + 4;                 // "<veld> <idx> <waarde>"
    char* field = q;
    while (*q && *q != ' ') q++;
    if (*q) *q++ = 0;
    while (*q == ' ') q++;
    int idx = atoi(q);
    while (*q && *q != ' ') q++;
    while (*q == ' ') q++;
    char* val = q;
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) { strcpy(reply, "Err - idx"); return; }

    if (strcmp(field, "name") == 0) {
      StrHelper::strncpy(rooms[idx].name, val, sizeof(rooms[idx].name));
      if (idx == 0) StrHelper::strncpy(_prefs.node_name, val, sizeof(_prefs.node_name));
    } else if (strcmp(field, "pass") == 0) {
      StrHelper::strncpy(rooms[idx].password, val, sizeof(rooms[idx].password));
      if (idx == 0) { StrHelper::strncpy(_prefs.password, val, sizeof(_prefs.password)); savePrefs(); }
    } else if (strcmp(field, "guest") == 0) {
      StrHelper::strncpy(rooms[idx].guest_password, val, sizeof(rooms[idx].guest_password));
    } else { strcpy(reply, "Err - name|pass|guest"); return; }
    saveRoomConfig();
    if (idx == 0 && strcmp(field, "name") == 0) savePrefs();
    if (strcmp(field, "name") == 0 && !rooms[idx].stealth) sendRoomAdvertisement(rooms[idx], 2000, false);
    sprintf(reply, "OK room %d %s gezet", idx, field);
    return;
  }
  strcpy(reply, "room list|add|del|stealth|set name|pass|guest");
}

/* sensornode list | add <naam> | del <idx> | set name <idx> <naam> |
 * stealth <idx> on|off
 *
 * Symmetrisch met handleRoomCommand, maar voor de VIRTUELE SENSOR-NODES. Een
 * sensor-node kent geen wachtwoord-velden of gastwachtwoord (hij neemt het
 * beheerderswachtwoord van deze node over); wel een naam en stealth. */
void RoomMesh::handleSensorNodeCommand(char* args, char* reply) {
  while (*args == ' ') args++;

  if (memcmp(args, "acl", 3) == 0 && (args[3] == ' ' || args[3] == 0)) {
    handleAclSubcommand(ACL_KIND_SNODE, args + 3, reply);
    return;
  }
  if (memcmp(args, "advert ", 7) == 0) {
    char* q = args + 7;
    int idx = atoi(q);
    while (*q && *q != ' ') q++;
    while (*q == ' ') q++;
    bool flood = (memcmp(q, "flood", 5) == 0);
    if (idx < 0 || idx >= MAX_SENSOR_NODES || !snodes[idx].active) { strcpy(reply, "Err - idx"); return; }
    sendSensorNodeAdvertisement(snodes[idx], 0, flood);
    sprintf(reply, "OK advert snode %d (%s)", idx, flood ? "flood" : "zero-hop");
    return;
  }
  if (strncmp(args, "list", 4) == 0) {
    int n = 0;
    char* p = reply;
    p += sprintf(p, "snodes:");
    for (int i = 0; i < MAX_SENSOR_NODES; i++) {
      if (!snodes[i].active) continue;
      p += sprintf(p, " [%d]%s%s", i, snodes[i].name, snodes[i].stealth ? "(stealth)" : "");
      n++;
    }
    if (n == 0) strcpy(reply, "geen sensor-nodes");
    return;
  }
  if (memcmp(args, "add ", 4) == 0) {
    char* name = args + 4;
    while (*name == ' ') name++;
    if (*name == 0) { strcpy(reply, "Err - naam?"); return; }
    int idx = -1;
    for (int i = 0; i < MAX_SENSOR_NODES; i++) { if (!snodes[i].active) { idx = i; break; } }
    if (idx < 0) { strcpy(reply, "Err - vol"); return; }
    snodes[idx].active = true;
    _num_active_snodes++;
    StrHelper::strncpy(snodes[idx].name, name, sizeof(snodes[idx].name));
    StrHelper::strncpy(snodes[idx].password, _prefs.password, sizeof(snodes[idx].password));
    snodes[idx].guest_password[0] = 0;
    snodes[idx].stealth = false;
    loadOrCreateSensorNodeIdentity(idx);
    saveSensorNodeConfig();
    updateAdvertTimer();
    updateFloodAdvertTimer();
    sendSensorNodeAdvertisement(snodes[idx], 2000, false);
    sprintf(reply, "OK snode %d '%s'", idx, snodes[idx].name);
    return;
  }
  if (memcmp(args, "del ", 4) == 0) {
    int idx = atoi(args + 4);
    if (idx < 0 || idx >= MAX_SENSOR_NODES || !snodes[idx].active) { strcpy(reply, "Err - idx"); return; }
    snodes[idx].active = false;
    _num_active_snodes--;
    saveSensorNodeConfig();
    sprintf(reply, "OK snode %d weg", idx);
    return;
  }
  if (memcmp(args, "stealth ", 8) == 0) {
    char* q = args + 8;
    int idx = atoi(q);
    while (*q && *q != ' ') q++;
    while (*q == ' ') q++;
    if (idx < 0 || idx >= MAX_SENSOR_NODES || !snodes[idx].active) { strcpy(reply, "Err - idx"); return; }
    snodes[idx].stealth = (memcmp(q, "on", 2) == 0);
    saveSensorNodeConfig();
    updateAdvertTimer();
    updateFloodAdvertTimer();
    if (!snodes[idx].stealth) sendSensorNodeAdvertisement(snodes[idx], 2000, false);
    sprintf(reply, "OK snode %d stealth=%d", idx, snodes[idx].stealth ? 1 : 0);
    return;
  }
  if (memcmp(args, "set ", 4) == 0) {
    char* q = args + 4;                 // "name <idx> <waarde>"
    char* field = q;
    while (*q && *q != ' ') q++;
    if (*q) *q++ = 0;
    while (*q == ' ') q++;
    int idx = atoi(q);
    while (*q && *q != ' ') q++;
    while (*q == ' ') q++;
    char* val = q;
    if (idx < 0 || idx >= MAX_SENSOR_NODES || !snodes[idx].active) { strcpy(reply, "Err - idx"); return; }
    if (strcmp(field, "name") != 0) { strcpy(reply, "Err - alleen 'name'"); return; }
    StrHelper::strncpy(snodes[idx].name, val, sizeof(snodes[idx].name));
    saveSensorNodeConfig();
    if (!snodes[idx].stealth) sendSensorNodeAdvertisement(snodes[idx], 2000, false);
    sprintf(reply, "OK snode %d naam gezet", idx);
    return;
  }
  strcpy(reply, "sensornode list|add|del|stealth|set name");
}

/* bot list | add <pubkey64> | del <prefix>=>12hex | post <msg> | sendto <pubkey64>
 * <msg> | advert [flood] | uri
 * De ontvangerslijst draagt VOLLEDIGE pubkeys (het gedeelde geheim wordt eruit
 * berekend); verwijderen mag op een prefix (>=12 hex). */
void RoomMesh::handleBotCommand(char* args, char* reply) {
  while (*args == ' ') args++;

  if (strncmp(args, "list", 4) == 0) {
    char* p = reply;
    int n = 0;
    p += sprintf(p, "bot-ontvangers:");
    for (int i = 0; i < MAX_BOT_RECIPS; i++) {
      if (_bot_recips[i].level == 0) continue;
      n++;
      if ((p - reply) > 170) continue;   // reply-buffer niet overschrijden
      char hex[PUB_KEY_SIZE * 2 + 1];
      mesh::Utils::toHex(hex, _bot_recips[i].pub_key, PUB_KEY_SIZE);
      hex[12] = 0;   // korte weergave: prefix
      p += sprintf(p, " %s", hex);
    }
    if (n == 0) strcpy(reply, "bot-ontvangers: (leeg)");
    return;
  }
  if (memcmp(args, "add ", 4) == 0) {
    char* hex = args + 4;
    while (*hex == ' ') hex++;
    if (strlen(hex) < PUB_KEY_SIZE * 2) { strcpy(reply, "Err - volledige pubkey (64 hex) nodig"); return; }
    uint8_t pubkey[PUB_KEY_SIZE];
    if (!mesh::Utils::fromHex(pubkey, PUB_KEY_SIZE, hex)) { strcpy(reply, "Err - bad pubkey"); return; }
    int r = botRecipAdd(pubkey);
    if (r == 0)      { saveBotRecips(); strcpy(reply, "OK ontvanger toegevoegd"); }
    else if (r == -2) strcpy(reply, "OK (stond al in de lijst)");
    else              strcpy(reply, "Err - lijst vol");
    return;
  }
  if (memcmp(args, "del ", 4) == 0) {
    char* hex = args + 4;
    while (*hex == ' ') hex++;
    int hexlen = (int)strlen(hex);
    if (hexlen < 12 || (hexlen & 1) || hexlen > PUB_KEY_SIZE * 2) { strcpy(reply, "Err - prefix >=12 hex"); return; }
    uint8_t prefix[PUB_KEY_SIZE];
    if (!mesh::Utils::fromHex(prefix, hexlen / 2, hex)) { strcpy(reply, "Err - bad prefix"); return; }
    int r = botRecipDelPrefix(prefix, hexlen / 2);
    if (r == 1)      { saveBotRecips(); strcpy(reply, "OK ontvanger verwijderd"); }
    else if (r == -3) strcpy(reply, "Err - prefix past op meerdere; maak hem langer");
    else              strcpy(reply, "Err - niet gevonden");
    return;
  }
  if (memcmp(args, "post ", 5) == 0) {
    char* msg = args + 5;
    while (*msg == ' ') msg++;
    if (*msg == 0) { strcpy(reply, "Err - lege tekst"); return; }
    int r = botPost(msg);
    if (r > 0)       sprintf(reply, "OK gepost naar %d ontvanger(s)", r);
    else if (r == 0) strcpy(reply, "geen ontvangers");
    else             strcpy(reply, "Err - wachtrij vol");
    return;
  }
  if (memcmp(args, "sendto ", 7) == 0) {
    char* hex = args + 7;
    while (*hex == ' ') hex++;
    char* sp = strchr(hex, ' ');
    if (sp == NULL) { strcpy(reply, "gebruik: bot sendto <pubkey64> <bericht>"); return; }
    *sp++ = 0;
    while (*sp == ' ') sp++;
    if (strlen(hex) != PUB_KEY_SIZE * 2) { strcpy(reply, "Err - volledige pubkey (64 hex) nodig"); return; }
    uint8_t pubkey[PUB_KEY_SIZE];
    if (!mesh::Utils::fromHex(pubkey, PUB_KEY_SIZE, hex)) { strcpy(reply, "Err - bad pubkey"); return; }
    if (*sp == 0) { strcpy(reply, "Err - lege tekst"); return; }
    int r = botSendTo(pubkey, sp);
    strcpy(reply, r == 0 ? "OK verstuurd" : "Err - versturen mislukt");
    return;
  }
  if (memcmp(args, "advert", 6) == 0) {
    char* q = args + 6;
    while (*q == ' ') q++;
    bool flood = (memcmp(q, "flood", 5) == 0);
    sendBotAdvertisement(0, flood);
    sprintf(reply, "OK bot-advert (%s)", flood ? "flood" : "zero-hop");
    return;
  }
  if (strncmp(args, "uri", 3) == 0 || strncmp(args, "info", 4) == 0) {
    char uri[160];
    if (webBotJoinUri(uri, sizeof(uri))) snprintf(reply, 200, "%s : %s", _bot_name, uri);
    else strcpy(reply, "bot niet actief");
    return;
  }
  strcpy(reply, "bot list|add <pubkey>|del <prefix>|post <msg>|sendto <pubkey> <msg>|advert [flood]|uri");
}

/* channel list | add <naam> [secrethex] | del <naam> | on <naam> | off <naam>
 * Zonder secret bij 'add' -> hashtag-kanaal (sleutel uit de naam, zoals de app). */
void RoomMesh::handleChannelCommand(char* args, char* reply) {
  while (*args == ' ') args++;

  if (strncmp(args, "list", 4) == 0) {
    char* p = reply; int n = 0;
    p += sprintf(p, "kanalen:");
    for (int i = 0; i < MAX_CHANNELS; i++) {
      if (!_channels[i].used) continue;
      if ((p - reply) > 180) { p += sprintf(p, " ..."); break; }
      p += sprintf(p, " %s(%d-bit,%s,%s)", _channels[i].name, _channels[i].secret_len * 8,
                   _channels[i].enabled ? "aan" : "uit",
                   _channels[i].is_public ? "publiek" : (_channels[i].derived ? "afgeleid" : "eigen"));
      n++;
    }
    if (n == 0) strcpy(reply, "kanalen: (geen)");
    return;
  }
  if (memcmp(args, "add ", 4) == 0) {
    char* nm = args + 4; while (*nm == ' ') nm++;
    char* sp = strchr(nm, ' ');
    char* sec = NULL;
    if (sp) { *sp = 0; sec = sp + 1; while (*sec == ' ') sec++; if (*sec == 0) sec = NULL; }
    if (*nm == 0) { strcpy(reply, "gebruik: channel add <naam> [secrethex]"); return; }
    int r = channelAdd(nm, sec, true);
    if (r == 0) {
      int idx = channelFindByName(nm);
      const char* mode = "eigen sleutel";
      if (idx >= 0) mode = _channels[idx].is_public ? "publiek kanaal (vaste sleutel)"
                          : (_channels[idx].derived ? "sleutel afgeleid uit naam" : "eigen sleutel");
      snprintf(reply, 200, "OK kanaal '%s' (%s, #%02x)", nm, mode, (idx >= 0) ? _channels[idx].hash : 0);
    } else if (r == -3) strcpy(reply, "Err - kanalenlijst vol");
    else strcpy(reply, "Err - secret moet 32 of 64 hextekens zijn (of laat leeg)");
    return;
  }
  if (memcmp(args, "del ", 4) == 0) {
    char* nm = args + 4; while (*nm == ' ') nm++;
    if (*nm == 0) { strcpy(reply, "gebruik: channel del <naam>"); return; }
    strcpy(reply, channelDel(nm) == 1 ? "OK kanaal verwijderd" : "Err - niet gevonden");
    return;
  }
  if (memcmp(args, "on ", 3) == 0 || memcmp(args, "off ", 4) == 0) {
    bool on = (args[1] == 'n');
    char* nm = args + (on ? 3 : 4); while (*nm == ' ') nm++;
    if (*nm == 0) { strcpy(reply, "gebruik: channel on|off <naam>"); return; }
    strcpy(reply, channelSetEnabled(nm, on) == 1 ? (on ? "OK aan" : "OK uit") : "Err - niet gevonden");
    return;
  }
  strcpy(reply, "channel list|add <naam> [secrethex]|del <naam>|on <naam>|off <naam>");
}

/* read/readwrite/admin (+ aliassen) -> PERM_ACL_* ; 0 = onbekend. */
uint8_t RoomMesh::aclLevelFromWord(const char* w) {
  if (strcasecmp(w, "read") == 0 || strcasecmp(w, "ro") == 0 || strcasecmp(w, "readonly") == 0) return PERM_ACL_READ_ONLY;
  if (strcasecmp(w, "readwrite") == 0 || strcasecmp(w, "rw") == 0 || strcasecmp(w, "write") == 0) return PERM_ACL_READ_WRITE;
  if (strcasecmp(w, "admin") == 0) return PERM_ACL_ADMIN;
  return 0;
}

/* "acl <idx> list | add <pubkey> <read|readwrite|admin> | del <prefix>" voor room
 * (kind=ACL_KIND_ROOM) én sensor-node (kind=ACL_KIND_SNODE). */
void RoomMesh::handleAclSubcommand(int kind, char* args, char* reply) {
  while (*args == ' ') args++;
  int idx = atoi(args);
  char* p = args;
  while (*p && *p != ' ') p++;   // idx
  while (*p == ' ') p++;
  const char* kn = (kind == ACL_KIND_SNODE) ? "snode" : "room";
  if (slotRef(kind, idx) == NULL) { sprintf(reply, "Err - %s %d niet actief", kn, idx); return; }

  if (strncmp(p, "list", 4) == 0) {
    static const char* LVL[] = { "guest", "read", "readwrite", "admin" };
    char* r = reply;
    r += sprintf(r, "acl %s %d:", kn, idx);
    int n = 0;
    for (int i = 0; i < MAX_ACL_GRANTS; i++) {
      if (_grants[i].level == 0 || _grants[i].kind != kind || _grants[i].slot != idx) continue;
      char hex12[13]; mesh::Utils::toHex(hex12, _grants[i].pub_key, 6); hex12[12] = 0;
      r += sprintf(r, " %s=%s", hex12, LVL[_grants[i].level & 3]);
      n++;
    }
    if (n == 0) strcpy(reply, "geen grants");
    return;
  }
  if (memcmp(p, "add ", 4) == 0) {
    p += 4; while (*p == ' ') p++;
    char* hex = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    while (*p == ' ') p++;
    uint8_t level = aclLevelFromWord(p);
    if (level == 0) { strcpy(reply, "Err - niveau: read|readwrite|admin"); return; }
    if (strlen(hex) != PUB_KEY_SIZE * 2) { strcpy(reply, "Err - VOLLEDIGE pubkey (64 hex) vereist"); return; }
    uint8_t pubkey[PUB_KEY_SIZE];
    if (!mesh::Utils::fromHex(pubkey, PUB_KEY_SIZE, hex)) { strcpy(reply, "Err - bad pubkey"); return; }
    int rc = aclGrantSet(kind, idx, pubkey, PUB_KEY_SIZE, level);
    if (rc == 0) sprintf(reply, "OK grant gezet");
    else if (rc == -3) strcpy(reply, "Err - grant-tabel vol");
    else strcpy(reply, "Err - geweigerd");
    return;
  }
  if (memcmp(p, "del ", 4) == 0) {
    p += 4; while (*p == ' ') p++;
    char* hex = p;
    while (*p && *p != ' ') p++; *p = 0;
    int hexlen = (int)strlen(hex);
    if (hexlen < 12 || (hexlen & 1)) { strcpy(reply, "Err - prefix min. 12 hex, even aantal"); return; }
    int nbytes = hexlen / 2; if (nbytes > PUB_KEY_SIZE) nbytes = PUB_KEY_SIZE;
    uint8_t prefix[PUB_KEY_SIZE];
    if (!mesh::Utils::fromHex(prefix, nbytes, hex)) { strcpy(reply, "Err - bad prefix"); return; }
    int rc = aclGrantDel(kind, idx, prefix, nbytes);
    if (rc == 1) strcpy(reply, "OK grant weg");
    else if (rc == -3) strcpy(reply, "Err - prefix past op meerdere");
    else if (rc == -2) strcpy(reply, "Err - niet gevonden");
    else strcpy(reply, "Err");
    return;
  }
  strcpy(reply, "acl <idx> list|add <pubkey> <read|readwrite|admin>|del <prefix>");
}

/* ================================================================== */
/*  IWebNode: room-beheer voor de web-GUI                              */
/*                                                                     */
/*  De MUTATIES lopen via handleRoomCommand -- dezelfde keuring, dezelfde  */
/*  persistentie en dezelfde adverts als de CLI. De join-URI en de backup  */
/*  worden HIER opgebouwd omdat alleen deze klasse de room-identiteiten    */
/*  (pubkey + prv_key) kent.                                               */
/* ================================================================== */

/* URL-encode (RFC 3986 unreserved blijft; spatie -> '+'; rest -> %XX). Voor de
 * naam in de join-URI, precies zoals de MeshCore-app hem verwacht. */
static void roomUrlEncode(const char* in, char* out, size_t out_len) {
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 4 < out_len; i++) {
    unsigned char c = (unsigned char)in[i];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      out[o++] = (char)c;
    } else if (c == ' ') {
      out[o++] = '+';
    } else {
      out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 0x0f];
    }
  }
  out[o] = 0;
}

/* JSON-escape van \ en " (en control-chars als \u00xx). Voor de backup. */
static void roomJsonEscape(const char* in, char* out, size_t out_len) {
  size_t o = 0;
  for (size_t i = 0; in[i] && o + 7 < out_len; i++) {
    unsigned char c = (unsigned char)in[i];
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
    else if (c < 0x20)         { o += snprintf(out + o, out_len - o, "\\u%04x", c); }
    else                       { out[o++] = (char)c; }
  }
  out[o] = 0;
}

/* Plat-JSON-veld eruit halen: zoekt "key":"value" en vult dest (met de-escape van
 * \" en \\). Geeft true als het veld bestond. Geen JSON-lib -- zelfde aanpak als
 * SIREN's restore, ruim genoeg voor onze eigen backup-vorm. */
static bool roomExtractField(const char* json, const char* key, char* dest, size_t dest_len) {
  char pat[32];
  snprintf(pat, sizeof(pat), "\"%s\":\"", key);
  const char* p = strstr(json, pat);
  dest[0] = 0;
  if (!p) return false;
  p += strlen(pat);
  size_t o = 0;
  while (*p && *p != '"' && o < dest_len - 1) {
    if (*p == '\\' && (p[1] == '"' || p[1] == '\\')) { dest[o++] = p[1]; p += 2; }
    else dest[o++] = *p++;
  }
  dest[o] = 0;
  return true;
}

bool RoomMesh::webRoomPubHex(int idx, char* out, size_t out_len) {
  if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) return false;
  if (out_len < (size_t)(PUB_KEY_SIZE * 2 + 1)) return false;
  mesh::Utils::toHex(out, rooms[idx].id.pub_key, PUB_KEY_SIZE);
  return true;
}

bool RoomMesh::webRoomJoinUri(int idx, char* out, size_t out_len) {
  if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) return false;
  char hex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, rooms[idx].id.pub_key, PUB_KEY_SIZE);
  char enc[sizeof(rooms[idx].name) * 3 + 1];
  roomUrlEncode(rooms[idx].name, enc, sizeof(enc));
  /* type=3 = ADV_TYPE_ROOM (Room Server). Formaat uit MeshCore docs/qr_codes.md. */
  int n = snprintf(out, out_len,
                   "meshcore://contact/add?name=%s&public_key=%s&type=3", enc, hex);
  return n > 0 && (size_t)n < out_len;
}

int RoomMesh::webRoomAdd(const char* name) {
  if (!name || name[0] == 0) return -1;
  char cmd[8 + sizeof(rooms[0].name) + 8];
  char reply[96];
  snprintf(cmd, sizeof(cmd), "add %s", name);
  handleRoomCommand(cmd, reply);
  /* add antwoordt "OK room <idx> '<naam>'"; idx eruit lezen zonder sscanf. */
  if (strncmp(reply, "OK room ", 8) == 0) {
    int idx = atoi(reply + 8);
    if (idx > 0 && idx < MAX_ROOMS) return idx;
  }
  return -1;
}

bool RoomMesh::webRoomEdit(int idx, const char* name, const char* pass,
                           const char* guest, int stealth) {
  if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) return false;
  char cmd[16 + 64];
  char reply[96];
  if (name && name[0]) {
    snprintf(cmd, sizeof(cmd), "set name %d %s", idx, name);
    handleRoomCommand(cmd, reply);
  }
  if (pass && pass[0]) {
    snprintf(cmd, sizeof(cmd), "set pass %d %s", idx, pass);
    handleRoomCommand(cmd, reply);
  }
  if (guest) {   // ook lege string: wist het gastwachtwoord
    snprintf(cmd, sizeof(cmd), "set guest %d %s", idx, guest);
    handleRoomCommand(cmd, reply);
  }
  if (stealth >= 0) {
    snprintf(cmd, sizeof(cmd), "stealth %d %s", idx, stealth ? "on" : "off");
    handleRoomCommand(cmd, reply);
  }
  return true;
}

bool RoomMesh::webRoomDel(int idx) {
  if (idx <= 0 || idx >= MAX_ROOMS || !rooms[idx].active) return false;   // room 0 blijft
  char cmd[16];
  char reply[96];
  snprintf(cmd, sizeof(cmd), "del %d", idx);
  handleRoomCommand(cmd, reply);
  return strncmp(reply, "OK", 2) == 0;
}

/* ---- IWebNode: virtuele sensor-nodes (web-GUI). Lopen via handleSensorNodeCommand. */
bool RoomMesh::webSNodePubHex(int idx, char* out, size_t out_len) {
  if (idx < 0 || idx >= MAX_SENSOR_NODES || !snodes[idx].active) return false;
  if (out_len < (size_t)(PUB_KEY_SIZE * 2 + 1)) return false;
  mesh::Utils::toHex(out, snodes[idx].id.pub_key, PUB_KEY_SIZE);
  return true;
}

bool RoomMesh::webSNodeJoinUri(int idx, char* out, size_t out_len) {
  if (idx < 0 || idx >= MAX_SENSOR_NODES || !snodes[idx].active) return false;
  char hex[PUB_KEY_SIZE * 2 + 1];
  mesh::Utils::toHex(hex, snodes[idx].id.pub_key, PUB_KEY_SIZE);
  char enc[sizeof(snodes[idx].name) * 3 + 1];
  roomUrlEncode(snodes[idx].name, enc, sizeof(enc));
  /* type=4 = ADV_TYPE_SENSOR (Sensor). Formaat uit MeshCore docs/qr_codes.md. */
  int n = snprintf(out, out_len,
                   "meshcore://contact/add?name=%s&public_key=%s&type=4", enc, hex);
  return n > 0 && (size_t)n < out_len;
}

int RoomMesh::webSNodeAdd(const char* name) {
  if (!name || name[0] == 0) return -1;
  char cmd[8 + sizeof(snodes[0].name) + 8];
  char reply[96];
  snprintf(cmd, sizeof(cmd), "add %s", name);
  handleSensorNodeCommand(cmd, reply);
  if (strncmp(reply, "OK snode ", 9) == 0) {
    int idx = atoi(reply + 9);
    if (idx >= 0 && idx < MAX_SENSOR_NODES) return idx;
  }
  return -1;
}

bool RoomMesh::webSNodeEdit(int idx, const char* name, int stealth) {
  if (idx < 0 || idx >= MAX_SENSOR_NODES || !snodes[idx].active) return false;
  char cmd[16 + 64];
  char reply[96];
  if (name && name[0]) {
    snprintf(cmd, sizeof(cmd), "set name %d %s", idx, name);
    handleSensorNodeCommand(cmd, reply);
  }
  if (stealth >= 0) {
    snprintf(cmd, sizeof(cmd), "stealth %d %s", idx, stealth ? "on" : "off");
    handleSensorNodeCommand(cmd, reply);
  }
  return true;
}

bool RoomMesh::webSNodeDel(int idx) {
  if (idx < 0 || idx >= MAX_SENSOR_NODES || !snodes[idx].active) return false;
  char cmd[16];
  char reply[96];
  snprintf(cmd, sizeof(cmd), "del %d", idx);
  handleSensorNodeCommand(cmd, reply);
  return strncmp(reply, "OK", 2) == 0;
}

/* Backup: platte JSON met de VOLLEDIGE room-config INCL. identiteiten (prv+pub,
 * 96 byte -> 192 hex). GEVOELIG. Geeft de geschreven lengte terug, of 0. */
int RoomMesh::webRoomsBackup(char* out, size_t out_len) {
  if (!out || out_len < 128) return 0;
  int n = snprintf(out, out_len,
      "{\"type\":\"meshuptime-rooms-backup\",\"version\":1,\"max\":%d", MAX_ROOMS);
  char nesc[sizeof(rooms[0].name) * 6 + 1];
  char pesc[sizeof(rooms[0].password) * 6 + 1];
  char gesc[sizeof(rooms[0].guest_password) * 6 + 1];
  uint8_t idbuf[PRV_KEY_SIZE + PUB_KEY_SIZE];
  char    idhex[(PRV_KEY_SIZE + PUB_KEY_SIZE) * 2 + 1];
  for (int i = 0; i < MAX_ROOMS; i++) {
    if ((size_t)n > out_len - 512) return 0;   // te krap -> weiger i.p.v. afkappen
    if (!rooms[i].active) {
      n += snprintf(out + n, out_len - n, ",\"room%d_active\":\"0\"", i);
      continue;
    }
    roomJsonEscape(rooms[i].name, nesc, sizeof(nesc));
    roomJsonEscape(rooms[i].password, pesc, sizeof(pesc));
    roomJsonEscape(rooms[i].guest_password, gesc, sizeof(gesc));
    size_t idn = rooms[i].id.writeTo(idbuf, sizeof(idbuf));   // prv||pub = 96 byte
    mesh::Utils::toHex(idhex, idbuf, idn);
    n += snprintf(out + n, out_len - n,
        ",\"room%d_active\":\"1\",\"room%d_name\":\"%s\",\"room%d_stealth\":\"%d\","
        "\"room%d_guest\":\"%s\",\"room%d_pass\":\"%s\",\"room%d_id\":\"%s\"",
        i, i, nesc, i, rooms[i].stealth ? 1 : 0, i, gesc, i, pesc, i, idhex);
  }

  /* Virtuele sensor-nodes: naam, stealth en identiteit (prv||pub). Geen aparte
   * wachtwoorden (die volgen de node-standaard). */
  n += snprintf(out + n, out_len - n, ",\"snode_max\":%d", MAX_SENSOR_NODES);
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    if ((size_t)n > out_len - 512) return 0;
    if (!snodes[i].active) {
      n += snprintf(out + n, out_len - n, ",\"snode%d_active\":\"0\"", i);
      continue;
    }
    roomJsonEscape(snodes[i].name, nesc, sizeof(nesc));
    size_t idn = snodes[i].id.writeTo(idbuf, sizeof(idbuf));
    mesh::Utils::toHex(idhex, idbuf, idn);
    n += snprintf(out + n, out_len - n,
        ",\"snode%d_active\":\"1\",\"snode%d_name\":\"%s\",\"snode%d_stealth\":\"%d\","
        "\"snode%d_id\":\"%s\"",
        i, i, nesc, i, snodes[i].stealth ? 1 : 0, i, idhex);
  }

  n += snprintf(out + n, out_len - n, "}");
  memset(idbuf, 0, sizeof(idbuf));   // sleutelmateriaal van de stapel vegen
  return ((size_t)n < out_len) ? n : 0;
}

/* Restore: parse de platte backup-JSON en herstel de rooms INCL. identiteiten.
 * Streng: alleen op de eigen backup-marker. Room 0 (hoofdidentiteit) wordt NIET
 * overschreven tenzij "overwrite_main":"1" in de JSON staat. */
bool RoomMesh::webRoomsRestore(const char* json) {
  if (!json || !strstr(json, "\"meshuptime-rooms-backup\"")) return false;

  char ovr[4] = {0};
  roomExtractField(json, "overwrite_main", ovr, sizeof(ovr));
  bool allow_main = (ovr[0] == '1');

  for (int i = 0; i < MAX_ROOMS; i++) {
    char key[20];
    char active[4] = {0};
    snprintf(key, sizeof(key), "room%d_active", i);
    roomExtractField(json, key, active, sizeof(active));
    bool want = (active[0] == '1');

    if (!want) {
      if (i > 0 && rooms[i].active) {
        rooms[i].active = false;
        _num_active_rooms--;
        for (int k = 0; k < MAX_TOTAL_POSTS; k++)
          if (_post_pool[k].room_idx == i) { memset(&_post_pool[k], 0, sizeof(PostInfo)); _post_pool[k].room_idx = 0xFF; }
      }
      continue;
    }

    if (!rooms[i].active) { rooms[i].active = true; _num_active_rooms++; }

    char name[sizeof(rooms[0].name)]           = {0};
    char pass[sizeof(rooms[0].password)]       = {0};
    char guest[sizeof(rooms[0].guest_password)]= {0};
    char st[4]      = {0};
    char idhex[(PRV_KEY_SIZE + PUB_KEY_SIZE) * 2 + 1] = {0};

    snprintf(key, sizeof(key), "room%d_name", i);
    if (roomExtractField(json, key, name, sizeof(name)) && name[0]) {
      StrHelper::strncpy(rooms[i].name, name, sizeof(rooms[i].name));
      /* Room 0's naam is ook de node-naam (NodePrefs) -- gelijk houden, net als
       * de CLI 'set name 0' doet, anders liegt 'ver'/de UI na een restore. */
      if (i == 0) StrHelper::strncpy(_prefs.node_name, name, sizeof(_prefs.node_name));
    }
    snprintf(key, sizeof(key), "room%d_pass", i);
    if (roomExtractField(json, key, pass, sizeof(pass)) && pass[0]) {
      StrHelper::strncpy(rooms[i].password, pass, sizeof(rooms[i].password));
      if (i == 0) StrHelper::strncpy(_prefs.password, pass, sizeof(_prefs.password));
    }
    snprintf(key, sizeof(key), "room%d_guest", i);
    if (roomExtractField(json, key, guest, sizeof(guest)))
      StrHelper::strncpy(rooms[i].guest_password, guest, sizeof(rooms[i].guest_password));
    snprintf(key, sizeof(key), "room%d_stealth", i);
    if (roomExtractField(json, key, st, sizeof(st)))
      rooms[i].stealth = (st[0] == '1');

    /* Identiteit (96 byte). Room 0 alleen met expliciete toestemming. */
    snprintf(key, sizeof(key), "room%d_id", i);
    bool have_id = roomExtractField(json, key, idhex, sizeof(idhex));
    size_t want_hex = (PRV_KEY_SIZE + PUB_KEY_SIZE) * 2;
    if (have_id && strlen(idhex) == want_hex && (i > 0 || allow_main)) {
      uint8_t idbuf[PRV_KEY_SIZE + PUB_KEY_SIZE];
      if (mesh::Utils::fromHex(idbuf, sizeof(idbuf), idhex)) {
        rooms[i].id.readFrom(idbuf, sizeof(idbuf));
        if (i == 0) saveIdentity(rooms[0].id);   // persisteert naar "_main"
        else        saveRoomIdentity(i);
      }
      memset(idbuf, 0, sizeof(idbuf));
    } else if (i > 0) {
      /* Actieve room zonder (geldige) sleutel in de backup: zorg dat hij er een
       * heeft, anders kan hij niet ondertekenen/adverteren. */
      loadOrCreateRoomIdentity(i);
    }
    memset(idhex, 0, sizeof(idhex));
  }

  /* ---- Virtuele sensor-nodes herstellen (naam/stealth/identiteit) ---- */
  for (int i = 0; i < MAX_SENSOR_NODES; i++) {
    char key[24];
    char active[4] = {0};
    snprintf(key, sizeof(key), "snode%d_active", i);
    if (!roomExtractField(json, key, active, sizeof(active))) continue;  // veld afwezig -> ongemoeid
    bool want = (active[0] == '1');

    if (!want) {
      if (snodes[i].active) { snodes[i].active = false; _num_active_snodes--; }
      continue;
    }
    if (!snodes[i].active) { snodes[i].active = true; _num_active_snodes++; }

    char sname[sizeof(snodes[0].name)] = {0};
    char sst[4] = {0};
    char sidhex[(PRV_KEY_SIZE + PUB_KEY_SIZE) * 2 + 1] = {0};
    snprintf(key, sizeof(key), "snode%d_name", i);
    if (roomExtractField(json, key, sname, sizeof(sname)) && sname[0])
      StrHelper::strncpy(snodes[i].name, sname, sizeof(snodes[i].name));
    snprintf(key, sizeof(key), "snode%d_stealth", i);
    if (roomExtractField(json, key, sst, sizeof(sst))) snodes[i].stealth = (sst[0] == '1');
    if (snodes[i].password[0] == 0)
      StrHelper::strncpy(snodes[i].password, _prefs.password, sizeof(snodes[i].password));

    snprintf(key, sizeof(key), "snode%d_id", i);
    if (roomExtractField(json, key, sidhex, sizeof(sidhex)) &&
        strlen(sidhex) == (PRV_KEY_SIZE + PUB_KEY_SIZE) * 2) {
      uint8_t sidbuf[PRV_KEY_SIZE + PUB_KEY_SIZE];
      if (mesh::Utils::fromHex(sidbuf, sizeof(sidbuf), sidhex)) {
        snodes[i].id.readFrom(sidbuf, sizeof(sidbuf));
        saveSensorNodeIdentity(i);
      }
      memset(sidbuf, 0, sizeof(sidbuf));
    } else {
      loadOrCreateSensorNodeIdentity(i);   // geen (geldige) sleutel -> er een geven
    }
    memset(sidhex, 0, sizeof(sidhex));
  }

  saveRoomConfig();
  saveSensorNodeConfig();
  savePrefs();   // room 0's naam/wachtwoord kan naar NodePrefs zijn geschreven
  updateAdvertTimer();
  updateFloodAdvertTimer();
  return true;
}

/* ------------------------------------------------------------------ */
/*  ACL-helpers (IWebNode)                                              */
/* ------------------------------------------------------------------ */
bool RoomMesh::aclSetPerms(const uint8_t* pubkey, uint8_t perms) {
  if (perms == 0) return aclRemove(pubkey, PUB_KEY_SIZE);
  if ((perms & PERM_ACL_ROLE_MASK) == PERM_ACL_GUEST) {
    mesh::Identity id(pubkey);
    ClientInfo* c = rooms[0].acl.putClient(id, 0);
    if (c == NULL) return false;
    c->permissions = perms;
    rooms[0].id.calcSharedSecret(c->shared_secret, pubkey);
  } else {
    if (!rooms[0].acl.applyPermissions(rooms[0].id, pubkey, PUB_KEY_SIZE, perms)) return false;
  }
  rooms[0].dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
  return true;
}

int RoomMesh::aclCountMatching(const uint8_t* pubkey, int key_len) {
  if (key_len <= 0 || key_len > PUB_KEY_SIZE) return 0;
  int n = 0;
  for (int i = 0; i < rooms[0].acl.getNumClients(); i++) {
    auto c = rooms[0].acl.getClientByIdx(i);
    if (c->permissions == 0) continue;
    if (memcmp(c->id.pub_key, pubkey, key_len) == 0) n++;
  }
  return n;
}

bool RoomMesh::aclRemove(const uint8_t* pubkey, int key_len) {
  if (aclCountMatching(pubkey, key_len) != 1) return false;
  if (!rooms[0].acl.applyPermissions(rooms[0].id, pubkey, key_len, 0)) return false;
  rooms[0].dirty_contacts_expiry = futureMillis(LAZY_CONTACTS_WRITE_DELAY);
  return true;
}

/* ------------------------------------------------------------------ */
/*  sendFloodScoped / sendFloodReply                                    */
/* ------------------------------------------------------------------ */
void RoomMesh::sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size) {
  if (scope.isNull()) {
    sendFlood(pkt, delay_millis, path_hash_size);
  } else {
    uint16_t codes[2] = { scope.calcTransportCode(pkt), 0 };
    sendFlood(pkt, codes, delay_millis, path_hash_size);
  }
}

void RoomMesh::sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size) {
  if (recv_pkt_region && !recv_pkt_region->isWildcard()) {
    TransportKey scope;
    if (region_map.getTransportKeysFor(*recv_pkt_region, &scope, 1) > 0) {
      sendFloodScoped(scope, packet, delay_millis, path_hash_size);
    } else {
      sendFlood(packet, delay_millis, path_hash_size);
    }
  } else {
    sendFlood(packet, delay_millis, path_hash_size);
  }
}

/* ------------------------------------------------------------------ */
/*  CommonCLICallbacks -- routine                                       */
/* ------------------------------------------------------------------ */
void RoomMesh::saveIdentity(const mesh::LocalIdentity& new_id) {
  rooms[0].id = new_id;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  IdentityStore store(*_fs, "");
#else
  IdentityStore store(*_fs, "/identity");
#endif
  store.save("_main", new_id);
}

void RoomMesh::clearStats() {
  radio_driver.resetStats();
  resetStats();
  ((SimpleMeshTables*)getTables())->resetStats();
}

void RoomMesh::applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) {
  set_radio_at = futureMillis(2000);
  pending_freq = freq; pending_bw = bw; pending_sf = sf; pending_cr = cr;
  revert_radio_at = futureMillis(2000 + timeout_mins * 60 * 1000);
}

bool RoomMesh::formatFileSystem() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  return InternalFS.format();
#elif defined(RP2040_PLATFORM)
  return LittleFS.format();
#elif defined(ESP32)
  return SPIFFS.format();
#else
  return false;
#endif
}

void RoomMesh::setTxPower(int8_t power_dbm) { radio_driver.setTxPower(power_dbm); }

void RoomMesh::formatStatsReply(char* reply) {
  StatsFormatHelper::formatCoreStats(reply, board, *_ms, _err_flags, _mgr);
}
void RoomMesh::formatRadioStatsReply(char* reply) {
  StatsFormatHelper::formatRadioStats(reply, _radio, radio_driver, getTotalAirTime(), getReceiveAirTime());
}
void RoomMesh::formatPacketStatsReply(char* reply) {
  StatsFormatHelper::formatPacketStats(reply, radio_driver, getNumSentFlood(), getNumSentDirect(),
                                       getNumRecvFlood(), getNumRecvDirect());
}

void RoomMesh::startRegionsLoad() {
  temp_map.resetFrom(region_map);
  memset(load_stack, 0, sizeof(load_stack));
  load_stack[0] = &temp_map.getWildcard();
  region_load_active = true;
}
bool RoomMesh::saveRegions() { return region_map.save(_fs); }
void RoomMesh::onDefaultRegionChanged(const RegionEntry* r) {
  if (r) region_map.getTransportKeysFor(*r, &default_scope, 1);
  else memset(default_scope.key, 0, sizeof(default_scope.key));
}

int RoomMesh::calcRxDelay(float score, uint32_t air_time) const {
  if (_prefs.rx_delay_base <= 0.0f) return 0;
  return (int)((pow(_prefs.rx_delay_base, 0.85f - score) - 1.0) * air_time);
}
uint32_t RoomMesh::getRetransmitDelay(const mesh::Packet* packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.tx_delay_factor);
  return getRNG()->nextInt(0, 5 * t + 1);
}
uint32_t RoomMesh::getDirectRetransmitDelay(const mesh::Packet* packet) {
  uint32_t t = (_radio->getEstAirtimeFor(packet->getPathByteLen() + packet->payload_len + 2) * _prefs.direct_tx_delay_factor);
  return getRNG()->nextInt(0, 5 * t + 1);
}
