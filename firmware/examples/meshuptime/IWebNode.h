#pragma once

/* ============================================================================
 * IWebNode -- de smalle interface tussen WebTask en "de node".
 *
 * WAAROM DIT BESTAAT
 *
 * WebTask was rechtstreeks aan SensorMesh gekoppeld (SensorMesh* _acl). De
 * room-variant gebruikt geen SensorMesh maar RoomMesh, en die moet dezelfde
 * webinterface (nodebeheer via /cli, /cfg.json, toegangsbeheer, buurtlijst)
 * kunnen aandrijven. In plaats van WebTask twee keer te bouwen of van type te
 * laten wisselen, praat WebTask nu tegen deze interface. SensorMesh EN RoomMesh
 * implementeren hem allebei.
 *
 * KLEIN GEHOUDEN MET OPZET: precies de methoden die WebTask op `_acl` aanriep,
 * niet meer. Elke methode erbij is er een die beide klassen moeten blijven
 * leveren.
 *
 * Namen die anders met de basisklasse zouden botsen (getSelfId, getRole,
 * getRTCClock, handleCommand) hebben hier een EIGEN naam (getSelfPubKey,
 * getRoleName, nowSecs, handleCommandWeb), zodat er geen dubbelzinnigheid is met
 * de gelijknamige, niet-virtuele methoden op mesh::Mesh / CommonCLICallbacks.
 * ==========================================================================*/

#include <Arduino.h>
#include <helpers/ClientACL.h>   // ClientInfo
#include <helpers/CommonCLI.h>   // NodePrefs

class NeighbourList;

class IWebNode {
public:
  virtual ~IWebNode() {}

  /* Nodebeheer: /cli geeft een opdrachtregel door aan de CLI van de node. */
  virtual void        handleCommandWeb(uint32_t sender_timestamp, char* command, char* reply) = 0;

  /* Leeskant voor /cfg.json en de beheerpagina. */
  virtual NodePrefs*  getNodePrefs() = 0;
  virtual const char* getRoleName() = 0;
  virtual const uint8_t* getSelfPubKey() = 0;   // PUB_KEY_SIZE byte
  virtual uint32_t    nowSecs() = 0;            // RTC-tijd in seconden

  /* Toegangsbeheer. */
  virtual bool        getAclStrict() const = 0;
  virtual void        setAclStrict(bool on) = 0;
  virtual int         getAclCount() = 0;
  virtual ClientInfo* getAclEntry(int idx) = 0;
  virtual bool        aclSetPerms(const uint8_t* pubkey, uint8_t perms) = 0;
  virtual bool        aclRemove(const uint8_t* pubkey, int key_len) = 0;
  virtual int         aclCountMatching(const uint8_t* pubkey, int key_len) = 0;

  /* Buurtlijst en het naar-voren-trekken van een sensorleesronde. */
  virtual const NeighbourList& getNeighbours() const = 0;
  virtual void        requestSensorReadNow() = 0;

