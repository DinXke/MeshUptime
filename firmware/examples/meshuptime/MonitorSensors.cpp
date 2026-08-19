#include "MonitorSensors.h"
#include "WifiTask.h"

/* Alleen voor de 'board'-global: getBattMilliVolts() is de enige meetweg die
 * dit bord heeft. SensorMesh gebruikt dezelfde global op precies dezelfde
 * manier, dus dit voegt geen nieuwe afhankelijkheid toe. */
#include <target.h>

/* Grenzen waarbinnen een drempel nog ergens op lijkt. Ruim genomen: dit is
 * geen ijking maar een zeef tegen typefouten ("sensor set mains.hi 41.2"). */
#define MAINS_THRESHOLD_MIN   1.0f
#define MAINS_THRESHOLD_MAX  12.0f

bool MonitorSensors::begin() {
  bool ok = EnvironmentSensorManager::begin();

  /* Zie de waarschuwing in de header: de basisklasse deelt kanalen uit vanaf 2
   * en komt dan op onze vaste kanalen terecht. Onze nummers verschuiven niet,
   * dus dit is een melding en geen fout. */
  if (_active_sensor_count > 0) {
    MESH_DEBUG_PRINTLN("MonitorSensors: %d omgevingssensor(en) gevonden; die krijgen kanaal 2 en verder, naast de vaste kanalen 2-4", _active_sensor_count);
  }

  /* Meteen een meting: zonder deze staat de voedingstoestand tot de eerste
   * loop()-ronde op zijn beginwaarde, en die zou kunnen liegen. */
  _state_since = millis();
  samplePower();
  _next_sample = millis() + SAMPLE_INTERVAL_MS;

  return ok;
}

void MonitorSensors::loop() {
  EnvironmentSensorManager::loop();   // GPS/BSEC-werk van de basisklasse

  /* De radio gaat voor: dit is een ADC-meting van een paar microseconden op
   * een vaste tik, geen wachten en geen I/O. */
  unsigned long now = millis();
  if ((long)(now - _next_sample) >= 0) {
    _next_sample = now + SAMPLE_INTERVAL_MS;
    samplePower();
  }
}

/* Een meting, en de toestandsmachine een stap verder. */
void MonitorSensors::samplePower() {
  _last_volts = (float)board.getBattMilliVolts() / 1000.0f;

  /* De eerste meting bepaalt de begintoestand: er is nog geen kant om vanuit
   * te vertrekken, dus hier geen hysterese maar het midden tussen de twee
   * drempels. Dat is symmetrisch en dus niet in het voordeel van een van de
   * twee uitkomsten. */
  if (!_seeded) {
    _mains = _last_volts >= (_mains_lo + _mains_hi) / 2.0f;
    _seeded = true;
    _agree = 0;
    return;
  }

  bool wants_mains;
  if (_last_volts >= _mains_hi) {
    wants_mains = true;
  } else if (_last_volts <= _mains_lo) {
    wants_mains = false;
  } else {
    _agree = 0;      // tussen de drempels: geen mening, en de teller vervalt
    return;
  }

  if (wants_mains == _mains) {   // bevestigt wat we al dachten
    _agree = 0;
    return;
  }

  /* Insteltijd. Ook na het opstarten, want _state_since wordt in begin() gezet:
   * de eerste minuut na een reset zit de node in dezelfde transient. */
  if ((unsigned long)(millis() - _state_since) < SETTLE_MS) {
    _agree = 0;
    return;
  }

  if (++_agree >= SAMPLES_TO_SWITCH) {
    _mains = wants_mains;
    _agree = 0;
    _state_since = millis();
    MESH_DEBUG_PRINTLN("MonitorSensors: voeding -> %s (%.3f V)", _mains ? "net" : "batterij", _last_volts);
  }
}

bool MonitorSensors::isWifiOnline() const {
  return _wifi != NULL && _wifi->isOnline();
}

