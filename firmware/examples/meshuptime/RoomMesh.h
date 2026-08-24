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

/* Aantal VIRTUELE SENSOR-NODES op EEN toestel. Elk is een aparte identiteit die
 * als ADV_TYPE_SENSOR adverteert, zodat de MeshCore-app telemetrie toont (de app
 * toont die alleen voor contacten van het sensor-type; onze rooms zijn ROOM-type).
 * Een sensor-node hergebruikt de RoomSlot-machinerie (login/ACL/telemetrie) maar
 * kent GEEN posts. Sensoren worden per stuk aan nul of meer sensor-nodes gekoppeld
 * (MonitorCfgEntry.sensornodes); zo kan de telemetrie over meerdere nodes verdeeld
 * worden, elk binnen de één-pakket-CayenneLPP-limiet. Bescheiden i.v.m. RAM (geen
 * PSRAM): elke slot draagt een eigen ClientACL. Zie het RAM-rapport. */
#ifndef MAX_SENSOR_NODES
  #define MAX_SENSOR_NODES   4
#endif

/* PER-SLEUTEL TOEGANGSGRANTS (wachtwoordloze toegang op basis van de pubkey).
 *
 * Naast het wachtwoord-pad kan elke room- én sensor-node-slot een lijst van
 * (pubkey -> niveau) dragen. Staat de afzender-pubkey erin met voldoende niveau,
 * dan wordt toegang verleend ZONDER wachtwoord (de pubkey is cryptografisch
 * gebonden: het antwoord wordt met het uit die pubkey afgeleide ECDH-geheim
 * versleuteld, dus een sleutel die je niet bezit is nutteloos).
 *
 * Deze GRANTS-tabel is bewust apart van de runtime-ClientACL: die ACL mengt
 * vluchtige login-sessies met expliciete grants; de tabel bevat ALLEEN de
 * expliciete grants en is de persistente bron (/acl_grants). Bij een login
 * (onAnonDataRecv) wordt de tabel geraadpleegd; een treffer maakt/ververst de
 * runtime-ACL-ingang met het grant-niveau, zonder wachtwoord.
 *
 * Niveau = de bestaande MeshCore ClientACL-rolbits (PERM_ACL_*):
 *   read      -> PERM_ACL_READ_ONLY  (1)
 *   readwrite -> PERM_ACL_READ_WRITE (2)
 *   admin     -> PERM_ACL_ADMIN      (3)
 * level == 0 (GUEST) = vrije/ongebruikte ingang. */
#define ACL_KIND_ROOM   0
#define ACL_KIND_SNODE  1
#ifndef MAX_ACL_GRANTS
  #define MAX_ACL_GRANTS  16
#endif

/* DM-ONTVANGERS VAN DE BOT (zie de bot-uitleg in RoomMesh.cpp). Een persistente
 * set volledige pubkeys; de bot DM't hen bij flash-meldingen en bij de per-sensor
 * dm/both-alerts. 16 ingangen, elk 33 byte. */
#ifndef MAX_BOT_RECIPS
  #define MAX_BOT_RECIPS  16
#endif
#ifndef BOT_NAME_DEFAULT
  #define BOT_NAME_DEFAULT   "BE-HSS-DinX-Bot"
#endif
/* MEERDERE BOT-IDENTITEITEN (v2.5.0). De node draagt N onafhankelijke bots, elk met
 * een eigen sleutelpaar, naam, ontvangerslijst en zend-diagnose. Bot #0 is de
 * bestaande alert-bot (BE-HSS-DinX-Bot, /bot_id + /bot_recips, ONGEWIJZIGD); bot #1
 * is de companion-MANAGEMENT-bot. Precies één bot draagt de "alert"-rol (die gebruikt
 * dispatchAlert). Zie de bot-uitleg in RoomMesh.cpp. */
#ifndef MAX_BOTS
  #define MAX_BOTS  4
#endif
#ifndef BOT_MGMT_NAME_DEFAULT
  #define BOT_MGMT_NAME_DEFAULT   "BE-HSS-DinX-MGMT"
#endif

/* COMPANIONS (v2.4.0). Een persistente lijst van companion-apparaten (T1000-E
 * e.d.): de bot stuurt hen `!`-commando's (find/loc/mute/...) als DM en de node
 * ontvangt hun #LOC-locatierapporten terug. 16 ingangen, elk ~72 byte. */
#ifndef MAX_COMPANIONS
  #define MAX_COMPANIONS  16
#endif

