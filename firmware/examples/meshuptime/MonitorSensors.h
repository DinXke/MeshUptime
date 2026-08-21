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

/* ==================== GEBEURTENISSEN NAAR BUITEN (push) ====================
 *
 * PushTask moet DEZELFDE gebeurtenissen zien die alertIf() de mesh op stuurt:
 * storing, herstel en gelabelde simulatie, met dezelfde tekst en dezelfde
 * ernst. Geen dubbele waarheid: de gebeurtenis wordt aangemaakt op precies het
 * moment dat de melding voor de mesh GEARMD wordt (de eerste keer, niet bij
 * elke herhaling -- een herhaling is dezelfde storing nog eens, en de server
 * heeft zijn eigen herinnering).
 *
 * Een abstracte afnemer en geen #include van PushTask.h, om dezelfde reden als
 * bij setWifiTask(): main.cpp is de enige plek die beide objecten kent, en deze
 * klasse blijft te bouwen zonder de push-laag. */

/* Ruim boven de langste alerttekst (nagerekend 123 tekens bij een gesimuleerde
 * ping-monitor; zie s_alert_buf in de .cpp). */
#define MON_EVENT_TEXT_LEN 128

struct MonitorEvent {
  uint8_t ch;                        /* kanaal 2..36 */
  uint8_t up;                        /* 0 = storing ("neer"), 1 = herstel ("op") */
  uint8_t sim;                       /* 1 = gelabelde simulatie */
  uint8_t sev_high;                  /* 1 = hoog; onze meldingen zijn LOW_PRI, dus 0 */
  char    text[MON_EVENT_TEXT_LEN];  /* exact de tekst die de DM-weg kreeg */
};

class MonitorEventSink {
public:
  virtual void onMonitorEvent(const MonitorEvent& ev) = 0;
};

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
 *   5..36  monitors         LPP_SWITCH 0/1 (up) + LPP_GENERIC_SENSOR (ms)
 *
 * Een monitor op 5..36 is van EEN VAN TWEE SOORTEN, en de telemetrie ziet er
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
 * Per monitor, in het duurste geval (monitor is up EN stuurt zijn pingtijd mee):
 *
 *   LPP_SWITCH         2 + 1 = 3
 *   LPP_GENERIC_SENSOR 2 + 4 = 6
 *                      ---------
 *                              9
 *
 * Stuurt hij zijn pingtijd NIET mee (MonitorCfgEntry::send_ms == 0), dan is het
 * 3 byte. Dat is de knop waarmee dit budget bestuurd wordt.
 *
 * HET AANTAL IS NIET DE GRENS -- DE BYTES ZIJN DE GRENS.
 *
 * 180 - 24 = 156 byte over. Met pingtijd overal passen er 156 / 9 = 17; zonder
 * pingtijd 156 / 3 = 52. Er zijn 32 vakjes (kanaal 5..36), en dat getal is met
 * opzet HOGER dan wat er met pingtijd in past: zo is het budget de rem en niet de
 * teller, en dan is de grens uit te leggen ("er is nog 12 byte") in plaats van
 * willekeurig ("er mogen er 8"). Hoeveel er werkelijk in passen hangt af van hoe
 * ze staan, en dat staat live op de pagina.
 *
 * DRIE PLAATSEN REKENEN, EN DAT IS MET OPZET GEEN DUBBELING:
 *
 *   1. querySensors() rekent bij ELKE uitlezing na en kapt per monitor alles of
 *      niets af. Dat is het slot: een aanname die alleen in commentaar staat is
 *      geen aanname waar je op kunt bouwen. Wat er buiten valt wordt GEMARKEERD
 *      (_telem_dropped), zodat het niet stil gebeurt.
 *   2. createMonitor() weigert een monitor die er niet meer in past, met de
 *      uitweg in de foutmelding. Dat dekt /hook, de CLI en elk script.
 *   3. de webpagina rekent hetzelfde uit om het te KUNNEN ZEGGEN voordat iemand
 *      op opslaan drukt. Dat is gemak en niet het slot -- vandaar 1 en 2.
 *
 * Het VASTE deel (die 24) wordt niet aangenomen maar gemeten: zie
 * TelemBudget::base. Zodra iemand GPS aanzet is het 35, en een geschat getal zou
 * dan stil 11 byte ruimte beloven die er niet is.
 */
class MonitorSensors : public EnvironmentSensorManager {
public:
  /* Vaste kanaalnummers, ook bruikbaar voor de UI en de ping-monitors. */
  static const uint8_t CH_MAINS         = 2;
  static const uint8_t CH_BATTERY       = 3;
  static const uint8_t CH_WIFI          = 4;
  static const uint8_t CH_MONITOR_FIRST = 5;
  static const uint8_t CH_MONITOR_LAST  = 4 + MON_MAX_MONITORS;   /* 36 */

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
   * gebruik", "alle vakjes vol" en "past niet meer in het pakket" zijn drie
   * verschillende dingen voor wie
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
    MON_ERR_UNKNOWN,    /* naam bestaat niet (bij verwijderen) */
    /* Past niet meer in het telemetriepakket van 180 byte. Een EIGEN reden en
     * niet MON_ERR_FULL erbij, want de uitweg is een andere: bij 'vol' moet er
     * een monitor weg, hier is het genoeg om bij een paar monitors de pingtijd
     * uit te zetten (3 byte in plaats van 9). Wie die twee door elkaar haalt,
     * gooit een monitor weg en verbrandt een kanaal voor niets. */
    MON_ERR_BYTES
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

  /* Voor de UI, de webinterface en de waarschuwingen in main.cpp.
   *
   * DEZE TWEE GEVEN DE GERAPPORTEERDE WAARDE, NIET DE GEMETEN WAARDE. Staat er
   * een simulatie op (zie SIMULEREN hieronder), dan geven zij de geforceerde
   * stand -- want dat is precies de bedoeling: de telemetrie, de pagina, de
   * DM-lijst en de waarschuwingen horen allemaal hetzelfde te zien. De meting
   * loopt onderwater gewoon door; hij staat in _mains en in wifiReallyOnline(). */
  bool     isMains() const;
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
  /* Alarm-bezorging per monitor (room-variant): route (MON_ALERT_*) en room-set
   * (bitmasker van room-indexen). De sensor-variant negeert deze. */
  uint8_t     monitorAlertMode(int slot) const;
  uint16_t    monitorRoomsMask(int slot) const;
  /* SENSOR-NODE-set per monitor: op welke virtuele sensor-nodes deze sensor als
   * telemetrie-kanaal verschijnt (bitmasker). */
  uint16_t    monitorSensorNodesMask(int slot) const;
  /* Idem voor de vaste bronnen (MON_FA_* uit MonitorStore.h). */
  uint8_t     fixedAlertMode(int idx) const;
  uint16_t    fixedRoomsMask(int idx) const;
  uint16_t    fixedSensorNodesMask(int idx) const;
  bool        setFixedAlertMode(int idx, uint8_t mode);
  bool        setFixedRoomsMask(int idx, uint16_t mask);
  bool        setFixedSensorNodesMask(int idx, uint16_t mask);

