/* ============================================================================
 * main_room.cpp -- entree van de MULTIROOM room-server-variant (env
 * `meshuptime_room`, build-flag ROOM_SERVER_VARIANT).
 *
 * Deelt alle modules met de sensor-variant (MonitorSensors, MonitorStore,
 * WifiTask, WebTask, PushTask, DmCommands, NeighbourList) maar gebruikt RoomMesh
 * i.p.v. SensorMesh. De env `meshuptime` blijft main.cpp + SensorMesh gebruiken;
 * build_src_filter sluit per env de andere main uit.
 *
 * IDENTITEIT BEHOUDEN: room 0 krijgt de BESTAANDE hoofdsleutel uit "_main"
 * (store.load), net als main.cpp. Er wordt niets geformatteerd bij boot en geen
 * nieuwe sleutel gemaakt tenzij "_main" ontbreekt -- de pubkey (48d7aade232b)
 * blijft dus behouden. Extra rooms krijgen hun eigen sleutelpaar (RoomMesh).
 * ==========================================================================*/

#include "RoomMesh.h"
#include "DmCommands.h"

#ifdef WIFI_SSID
  #include "WifiTask.h"
  #include "WebTask.h"
  #include "PushTask.h"
  static WifiTask wifi_task;
  static WebTask  web_task;
  static PushTask push_task;
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

/* Standaard-roomindeling. Room 0 = "Storingen" (alarmen), room 1 = "Telemetrie".
 * Per-sensor herconfigureerbaar in een latere stap (MonitorSensors-config); nu
 * gaan alle alarmen naar "Storingen". */
#define ROOM_STORINGEN   0
#define ROOM_TELEMETRIE  1

/* De brug tussen de sensorlaag en de DM/room-commando's -- identiek aan de bron
 * in main.cpp (de sensor-variant); DmCommands praat alleen met deze bron. */
class MonitorDmSource : public DmDataSource {
public:
  int dmSensorCount() override { return 3 + MonitorSensors::MAX_MONITORS; }

  bool dmSensorAt(int idx, DmSensorInfo& dest) override {
    memset(&dest, 0, sizeof(dest));
    if (idx == 0) {
      dest.channel = MonitorSensors::CH_MAINS;
      strncpy(dest.name, "netvoeding", sizeof(dest.name) - 1);
      strncpy(dest.state, sensors.isMains() ? "aan" : "uit", sizeof(dest.state) - 1);
      snprintf(dest.detail, sizeof(dest.detail), "%.3fV, %us in deze stand",
               sensors.lastVolts(), (unsigned)sensors.secsInPowerState());
      return true;
    }
    if (idx == 1) {
      dest.channel = MonitorSensors::CH_BATTERY;
      strncpy(dest.name, "batterijvoeding", sizeof(dest.name) - 1);
      strncpy(dest.state, sensors.isMains() ? "uit" : "aan", sizeof(dest.state) - 1);
      snprintf(dest.detail, sizeof(dest.detail), "%.3fV", sensors.lastVolts());
      return true;
    }
    if (idx == 2) {
      dest.channel = MonitorSensors::CH_WIFI;
      strncpy(dest.name, "wifi", sizeof(dest.name) - 1);
      strncpy(dest.state, sensors.isWifiOnline() ? "online" : "weg", sizeof(dest.state) - 1);
      return true;
    }
    int slot = idx - 3;
    if (slot < 0 || slot >= MonitorSensors::MAX_MONITORS) return false;
    if (!sensors.monitorUsed(slot)) return false;

    dest.channel = sensors.monitorChannel(slot);
    strncpy(dest.name, sensors.monitorName(slot), sizeof(dest.name) - 1);
    if (sensors.monitorsPaused()) {
      strncpy(dest.state, "pauze", sizeof(dest.state) - 1);
    } else if (!sensors.monitorSeeded(slot)) {
      strncpy(dest.state, "?", sizeof(dest.state) - 1);
    } else {
      strncpy(dest.state, sensors.monitorIsUp(slot) ? "op" : "neer", sizeof(dest.state) - 1);
    }
    snprintf(dest.detail, sizeof(dest.detail), "%s, %ums, %u/%u mislukt",
             sensors.monitorHost(slot), (unsigned)sensors.monitorPingMs(slot),
             (unsigned)sensors.monitorFails(slot), (unsigned)sensors.monitorChecks(slot));
    return true;
  }

