#pragma once

/* ============================================================================
 * RoomMesh -- MeshUptime als MULTIROOM room-server.
 *
 * WAT DIT IS, EN WAAROM HET EEN EIGEN KLASSE IS (en geen SensorMesh-modus)
 *
 * SensorMesh heeft EEN identiteit (self_id) en bedient het mesh als een enkele
 * node. Een room-server die de MeshCore-app als room ziet moet als het ROOM-type
 * adverteren en de room-inlog/post-synchronisatie spreken. MULTIROOM gaat nog een
 * stap verder: EEN toestel bedient MEERDERE room-identiteiten tegelijk, elk met
 * een eigen sleutelpaar, naam, wachtwoord en toegangslijst. De app ziet ze als
 * losse rooms.
 *
 * De truc waarmee EEN mesh::Mesh (die intern maar EEN self_id kent) toch N
 * identiteiten bedient komt uit SIREN (DinXke/SIREN, examples/siren_room_server):
 * onRecvPacket() kijkt naar de bestemmingshash (payload[0]) van een geadresseerd
 * pakket, zoekt de room-slot met die identiteit, en zet VOOR de afhandeling
 * `self_id = rooms[s].id` en `_active_slot = s`. De basisklasse ontsleutelt dan
 * met de juiste sleutel, en elke callback (searchPeersByHash, onAnonDataRecv,
 * onPeerDataRecv, ...) leest `_active_slot` om de juiste room te kiezen.
 *
 * De room-machinerie zelf (post-wachtrij, client-sync, login) is de v1.17.0
 * room-server uit MeshCore (examples/simple_room_server), veralgemeend naar N
 * slots met een GEDEELDE post-pool: MAX_TOTAL_POSTS posts, per-room quota =
 * totaal / aantal-actieve-rooms. Zo schaalt het RAM netjes met het aantal rooms.
 *
 * BEWUST NIET OVERGENOMEN uit SIREN (buiten scope, houdt het klein): de
 * server-naar-server-replicatie (version vectors / SYNCREQ), MQTT, tombstones,
 * de DM-ringbuffer, de naamtabel en de histogrammen. Wat blijft is de
 * multiroom-KERN plus onze eigen telemetrie, alert-bezorging en web/monitoring.
 *
 * Deze klasse wordt ALLEEN gebouwd in de env `meshuptime_room` (build-flag
 * ROOM_SERVER_VARIANT). De env `meshuptime` blijft SensorMesh gebruiken en
 * verandert niet -- dat is de terugvalweg.
 * ==========================================================================*/

#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "NeighbourList.h"

#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
#include <InternalFileSystem.h>
#elif defined(RP2040_PLATFORM)
#include <LittleFS.h>
#elif defined(ESP32)
#include <SPIFFS.h>
#endif

#include <helpers/ArduinoHelpers.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/IdentityStore.h>
#include <helpers/AdvertDataHelpers.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/CommonCLI.h>
#include <helpers/StatsFormatHelper.h>
#include <helpers/ClientACL.h>
#include <helpers/RegionMap.h>
#include <RTClib.h>
#include <target.h>

#include "IWebNode.h"

/* ------------------------------ Config -------------------------------- */

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "21 Aug 2026"
#endif
#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.17.0"
#endif

#include "Branding.h"

#define FIRMWARE_ROLE "room_server"

/* Alarm-bezorgroutes, als bitmasker. Zelfde waarden als in MonitorSensors, want
 * de per-sensor config draagt precies dit masker. */
#ifndef ALERT_MODE_DM
  #define ALERT_MODE_DM     1
  #define ALERT_MODE_ROOM   2
  #define ALERT_MODE_BOTH   (ALERT_MODE_DM | ALERT_MODE_ROOM)
#endif

/* De twee alarm-rechtbits op een ACL-ingang (DM-doelen). Zelfde waarden als in
 * SensorMesh.h; hier herhaald zodat RoomMesh die header niet hoeft te trekken. */
#ifndef PERM_RECV_ALERTS_LO
  #define PERM_RECV_ALERTS_LO    (1 << 6)
  #define PERM_RECV_ALERTS_HI    (1 << 7)
#endif

/* Aantal virtuele room-servers op EEN toestel. Bescheiden gehouden: elke room
 * draagt een eigen ClientACL (clients[MAX_CLIENTS]) en dat is de grootste
 * RAM-post. Zie het RAM-rapport bij deze wijziging. */