  /* SUBSET-telemetrie voor een virtuele sensor-node: als querySensors(), maar met
   * ALLEEN de kanalen van de sensoren die aan sensor-node `snode_idx` gekoppeld
   * zijn (via hun sensornodes-masker). Zo kunnen sensoren over meerdere sensor-
   * nodes verdeeld worden, elk binnen de één-pakket-CayenneLPP-limiet. */
  bool        querySensorsForNode(int snode_idx, uint8_t requester_permissions, CayenneLPP& telemetry);

  /* ---- MUTE / SNOOZE (alerts tijdelijk dempen) + CHECKNOW ----
   * Een gemute sensor of een globaal gesnoozede node genereert geen dm/room/both
   * tot de tijd om is. main_room checkt isMuted(slot) vóór het alarm-dispatch.
   * Alle tijden in millis(); RAM-only (na herstart weer actief). */
  int         findByNameOrChannel(const char* s) const;              // slot, of -1
  bool        setMute(const char* name_or_ch, unsigned long secs);   // secs 0 = wissen
  bool        clearMute(const char* name_or_ch);
  void        setSnooze(unsigned long secs);                          // 0 = wissen
  bool        isSnoozed() const;
  unsigned long snoozeSecsLeft() const;
  bool        isMuted(int slot) const;         // slot-mute OF globale snooze
  unsigned long muteSecsLeft(int slot) const;  // resterend voor DEZE slot (0 = niet gemute)
  bool        checkNow(const char* name_or_ch);   // forceer meting; leeg/NULL = alle

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

  /* ============== HET BYTEBUDGET VAN DE TELEMETRIE ==============
   *
   * DE ECHTE GRENS ZIJN BYTES EN NIET MONITORS.
   *
   * SensorMesh maakt zijn CayenneLPP als MAX_PACKET_PAYLOAD - 4 = 180 byte, en
   * daar moet alles in: de batterijspanning, de GPS als die aanstaat, eventuele
   * omgevingssensoren, onze drie vaste schakelaars en dan de monitors. Een
   * monitor kost 9 byte mét pingtijd en 3 byte zonder, dus er passen er zeventien
   * of ruim vijftig. Een teller ("hoogstens 8") kan dat verschil niet uitdrukken
   * en is dus altijd of te krap of onwaar.
   *
   * WAAROM DIT ZICHTBAAR MOET ZIJN. Wie zijn achttiende monitor aanmaakt en
   * daarna merkt dat er willekeurig een paar kanalen uit zijn telemetrie
   * verdwenen zijn, heeft geen foutmelding gekregen maar wel verkeerde gegevens
   * op zijn dashboard. Het budget tonen is daarom geen sier: het is het verschil
   * tussen een begrijpelijke grens en een stille fout.
   *
   * HET VASTE DEEL WORDT GEMETEN EN NIET GESCHAT. querySensors() kijkt hoeveel
   * byte de basisklasse verbruikt heeft voordat wij aan de beurt zijn, en bewaart
   * dat. Zo klopt het getal ook als er GPS bij komt (11 byte) of ooit een I2C
   * omgevingssensor -- twee dingen die een geschat getal stil verkeerd zouden
   * maken. Tot de eerste uitlezing staat er de bekende ondergrens (de 4 byte van
   * de batterijspanning); de pagina zegt erbij dat het na de eerste ronde
   * definitief is.
   */
  static const uint8_t TELEM_BYTES_SWITCH_PUB  = 3;    /* LPP_SWITCH: 2 kop + 1 */
  static const uint8_t TELEM_BYTES_GENERIC_PUB = 6;    /* LPP_GENERIC_SENSOR: 2 + 4 */

  struct TelemBudget {
    uint8_t total;      /* altijd MAX_PACKET_PAYLOAD - 4 = 180 */
    uint8_t base;       /* wat de basisklasse gebruikt (spanning, GPS, omgeving) */
    uint8_t fixed;      /* base + onze drie vaste schakelaars */
    uint8_t mons;       /* som van de monitors */
    uint8_t used;       /* fixed + mons */
    uint8_t left;       /* total - used, of 0 */
    uint8_t num_ms;     /* hoeveel monitors hun pingtijd meesturen */
    uint8_t dropped;    /* hoeveel er bij de LAATSTE uitlezing buiten vielen */
    bool    measured;   /* is 'base' al een keer echt gemeten? */
  };
  void telemBudget(TelemBudget& out) const;

  /* Wat één monitor in het pakket kost: 3 of 9 byte. 0 voor een leeg vakje. */
  uint8_t monitorTelemBytes(int slot) const;

  /* Gaat de pingtijd van dit vakje mee over het mesh? De METING loopt door en
   * blijft zichtbaar op de pagina en in 'sensor list'; dit gaat alleen over wat
   * er de ether in gaat. */
  bool monitorSendsMs(int slot) const;

  /* Viel dit vakje bij de laatste uitlezing BUITEN het pakket? Dan staat hij niet
   * in de telemetrie en ziet een dashboard hem niet -- en dat hoort niemand zelf
   * te hoeven ontdekken. */
  bool monitorDropped(int slot) const;

  /* Past er nog een monitor bij met deze instelling? Voor de keuring bij het
   * aanmaken: de pagina rekent het ook uit, maar de pagina is gemak en niet het
   * slot. */
  bool telemFits(uint8_t extra_bytes) const;

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

  /* ==================== HERSTELMELDINGEN ====================
   *
   * HET GAT DAT DIT DICHT
   *
   * alertIf() vuurt bij de overgang naar waar en ruimt bij onwaar alleen de
   * Trigger op -- er gaat dan GEEN bericht uit. Je krijgt dus "router
   * onbereikbaar" en daarna nooit meer iets. En dan is er geen verschil te zien
   * tussen "het is opgelost" en "de node is zelf gestopt met melden", terwijl dat
   * tweede juist het geval is dat je wil onderscheiden. Een bewakingsnode die
   * alleen slecht nieuws stuurt, laat je in het ongewisse over zijn eigen
   * gezondheid.
   *
   * EEN TWEEDE TRIGGER PER VAKJE, LANGS HETZELFDE alertIf()
   *
   * main.cpp krijgt naast monitor_down[] een rij monitor_up[], en hangt daar
   * recoverAlert()/recoverAlertText() aan. Geen eigen verzendweg: zo geldt de
   * bezorgcontrole met expected_acks ook voor een herstelmelding, en blijft er
   * één plek waar waarschuwingen ontstaan.
   *
   * De voorwaarde is waar voor een KORT VENSTER (RECOVER_HOLD_MS) en daarna
   * onwaar, want een herstelmelding is een gebeurtenis en geen toestand. Zou zij
   * waar blijven zolang de dienst op is, dan bleef er voor elke gezonde monitor
   * een plaats in de wachtrij bezet -- en die zijn er maar vier.
   *
   * DRIE VOORWAARDEN, en elk repareert een manier waarop dit hinderlijk zou zijn:
   *
   *  1. ER MOET EEN STORINGSMELDING UIT ZIJN GEGAAN. Niet "hij was down", maar
   *     "wij hebben het gemeld". Een dienst die tijdens de insteltijd even wegviel
   *     of die korter plat lag dan één leesronde, is nooit gemeld -- en dan is
   *     "weer bereikbaar" een antwoord op een vraag die niemand gesteld heeft.
   *     Daarom telt down_sent en niet down.
   *  2. DE DIENST MOET rhold_s AANEENGESLOTEN OP ZIJN. Dat is de rem tegen
   *     flappen; zie MonitorCfg.
   *  3. DE MELDING MOET AANSTAAN (recover_alerts).
   *
   * PRIORITEIT: LOW_PRI_ALERT, altijd, ook als de storing HIGH_PRI was. Dat is
   * een bewuste afweging en niet een detail: een gemiste "het werkt weer" is
   * hinderlijk, een gemiste "het is stuk" is erger. LOW_PRI doet één poging per
   * contact in plaats van vier, dus een herstelgolf -- bij een internetstoring
   * komen alle monitors ongeveer gelijk terug -- kost een kwart van de zendtijd.
   * De MAX_MONITOR_ALERTS-rem geldt er bovenop, dus die golf wordt hoe dan ook
   * uitgesmeerd in plaats van tegelijk uitgezonden.
   *
   * DE VASTE KANALEN DOEN MEE. Netvoeding terug en wifi terug horen er net zo
   * bij; fixedRecoverAlert() werkt op dezelfde twee nummers als fixedAlert().
   *
   * EEN GEMELDE DIENST DIE UIT 'STIL' TERUGKOMT IS GEEN HERSTEL, en dat is de
   * verdedigbare regel die hier gekozen is. 'Stil' betekent dat de MELDER zweeg
   * en dat wij dus niets wisten -- de toestand was ONBEKEND, niet neer. Wie een
   * bericht kreeg dat de melder stil was, hoort te horen dat de melder weer
   * meldt, en dat is precies wat er dan gebeurt: ook de stilte-waarschuwing zet
   * down_sent, dus de herstelmelding komt met een eigen tekst ("meldt weer") en
   * niet met "weer bereikbaar". Van onbekend naar op zonder voorafgaande melding
   * levert niets, want dan is voorwaarde 1 niet gehaald.
   */
  static const uint32_t RECOVER_HOLD_MS = 45000;

