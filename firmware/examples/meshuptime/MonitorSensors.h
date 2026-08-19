#pragma once

#include <Arduino.h>
#include <helpers/sensors/EnvironmentSensorManager.h>

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
 *   5+ gereserveerd         voor de nog te bouwen ping-monitors
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
 */
class MonitorSensors : public EnvironmentSensorManager {
public:
  /* Vaste kanaalnummers, ook bruikbaar voor de UI en de ping-monitors. */
  static const uint8_t CH_MAINS         = 2;
  static const uint8_t CH_BATTERY       = 3;
  static const uint8_t CH_WIFI          = 4;
  static const uint8_t CH_MONITOR_FIRST = 5;

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

  WifiTask* _wifi = NULL;

  /* Instelbaar via "sensor set mains.hi 4.13", niet als constante: de drempel
   * moet uit echte historiek bijgesteld kunnen worden zonder te flashen. */
  float _mains_hi = 4.12f;
  float _mains_lo = 4.09f;

  bool          _mains  = true;    // wordt bij de eerste meting overschreven
  bool          _seeded = false;
  uint8_t       _agree  = 0;       // metingen achter elkaar aan de andere kant
  float         _last_volts = 0.0f;
  unsigned long _next_sample = 0;
  unsigned long _state_since = 0;

  void samplePower();
};