  /* ---- ROOM-BEHEER (web-GUI) ----------------------------------------------
   *
   * ALLEEN de room-server (RoomMesh) heeft rooms; SensorMesh niet. Daarom staan
   * deze methoden hier NIET als pure virtuals maar met een veilige standaard:
   * webRoomMax()==0 betekent "deze node kent geen rooms", en dan antwoorden de
   * /rooms*-endpoints in WebTask netjes met 'niet ondersteund'. Zo hoeft
   * SensorMesh niets te leveren en blijft de sensor-env (de terugval) ongemoeid.
   *
   * De JOIN-URI en de pubkey-hex worden HIER (in de node) opgebouwd en niet in
   * WebTask: alleen de node kent de room-identiteit (pubkey) en de room-naam. De
   * mutaties lopen via dezelfde room-logica als de CLI (handleRoomCommand), zodat
   * er geen tweede schrijfpad ontstaat. */
  virtual int  webRoomMax()          { return 0; }   // 0 = geen rooms op deze node
  virtual int  webRoomActiveCount()  { return 0; }
  virtual bool webRoomActive(int idx)     { (void)idx; return false; }
  virtual const char* webRoomName(int idx){ (void)idx; return ""; }
  virtual bool webRoomStealth(int idx)    { (void)idx; return false; }
  virtual bool webRoomHasGuest(int idx)   { (void)idx; return false; }  // NOOIT het ww zelf
  virtual int  webRoomPosts(int idx)      { (void)idx; return 0; }
  virtual bool webRoomPubHex(int idx, char* out, size_t out_len)
                                          { (void)idx; (void)out; (void)out_len; return false; }
  virtual bool webRoomJoinUri(int idx, char* out, size_t out_len)
                                          { (void)idx; (void)out; (void)out_len; return false; }
  /* Toevoegen: geeft de nieuwe idx terug, of -1 (vol / geweigerd). */
  virtual int  webRoomAdd(const char* name) { (void)name; return -1; }
  /* Bewerken: name/pass/guest == NULL laat het veld ONgewijzigd; guest=="" wist
   * het gastwachtwoord; stealth <0 = ongewijzigd, 0/1 = uit/aan. */
  virtual bool webRoomEdit(int idx, const char* name, const char* pass,
                           const char* guest, int stealth)
                                          { (void)idx; (void)name; (void)pass;
                                            (void)guest; (void)stealth; return false; }
  virtual bool webRoomDel(int idx)        { (void)idx; return false; }  // room 0 kan niet weg
  /* Backup/restore van de VOLLEDIGE room-config INCL. sleutels. GEVOELIG: alleen
   * achter auth aanroepen, niet loggen. webRoomsBackup geeft de lengte terug (0 =
   * mislukt/te klein). */
  virtual int  webRoomsBackup(char* out, size_t out_len)
                                          { (void)out; (void)out_len; return 0; }
  virtual bool webRoomsRestore(const char* json)
                                          { (void)json; return false; }

  /* ---- VIRTUELE SENSOR-NODES (web-GUI) --------------------------------------
   * Symmetrisch met de room-API. Alleen de room-server (RoomMesh) implementeert
   * dit; SensorMesh laat de standaarden staan (webSNodeMax()==0 -> geen sensor-
   * nodes). De join-URI gebruikt het sensor-contacttype (type=4, MeshCore
   * docs/qr_codes.md). Backup/restore van de sensor-nodes loopt mee in
   * webRoomsBackup()/webRoomsRestore(). */
  virtual int  webSNodeMax()          { return 0; }   // 0 = geen sensor-nodes
  virtual int  webSNodeActiveCount()  { return 0; }
  virtual bool webSNodeActive(int idx)     { (void)idx; return false; }
  virtual const char* webSNodeName(int idx){ (void)idx; return ""; }
  virtual bool webSNodeStealth(int idx)    { (void)idx; return false; }
  virtual bool webSNodePubHex(int idx, char* out, size_t out_len)
                                          { (void)idx; (void)out; (void)out_len; return false; }
  virtual bool webSNodeJoinUri(int idx, char* out, size_t out_len)
                                          { (void)idx; (void)out; (void)out_len; return false; }
  virtual int  webSNodeAdd(const char* name) { (void)name; return -1; }
  virtual bool webSNodeEdit(int idx, const char* name, int stealth)
                                          { (void)idx; (void)name; (void)stealth; return false; }
  virtual bool webSNodeDel(int idx)        { (void)idx; return false; }

  /* ---- PER-SLEUTEL TOEGANGSGRANTS (web-GUI/server) --------------------------
   * kind: 0 = room, 1 = sensor-node (zie ACL_KIND_* in RoomMesh.h). Niveau =
   * 1 read, 2 readwrite, 3 admin. Alleen de room-server implementeert dit; de
   * pubkey is publiek (nooit een geheim/gedeelde sleutel teruggeven). */
  virtual int  webAclCount(int kind, int slot) { (void)kind; (void)slot; return 0; }
  virtual bool webAclGet(int kind, int slot, int i, char* pub64, size_t out_len, int* level)
                                          { (void)kind; (void)slot; (void)i; (void)pub64; (void)out_len; (void)level; return false; }
  virtual int  webAclSet(int kind, int slot, const char* pub_hex, int level)
                                          { (void)kind; (void)slot; (void)pub_hex; (void)level; return -1; }
  virtual int  webAclDel(int kind, int slot, const char* prefix_hex)
                                          { (void)kind; (void)slot; (void)prefix_hex; return -1; }