  bool        recoverAlert(int slot);
  const char* recoverAlertText(int slot) const;
  bool        fixedRecoverAlert(int which);
  const char* fixedRecoverAlertText(int which) const;

  /* Voor de pagina: staat de herstelmelding aan, en hoe lang is de rustperiode?
   * Zetten gaat via "sensor set alert.recover" / "alert.rhold" -- dus over de CLI,
   * net als elke andere instelling, zodat er geen tweede schrijfpad ontstaat. */
  bool     recoverEnabled() const { return _cfg.recover_alerts != 0; }
  uint16_t recoverHoldSecs() const { return _cfg.rhold_s; }

  /* ==================== HERHALEN TOT EEN MENS BEVESTIGT ====================
   *
   * WAAROM, EN WAT HET NIET IS
   *
   * alertIf() vuurt eenmaal per storingsovergang, doet zijn vier
   * TRANSPORT-pogingen en gaat dan stil -- de Trigger blijft staan maar er gaat
   * niets meer uit tot de storing over is. Een transport-ACK bewijst alleen dat
   * een PAKKET aankwam, niet dat een MENS keek. Bjorn wil het tweede: de melding
   * herhaalt tot een companion een DM met "ok" terugstuurt. Als een pieper die
   * blijft piepen tot iemand hem indrukt.
   *
   * HOE HET DOOR HET BESTAANDE alertIf LOOPT, ZONDER TWEEDE VERZENDWEG
   *
   * main.cpp roept elke leesronde alertIf(monitorAlert(i), monitor_down[i], ...)
   * aan. alertIf her-verstuurt alleen op een OVERGANG van de voorwaarde (onwaar
   * -> waar). Om te herhalen laat monitorAlert() de voorwaarde daarom één ronde
   * op onwaar vallen en de ronde erna weer op waar -- een "puls laag". alertIf
   * ruimt bij de onwaar de Trigger op en herbouwt hem bij de waar, en zijn eigen
   * lus loopt dan opnieuw langs alle ontvangers. Er is dus geen tweede pad en
   * geen tweede boekhouding: de herhaling spreekt exact de taal die alertIf al
   * kent. Gevolg: een herhaling is een echte her-arm en gaat door DEZELFDE
   * MAX_MONITOR_ALERTS-rem als een eerste melding -- een golf storingen die
   * tegelijk herhaalt, verdringt elkaar netjes in de wachtrij in plaats van de
   * band vol te zetten. De puls-laag freeft bovendien de plaats even, zodat een
   * wachtende storing ertussen kan.
   *
   * Omdat alertIf alleen uit onSensorDataRead() loopt (elke leesronde), is de
   * herhaalperiode grofweg op de leesronde afgerond -- en dat is precies waarom
   * de ondergrens 60 s is. De klik die de herhaling STOPT ("ok") werkt wel
   * meteen: human_ack laat monitorAlert() de eerstvolgende keer onwaar geven.
   *
   * DRIE STOPVOORWAARDEN: de storing is voorbij (dienst weer op), OF een companion
   * bevestigde met "ok", OF de harde bovengrens MAX_ALERT_REPEATS is bereikt.
   *
   * SAMENHANG MET DE HERSTELMELDING: down_sent blijft staan zolang de storing
   * loopt, dus als hij oplost -- of iemand nu "ok" stuurde of niet -- gaat de
   * herstelmelding gewoon uit. "ok" bevestigt de MELDING, niet de storing. */

  /* De harde bovengrens op het aantal HERHALINGEN (de eerste melding telt niet
   * mee). Zonder grens vult een node die niemand bevestigt eeuwig de band.
   *
   * 12 is verdedigbaar: bij de standaardperiode van 300 s is dat een uur lang
   * herhalen, en een storing die na een uur nog door niemand is bevestigd, heeft
   * een groter probleem dan een gemiste melding -- doorgaan kost dan alleen
   * zendtijd. Bij de ondergrens van 60 s is het ~12 minuten; ook dan is "niemand
   * reageert" op zichzelf al het signaal. Bij het bereiken ervan stopt het
   * herhalen (de monitor blijft op de pagina en in de telemetrie gewoon down) en
   * komt er een regel in de debug-uitvoer en op de pagina. */
  static const uint8_t MAX_ALERT_REPEATS = 12;

  /* Herkent een DM-tekst als bevestiging: "ok" of "ack", hoofdletterongevoelig,
   * na het wegknippen van spaties. STATISCH en publiek zodat main.cpp hem in
   * handleIncomingMsg kan aanroepen -- de herkenning hoort op één plek en niet
   * half in main.cpp. Werkt op de ruwe (mogelijk niet-afgesloten) DM-payload. */
  static bool isAckText(const uint8_t* data, size_t len);

  /* Een menselijke bevestiging verwerken. Zet human_ack op ALLE op dit moment
   * openstaande storingsmeldingen (monitors én de vaste kanalen) en geeft terug
   * hoeveel er zo gestopt zijn. De PERMISSIECONTROLE zit NIET hier maar in
   * main.cpp: die kent de ClientInfo en dus het alarmrecht, deze klasse niet.
   * Een "ok" van iemand zonder alarmrecht hoort main.cpp dus niet door te geven. */
  uint8_t confirmAlerts();

