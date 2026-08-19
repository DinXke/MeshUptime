#include "SensorMesh.h"
#include "DmCommands.h"

#ifdef WIFI_SSID
  #include "WifiTask.h"
  #include "WebTask.h"
  static WifiTask wifi_task;
  static WebTask  web_task;
#endif

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

/* De brug tussen de sensorlaag en de DM-opdrachten.
 *
 * DmCommands kent MonitorSensors met opzet niet: het praat alleen met deze
 * DmDataSource. Daardoor kan de ene kant veranderen zonder de andere te breken,
 * en dat is hier geen theorie -- beide zijn vandaag naast elkaar gebouwd.
 *
 * De VAKJENUMMERING loopt gelijk met die van MonitorSensors: 0,1,2 zijn de vaste
 * sensoren en 3 en hoger zijn de monitorvakjes. Een leeg vakje geeft false,
 * zodat er niets opschuift als er een monitor verdwijnt -- dezelfde regel als bij
 * de kanaalnummers, en om dezelfde reden.
 */
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
    if (!sensors.monitorUsed(slot)) return false;   // leeg vakje: overslaan

    dest.channel = sensors.monitorChannel(slot);
    strncpy(dest.name, sensors.monitorName(slot), sizeof(dest.name) - 1);
    if (sensors.monitorsPaused()) {
      /* Niet "neer" zeggen als we niet gemeten hebben. Zonder wifi meet je je
       * eigen netwerk en niet de dienst. */
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
};
static MonitorDmSource dm_source;

class MyMesh : public SensorMesh {
public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms, mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables)
     : SensorMesh(board, radio, ms, rng, rtc, tables), 
       battery_data(12*24, 5*60)    // 24 hours worth of battery data, every 5 minutes
  {
  }

  /* Publiek: setup() en loop() moeten erbij. */
  DmCommands dm;

protected:
  /* ========================== custom logic here ========================== */
  Trigger low_batt, critical_batt;

  /* Een Trigger per VAKJE en niet per actieve monitor: dat verband mag niet
   * verschuiven als er een monitor verdwijnt. Kost ongeveer 1,7 kB, want elke
   * Trigger draagt een tekstbuffer van MAX_PACKET_PAYLOAD. */
  Trigger monitor_down[MonitorSensors::MAX_MONITORS];
  TimeSeriesData  battery_data;

  void onSensorDataRead() override {
    float batt_voltage = getVoltage(TELEM_CHANNEL_SELF);

    battery_data.recordData(getRTCClock(), batt_voltage);   // record battery
    alertIf(batt_voltage < 3.4f, critical_batt, HIGH_PRI_ALERT, "Battery is critical!");
    alertIf(batt_voltage < 3.6f, low_batt, LOW_PRI_ALERT, "Battery is low");

    /* LOW_PRI_ALERT is bewust: dat doet een poging per contact in plaats van
     * vier. MonitorSensors laat er zelf hoogstens twee tegelijk los, want
     * MAX_CONCURRENT_ALERTS is 4 en de batterij gebruikt er al twee. */
    for (int i = 0; i < MonitorSensors::MAX_MONITORS; i++) {
      alertIf(sensors.monitorAlert(i), monitor_down[i], LOW_PRI_ALERT,
              sensors.monitorAlertText(i));
    }
  }

  int querySeriesData(uint32_t start_secs_ago, uint32_t end_secs_ago, MinMaxAvg dest[], int max_num) override {
    battery_data.calcMinMaxAvg(getRTCClock(), start_secs_ago, end_secs_ago, &dest[0], TELEM_CHANNEL_SELF, LPP_VOLTAGE);
    return 1;
  }

  bool handleIncomingMsg(ClientInfo& from, uint32_t timestamp, uint8_t* data, uint8_t flags, size_t len) override {
    return dm.handleDm(from, timestamp, data, len);
  }

  void onAckRecv(mesh::Packet* packet, uint32_t ack_crc) override {
    if (dm.onAck(ack_crc)) { packet->markDoNotRetransmit(); return; }
    SensorMesh::onAckRecv(packet, ack_crc);
  }

  bool handleCustomCommand(uint32_t sender_timestamp, char* command, char* reply) override {
    if (strcmp(command, "magic") == 0) {    // example 'custom' command handling
      strcpy(reply, "**Magic now done**");
      return true;   // handled
    }
    return false;  // not handled
  }
  /* ======================================================================= */
};

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

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
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Sensor ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

  the_mesh.dm.begin(&the_mesh, &dm_source);
  the_mesh.dm.setPathHashSize(the_mesh.getNodePrefs()->path_hash_mode + 1);

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

