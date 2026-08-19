#pragma once

#include <Arduino.h>
#include <FS.h>   /* fs::FS -- op ESP32 is dat waar de FILESYSTEM-macro op uitkomt */

/* MonitorStore -- de instellingen van MeshUptime blijvend bewaren.
 *
 * WAAROM DIT BESTAAT
 *
 * CommonCLI kent wel "sensor set", maar bewaart het resultaat niet. Bij het
 * opstarten wordt uit de voorkeuren maar EEN sensorinstelling teruggezet:
 *
 *     SensorMesh.h:  sensors.setSettingValue("gps", _prefs.gps_enabled?"1":"0");
 *
 * Al het andere leeft in RAM. Een bijgestelde mains.hi geldt dus tot de
 * volgende herstart, en dat is precies de instelling waarvan we in de meting
 * van 19 augustus wisten dat hij ooit bijgesteld zou moeten worden. Hetzelfde
 * geldt voor de ping-monitors: die zou je na elke stroomstoring opnieuw moeten
 * intypen, en dat is voor een bewakingsnode geen optie.
 *
 * FORMAAT -- tekst, regelgebaseerd, met een sluitregel
 *
 *     #MU1
 *     hi 4.120
 *     lo 4.090
 *     ever 3
 *     m 5 60 google 8.8.8.8
 *     m 6 30 hoas hoas.scheepers.one
 *     .
 *
 * Tekst en niet een binaire struct, om drie redenen: het is met "cat" te lezen
 * over de seriële console, een veld erbij maakt oude bestanden niet onleesbaar
 * (onbekende sleutels worden overgeslagen), en er is geen uitlijning of
 * endianness om je aan te vergissen.
 *
 * HALF WEGGESCHREVEN BESTANDEN
 *
 * Een node die tijdens het schrijven zijn stroom verliest -- en deze node hangt
 * aan een batterij die we juist bewaken -- laat een afgekapt bestand achter.
 * Twee voorzieningen:
 *
 *  1. schrijven gaat altijd naar /monitors.tmp, en pas als dat compleet en
 *     gesloten is wordt het over /monitors.cfg heen genoemd. Bij een crash
 *     halverwege blijft de oude, complete .cfg staan.
 *  2. de sluitregel "." moet aanwezig zijn. Zonder die regel is het bestand
 *     afgekapt, en dan wordt ALLES verworpen in plaats van half toegepast: een
 *     halve monitorlijst is erger dan geen, want dan bewaakt de node stil
 *     minder dan je denkt. load() geeft dan false en de aanroeper houdt zijn
 *     standaardwaarden.
 *
 * Er wordt nergens gealloceerd en er is geen String: één statische leesbuffer,
 * en het ontleden gebeurt op zijn plaats in die buffer.
 */

#define MON_MAX_MONITORS   8      /* net zoveel als er kanalen zijn: 5..12 */
#define MON_NAME_LEN      17      /* 16 tekens + afsluiter */
#define MON_HOST_LEN      41      /* 40 tekens + afsluiter */

#define MON_CFG_PATH   "/monitors.cfg"
#define MON_TMP_PATH   "/monitors.tmp"

/* Grenzen van het ping-interval, hier en niet in MonitorSensors.cpp: het
 * inleesfilter en het instellingenfilter moeten dezelfde grenzen aanhouden,
 * anders keurt de een af wat de ander wegschrijft.
 *
 * 10 s is de ondergrens uit het ontwerp: korter levert geen betere bewaking
 * maar wel meer verkeer en meer kans dat monitors op elkaar wachten. 3600 s is
 * de bovengrens, en die is niet willekeurig: interval_s is een uint16_t, dus
 * technisch past er 65535, maar een bewakingsnode met een interval van meer dan
 * een uur bewaakt niets meer. De ronde grens is bovendien makkelijker te
 * verdedigen dan "65535" als iemand zich ooit afvraagt waarom hij daar staat. */
#define MON_INTERVAL_MIN       10
#define MON_INTERVAL_MAX     3600
#define MON_INTERVAL_DEFAULT   60

/* Eén monitor zoals hij op schijf staat: alleen instellingen, geen meetwerk.
 * De gemeten toestand hoort niet in dit bestand -- die is na een herstart
 * ongeldig en moet opnieuw gemeten worden. */
struct MonitorCfgEntry {
  char     name[MON_NAME_LEN];
  char     host[MON_HOST_LEN];
  uint16_t interval_s;
  uint8_t  channel;        /* 5..12; 0 betekent: dit vakje is leeg */
};

struct MonitorCfg {
  float   mains_hi;
  float   mains_lo;

  /* Bitmasker van kanalen die OOIT zijn uitgedeeld, bit 0 = kanaal 5.
   * Dit hoort bij de blijvende gegevens en niet bij het geheugen: zonder deze
   * byte zou de node na een herstart weer bij kanaal 5 beginnen uitdelen, en
   * dan wijst een bewaarde koppeling bij een vraagsteller opnieuw naar de
   * verkeerde dienst. Zie de uitleg bij allocChannel() in MonitorSensors.cpp. */
  uint8_t ch_ever_used;

  MonitorCfgEntry mons[MON_MAX_MONITORS];
};

class MonitorStore {
public:
  /* Zet cfg op de standaardwaarden: geen monitors, de gemeten drempels. */
  static void setDefaults(MonitorCfg& cfg);

  /* Leest MON_CFG_PATH. Geeft alleen true als het bestand compleet was; bij
   * false is cfg ONGEWIJZIGD gelaten, dus de aanroeper kan gewoon met zijn
   * standaardwaarden verder. */
  static bool load(fs::FS& fs, MonitorCfg& cfg);

  /* Schrijft via MON_TMP_PATH en noemt dat daarna om. */
  static bool save(fs::FS& fs, const MonitorCfg& cfg);
};
