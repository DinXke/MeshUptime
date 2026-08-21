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
#if defined(ESP32)
  #include <WiFi.h>
#endif

/* Uitgestelde herstart (room/DM-commando 'reboot'): niet meteen, zodat het
 * antwoord nog verstuurd kan worden. loop() voert hem uit. */
static unsigned long g_reboot_at = 0;

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
  const char* dmHelpExtra() override {
    static char h[480];
    snprintf(h, sizeof(h),
      "%s || diagnose(read): dns <host>, ping <host> [n], port <host> <poort>, "
      "http <url>, scan, traceroute <host>, neighbors/nb, wifi, sys, history <s> | "
      "bedien(readwrite): checknow <s>, mute <s> [sec], unmute <s>, snooze [sec], "
      "test | beheer(admin): sendto <pubkey> <msg>, reboot, ntp",
      MonitorSensors::dmCommandHelp());
    return h;
  }

  /* NODE-/NETWERK-/BEDIEN-COMMANDO'S. Out-of-line gedefinieerd (na the_mesh). */
  int dmNodeCommand(const char* line, uint8_t role, char* out, size_t out_len) override;
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
            const char* down_text, const char* up_text, bool muted = false) {
    /* GEMUTE/GESNOOZED: de kantelaar BEVRIEZEN -- geen alarm EN was_down niet
     * bijwerken. Zo blijft een openstaande overgang bewaard en vuurt hij alsnog
     * zodra de mute voorbij is (als de sensor dan nog neer staat). */
    if (muted) return;
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
         t1, "Batterij hersteld", sensors.isSnoozed());
    char t2[48];
    snprintf(t2, sizeof(t2), "Batterij laag (%.2fV)", v);
    edge(v < 3.6f, st_batt_low, false,
         sensors.fixedAlertMode(MON_FA_BATT_LOW), sensors.fixedRoomsMask(MON_FA_BATT_LOW),
         t2, "Batterij weer op peil", sensors.isSnoozed());

    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) {
      bool down = sensors.monitorUsed(i) && sensors.monitorSeeded(i) &&
                  !sensors.monitorsPaused() && !sensors.monitorIsUp(i);
      edge(down, st_mon_down[i], false,
           sensors.monitorAlertMode(i), sensors.monitorRoomsMask(i),
           sensors.monitorAlertText(i), sensors.recoverAlertText(i),
           sensors.isMuted(i));
    }

    edge(!sensors.isMains(), st_mains_down, false,
         sensors.fixedAlertMode(MON_FA_MAINS), sensors.fixedRoomsMask(MON_FA_MAINS),
         sensors.fixedAlertText(MonitorSensors::FIXED_POWER),
         sensors.fixedRecoverAlertText(MonitorSensors::FIXED_POWER), sensors.isSnoozed());
    edge(!sensors.isWifiOnline(), st_wifi_down, false,
         sensors.fixedAlertMode(MON_FA_WIFI), sensors.fixedRoomsMask(MON_FA_WIFI),
         sensors.fixedAlertText(MonitorSensors::FIXED_WIFI),
         sensors.fixedRecoverAlertText(MonitorSensors::FIXED_WIFI), sensors.isSnoozed());
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

/* De node-/netwerk-/bedien-commando's. Werkt zowel over DM als in een room
 * (DmCommands roept dit vanuit beide paden aan). role = ACL-rol van de afzender
 * (1 read, 2 readwrite, 3 admin). Retour: lengte in 'out', of 0 = geen node-cmd. */
