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
 *     rec 1
 *     rhold 120
 *     arepeat 300
 *     m 5 60 google 8.8.8.8 1
 *     m 6 30 hoas hoas.scheepers.one 0
 *     .
 *
 * De velden van een "m"-regel zijn: kanaal, interval, naam, adres en of de
 * PINGTIJD meegaat over het mesh (zie MonitorCfgEntry::send_ms). Dat laatste veld
 * is bij het LEZEN optioneel en krijgt dan de waarde 1 -- een bestand van een
 * eerdere versie komt van firmware die de pingtijd altijd meestuurde, dus zo
 * verandert er na een update niets aan wat er de ether in gaat. Bij het SCHRIJVEN
 * staat hij er altijd.
 *
 * De kenregel blijft #MU1 nadat er "rec", "rhold" en dat zesde veld bij zijn
 * gekomen, en dat is met opzet: onbekende sleutels worden overgeslagen en een
 * ontbrekend veld heeft een verdedigbare standaard. Een bestand MET die regels is
 * dus nog leesbaar door firmware die ze niet kent, en een bestand ZONDER is
 * leesbaar door deze firmware. Het nummer verhogen zou beide kanten stukmaken
 * voor een uitbreiding die niets breekt.
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

/* HET AANTAL VAKJES -- 32, en de kanalen zijn dus 5..36.
 *
 * WAAROM NIET 8 MEER, EN WAAROM DIT GETAL GEEN GRENS IS
 *
 * Een vast maximum op het AANTAL monitors is altijd of te krap of onwaar, want de
 * echte grens zijn BYTES: de CayenneLPP van SensorMesh is MAX_PACKET_PAYLOAD - 4
 * = 180 byte, en daar moet alles in. Met een pingtijd erbij kost een monitor
 * 9 byte, zonder 3 byte, dus er passen er 17 of ruim vijftig -- afhankelijk van
 * hoe ze ingesteld staan. Een teller van 8 kon dat verschil niet uitdrukken.
 *
 * 32 is daarom geen grens maar ruimte: genoeg dat het BUDGET de rem wordt en niet
 * de teller, en dat budget staat live op de pagina. Wat het kost is nagerekend en
 * niet aangenomen -- zie het rapport bij deze wijziging: ~1,7 kB aan MonState,
 * ~2,0 kB aan MonitorCfgEntry en de Trigger-rijen in main.cpp. Op een bord met
 * ~250 kB vrij RAM is dat te overzien; op 8 monitors stond er al net zoveel
 * ongebruikt.
 *
 * DE KANAALNUMMERS BLIJVEN VASTLIGGEN. 5..12 betekende hetzelfde vóór deze
 * wijziging als erna, dus een dashboard dat "kanaal 6 = google" bewaard heeft,
 * blijft kloppen. Er komen alleen nummers bij aan de bovenkant. */
#define MON_MAX_MONITORS  32      /* net zoveel als er kanalen zijn: 5..36 */
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
  uint8_t  channel;        /* 5..36; 0 betekent: dit vakje is leeg */

  /* GAAT DE PINGTIJD MEE OVER HET MESH? Standaard ja, want dat is wat iemand
   * verwacht die een monitor aanmaakt.
   *
   * Staat hij uit, dan publiceert querySensors() alleen de SCHAKELAAR (3 byte) en
   * niet de LPP_GENERIC_SENSOR met de tijd (6 byte). Dat is het verschil tussen
   * 9 en 3 byte per monitor, en daarmee tussen zeventien monitors en ruim vijftig
   * in hetzelfde pakket.
   *
   * LET OP HET VERSCHIL, want het is niet hetzelfde en de pagina zegt het er ook
   * bij: de pingtijd wordt nog steeds GEMETEN en is nog steeds te zien op de
   * pagina, in 'sensor list' en in de DM-lijst. Hij gaat alleen niet meer de
   * ether in. "De meting is er niet" en "de meting gaat niet mee" zijn twee heel
   * verschillende dingen -- wie ze verwart, gaat een sensor repareren die werkt. */
  uint8_t  send_ms;        /* 0 = alleen de schakelaar, 1 = ook de pingtijd */
};

/* Grenzen van de rustperiode voor een HERSTELMELDING. Hier en niet in
 * MonitorSensors.cpp, om dezelfde reden als bij het interval: het inleesfilter en
 * het instellingenfilter moeten dezelfde grenzen aanhouden.
 *
 * 0 is toegestaan en betekent "meteen melden". 120 s is de standaard: dat is
 * langer dan twee ping-rondes op het standaardinterval, dus een dienst die
 * flappert haalt hem niet. 3600 s is de bovengrens; wie langer wil wachten, wil
 * eigenlijk geen herstelmelding. */