#ifdef WIFI_SSID
  {
    /* De OPGESLAGEN instelling gaat voor op de gebakken bouwvlag. Dat is een
     * ontwerpeis en geen gemak: zo hoeft een node die naar een ander netwerk
     * verhuist niet opnieuw geflasht te worden, en zo is een node die je
     * weggeeft schoon te maken door de opslag te wissen. De gebakken waarde
     * blijft het laatste redmiddel. */
    char ssid[33], pwd[65];   // WifiTask kopieert ze, dus lokaal mag
    if (loadWifiConfig(ssid, sizeof(ssid), pwd, sizeof(pwd))) {
      wifi_task.begin(ssid, pwd);
    } else {
      wifi_task.begin(WIFI_SSID, WIFI_PWD);
    }
    web_task.begin(&wifi_task, FIRMWARE_VERSION);
    /* Zonder deze regel compileert alles en werkt de pagina, maar antwoordt
     * /acl.json met 503 en blijft het toegangsdeel leeg met de reden erin. Om
     * dezelfde reden als bij setMonitors() staat de koppeling hier: main.cpp is
     * de enige plek die zowel de webtaak als de mesh kent.
     *
     * NA the_mesh.begin(), en dat is geen stijl maar noodzaak: begin() leest de
     * toegangslijst en de stand van het slot uit SPIFFS. */
    web_task.setAcl(&the_mesh);
  }
  #ifdef HAS_MONITOR_SENSORS
    /* main.cpp is de enige plek die zowel de wifi-taak als de sensoren kent;
     * daarom hier de koppeling, en niet via een global in een header. */
    sensors.setWifiTask(&wifi_task);
    /* Zonder deze regel compileert alles en werkt de pagina, maar toont de
     * kanaalkaart alleen een waarschuwing en antwoordt /hook met 503. De
     * webserver is de enige plek die de kanaalkaart kan tonen, want CayenneLPP
     * draagt geen namen. */
    web_task.setMonitors(&sensors);
    /* Na the_mesh.begin(fs): die leest de ACL en de stand van het slot uit
     * SPIFFS. Zonder deze regel werkt de pagina maar antwoordt /acl.json met
     * 503 en de reden erin. */
    web_task.setAcl(&the_mesh);
  #endif
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  the_mesh.sendSelfAdvertisement(16000, false);
#endif
}

void loop() {
  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
    }
    Serial.print(c);
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    command[len - 1] = 0;  // replace newline with C string null terminator
    /* 256 en niet 160. CommonCLI's "sensor list" schrijft met ongebonden
     * sprintf en stopt pas als de schrijfpositie 134 voorbij is; daarna komt er
     * nog "... next:N" bij. Met een waarde als mon.5.host=192.168.110.254 komt
     * dat op 168 byte uit, en dat is 8 byte over de rand van een buffer van 160
     * -- gemeten: de node ging om in een LoadProhibited-panic op precies
     * "sensor list" met beginindex 0. Met de hostnaam op 40 tekens begrensd is
     * de bovengrens 134 + 53 + 12 = 199, dus 256 geeft ruimte.
     *
     * De eigenlijke fout zit in src/helpers/CommonCLI.cpp en is niet van ons;
     * dit is de rand dichtzetten aan de kant die we bezitten. */
    char reply[256];
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

  the_mesh.loop();
  the_mesh.dm.loop();
  sensors.loop();
#ifdef WIFI_SSID
  wifi_task.loop();
  web_task.loop();
#endif
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
}