int MonitorDmSource::dmNodeCommand(const char* line, uint8_t role, char* out, size_t out_len) {
  const char* a = line;
  while (*a == ' ') a++;
  char verb[16]; int vi = 0;
  while (a[vi] && a[vi] != ' ' && vi < 15) { verb[vi] = a[vi]; vi++; }
  verb[vi] = 0;
  const char* arg = a + vi;
  while (*arg == ' ') arg++;

  #define ND_NEED(lvl) do { if (role < (lvl)) { \
      snprintf(out, out_len, "geen rechten: '%s' vereist %s", verb, (lvl)==3?"admin":"readwrite"); \
      return (int)strlen(out); } } while (0)

  /* ---------- READ / diagnose (elk room-lid) ---------- */
  if (!strcasecmp(verb, "neighbors") || !strcasecmp(verb, "nb")) {
    const NeighbourList& nb = the_mesh.getNeighbours();
    int cnt = nb.getNumEntries();
    if (cnt == 0) { snprintf(out, out_len, "geen mesh-buren gehoord"); return (int)strlen(out); }
    uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
    int n = snprintf(out, out_len, "buren (%d):", cnt);
    for (int i = 0; i < cnt && (size_t)n < out_len - 44; i++) {
      const NeighbourEntry* e = nb.getEntryByIdx(i);
      long age = (long)now - (long)e->heard_at; if (age < 0) age = 0;
      n += snprintf(out + n, out_len - n, " %s(snr%.1f,%uh,%lds)",
                    e->name[0] ? e->name : "?", e->snr4 / 4.0f, (unsigned)e->hops, age);
    }
    return (int)strlen(out);
  }
#if defined(ESP32)
  if (!strcasecmp(verb, "wifi")) {
    if (WiFi.status() == WL_CONNECTED) {
      snprintf(out, out_len, "wifi: %s | ip %s | gw %s | rssi %d dBm",
               WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
               WiFi.gatewayIP().toString().c_str(), (int)WiFi.RSSI());
    } else {
      snprintf(out, out_len, "wifi: niet verbonden");
    }
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "dns")) {
    if (!*arg) { snprintf(out, out_len, "gebruik: dns <host>"); return (int)strlen(out); }
    IPAddress ip;
    if (WiFi.hostByName(arg, ip)) snprintf(out, out_len, "%s -> %s", arg, ip.toString().c_str());
    else snprintf(out, out_len, "dns: %s niet opgelost", arg);
    return (int)strlen(out);
  }
