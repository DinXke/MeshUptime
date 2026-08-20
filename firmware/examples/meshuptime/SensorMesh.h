#pragma once

#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "TimeSeriesData.h"
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

#define PERM_RESERVED1         (1 << 2)
#define PERM_RESERVED2         (1 << 3)
#define PERM_RESERVED3         (1 << 4)
#define PERM_RESERVED4         (1 << 5)
#define PERM_RECV_ALERTS_LO    (1 << 6)   // low priority alerts
#define PERM_RECV_ALERTS_HI    (1 << 7)   // high priority alerts

#ifndef FIRMWARE_BUILD_DATE
  #define FIRMWARE_BUILD_DATE   "9 Aug 2026"
#endif

#ifndef FIRMWARE_VERSION
  #define FIRMWARE_VERSION   "v1.17.0"
#endif

#define FIRMWARE_ROLE "sensor"

#define MAX_SEARCH_RESULTS      8
#define MAX_CONCURRENT_ALERTS   4

class SensorMesh : public mesh::Mesh, public CommonCLICallbacks {
public:
  SensorMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables);
  void begin(FILESYSTEM* fs);
  void loop();
  void handleCommand(uint32_t sender_timestamp, char* command, char* reply);

  // CommonCLI callbacks
  const char* getFirmwareVer() override { return FIRMWARE_VERSION; }
  const char* getBuildDate() override { return FIRMWARE_BUILD_DATE; }
  const char* getRole() override { return FIRMWARE_ROLE; }
  const char* getNodeName() { return _prefs.node_name; }
  NodePrefs* getNodePrefs() { return &_prefs; }
  void savePrefs() override { _cli.savePrefs(_fs); }
  bool formatFileSystem() override;
  void sendSelfAdvertisement(int delay_millis, bool flood) override;
  void updateAdvertTimer() override;
  void updateFloodAdvertTimer() override;
  void setLoggingOn(bool enable) override {  }
  void eraseLogFile() override { }
  void dumpLogFile() override { }
  void setTxPower(int8_t power_dbm) override;
  /* Was "not supported", en dat was zonde: de gegevens BESTAAN. De buurtring
   * die voor het toegangsbeheer gebouwd is (NeighbourList, gevuld uit
   * onAdvertRecv) houdt precies bij wie er langskomt. Alleen de verbinding
   * ontbrak, dus een knop die er wel was gaf een antwoord dat niets zei.
   *
   * De uitvoer is HARD BEGRENSD en dat is geen zuinigheid: CommonCLI schrijft
   * met sprintf zonder grens, de seriele buffer is 256 byte en de mesh-CLI heeft
   * 257 (temp[262] met reply op offset 5). Twaalf buren van ~34 tekens zijn
   * 400 byte, dus zonder rem is dit een stack-overflow -- precies de fout die
   * 'sensor list' hier eerder veroorzaakte. Wat niet past wordt geteld en
   * gemeld, in plaats van stil weggelaten. */
  void formatNeighborsReply(char *reply) override {
    const int LIMIT = 200;              // ruim onder de kleinste buffer (256)
    int n = neighbours.getNumEntries();
    if (n == 0) {
      strcpy(reply, "nog geen adverts gehoord");
      return;
    }
    uint32_t now = getRTCClock()->getCurrentTime();
    int p = snprintf(reply, LIMIT, "%d gehoord:", n);
    int shown = 0;
    for (int i = 0; i < n; i++) {
      const NeighbourEntry* e = neighbours.getEntryByIdx(i);
      if (e == NULL) continue;
      char hex[9];
      mesh::Utils::toHex(hex, e->pub_key, 4);
      /* Naam kan leeg zijn als het advert er geen droeg; dan alleen de prefix,
       * want een lege plek in een lijst leest als een fout. */
      int w = snprintf(reply + p, LIMIT - p, " %s%s%s %d.%dq %uh %us;",
                       hex, e->name[0] ? "/" : "", e->name,
                       e->snr4 / 4, (e->snr4 < 0 ? -e->snr4 : e->snr4) % 4 * 25,
                       (unsigned) e->hops,
                       (unsigned) (now > e->heard_at ? now - e->heard_at : 0));
      if (w < 0 || p + w >= LIMIT - 18) break;   // 18 = ruimte voor de slotregel
      p += w; shown++;
    }
    if (shown < n) snprintf(reply + p, LIMIT - p, " (+%d niet vermeld)", n - shown);
  }
  void formatStatsReply(char *reply) override;
  void formatRadioStatsReply(char *reply) override;
  void formatPacketStatsReply(char *reply) override;
  mesh::LocalIdentity& getSelfId() override { return self_id; }
  void saveIdentity(const mesh::LocalIdentity& new_id) override;
  void clearStats() override { }
  void applyTempRadioParams(float freq, float bw, uint8_t sf, uint8_t cr, int timeout_mins) override;

  float getTelemValue(uint8_t channel, uint8_t type);

  /* ---------------------- toegangsbeheer, voor de webinterface -------------
   *
   * WAAROM DIT HIER STAAT EN NIET IN WebTask: de toegangslijst en het slot dat
   * hem afdwingt horen bij de mesh en niet bij de webserver. De webinterface is
   * één van de bedieningswegen (de andere zijn 'setperm' en 'set acl.strict'
   * over serieel of over een DM); zou het beheer in WebTask zitten, dan had een
   * node zonder wifi geen toegangsbeheer. Deze methoden zijn daarom publiek en
   * WebTask krijgt alleen een pointer.
   *
   * HET SLOT (acl.strict). Staat hij AAN, dan antwoordt handleRequest op een
   * telemetrieverzoek alleen aan een afzender met minstens leesrecht. Staat hij
   * UIT, dan blijft het gedrag van upstream: iedereen op het mesh mag alles
   * uitlezen. UIT is de standaard, met opzet -- bij het flashen mag een werkende
   * opstelling zichzelf niet buitensluiten. De webinterface zegt in dat geval
   * met zoveel woorden dat de deur open staat.
   */
  bool getAclStrict() const { return acl_strict != 0; }
  void setAclStrict(bool on);

  int  getAclCount() { return acl.getNumClients(); }
  ClientInfo* getAclEntry(int idx) { return acl.getClientByIdx(idx); }

  /* Rechten van één ingang zetten. perms == 0 verwijdert de ingang.
   *
   * Een VOLLEDIGE sleutel is verplicht bij toevoegen of wijzigen, en dat is geen
   * strengheid maar rekenkunde: de gedeelde sleutel wordt uit de volle publieke
   * sleutel berekend (calcSharedSecret), dus met een prefix kan deze node
   * eenvoudig niet met de tegenpartij praten. Bij VERWIJDEREN mag een prefix
   * wel -- zie aclRemove().
   */
  bool aclSetPerms(const uint8_t* pubkey, uint8_t perms);

  /* Verwijderen mag op een prefix van minstens 'min_len' byte, omdat de lijst
   * die MeshCore zelf over het mesh teruggeeft (REQ_TYPE_GET_ACCESS_LIST) ook
   * maar 6 byte per ingang draagt. Geeft false als de prefix op MEER dan één
   * ingang past: stil de verkeerde ingang wissen is de fout die je pas merkt
   * als de juiste node niets meer krijgt. */
  bool aclRemove(const uint8_t* pubkey, int key_len);
  int  aclCountMatching(const uint8_t* pubkey, int key_len);

  const NeighbourList& getNeighbours() const { return neighbours; }

