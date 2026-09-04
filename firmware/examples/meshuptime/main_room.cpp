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
#include "TimeFmt.h"
#if defined(ESP32)
  #include <WiFi.h>
#endif

/* VEILIGHEIDSMARGE OP DE loopTask-STACK. De Arduino-standaard is 8 kB; setup()
 * en loop() draaien daarop. Grote features (SNMP-opbouw, JSON-bouwers, crypto,
 * SPIFFS-lees) kunnen tijdelijk veel stapel vragen. De root-fix haalt de dikke
 * MonitorCfg (~6 kB) en de SNMP-buffers van de stapel; deze 16 kB is de gordel
 * bovenop de bretels. SET_LOOP_TASK_STACK_SIZE is de door de ESP32-core
 * ondersteunde weg (build-flag pakt de voorgecompileerde core niet). */
#if defined(ESP32)
  SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

/* Uitgestelde herstart (room/DM-commando 'reboot'): niet meteen, zodat het
 * antwoord nog verstuurd kan worden. loop() voert hem uit. */
static unsigned long g_reboot_at = 0;

/* === TIJDELIJKE DIAGNOSE (v2.3.5) ===========================================
 * ALTIJD-AAN seriële logging (MESH_DEBUG staat uit) zodat de coordinator bij het
 * uittrekken/insteken/sim live ziet WAT fixedEdge beslist. Prefix "[fixedge]"
 * voor grepbaar filteren op COM4. Mag na de veldverificatie weer weg. */
#define FIXEDGE_DIAG(...) do { Serial.printf("[fixedge] " __VA_ARGS__); Serial.println(); } while (0)