#ifndef MAX_ROOMS
  #define MAX_ROOMS   4
#endif

/* Gedeelde post-begroting over alle rooms. Per-room quota = totaal / actief.
 * Elke PostInfo ~= 32 (author) + 4 + 152 + 2 = ~190 byte; 48 * 190 = ~9 kB. */
#ifndef MAX_TOTAL_POSTS
  #define MAX_TOTAL_POSTS   48
#endif

#ifndef MAX_POST_TEXT_LEN
  #define MAX_POST_TEXT_LEN   (160 - 9)   /* 151, gelijk aan de room-server */
#endif

#ifndef ADVERT_LAT
  #define ADVERT_LAT   0.0
#endif
#ifndef ADVERT_LON
  #define ADVERT_LON   0.0
#endif
#ifndef ADMIN_PASSWORD
  #define ADMIN_PASSWORD   "password"
#endif
#ifndef ROOM_GUEST_PASSWORD
  #define ROOM_GUEST_PASSWORD   ""
#endif

#ifndef LORA_FREQ
  #define LORA_FREQ   869.525
#endif
#ifndef LORA_BW
  #define LORA_BW     250
#endif
#ifndef LORA_SF
  #define LORA_SF     11
#endif
#ifndef LORA_CR
  #define LORA_CR      5
#endif
#ifndef LORA_TX_POWER
  #define LORA_TX_POWER  22
#endif

#ifndef SERVER_RESPONSE_DELAY
  #define SERVER_RESPONSE_DELAY   300
#endif
#ifndef TXT_ACK_DELAY
  #define TXT_ACK_DELAY     200
#endif

#define ROOM_MAX_ALERTS   4   /* gelijktijdige DM-alarmtaken in de wachtrij */

/* Een post in de gedeelde pool. room_idx == 0xFF is een vrije plek. */
struct PostInfo {
  mesh::Identity author;
  uint32_t post_timestamp;     // door ONZE klok
  char     text[MAX_POST_TEXT_LEN + 1];
  uint8_t  room_idx;           // eigenaar-room; 0xFF = vrij
};

/* Een virtuele room-server: eigen sleutelpaar, naam, wachtwoorden, ACL en
 * synchronisatie-boekhouding. De posts staan NIET hier maar in de gedeelde
 * _post_pool (met room_idx). */
struct RoomSlot {
  bool                active;
  mesh::LocalIdentity id;

  char  name[24];
  char  password[16];        // beheerderswachtwoord (admin)
  char  guest_password[16];  // room-wachtwoord voor lezers/schrijvers; leeg = geen
  bool  stealth;             // true = adverteert niet (privé-room)

  ClientACL     acl;
  int           next_client_idx;
  uint16_t      num_posted, num_post_pushes;

  unsigned long next_push;
  unsigned long next_local_advert, next_flood_advert;
  unsigned long dirty_contacts_expiry;
};

/* Alarmtaak voor het DM-pad (met ACK-herhaling per contact). Overgenomen van
 * SensorMesh: room-posts zijn de betrouwbare weg (client-sync), maar wie DM wil
 * krijgt hier dezelfde herhaal-tot-ACK-bezorging als in de sensor-variant. */
struct AlertTask {
  uint32_t timestamp;
  bool     high_pri;
  uint32_t expected_acks[4];
  int8_t   curr_contact_idx;
  uint8_t  attempt;
  unsigned long send_expiry;
  char     text[MAX_PACKET_PAYLOAD];

  AlertTask() { text[0] = 0; }
  bool isTriggered() const { return text[0] != 0; }
};

