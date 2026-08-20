#include "MonitorStore.h"

#include <Mesh.h>   /* alleen voor MESH_DEBUG_PRINTLN */

/* Ruim genomen: de langste regel is "m 36 3600 zestien-tekens. " plus een adres
 * van 40 tekens plus de ping-vlag = 70 tekens. 96 laat plek voor nog een veld
 * zonder dat een oud bestand ineens afgekapt lijkt. */
#define MON_LINE_MAX  96

/* De drempels uit docs/meting-voeding-2026-08-19.log; dezelfde getallen als in
 * MonitorSensors.h, maar hier is de plek waar ze na een wis terugkomen. */
#define MON_DEFAULT_HI  4.12f
#define MON_DEFAULT_LO  4.09f

void MonitorStore::setDefaults(MonitorCfg& cfg) {
  memset(&cfg, 0, sizeof(cfg));
  cfg.mains_hi = MON_DEFAULT_HI;
  cfg.mains_lo = MON_DEFAULT_LO;
  cfg.ch_ever_used = 0;
  /* Herstelmeldingen standaard AAN. Zonder die melding krijg je "onbereikbaar"
   * en daarna nooit meer iets, en dan is "het is opgelost" niet te onderscheiden
   * van "de node is gestopt met melden". Wie het niet wil, zet het uit -- maar de
   * standaard hoort de stand te zijn die het minste stilte oplevert. */
  cfg.recover_alerts = 1;
  cfg.rhold_s = MON_RHOLD_DEFAULT;
  /* Herhalen tot bevestiging standaard aan (300 s). Zie MonitorCfg: dit is de
   * gevraagde standaard en dus een gedragsverandering bij het bijwerken; op 0
   * zetten geeft het oude "één melding en klaar". */
  cfg.repeat_s = MON_AREPEAT_DEFAULT;
  /* channel == 0 in alle vakjes: memset heeft dat al gedaan. Expliciet houden
   * we het niet, want een leeg vakje IS een nul-kanaal. */
}

/* Leest één regel tot '\n' of het einde van het bestand. Geeft de lengte, of
 * -1 als er niets meer te lezen was. Een te lange regel wordt afgekapt en de
 * rest weggegooid -- die regel valt dan bij het ontleden af, en dat is beter
 * dan dat de rest van het bestand verschuift. */
static int readLine(fs::File& f, char* dest, size_t max) {
  size_t len = 0;
  bool got_any = false;

  while (f.available()) {
    int c = f.read();
    got_any = true;
    if (c < 0 || c == '\n') break;
    if (c == '\r') continue;
    if (len < max - 1) dest[len++] = (char)c;
  }
  dest[len] = 0;
  return got_any ? (int)len : -1;
}

/* Splitst op enkele spaties. Geeft het aantal gevonden stukken; schrijft in de
 * buffer zelf (nul-bytes in de plaats van de spaties), dus geen kopie en geen
 * allocatie. */
static int splitTokens(char* line, char* parts[], int max_parts) {
  int n = 0;
  char* p = line;

  while (*p && n < max_parts) {
    while (*p == ' ') p++;         /* meerdere spaties overslaan */
    if (!*p) break;
    parts[n++] = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
  }
  return n;
}