  /* Hetzelfde als confirmAlerts(), maar voor EEN kanaal: het antwoord van de
   * push-server bevestigt per kanaal, de ok-DM bevestigt alles. Kanaal 2/3 is
   * de voeding (een meting met twee namen), 4 wifi, 5..36 een monitor. Geeft 1
   * als er een openstaande melding gestopt is, anders 0.
   *
   * Een server-bevestiging gaat NIET in de "acked" van de volgende push mee:
   * die lijst is voor wat er op de NODE bevestigd is, en de server weet zijn
   * eigen bevestigingen al. */
  uint8_t confirmAlertChannel(uint8_t ch);

  /* De kanalen die sinds de vorige afname op de NODE bevestigd zijn (de ok-DM),
   * als bitmasker (bit n = kanaal n). Afnemen WIST het masker hier; PushTask
   * houdt het zelf vast tot de POST gelukt is, zodat een mislukte push geen
   * bevestiging verliest. */
  uint64_t takePushAcked() { uint64_t v = _push_acked; _push_acked = 0; return v; }

  /* ==================== gebeurtenis-haak voor PushTask ====================
   * Zie de uitleg bij MonitorEventSink bovenaan dit bestand. Zelfde patroon als
   * setWifiTask(): main.cpp legt de koppeling, deze klasse kent PushTask niet. */
  void setEventSink(MonitorEventSink* sink) { _events = sink; }

  /* De push-instellingen, voor PushTask. Een lege url betekent: push uit. */
  const char* pushUrl() const     { return _cfg.push_url; }
  const char* pushToken() const   { return _cfg.push_token; }
  uint16_t    pushHbSecs() const  { return _cfg.push_hb_s; }

  /* Voor de pagina: de herhaalperiode, hoeveel meldingen nu actief herhaald
   * worden, en hoeveel er de bovengrens raakten. */
  uint16_t alertRepeatSecs() const { return _cfg.repeat_s; }
  void     repeatStatus(uint8_t& nagging, uint8_t& capped) const;

  /* Uitkomst van simSet(), testRequest() en startAdhocPing(). Zelfde gedachte als
   * MonResult: wie aan de andere kant staat moet WETEN waarom het niet ging, en
   * de keuring hoort op één plek te zitten en niet half in WebTask of DmCommands.
   * Staat hier (en niet bij de SIMULEREN-sectie) omdat de ad-hoc ping hierboven
   * hem al gebruikt; een enum moet vóór zijn eerste gebruik gedefinieerd zijn. */
  enum SimResult : uint8_t {
    SIM_OK = 0,
    SIM_ERR_INDEX,      /* geen bestaande sensor/vakje, of een ongeldig adres */
    SIM_ERR_SECS,       /* vervaltijd buiten SIM_SECS_MIN..MAX */
    SIM_ERR_FULL,       /* MAX_SIM_ACTIVE bereikt */
    SIM_ERR_GAP,        /* te snel achter elkaar (SIM_GAP_MS / TEST_GAP_MS) */
    SIM_ERR_BUSY        /* er loopt al een testbericht of een ad-hoc ping */
  };

  /* ==================== SENSORBEHEER PER DM ====================
   *
   * add/edit/del over een DM, met exact DEZELFDE keuring en DEZELFDE schrijfweg
   * als de web-GUI en de CLI: createMonitor(), deleteMonitor() en de
   * settings-zeef (setSettingValue op "mon.<kanaal>.*"). Geen tweede schrijfpad,
   * geen tweede keuring -- die zouden ooit uiteenlopen.
   *
   * Deze ENE methode neemt de al-uit-de-DM-geknipte regel en geeft de
   * antwoordtekst terug (of NULL als de regel niet met add/edit/del begint, zodat
   * de DM-afhandeling naar zijn eigen list/get/status/help kan doorvallen). De
   * DM-plumbing -- het herkennen van de regel, de knipper, de ACK-opvolging en de
   * admin-check (onPeerDataRecv laat DM-tekst alleen door voor from->isAdmin())
   * -- blijft in DmCommands; dat is met opzet, want DmCommands kent deze klasse
   * niet en praat alleen met zijn DmDataSource.
   *
   * SYNTAX (argumenten door spaties gescheiden):
   *   add <naam> <adres> [interval]
   *   edit <naam|kanaal> [host=<adres>] [int=<secs>] [naam=<nieuw>] [ms=0|1]
   *   del <naam|kanaal>
   *
   * NAAM vs KANAAL: een argument dat ALLEEN uit cijfers bestaat is een
   * kanaalnummer, al het andere is een naam. del accepteert alleen een EXACTE
   * naam of een exact kanaal -- nooit een prefix, want stil de verkeerde monitor
   * wissen kost een kanaal dat niet terugkomt.
   *
   * Het antwoord past in de bestaande knipper (kan meerdere stukken worden). */
  const char* handleDmMonCommand(const char* line);

  /* De helpregels voor de DM-commando's van deze klasse (add/edit/del en ping),
   * zodat DmCommands ze bij zijn eigen `help` kan zetten zonder ze over te typen
   * -- één plek voor de syntax. */
  static const char* dmCommandHelp();

  /* ==================== AD-HOC PING ====================
   *
   * `ping <adres> [n]` -- n keer pingen naar een vrij op te geven adres, uitslag
   * per DM terug. De kern is TIMING: de DM-afhandeling draait in de mesh-loop, en
   * n pings duren seconden, dus dit mag NIET blokkeren. Daarom: startAdhocPing()
   * keurt en start, en het antwoord komt LATER -- DmCommands vraagt elke ronde
   * adhocReady() en stuurt zodra de uitslag klaar is één antwoord-DM met alles
   * erin (niet één per ping -- dat is zendtijd).
   *
   * EEN PING TEGELIJK BLIJFT DE WET. Er is precies één esp_ping-sessie in dit
   * bestand (s_ping). De ad-hoc ping deelt die, en krijgt VOORRANG: loopt er een
   * monitorronde, dan wordt die afgebroken (niet als mislukking geteld -- zie
   * abortPing) en schuift op. De keuze is bewust: een mens wacht op zijn uitslag,
   * een monitor niet. Was de start vertraagd, dan staat dat in het antwoord.
   *
   * EEN AD-HOC TEGELIJK. Eén pending-slot, geen wachtrij: een tweede `ping`
   * terwijl er één loopt, wordt geweigerd met "bezig met <adres>".
   *
   * DE KOPPELING AAN DE VRAGER zit NIET hier. Deze klasse weet niet wie het
   * vroeg; ze levert alleen de uitslag en een "klaar"-vlag. DmCommands onthoudt de
   * vrager -- op PUBLIEKE SLEUTEL en niet op contact-index, want indexen
   * verschuiven als de ACL verandert -- en zoekt het contact opnieuw op wanneer de
   * uitslag klaar is. Is die sleutel dan uit de ACL verdwenen, dan vervalt het
   * antwoord (de uitslag wordt geconsumeerd en weggegooid). */
  static const uint8_t ADHOC_DEFAULT_PINGS = 3;
  static const uint8_t ADHOC_MAX_PINGS     = 5;
  static const uint32_t ADHOC_TIMEOUT_MS   = 2000;   /* per ping */