#ifdef WIFI_SSID
  #include "WifiTask.h"
  #include "WebTask.h"
  #include "PushTask.h"
  #include "Poller.h"
  static WifiTask wifi_task;
  static WebTask  web_task;
  static PushTask push_task;
  /* v2.6.0: de MeshManager-opdrachtwachtrij-poller. Haalt HA uit de keten. Alleen
   * in de room-variant met wifi -- hij leunt op PushTask (HTTP) en the_mesh.rcli
   * (RepeaterCli). Zie Poller.h. */
  static Poller   poller;
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
  /* KANTELSTANDEN. Voor ELK alert-type twee vlaggen, en sinds v2.3.7 voor
   * ALLEMAAL hetzelfde paar (de monitors en de batterij hadden vroeger alleen een
   * baseline en dat gaf een spook-"terug" op een pauze/flap):
   *  st_*_down       = baseline: de laatst waargenomen GEDEBOUNCETE toestand.
   *  st_*_announced  = hebben wij voor de LOPENDE onderbreking echt een "weg"
   *                    gemeld? De harde grendel: zonder gemelde "weg" nooit "terug". */
  bool st_batt_crit = false, st_batt_low = false;
  bool st_batt_crit_ann = false, st_batt_low_ann = false;
  bool st_mon_down[MonitorSensors::MAX_MONITORS];
  bool st_mon_ann[MonitorSensors::MAX_MONITORS];
  bool st_mains_down = false, st_wifi_down = false;
  bool st_mains_announced = false, st_wifi_announced = false;

  /* ============ v2.3.7: ÉÉN gegrendelde kantelaar voor ALLE alert-types ============
   *
   * Vervangt de kale edge() (monitors + batterij) én de losse fixedEdge() (vaste
   * kanalen). Vóór v2.3.7 draaiden monitors en batterij op de kale edge() en hadden
   * ze exact de bugs die de vaste kanalen vóór v2.3.5 hadden:
   *   - een pauze/niet-meetbare toestand werd als "weer bereikbaar" gelezen;
   *   - "terug" kwam ook na een flap (geen announced-grendel, geen duur-grendel);
   *   - de duur klopte niet (down_since bleef 0 -> "na <uptime>", vandaar de 2u21);
   *   - een echte storing kon als SIMULATIE gelabeld worden (was_sim-lek).
   *
   * De grendels, nu voor iedereen gelijk:
   *  1. FREEZE: niet-meetbaar (pauze/onbekend) OF gemute/gesnoozed -> GEEN dispatch
   *     en de baseline NIET bijwerken. Een openstaande overgang blijft zo bewaard en
   *     vuurt alsnog zodra het weer meetbaar/actief is. Een pauze is NOOIT "herstel".
   *  2. OPSTART-GENADE ZONDER DESYNC: tijdens de genade GEEN dispatch, maar de
   *     baseline wel stil bijwerken (geen kunstmatige flank bij het einde).
   *  3. HERSTEL-GRENDEL: "terug" ALLEEN als wij voor deze onderbreking echt een "weg"
   *     meldden (announced) EN de onderbreking plausibel lang was (recovery_ok --
   *     nooit 0s/sub-seconde, ook niet met debounce == 0 of bij een sim).
   *  4. SIM-MERK: zit in de tekst-callbacks zelf (alleen bij een echte forcering).
   *
   * De teksten komen via callbacks (lazy): monitorAlertText()/recoverAlertText() en
   * fixedAlertText()/fixedRecoverAlertText() delen allemaal ÉÉN statische buffer, dus
   * ze vooraf allebei berekenen zou de ene de andere laten overschrijven -- de
   * vurende tak haalt er precies één op. Herstel gaat ALTIJD als LOW_PRI (ook als de
   * storing HIGH_PRI was): een gemiste "het werkt weer" is minder erg dan een gemiste
   * "het is stuk". */
  struct EdgeIn {
    bool     down_now;      // gedebouncete/gesettlede storingstoestand
    bool     measurable;    // false -> bevriezen (niet te meten)
    bool     frozen;        // gemute/gesnoozed -> bevriezen
    bool     grace;         // opstart-genade -> onderdruk dispatch, baseline wel bij
    bool     recovery_ok;   // onderbreking plausibel lang
    uint8_t  mode;
    uint16_t rooms;
    bool     high_pri;      // ernst van de STORING (herstel is altijd LOW_PRI)
    uint8_t  severity;      // MON_SEV_* voor de ernst-emoji vooraan de storings-DM
  };
  template <typename DownText, typename UpText>
  void edgeLatched(bool& was_down, bool& announced, const EdgeIn& in,
                   DownText down_text, UpText up_text, const char* tag) {
    if (in.frozen || !in.measurable) {
      FIXEDGE_DIAG("%s BEVROREN (frozen=%d meetbaar=%d) -> geen dispatch",
                   tag, (int)in.frozen, (int)in.measurable);
      return;
    }
    if (in.down_now == was_down) return;             // geen flank
    const bool grace = in.grace;
    was_down = in.down_now;                          // baseline volgt de echte toestand
    if (in.down_now) {
      if (grace) { announced = false; FIXEDGE_DIAG("%s DOWN in boot-grace -> onderdrukt", tag); return; }
      announced = true;
      dispatchAlert(in.mode, in.rooms, in.high_pri, down_text(), in.severity);
      FIXEDGE_DIAG("%s DOWN gemeld", tag);
    } else {
      const bool was_ann = announced;
      announced = false;
      if (grace) { FIXEDGE_DIAG("%s UP in boot-grace -> geen herstel", tag); return; }
      if (was_ann && in.recovery_ok) {
        // herstel = LOW_PRI EN altijd groen (MON_SEV_LOW), ongeacht de ernst van de storing
        dispatchAlert(in.mode, in.rooms, false, up_text(), MON_SEV_LOW);
        FIXEDGE_DIAG("%s HERSTEL gemeld (announced=1 dur_ok=1)", tag);
      } else {
        FIXEDGE_DIAG("%s herstel ONDERDRUKT (announced=%d dur_ok=%d) -- geen spook-terug",
                     tag, (int)was_ann, (int)in.recovery_ok);
      }
    }
  }

  void onSensorDataRead() override {
    const float v = (float)board.getBattMilliVolts() / 1000.0f;
    const bool  snoozed = sensors.isSnoozed();

    /* Batterij (crit/laag): sinds v2.3.7 gedebouncete Schmitt-drempels i.p.v. een
     * kale vergelijking rond 3,4/3,6 V -- geen geflikker, en dezelfde grendels als
     * de rest. battTick() voedt de debounce met de laatste meting. */
    sensors.battTick(v);

    char t1[48];
    EdgeIn bc = { sensors.battAlertDown(MonitorSensors::BATT_CRIT), true, snoozed,
                  sensors.battInBootGrace(), sensors.battRecoveryOk(MonitorSensors::BATT_CRIT),
                  sensors.fixedAlertMode(MON_FA_BATT_CRIT), sensors.fixedRoomsMask(MON_FA_BATT_CRIT), true,
                  sensors.fixedSeverity(MON_FA_BATT_CRIT) };
    edgeLatched(st_batt_crit, st_batt_crit_ann, bc,
      [&]() -> const char* { snprintf(t1, sizeof(t1), "Batterij kritisch (%.2fV)!", v); return t1; },
      []()  -> const char* { return "Batterij hersteld"; }, "batt.crit");

    char t2[48];
    EdgeIn bl = { sensors.battAlertDown(MonitorSensors::BATT_LOW), true, snoozed,
                  sensors.battInBootGrace(), sensors.battRecoveryOk(MonitorSensors::BATT_LOW),
                  sensors.fixedAlertMode(MON_FA_BATT_LOW), sensors.fixedRoomsMask(MON_FA_BATT_LOW), false,
                  sensors.fixedSeverity(MON_FA_BATT_LOW) };
    edgeLatched(st_batt_low, st_batt_low_ann, bl,
      [&]() -> const char* { snprintf(t2, sizeof(t2), "Batterij laag (%.2fV)", v); return t2; },
      []()  -> const char* { return "Batterij weer op peil"; }, "batt.low");

    /* Ping-monitors: dezelfde gegrendelde kantelaar. measurable=false (bewaking
     * gepauzeerd, of nog geen uitslag) BEVRIEST -- dat was de bug van vóór v2.3.7:
     * een pauze (wifi weg) las als "weer bereikbaar". De duur komt nu uit
     * monitorNoteDown/recoverAlertText (echte onbereikbaarheid), niet uit de uptime. */
    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) {
      if (!sensors.monitorUsed(i)) {
        st_mon_down[i] = false; st_mon_ann[i] = false; sensors.monitorNoteClear(i);
        continue;
      }
      const int slot = i;
      EdgeIn mi = { !sensors.monitorIsUp(i), sensors.monitorMeasurable(i), sensors.isMuted(i),
                    false, sensors.monitorRecoveryOk(i),
                    sensors.monitorAlertMode(i), sensors.monitorRoomsMask(i), false,
                    sensors.monitorSeverity(i) };
      edgeLatched(st_mon_down[i], st_mon_ann[i], mi,
        [slot]() -> const char* { sensors.monitorNoteDown(slot); return sensors.monitorAlertText(slot); },
        [slot]() -> const char* { const char* s = sensors.recoverAlertText(slot); sensors.monitorNoteClear(slot); return s; },
        "mon");
    }

    /* De vaste kanalen (netvoeding/wifi): ongewijzigd gedrag (v2.3.5), nu via
     * dezelfde helper -- gedebouncete toestand, opstart-genade zonder OLED-desync,
     * harde herstel-grendel. */
    EdgeIn fp = { sensors.fixedAlertDown(MonitorSensors::FIXED_POWER), true, snoozed,
                  sensors.fixedInBootGrace(), sensors.fixedRecoveryOk(MonitorSensors::FIXED_POWER),
                  sensors.fixedAlertMode(MON_FA_MAINS), sensors.fixedRoomsMask(MON_FA_MAINS), false,
                  sensors.fixedSeverity(MON_FA_MAINS) };
    edgeLatched(st_mains_down, st_mains_announced, fp,
      []() -> const char* { return sensors.fixedAlertText(MonitorSensors::FIXED_POWER); },
      []() -> const char* { return sensors.fixedRecoverAlertText(MonitorSensors::FIXED_POWER); }, "mains");

    EdgeIn fw = { sensors.fixedAlertDown(MonitorSensors::FIXED_WIFI), true, snoozed,
                  sensors.fixedInBootGrace(), sensors.fixedRecoveryOk(MonitorSensors::FIXED_WIFI),
                  sensors.fixedAlertMode(MON_FA_WIFI), sensors.fixedRoomsMask(MON_FA_WIFI), false,
                  sensors.fixedSeverity(MON_FA_WIFI) };
    edgeLatched(st_wifi_down, st_wifi_announced, fw,
      []() -> const char* { return sensors.fixedAlertText(MonitorSensors::FIXED_WIFI); },
      []() -> const char* { return sensors.fixedRecoverAlertText(MonitorSensors::FIXED_WIFI); }, "wifi");
  }

  /* Een room-post herkennen als commando en de antwoordtekst opbouwen. RoomMesh
   * post die tekst dan terug in de room (geknipt). */
  int roomCommandReply(ClientInfo* from, int room_idx, const char* line, char* out, size_t out_len) override {
    if (from == nullptr) return 0;
    return dm.renderReply(*from, room_idx, line, out, out_len);
  }

  /* BOT-DM-COMMANDOPAD. handleBotDm heeft de afzender al tegen _bot_recips
   * gecontroleerd, dus hier draaien we met VOLLE admin-rechten. We bouwen een
   * tijdelijke ClientInfo (STATIC: weg van de 16 KB loopTask-stapel) met alleen wat
   * renderReply leest: isAdmin() + de rol-bits + alarmrechten. Geen scope
   * (room_idx=-1). Loopt er een uitgestelde net-diagnose, dan de room-routing van
   * renderReply annuleren -- wij leveren de uitslag zelf af via botAdhocPoll(). */
  int botCommandReply(const uint8_t* sender_pub, const char* line,
                      char* out, size_t out_len, bool& async_started) override {
    static ClientInfo tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.id = mesh::Identity(sender_pub);
    tmp.permissions = PERM_ACL_ADMIN | PERM_RECV_ALERTS_LO | PERM_RECV_ALERTS_HI;
    tmp.out_path_len = OUT_PATH_UNKNOWN;
    async_started = false;
    int n = dm.renderReply(tmp, -1 /*geen room-scope*/, line, out, out_len);
    if (dm_source.dmAdhocState() != 0 && !dm_source.dmAdhocReady()) {
      async_started = true;
      dm.cancelPendingAdhocRouting();
    }
    return n;
  }

  bool botAdhocPoll(char* out, size_t out_len) override {
    if (!dm_source.dmAdhocReady()) return false;
    StrHelper::strncpy(out, dm_source.dmAdhocResult(), out_len);
    dm_source.dmAdhocClear();
    return true;
  }