bool MonitorStore::load(fs::FS& fs, MonitorCfg& cfg) {
  if (!fs.exists(MON_CFG_PATH)) {
    MESH_DEBUG_PRINTLN("MonitorStore: %s bestaat niet, standaardwaarden", MON_CFG_PATH);
    return false;
  }

  fs::File f = fs.open(MON_CFG_PATH, "r");
  if (!f) {
    MESH_DEBUG_PRINTLN("MonitorStore: %s niet te openen", MON_CFG_PATH);
    return false;
  }

  /* Alles gaat eerst naar een SCHADUWKOPIE. Zo blijft cfg ongemoeid als het
   * bestand halverwege ophoudt: de aanroeper krijgt dan zijn standaardwaarden
   * en niet een halve monitorlijst. Kost 8 * 60 byte stapel, en dat is de
   * enige plek in deze klasse waar dat gebeurt (begin(), eenmalig). */
  MonitorCfg staged;
  setDefaults(staged);

  char line[MON_LINE_MAX];
  bool closed = false;      /* sluitregel gezien? */
  int  num_mons = 0;
  int  lineno = 0;

  /* Eerste regel moet de kenregel zijn. Een ander bestand met dezelfde naam
   * (of een bestand van een toekomstige versie) wordt zo niet half ingelezen. */
  if (readLine(f, line, sizeof(line)) < 0 || strcmp(line, "#MU1") != 0) {
    MESH_DEBUG_PRINTLN("MonitorStore: kenregel ontbreekt of onbekende versie");
    f.close();
    return false;
  }

  while (readLine(f, line, sizeof(line)) >= 0) {
    lineno++;

    if (line[0] == 0) continue;         /* lege regel */
    if (line[0] == '#') continue;       /* commentaar */

    if (strcmp(line, ".") == 0) {       /* sluitregel: bestand is compleet */
      closed = true;
      break;
    }

    char* parts[6];
    int n = splitTokens(line, parts, 6);
    if (n < 2) continue;               /* onvolledige regel: overslaan */

    if (strcmp(parts[0], "hi") == 0) {
      staged.mains_hi = atof(parts[1]);
    } else if (strcmp(parts[0], "lo") == 0) {
      staged.mains_lo = atof(parts[1]);
    } else if (strcmp(parts[0], "ever") == 0) {
      /* strtoul en niet atoi: met 32 kanalen loopt dit masker tot 2^32-1 en atoi
       * geeft een int. Een bestand van vóór deze wijziging draagt hier een
       * waarde 0..255 en blijft dus gewoon leesbaar. */
      staged.ch_ever_used = (uint32_t)strtoul(parts[1], NULL, 10);
    } else if (strcmp(parts[0], "rec") == 0) {
      staged.recover_alerts = atoi(parts[1]) ? 1 : 0;
    } else if (strcmp(parts[0], "rhold") == 0) {
      /* Buiten de grenzen: de standaard en niet de waarde uit het bestand. Een
       * rustperiode van een dag zou een herstelmelding stil afschaffen. */
      int v = atoi(parts[1]);
      staged.rhold_s = (v >= MON_RHOLD_MIN && v <= MON_RHOLD_MAX)
                     ? (uint16_t)v : MON_RHOLD_DEFAULT;
    } else if (strcmp(parts[0], "arepeat") == 0) {
      /* 0 (uit) is geldig; anders binnen de grenzen, en buiten de grenzen valt
       * hij op de standaard terug in plaats van op de rommel uit het bestand. */
      int v = atoi(parts[1]);
      staged.repeat_s = (v == 0 || (v >= MON_AREPEAT_MIN && v <= MON_AREPEAT_MAX))
                      ? (uint16_t)v : MON_AREPEAT_DEFAULT;
    } else if (strcmp(parts[0], "m") == 0) {
      /* m <kanaal> <interval> <naam> <adres> [<pingtijd 0|1>]
       *
       * Het zesde veld is OPTIONEEL, en dat is geen luiheid: een bestand dat door
       * een eerdere versie geschreven is heeft het niet, en dan is 1 het juiste
       * antwoord -- die versie stuurde de pingtijd altijd mee, dus zo verandert er
       * na een firmware-update niets aan wat er over het mesh gaat. Een monitor
       * die stil ophoudt met het sturen van zijn pingtijd is precies het soort
       * verandering waar een dashboard op stukloopt. */
      if (n < 5) continue;
      if (num_mons >= MON_MAX_MONITORS) continue;

      int ch  = atoi(parts[1]);
      int ivl = atoi(parts[2]);
      if (ch < 5 || ch > 4 + MON_MAX_MONITORS) continue;   /* buiten 5..36 */
      if (ivl < MON_INTERVAL_MIN || ivl > MON_INTERVAL_MAX) continue;

      MonitorCfgEntry& e = staged.mons[num_mons];
      e.channel    = (uint8_t)ch;
      e.interval_s = (uint16_t)ivl;
      strncpy(e.name, parts[3], MON_NAME_LEN - 1); e.name[MON_NAME_LEN - 1] = 0;
      strncpy(e.host, parts[4], MON_HOST_LEN - 1); e.host[MON_HOST_LEN - 1] = 0;
      e.send_ms    = (n >= 6) ? (atoi(parts[5]) ? 1 : 0) : 1;
      num_mons++;
    }
    /* Onbekende sleutel: stil overslaan, zodat een nieuwer bestand op oude
     * firmware nog leesbaar is. */
  }

  f.close();

  if (!closed) {
    MESH_DEBUG_PRINTLN("MonitorStore: %s afgekapt (geen sluitregel na %d regels), verworpen", MON_CFG_PATH, lineno);
    return false;
  }

  /* Laatste zeef: twee monitors op hetzelfde kanaal mag niet bestaan, want dan
   * krijgt een vraagsteller twee waarden op één kanaal. Zo'n bestand is niet
   * door deze firmware geschreven; verwerpen. */
  for (int i = 0; i < num_mons; i++) {
    for (int j = i + 1; j < num_mons; j++) {
      if (staged.mons[i].channel == staged.mons[j].channel) {
        MESH_DEBUG_PRINTLN("MonitorStore: kanaal %d dubbel, bestand verworpen", staged.mons[i].channel);
        return false;
      }
    }
  }

  /* De drempels moeten elkaar niet gekruist hebben; anders verdwijnt de
   * hysterese. Bij twijfel de gebakken waarden. */
  if (staged.mains_hi <= staged.mains_lo) {
    MESH_DEBUG_PRINTLN("MonitorStore: hi <= lo in bestand, drempels op standaard");
    staged.mains_hi = MON_DEFAULT_HI;
    staged.mains_lo = MON_DEFAULT_LO;
  }

  /* Een kanaal dat in gebruik is, is per definitie ooit uitgedeeld. Dit
   * repareert een bestand dat met een oudere versie zonder "ever" is
   * geschreven. */
  for (int i = 0; i < num_mons; i++) {
    staged.ch_ever_used |= ((uint32_t)1 << (staged.mons[i].channel - 5));
  }

  cfg = staged;
  MESH_DEBUG_PRINTLN("MonitorStore: %d monitor(s) ingelezen, hi=%.3f lo=%.3f, herstelmelding %s (rust %us)", num_mons, cfg.mains_hi, cfg.mains_lo, cfg.recover_alerts ? "aan" : "uit", (unsigned)cfg.rhold_s);
  return true;
}

