#pragma once

#include <Arduino.h>
#include <MeshCore.h>   /* MAX_PACKET_PAYLOAD, voor de budgetberekening onderaan */
#include <helpers/sensors/EnvironmentSensorManager.h>

#include "MonitorStore.h"

/* Zodat main.cpp kan zien of deze klasse mee gebouwd is. Zonder de macro's
 * SENSOR_MANAGER_CLASS/SENSOR_MANAGER_INCLUDE (zie patches/) bouwt de variant
 * de gewone EnvironmentSensorManager en bestaat setWifiTask() niet. */
#define HAS_MONITOR_SENSORS 1

class WifiTask;   // alleen als pointer nodig; de definitie staat in de .cpp

/* MonitorSensors -- de sensorlaag van MeshUptime.
 *
 * WAAROM DIT IN querySensors() ZIT EN NIET IN onSensorDataRead()
 *
 * SensorMesh leest de sensoren op TWEE plaatsen:
 *   1. de periodieke ronde in SensorMesh::loop()  (~regel 931), die
 *      querySensors() aanroept EN daarna onSensorDataRead()
 *   2. het antwoord op een telemetrieverzoek, SensorMesh::handleRequest()
 *      (~regel 176), die ALLEEN querySensors() aanroept
 *
 * Wie zijn velden in onSensorDataRead() toevoegt, krijgt ze dus niet in het
 * antwoord op een verzoek: de vraagsteller ziet alleen de batterijspanning.
 * handleRequest is niet virtueel en reply_data is private, dus de toepassing
 * kan daar niet tussen komen. querySensors() is de enige haak die in beide
 * paden loopt, en daarom staat alles wat naar buiten moet hier.
 *
 * KANAALINDELING -- VAST, MAG NOOIT VERSCHUIVEN
 *
 * CayenneLPP draagt geen namen, alleen kanaalnummers. Een vraagsteller die
 * "kanaal 4 = wifi" bewaart en later andere gegevens op kanaal 4 krijgt, ziet
 * geen foutmelding maar verkeerde cijfers. Daarom staan deze nummers vast:
 *
 *   1  TELEM_CHANNEL_SELF   batterijspanning (LPP_VOLTAGE) -- zet SensorMesh zelf
 *   2  netvoeding           LPP_SWITCH 0/1
 *   3  batterijvoeding      LPP_SWITCH 0/1  (altijd het omgekeerde van 2)
 *   4  wifi online          LPP_SWITCH 0/1
 *   5..12  monitors         LPP_SWITCH 0/1 (up) + LPP_GENERIC_SENSOR (ms)
 *
 * Een monitor op 5..12 is van EEN VAN TWEE SOORTEN, en de telemetrie ziet er
 * voor beide identiek uit -- met opzet, want een dashboard hoort niet te hoeven
 * weten hoe wij aan onze uitslag komen:
 *
 *   PING    wij pingen zelf, op ons eigen interval.
 *   GEMELD  iets van buiten (Uptime Kuma) meldt de uitslag via /hook. Wij
 *           pingen zo'n monitor NIET; wij verouderen hem juist als de melding
 *           uitblijft. Zie PUSH_HOST en PUSH_STALE_FACTOR hieronder.
 *
 * De ruwe spanning gaat NIET nog eens mee als addVoltage(2, ...): kanaal 1
 * heeft die al, en dubbel verzenden kost bytes zonder iets toe te voegen.
 *
 * WAARSCHUWING over de basisklasse: EnvironmentSensorManager::querySensors()
 * deelt kanalen uit vanaf TELEM_CHANNEL_SELF + 1, dus vanaf 2. Zijn er I2C
 * omgevingssensoren aangesloten, dan komen die op 2, 3, 4 ... naast onze
 * schakelaars te staan. Dat botst niet in het formaat -- per kanaal mogen
 * meerdere types staan, en een decoder kijkt naar het type -- maar het maakt
 * een dashboard wel verwarrend. Deze node heeft geen omgevingssensoren; als er
 * ooit een bij komt, waarschuwt begin() erover in de debug-uitvoer.
 *
 * ================== HET TELEMETRIEBUDGET ==================
 *
 * SensorMesh maakt zijn CayenneLPP als telemetry(MAX_PACKET_PAYLOAD - 4), dus
 * 180 byte (SensorMesh.cpp:704; die 4 zijn de tijdstempel die handleRequest
 * voor de gegevens zet). Elk veld kost 2 byte kop (kanaal + type) plus zijn
 * gegevens:
 *
 *   kanaal 1  batterijspanning   LPP_VOLTAGE          2 + 2 =  4
 *   kanaal 1  GPS, als actief    LPP_GPS              2 + 9 = 11
 *   kanaal 2  netvoeding         LPP_SWITCH           2 + 1 =  3
 *   kanaal 3  batterijvoeding    LPP_SWITCH           2 + 1 =  3
 *   kanaal 4  wifi               LPP_SWITCH           2 + 1 =  3
 *                                               vast: --------
 *                                                           24
 *
 * Per monitor, in het duurste geval (monitor is up, dus mét pingtijd):
 *
 *   LPP_SWITCH         2 + 1 = 3
 *   LPP_GENERIC_SENSOR 2 + 4 = 6
 *                      ---------
 *                              9
 *
 * 180 - 24 = 156 byte over, dus er zouden 156 / 9 = 17 monitors passen. We
 * staan er 8 toe, want er zijn maar 8 vaste kanalen (5..12); dat is 72 byte en
 * laat 84 byte over voor omgevingssensoren die er later bij komen.
 * querySensors() rekent het toch bij elke ronde na (zie TELEM_BUDGET) en kapt
 * af, want een aanname die alleen in commentaar staat is geen aanname waar je
 * op kunt bouwen.
 */