protected:
  // current telemetry data queries
  float getVoltage(uint8_t channel) { return getTelemValue(channel, LPP_VOLTAGE); }
  float getCurrent(uint8_t channel) { return getTelemValue(channel, LPP_CURRENT); }
  float getPower(uint8_t channel) { return getTelemValue(channel, LPP_POWER); }
  float getTemperature(uint8_t channel) { return getTelemValue(channel, LPP_TEMPERATURE); }
  float getRelativeHumidity(uint8_t channel) { return getTelemValue(channel, LPP_RELATIVE_HUMIDITY); }
  float getBarometricPressure(uint8_t channel) { return getTelemValue(channel, LPP_BAROMETRIC_PRESSURE); }
  float getAltitude(uint8_t channel) { return getTelemValue(channel, LPP_ALTITUDE); }
  bool  getGPS(uint8_t channel, float& lat, float& lon, float& alt);

  // alerts
  enum AlertPriority { LOW_PRI_ALERT, HIGH_PRI_ALERT };

  struct Trigger {
    uint32_t timestamp;
    AlertPriority pri;
    uint32_t expected_acks[4];
    int8_t   curr_contact_idx;
    uint8_t  attempt;
    unsigned long send_expiry;
    char text[MAX_PACKET_PAYLOAD];

    Trigger() { text[0] = 0; }
    bool isTriggered() const { return text[0] != 0; }
  };
  void alertIf(bool condition, Trigger& t, AlertPriority pri, const char* text);

  virtual void onSensorDataRead() = 0;   // for app to implement
  virtual int querySeriesData(uint32_t start_secs_ago, uint32_t end_secs_ago, MinMaxAvg dest[], int max_num) = 0;  // for app to implement
  virtual bool handleCustomCommand(uint32_t sender_timestamp, char* command, char* reply) { return false; }

  // Mesh overrides
  float getAirtimeBudgetFactor() const override;
  bool allowPacketForward(const mesh::Packet* packet) override;
  int calcRxDelay(float score, uint32_t air_time) const override;
  uint32_t getRetransmitDelay(const mesh::Packet* packet) override;
  uint32_t getDirectRetransmitDelay(const mesh::Packet* packet) override;
  int getInterferenceThreshold() const override;
  bool getCADEnabled() const override;
  int getAGCResetInterval() const override;
  void onAnonDataRecv(mesh::Packet* packet, const uint8_t* secret, const mesh::Identity& sender, uint8_t* data, size_t len) override;
  void onAdvertRecv(mesh::Packet* packet, const mesh::Identity& id, uint32_t timestamp, const uint8_t* app_data, size_t app_data_len) override;
  int searchPeersByHash(const uint8_t* hash) override;
  void getPeerSharedSecret(uint8_t* dest_secret, int peer_idx) override;
  void onPeerDataRecv(mesh::Packet* packet, uint8_t type, int sender_idx, const uint8_t* secret, uint8_t* data, size_t len) override;
  bool onPeerPathRecv(mesh::Packet* packet, int sender_idx, const uint8_t* secret, uint8_t* path, uint8_t path_len, uint8_t extra_type, uint8_t* extra, uint8_t extra_len) override;
  void onControlDataRecv(mesh::Packet* packet) override;
  void onAckRecv(mesh::Packet* packet, uint32_t ack_crc) override;

  /* Zonder deze override gaf 'region save' altijd 'Err - save failed'.
   *
   * CommonCLI roept _callbacks->saveRegions() aan, en de basisklasse heeft daar
   * alleen een stub die false teruggeeft. SensorMesh HAD al een region_map en
   * laadde die ook (region_map.load(_fs) in begin()), maar kon hem niet
   * wegschrijven -- dus een gewijzigde regiokaart was na een herstart weg. Dit is
   * dezelfde eenregelaar die simple_repeater al had (MyMesh.cpp:1131).
   *
   * Let op wat er WEL werkte: 'region home' en 'region default' staan in
   * NodePrefs, en savePrefs() loopt in CommonCLI vóór saveRegions(). Die twee
   * bleven dus bewaard terwijl de opdracht een fout meldde -- een foutmelding
   * die maar de helft van de waarheid vertelde.
   *
   * Nog niet geïmplementeerd: startRegionsLoad(), waar 'region load' op wacht.
   * Die vraagt een tijdelijke kaart en een laadstapel (zie MyMesh.cpp:1124) en
   * hoort bij het INLEZEN van een regioboom, niet bij bewaren. Blijft dus stil
   * niets doen; dat is een apart gat en het staat hier zodat het vindbaar is. */
  bool saveRegions() override { return region_map.save(_fs); }

  /* Zonder deze override deed 'region default <naam>' niets aan de UITGAANDE
   * pakketten.
   *
   * default_scope is de transportsleutel die bij het verzenden gebruikt wordt, en
   * die werd alleen in begin() berekend (zie de twee getTransportKeysFor-aanroepen
   * daar). CommonCLI meldt een gewijzigde standaardregio via deze callback, maar
   * de basisklasse heeft er een 'no op' voor -- dus de regiokaart klopte wel en
   * de sleutel niet, en de node zond ongescoped verder tot de volgende herstart.
   *
   * Zo zag het eruit van buiten: waarnemers als CoreScope toonden onze node zonder
   * scope terwijl elke andere node #be had. Geen foutmelding, geen aanwijzing --
   * alleen een leeg vakje in andermans lijst. Dat is precies de foutklasse waar
   * dit project vol van staat: een instelling die geaccepteerd wordt en niet
   * doorwerkt.
   *
   * Dit is dezelfde drieregelaar die simple_repeater al had (MyMesh.cpp:1135). */
  void onDefaultRegionChanged(const RegionEntry* r) override {
    if (r) {
      region_map.getTransportKeysFor(*r, &default_scope, 1);
    } else {
      memset(default_scope.key, 0, sizeof(default_scope.key));
    }
  }
  virtual bool handleIncomingMsg(ClientInfo& from, uint32_t timestamp, uint8_t* data, uint8_t flags, size_t len);
  void sendAckTo(const ClientInfo& dest, uint32_t ack_hash, uint8_t path_hash_size=1);