  enum AdhocState : uint8_t {
    ADHOC_NONE = 0,   /* niets gevraagd, of de uitslag is opgehaald */
    ADHOC_PENDING,    /* gevraagd, wacht tot de pingmachine vrij is */
    ADHOC_BUSY,       /* bezig met opzoeken of pingen */
    ADHOC_DONE        /* klaar; adhocResultText() staat klaar tot adhocClear() */
  };

  /* Start een ad-hoc ping. n == 0 -> ADHOC_DEFAULT_PINGS; boven ADHOC_MAX_PINGS
   * wordt afgekapt. Zelfde adreszeef als overal (validHost). Zonder wifi zet dit
   * meteen een klare uitslag "geen wifi, niet gepingd" -- eerlijk en zonder te
   * pingen. */
  SimResult startAdhocPing(const char* host, uint8_t n);

  AdhocState  adhocState() const { return (AdhocState)_adhoc.state; }
  bool        adhocReady() const { return _adhoc.state == ADHOC_DONE; }
  /* De uitslagtekst: per ping de tijd of "timeout", plus een slotregel
   * "x/y ok, min/gem/max ms" en hoe oud de uitslag is. Zinvol zodra adhocReady().
   * Geen String; vaste buffer. */
  const char* adhocResultText() const;
  /* De uitslag is verstuurd (of de vrager is weg): slot vrijgeven. */
  void        adhocClear();
  /* Het adres waar we nu mee bezig zijn -- voor de "bezig met <adres>"-weigering
   * en voor het help/statusantwoord. */
  const char* adhocHost() const { return _adhoc.host; }

  /* ==================== SIMULEREN EN TESTEN ====================
   *
   * WAAROM DIT ER IS
   *
   * De waarschuwingen hierboven zijn gebouwd maar nog nooit afgegaan. Een node
   * die pas bij een echte storing voor het eerst een bericht stuurt, is een node
   * waarvan niemand weet of dat bericht aankomt -- en dan blijkt het op het
   * slechtste moment. Alles hieronder bestaat om dat vooraf te weten.
   *
   * HET GAAT DOOR HET ECHTE PAD, EN DAT IS DE HELE ONTWERPEIS.
   *
   * Een forcering verandert wat monitorIsUp(), monitorAlert(), isMains() en
   * isWifiOnline() TERUGGEVEN. Daarmee doet alertIf() in main.cpp gewoon zijn
   * werk: hij vuurt op de overgang, kiest de contacten met PERM_RECV_ALERTS_*,
   * doet zijn pogingen en wacht op echte ACK's. Er is dus GEEN tweede verzendweg
   * en GEEN nepbericht -- die zouden de test testen in plaats van het systeem.
   * Om dezelfde reden werkt de forcering door in querySensors(): een dashboard
   * aan de andere kant hoort hetzelfde te zien als deze node.
   *
   * DE MEETWAARDE WORDT NOOIT OVERSCHREVEN. Een forcering is een LAAG die er
   * bovenop ligt en niet een waarde die in _mon[] of _mains geschreven wordt.
   * Dat is geen nettigheid: bij het aflopen moet er teruggevallen worden op de
   * waarde die er ONDERTUSSEN gemeten is, en die is er alleen nog als niemand
   * hem overschreven heeft.
   *
   * DE NUMMERING. Eén rij voor alles wat te simuleren is, en die loopt gelijk met
   * de vakjenummering van de monitors:
   *
   *   0                netvoeding EN batterijvoeding (kanaal 2 en 3)
   *   1                wifi (kanaal 4)
   *   2 .. 2+MAX-1     monitorvakje 0 .. MAX_MONITORS-1 (kanaal 5..12)
   *
   * WAAROM NETVOEDING EN BATTERIJVOEDING EEN INGANG ZIJN, en dus niet twee.
   * Kanaal 3 is per definitie de spiegel van kanaal 2 (zie querySensors): het is
   * één meting met twee namen. Twee losse forceringen zouden "netvoeding aan" en
   * "batterijvoeding aan" tegelijk kunnen zetten, en dat is een toestand die
   * fysisch niet bestaat -- een dashboard dat hem krijgt kan er niets goeds mee
   * doen. Eén ingang die beide kanalen tegelijk beweegt is de enige stand die
   * altijd klopt. De pagina toont beide regels als gesimuleerd.
   *
   * WAAROM DE PING-MACHINE DE ECHTE WIFI BLIJFT GEBRUIKEN. Een forcering van
   * kanaal 4 verandert wat wij MELDEN over onze wifi, niet of wij nog pingen.
   * Andersom zou het twee dingen stukmaken: "wifi neer" forceren zou alle
   * monitors bevriezen en hun waarschuwingen onderdrukken (monitorsPaused), dus
   * één simulatie zou stil de hele bewaking uitzetten -- precies de storing die
   * de vervaltijd hieronder moet voorkomen. En "wifi op" forceren terwijl er
   * geen netwerk is, zou acht monitors laten pingen over een verbinding die er
   * niet is en ze onterecht down verklaren. Vandaar: loopMonitors() en
   * monitorsPaused() kijken naar wifiReallyOnline(), al het overige naar
   * isWifiOnline().
   */
  static const uint8_t SIM_POWER     = 0;
  static const uint8_t SIM_WIFI      = 1;
  static const uint8_t SIM_MON_FIRST = 2;
  static const uint8_t SIM_COUNT     = SIM_MON_FIRST + MAX_MONITORS;   /* 10 */

  enum SimMode : uint8_t {
    SIM_OFF = 0,        /* geen forcering: de gemeten waarde geldt */
    SIM_UP  = 1,        /* forceer de GOEDE stand (net aan / wifi online / dienst op) */
    SIM_DOWN = 2        /* forceer de SLECHTE stand -- dit is wat een waarschuwing geeft */
  };

  /* DE VERVALTIJD -- de belangrijkste afspraak van dit hele onderdeel.
   *
   * Een forcering die blijft staan zet een monitor stil uit: hij meldt dan niet
   * meer wat er echt gebeurt, en niemand ziet dat. Een node die na een test in
   * testmodus blijft hangen is erger dan een node zonder testknop. Daarom heeft
   * elke forcering een einde en is dat einde niet optioneel: er is geen stand
   * "voor altijd". Standaard tien minuten, hoogstens een uur.
   *
   * De ondergrens van 30 s is er zodat een forcering de leesronde van
   * SENSOR_READ_INTERVAL_SECS (60 s) kan halen -- alertIf() wordt alleen uit
   * onSensorDataRead() aangeroepen, dus een forcering van 10 s zou kunnen
   * vervallen voordat hij ooit gezien is, en dan test je niets. */
  static const uint16_t SIM_SECS_DEFAULT = 600;
  static const uint16_t SIM_SECS_MIN     = 30;
  static const uint16_t SIM_SECS_MAX     = 3600;