/* HASHTAG-/PUBLIEKE KANALEN die de bot meeleest. Een MeshCore group-channel is een
 * gedeeld geheim (16 of 32 byte); de kanaal-hash = eerste byte van sha256(secret).
 * De bot antwoordt IN het kanaal op ping/test/path. Bescheiden aantal i.v.m. RAM. */
#ifndef MAX_CHANNELS
  #define MAX_CHANNELS  8
#endif

/* Gedeelde post-begroting over alle rooms. Per-room quota = totaal / actief.
 * Elke PostInfo ~= 32 (author) + 4 + 152 + 2 = ~190 byte; 48 * 190 = ~9 kB. */
#ifndef MAX_TOTAL_POSTS
  #define MAX_TOTAL_POSTS   48
#endif

#ifndef MAX_POST_TEXT_LEN
  #define MAX_POST_TEXT_LEN   (160 - 9)   /* 151, gelijk aan de room-server */
#endif

/* MeshCore's tekstlimiet voor één bericht (BaseChatMesh MAX_TEXT_LEN, 10 cipher-
 * blokken). Hier los gedefinieerd: RoomMesh includeert BaseChatMesh niet. */
#ifndef BOT_MAX_TEXT_LEN
  #define BOT_MAX_TEXT_LEN    160
#endif

/* Uitleg-URL achter "geen scope" in de zend-diagnose. Ruim genoeg voor een
 * gewone docs-link; de bot kapt de URL NOOIT af (zie buildTxDiag). */
#ifndef MAX_DIAG_URL_LEN
  #define MAX_DIAG_URL_LEN    96
#endif
#ifndef DEFAULT_DIAG_URL
  #define DEFAULT_DIAG_URL    "https://nodenet.be/instellingen/regio-en-scope/"
#endif

/* Per welk bot-commando de zend-diagnose achter het antwoord komt. */
#define DIAG_PING   0x01
#define DIAG_TEST   0x02
#define DIAG_PATH   0x04
#define DIAG_ALL    (DIAG_PING | DIAG_TEST | DIAG_PATH)

/* Hoe de uitleg-URL bij een ongescopet pakket meegaat. INLINE kan wegvallen als
 * het antwoord al tegen de 160 tekens zit; SEPARATE past altijd. */
#define DIAG_URL_OFF        0
#define DIAG_URL_INLINE     1
#define DIAG_URL_SEPARATE   2

#ifndef DIAG_URL_MSG_PREFIX
  #define DIAG_URL_MSG_PREFIX  "Meer info over regions en scopes: "
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

/* Een expliciete per-sleutel toegangsgrant op een room- of sensor-node-slot.
 * level == 0 (GUEST) = vrije ingang. Zie de uitleg bij MAX_ACL_GRANTS. */
struct AclGrant {
  uint8_t kind;                    // ACL_KIND_ROOM / ACL_KIND_SNODE
  uint8_t slot;                    // slot-index binnen dat type
  uint8_t level;                   // PERM_ACL_READ_ONLY/READ_WRITE/ADMIN; 0 = vrij
  uint8_t pub_key[PUB_KEY_SIZE];   // VOLLEDIGE pubkey (nodig voor het ECDH-geheim)
};

/* Alarmtaak voor het DM-pad (met ACK-herhaling per contact). Overgenomen van
 * SensorMesh: room-posts zijn de betrouwbare weg (client-sync), maar wie DM wil
 * krijgt hier dezelfde herhaal-tot-ACK-bezorging als in de sensor-variant. */
struct AlertTask {
  uint32_t timestamp;
  bool     high_pri;
  bool     from_bot;           // true = DM vanaf een bot naar zijn ontvangerslijst
  uint8_t  bot_idx;            // welke bot verstuurt (alleen zinvol als from_bot)
  uint32_t expected_acks[4];
  int8_t   curr_contact_idx;
  uint8_t  attempt;
  unsigned long send_expiry;
  char     text[MAX_PACKET_PAYLOAD];

  AlertTask() { text[0] = 0; from_bot = false; bot_idx = 0; }
  bool isTriggered() const { return text[0] != 0; }
};

/* Eén DM-ontvanger van de bot: de VOLLEDIGE pubkey (nodig voor het ECDH-geheim).
 * level is gereserveerd (nu altijd 1 = "krijgt alle meldingen"); apart gehouden
 * zodat later filtering per ontvanger kan zonder het opslagformaat te breken. */
struct BotRecip {
  uint8_t pub_key[PUB_KEY_SIZE];
  uint8_t level;               // 0 = vrije ingang; >=1 = actief
};