  /* Handmatig een advert sturen voor een room/sensor-node. flood = geflood
   * (multi-hop) i.p.v. zero-hop lokaal. */
  virtual bool webRoomAdvert(int idx, bool flood)  { (void)idx; (void)flood; return false; }
  virtual bool webSNodeAdvert(int idx, bool flood) { (void)idx; (void)flood; return false; }

  /* ---- BOT: virtuele CHAT/notifier-identiteit + DM-ontvangerslijst ----------
   * Eén CHAT-contact (ADV_TYPE_CHAT / type=1) dat SCHONE DM's stuurt: voor
   * flash-meldingen en voor de per-sensor dm/both-alerts. Zo tonen die DM's als
   * een gewoon chatcontact in de MeshCore-app i.p.v. rommelig vanaf een room.
   * Alleen de room-server (RoomMesh) implementeert dit; SensorMesh laat de
   * standaarden staan (webBotActive()==false). De pubkey is publiek; de
   * ontvangerslijst bevat ALLEEN publieke sleutels (nooit een geheim). */
  /* v2.5.0: N bot-identiteiten. `i` is een absoluut slot (0..webBotSlotMax()-1);
   * webBotResolve() zet een `bot=<idx-of-naam>` uit de web-laag om naar een slot
   * (leeg/NULL -> de alert-bot). De pubkey is publiek; ontvangerslijsten bevatten
   * ALLEEN publieke sleutels (nooit een geheim). */
  virtual bool webBotActive()               { return false; }   // is er minstens één bot?
  virtual int  webBotSlotMax()              { return 0; }        // MAX_BOTS
  virtual int  webBotAlertIdx()             { return -1; }       // slot met de alert-rol
  virtual int  webBotResolve(const char* sel) { (void)sel; return -1; }
  virtual int  webBotRecipMax()             { return 0; }
  virtual int  webBotDiagUrlMax()           { return 0; }

  virtual bool webBotSlotUsed(int i)        { (void)i; return false; }
  virtual bool webBotSlotEnabled(int i)     { (void)i; return false; }
  virtual const char* webBotSlotName(int i) { (void)i; return ""; }
  virtual bool webBotSlotIsAlert(int i)     { (void)i; return false; }
  virtual bool webBotSlotPubHex(int i, char* out, size_t out_len)  { (void)i; (void)out; (void)out_len; return false; }
  virtual bool webBotSlotJoinUri(int i, char* out, size_t out_len) { (void)i; (void)out; (void)out_len; return false; }
  virtual int  webBotSlotRecipCount(int i)  { (void)i; return 0; }
  virtual bool webBotSlotRecipGet(int i, int j, char* pub64, size_t out_len, int* level)
                                            { (void)i; (void)j; (void)pub64; (void)out_len; (void)level; return false; }
  /* Toevoegen/wijzigen: VOLLEDIGE pubkey (64 hex) vereist. Retour 0 ok, <0 fout. */
  virtual int  webBotSlotRecipSet(int i, const char* pub_hex, int level) { (void)i; (void)pub_hex; (void)level; return -1; }
  /* Verwijderen: prefix >= 12 hex. Retour 1 ok, <0 fout. */
  virtual int  webBotSlotRecipDel(int i, const char* prefix_hex)  { (void)i; (void)prefix_hex; return -1; }
  virtual bool webBotSlotAdvert(int i, bool flood)     { (void)i; (void)flood; return false; }
  /* Ad-hoc schone DM vanaf bot i naar één pubkey. Retour 0 ok. */
  virtual int  webBotSlotSendTo(int i, const char* pub_hex, const char* text) { (void)i; (void)pub_hex; (void)text; return -1; }
  /* DM de HELE ontvangerslijst van bot i. Retour = aantal, <0 fout. */
  virtual int  webBotSlotPost(int i, const char* text) { (void)i; (void)text; return -1; }
  /* Zend-diagnose per bot. Masker: bit0 ping, bit1 test, bit2 path. */
  virtual int  webBotSlotDiagMask(int i)          { (void)i; return 0; }
  virtual bool webBotSlotSetDiagMask(int i, int m) { (void)i; (void)m; return false; }
  virtual int  webBotSlotDiagUrlMode(int i)    { (void)i; return 0; }
  virtual const char* webBotSlotDiagUrl(int i) { (void)i; return ""; }
  virtual bool webBotSlotSetDiagUrl(int i, int mode, const char* url) { (void)i; (void)mode; (void)url; return false; }
  virtual int  webBotSlotDiagUrlBudget(int i, int kind) { (void)i; (void)kind; return 0; }