#define MON_RHOLD_MIN         0
#define MON_RHOLD_MAX      3600
#define MON_RHOLD_DEFAULT   120

/* Grenzen van de herhaalperiode (repeat_s). 0 is toegestaan en betekent uit.
 * De ondergrens is 60 s en niet lager: elke herhaling is een DM-keten naar alle
 * ontvangers, dus zendtijd op een band die met anderen gedeeld wordt, en de
 * waarschuwingen worden toch maar eens per leesronde (60 s) beoordeeld -- korter
 * instellen zou niets versnellen en alleen de band belasten. 3600 s bovengrens:
 * wie langer wil wachten kan net zo goed uitzetten. */
#define MON_AREPEAT_MIN       60
#define MON_AREPEAT_MAX     3600
#define MON_AREPEAT_DEFAULT  300

struct MonitorCfg {
  float   mains_hi;
  float   mains_lo;

  /* HERSTELMELDINGEN -- gaat er ook een bericht uit als iets weer WERKT?
   *
   * Standaard aan, want zonder herstelmelding krijg je "router onbereikbaar" en
   * daarna nooit meer iets. Dan is er geen verschil te zien tussen "het is
   * opgelost" en "de node is zelf gestopt met melden", en dat tweede is precies
   * het geval dat je wil onderscheiden.
   *
   * Uit te zetten, want het verdubbelt het aantal berichten per storing en op een
   * node met een krap zendbudget is dat een verdedigbare keuze.
   *
   * rhold_s is de rem tegen flappen: zo lang moet een dienst aaneengesloten weer
   * op zijn voordat het herstel gemeld wordt. Zonder die rem stuurt een dienst
   * die elke minuut op en neer gaat elke minuut twee berichten, en dat is de
   * ergste vorm van een alarmsysteem -- het leert mensen meldingen negeren. */
  uint8_t  recover_alerts;   /* 0 = uit, 1 = aan */
  uint16_t rhold_s;          /* MON_RHOLD_MIN..MAX */

  /* HERHALEN TOT EEN MENS BEVESTIGT -- als een pieper die blijft piepen.
   *
   * Zolang een storing actief is EN nog geen companion "ok" terugstuurde, wordt
   * de melding elke repeat_s seconden opnieuw naar de ontvangers gestuurd. Dit is
   * NIET de transport-ACK (die bewijst alleen dat een pakket aankwam); dit wacht
   * op een MENS. Een aankomstbewijs zegt niet dat iemand keek.
   *
   * 0 = uit: één melding en klaar, het gedrag van vóór deze functie. Anders
   * minimaal MON_AREPEAT_MIN, want elke herhaling is zendtijd op een gedeelde
   * band. Standaard MON_AREPEAT_DEFAULT.
   *
   * LET OP -- dit is een gedragsverandering bij het bijwerken: een node die dit
   * bestand nog zonder "arepeat"-regel heeft, krijgt de standaard (300 s) en gaat
   * dus herhalen waar hij dat eerst niet deed. Dat is met opzet de gevraagde
   * standaard; wie het oude gedrag wil, zet alert.repeat op 0. */
  uint16_t repeat_s;         /* 0 = uit, anders MON_AREPEAT_MIN..MAX */

  /* Bitmasker van kanalen die OOIT zijn uitgedeeld, bit 0 = kanaal 5.
   * Dit hoort bij de blijvende gegevens en niet bij het geheugen: zonder dit
   * masker zou de node na een herstart weer bij kanaal 5 beginnen uitdelen, en
   * dan wijst een bewaarde koppeling bij een vraagsteller opnieuw naar de
   * verkeerde dienst. Zie de uitleg bij allocChannel() in MonitorSensors.cpp.
   *
   * uint32_t EN NIET uint8_t: met 32 kanalen past het masker niet meer in een
   * byte. Dat is precies het soort verandering dat stil fout gaat -- de code
   * compileert, en dan worden de kanalen boven 12 gewoon opnieuw uitgedeeld
   * zonder dat er iets van te zien is. Het bestandsformaat draagt het getal als
   * tekst, dus oude bestanden (waarden 0..255) blijven leesbaar. */
  uint32_t ch_ever_used;

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