private:
  FILESYSTEM* _fs;
  unsigned long next_local_advert, next_flood_advert;
  NodePrefs _prefs;
  ClientACL  acl;
  CommonCLI _cli;
  /* Het slot. NIET in NodePrefs: die struct staat in src/helpers/CommonCLI.h en
   * is van upstream; een veld erbij zetten raakt elke andere rol en het formaat
   * van /new_prefs. Eén byte in een eigen bestandje is hier het goedkoopste
   * eerlijke antwoord. */
  uint8_t   acl_strict;
  NeighbourList neighbours;
  uint8_t reply_data[MAX_PACKET_PAYLOAD];
  unsigned long dirty_contacts_expiry;
  CayenneLPP telemetry;
  TransportKeyStore key_store;
  RegionMap region_map;
  TransportKey default_scope;
  uint32_t last_read_time;
  int matching_peer_indexes[MAX_SEARCH_RESULTS];
  int num_alert_tasks;
  Trigger* alert_tasks[MAX_CONCURRENT_ALERTS];
  unsigned long set_radio_at, revert_radio_at;
  float pending_freq;
  float pending_bw;
  uint8_t pending_sf;
  uint8_t pending_cr;

  void loadAclStrict();
  void saveAclStrict();

  uint8_t handleLoginReq(const mesh::Identity& sender, const uint8_t* secret, uint32_t sender_timestamp, const uint8_t* data, bool is_flood);
  uint8_t handleRequest(uint8_t perms, uint32_t sender_timestamp, uint8_t req_type, uint8_t* payload, size_t payload_len);
  mesh::Packet* createSelfAdvert();

  void sendAlert(const ClientInfo* c, Trigger* t);

  #if ENV_INCLUDE_GPS == 1
  void applyGpsPrefs() {
    sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
  }
#endif
};