  /* DE REM. Elke simulatie kost echte zendtijd op een gedeelde band.
   *
   * Hoogstens twee forceringen tegelijk, en dat is niet willekeurig: het is
   * dezelfde grens als MAX_MONITOR_ALERTS, en die grens bestaat omdat
   * MAX_CONCURRENT_ALERTS 4 is en de batterij er twee gebruikt. Zouden er meer
   * forceringen tegelijk mogen, dan zou de wachtrij van alertIf() vollopen en
   * zou een ECHTE batterijwaarschuwing stil overgeslagen worden. De test mag
   * nooit de bewaking verdringen.
   *
   * SIM_GAP_MS is er tegen flapperen: wie een sensor snel op en neer zet, stuurt
   * met elke overgang een DM-keten naar alle ontvangers. */
  static const uint8_t  MAX_SIM_ACTIVE = MAX_MONITOR_ALERTS;   /* = 2 */
  static const uint32_t SIM_GAP_MS     = 10000;

  static const char* simResultText(SimResult r);

  /* Een forcering zetten of opheffen. mode == SIM_OFF heft op en heeft geen
   * vervaltijd nodig; secs == 0 betekent SIM_SECS_DEFAULT. */
  SimResult simSet(uint8_t idx, SimMode mode, uint16_t secs);
  /* Alles in één keer terug naar de meting. De noodknop op de pagina. */
  void      simClearAll();

  SimMode   simMode(uint8_t idx) const;
  uint32_t  simSecsLeft(uint8_t idx) const;      /* 0 als er niets geforceerd is */
  uint8_t   simActiveCount() const;
  bool      simActive() const { return simActiveCount() > 0; }
  /* Sensornummer -> vakjenummer, en terug. -1 als het niet van toepassing is. */
  static int simIndexOfSlot(int slot) { return SIM_MON_FIRST + slot; }

  /* ---------------- de vaste kanalen krijgen een waarschuwing ----------------
   *
   * Voor de monitors bestond monitorAlert() al; voor netvoeding en wifi bestond
   * er niets -- main.cpp hing er geen Trigger aan. Zonder zo'n voorwaarde is een
   * forcering van kanaal 2/3/4 alleen in de telemetrie te zien en gaat er nooit
   * een bericht uit, en dan is juist het deel dat "anders niet te testen is
   * zonder de stekker eruit te trekken" nog steeds niet te testen.
   *
   * DEZE VOORWAARDE IS WAAR ZOLANG ER EEN FORCERING OP STAAT, en niet bij een
   * echte storing. Dat is een keuze en geen vergissing:
   *
   *  - een echte alarmering op netvoeding en wifi is NIET gevraagd, en stil
   *    nieuw alarmgedrag invoeren is het soort verrassing dat je om 3 uur 's
   *    nachts ontdekt;
   *  - er is geen ruimte voor: van de vier plaatsen in MAX_CONCURRENT_ALERTS
   *    gebruikt de batterij er twee, en twee permanente kanalen erbij zouden de
   *    monitorwaarschuwingen volledig verdringen.
   *
   * Wat WEL getest wordt is de hele aflevering: alertIf, de contactkeuze op
   * PERM_RECV_ALERTS_*, het pakket, de pogingen en de ACK's. Wie de echte
   * storing ook wil laten alarmeren, hoeft in fixedAlert() alleen de regel te
   * veranderen die op de forcering kijkt -- dat staat er met zoveel woorden bij.
   */
  static const uint8_t FIXED_POWER       = 0;
  static const uint8_t FIXED_WIFI        = 1;
  static const uint8_t FIXED_ALERT_COUNT = 2;

  bool        fixedAlert(int which);
  const char* fixedAlertText(int which) const;

  /* ---------------- het testbericht ----------------
   *
   * Los van alle sensoren: één kort bericht naar de ontvangers met
   * PERM_RECV_ALERTS_LO, alleen om te zien of aflevering werkt.
   *
   * OOK DIT GAAT DOOR alertIf(). main.cpp hangt er één Trigger aan; er is geen
   * eigen verzendweg. Gevolg dat de pagina moet vertellen: het bericht gaat pas
   * de deur uit bij de volgende LEESRONDE, want alertIf() wordt alleen uit
   * onSensorDataRead() aangeroepen en dat is elke SENSOR_READ_INTERVAL_SECS
   * (60 s). Vandaar de toestand TEST_PENDING -- "aangevraagd" en "verstuurd" zijn
   * twee dingen, net zoals "verstuurd" en "aangekomen" dat zijn.
   *
   * VERSTUURD IS NIET AANGEKOMEN, en dat verschil is de hele reden dat er op
   * ACK's gelet wordt. testRecipients() is hoeveel ingangen op het moment van
   * aanvragen het alarmrecht hadden; testAcks() is hoeveel er bevestigd hebben.
   * Staan die twee niet gelijk, dan is er iemand die het bericht niet krijgt --
   * en dat is precies wat je wil weten VOORDAT er iets stuk is.
   */
  enum TestState : uint8_t {
    TEST_IDLE = 0,      /* nog nooit, of de uitslag is opgehaald */
    TEST_PENDING,       /* aangevraagd, wacht op de volgende leesronde */
    TEST_SENDING,       /* alertIf() heeft hem opgepakt; ACK's worden geteld */
    TEST_DONE           /* klaar; de uitslag blijft staan tot de volgende test */
  };

  /* Hoogstens één testbericht per minuut. Zendtijd is gedeeld, en een knop die
   * je vijf keer achter elkaar kunt indrukken is vijf DM-ketens naar iedereen. */
  static const uint32_t TEST_GAP_MS = 60000;
  /* Hoe lang de voorwaarde waar blijft nadat alertIf() hem opgepakt heeft.
   * alertIf() loopt de contacten AF met ALERT_ACK_EXPIRY_MILLIS (8 s) per
   * ontvanger; de voorwaarde moet waar blijven tot dat rondje klaar is, anders
   * breekt hij halverwege af. 15 s + 10 s per ontvanger, met een bovengrens,
   * want de plaats in de wachtrij is er één van twee. */
  static const uint32_t TEST_HOLD_BASE_MS = 15000;
  static const uint32_t TEST_HOLD_PER_MS  = 10000;
  static const uint32_t TEST_HOLD_MAX_MS  = 180000;
  /* Wordt de aanvraag niet opgepakt (geen plaats in de wachtrij), dan vervalt
   * hij. Een aanvraag die eeuwig blijft wachten is een knop die niets deed. */
  static const uint32_t TEST_PENDING_MAX_MS = 300000;

  /* recipients = hoeveel ingangen NU het alarmrecht hebben. Die telling hoort bij
   * de toegangslijst en die kent deze klasse niet; WebTask geeft hem mee. Zo
   * blijft de sensorlaag los van de meshlaag, net als bij setWifiTask(). */
  SimResult testRequest(uint8_t recipients);

  /* De twee aanroepen voor main.cpp, naast die van de monitors:
   *
   *   alertIf(sensors.testAlert(), alert_test, LOW_PRI_ALERT,
   *           sensors.testAlertText());
   */
  bool        testAlert();
  const char* testAlertText() const;

  /* Een ACK doorgeven. main.cpp ziet ze in onAckRecv() en kent de Trigger; wij
   * doen de vergelijking en de telling, zodat die logica hier staat en niet in
   * main.cpp. Geeft true als deze ACK bij het testbericht hoorde.
   *
   * Waarom de vergelijking hier en niet daar: expected_acks[] is een veld van
   * Trigger en dus van SensorMesh, maar wat er MEE moet gebeuren (niet dubbel
   * tellen, alleen tellen zolang de test loopt) hoort bij de test. */
  bool noteAlertAck(uint32_t ack_crc, const uint32_t* expected, uint8_t n);