bool MonitorStore::save(fs::FS& fs, const MonitorCfg& cfg) {
  /* Altijd eerst naar het kladbestand. Blijft er een oud kladbestand liggen
   * van een mislukte poging, dan wordt dat hier overschreven. */
  fs::File f = fs.open(MON_TMP_PATH, "w");
  if (!f) {
    MESH_DEBUG_PRINTLN("MonitorStore: %s niet te schrijven", MON_TMP_PATH);
    return false;
  }

  char line[MON_LINE_MAX];
  int len;

  len = snprintf(line, sizeof(line), "#MU1\n");
  f.write((const uint8_t*)line, len);

  len = snprintf(line, sizeof(line), "hi %.3f\nlo %.3f\never %lu\n",
                 cfg.mains_hi, cfg.mains_lo, (unsigned long)cfg.ch_ever_used);
  f.write((const uint8_t*)line, len);

  /* De alarminstellingen: herstelmelding aan/uit, de rustperiode daarvan, en de
   * herhaalperiode. Deze regel ontbrak eerder -- rec/rhold werden wel gelezen maar
   * nooit geschreven, dus een gewijzigde herstelmelding overleefde geen herstart.
   * Alle drie staan er nu, elk op een eigen sleutel zodat een oud bestand zonder
   * een ervan gewoon de standaard houdt. */
  len = snprintf(line, sizeof(line), "rec %u\nrhold %u\narepeat %u\n",
                 (unsigned)(cfg.recover_alerts ? 1 : 0), (unsigned)cfg.rhold_s,
                 (unsigned)cfg.repeat_s);
  f.write((const uint8_t*)line, len);

  for (int i = 0; i < MON_MAX_MONITORS; i++) {
    const MonitorCfgEntry& e = cfg.mons[i];
    if (e.channel == 0) continue;

    /* De ping-vlag gaat ALTIJD mee bij het schrijven, ook als hij 1 is. Alleen
     * bij het LEZEN is hij optioneel (voor oudere bestanden). Zo staat er na één
     * schrijfronde een expliciete waarde in het bestand en hoeft niemand te
     * raden wat de standaard was toen dit weggeschreven werd. */
    len = snprintf(line, sizeof(line), "m %u %u %s %s %u\n",
                   (unsigned)e.channel, (unsigned)e.interval_s, e.name, e.host,
                   (unsigned)(e.send_ms ? 1 : 0));
    if (f.write((const uint8_t*)line, len) != (size_t)len) {
      /* Schijf vol of stuk: het kladbestand is nu onbetrouwbaar, dus laten we
       * de bestaande .cfg met rust. */
      f.close();
      fs.remove(MON_TMP_PATH);
      MESH_DEBUG_PRINTLN("MonitorStore: schrijven mislukt, oude %s blijft staan", MON_CFG_PATH);
      return false;
    }
  }

  /* De sluitregel is het bewijs dat het bestand compleet is. Hij gaat er als
   * laatste in, en pas daarna mag het over de echte plaats heen. */
  len = snprintf(line, sizeof(line), ".\n");
  bool ok = (f.write((const uint8_t*)line, len) == (size_t)len);
  f.flush();
  f.close();

  if (!ok) {
    fs.remove(MON_TMP_PATH);
    return false;
  }

  /* SPIFFS kent geen atomair vervangen: rename() faalt als het doel bestaat.
   * Het gaatje tussen remove en rename is de enige plek waar een stroomstoring
   * ons de instellingen kost. Dat is aanvaardbaar en omkeerbaar: het klad
   * blijft dan staan en de volgende save schrijft hem opnieuw. Andersom (eerst
   * rename, dan remove) kan niet, en een oude kopie bijhouden kost SPIFFS-ruimte
   * die dit bord niet over heeft. */
  fs.remove(MON_CFG_PATH);
  if (!fs.rename(MON_TMP_PATH, MON_CFG_PATH)) {
    MESH_DEBUG_PRINTLN("MonitorStore: rename naar %s mislukt", MON_CFG_PATH);
    return false;
  }

  return true;
}
