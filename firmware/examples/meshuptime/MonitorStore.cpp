#include "MonitorStore.h"

#include <Mesh.h>   /* alleen voor MESH_DEBUG_PRINTLN */

/* Ruim genomen: de langste regel was "m 36 3600 zestien-tekens. " plus een
 * adres van 40 tekens plus de ping-vlag = 70 tekens; sinds de push-instellingen
 * is het "purl " plus een URL van 100 tekens = 105. 132 laat plek voor nog een
 * veld zonder dat een oud bestand ineens afgekapt lijkt. */
#define MON_LINE_MAX  132

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
  /* Push standaard UIT (lege url): een node hoort niet ongevraagd naar buiten
   * te praten. De memset heeft url en token al leeg gemaakt; alleen het
   * heartbeat-interval krijgt zijn standaard, zodat "sensor get push.hb" iets
   * zinnigs zegt nog voordat er ooit een url gezet is. */
  cfg.push_hb_s = MON_PUSH_HB_DEFAULT;
  /* Alarm-bezorging vaste bronnen: standaard BOTH naar room 0 ("Storingen"). */
  for (int i = 0; i < MON_FA_COUNT; i++) {
    cfg.fixed_alert_mode[i] = MON_ALERT_DEFAULT;
    cfg.fixed_rooms_mask[i] = MON_ROOMS_DEFAULT;
  }
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

    char* parts[8];
    int n = splitTokens(line, parts, 8);
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
    } else if (strcmp(parts[0], "purl") == 0) {
      /* Geen keuring op de inhoud hier: die zit in setSettingValue(), en wat in
       * dit bestand staat is door diezelfde zeef geschreven. Alleen de lengte
       * wordt bewaakt, want een afgekapte regel (zie readLine) mag geen half
       * adres opleveren -- liever geen push dan een push naar het verkeerde
       * adres. */
      if (strlen(parts[1]) < MON_PUSH_URL_LEN) {
        strncpy(staged.push_url, parts[1], MON_PUSH_URL_LEN - 1);
        staged.push_url[MON_PUSH_URL_LEN - 1] = 0;
      }
    } else if (strcmp(parts[0], "ptok") == 0) {
      if (strlen(parts[1]) < MON_PUSH_TOKEN_LEN) {
        strncpy(staged.push_token, parts[1], MON_PUSH_TOKEN_LEN - 1);
        staged.push_token[MON_PUSH_TOKEN_LEN - 1] = 0;
      }
    } else if (strcmp(parts[0], "phb") == 0) {
      /* Buiten de grenzen: de standaard, om dezelfde reden als bij rhold. */
      int v = atoi(parts[1]);
      staged.push_hb_s = (v >= MON_PUSH_HB_MIN && v <= MON_PUSH_HB_MAX)
                       ? (uint16_t)v : MON_PUSH_HB_DEFAULT;
    } else if (strcmp(parts[0], "fa") == 0) {
      /* fa <idx> <mode> <rooms> -- alarm-bezorging van een VASTE bron (MON_FA_*).
       * Onbekende/lege waarden vallen op de standaard terug. */
      if (n >= 4) {
        int fi = atoi(parts[1]);
        if (fi >= 0 && fi < MON_FA_COUNT) {
          uint8_t m = (uint8_t)(atoi(parts[2]) & MON_ALERT_BOTH);
          staged.fixed_alert_mode[fi] = m ? m : MON_ALERT_DEFAULT;
          staged.fixed_rooms_mask[fi] = (uint16_t)strtoul(parts[3], NULL, 10);
        }
      }
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
      /* Velden 7 en 8 (alarm-route + room-set) zijn optioneel: een bestand van
       * vóór de room-variant heeft ze niet en krijgt de standaard (BOTH, room 0).
       * De sensor-variant negeert ze. */
      {
        uint8_t am = (n >= 7) ? (uint8_t)(atoi(parts[6]) & MON_ALERT_BOTH) : MON_ALERT_DEFAULT;
        e.alert_mode = am ? am : MON_ALERT_DEFAULT;
        e.rooms_mask = (n >= 8) ? (uint16_t)strtoul(parts[7], NULL, 10) : MON_ROOMS_DEFAULT;
      }
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

  /* De push-instellingen. url en token alleen als ze er ZIJN: een lege regel
   * "purl " zou bij het lezen op n < 2 stranden en is dus alleen ruis. phb gaat
   * altijd mee, zodat een bijgesteld interval ook zonder url bewaard blijft. */
  if (cfg.push_url[0]) {
    len = snprintf(line, sizeof(line), "purl %s\n", cfg.push_url);
    f.write((const uint8_t*)line, len);
  }
  if (cfg.push_token[0]) {
    len = snprintf(line, sizeof(line), "ptok %s\n", cfg.push_token);
    f.write((const uint8_t*)line, len);
  }
  len = snprintf(line, sizeof(line), "phb %u\n", (unsigned)cfg.push_hb_s);
  f.write((const uint8_t*)line, len);

  /* Alarm-bezorging van de vaste bronnen (MON_FA_*): route + room-set. */
  for (int i = 0; i < MON_FA_COUNT; i++) {
    len = snprintf(line, sizeof(line), "fa %d %u %u\n", i,
                   (unsigned)cfg.fixed_alert_mode[i], (unsigned)cfg.fixed_rooms_mask[i]);
    f.write((const uint8_t*)line, len);
  }

  for (int i = 0; i < MON_MAX_MONITORS; i++) {
    const MonitorCfgEntry& e = cfg.mons[i];
    if (e.channel == 0) continue;

    /* De ping-vlag gaat ALTIJD mee bij het schrijven, ook als hij 1 is. Alleen
     * bij het LEZEN is hij optioneel (voor oudere bestanden). Zo staat er na één
     * schrijfronde een expliciete waarde in het bestand en hoeft niemand te
     * raden wat de standaard was toen dit weggeschreven werd. */
    len = snprintf(line, sizeof(line), "m %u %u %s %s %u %u %u\n",
                   (unsigned)e.channel, (unsigned)e.interval_s, e.name, e.host,
                   (unsigned)(e.send_ms ? 1 : 0),
                   (unsigned)e.alert_mode, (unsigned)e.rooms_mask);
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

/* ----------------------- de eigen web-inloggegevens ----------------------- */

bool MonitorStore::loadWebCred(fs::FS& fs, char* user, size_t user_len,
                               char* pass, size_t pass_len) {
  if (user == nullptr || user_len < 2 || pass == nullptr || pass_len < 2) return false;
  user[0] = 0;
  pass[0] = 0;

  if (!fs.exists(WEB_CFG_PATH)) {
    MESH_DEBUG_PRINTLN("MonitorStore: %s bestaat niet, gebakken web-login", WEB_CFG_PATH);
    return false;
  }

  fs::File f = fs.open(WEB_CFG_PATH, "r");
  if (!f) return false;

  /* readLine() haalt zelf al de '\r' weg en geeft -1 als er niets meer te lezen
   * was. Een afgekapt bestand (wel user, geen pass) valt daarmee vanzelf op false:
   * de tweede regel is er dan niet. */
  int nu = readLine(f, user, user_len);
  int np = readLine(f, pass, pass_len);
  f.close();

  /* Beide regels moeten er zijn en geen van beide leeg. Een lege pass telt met
   * opzet NIET als een geldige login -- zie de noot in de header. */
  if (nu <= 0 || np <= 0 || user[0] == 0 || pass[0] == 0) {
    user[0] = 0;
    pass[0] = 0;
    return false;
  }
  MESH_DEBUG_PRINTLN("MonitorStore: eigen web-login geladen (user %s)", user);
  return true;
}

bool MonitorStore::saveWebCred(fs::FS& fs, const char* user, const char* pass) {
  /* Nooit een lege user of pass wegschrijven: een lege pass zet de node open. Deze
   * zeef staat hier zodat GEEN enkele schrijver (route, console) hem kan omzeilen. */
  if (user == nullptr || pass == nullptr || user[0] == 0 || pass[0] == 0) return false;

  fs::File f = fs.open(WEB_CFG_PATH, "w");
  if (!f) {
    MESH_DEBUG_PRINTLN("MonitorStore: %s niet te schrijven", WEB_CFG_PATH);
    return false;
  }
  f.printf("%s\n%s\n", user, pass);
  f.close();
  return true;
}

bool MonitorStore::clearWebCred(fs::FS& fs) {
  /* Geen bestand = al terug op de gebakken standaard; dat is geen fout maar het
   * gewenste eindresultaat, dus true. */
  if (!fs.exists(WEB_CFG_PATH)) {
    MESH_DEBUG_PRINTLN("MonitorStore: %s bestond niet, gebakken web-login geldt", WEB_CFG_PATH);
    return true;
  }
  bool ok = fs.remove(WEB_CFG_PATH);
  MESH_DEBUG_PRINTLN("MonitorStore: %s verwijderd (%s)", WEB_CFG_PATH, ok ? "ok" : "MISLUKT");
  return ok;
}