class MonitorSensors : public EnvironmentSensorManager {
public:
  /* Vaste kanaalnummers, ook bruikbaar voor de UI en de ping-monitors. */
  static const uint8_t CH_MAINS         = 2;
  static const uint8_t CH_BATTERY       = 3;
  static const uint8_t CH_WIFI          = 4;
  static const uint8_t CH_MONITOR_FIRST = 5;
  static const uint8_t CH_MONITOR_LAST  = 12;

  /* Aantal vakjes = aantal kanalen. main.cpp gebruikt dit voor zijn rij
   * Trigger-objecten, dus het moet publiek zijn. */
  static const uint8_t MAX_MONITORS = MON_MAX_MONITORS;

  /* Hoeveel monitorwaarschuwingen er tegelijk mogen lopen.
   *
   * SensorMesh heeft MAX_CONCURRENT_ALERTS = 4 plaatsen in zijn wachtrij
   * (SensorMesh.h:46) en main.cpp gebruikt er al twee voor low_batt en
   * critical_batt. alertIf() slaat een waarschuwing STIL over als die rij vol
   * is: de Trigger blijft dan onaangeroerd, dus de volgende leesronde probeert
   * het opnieuw en er gaat niets verloren. Maar acht monitors die tegelijk
   * omvallen zouden acht DM-ketens naar alle contacten sturen, en dat is meer
   * LoRa-tijd dan de storing waard is. Daarom laten we er hoogstens twee los en
   * houden we twee plaatsen vrij voor de batterij -- die kan niet wachten. */
  static const uint8_t MAX_MONITOR_ALERTS = 2;

  /* ---------------- de tweede soort monitor: van buiten GEMELD ----------------
   *
   * WAAROM HET ADRES DE SOORT BEPAALT EN ER GEEN 'kind'-VELD IS
   *
   * De soort moet een herstart overleven, en het opslagbestand is van
   * MonitorStore: één regel "m <kanaal> <interval> <naam> <adres>". Een veld
   * erbij betekent een nieuw bestandsformaat en dus MonitorStore aanpassen. Dat
   * hoeft niet: het adres kan de soort al dragen. Een monitor met adres "-"
   * pingt niet -- "-" is geen geldige hostnaam en geen IP-adres, dus er is geen
   * echte monitor die per ongeluk in deze tak valt, en oude bestanden blijven
   * leesbaar zonder één regel aan MonitorStore.
   *
   * Gevolg is precies wat gewenst is voor een gemelde dienst: van hem worden
   * alleen de NAAM en het KANAAL bewaard (en het meldritme, want dat is een
   * instelling en geen meting). De toestand up/ms staat nergens in het bestand,
   * want die van een gemelde dienst is per definitie vluchtig: na een herstart
   * weten wij niets over die dienst tot de eerste nieuwe melding, en "onbekend"
   * is dan het enige eerlijke antwoord.
   *
   * Het kanaal MOET wel bewaard blijven, en om dezelfde reden als bij de
   * ping-monitors: een node die telemetrie opvraagt bewaart "kanaal 6 = google"
   * aan zijn kant. Zie allocChannel() in de .cpp.
   */
  static const char PUSH_HOST[];        /* "-" */