#endif
  if (!strcasecmp(verb, "sys") || !strcasecmp(verb, "health")) {
    unsigned long up = millis() / 1000UL;
    float t = board.getMCUTemperature();
    uint32_t rt = the_mesh.getRTCClock()->getCurrentTime();
    snprintf(out, out_len,
             "heap %uB, up %lud%luh%lum, mcu %.1fC, batt %.3fV, epoch %lu, %s",
             (unsigned)ESP.getFreeHeap(), up / 86400, (up % 86400) / 3600, (up % 3600) / 60,
             isnan(t) ? 0.0f : t, board.getBattMilliVolts() / 1000.0f, (unsigned long)rt,
             MESHUPTIME_BRAND_FULL(FIRMWARE_VERSION));
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "history")) {
    if (!*arg) { snprintf(out, out_len, "gebruik: history <sensor>"); return (int)strlen(out); }
    int slot = sensors.findByNameOrChannel(arg);
    if (slot < 0 || !sensors.monitorUsed(slot)) { snprintf(out, out_len, "onbekende sensor: %s", arg); return (int)strlen(out); }
    const char* st = !sensors.monitorSeeded(slot) ? "?" : (sensors.monitorIsUp(slot) ? "op" : "neer");
    unsigned long ms = sensors.muteSecsLeft(slot);
    snprintf(out, out_len, "%s (kan.%u): nu %s, %lums, %lu/%lu mislukt%s",
             sensors.monitorName(slot), (unsigned)sensors.monitorChannel(slot), st,
             (unsigned long)sensors.monitorPingMs(slot), (unsigned long)sensors.monitorFails(slot),
             (unsigned long)sensors.monitorChecks(slot),
             ms ? " [gemute]" : "");
    return (int)strlen(out);
  }

  /* ---------- ASYNC NETWERK-DIAGNOSES (read; uitslag volgt zo) ---------- */
  if (!strcasecmp(verb, "port")) {
    char host[64]; int hi = 0; const char* q = arg;
    while (*q && *q != ' ' && hi < 63) host[hi++] = *q++; host[hi] = 0;
    while (*q == ' ') q++;
    int pt = atoi(q);
    if (!host[0] || pt <= 0 || pt > 65535) { snprintf(out, out_len, "gebruik: port <host> <poort>"); return (int)strlen(out); }
    MonitorSensors::SimResult r = sensors.startNetDiag(MonitorSensors::NET_PORT, host, (uint16_t)pt, nullptr);
    if (r == MonitorSensors::SIM_OK) snprintf(out, out_len, "port-check %s:%d gestart; uitslag volgt zo", host, pt);
    else if (r == MonitorSensors::SIM_ERR_BUSY) snprintf(out, out_len, "netwerk-diagnose bezig, probeer zo opnieuw");
    else snprintf(out, out_len, "ongeldig adres");
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "http")) {
    if (!*arg) { snprintf(out, out_len, "gebruik: http <url>  (bv http://host[:poort]/pad)"); return (int)strlen(out); }
    const char* u = arg;
    if (!strncasecmp(u, "http://", 7)) u += 7;
    char host[64]; int hi = 0;
    while (*u && *u != '/' && *u != ':' && hi < 63) host[hi++] = *u++; host[hi] = 0;
    uint16_t pt = 80;
    if (*u == ':') { u++; pt = (uint16_t)atoi(u); while (*u && *u != '/') u++; }
    char path[80]; StrHelper::strncpy(path, (*u == '/') ? u : "/", sizeof(path));
    if (!host[0]) { snprintf(out, out_len, "http: geen host in de url"); return (int)strlen(out); }
    MonitorSensors::SimResult r = sensors.startNetDiag(MonitorSensors::NET_HTTP, host, pt, path);
    if (r == MonitorSensors::SIM_OK) snprintf(out, out_len, "http-check %s%s gestart; uitslag volgt zo", host, path);
    else if (r == MonitorSensors::SIM_ERR_BUSY) snprintf(out, out_len, "netwerk-diagnose bezig, probeer zo opnieuw");
    else snprintf(out, out_len, "ongeldig adres");
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "scan")) {
    MonitorSensors::SimResult r = sensors.startNetDiag(MonitorSensors::NET_SCAN, "scan", 0, nullptr);
    if (r == MonitorSensors::SIM_OK) snprintf(out, out_len, "wifi-scan gestart; uitslag volgt zo");
    else snprintf(out, out_len, "netwerk-diagnose bezig, probeer zo opnieuw");
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "traceroute") || !strcasecmp(verb, "trace")) {
    if (!*arg) { snprintf(out, out_len, "gebruik: traceroute <host>"); return (int)strlen(out); }
    MonitorSensors::SimResult r = sensors.startNetDiag(MonitorSensors::NET_TRACE, arg, 0, nullptr);
    if (r == MonitorSensors::SIM_OK) snprintf(out, out_len, "traceroute %s gestart; uitslag volgt zo", arg);
    else if (r == MonitorSensors::SIM_ERR_BUSY) snprintf(out, out_len, "netwerk-diagnose bezig, probeer zo opnieuw");
    else snprintf(out, out_len, "ongeldig adres");
    return (int)strlen(out);
  }

  /* ---------- BEDIENING (readwrite) ---------- */
  if (!strcasecmp(verb, "checknow")) {
    ND_NEED(2);
    if (sensors.checkNow(*arg ? arg : nullptr))
      snprintf(out, out_len, "meting geforceerd%s%s", *arg ? " voor " : " (alle sensoren)", *arg ? arg : "");
    else snprintf(out, out_len, "onbekende sensor: %s", arg);
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "mute")) {
    ND_NEED(2);
    char nm[24]; int ni = 0; const char* q = arg;
    while (*q && *q != ' ' && ni < 23) nm[ni++] = *q++; nm[ni] = 0;
    while (*q == ' ') q++;
    long secs = *q ? atol(q) : 3600;   // standaard 1 uur
    if (!nm[0]) { snprintf(out, out_len, "gebruik: mute <sensor> [seconden]"); return (int)strlen(out); }
    if (sensors.setMute(nm, (unsigned long)(secs > 0 ? secs : 0)))
      snprintf(out, out_len, "%s gemute voor %ld s", nm, secs > 0 ? secs : 0);
    else snprintf(out, out_len, "onbekende sensor: %s", nm);
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "unmute")) {
    ND_NEED(2);
    if (!*arg) { snprintf(out, out_len, "gebruik: unmute <sensor>"); return (int)strlen(out); }
    if (sensors.clearMute(arg)) snprintf(out, out_len, "%s niet langer gemute", arg);
    else snprintf(out, out_len, "onbekende sensor: %s", arg);
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "snooze")) {
    ND_NEED(2);
    long secs = *arg ? atol(arg) : 900;   // standaard 15 min
    sensors.setSnooze((unsigned long)(secs > 0 ? secs : 0));
    if (secs > 0) snprintf(out, out_len, "ALLE alerts gesnoozed voor %ld s", secs);
    else snprintf(out, out_len, "snooze uit -- alerts weer actief");
    return (int)strlen(out);
  }

  if (!strcasecmp(verb, "test")) {
    ND_NEED(2);
    /* Testalarm via de fa.<TEST>-config (route + room-set), zoals /alert/test. */
    the_mesh.dispatchAlert(sensors.fixedAlertMode(MON_FA_TEST),
                           sensors.fixedRoomsMask(MON_FA_TEST), false,
                           "Testalarm (handmatig aangevraagd)");
    snprintf(out, out_len, "testalarm afgevuurd (volgens fa.test-instelling)");
    return (int)strlen(out);
  }

  /* ---------- BEHEER (admin) ---------- */
  if (!strcasecmp(verb, "sendto")) {
    ND_NEED(3);
    /* sendto <pubkey64> <bericht> -- schone DM vanaf de bot naar één contact. */
    char* sp = (char*)strchr(arg, ' ');
    if (!sp) { snprintf(out, out_len, "gebruik: sendto <pubkey64> <bericht>"); return (int)strlen(out); }
    char hex[PUB_KEY_SIZE * 2 + 1];
    int hl = (int)(sp - arg);
    if (hl != PUB_KEY_SIZE * 2) { snprintf(out, out_len, "sendto: volledige pubkey (64 hex) nodig"); return (int)strlen(out); }
    memcpy(hex, arg, hl); hex[hl] = 0;
    const char* msg = sp + 1;
    while (*msg == ' ') msg++;
    if (!*msg) { snprintf(out, out_len, "sendto: leeg bericht"); return (int)strlen(out); }
    int r = the_mesh.webBotSendTo(hex, msg);
    snprintf(out, out_len, r == 0 ? "DM verstuurd vanaf de bot" : "sendto mislukt (pubkey/bot?)");
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "reboot")) {
    ND_NEED(3);
    g_reboot_at = millis() + 1500;
    snprintf(out, out_len, "node herstart over ~1,5 s");
    return (int)strlen(out);
  }
  if (!strcasecmp(verb, "ntp") || !strcasecmp(verb, "sync")) {
    ND_NEED(2);
#ifdef WIFI_SSID
    wifi_task.syncNow();
    snprintf(out, out_len, "klok-sync aangevraagd (%s)", wifi_task.lastSyncMsg());
#else
    snprintf(out, out_len, "geen wifi: klok-sync niet mogelijk");
#endif
    return (int)strlen(out);
  }

  #undef ND_NEED
  return 0;   // geen node-commando -> DmCommands valt door
}

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
  /* Uitgestelde resultaten (ad-hoc ping e.d.) die IN een room gevraagd zijn, terug
   * in DIE room posten i.p.v. als DM. De callback is captureless -> functiepointer. */
  the_mesh.dm.setRoomPostCallback(
      [](void* ctx, int room_idx, const char* text) {
        ((RoomMesh*)ctx)->addServerPost(room_idx, text);
      },
      &the_mesh);

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
  /* Uitgestelde herstart (room/DM-commando 'reboot'): het antwoord is inmiddels
   * verstuurd, dus nu mag de node om. */
  if (g_reboot_at && (long)(millis() - g_reboot_at) >= 0) {
#if defined(ESP32)
    ESP.restart();
#endif
  }

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