bool MonitorSensors::querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) {
  /* EERST de basisklasse: batterij (kanaal 1, door SensorMesh), GPS en de
   * omgevingssensoren blijven werken zoals upstream. */
  EnvironmentSensorManager::querySensors(requester_permissions, telemetry);

  /* Voeding en wifi vallen onder de basispermissie: dit is de toestand van de
   * node zelf, niet die van een omgevingssensor. Een vraagsteller die bit 0
   * wegmaskeert krijgt ze dus niet -- dat is de bedoeling van het masker.
   *
   * Kanaal 3 is de spiegel van kanaal 2 en dus strikt gezien overtollig. Het
   * staat er toch, omdat "netvoeding" en "batterijvoeding" in de bewaking twee
   * aparte sensoren zijn die elk hun eigen waarschuwing kunnen krijgen, en
   * omdat een LPP_SWITCH 3 byte kost. */
  if (requester_permissions & TELEM_PERM_BASE) {
    telemetry.addSwitch(CH_MAINS,   _mains ? 1 : 0);
    telemetry.addSwitch(CH_BATTERY, _mains ? 0 : 1);
    /* Zonder aangehangen WifiTask is er geen wifi, en dan is "niet online" het
     * eerlijke antwoord. Het kanaal blijft altijd aanwezig, zodat een
     * dashboard geen veld ziet komen en gaan. */
    telemetry.addSwitch(CH_WIFI,    isWifiOnline() ? 1 : 0);
  }

  return true;
}

/* ===================== instellingen =====================
 *
 * Bereikbaar via de seriële console en over DM-CLI met de generieke
 * opdrachten van CommonCLI: "sensor list", "sensor get mains.hi",
 * "sensor set mains.hi 4.13".
 *
 * LET OP: CommonCLI bewaart deze waarden niet. Alleen "gps" wordt bij het
 * opstarten teruggezet uit de voorkeuren (SensorMesh.h:164). Een aangepaste
 * drempel geldt dus tot de volgende herstart; blijvend opslaan hoort bij de
 * eigen instellingenopslag van MeshUptime en die is er nog niet.
 *
 * De eigen instellingen komen ACHTER die van de basisklasse, zodat de
 * bestaande indices niet verschuiven.
 */

/* Vaste buffers, één per instelling: getSettingValue() is const en mag niets
 * alloceren, en met een buffer per instelling kan een aanroeper meerdere
 * waarden naast elkaar vasthouden zonder verrassing. */
static char s_setting_buf[3][12];

int MonitorSensors::getNumSettings() const {
  return EnvironmentSensorManager::getNumSettings() + 3;
}

const char* MonitorSensors::getSettingName(int i) const {
  const int base = EnvironmentSensorManager::getNumSettings();
  if (i < base) return EnvironmentSensorManager::getSettingName(i);

  switch (i - base) {
    case 0: return "mains.hi";      // drempel omhoog: hierboven netspanning
    case 1: return "mains.lo";      // drempel omlaag: hieronder batterij
    case 2: return "mains.state";   // alleen lezen: huidige uitkomst
  }
  return NULL;
}

const char* MonitorSensors::getSettingValue(int i) const {
  const int base = EnvironmentSensorManager::getNumSettings();
  if (i < base) return EnvironmentSensorManager::getSettingValue(i);

  const int idx = i - base;
  switch (idx) {
    case 0:
    case 1:
      snprintf(s_setting_buf[idx], sizeof(s_setting_buf[idx]), "%.3f", idx == 0 ? _mains_hi : _mains_lo);
      return s_setting_buf[idx];
    case 2:
      snprintf(s_setting_buf[idx], sizeof(s_setting_buf[idx]), "%d", _mains ? 1 : 0);
      return s_setting_buf[idx];
  }
  return NULL;
}

bool MonitorSensors::setSettingValue(const char* name, const char* value) {
  const bool is_hi = strcmp(name, "mains.hi") == 0;
  const bool is_lo = strcmp(name, "mains.lo") == 0;

  if (is_hi || is_lo) {
    const float v = atof(value);
    if (v < MAINS_THRESHOLD_MIN || v > MAINS_THRESHOLD_MAX) return false;

    /* De bovendrempel moet boven de onderdrempel blijven; anders verdwijnt de
     * hysterese en gaat de toestand flikkeren. Weigeren en niets veranderen is
     * hier beter dan de andere drempel stil meeslepen. CommonCLI antwoordt
     * helaas met "can't find custom var" -- dat is zijn enige foutmelding. */
    if (is_hi && v <= _mains_lo) return false;
    if (is_lo && v >= _mains_hi) return false;

    if (is_hi) _mains_hi = v; else _mains_lo = v;

    /* Een nieuwe drempel maakt de lopende teller waardeloos: die is met de
     * oude grenzen opgebouwd. */
    _agree = 0;
    return true;
  }

  /* mains.state is een uitlezing, geen knop. */
  if (strcmp(name, "mains.state") == 0) return false;

  return EnvironmentSensorManager::setSettingValue(name, value);
}