  TestState testState() const;
  uint8_t   testRecipients() const { return _test_recipients; }
  uint8_t   testAcks() const { return _test_acks; }
  uint16_t  testSeq() const { return _test_seq; }
  uint32_t  testAgeSecs() const;      /* sinds de aanvraag; 0 als er niets was */
  uint32_t  testWaitSecs() const;     /* nog te wachten voor een volgende test */

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
  /* Gedeelde kern van querySensors()/querySensorsForNode(): schrijft de node-
   * toestandsschakelaars (netvoeding/batterij/wifi) en de monitorkanalen in de
   * CayenneLPP. snode_filter < 0 = ALLE kanalen (volledige telemetrie); >= 0 =
   * alleen de kanalen waarvan het sensornodes-masker die node bevat (subset). */
  void emitStateAndMonitors(uint8_t requester_permissions, CayenneLPP& telemetry, int snode_filter);

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

  /* De afnemer van gebeurtenissen (PushTask). NULL = niemand luistert, en dan
   * kost emitEvent() niets. */
  MonitorEventSink* _events = NULL;

  /* Bitmasker van kanalen die op de node bevestigd zijn (ok-DM) en nog niet in
   * een gelukte push gemeld. bit n = kanaal n; met kanalen tot 36 moet dit een
   * uint64_t zijn. RAM en niet SPIFFS: na een herstart is er niets meer te
   * melden, want dan is er ook geen openstaande melding meer. */
  uint64_t _push_acked = 0;

  /* Een gebeurtenis aan de afnemer geven. De tekst wordt GEKOPIEERD (de bron is
   * s_alert_buf en die is zo weer overschreven). */
  void emitEvent(uint8_t ch, bool up, bool sim, const char* text);

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
  /* De staat van "herhalen tot een mens bevestigt", voor één alarm-ingang.
   * Zowel een monitorvakje als een vast kanaal heeft er één; de logica in
   * stepStoringAlert() werkt op referenties zodat beide soorten hetzelfde recept
   * volgen. Geen SPIFFS: dit is meetwerk, geen instelling, en na een herstart is
   * er per definitie niets te herhalen. */
  struct AlertRepeat {
    bool          human_ack;     /* een companion bevestigde met "ok" */
    bool          pulse;         /* wij lieten de voorwaarde deze ronde vallen om
                                    hem volgende ronde te her-armen (de "puls laag") */
    bool          max_logged;    /* de bovengrens is al in de log gemeld */
    uint8_t       repeats;       /* aantal HERHALINGEN (de eerste melding telt niet) */
    unsigned long next_repeat;   /* millis waarop de volgende herhaling mag */
  };

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

    /* ---- voor de herstelmelding ----
     *
     * down_sent is het antwoord op "hebben WIJ deze storing gemeld", en niet op
     * "was hij down". Dat verschil is voorwaarde 1 hierboven: zonder dit veld zou
     * een dienst die even wegviel zonder dat er ooit iemand iets van hoorde, een
     * "weer bereikbaar" opleveren -- een antwoord op een vraag die niemand
     * gesteld heeft.
     *
     * down_since staat er voor de DUUR in het bericht. "router weer bereikbaar na
     * 4 min" is bruikbaar; "router weer bereikbaar" laat de lezer zelf rekenen,
     * en die duur is het enige wat een herstelmelding boven een geruststelling
     * uittilt. */
    bool          down_sent;     /* er is een storingsmelding uitgegaan */
    bool          was_stale;     /* die melding ging over een stille MELDER */
    /* De storingsmelding was een SIMULATIE. Dan is de herstelmelding er ook een,
     * ook als de forcering ondertussen vervallen is -- anders komt er een
     * ongemarkeerd "weer bereikbaar" na een gemarkeerd "onbereikbaar", en dan
     * lijkt de storing achteraf echt te zijn geweest. */
    bool          was_sim;
    unsigned long down_since;    /* millis waarop de storing gemeld werd */
    unsigned long up_since;      /* millis waarop hij weer op ging; 0 = niet op */
    unsigned long rec_until;     /* venster waarin de herstelmelding waar is */
    bool          rec_alerting;  /* wij vragen nu om een herstelmelding */

    AlertRepeat   rep;           /* herhalen tot bevestiging */

