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

/* ALARM-BEZORGROUTE, per sensor. Bitmasker; zelfde waarden als ALERT_MODE_* in
 * RoomMesh/SensorMesh, hier los gedefinieerd zodat de opslaglaag niet van de
 * meshlaag afhangt. DM = via het bestaande alertIf/sendAlert-pad (ACK-herhaling),
 * ROOM = als servertekst in de toegewezen room(s), BOTH = allebei. */
#define MON_ALERT_DM       1
#define MON_ALERT_ROOM     2
#define MON_ALERT_BOTH     3
#define MON_ALERT_DEFAULT  MON_ALERT_BOTH

/* ROOM-LIDMAATSCHAPSSET per sensor: een bitmasker van room-indexen (bit i = room
 * i). Een sensor kan zo in MEERDERE rooms tegelijk posten. Standaard room 0
 * ("Storingen"). uint16_t = tot 16 rooms; ruim boven MAX_ROOMS. */
#define MON_ROOMS_DEFAULT  0x0001

/* SENSOR-NODE-LIDMAATSCHAPSSET per sensor: een bitmasker van virtuele sensor-node-
 * indexen (bit i = sensor-node i). Bepaalt op WELKE virtuele sensor-nodes deze
 * sensor als TELEMETRIE-kanaal verschijnt -- los van rooms_mask (dat bepaalt in
 * welke rooms de status-TEKST wordt gepost). Een sensor kan zo tegelijk in
 * meerdere rooms EN meerdere sensor-nodes zitten, en sensoren kunnen over meerdere
 * sensor-nodes verdeeld worden om voorbij de één-pakket-CayenneLPP-limiet te
 * schalen. Standaard sensor-node 0 ("BE-HSS-DinX-Up"). uint16_t = tot 16 nodes. */
#define MON_SNODES_DEFAULT  0x0001

/* De VASTE alarmbronnen (geen ping-monitor). Elk heeft een eigen route+room-set
 * in MonitorCfg. Volgorde vastgelegd zodat de opslag-indexen stabiel blijven. */
enum {
  MON_FA_BATT_CRIT = 0,   /* batterij kritisch */
  MON_FA_BATT_LOW,        /* batterij laag */
  MON_FA_MAINS,           /* netvoeding weg */
  MON_FA_WIFI,            /* wifi weg */
  MON_FA_TEST,            /* testbericht */
  MON_FA_COUNT
};

#define MON_CFG_PATH   "/monitors.cfg"
#define MON_TMP_PATH   "/monitors.tmp"

/* De EIGEN web-inloggegevens van deze node -- een apart, klein bestand.
 *
 * WAAROM NIET IN /monitors.cfg. Dat bestand is van MonitorSensors: die leest het
 * bij het opstarten in zijn eigen MonitorCfg en HERSCHRIJFT het volledig bij elke
 * monitorwijziging. WebTask kan die MonitorCfg in RAM niet aanraken zonder
 * MonitorSensors.* te wijzigen (buiten deze opdracht gehouden). Zou WebTask de
 * web-login in /monitors.cfg zetten, dan gooit de eerstvolgende save() van
 * MonitorSensors hem er weer uit -- twee schrijvers op één bestand lopen uit
 * elkaar en dan is er geen manier meer te zien welke de waarheid is.
 *
 * Een eigen bestand geeft WebTask het VOLLEDIGE eigenaarschap: niemand anders leest
 * of schrijft /web.cfg, geen race, geen coördinatie. Het is precies de opzet van de
 * wifi-instelling (/wifi.cfg, ook een apart tweeregelig bestand), en die filosofie
 * -- opgeslagen wint van gebakken -- geldt hier één op één.
 *
 * FORMAAT: twee regels, net als /wifi.cfg. Regel 1 de gebruiker, regel 2 het
 * wachtwoord. Tekst, met "cat" te lezen over de seriële console; geen struct, geen
 * uitlijning om je aan te vergissen. */
#define WEB_CFG_PATH   "/web.cfg"
#define WEB_USER_LEN   33      /* 32 tekens + afsluiter */
#define WEB_PASS_LEN   65      /* 64 tekens + afsluiter */

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

  /* ALARM-BEZORGING per monitor (room-variant). alert_mode = MON_ALERT_* ,
   * rooms_mask = bitmasker van room-indexen. Bij het LEZEN optioneel (oude
   * bestanden krijgen de standaard: BOTH, room 0); bij het SCHRIJVEN altijd. De
   * sensor-variant negeert deze velden -- die alarmeert altijd via DM. */
  uint8_t  alert_mode;     /* MON_ALERT_DM | MON_ALERT_ROOM */
  uint16_t rooms_mask;     /* bit i = room i */

  /* SENSOR-NODE-set (room-variant): op welke virtuele sensor-nodes deze sensor als
   * telemetrie-kanaal verschijnt. Bij het LEZEN optioneel (oude bestanden krijgen
   * MON_SNODES_DEFAULT); bij het SCHRIJVEN altijd. Los van rooms_mask. */
  uint16_t sensornodes;    /* bit i = sensor-node i */
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