/* Eén bot-identiteit (v2.5.0). Elk slot heeft een eigen persistent sleutelpaar
 * (bot #0 in "/bot_id", bot #i in "/bot_id_i"), een naam, een aan/uit-vlag, een
 * eigen ontvangerslijst + zend-diagnose (bot #0 in "/bot_recips", bot #i in
 * "/bot_recips_i") en de rol-vlag is_alert (precies één bot). used=false is een
 * vrije ingang. De alert-bot (is_alert) is de zendweg voor dispatchAlert; de andere
 * bots dragen bv. companion-MANAGEMENT-verkeer. */
struct BotSlot {
  bool                used;          // slot bezet (draagt een sleutelpaar)
  bool                enabled;       // doet mee (adverteert + beantwoordt DM's); uit = stil
  bool                is_alert;      // rol: dispatchAlert gebruikt deze bot
  mesh::LocalIdentity id;
  char                name[24];
  uint8_t             diag_mask;     // bit0 ping, bit1 test, bit2 path
  uint8_t             diag_url_mode; // DIAG_URL_OFF / _INLINE / _SEPARATE
  char                diag_url[MAX_DIAG_URL_LEN + 1];
  BotRecip            recips[MAX_BOT_RECIPS];
  unsigned long       next_local_advert, next_flood_advert;
};

/* Eén companion (v2.4.0). pub_key = de VOLLEDIGE pubkey (nodig voor het ECDH-
 * geheim bij het versturen van commando's). last_lat/last_lon = de laatst
 * ontvangen locatie (uit een #LOC-DM), NAN als er nog geen is; last_seen = de
 * RTC-tijd (s) van dat rapport. used=false is een vrije ingang. */
/* fall_kind-codes: 0 = geen val-event, 1 = "val" (impact), 2 = "nomotion"
 * (dead-man / geen beweging), 3 = "sos" (handmatige SOS). fall_ts = RTC-tijd (s)
 * van het LAATSTE val-event (0 = nooit). Zo kan MeshManager een NIEUWE val
 * detecteren (ts nam toe) en escaleren. */
#define FALL_KIND_NONE     0
#define FALL_KIND_FALL     1
#define FALL_KIND_NOMOTION 2
#define FALL_KIND_SOS      3

struct Companion {
  uint8_t  pub_key[PUB_KEY_SIZE];
  char     name[24];
  float    last_lat;
  float    last_lon;
  uint32_t last_seen;
  uint32_t fall_ts;        // RTC-s van het laatste val-event; 0 = nooit
  uint8_t  fall_kind;      // FALL_KIND_*
  bool     used;
};

/* Eén hashtag-/publiek kanaal dat de bot meeleest. secret = het gedeelde
 * kanaalgeheim (16 of 32 byte); hash = eerste byte van sha256(secret, secret_len)
 * (het MeshCore group-channel-formaat). used=false is een vrije ingang. */
struct BotChannel {
  char    name[24];
  uint8_t secret[PUB_KEY_SIZE];   // 16- of 32-byte sleutel (rest 0)
  uint8_t secret_len;             // 16 of 32
  uint8_t hash;                   // sha256(secret, secret_len)[0]
  bool    used;
  bool    enabled;                // meelezen/antwoorden aan/uit
  bool    derived;                // true = geen expliciet secret (naam-only add)
  bool    is_public;              // true = de vaste publieke sleutel (het echte Public)
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
  /* severity = MON_SEV_* (uit MonitorStore.h): de ernst-emoji die vooraan de
   * alert-DM komt, zodat een companion (T1000-E) er zijn buzzer-tune op kiest.
   * Standaard 0 = MON_SEV_HIGH (rood). ALLEEN de DM krijgt de emoji-prefix; de
   * room-post blijft schoon. Herstel: de aanroeper geeft MON_SEV_LOW mee. */
  void dispatchAlert(uint8_t mode, uint16_t room_mask, bool high_pri, const char* text,
                     uint8_t severity = 0 /* MON_SEV_HIGH */);

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