    /* Alleen voor GEMELDE diensten. Ook deze twee horen niet in MonitorCfg: ze
     * gaan over metingen en niet over instellingen, en na een herstart weten wij
     * per definitie niets over een gemelde dienst. */
    unsigned long last_report;   /* millis van de laatste melding; 0 = nooit */
    bool          stale;         /* melding te oud; toestand onbekend */
  };
  MonState _mon[MAX_MONITORS];

  /* MUTE/SNOOZE-toestand (RAM-only). _mute_until[slot] en _snooze_until in millis();
   * 0 = niet actief. Zie de mute/snooze-methoden. */
  unsigned long _mute_until[MAX_MONITORS] = {0};
  unsigned long _snooze_until = 0;

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
  /* _busy_slot: 0..MAX_MONITORS-1 = een monitorvakje, -1 = niets, ADHOC_SLOT =
   * de ad-hoc ping. Die sentinel laat de fasemachine één sessie delen zonder dat
   * de monitorpaden veranderen: elke ad-hoc-tak is met (_busy_slot == ADHOC_SLOT)
   * bewaakt, de rest blijft byte-voor-byte het bestaande pad. */
  static const int8_t ADHOC_SLOT = -2;
  int8_t        _busy_slot = -1;        /* vakje dat nu aan de beurt is */
  unsigned long _phase_deadline = 0;
  unsigned long _next_ping_at = 0;      /* rusttijd tussen twee pings */

  /* ---------------- de ad-hoc ping ----------------
   *
   * Eén slot, geen wachtrij. De uitslag staat hier tot DmCommands hem opgehaald
   * heeft (adhocClear). Geen SPIFFS: dit is vluchtig meetwerk. */
  struct AdhocPing {
    uint8_t       state;         /* AdhocState */
    char          host[MON_HOST_LEN];
    uint32_t      addr_v4;       /* opgelost adres; 0 = nog niet */
    uint8_t       want;          /* aantal gevraagde pings */
    uint8_t       done;          /* aantal afgeronde pings */
    uint8_t       ok;            /* daarvan gelukt */
    uint32_t      results[ADHOC_MAX_PINGS];  /* per ping: ms, of 0xFFFFFFFF = timeout */
    uint32_t      min_ms;
    uint32_t      max_ms;
    uint32_t      sum_ms;
    bool          delayed;       /* de start moest wachten op de pingmachine */
    unsigned long finished_at;   /* millis waarop de uitslag klaar kwam */
    const char*   note;          /* korte reden bij een vroeg einde (wifi weg e.d.) */
  };
  AdhocPing _adhoc = { ADHOC_NONE, {0}, 0, 0, 0, 0, {0}, 0, 0, 0, false, 0, NULL };

  void startAdhocResolve();
  void startAdhocOnePing();
  void recordAdhocResult(bool ok, uint32_t ms);
  void finishAdhoc(const char* note);

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

  /* ---------------- de simulatielaag ----------------
   *
   * STAAT MET OPZET NIET IN MonitorCfg EN DUS NIET IN SPIFFS. Een forcering is
   * per definitie tijdelijk, en een node die na een stroomstoring in testmodus
   * opstart is precies verkeerd: hij zwijgt dan over een echte storing en
   * niemand weet waarom. Een herstart hoort een forcering te WISSEN, en dat doet
   * hij hier gratis -- dit is gewoon RAM. */
  struct SimSlot {
    unsigned long until;    /* millis waarop de forcering vervalt */
    uint8_t       mode;     /* SimMode */
  };
  SimSlot _sim[SIM_COUNT];
  unsigned long _sim_last_change = 0;    /* voor SIM_GAP_MS */

  /* Vervallen forceringen opruimen. In loop() en niet in de getters: die worden
   * uit querySensors() en uit de webserver aangeroepen en horen geen tijd te
   * kennen. Bovendien hoort het aflopen één keer in de debug-uitvoer te komen en
   * niet bij elke uitlezing. */
  void loopSim();

  /* De GEMETEN wifi-toestand, dus zonder simulatie. Alleen de ping-machine en
   * monitorsPaused() gebruiken deze; zie de uitleg bij SIMULEREN hierboven. */
  bool wifiReallyOnline() const;

  /* Hoeveel waarschuwingen wij op dit moment bij alertIf() hebben lopen: de
   * monitors, de vaste kanalen en het testbericht bij elkaar. EEN teller voor
   * alle drie, want ze delen dezelfde wachtrij -- drie losse tellers van twee
   * zouden zes plaatsen vragen waar er vier zijn, en dan valt een echte
   * batterijwaarschuwing weg. */
  uint8_t alertsActive() const;

  /* Wij vragen nu een waarschuwing voor een vast kanaal. Zelfde rol als
   * MonState::alerting, maar de vaste kanalen hebben geen MonState. */
  bool _fixed_alerting[FIXED_ALERT_COUNT];

  /* Dezelfde vier velden als in MonState, voor de twee vaste kanalen. Een eigen
   * struct hiervoor zou netter staan maar niets toevoegen: het zijn twee
   * ingangen en ze doen precies wat de monitorvelden doen. */
  bool          _fixed_down_sent[FIXED_ALERT_COUNT];
  unsigned long _fixed_down_since[FIXED_ALERT_COUNT];
  unsigned long _fixed_up_since[FIXED_ALERT_COUNT];
  unsigned long _fixed_rec_until[FIXED_ALERT_COUNT];
  bool          _fixed_rec_alerting[FIXED_ALERT_COUNT];
  AlertRepeat   _fixed_rep[FIXED_ALERT_COUNT];    /* herhalen tot bevestiging */

  /* De kern van "herhalen tot bevestiging", gedeeld door de monitors en de vaste
   * kanalen. Geeft terug wat er aan alertIf() gevoerd moet worden en beheert de
   * eerste melding, het her-armen per periode (de puls-laag), de menselijke
   * bevestiging en de bovengrens. armed_first (mag NULL) wordt true op de ronde
   * waarin de EERSTE melding wordt gearmd, zodat de monitorkant zijn was_stale/
   * was_sim daar kan zetten. */
  bool stepStoringAlert(bool want, bool& alerting, bool& down_sent,
                        unsigned long& down_since, AlertRepeat& r,
                        int idx_for_log, bool* armed_first);
  /* De ruwe storingsvoorwaarde van een monitorvakje (sim of gemeten), zonder de
   * herhaal-, ack- of remlogica. Gedeeld door monitorAlert() en confirmAlerts(). */
  bool monitorWantAlert(int slot) const;

  /* De boekhouding van de herstelmelding bijwerken. Aparte lus en niet in
   * applyResult(): de vaste kanalen hebben geen applyResult, en de klok van de
   * rustperiode moet ook lopen als er niets binnenkomt. */
  void loopRecovery();
  /* Zet de teller van één ingang op grond van "is hij nu op". Eén functie voor
   * de monitors en de vaste kanalen, want de regel is voor beide dezelfde. */
  void trackRecovery(bool up_now, bool& down_sent, unsigned long& down_since,
                     unsigned long& up_since, unsigned long& rec_until);
  /* "Wij hebben deze storing gemeld", plus het moment. Wordt op vier plaatsen
   * gezet en bepaalt de duur in het herstelbericht. */
  static void noteDownSent(bool& down_sent, unsigned long& down_since);

  /* ---------------- het testbericht ---------------- */
  uint8_t       _test_state      = TEST_IDLE;
  uint8_t       _test_recipients = 0;
  uint8_t       _test_acks       = 0;
  uint16_t      _test_seq        = 0;    /* loopnummer, staat in het bericht */
  unsigned long _test_asked_at   = 0;    /* millis van de aanvraag */
  unsigned long _test_sent_at    = 0;    /* millis waarop alertIf() hem oppakte */
  unsigned long _test_hold_until = 0;
  uint32_t      _test_last_ack   = 0;    /* tegen dubbel tellen van dezelfde ACK */

  void loopTest();

  bool          _dirty = false;         /* instellingen nog niet weggeschreven */
  unsigned long _save_at = 0;

  /* ---------------- het bytebudget ----------------
   *
   * _telem_base is GEMETEN: querySensors() leest af hoeveel de basisklasse
   * verbruikt heeft (batterijspanning, GPS als die aanstaat, omgevingssensoren)
   * en bewaart dat hier. Een geschat getal zou stil verkeerd worden zodra iemand
   * GPS aanzet -- dat is 11 byte, en dat is één monitor mét pingtijd.
   *
   * De beginwaarde 4 is de bekende ondergrens: SensorMesh zet altijd de
   * batterijspanning (LPP_VOLTAGE) neer voordat hij ons aanroept. De pagina zegt
   * erbij dat het getal na de eerste leesronde definitief is, want een budget dat
   * doet alsof het al gemeten is terwijl het geraden is, is erger dan een budget
   * met een voorbehoud.
   *
   * _telem_dropped is het masker van vakjes die bij de LAATSTE uitlezing niet in
   * het pakket pasten. Een bitmasker en geen teller: de pagina moet kunnen zeggen
   * WELKE monitor er buiten viel, want "er vielen er drie buiten" laat iemand
   * zoeken in plaats van kijken. */
  uint8_t  _telem_base = 4;
  bool     _telem_measured = false;
  uint32_t _telem_dropped = 0;

  /* De tik waarop loopSim(), loopTest() en loopRecovery() lopen. Zie loop() voor
   * waarom: die drie samen lopen door 32 vakjes, en loop() draait duizenden keren
   * per seconde op een kern die ook de radio bedient. Alles waar ze op letten
   * wordt in seconden gemeten, dus 250 ms is ruim. */
  static const uint32_t TICK_MS = 250;
  unsigned long _next_tick = 0;

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
  /* Een "naam of kanaal"-argument (uit een DM-commando) naar een vakjenummer.
   * Alleen cijfers = kanaal; anders een naam. Exact, nooit een prefix. */
  int     resolveTarget(const char* tok) const;
  int     slotOfNth(int nth) const;     /* nde bezette vakje -> vakjenummer */
  bool    addMonitor(const char* spec);
  bool    delMonitor(const char* name);
  void    markDirty();
};