/* Grenzen van de gebeurtenis-push (PushTask). Hier om dezelfde reden als de
 * andere grenzen: het inleesfilter en het instellingenfilter moeten dezelfde
 * aanhouden.
 *
 * De URL-lengte is nagerekend tegen de CLI-weg: "sensor set push.url <url>"
 * loopt door CommonCLI's tmp-buffer (gemeten 132 byte, dus sleutel + waarde tot
 * ~119 tekens). "push.url " is 9 tekens, dus een waarde van 100 past met marge
 * -- langer wordt GEWEIGERD en niet afgekapt, want een half adres is een
 * verkeerd adres. De heartbeat-grenzen: 10 s ondergrens (sneller belooft niets
 * dat de gebeurtenis-push niet al doet), 3600 s bovengrens (een stiltesignaal
 * van meer dan een uur bewaakt niets meer), 30 s standaard. */
#define MON_PUSH_URL_LEN     101      /* 100 tekens + afsluiter */
#define MON_PUSH_TOKEN_LEN    41      /* 40 tekens + afsluiter */
#define MON_PUSH_HB_MIN       10
#define MON_PUSH_HB_MAX     3600
#define MON_PUSH_HB_DEFAULT   30

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

  /* DE GEBEURTENIS-PUSH -- naar de statsserver, zodat een storing hem in ~1 s
   * bereikt in plaats van bij de volgende poll, en elke stilte een betekenis
   * krijgt (heartbeat).
   *
   * push_url leeg = push uit; dat is de standaard, want een node hoort niet
   * ongevraagd naar buiten te praten. Alleen http:// -- een TLS-stapel kost
   * tientallen kB RAM op een bord dat die niet over heeft, en de server staat
   * op het eigen LAN. Het token staat hier in klare tekst, net als het
   * wifi-wachtwoord in de gebakken vlaggen: SPIFFS van dit bord is niet
   * versleuteld en dat is een aanvaarde eigenschap van het hele apparaat. */
  char     push_url[MON_PUSH_URL_LEN];
  char     push_token[MON_PUSH_TOKEN_LEN];
  uint16_t push_hb_s;      /* beloofd heartbeat-interval, MON_PUSH_HB_MIN..MAX */

  /* ALARM-BEZORGING voor de VASTE bronnen (MON_FA_*): route + room-set, zoals de
   * per-monitor velden hierboven maar voor batterij/netvoeding/wifi/test. */
  uint8_t  fixed_alert_mode[MON_FA_COUNT];
  uint16_t fixed_rooms_mask[MON_FA_COUNT];
  /* SENSOR-NODE-set voor de vaste bronnen, symmetrisch met fixed_rooms_mask. */
  uint16_t fixed_sensornodes[MON_FA_COUNT];

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

  /* --- de eigen web-inloggegevens (zie WEB_CFG_PATH hierboven) --- */

  /* Leest /web.cfg. Geeft alleen true als er een BRUIKBARE login staat: beide
   * regels aanwezig en geen van beide leeg. Een lege pass telt niet als geldig --
   * die zou de node openzetten. Bij false blijven user/pass leeg en valt de
   * aanroeper terug op de gebakken WEB_USER/WEB_PASS. */
  static bool loadWebCred(fs::FS& fs, char* user, size_t user_len,
                          char* pass, size_t pass_len);

  /* Schrijft /web.cfg. Weigert (false) een lege user of een lege pass: een lege
   * pass zou de node openzetten, en dat mag nooit -- ook niet op verzoek. */
  static bool saveWebCred(fs::FS& fs, const char* user, const char* pass);

  /* Verwijdert /web.cfg, zodat de node terugvalt op de GEBAKKEN WEB_USER/WEB_PASS
   * (admin/meshcore) -- de "opgeslagen wint van gebakken"-regel omgekeerd. Dit is
   * de schone weg om een geroteerde login terug te zetten naar de standaard zonder
   * de rest van SPIFFS (monitors, wifi) te wissen. Geeft true als het bestand daarna
   * weg is (verwijderd of het bestond al niet); false als remove() faalde. */
  static bool clearWebCred(fs::FS& fs);
};