  /* VEROUDEREN VAN EEN GEMELDE DIENST
   *
   * Een gemelde dienst mag niet eeuwig "op" blijven staan omdat de MELDER stil
   * is gevallen. Dat is juist het geval dat de gebruiker wil weten: als Uptime
   * Kuma zelf plat ligt, weten wij niets meer -- en dat is niet hetzelfde als
   * "nog steeds up".
   *
   * De regel: een melding is verouderd na
   *
   *     max(PUSH_STALE_FACTOR * meldperiode, PUSH_STALE_MIN_MS)
   *
   * De meldperiode is wat /hook meegeeft (&every=<s>) of anders het interval van
   * de monitor, standaard MON_INTERVAL_DEFAULT. Drie perioden, want twee is te
   * scherp: één gemiste melding door een herstart van de melder of een hikje in
   * het netwerk mag geen alarm zijn. De ondergrens van 90 s zit erbij omdat de
   * ondergrens van het interval 10 s is, en drie keer 10 s is korter dan de
   * jitter van een melder die zelf ook nog een time-out afwacht.
   *
   * Verouderd is NIET "neer": de toestand wordt ONBEKEND (seeded valt weg), dus
   * de telemetrie zet de schakelaar op 0 zonder pingtijd en de pagina toont "?".
   * Er gaat wel een waarschuwing uit -- met een eigen tekst, zodat de ontvanger
   * ziet dat de MELDER weg is en niet de dienst.
   *
   * De klok begint niet te lopen zolang wij zelf blind zijn: zonder onze WiFi
   * kan /hook ons niet bereiken, en dan is het uitblijven van meldingen onze
   * fout. Zie monitorStaleRef().
   */
  static const uint8_t  PUSH_STALE_FACTOR = 3;
  static const uint32_t PUSH_STALE_MIN_MS = 90000;

  /* Uitkomst van createMonitor() en reportMonitor(). Geen bool, omdat de
   * webinterface en /hook een BRUIKBAAR antwoord moeten geven: "naam al in
   * gebruik" en "alle acht vakjes vol" zijn twee verschillende dingen voor wie
   * aan de andere kant staat, en de keuring hoort op één plek te zitten en niet
   * half in WebTask. */
  enum MonResult : uint8_t {
    MON_OK = 0,
    MON_ERR_NAME,       /* naam leeg, te lang of verkeerde tekens */
    MON_ERR_HOST,       /* adres leeg, te lang of verkeerde tekens */
    MON_ERR_INTERVAL,   /* buiten MON_INTERVAL_MIN..MAX */
    MON_ERR_TAKEN,      /* naam bestaat al */
    MON_ERR_KIND,       /* bestaat al, maar als andere soort */
    MON_ERR_FULL,       /* geen vakje of geen kanaal meer vrij */
    MON_ERR_UNKNOWN     /* naam bestaat niet (bij verwijderen) */
  };

  /* Waarom deze tekst hier en niet in WebTask: dan staat de reden één keer
   * beschreven, en de seriële console kan hem later net zo goed gebruiken. */
  static const char* monResultText(MonResult r);

#if ENV_INCLUDE_GPS
  MonitorSensors(LocationProvider& location) : EnvironmentSensorManager(location) { }
#else
  MonitorSensors() { }
#endif