  /* ---- IWebNode: room-beheer (web-GUI). Zie IWebNode.h voor het contract. ---- */
  int  webRoomMax() override         { return MAX_ROOMS; }
  int  webRoomActiveCount() override { return _num_active_rooms; }
  bool webRoomActive(int idx) override  { return isRoomActive(idx); }
  const char* webRoomName(int idx) override { return getRoomName(idx); }
  bool webRoomStealth(int idx) override { return (idx >= 0 && idx < MAX_ROOMS) ? rooms[idx].stealth : false; }
  bool webRoomHasGuest(int idx) override { return (idx >= 0 && idx < MAX_ROOMS) ? (rooms[idx].guest_password[0] != 0) : false; }
  int  webRoomPosts(int idx) override   { return (idx >= 0 && idx < MAX_ROOMS) ? (int)rooms[idx].num_posted : 0; }
  bool webRoomPubHex(int idx, char* out, size_t out_len) override;
  bool webRoomJoinUri(int idx, char* out, size_t out_len) override;
  int  webRoomAdd(const char* name) override;
  bool webRoomEdit(int idx, const char* name, const char* pass, const char* guest, int stealth) override;
  bool webRoomDel(int idx) override;
  int  webRoomsBackup(char* out, size_t out_len) override;
  bool webRoomsRestore(const char* json) override;

  /* ---- IWebNode: virtuele sensor-nodes (web-GUI). Symmetrisch met de rooms. ---- */
  int  webSNodeMax() override         { return MAX_SENSOR_NODES; }
  int  webSNodeActiveCount() override { return _num_active_snodes; }
  bool webSNodeActive(int idx) override  { return idx >= 0 && idx < MAX_SENSOR_NODES && snodes[idx].active; }
  const char* webSNodeName(int idx) override { return (idx >= 0 && idx < MAX_SENSOR_NODES) ? snodes[idx].name : ""; }
  bool webSNodeStealth(int idx) override { return (idx >= 0 && idx < MAX_SENSOR_NODES) ? snodes[idx].stealth : false; }
  bool webSNodePubHex(int idx, char* out, size_t out_len) override;
  bool webSNodeJoinUri(int idx, char* out, size_t out_len) override;
  int  webSNodeAdd(const char* name) override;
  bool webSNodeEdit(int idx, const char* name, int stealth) override;
  bool webSNodeDel(int idx) override;

  /* ---- Per-sleutel toegangsgrants (CLI + web). kind = ACL_KIND_ROOM/SNODE. ----
   * aclGrantSet: TOEVOEGEN/wijzigen -- vereist de VOLLEDIGE pubkey (key_len ==
   *   PUB_KEY_SIZE), want het gedeelde geheim wordt eruit berekend. level 1..3.
   *   Retour: 0 ok, -1 ongeldig slot, -2 ongeldige key_len/level, -3 tabel vol.
   * aclGrantDel: VERWIJDEREN -- mag op een prefix (key_len >= 6). Weigert als de
   *   prefix op MEER dan één grant past. Retour: 1 verwijderd, -1 ongeldig slot,
   *   -2 niet gevonden, -3 dubbelzinnig (meerdere treffers). */
  int  aclGrantSet(int kind, int slot, const uint8_t* pubkey, int key_len, uint8_t level);
  int  aclGrantDel(int kind, int slot, const uint8_t* prefix, int key_len);

  /* ---- IWebNode: ACL-weergave/-beheer voor de web-GUI/server ---- */
  int  webAclCount(int kind, int slot) override;
  bool webAclGet(int kind, int slot, int i, char* pub64, size_t out_len, int* level) override;
  int  webAclSet(int kind, int slot, const char* pub_hex, int level) override;
  int  webAclDel(int kind, int slot, const char* prefix_hex) override;
  bool webRoomAdvert(int idx, bool flood) override {
    if (idx < 0 || idx >= MAX_ROOMS || !rooms[idx].active) return false;
    sendRoomAdvertisement(rooms[idx], 0, flood); return true;
  }
  bool webSNodeAdvert(int idx, bool flood) override {
    if (idx < 0 || idx >= MAX_SENSOR_NODES || !snodes[idx].active) return false;
    sendSensorNodeAdvertisement(snodes[idx], 0, flood); return true;
  }

  /* ---- IWebNode: bots (N CHAT/notifier-identiteiten) ----
   * Alle mutaties zijn index-adresseerbaar (i = absoluut slot 0..MAX_BOTS-1). De
   * web-laag lost een `bot=<idx-of-naam>` op via webBotResolve() (leeg -> alert-bot). */
  bool        webBotActive() override { return _bots[0].used; }   // is er een bot? (guard)
  int         webBotSlotMax() override    { return MAX_BOTS; }
  int         webBotAlertIdx() override   { return alertBotIndex(); }
  int         webBotResolve(const char* sel) override { return botResolve(sel); }
  int         webBotRecipMax() override   { return MAX_BOT_RECIPS; }
  int         webBotDiagUrlMax() override { return MAX_DIAG_URL_LEN; }