public:
  void beginApp() {
    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) { st_mon_down[i] = false; st_mon_ann[i] = false; }
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
                           "Testalarm (handmatig aangevraagd)",
                           sensors.fixedSeverity(MON_FA_TEST));
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
    int r = the_mesh.webBotSlotSendTo(the_mesh.webBotResolve(""), hex, msg);  // alert-bot
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
    /* Tijd-config (NTP-server + tijdzone) VÓÓR wifi begint: setNtpServer moet staan
     * voordat de eerste sync (bij connect) draait, en de TZ moet gezet zijn voordat
     * er een menselijke tijd getoond wordt. RTC/protocol blijven UTC; alleen de
     * WEERGAVE is lokaal (zie TimeFmt.h). */
    char ntp[48], tz[48];
    loadTimeConfig(ntp, sizeof(ntp), tz, sizeof(tz));
    applyTimeZone(tz);
    wifi_task.setNtpServer(ntp);

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
    /* v2.5.1: de room-mesh mag companion-#LOC/val METEEN pushen (POST
     * /api/companion) via dezelfde PushTask -- geen wachten op de poll. */
    the_mesh.setPushTask(&push_task);

    /* ADMIN-CLI NAAR EEN ANDERE REPEATER (zie RepeaterCli.h).
     *
     * Twee regels, en allebei een KOPPELING en geen gedrag:
     *  - de webroute POST/GET /cli/remote krijgt de module te zien;
     *  - het antwoord gaat naar MeshManager, via dezelfde PushTask/host/token als
     *    de sensorpush (POST /api/v1/repeater_settings).
     *
     * De callback is captureless -> een gewone functiepointer, zodat std::function
     * er niets voor op de heap zet. Dit is dezelfde vorm als de room-post-callback
     * hierboven, en om dezelfde reden: RepeaterCli hoort niets van HTTP, tokens of
     * MeshManager te weten, en main_room.cpp is de enige plek die beide kent. */
    web_task.setRepeaterCli(&the_mesh.rcli);
    /* De uitslag van ELK commando gaat naar MeshManager. De klok-job (v2.8.0) komt
     * hier langs onder "cmd:clockfix"; die melden we ook aan de Poller, zodat zijn
     * tellers en /poller.json de uitkomst kennen. De captureless lambda krijgt
     * daarom niet PushTask maar een klein vast paar mee. */
    the_mesh.rcli.setResultCallback(
        [](void* ctx, const char* pubkey_hex12, const char* param, const char* value) {
          (void)ctx;
          push_task.queueRepeaterSetting(pubkey_hex12, param, value);
          if (param != nullptr && strcmp(param, "cmd:clockfix") == 0) {
            poller.noteClockFix(value);
          }
        },
        nullptr);

    /* DE POLLER (v2.6.0). Dezelfde PushTask (poll-GET + antwoord-POST) en dezelfde
     * RepeaterCli (login + commando's). De antwoorden lopen via de callback die we
     * hierboven al op rcli zetten -- de poller hoeft ze niet te onderscheppen; hij
     * start alleen de jobs en pusht zelf de null-antwoorden voor geweigerde/
     * wachtwoordloze parameters. De web-GUI beheert aan/uit, interval en de
     * doel-wachtwoorden. */
    poller.begin(&SPIFFS, &push_task, &the_mesh.rcli);
    web_task.setPoller(&poller);
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
  the_mesh.dm.loop();   // v2.3.12: room-variant miste dit -> uitgestelde net-cmd-uitslag (ping/dns/...) werd nooit teruggepost in de room
#ifdef WIFI_SSID
  wifi_task.loop();
  web_task.loop();
  push_task.loop();
  poller.loop();   // v2.6.0: MeshManager-opdrachtwachtrij; niet-blokkerend, na de bewaking
#endif
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif

#if defined(ESP32)
  /* Stapel-waakhond: het LAAGSTE vrije stapelniveau van de loopTask sinds boot,
   * eens per ~30 s. Zo wordt een krappe of teruglopende marge VROEG zichtbaar in
   * plaats van pas bij een canary-panic. Waarde in bytes; blijft dit onder ~1500,
   * dan is er een dikke stapelgebruiker vanuit loop() bijgekomen. */
  static unsigned long next_stack_log = 0;
  if ((long)(millis() - next_stack_log) >= 0) {
    next_stack_log = millis() + 30000;
    UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);   // woorden vrij (minimum)
    MESH_DEBUG_PRINTLN("loopTask vrije stapel (min sinds boot): %u byte",
                       (unsigned)(hw * sizeof(StackType_t)));
  }
#endif
}