  bool begin() override;
  void loop() override;
  bool querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) override;

  int         getNumSettings() const override;
  const char* getSettingName(int i) const override;
  const char* getSettingValue(int i) const override;
  bool        setSettingValue(const char* name, const char* value) override;

  /* De wifi-toestand komt van WifiTask. Een setter en geen global in een
   * header: main.cpp is de enige plek die beide objecten kent, en zo blijft
   * deze klasse te bouwen zonder WiFi. */
  void setWifiTask(WifiTask* task) { _wifi = task; }

  /* Voor de UI, de webinterface en de waarschuwingen in main.cpp. */
  bool     isMains() const { return _mains; }
  bool     isWifiOnline() const;
  float    lastVolts() const { return _last_volts; }
  uint32_t secsInPowerState() const { return (millis() - _state_since) / 1000; }

  /* ---------------- monitors, voor main.cpp / UI / web ----------------
   *
   * Alles hieronder werkt op het VAKJENUMMER (0..MAX_MONITORS-1) en niet op een
   * rangnummer van bezette vakjes. Een vakje hoort bij één Trigger in main.cpp
   * en dat verband mag niet verschuiven als er een monitor verdwijnt. */
  uint8_t     getNumMonitors() const;                    /* aantal bezette vakjes */
  bool        monitorUsed(int slot) const;
  uint8_t     monitorChannel(int slot) const;            /* 0 = leeg vakje */
  const char* monitorName(int slot) const;
  const char* monitorHost(int slot) const;
  uint16_t    monitorInterval(int slot) const;
  bool        monitorIsUp(int slot) const;
  bool        monitorSeeded(int slot) const;             /* al ooit een uitslag? */
  uint32_t    monitorPingMs(int slot) const;
  uint32_t    monitorChecks(int slot) const;
  uint32_t    monitorFails(int slot) const;

  /* Soort: waar als dit een van buiten GEMELDE dienst is (adres == PUSH_HOST).
   * Voor zo'n vakje betekent monitorHost() niets en monitorInterval() de
   * afgesproken MELDPERIODE in plaats van een pinginterval. */
  bool     monitorIsPush(int slot) const;
  /* Secondes sinds de laatste melding; 0 als er nog nooit een melding was of
   * als dit geen gemelde dienst is. */
  uint32_t monitorReportAge(int slot) const;
  /* Na hoeveel secondes zonder melding dit vakje op onbekend gaat. 0 voor een
   * ping-monitor. */
  uint32_t monitorStaleSecs(int slot) const;
  /* Is de laatste melding te oud? Dan is de toestand onbekend (monitorSeeded()
   * geeft dan al false) EN vragen wij een waarschuwing aan. */
  bool     monitorIsStale(int slot) const;

  /* Staat het pingen stil omdat WiFi weg is? Dan zijn de toestanden hierboven
   * de laatst GEMETEN waarden en niet de huidige. */
  bool monitorsPaused() const;

  /* De twee aanroepen die main.cpp in onSensorDataRead() nodig heeft:
   *
   *   alertIf(sensors.monitorAlert(i), monitor_down[i], LOW_PRI_ALERT,
   *           sensors.monitorAlertText(i));
   *
   * monitorAlert() is de voorwaarde: waar zolang deze monitor bevestigd down
   * is, WiFi aanstaat, en er nog een van de MAX_MONITOR_ALERTS plaatsen vrij
   * is. Het versturen, het opnieuw proberen en het opruimen bij herstel doet
   * alertIf() zelf -- hier staat geen eigen waarschuwingslus. */
  bool        monitorAlert(int slot);
  const char* monitorAlertText(int slot) const;

  /* ---------------- beheren van buiten de sensorlaag ----------------
   *
   * Dezelfde twee handelingen die "sensor set mon.add" en "mon.del" doen, maar
   * met losse velden en met een REDEN als het niet lukt. De webinterface roept
   * deze aan; de keuring (validName/validHost/de intervalgrenzen) staat daardoor
   * op één plek en niet ook nog eens in WebTask.
   *
   * out_channel is optioneel en krijgt bij MON_OK het toegewezen kanaal. Dat
   * getal is het antwoord waar de aanroeper op zit te wachten: de naam reist
   * niet mee in de telemetrie, het kanaalnummer wel. */
  MonResult createMonitor(const char* name, const char* host, uint16_t interval_s,
                          uint8_t* out_channel = NULL);
  MonResult deleteMonitor(const char* name);

  /* Een melding van buiten (/hook) doorkoppelen naar de telemetrie.
   *
   * Bestaat de naam nog niet, dan wordt er een GEMELDE monitor aangemaakt en
   * krijgt hij een kanaal van dezelfde toewijzer als een ping-monitor -- er is
   * met opzet maar één toewijzer. Bestaat de naam al als PING-monitor, dan
   * volgt MON_ERR_KIND: twee bronnen voor één kanaal geeft twee waarheden.
   *
   * every_s is de meldperiode die de melder zegt aan te houden; 0 betekent
   * "laat staan wat er stond". Hij bepaalt alleen wanneer een melding veroudert.
   */
  MonResult reportMonitor(const char* name, bool up, uint32_t ms,
                          uint16_t every_s, uint8_t* out_channel = NULL);

  /* De tekenzeef, publiek omdat de webinterface zijn invoer met DEZELFDE zeef
   * moet keuren als de CLI. Een tweede, iets andere zeef is een tweede waarheid:
   * dan komt er via het ene pad een naam binnen die het andere pad afkeurt. */
  static bool validName(const char* s);
  static bool validHost(const char* s);