class RoomMesh : public mesh::Mesh, public CommonCLICallbacks, public IWebNode {
public:
  RoomMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms,
           mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);

  void begin(FILESYSTEM* fs);
  void loop();

  /* Room 0 gebruikt de BESTAANDE hoofdidentiteit; main_room.cpp laadt die met
   * store.load("_main", ...) en geeft haar hier door VOOR begin(). Zo blijft de
   * pubkey (48d7aade232b) behouden en wordt er geen nieuwe sleutel gemaakt. */
  void setRoom0Identity(const mesh::LocalIdentity& id) { rooms[0].id = id; }

  /* ---- Publieke room-API (alerts, commando-antwoorden, web/CLI) ---- */

  /* Post een servertekst in room room_idx. Lange tekst wordt over meerdere posts
   * geknipt (MAX_POST_TEXT_LEN). Dit IS de bezorging: de post gaat de pool in en
   * wordt via client-sync naar alle gesynchroniseerde clients gepusht. */
  void addServerPost(int room_idx, const char* text);

  /* Fan-out-haak voor alarm-bezorging (JES/SIREN-stijl PostPublishCallback):
   * op een OVERGANG stelt main_room.cpp de tekst samen en roept dit aan met de
   * per-sensor mode (dm/room/both) en de room-set. Room-deel -> addServerPost naar
   * elke room in room_mask; DM-deel -> het bestaande AlertTask/sendAlert-pad. */
  void dispatchAlert(uint8_t mode, uint16_t room_mask, bool high_pri, const char* text);

  /* Optionele externe fan-out (bv. MQTT/push later). Wordt bij ELKE server-post
   * aangeroepen. */
  typedef void (*PostPublishCallback)(int room_idx, uint32_t ts,
                                      const uint8_t* author_pub, const char* text, void* ctx);
  void setPostPublishCallback(PostPublishCallback cb, void* ctx) { _post_cb = cb; _post_cb_ctx = ctx; }

  int  getNumActiveRooms() const { return _num_active_rooms; }
  bool isRoomActive(int i) const { return i >= 0 && i < MAX_ROOMS && rooms[i].active; }
  const char* getRoomName(int i) const { return (i >= 0 && i < MAX_ROOMS) ? rooms[i].name : ""; }
  const uint8_t* getRoomPubKey(int i) const { return (i >= 0 && i < MAX_ROOMS) ? rooms[i].id.pub_key : nullptr; }

  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);

  /* ---- IWebNode (webbeheer) ---- */
  void        handleCommandWeb(uint32_t ts, char* command, char* reply) override { handleCommand(ts, command, reply); }
  NodePrefs*  getNodePrefs() override { return &_prefs; }
  const char* getRoleName() override { return FIRMWARE_ROLE; }
  const uint8_t* getSelfPubKey() override { return rooms[0].id.pub_key; }
  uint32_t    nowSecs() override { return getRTCClock()->getCurrentTime(); }
  bool        getAclStrict() const override { return true; }   // room: altijd wachtwoord-poort
  void        setAclStrict(bool) override { }
  int         getAclCount() override { return rooms[0].acl.getNumClients(); }
  ClientInfo* getAclEntry(int idx) override { return rooms[0].acl.getClientByIdx(idx); }
  bool        aclSetPerms(const uint8_t* pubkey, uint8_t perms) override;
  bool        aclRemove(const uint8_t* pubkey, int key_len) override;
  int         aclCountMatching(const uint8_t* pubkey, int key_len) override;
  const NeighbourList& getNeighbours() const override { return neighbours; }
  void        requestSensorReadNow() override { last_read_time = 0; }

  /* ---- CommonCLICallbacks ---- */
  /* `ver` toont de MeshUptime-branding MET de MeshCore-versie erbij. Puur
   * informatief (alleen de `ver`-CLI leest dit); het protocol-versieveld is het
   * losse FIRMWARE_VER_LEVEL-byte, dus dit raakt de compatibiliteit niet. */
  const char* getFirmwareVer() override { return MESHUPTIME_BRAND_FULL(FIRMWARE_VERSION); }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  void savePrefs() override { _cli.savePrefs(_fs); }
  mesh::LocalIdentity& getSelfId() override { return rooms[0].id; }
  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;
  void setLoggingOn(bool enable) override { _logging = enable; }
  void eraseLogFile() override { }
  void dumpLogFile() override { }
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;
  bool formatFileSystem() override;
  void setTxPower(int8_t power_dbm) override;
  bool setRxBoostedGain(bool enable) override { return radio_driver.setRxBoostedGainMode(enable); }
  void formatNeighborsReply(char* reply) override { strcpy(reply, "not supported"); }
  void formatStatsReply(char* reply) override;
  void formatRadioStatsReply(char* reply) override;
  void formatPacketStatsReply(char* reply) override;
  void startRegionsLoad() override;
  bool saveRegions() override;
  void onDefaultRegionChanged(const RegionEntry* r) override;

  void sendFloodScoped(const TransportKey& scope, mesh::Packet* pkt, uint32_t delay_millis, uint8_t path_hash_size);