  bool        webBotSlotUsed(int i) override    { return i >= 0 && i < MAX_BOTS && _bots[i].used; }
  bool        webBotSlotEnabled(int i) override { return i >= 0 && i < MAX_BOTS && _bots[i].used && _bots[i].enabled; }
  const char* webBotSlotName(int i) override    { return (i >= 0 && i < MAX_BOTS) ? _bots[i].name : ""; }
  bool        webBotSlotIsAlert(int i) override { return i >= 0 && i < MAX_BOTS && _bots[i].used && _bots[i].is_alert; }
  bool        webBotSlotPubHex(int i, char* out, size_t out_len) override;
  bool        webBotSlotJoinUri(int i, char* out, size_t out_len) override;
  int         webBotSlotRecipCount(int i) override { return botRecipCount(i); }
  bool        webBotSlotRecipGet(int i, int j, char* pub64, size_t out_len, int* level) override;
  int         webBotSlotRecipSet(int i, const char* pub_hex, int level) override;
  int         webBotSlotRecipDel(int i, const char* prefix_hex) override;
  bool        webBotSlotAdvert(int i, bool flood) override {
    if (i < 0 || i >= MAX_BOTS || !_bots[i].used) return false;
    sendBotAdvertisement(i, 0, flood); return true;
  }
  int         webBotSlotSendTo(int i, const char* pub_hex, const char* text) override;
  int         webBotSlotPost(int i, const char* text) override;
  int         webBotSlotDiagMask(int i) override { return (i >= 0 && i < MAX_BOTS) ? _bots[i].diag_mask : 0; }
  bool        webBotSlotSetDiagMask(int i, int mask) override {
    if (i < 0 || i >= MAX_BOTS || !_bots[i].used) return false;
    setBotDiagMask(i, (uint8_t)(mask & DIAG_ALL)); return true;
  }
  int         webBotSlotDiagUrlMode(int i) override { return (i >= 0 && i < MAX_BOTS) ? _bots[i].diag_url_mode : 0; }
  const char* webBotSlotDiagUrl(int i) override     { return (i >= 0 && i < MAX_BOTS) ? _bots[i].diag_url : ""; }
  bool        webBotSlotSetDiagUrl(int i, int mode, const char* url) override {
    if (i < 0 || i >= MAX_BOTS || !_bots[i].used) return false;
    setBotDiagUrl(i, (uint8_t)mode, url); return true;
  }
  int         webBotSlotDiagUrlBudget(int i, int kind) override { return diagUrlBudget(i, kind); }

  /* Beheer: nieuwe bot (genereert sleutel), hernoemen, aan/uit, wissen, alert-rol. */
  int         webBotAdd(const char* name) override;
  bool        webBotRename(int i, const char* name) override;
  bool        webBotEnable(int i, int en) override;
  bool        webBotDel(int i) override;
  bool        webBotSetAlert(int i) override;

  /* ---- IWebNode: hashtag-/publieke kanalen ---- */
  int  webChannelMax() override   { return channelMax(); }
  int  webChannelCount() override { return channelCount(); }
  bool webChannelGet(int i, char* name, size_t name_len, int* bits, bool* enabled, char* hashhex, bool* derived, bool* is_public) override;
  int  webChannelAdd(const char* name, const char* secret_hex, int enabled) override {
    return channelAdd(name, secret_hex, enabled != 0);
  }
  int  webChannelDel(const char* name) override    { return channelDel(name); }
  int  webChannelToggle(const char* name, int enabled) override { return channelSetEnabled(name, enabled != 0); }

  /* ---- IWebNode: companions (v2.4.0) ---- */
  int  webCompanionMax() override   { return MAX_COMPANIONS; }
  int  webCompanionCount() override { return companionCount(); }
  bool webCompanionGet(int i, char* name, size_t name_len, char* pub64, size_t pub_len,
                       float* lat, float* lon, uint32_t* seen, bool* has_loc,
                       uint32_t* fall_ts, int* fall_kind) override;
  int  webCompanionSet(const char* pub_hex, const char* name) override;
  int  webCompanionDel(const char* prefix_hex) override;