private:
  /* De toestandsmachine. Alle drie de getallen komen uit de meting van
   * 19 augustus 2026 (docs/meting-voeding-2026-08-19.log) en niet uit een
   * datasheet; dit bord heeft geen PMU en dus geen VBUS-detectie, netspanning
   * wordt volledig afgeleid uit de klemspanning.
   *
   *   op USB, lader klaar         : 4,139 V  (5 van 5 metingen identiek)
   *   op USB, actief ladend       : 4,209-4,226 V
   *   op batterij, onder belasting: 4,034 V
   *
   * Op batterij zakt de klemspanning omdat de node stroom trekt; op USB vult
   * de lader die belasting aan. Dat verschil is fysisch en het wordt groter
   * als de batterij ouder wordt.
   *
   * Drie voorzieningen tegen fout schakelen:
   *  - twee drempels (Schmitt), zodat een spanning tussen 4,09 en 4,12 geen
   *    mening oplevert in plaats van geflikker
   *  - drie opeenvolgende metingen aan dezelfde kant voor een overgang
   *  - 60 s insteltijd na een overgang; in de eerste minuut na uittrekken is
   *    een transient van ~50 mV gemeten (4,052 -> 4,087 -> 4,034)
   */
  static const uint8_t  SAMPLES_TO_SWITCH  = 3;
  static const uint32_t SAMPLE_INTERVAL_MS = 10000;   // 10 s: 3 metingen = 30 s reactietijd
  static const uint32_t SETTLE_MS          = 60000;   // insteltijd na een overgang

  /* ---------------- afspraken van de ping-bewaking ---------------- */

  /* Dezelfde soort hysterese als bij de voeding, en om dezelfde reden: één
   * verloren ICMP-pakket op een druk netwerk is geen storing. 3 mislukte
   * rondes op een interval van 60 s is 3 minuten reactietijd; dat is voor een
   * dienstbewaking die over LoRa waarschuwt snel genoeg. */
  static const uint8_t PINGS_TO_DOWN = 3;
  static const uint8_t PINGS_TO_UP   = 2;   /* omhoog mag sneller: dat kost geen alarm */

  static const uint16_t PING_TIMEOUT_MS = 1000;
  /* Noodrem: als de ping-sessie van ESP-IDF om welke reden ook nooit zijn
   * eindfunctie aanroept, wordt hij hierna alsnog opgeruimd. Ruim boven
   * PING_TIMEOUT_MS, zodat een gewone mislukking niet in deze tak valt. */
  static const uint32_t PING_DEADLINE_MS = 4000;
  static const uint32_t DNS_DEADLINE_MS  = 5000;
  /* Een naam wordt niet bij elke ronde opnieuw opgezocht: dat is nodeloos
   * verkeer en de meeste diensten verhuizen niet elk uur. */
  static const uint32_t DNS_TTL_MS = 10UL * 60 * 1000;

  /* Rusttijd tussen twee pings, ook als er meerdere monitors klaarstaan. Eén
   * ping tegelijk is de regel; deze pauze smeert bovendien acht monitors met
   * hetzelfde interval uit in plaats van ze in een rij te laten dringen. */
  static const uint32_t PING_GAP_MS = 1500;

  /* Na terugkomst van WiFi eerst even niets doen: DHCP, DNS en de route zijn
   * dan nog niet altijd klaar, en een ping die daarop stukloopt zou een dienst
   * onterecht down verklaren. */
  static const uint32_t WIFI_GRACE_MS = 30000;

  /* Het budget uit de uitleg bovenaan: de maat waarmee SensorMesh zijn
   * CayenneLPP maakt. Staat hier zodat querySensors() kan afkappen zonder op de
   * goede wil van de bibliotheek te vertrouwen. */
  static const uint8_t TELEM_BUDGET        = MAX_PACKET_PAYLOAD - 4;   /* = 180 */
  static const uint8_t TELEM_BYTES_SWITCH  = 3;   /* 2 kop + 1 gegeven */
  static const uint8_t TELEM_BYTES_GENERIC = 6;   /* 2 kop + 4 gegeven */

  WifiTask* _wifi = NULL;

  /* Instellingen die een herstart moeten overleven, in één blok zodat
   * MonitorStore ze in één keer kan lezen en schrijven. De drempels waren hier
   * eerst losse velden; ze staan nu in _cfg omdat ze bij dezelfde opslag horen
   * (zie MonitorStore.h voor waarom die opslag nodig is). */
  MonitorCfg _cfg;

  bool          _mains  = true;    // wordt bij de eerste meting overschreven
  bool          _seeded = false;
  uint8_t       _agree  = 0;       // metingen achter elkaar aan de andere kant
  float         _last_volts = 0.0f;
  unsigned long _next_sample = 0;
  unsigned long _state_since = 0;

  void samplePower();

  /* Gemeten toestand per vakje. Staat NIET in MonitorCfg: na een herstart is
   * hij ongeldig en moet er opnieuw gemeten worden. */
  struct MonState {
    unsigned long next_check;    /* millis van de volgende ronde */
    uint32_t      last_ms;       /* laatst gemeten rondetijd */
    uint32_t      checks;        /* rondes sinds opstarten */
    uint32_t      fails;         /* daarvan mislukt */
    uint32_t      addr_v4;       /* opgelost adres, netwerk-volgorde; 0 = geen */
    unsigned long addr_expiry;   /* millis waarna opnieuw opzoeken */
    uint8_t       agree;         /* uitslagen achter elkaar aan de andere kant */
    bool          up;
    bool          seeded;        /* al ooit een uitslag gehad */
    bool          alerting;      /* wij vragen main.cpp nu om een waarschuwing */

    /* Alleen voor GEMELDE diensten. Ook deze twee horen niet in MonitorCfg: ze
     * gaan over metingen en niet over instellingen, en na een herstart weten wij
     * per definitie niets over een gemelde dienst. */
    unsigned long last_report;   /* millis van de laatste melding; 0 = nooit */
    bool          stale;         /* melding te oud; toestand onbekend */
  };
  MonState _mon[MAX_MONITORS];

  /* ---------------- de ping-machine ----------------
   *
   * Eén ping tegelijk, en alles in stappen: loop() start iets en kijkt bij een
   * volgende ronde of het klaar is. Er wordt nooit gewacht, want de LoRa-radio
   * wordt vanuit dezelfde loop() bediend en die heeft de strengste tijdseisen
   * van het hele apparaat. */
  enum PingPhase : uint8_t {
    PING_IDLE,
    PING_RESOLVING,   /* wacht op een naamsopzoeking bij lwIP */
    PING_RUNNING,     /* wacht op de callback van esp_ping */
    PING_REAPING      /* uitslag binnen; de sessie mag over een moment weg */
  };

  PingPhase     _phase = PING_IDLE;
  int8_t        _busy_slot = -1;        /* vakje dat nu aan de beurt is */
  unsigned long _phase_deadline = 0;
  unsigned long _next_ping_at = 0;      /* rusttijd tussen twee pings */

  /* De uitslag van de lopende ping en van de lopende naamsopzoeking staat NIET
   * hier maar in bestandsbereik in MonitorSensors.cpp (s_ping). Reden: die
   * velden worden geschreven door de ping-taak en de lwIP-taak, en die roepen
   * C-functies aan. Een static lidfunctie met de juiste lwIP-signatuur kan niet
   * zonder ip_addr_t in deze header, en een gecaste functieaanwijzer is erger
   * dan een static in de .cpp. Er is precies één MonitorSensors (de global
   * `sensors` uit variants/heltec_v3/target.cpp) en precies één ping tegelijk,
   * dus bestandsbereik is hier eerlijk en niet een verkapte global. */

  bool          _wifi_was_online = false;
  unsigned long _wifi_ok_since = 0;     /* millis waarop WiFi weer online kwam */

  bool          _dirty = false;         /* instellingen nog niet weggeschreven */
  unsigned long _save_at = 0;

  void loopMonitors();
  /* Laat de gemelde diensten verouderen. Aparte lus en niet in applyResult():
   * er komt bij deze soort juist GEEN uitslag binnen, dus er is niets om aan
   * mee te liften. */
  void loopPushStale();
  /* Vanaf welk moment de stiltetijd van een gemelde dienst geteld wordt: de
   * laatste melding, of het einde van onze eigen wifi-insteltijd als die later
   * is. Zonder dat tweede zou een wifi-storing van ons alle gemelde diensten
   * onbekend maken zodra wij terugkomen. */
  unsigned long monitorStaleRef(int slot) const;
  uint32_t      monitorStaleMs(int slot) const;

  void startNextPing();
  void startResolve(int slot);
  void startPing(int slot);
  void abortPing();
  void applyResult(int slot, bool ok, uint32_t ms);

  uint8_t allocChannel();
  int     findByName(const char* name) const;
  int     findByChannel(uint8_t ch) const;
  int     slotOfNth(int nth) const;     /* nde bezette vakje -> vakjenummer */
  bool    addMonitor(const char* spec);
  bool    delMonitor(const char* name);
  void    markDirty();
};