  void dmStatusLine(char* dest, size_t max_len) override {
    snprintf(dest, max_len, "voeding %s (%.3fV), wifi %s",
             sensors.isMains() ? "net" : "batterij", sensors.lastVolts(),
             sensors.isWifiOnline() ? "online" : "weg");
  }

  bool        dmIsAck(const uint8_t* d, size_t n) override { return MonitorSensors::isAckText(d, n); }
  uint8_t     dmConfirmAlerts() override { return sensors.confirmAlerts(); }
  const char* dmMonCommand(const char* line) override { return sensors.handleDmMonCommand(line); }
  int         dmAdhocState() override { return (int) sensors.adhocState(); }
  bool        dmAdhocReady() override { return sensors.adhocReady(); }
  const char* dmAdhocResult() override { return sensors.adhocResultText(); }
  void        dmAdhocClear() override { sensors.adhocClear(); }
  const char* dmHelpExtra() override { return MonitorSensors::dmCommandHelp(); }
};
static MonitorDmSource dm_source;

class RoomApp : public RoomMesh {
public:
  RoomApp(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng,
          mesh::RTCClock& rtc, mesh::MeshTables& tables)
    : RoomMesh(board, radio, ms, rng, rtc, tables) { }

  DmCommands dm;

protected:
  /* Kantelstanden voor de alarm-OVERGANGdetectie. Anti-spam: we posten/DM'en
   * ALLEEN op de overgang up->down / down->up, niet elke leesronde. */
  bool st_batt_crit = false, st_batt_low = false;
  bool st_mon_down[MonitorSensors::MAX_MONITORS];
  bool st_mains_down = false, st_wifi_down = false;

  /* Per-sensor route (dm/room/both) + room-set komen uit de MonitorSensors-config
   * (mon.<ch>.alert/rooms en fa.<idx>.mode/rooms), instelbaar via web/serieel/room
   * en bewaard. Op een OVERGANG verdeelt dispatchAlert() volgens die keuze. */
  void edge(bool down_now, bool& was_down, bool high_pri, uint8_t mode, uint16_t rooms,
            const char* down_text, const char* up_text) {
    if (down_now && !was_down) {
      was_down = true;
      dispatchAlert(mode, rooms, high_pri, down_text);
    } else if (!down_now && was_down) {
      was_down = false;
      dispatchAlert(mode, rooms, high_pri, up_text);
    }
  }

  void onSensorDataRead() override {
    float v = (float)board.getBattMilliVolts() / 1000.0f;

    char t1[48];
    snprintf(t1, sizeof(t1), "Batterij kritisch (%.2fV)!", v);
    edge(v < 3.4f, st_batt_crit, true,
         sensors.fixedAlertMode(MON_FA_BATT_CRIT), sensors.fixedRoomsMask(MON_FA_BATT_CRIT),
         t1, "Batterij hersteld");
    char t2[48];
    snprintf(t2, sizeof(t2), "Batterij laag (%.2fV)", v);
    edge(v < 3.6f, st_batt_low, false,
         sensors.fixedAlertMode(MON_FA_BATT_LOW), sensors.fixedRoomsMask(MON_FA_BATT_LOW),
         t2, "Batterij weer op peil");

    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) {
      bool down = sensors.monitorUsed(i) && sensors.monitorSeeded(i) &&
                  !sensors.monitorsPaused() && !sensors.monitorIsUp(i);
      edge(down, st_mon_down[i], false,
           sensors.monitorAlertMode(i), sensors.monitorRoomsMask(i),
           sensors.monitorAlertText(i), sensors.recoverAlertText(i));
    }