  /* ---- Bots: publieke API (CLI + intern). Alle bewerkingen zijn index-adresseerbaar
   * (b = slot 0..MAX_BOTS-1). ---- */
  bool botActive() const { return _bots[0].used; }
  int  alertBotIndex() const;              // slot van de bot met de alert-rol; -1 als geen
  int  botResolve(const char* sel) const;  // "" / NULL -> alert-bot; anders index of naam; -1 = onbekend
  void setBotDiagMask(int b, uint8_t mask);           // persistent (bij de ontvangerslijst)
  void setBotDiagUrl(int b, uint8_t mode, const char* url);  // url==NULL laat de huidige staan
  /* Max. URL-lengte die INLINE nog past. kind: 0=ping, 1=test, 2=path. */
  int  diagUrlBudget(int b, int kind) const;
  int  botRecipCount(int b) const;
  bool botRecipGetByIdx(int b, int i, uint8_t* pub_out) const;   // pub_out >= PUB_KEY_SIZE
  int  botRecipAdd(int b, const uint8_t* pubkey);                // 0 ok, -2 dup(ok), -3 vol
  int  botRecipDelPrefix(int b, const uint8_t* prefix, int key_len);  // 1 ok, -2 niet gevonden, -3 dubbelzinnig
  /* Staat deze VOLLEDIGE pubkey (actief) in de ontvangerslijst van bot b? Bepaalt of
   * de bot de volledige monitoring/admin-commandoset over DM voor deze afzender uitvoert. */
  bool botRecipHas(int b, const uint8_t* pubkey) const;
  int  botSendTo(int b, const uint8_t* pubkey, const char* text);     // 0 ok, <0 fout
  int  botPost(int b, const char* text);                              // aantal aangeschreven, <0 fout

  /* ---- Hashtag-/publieke kanalen: publieke API (CLI + web) ---- */
  int  channelMax() const { return MAX_CHANNELS; }
  int  channelCount() const;
  /* Op index (over de GEBRUIKTE ingangen). out_name >= 24. Geeft naam, sleutellengte
   * in bits (128/256), enabled, de kanaal-hash en of de sleutel uit de naam is
   * afgeleid. Het SECRET wordt NOOIT teruggegeven (schrijf-alleen). false = geen. */
  bool channelGet(int i, char* out_name, int* out_bits, bool* out_enabled, uint8_t* out_hash, bool* out_derived, bool* out_public) const;
  /* Toevoegen/bijwerken op NAAM. secret_hex leeg/NULL -> naam-only: bij naam
   * "public" (case-insensitief, met/zonder #) de VASTE publieke sleutel; anders een
   * HASHTAG-kanaal (sleutel = eerste 16 byte van sha256(naam)), exact zoals de
   * MeshCore-app. Een expliciete 32/64-hex sleutel wint altijd. 0 ok, -2 ongeldig,
   * -3 vol. */
  int  channelAdd(const char* name, const char* secret_hex, bool enabled);
  int  channelDel(const char* name);                 // 1 ok, -2 niet gevonden
  int  channelSetEnabled(const char* name, bool en); // 1 ok, -2 niet gevonden

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
  /* Hashtag-/publieke kanalen: de bot leest mee en antwoordt op ping/test/path. */
  int  searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel channels[], int max_matches) override;
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel, uint8_t* data, size_t len) override;

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

  /* ---- app-hook: de bot beantwoordt de VOLLEDIGE monitoring/admin-commandoset
   * over DM voor een alert-ontvanger (pubkey in _bot_recips). handleBotDm roept dit
   * aan met VOLLE admin-rechten (main_room.cpp bouwt een tijdelijke ClientInfo). De
   * lengte van 'out' terug (0 = geen herkend commando). Zet async_started=true als
   * er een uitgestelde net-diagnose loopt; de uitslag komt dan later via
   * botAdhocPoll(). Standaard: niets (de sensor-variant kent geen DmCommands). */
  virtual int botCommandReply(const uint8_t* sender_pub, const char* line,
                              char* out, size_t out_len, bool& async_started) {
    (void)sender_pub; (void)line; (void)out; (void)out_len; async_started = false; return 0;
  }
  /* Uitgestelde net-diagnose-uitslag ophalen (na botCommandReply met async). true +
   * tekst in 'out' zodra klaar; anders false. */
  virtual bool botAdhocPoll(char* out, size_t out_len) {
    (void)out; (void)out_len; return false;
  }

#if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() { sensors.setSettingValue("gps", _prefs.gps_enabled ? "1" : "0"); }
#endif