  /* Beheer. webBotAdd genereert een nieuw sleutelpaar; retour = nieuw slot of <0. */
  virtual int  webBotAdd(const char* name)   { (void)name; return -1; }
  virtual bool webBotRename(int i, const char* name) { (void)i; (void)name; return false; }
  virtual bool webBotEnable(int i, int en)   { (void)i; (void)en; return false; }
  virtual bool webBotDel(int i)              { (void)i; return false; }
  virtual bool webBotSetAlert(int i)         { (void)i; return false; }

  /* ---- HASHTAG-/PUBLIEKE KANALEN (web-GUI) ----------------------------------
   * De bot leest de ingeschakelde kanalen mee en antwoordt IN het kanaal op
   * ping/test/path. Alleen de room-server implementeert dit. Het kanaal-SECRET
   * wordt nooit teruggegeven (schrijf-alleen, zoals een wachtwoord). */
  virtual int  webChannelMax()   { return 0; }
  virtual int  webChannelCount() { return 0; }
  virtual bool webChannelGet(int i, char* name, size_t name_len, int* bits, bool* enabled, char* hashhex, bool* derived, bool* is_public)
                                 { (void)i; (void)name; (void)name_len; (void)bits; (void)enabled; (void)hashhex; (void)derived; (void)is_public; return false; }
  /* Toevoegen/bijwerken op naam; secret_hex leeg -> hashtag (sleutel uit naam),
   * anders 32 of 64 hex. 0 ok, <0 fout. */
  virtual int  webChannelAdd(const char* name, const char* secret_hex, int enabled)
                                 { (void)name; (void)secret_hex; (void)enabled; return -1; }
  virtual int  webChannelDel(const char* name)          { (void)name; return -1; }
  virtual int  webChannelToggle(const char* name, int enabled) { (void)name; (void)enabled; return -1; }

  /* ---- COMPANIONS (web-GUI, v2.4.0) -----------------------------------------
   * Companion-apparaten (T1000-E e.d.) die de bot aanstuurt en waarvan de node
   * #LOC-locatierapporten ontvangt. Alleen de room-server (RoomMesh) implementeert
   * dit; SensorMesh laat de standaarden staan (webCompanionMax()==0). De pubkey is
   * publiek; commando's gaan via het bestaande botSendTo-pad (/bot/sendto). */
  virtual int  webCompanionMax()   { return 0; }   // 0 = geen companions op deze node
  virtual int  webCompanionCount() { return 0; }
  /* Leest companion i: naam, pubkey (64 hex), lat/lon (NAN als onbekend), seen (RTC
   * s, 0 = nooit). has_loc=true als er een geldige locatie is. fall_ts = RTC-s van
   * het laatste val-event (0 = geen), fall_kind = FALL_KIND_* (0 = geen). */
  virtual bool webCompanionGet(int i, char* name, size_t name_len, char* pub64,
                               size_t pub_len, float* lat, float* lon,
                               uint32_t* seen, bool* has_loc,
                               uint32_t* fall_ts, int* fall_kind)
                               { (void)i; (void)name; (void)name_len; (void)pub64;
                                 (void)pub_len; (void)lat; (void)lon; (void)seen;
                                 (void)has_loc; (void)fall_ts; (void)fall_kind; return false; }
  /* Toevoegen/bijwerken op VOLLEDIGE pubkey (64 hex) + naam. 0 ok, <0 fout. */
  virtual int  webCompanionSet(const char* pub_hex, const char* name)
                               { (void)pub_hex; (void)name; return -1; }
  /* Verwijderen op prefix (>= 12 hex). 1 ok, <0 fout. */
  virtual int  webCompanionDel(const char* prefix_hex) { (void)prefix_hex; return -1; }
};