protected:
  float getAirtimeBudgetFactor() const override { return _prefs.airtime_factor; }
  int   calcRxDelay(float score, uint32_t air_time) const override;
  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;
  int   getInterferenceThreshold() const override { return _prefs.interference_threshold; }
  bool  getCADEnabled() const override { return _prefs.cad_enabled; }
  int   getAGCResetInterval() const override { return ((int)_prefs.agc_reset_interval) * 4000; }
  uint8_t getExtraAckTransmitCount() const override { return _prefs.multi_acks; }

  mesh::DispatcherAction onRecvPacket(mesh::Packet* pkt) override;
  bool allowPacketForward(const mesh::Packet* packet) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len) override;
  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  int  searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onAckRecv(mesh::Packet* packet, uint32_t ack_crc) override;

  void sendFloodReply(mesh::Packet* packet, unsigned long delay_millis, uint8_t path_hash_size);

  /* ---- app-hook: main_room.cpp leest de sensoren en post/alarmeert ---- */
  virtual void onSensorDataRead() { }

  /* ---- app-hook: een room-post herkennen als commando (list/get/status/...).
   * main_room.cpp overschrijft dit en gebruikt DmCommands + MonitorDmSource om
   * het antwoord op te bouwen. Geeft de lengte van de opgebouwde tekst terug (0 =
   * geen commando); RoomMesh post die tekst dan terug in de room (geknipt). */
  virtual int roomCommandReply(ClientInfo* from, int room_idx, const char* line, char* out, size_t out_len) {
    (void)from; (void)room_idx; (void)line; (void)out; (void)out_len; return 0;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() { sensors.setSettingValue("gps", _prefs.gps_enabled ? "1" : "0"); }
#endif

private:
  FILESYSTEM*   _fs;
  RoomSlot      rooms[MAX_ROOMS];
  int           _num_active_rooms;
  int           _active_slot;

  PostInfo      _post_pool[MAX_TOTAL_POSTS];

  uint32_t      last_millis;
  uint64_t      uptime_millis;
  bool          _logging;
  bool          region_load_active;

  NodePrefs         _prefs;
  TransportKeyStore key_store;
  RegionMap         region_map, temp_map;
  ClientACL&        cli_acl;   // = rooms[0].acl, geleend aan CommonCLI
  CommonCLI         _cli;

  NeighbourList neighbours;
  uint8_t       reply_data[MAX_PACKET_PAYLOAD];
  int           matching_peer_indexes[MAX_CLIENTS];
  CayenneLPP    telemetry;
  RegionEntry*  load_stack[8];
  RegionEntry*  recv_pkt_region;
  TransportKey  default_scope;

  unsigned long set_radio_at, revert_radio_at;
  float         pending_freq, pending_bw;
  uint8_t       pending_sf, pending_cr;

  uint32_t      last_read_time;

  /* DM-alarmpad. alert_tasks[] is de opslag EN de wachtrij (kop = index 0); bij
   * afronden schuift de rij op. Vier stuks van ~200 byte = goedkoop te kopieren. */
  int           num_alert_tasks;
  AlertTask     alert_tasks[ROOM_MAX_ALERTS];

  PostPublishCallback _post_cb;
  void*               _post_cb_ctx;

  /* ---- per-slot helpers ---- */
  void          addPost(RoomSlot& slot, ClientInfo* client, const char* text);
  void          storePost(uint8_t room_idx, const mesh::Identity& author, const char* text);
  void          pushPostToClient(RoomSlot& slot, ClientInfo* client, PostInfo& post);
  uint8_t       getUnsyncedCount(RoomSlot& slot, ClientInfo* client);
  bool          processAckForSlot(RoomSlot& slot, const uint8_t* data);
  mesh::Packet* createRoomAdvert(RoomSlot& slot);
  void          sendRoomAdvertisement(RoomSlot& slot, int delay_millis, bool flood);
  void          loopSlot(RoomSlot& slot);
  int           handleRequest(RoomSlot& slot, ClientInfo* sender, uint32_t sender_timestamp, uint8_t* payload, size_t payload_len);

  /* ---- DM-alarmpad ---- */
  void          sendAlertDM(const ClientInfo* c, AlertTask* t);

  /* ---- persistentie ---- */
  void          saveRoomIdentity(int idx);
  void          loadOrCreateRoomIdentity(int idx);
  void          saveRoomConfig();
  void          loadRoomConfig();

  /* ---- CLI ---- */
  void          handleRoomCommand(char* args, char* reply);

  static bool   saveFilter(ClientInfo* client) { return client->isAdmin(); }
};