private:
  FILESYSTEM*   _fs;
  RoomSlot      rooms[MAX_ROOMS];
  int           _num_active_rooms;
  int           _active_slot;

  /* Virtuele sensor-nodes (ADV_TYPE_SENSOR). Hergebruiken de RoomSlot-struct maar
   * kennen geen posts. _active_snode >= 0 tijdens onRecvPacket-dispatch als het
   * geadresseerde pakket voor een sensor-node was; anders -1 (dan telt _active_slot,
   * een room). */
  RoomSlot      snodes[MAX_SENSOR_NODES];
  int           _num_active_snodes;
  int           _active_snode;

  /* Persistente per-sleutel toegangsgrants (wachtwoordloos). Zie MAX_ACL_GRANTS. */
  AclGrant      _grants[MAX_ACL_GRANTS];

  /* ---- BOTS: N virtuele CHAT/notifier-identiteiten (v2.5.0) ----
   * Elk CHAT-contact heeft een eigen persistent sleutelpaar (bot #0 in "/bot_id",
   * bot #i in "/bot_id_i"), naam, ontvangerslijst en zend-diagnose. Ze adverteren
   * als ADV_TYPE_CHAT op de gewone advert-timers, zichtbaar in de MeshCore-app als
   * gewone chatcontacten. Bot #0 = de bestaande alert-bot (BE-HSS-DinX-Bot). */
  BotSlot       _bots[MAX_BOTS];

  /* TWEERICHTING. true tijdens onRecvPacket-dispatch als het geadresseerde pakket
   * voor een bot-identiteit was (naast _active_slot/_active_snode). _active_bot is
   * dan het bot-slot dat matchte; de basisklasse ontsleutelt met _bots[_active_bot].id
   * en de inkomende data gaat naar het bot-diagnosepad van die bot. */
  bool          _active_is_bot;
  int           _active_bot;
  /* De afzenders die op de src-hash van een inkomend bot-pakket passen (volledige
   * pubkeys; uit de buurtlijst én de ontvangerslijst). searchPeersByHash vult dit,
   * getPeerSharedSecret/onPeerDataRecv lezen het. MEMBER, niet op de stapel. */
  uint8_t       _bot_match_pub[4][PUB_KEY_SIZE];
  int           _bot_match_n;

  /* Wacht op een uitgestelde net-diagnose-uitslag die via een bot-DM-commando is
   * gestart (botCommandReply meldde async). De uitslag wordt via botAdhocPoll()
   * opgehaald en met botSendTo() naar deze pubkey teruggestuurd. */
  bool          _bot_cmd_wait = false;
  uint8_t       _bot_cmd_wait_pub[PUB_KEY_SIZE];
  int           _bot_cmd_wait_bot = 0;   // welke bot de uitgestelde uitslag terugstuurt

  /* Hashtag-/publieke kanalen die de bot meeleest (zie BotChannel). */
  BotChannel    _channels[MAX_CHANNELS];

  /* Companions (v2.4.0): de bot stuurt hen `!`-commando's, de node ontvangt hun
   * #LOC-locatierapporten. Persistent in /companions.cfg (zie Companion). */
  Companion     _companions[MAX_COMPANIONS];

  /* De actieve slot (room OF sensor-node) tijdens de dispatch. Zo delen room- en
   * sensor-node-verkeer dezelfde login/ACL/telemetrie-code. */
  RoomSlot&     activeSlot()          { return _active_snode >= 0 ? snodes[_active_snode] : rooms[_active_slot]; }
  bool          activeIsSnode() const { return _active_snode >= 0; }
  /* &rooms[idx] of &snodes[idx] als die actief is; anders NULL. */
  RoomSlot*     slotRef(int kind, int idx);
  /* Het grant-niveau (PERM_ACL_*) voor deze pubkey op dit slot, of 0 (geen). */
  uint8_t       grantLookup(int kind, int slot, const uint8_t* pubkey);

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
  /* Schone DM vanaf bot b naar een losse pubkey (niet in een ACL): berekent het
   * gedeelde geheim zelf. Deelt de AlertTask-ACK-boekhouding met sendAlertDM. */
  void          sendBotAlertDM(int b, const uint8_t* pubkey, AlertTask* t);

  /* ---- bots: identiteit, advert, ontvangerslijst, beheer ---- */
  void          loadOrCreateBotIdentity(int b);   // bot #0 -> "/bot_id"; bot #i -> "/bot_id_i"
  void          saveBotRecips(int b);             // bot #0 -> "/bot_recips"; bot #i -> "/bot_recips_i"
  void          loadBotRecips(int b);
  void          saveBotsConfig();                 // "/bots.cfg": actief/rol/naam per slot
  void          loadBotsConfig();
  int           botRecipFindFree(int b) const;
  int           botFindFreeSlot() const;          // eerste ongebruikt slot; -1 = vol

  /* ---- companions (v2.4.0) ---- */
  int           companionCount() const;
  int           companionFindByPub(const uint8_t* pubkey) const;   // -1 = niet gevonden
  int           companionFindFree() const;                         // -1 = vol
  /* Toevoegen/bijwerken: VOLLEDIGE pubkey + naam. Bestaat de pubkey al -> naam
   * bijwerken (locatie blijft). 0 ok, -3 vol. */
  int           companionSet(const uint8_t* pubkey, const char* name);
  int           companionDelPrefix(const uint8_t* prefix, int key_len);  // 1 ok, -2 niet, -3 dubbel
  /* Locatie van een companion bijwerken (uit een #LOC-DM). */
  void          companionUpdateLoc(int idx, float lat, float lon, uint32_t seen);
  /* Val-event van een companion vastleggen (uit een #LOC-DM met val-merkteken). */
  void          companionRecordFall(int idx, uint8_t kind, uint32_t ts);
  void          saveCompanions();
  void          loadCompanions();
  /* Zend-diagnose-achtervoegsel opbouwen binnen `budget` bytes; retour = lengte.
   * `kind`: 0=ping, 1=test, 2=path (bepaalt of het masker dit type toelaat). */
  size_t        buildTxDiag(int b, char* out, size_t cap, size_t budget,
                            const mesh::Packet* packet, int kind) const;
  /* Los uitleg-bericht ("Meer info ...: <url>") als DIAG_URL_SEPARATE aan staat
   * en dit pakket ongescoped was. Retour = lengte, 0 = niets sturen. */
  size_t        buildDiagUrlMsg(int b, char* out, size_t cap,
                                const mesh::Packet* packet, int kind) const;
  mesh::Packet* createBotAdvert(int b);
  void          sendBotAdvertisement(int b, int delay_millis, bool flood);
  void          handleBotCommand(char* args, char* reply);
  void          handleChannelCommand(char* args, char* reply);   // CLI: channel ...
  /* Inkomende DM op de bot-identiteit: het kleine mesh-diagnose-commandoset
   * (ping/path/help). Antwoordt als schone DM VANAF de bot naar de afzender. De
   * antwoordbuffer is static (niet op de loopTask-stapel). */
  void          handleBotDm(int b, mesh::Packet* packet, const uint8_t* sender_pub,
                            const uint8_t* secret, uint8_t* data, size_t len);

  /* ---- kanalen: persistentie + diagnose-antwoord ---- */
  void          loadChannels();
  void          saveChannels();
  int           channelFindByName(const char* name) const;   // -1 = niet gevonden
  void          channelComputeHash(BotChannel& c);           // hash uit secret+len
  /* Een binnengekomen kanaaltekst afhandelen (ping/test/path) en, indien herkend,
   * IN het kanaal antwoorden. Antwoordbuffer static (niet op de loopTask-stapel). */
  void          handleChannelText(mesh::Packet* packet, const mesh::GroupChannel& channel,
                                  const char* text);
  /* Bouwen + IN het kanaal versturen: "<botnaam>: <reply>". */
  void          sendChannelReply(const mesh::GroupChannel& channel, const char* reply,
                                 uint32_t delay_millis = SERVER_RESPONSE_DELAY);

  /* ---- adverts + telemetrie voor sensor-nodes ---- */
  mesh::Packet* createSensorNodeAdvert(RoomSlot& slot);
  void          sendSensorNodeAdvertisement(RoomSlot& slot, int delay_millis, bool flood);

  /* ---- persistentie ---- */
  void          saveRoomIdentity(int idx);
  void          loadOrCreateRoomIdentity(int idx);
  void          saveRoomConfig();
  void          loadRoomConfig();
  void          saveSensorNodeIdentity(int idx);
  void          loadOrCreateSensorNodeIdentity(int idx);
  void          saveSensorNodeConfig();
  void          loadSensorNodeConfig();
  void          saveAclGrants();
  void          loadAclGrants();

  /* ---- CLI ---- */
  void          handleRoomCommand(char* args, char* reply);
  void          handleSensorNodeCommand(char* args, char* reply);
  /* Gedeelde "acl <idx> list|add <pubkey> <niveau>|del <prefix>" voor room + snode. */
  void          handleAclSubcommand(int kind, char* args, char* reply);
  static uint8_t aclLevelFromWord(const char* w);   // read/readwrite/admin -> 1/2/3, 0 = fout

  static bool   saveFilter(ClientInfo* client) { return client->isAdmin(); }
};