    edge(!sensors.isMains(), st_mains_down, false,
         sensors.fixedAlertMode(MON_FA_MAINS), sensors.fixedRoomsMask(MON_FA_MAINS),
         sensors.fixedAlertText(MonitorSensors::FIXED_POWER),
         sensors.fixedRecoverAlertText(MonitorSensors::FIXED_POWER));
    edge(!sensors.isWifiOnline(), st_wifi_down, false,
         sensors.fixedAlertMode(MON_FA_WIFI), sensors.fixedRoomsMask(MON_FA_WIFI),
         sensors.fixedAlertText(MonitorSensors::FIXED_WIFI),
         sensors.fixedRecoverAlertText(MonitorSensors::FIXED_WIFI));
  }

  /* Een room-post herkennen als commando en de antwoordtekst opbouwen. RoomMesh
   * post die tekst dan terug in de room (geknipt). */
  int roomCommandReply(ClientInfo* from, int room_idx, const char* line, char* out, size_t out_len) override {
    if (from == nullptr) return 0;
    return dm.renderReply(*from, room_idx, line, out, out_len);
  }

public:
  void beginApp() {
    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) st_mon_down[i] = false;
  }
};

StdRNG fast_rng;
SimpleMeshTables tables;
RoomApp the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() { while (1) ; }

static char command[160];

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) { halt(); }
  fast_rng.begin(radio_driver.getRngSeed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif

  /* Room 0 = de BESTAANDE hoofdidentiteit. Alleen nieuw genereren als "_main"
   * ontbreekt -- net als in main.cpp. Keys blijven behouden. */
  mesh::LocalIdentity self_id;
  if (!store.load("_main", self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    self_id = radio_new_identity();
    int count = 0;
    while (count < 10 && (self_id.pub_key[0] == 0x00 || self_id.pub_key[0] == 0xFF)) {
      self_id = radio_new_identity(); count++;
    }
    store.save("_main", self_id);
  }
  the_mesh.setRoom0Identity(self_id);
  the_mesh.self_id = self_id;

  Serial.print("Room-server ID (room 0): ");
  mesh::Utils::printHex(Serial, self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();
  the_mesh.begin(fs);
  the_mesh.beginApp();

  the_mesh.dm.begin(&the_mesh, &dm_source);
  the_mesh.dm.setPathHashSize(the_mesh.getNodePrefs()->path_hash_mode + 1);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

#ifdef WIFI_SSID
  {
    char ssid[33], pwd[65];
    if (loadWifiConfig(ssid, sizeof(ssid), pwd, sizeof(pwd))) {
      wifi_task.begin(ssid, pwd);
    } else {
      wifi_task.begin(WIFI_SSID, WIFI_PWD);
    }
    /* De webinterface toont de MeshUptime-branding + MeshCore-versie in de
     * voettekst/statusregel (via het bewaarde _fw). */
    web_task.begin(&wifi_task, MESHUPTIME_BRAND_FULL(FIRMWARE_VERSION));
    /* De webinterface wordt aan de room-server gekoppeld via IWebNode: /cli,
     * /cfg.json, buurtlijst en toegangsbeheer (room 0) werken zoals op de
     * sensor-node. */
    web_task.setAcl(&the_mesh);
    push_task.begin(&wifi_task, &sensors, the_mesh.getSelfPubKey());
    sensors.setEventSink(&push_task);
  }
  #ifdef HAS_MONITOR_SENSORS
    sensors.setWifiTask(&wifi_task);
    web_task.setMonitors(&sensors);
  #endif
#endif

#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < (int)sizeof(command) - 1) {
    char c = Serial.read();
    if (c != '\n') { command[len++] = c; command[len] = 0; }
    Serial.print(c);
  }
  if (len == sizeof(command) - 1) command[sizeof(command) - 1] = '\r';

  if (len > 0 && command[len - 1] == '\r') {
    command[len - 1] = 0;
    char reply[256];
    the_mesh.handleCommand(0, command, reply);
    if (reply[0]) { Serial.print("  -> "); Serial.println(reply); }
    command[0] = 0;
  }

  the_mesh.loop();
  sensors.loop();
#ifdef WIFI_SSID
  wifi_task.loop();
  web_task.loop();
  push_task.loop();
#endif
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
}
