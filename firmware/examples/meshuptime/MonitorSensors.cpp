#include "MonitorSensors.h"
#include "WifiTask.h"

/* Alleen voor de 'board'-global: getBattMilliVolts() is de enige meetweg die
 * dit bord heeft. SensorMesh gebruikt dezelfde global op precies dezelfde
 * manier, dus dit voegt geen nieuwe afhankelijkheid toe. */
#include <target.h>

#include <helpers/TxtDataHelpers.h>   /* StrHelper::strncpy -- kapt af en sluit af */
#include <SPIFFS.h>

/* De bewaking zelf. esp_ping is de ping van ESP-IDF: hij draait in zijn eigen
 * taak, meldt zich via callbacks, en houdt onze loop() dus nooit op. Een
 * blokkerende ping-bibliotheek zou hier niet kunnen: de LoRa-radio wordt uit
 * dezelfde loop() bediend en heeft de strengste tijdseisen van het apparaat.
 *
 * dns/tcpip zijn er om DEZELFDE reden. getaddrinfo() en WiFi.hostByName()
 * wachten tot het antwoord er is -- tot enkele seconden als de DNS-server niet
 * antwoordt -- en dat is precies de vertraging die we niet mogen maken. De
 * rauwe lwIP-oproep dns_gethostbyname() geeft zijn antwoord via een callback,
 * maar mag alleen uit de lwIP-taak komen; tcpip_try_callback() zet hem daar
 * neer zonder te wachten. */
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

/* Grenzen waarbinnen een drempel nog ergens op lijkt. Ruim genomen: dit is
 * geen ijking maar een zeef tegen typefouten ("sensor set mains.hi 41.2"). */
#define MAINS_THRESHOLD_MIN   1.0f
#define MAINS_THRESHOLD_MAX  12.0f

/* MON_INTERVAL_MIN/MAX/DEFAULT staan in MonitorStore.h: het inleesfilter en het
 * instellingenfilter moeten dezelfde grenzen aanhouden. */

/* Het adres dat "ik ping niet zelf, ik word van buiten gemeld" betekent. Zie de
 * uitleg bij PUSH_HOST in de header: de soort monitor zit in dit veld omdat het
 * opslagformaat van MonitorStore geen plek voor een eigen soortveld heeft, en
 * "-" is geen hostnaam en geen IP-adres. */
const char MonitorSensors::PUSH_HOST[] = "-";

const char* MonitorSensors::monResultText(MonResult r) {
  switch (r) {
    case MON_OK:           return "ok";
    case MON_ERR_NAME:     return "naam: 1-16 tekens uit a-z A-Z 0-9 . - _";
    case MON_ERR_HOST:     return "adres: 1-40 tekens uit a-z A-Z 0-9 . - _";
    case MON_ERR_INTERVAL: return "interval: 10-3600 s";
    case MON_ERR_TAKEN:    return "naam bestaat al";
    case MON_ERR_KIND:     return "naam bestaat al als ping-monitor";
    case MON_ERR_FULL:     return "geen vakje vrij (max 32)";
    case MON_ERR_UNKNOWN:  return "naam bestaat niet";
    /* De uitweg staat IN de melding. "vol" laat iemand zoeken; "zet bij een paar
     * monitors de pingtijd uit" is een handeling. */
    case MON_ERR_BYTES:    return "past niet in het telemetriepakket van 180 byte;"
                                  " zet bij een of meer monitors de pingtijd uit"
                                  " (3 byte in plaats van 9) of verwijder er een";
  }
  return "onbekende fout";
}

/* ================== de uitslag van de lopende ping ==================
 *
 * In bestandsbereik en niet in de klasse; de reden staat in MonitorSensors.h
 * bij _phase. Kort: deze velden worden geschreven door de ping-taak en door de
 * lwIP-taak, die C-functies aanroepen, en er is precies één MonitorSensors met
 * precies één ping tegelijk.
 *
 * volatile omdat de schrijver en de lezer verschillende taken zijn. Er is geen
 * slot nodig: elk veld is één woord, en de lezer kijkt eerst naar 'state'
 * (respectievelijk 'dns_state') en pas daarna naar de rest. De schrijver zet
 * die vlag als LAATSTE. Dat is de enige ordening die hier telt. */
static struct {
  volatile uint8_t  state;      /* 0 bezig, 1 gelukt, 2 mislukt */
  volatile uint32_t ms;         /* rondetijd bij 'gelukt' */
  volatile uint8_t  dns_state;  /* 0 bezig, 1 gelukt, 2 mislukt */
  volatile uint32_t dns_addr;   /* IPv4 in netwerk-volgorde */
  esp_ping_handle_t handle;     /* NULL = geen sessie open */
  /* Eigen kopie van de op te zoeken naam: het vakje kan tijdens het opzoeken
   * hernoemd of verwijderd worden, en lwIP houdt de aanwijzer vast. */
  char host[MON_HOST_LEN];
} s_ping = { 0, 0, 0, 0, NULL, {0} };

/* ---- callbacks; deze lopen NIET in de hoofdtaak. Alleen een waarde
 * wegzetten, verder niets: geen debug-uitvoer, geen SPIFFS, geen radio. ---- */

static void mon_ping_success(esp_ping_handle_t hdl, void* args) {
  uint32_t elapsed = 0;
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
  s_ping.ms = elapsed;
  s_ping.state = 1;      /* als laatste: hierop kijkt de lezer */
}

static void mon_ping_timeout(esp_ping_handle_t hdl, void* args) {
  s_ping.state = 2;
}

static void mon_dns_found(const char* name, const ip_addr_t* ipaddr, void* arg) {
  if (ipaddr != NULL && IP_IS_V4(ipaddr)) {
    s_ping.dns_addr = ip4_addr_get_u32(ip_2_ip4(ipaddr));
    s_ping.dns_state = 1;
  } else {
    s_ping.dns_state = 2;    /* niet gevonden, of alleen IPv6 (zie startPing) */
  }
}

/* Loopt in de lwIP-taak, neergezet door tcpip_try_callback(). */
static void mon_do_resolve(void* ctx) {
  ip_addr_t addr;
  err_t e = dns_gethostbyname(s_ping.host, &addr, mon_dns_found, NULL);

  if (e == ERR_OK) {                 /* stond al in de cache van lwIP */
    mon_dns_found(s_ping.host, &addr, NULL);
  } else if (e != ERR_INPROGRESS) {  /* ERR_ARG e.d.: geen vraag verstuurd */
    s_ping.dns_state = 2;
  }
  /* ERR_INPROGRESS: mon_dns_found() komt straks langs. */
}

bool MonitorSensors::begin() {
  bool ok = EnvironmentSensorManager::begin();

  /* Zie de waarschuwing in de header: de basisklasse deelt kanalen uit vanaf 2
   * en komt dan op onze vaste kanalen terecht. Onze nummers verschuiven niet,
   * dus dit is een melding en geen fout. */
  if (_active_sensor_count > 0) {
    MESH_DEBUG_PRINTLN("MonitorSensors: %d omgevingssensor(en) gevonden; die krijgen kanaal 2 en verder, naast de vaste kanalen 2-4", _active_sensor_count);
  }

  /* Instellingen terugzetten. SPIFFS is op dit punt al gemount: main.cpp doet
   * SPIFFS.begin(true) vóór sensors.begin(). Lukt het lezen niet, dan blijven
   * de standaardwaarden staan -- zie MonitorStore.h over afgekapte bestanden. */
  MonitorStore::setDefaults(_cfg);
  MonitorStore::load(SPIFFS, _cfg);

  memset(_mon, 0, sizeof(_mon));

  /* Geen forceringen na een start, en dat is geen initialisatiehygiëne maar de
   * ontwerpeis uit de header: een node die na een stroomstoring in testmodus
   * opstart zwijgt over een echte storing. Omdat de simulatiestand alleen in RAM
   * staat, is dit het enige dat er voor nodig is. */
  memset(_sim, 0, sizeof(_sim));
  memset(_fixed_alerting, 0, sizeof(_fixed_alerting));
  memset(_fixed_down_sent, 0, sizeof(_fixed_down_sent));
  memset(_fixed_down_since, 0, sizeof(_fixed_down_since));
  memset(_fixed_up_since, 0, sizeof(_fixed_up_since));
  memset(_fixed_rec_until, 0, sizeof(_fixed_rec_until));
  memset(_fixed_rec_alerting, 0, sizeof(_fixed_rec_alerting));
  memset(_fixed_rep, 0, sizeof(_fixed_rep));

  /* Geen ad-hoc ping na een start. Vluchtig, dus alleen RAM. */
  memset(&_adhoc, 0, sizeof(_adhoc));

  unsigned long now = millis();
  for (int i = 0; i < MAX_MONITORS; i++) {
    /* up begint op 'waar' zonder 'seeded': zolang er nog geen uitslag is, is
     * "onbekend" het eerlijke antwoord, en een monitor die bij het opstarten
     * al plat ligt moet net als elke andere eerst PINGS_TO_DOWN mislukkingen
     * halen voordat er iemand gewekt wordt. */
    _mon[i].up = true;
    /* Uitgesmeerd starten. Alle acht tegelijk zou een piek geven op het moment
     * dat de node net zijn adres heeft en ook zijn advert nog moet sturen. */
    _mon[i].next_check = now + WIFI_GRACE_MS + (unsigned long)i * PING_GAP_MS;
  }

  MESH_DEBUG_PRINTLN("MonitorSensors: %d monitor(s) actief", (int)getNumMonitors());

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

  loopMonitors();

  /* OP EEN TIK VAN 250 ms EN NIET ELKE RONDE.
   *
   * loop() draait duizenden keren per seconde, en deze drie lopen samen door 32
   * monitorvakjes plus de vaste kanalen. Per ronde is dat niets; duizenden keren
   * per seconde niets is meetbare rekentijd op een kern die óók de LoRa-radio
   * bedient, en de radio gaat voor. Alles waar deze drie op letten wordt in
   * SECONDEN gemeten -- een vervaltijd, een rustperiode, een venster van 45 s --
   * dus een nauwkeurigheid van 250 ms verandert geen enkel besluit.
   *
   * NA loopMonitors(): een forcering die nu vervalt hoort niet nog een ronde mee
   * te doen. En binnen de tik loopSim() vóór loopRecovery(), want een forcering
   * die net vervallen is moet in dezelfde tik alweer met de GEMETEN waarde geteld
   * worden -- anders wacht het herstelvenster op een forcering die er niet meer
   * is. */
  if ((long)(now - _next_tick) >= 0) {
    _next_tick = now + TICK_MS;
    loopSim();
    loopTest();
    loopRecovery();
  }

  /* Wegschrijven gebeurt uitgesteld en nooit tijdens een ping. Een SPIFFS-
   * schrijfronde kost tientallen milliseconden; die wachten we liever af op een
   * moment dat we zelf kiezen, en meerdere wijzigingen achter elkaar (drie keer
   * "sensor set" in een DM-sessie) worden zo één schrijfronde. */
  if (_dirty && _phase == PING_IDLE && (long)(millis() - _save_at) >= 0) {
    _dirty = false;
    MonitorStore::save(SPIFFS, _cfg);
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
    _mains = _last_volts >= (_cfg.mains_lo + _cfg.mains_hi) / 2.0f;
    _seeded = true;
    _agree = 0;
    return;
  }

  bool wants_mains;
  if (_last_volts >= _cfg.mains_hi) {
    wants_mains = true;
  } else if (_last_volts <= _cfg.mains_lo) {
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

/* De GEMETEN wifi-toestand. Alleen de ping-machine en monitorsPaused() gebruiken
 * deze; zie de uitleg bij SIMULEREN in de header voor waarom die twee juist niet
 * naar de gesimuleerde waarde mogen kijken. */
bool MonitorSensors::wifiReallyOnline() const {
  return _wifi != NULL && _wifi->isOnline();
}

/* De GERAPPORTEERDE toestanden. Deze twee gaan naar de telemetrie, de pagina, de
 * DM-lijst en de waarschuwingen, en dus horen zij de forcering te volgen -- als
 * de een iets anders zegt dan de ander, is de simulatie zelf een bron van
 * verwarring in plaats van een test.
 *
 * Geen millis() hier: het verlopen van een forcering doet loopSim(), zodat deze
 * twee niets anders zijn dan een veld lezen. Ze worden uit querySensors() en uit
 * de webserver aangeroepen en horen geen tijd of toestand te kennen. */
bool MonitorSensors::isMains() const {
  switch (_sim[SIM_POWER].mode) {
    case SIM_UP:   return true;    /* netvoeding aan, batterijvoeding uit */
    case SIM_DOWN: return false;   /* op batterij -- de slechte stand */
    default:       return _mains;
  }
}

bool MonitorSensors::isWifiOnline() const {
  switch (_sim[SIM_WIFI].mode) {
    case SIM_UP:   return true;
    case SIM_DOWN: return false;
    default:       return wifiReallyOnline();
  }
}

/* ===================== de ping-bewaking =====================
 *
 * ALS WIFI WEGVALT: BEVRIEZEN, NIET DOWN MELDEN.
 *
 * Dit is de belangrijkste afspraak van dit onderdeel. Zonder WiFi meet een
 * ping niets over de dienst -- hij meet onze eigen verbinding. Acht monitors
 * die dan op "down" springen leveren acht waarschuwingen over één storing die
 * al op kanaal 4 te zien is, en ze wissen bovendien de laatste waarheid die we
 * wél hadden.
 *
 * De regel, in drieën:
 *
 *  1. Geen WiFi -> er wordt niet gepingd. Geen mislukking geteld, geen
 *     toestand veranderd; up/down blijft staan op wat er het laatst gemeten is.
 *     Een lopende ping wordt afgebroken en de uitslag weggegooid, want die zou
 *     over de wegvallende verbinding gaan en niet over de dienst.
 *  2. Wie de telemetrie leest, ziet dit: kanaal 4 (wifi) staat op 0, dus de
 *     monitorkanalen zijn per definitie oude waarden. Daarom hoeven en mogen
 *     ze niet op nul: "onbekend" bestaat niet in LPP_SWITCH, en van de twee
 *     leugens is "de laatst gemeten stand" de nuttigste.
 *  3. WiFi terug -> eerst WIFI_GRACE_MS niets doen. DHCP, de route en de
 *     DNS-servers zijn dan nog niet altijd binnen, en een ping die daarop
 *     stukloopt zou een dienst onterecht down verklaren. In diezelfde tijd
 *     worden de rondes opnieuw uitgesmeerd.
 *
 * Waarschuwingen zijn in die hele periode onderdrukt (zie monitorAlert): een
 * dienst die down is terwijl wij blind zijn, is niet vastgesteld.
 */
void MonitorSensors::loopMonitors() {
  /* wifiReallyOnline() en niet isWifiOnline(): de ping-machine hangt aan de
   * ECHTE verbinding. Zie de uitleg bij SIMULEREN in de header -- "wifi neer"
   * forceren mag niet stil de hele bewaking uitzetten, en "wifi op" forceren mag
   * niet acht monitors laten pingen over een verbinding die er niet is. */
  const bool online = wifiReallyOnline();
  unsigned long now = millis();

  if (online != _wifi_was_online) {
    _wifi_was_online = online;

    if (online) {
      _wifi_ok_since = now;
      /* Opnieuw uitsmeren: anders staan na de insteltijd alle monitors
       * tegelijk klaar en dringen ze achter elkaar in de rij. */
      for (int i = 0; i < MAX_MONITORS; i++) {
        _mon[i].next_check = now + WIFI_GRACE_MS + (unsigned long)i * PING_GAP_MS;
        _mon[i].addr_expiry = now;   /* adressen opnieuw opzoeken: het netwerk kan een ander zijn */
      }
      MESH_DEBUG_PRINTLN("MonitorSensors: wifi terug, bewaking start over %lu s", (unsigned long)(WIFI_GRACE_MS / 1000));
    } else {
      /* Lopende meting weggooien. abortPing() rekent hem NIET als mislukking. */
      if (_phase != PING_IDLE) abortPing();
      /* Liep er een ad-hoc ping? Die krijgt een eerlijke uitslag in plaats van
       * stil te verdwijnen: de vrager wacht erop. */
      if (_adhoc.state == ADHOC_PENDING || _adhoc.state == ADHOC_BUSY) {
        finishAdhoc(_adhoc.done ? "wifi viel weg tijdens de meting" : "geen wifi, niet gepingd");
      }
      MESH_DEBUG_PRINTLN("MonitorSensors: wifi weg, bewaking bevroren (toestanden blijven staan)");
    }
  }

  if (!online) return;
  if ((long)(now - (_wifi_ok_since + WIFI_GRACE_MS)) < 0) return;   /* insteltijd */

  /* Gemelde diensten laten verouderen. Staat VOOR de ping-machine en niet erin:
   * bij deze soort komt er geen uitslag binnen om aan mee te liften, en het
   * verstrijken van de tijd is precies de gebeurtenis waar we op letten. */
  loopPushStale();

  switch (_phase) {
    case PING_RESOLVING: {
      uint8_t st = s_ping.dns_state;
      /* AD-HOC: de naamsopzoeking van de vrij opgegeven ping. Aparte tak, want
       * hierachter wordt _mon[_busy_slot] gebruikt en een ad-hoc heeft geen
       * vakje. */
      if (_busy_slot == ADHOC_SLOT) {
        if (st == 1) {
          _adhoc.addr_v4 = s_ping.dns_addr;
          startAdhocOnePing();
        } else if (st == 2 || (long)(now - _phase_deadline) >= 0) {
          MESH_DEBUG_PRINTLN("MonitorSensors: ad-hoc naam '%s' niet op te lossen", s_ping.host);
          finishAdhoc("kon het adres niet opzoeken");
        }
        break;
      }
      if (st == 1) {
        if (_busy_slot >= 0) {
          _mon[_busy_slot].addr_v4    = s_ping.dns_addr;
          _mon[_busy_slot].addr_expiry = now + DNS_TTL_MS;
          startPing(_busy_slot);
        } else {
          abortPing();     /* vakje is tussentijds verdwenen */
        }
      } else if (st == 2 || (long)(now - _phase_deadline) >= 0) {
        /* Naam niet op te lossen. Dat telt als een mislukte ronde: voor wie de
         * dienst gebruikt is "de naam bestaat niet" net zo stuk als "geen
         * antwoord". Het adres wordt vergeten, zodat de volgende ronde het
         * opnieuw probeert. */
        int slot = _busy_slot;
        MESH_DEBUG_PRINTLN("MonitorSensors: naam '%s' niet op te lossen", s_ping.host);
        abortPing();
        if (slot >= 0) applyResult(slot, false, 0);
      }
      break;
    }

    case PING_RUNNING: {
      uint8_t st = s_ping.state;
      /* AD-HOC: één van de n pings is klaar (of de noodrem sloeg toe). Uitslag
       * bijschrijven en via REAPING naar de volgende ping of naar het einde --
       * _busy_slot blijft ADHOC_SLOT zodat REAPING weet dat dit ad-hoc is. */
      if (_busy_slot == ADHOC_SLOT) {
        if (st != 0) {
          recordAdhocResult(st == 1, s_ping.ms);
          _phase = PING_REAPING;
          _phase_deadline = now + 100;
        } else if ((long)(now - _phase_deadline) >= 0) {
          /* Geen enkele callback: de sessie hangt. Zelf opruimen en als timeout
           * tellen; REAPING ziet dan een lege handle en gaat door. */
          if (s_ping.handle != NULL) {
            esp_ping_stop(s_ping.handle);
            esp_ping_delete_session(s_ping.handle);
            s_ping.handle = NULL;
          }
          recordAdhocResult(false, 0);
          _phase = PING_REAPING;
          _phase_deadline = now + 100;
        }
        break;
      }
      if (st != 0) {
        int slot = _busy_slot;
        uint32_t ms = s_ping.ms;
        if (slot >= 0) applyResult(slot, st == 1, ms);

        /* De sessie wordt niet hier opgeruimd maar één ronde later. De
         * callback die we net gezien hebben loopt in de ping-taak; die taak
         * heeft na het aanroepen ervan nog werk aan zijn eigen structuur, en
         * die structuur is precies wat esp_ping_delete_session() vrijgeeft.
         * Honderd milliseconde wachten kost ons niets -- er wordt niet
         * geblokkeerd, alleen een fase later gekeken -- en het haalt de race
         * eruit. */
        _busy_slot = -1;
        _phase = PING_REAPING;
        _phase_deadline = now + 100;
      } else if ((long)(now - _phase_deadline) >= 0) {
        /* De noodrem: geen enkele callback binnen PING_DEADLINE_MS. Dat is
         * geen normale mislukking (die geeft on_ping_timeout), dus hier is er
         * iets met de sessie zelf aan de hand. Als mislukking rekenen en
         * opruimen. */
        int slot = _busy_slot;
        MESH_DEBUG_PRINTLN("MonitorSensors: ping-sessie gaf geen uitslag, opgeruimd");
        abortPing();
        if (slot >= 0) applyResult(slot, false, 0);
      }
      break;
    }

    case PING_REAPING:
      if ((long)(now - _phase_deadline) >= 0) {
        if (s_ping.handle != NULL) {
          esp_ping_delete_session(s_ping.handle);
          s_ping.handle = NULL;
        }
        /* AD-HOC: nog een ping te gaan? Dan de volgende; anders de uitslag
         * afronden. _busy_slot bleef ADHOC_SLOT (de monitorpaden zetten hem op
         * -1 vóór REAPING), dus dat is het onderscheid. */
        if (_busy_slot == ADHOC_SLOT) {
          if (_adhoc.done < _adhoc.want) {
            startAdhocOnePing();
          } else {
            finishAdhoc(NULL);
          }
        } else {
          _phase = PING_IDLE;
          _next_ping_at = now + PING_GAP_MS;
        }
      }
      break;

    case PING_IDLE:
      if ((long)(now - _next_ping_at) >= 0) startNextPing();
      break;
  }
}

/* ===================== gemelde diensten laten verouderen =====================
 *
 * De regel staat bij PUSH_STALE_FACTOR in de header. Hier alleen de uitvoering,
 * en één ding dat de moeite van het benoemen waard is: verouderen zet
 * seeded op false en NIET up op false.
 *
 * "Onbekend" en "neer" zijn twee verschillende uitspraken. Wij weten niet dat de
 * dienst plat ligt -- wij weten dat de MELDER niets meer zegt. Van die twee is
 * alleen de tweede vastgesteld, en de bewaking hoort niets te beweren wat ze
 * niet gemeten heeft. Dat de telemetrie er hetzelfde uitziet (LPP_SWITCH 0, want
 * dat formaat kent geen "onbekend") verandert daar niets aan: de pagina, de
 * DM-lijst en de waarschuwingstekst zeggen wel wat er echt aan de hand is.
 */
unsigned long MonitorSensors::monitorStaleRef(int slot) const {
  const unsigned long blind_until = _wifi_ok_since + WIFI_GRACE_MS;
  const unsigned long reported    = _mon[slot].last_report;

  /* De latere van de twee. Zonder wifi kan /hook ons niet bereiken, dus die tijd
   * telt niet mee als stilte van de melder. */
  return ((long)(blind_until - reported) > 0) ? blind_until : reported;
}

uint32_t MonitorSensors::monitorStaleMs(int slot) const {
  uint16_t period = _cfg.mons[slot].interval_s;
  if (period == 0) period = MON_INTERVAL_DEFAULT;

  uint32_t limit = (uint32_t)period * 1000UL * PUSH_STALE_FACTOR;
  return (limit < PUSH_STALE_MIN_MS) ? PUSH_STALE_MIN_MS : limit;
}

void MonitorSensors::loopPushStale() {
  const unsigned long now = millis();

  for (int i = 0; i < MAX_MONITORS; i++) {
    if (!monitorIsPush(i)) continue;

    MonState& m = _mon[i];

    /* Nog nooit een melding gehad? Dan is er niets verouderd; deze dienst is
     * simpelweg nog onbekend. Zonder deze regel zou elke herstart 90 s later een
     * waarschuwing geven over diensten waarover we nooit iets wisten. */
    if (m.last_report == 0) continue;

    const bool over = (long)(now - (monitorStaleRef(i) + monitorStaleMs(i))) >= 0;
    if (over == m.stale) continue;

    m.stale = over;
    if (over) {
      /* Toestand valt weg. up blijft staan zoals hij was; hij doet niets meer
       * zolang seeded false is, en zo is de laatste bekende stand nog te zien in
       * de debug-uitvoer als iemand ernaar zoekt. */
      m.seeded = false;
      m.agree  = 0;
      MESH_DEBUG_PRINTLN("MonitorSensors: '%s' geen melding meer binnen %lu s, toestand onbekend",
                         _cfg.mons[i].name, (unsigned long)(monitorStaleMs(i) / 1000));
    }
  }
}

/* Kiest het vakje dat het langst op zijn ronde wacht. Eén ping tegelijk, dus
 * hier wordt gekozen en niet gestapeld. Het meest achterlopende vakje eerst
 * houdt de intervallen eerlijk als er meer monitors zijn dan er tijd is. */
void MonitorSensors::startNextPing() {
  /* AD-HOC KRIJGT VOORRANG. Een mens wacht op zijn uitslag, een monitor niet:
   * een wachtende ad-hoc gaat vóór de monitorronde. De lopende monitorping was
   * al afgebroken toen de ad-hoc werd aangevraagd (zie startAdhocPing), dus hier
   * hoeft alleen de start gekozen te worden. */
  if (_adhoc.state == ADHOC_PENDING) {
    startAdhocResolve();
    return;
  }

  unsigned long now = millis();
  int  best = -1;
  long best_overdue = -1;

  for (int i = 0; i < MAX_MONITORS; i++) {
    if (!monitorUsed(i)) continue;
    /* Een gemelde dienst pingen wij niet. Zou hij hier meedoen, dan werd zijn
     * adres "-" opgezocht, dat mislukt altijd, en dan zou hij bij elke ronde als
     * mislukking geteld worden -- twee bronnen voor één toestand, en de
     * verkeerde zou winnen. */
    if (monitorIsPush(i)) continue;
    long overdue = (long)(now - _mon[i].next_check);
    if (overdue >= 0 && overdue > best_overdue) {
      best = i;
      best_overdue = overdue;
    }
  }
  if (best < 0) return;

  /* De volgende ronde wordt NU al gezet, niet na de uitslag: zo bepaalt het
   * interval de tik en niet de duur van de meting. */
  _mon[best].next_check = now + (unsigned long)_cfg.mons[best].interval_s * 1000UL;

  /* Adres nog geldig? Dan hoeft er niets opgezocht te worden. */
  if (_mon[best].addr_v4 != 0 && (long)(now - _mon[best].addr_expiry) < 0) {
    startPing(best);
  } else {
    startResolve(best);
  }
}

void MonitorSensors::startResolve(int slot) {
  StrHelper::strncpy(s_ping.host, _cfg.mons[slot].host, sizeof(s_ping.host));

  /* Eerst kijken of het al een adres IS. ip4addr_aton() rekent alleen en praat
   * met niemand, dus dit spaart bij een monitor op een IP elke DNS-vraag uit. */
  ip4_addr_t lit;
  if (ip4addr_aton(s_ping.host, &lit)) {
    _mon[slot].addr_v4    = ip4_addr_get_u32(&lit);
    _mon[slot].addr_expiry = millis() + DNS_TTL_MS;
    startPing(slot);
    return;
  }

  s_ping.dns_state = 0;
  s_ping.dns_addr  = 0;
  _busy_slot = (int8_t)slot;
  _phase = PING_RESOLVING;
  _phase_deadline = millis() + DNS_DEADLINE_MS;

  /* try_callback en niet callback: die tweede WACHT tot lwIP plek heeft in zijn
   * berichtenrij, en wachten mag hier niet. Is er geen plek, dan proberen we
   * het over PING_GAP_MS gewoon opnieuw; dat is een tekort aan onze kant en
   * geen storing van de dienst, dus er wordt niets geteld. */
  if (tcpip_try_callback(mon_do_resolve, this) != ERR_OK) {
    MESH_DEBUG_PRINTLN("MonitorSensors: lwIP-rij vol, naam later opzoeken");
    _busy_slot = -1;
    _phase = PING_IDLE;
    _next_ping_at = millis() + PING_GAP_MS;
    /* Deze ronde is niet gemeten; meteen weer aan de beurt zetten. */
    _mon[slot].next_check = millis();
  }
}

void MonitorSensors::startPing(int slot) {
  /* Alle velden zelf zetten in plaats van ESP_PING_DEFAULT_CONFIG(): die macro
   * gebruikt TASK_EXTRA_STACK_SIZE, dat niet uit ping_sock.h komt, en hij zet
   * count op 5. Wij willen er precies één -- de uitslag van één pakket is wat
   * de toestandsmachine verwerkt, en vijf pakketten per ronde is vijf keer zo
   * veel verkeer voor dezelfde ja/nee.
   *
   * ttl NIET op nul laten staan: een pakket met ttl 0 wordt bij de eerste
   * router weggegooid en dan lijkt elke dienst buiten het eigen netwerk down. */
  esp_ping_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.count           = 1;
  cfg.interval_ms     = 1000;
  cfg.timeout_ms      = PING_TIMEOUT_MS;
  cfg.data_size       = 32;
  cfg.tos             = 0;
  cfg.ttl             = 64;
  cfg.task_stack_size = 3072;
  /* Prioriteit 2 staat boven de Arduino-lus (1) maar ver onder de lwIP-taak.
   * Die taak slaapt in select() tot er een antwoord is, dus hij kost geen
   * rekentijd; hij mag daarom hoger staan zonder de radio te hinderen. */
  cfg.task_prio       = 2;
  cfg.interface       = 0;
  ip_addr_set_ip4_u32(&cfg.target_addr, _mon[slot].addr_v4);

  esp_ping_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.cb_args         = NULL;
  cbs.on_ping_success = mon_ping_success;
  cbs.on_ping_timeout = mon_ping_timeout;
  /* on_ping_end laten we leeg: met count = 1 komt success of timeout precies
   * één keer, en dat is al het signaal dat we nodig hebben. */
  cbs.on_ping_end     = NULL;

  s_ping.state = 0;
  s_ping.ms    = 0;

  if (s_ping.handle != NULL) {     /* zou niet moeten kunnen; opruimen */
    esp_ping_delete_session(s_ping.handle);
    s_ping.handle = NULL;
  }

  if (esp_ping_new_session(&cfg, &cbs, &s_ping.handle) != ESP_OK) {
    /* Geen geheugen of geen socket: onze kant, niet die van de dienst. Niets
     * tellen en het later opnieuw proberen. */
    MESH_DEBUG_PRINTLN("MonitorSensors: kan geen ping-sessie maken");
    s_ping.handle = NULL;
    _busy_slot = -1;
    _phase = PING_IDLE;
    _next_ping_at = millis() + PING_GAP_MS;
    return;
  }

  if (esp_ping_start(s_ping.handle) != ESP_OK) {
    esp_ping_delete_session(s_ping.handle);
    s_ping.handle = NULL;
    _busy_slot = -1;
    _phase = PING_IDLE;
    _next_ping_at = millis() + PING_GAP_MS;
    return;
  }

  _busy_slot = (int8_t)slot;
  _phase = PING_RUNNING;
  _phase_deadline = millis() + PING_DEADLINE_MS;
}

/* Alles afbreken zonder de uitslag te tellen. Wordt gebruikt als WiFi wegvalt,
 * als het vakje verdwijnt en als noodrem. */
void MonitorSensors::abortPing() {
  if (s_ping.handle != NULL) {
    esp_ping_stop(s_ping.handle);
    esp_ping_delete_session(s_ping.handle);
    s_ping.handle = NULL;
  }
  s_ping.state = 0;
  s_ping.dns_state = 0;
  _busy_slot = -1;
  _phase = PING_IDLE;
  _next_ping_at = millis() + PING_GAP_MS;
}

/* Een uitslag door de toestandsmachine halen. Zelfde vorm als samplePower():
 * een enkele meting verandert niets, een aantal metingen achter elkaar wel. */
void MonitorSensors::applyResult(int slot, bool ok, uint32_t ms) {
  MonState& m = _mon[slot];

  m.checks++;
  if (!ok) m.fails++;
  if (ok) m.last_ms = ms;

  /* De eerste GELUKTE ronde zet de toestand meteen: dat kost geen alarm en het
   * scheelt een minuut voordat een dashboard iets zinnigs toont. De eerste
   * MISLUKTE ronde doet dat niet -- die gaat door de teller, want anders zou
   * een node die opstart terwijl een dienst even hapert direct waarschuwen. */
  if (ok && !m.seeded) {
    m.seeded = true;
    m.up = true;
    m.agree = 0;
    MESH_DEBUG_PRINTLN("MonitorSensors: %s up (%lu ms)", _cfg.mons[slot].name, (unsigned long)ms);
    return;
  }

  if (m.seeded && ok == m.up) {   /* bevestigt wat we al dachten */
    m.agree = 0;
    return;
  }

  const uint8_t need = ok ? PINGS_TO_UP : PINGS_TO_DOWN;
  if (++m.agree >= need) {
    m.up = ok;
    m.seeded = true;
    m.agree = 0;
    MESH_DEBUG_PRINTLN("MonitorSensors: %s -> %s", _cfg.mons[slot].name, ok ? "up" : "DOWN");
  }
}

/* ===================== monitors opvragen ===================== */

uint8_t MonitorSensors::getNumMonitors() const {
  uint8_t n = 0;
  for (int i = 0; i < MAX_MONITORS; i++) if (_cfg.mons[i].channel != 0) n++;
  return n;
}

bool MonitorSensors::monitorUsed(int slot) const {
  return slot >= 0 && slot < MAX_MONITORS && _cfg.mons[slot].channel != 0;
}

uint8_t     MonitorSensors::monitorChannel(int slot) const  { return monitorUsed(slot) ? _cfg.mons[slot].channel : 0; }
const char* MonitorSensors::monitorName(int slot) const     { return monitorUsed(slot) ? _cfg.mons[slot].name : ""; }
const char* MonitorSensors::monitorHost(int slot) const     { return monitorUsed(slot) ? _cfg.mons[slot].host : ""; }
uint16_t    MonitorSensors::monitorInterval(int slot) const  { return monitorUsed(slot) ? _cfg.mons[slot].interval_s : 0; }

/* DEZE TWEE ZIJN HET HELE SIMULATIEPAD voor de monitors.
 *
 * Staat er een forcering op dit vakje, dan geven zij de geforceerde stand; de
 * gemeten stand in _mon[slot] blijft ongemoeid en loopt eronder door, zodat het
 * aflopen van de forcering terugvalt op een ACTUELE waarde en niet op de waarde
 * van een minuut geleden.
 *
 * Omdat alles wat naar buiten gaat langs deze twee loopt -- querySensors(),
 * monitorAlert(), de pagina, de DM-lijst -- hoeft er nergens anders iets van de
 * simulatie te weten. Dat is de bedoeling: één plek waar de forcering wordt
 * toegepast betekent dat er geen pad kan zijn dat hem overslaat.
 *
 * Een geforceerd vakje is per definitie 'seeded': er IS een mening, hij komt
 * alleen niet uit een ping. */
bool MonitorSensors::monitorIsUp(int slot) const {
  if (!monitorUsed(slot)) return false;
  switch (_sim[SIM_MON_FIRST + slot].mode) {
    case SIM_UP:   return true;
    case SIM_DOWN: return false;
    default:       return _mon[slot].seeded && _mon[slot].up;
  }
}

bool MonitorSensors::monitorSeeded(int slot) const {
  if (!monitorUsed(slot)) return false;
  if (_sim[SIM_MON_FIRST + slot].mode != SIM_OFF) return true;
  return _mon[slot].seeded;
}

uint32_t    MonitorSensors::monitorPingMs(int slot) const   { return monitorUsed(slot) ? _mon[slot].last_ms : 0; }
uint32_t    MonitorSensors::monitorChecks(int slot) const   { return monitorUsed(slot) ? _mon[slot].checks : 0; }
uint32_t    MonitorSensors::monitorFails(int slot) const    { return monitorUsed(slot) ? _mon[slot].fails : 0; }

bool MonitorSensors::monitorIsPush(int slot) const {
  return monitorUsed(slot) && strcmp(_cfg.mons[slot].host, PUSH_HOST) == 0;
}

uint32_t MonitorSensors::monitorReportAge(int slot) const {
  if (!monitorIsPush(slot) || _mon[slot].last_report == 0) return 0;
  return (uint32_t)((millis() - _mon[slot].last_report) / 1000);
}

uint32_t MonitorSensors::monitorStaleSecs(int slot) const {
  if (!monitorIsPush(slot)) return 0;
  return monitorStaleMs(slot) / 1000;
}

/* Een geforceerd vakje is nooit 'stil': er IS een mening over deze dienst, hij
 * komt alleen niet van de melder. Zonder deze uitzondering zou een gemelde
 * dienst die geforceerd op 'op' staat tegelijk "op" en "geen melding meer"
 * kunnen zijn, en dat zijn twee waarheden over hetzelfde kanaal. */
bool MonitorSensors::monitorIsStale(int slot) const {
  if (!monitorIsPush(slot)) return false;
  if (_sim[SIM_MON_FIRST + slot].mode != SIM_OFF) return false;
  return _mon[slot].stale;
}

/* Ook hier de ECHTE wifi: "pauze" is een uitspraak over of wij hebben kunnen
 * meten, en dat hangt aan de verbinding en niet aan wat wij erover melden. */
bool MonitorSensors::monitorsPaused() const {
  if (!wifiReallyOnline()) return true;
  return (long)(millis() - (_wifi_ok_since + WIFI_GRACE_MS)) < 0;
}

int MonitorSensors::findByName(const char* name) const {
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (_cfg.mons[i].channel != 0 && strcmp(_cfg.mons[i].name, name) == 0) return i;
  }
  return -1;
}

int MonitorSensors::findByChannel(uint8_t ch) const {
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (_cfg.mons[i].channel == ch) return i;
  }
  return -1;
}

int MonitorSensors::slotOfNth(int nth) const {
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (_cfg.mons[i].channel == 0) continue;
    if (nth-- == 0) return i;
  }
  return -1;
}

/* ===================== waarschuwingen =====================
 *
 * Geen eigen lus en geen eigen verzendweg: alles gaat via alertIf() van
 * SensorMesh. De HERHALING tot bevestiging zit ook hier -- niet als een tweede
 * pad, maar als een voorwaarde die per periode even op onwaar valt en weer op
 * waar komt, zodat alertIf zijn eigen lus opnieuw langs de ontvangers stuurt.
 * Zie de header bij HERHALEN TOT EEN MENS BEVESTIGT en stepStoringAlert().
 */

/* De ruwe storingsvoorwaarde, zonder herhaal-/ack-/remlogica. Gedeeld met
 * confirmAlerts(), zodat "is er een storing" op één plek beslist wordt.
 *
 * Een FORCERING komt hier het echte pad binnen: sim DOWN telt als storing, sim UP
 * onderdrukt hem. De pauzeregel geldt alleen voor de GEMETEN toestand -- een
 * forcering is een bewering en geen meting, en die blijft geldig terwijl onze
 * wifi weg is (LoRa staat daar los van). */
bool MonitorSensors::monitorWantAlert(int slot) const {
  if (!monitorUsed(slot)) return false;
  const uint8_t sim = _sim[SIM_MON_FIRST + slot].mode;
  if (sim == SIM_UP)   return false;
  if (sim == SIM_DOWN) return true;
  /* Verouderd EN neer kan niet tegelijk: verouderen zet seeded op false. */
  return !monitorsPaused() && ((_mon[slot].seeded && !_mon[slot].up) || _mon[slot].stale);
}

/* De gedeelde kern. Zie de header voor het waarom; hier het recept.
 *
 * Retourneert wat main.cpp aan alertIf() voert. Beheert vier dingen:
 *  - eerste melding (armt, zet down_sent, meldt armed_first);
 *  - herhalen: als er een melding staat en de periode verstreken is, valt de
 *    voorwaarde deze ronde op onwaar (de "puls laag") zodat alertIf de Trigger
 *    opruimt; de ronde erna komt hij op waar en her-armt alertIf -> nieuwe zending;
 *  - stoppen bij menselijke bevestiging (human_ack);
 *  - de harde bovengrens MAX_ALERT_REPEATS.
 */
bool MonitorSensors::stepStoringAlert(bool want, bool& alerting, bool& down_sent,
                                      unsigned long& down_since, AlertRepeat& r,
                                      int idx_for_log, bool* armed_first) {
  if (armed_first) *armed_first = false;
  const unsigned long now = millis();

  /* Storing voorbij: alles van de herhaling terug naar nul. down_sent NIET --
   * dat blijft staan voor de herstelmelding, die het na de storing opruimt. */
  if (!want) {
    alerting = false;
    r.human_ack = false; r.pulse = false; r.max_logged = false;
    r.repeats = 0; r.next_repeat = 0;
    return false;
  }

  /* Een mens heeft bevestigd: de pieper zwijgt, ook al is de storing er nog.
   * down_sent blijft staan, dus de herstelmelding komt straks gewoon. */
  if (r.human_ack) { alerting = false; return false; }

  const bool rep_on = (_cfg.repeat_s > 0);

  /* Bovengrens bereikt: stoppen met herhalen. De monitor blijft down op de pagina
   * en in de telemetrie; alleen de DM's houden op. Eén regel in de log. */
  if (rep_on && r.repeats >= MAX_ALERT_REPEATS) {
    if (!r.max_logged) {
      MESH_DEBUG_PRINTLN("MonitorSensors: alarm %d bovengrens van %d herhalingen bereikt, stopt met herhalen",
                         idx_for_log, (int)MAX_ALERT_REPEATS);
      r.max_logged = true;
    }
    alerting = false;
    return false;
  }

  if (r.pulse) {
    /* Dit is de her-arm-ronde na een puls laag: door naar het armen hieronder. */
    r.pulse = false;
  } else if (alerting) {
    /* Er staat een melding. Tijd voor een herhaling? Dan één ronde onwaar geven
     * zodat alertIf de Trigger opruimt; volgende ronde her-armen we. */
    if (rep_on && (long)(now - r.next_repeat) >= 0) {
      r.pulse = true;
      alerting = false;
      return false;
    }
    return true;   /* melding staat, niets te doen */
  }

  /* Armen -- eerste melding of een herhaling. Door dezelfde rem als elke andere
   * waarschuwing: een herhaling moet een plaats in de wachtrij heroveren, dus een
   * golf herhalingen verdringt zichzelf netjes in plaats van de band vol te
   * zetten. Lukt het armen nu niet, dan volgende ronde opnieuw -- geen verlies. */
  if (alertsActive() >= MAX_MONITOR_ALERTS) return false;

  const bool first = !down_sent;
  alerting = true;
  if (first) {
    noteDownSent(down_sent, down_since);
    if (armed_first) *armed_first = true;
  } else if (r.repeats < 255) {
    r.repeats++;
  }
  if (rep_on) r.next_repeat = now + (unsigned long)_cfg.repeat_s * 1000UL;
  return true;
}

bool MonitorSensors::monitorAlert(int slot) {
  if (!monitorUsed(slot)) {
    /* Vakje is leeg (of net verwijderd): voorwaarde uit, zodat main.cpp met
     * alertIf(false, ...) de bijbehorende Trigger opruimt. */
    if (slot >= 0 && slot < MAX_MONITORS) _mon[slot].alerting = false;
    return false;
  }

  MonState& m = _mon[slot];
  const bool want = monitorWantAlert(slot);

  bool first = false;
  const bool res = stepStoringAlert(want, m.alerting, m.down_sent, m.down_since,
                                    m.rep, (int)_cfg.mons[slot].channel, &first);
  if (first) {
    /* Alleen op de EERSTE melding vastgelegd, zodat de herstel- en herhaaltekst
     * weten of dit een simulatie of een stille melder was. */
    m.was_stale = m.stale;
    m.was_sim   = (_sim[SIM_MON_FIRST + slot].mode != SIM_OFF);
  }
  return res;
}

/* Vaste buffer. alertIf() kopieert de tekst meteen in de Trigger, dus hij hoeft
 * maar tot het einde van die aanroep te bestaan.
 *
 * 160 EN NIET 80, nagerekend. De langste tekst is een gesimuleerde ping-monitor:
 *
 *   SIM_MARK  "TEST "                                            5
 *   naam                                    MON_NAME_LEN-1  =   16
 *   " onbereikbaar ("  + ")"                                     17
 *   adres                                   MON_HOST_LEN-1  =   40
 *   SIM_TAIL  " -- dit is een SIMULATIE, geen echte storing"      45
 *                                                              ----
 *                                                               123
 *
 * 160 geeft daar 37 byte marge boven. De bovengrens die ECHT telt zit in
 * SensorMesh::sendAlert: die schrijft 5 + strlen(text) in een pakket van
 * MAX_PACKET_PAYLOAD (184), dus 179 tekens is het absolute plafond. 160 blijft
 * daaronder, ook als deze buffer helemaal vol staat. */
static char s_alert_buf[160];

/* HET MERKTEKEN EN DE UITLEG, op één plek.
 *
 * Wie een bericht op zijn telefoon krijgt moet ZONDER NADENKEN zien of dit een
 * test is of dat zijn router echt uit staat. Daarom twee dingen en niet één: een
 * merkteken vooraan (dat zie je in de meldingsbalk, waar de tekst wordt
 * afgekapt) EN een uitleg in gewone taal achteraan (dat lees je als je het
 * bericht opent). Een merkteken alleen is een geheimtaal die je moet kennen; een
 * uitleg alleen staat te ver naar achteren om op tijd te helpen. */
static const char SIM_MARK[] = "TEST ";
static const char SIM_TAIL[] = " -- dit is een SIMULATIE, geen echte storing";

/* Het herhaalachtervoegsel: " (herhaling N)" zodra dit een tweede of latere
 * zending is. Dat getal is voor de ontvanger het teken dat de melding nog
 * openstaat -- de pieper piept nog, niemand heeft "ok" gestuurd. Op de eerste
 * melding (repeats == 0) staat er niets. Vaste buffer, want alertText gebruikt
 * geen String in dit pad. */
static const char* repeatSuffix(uint8_t repeats) {
  static char rp[20];
  if (repeats == 0) { rp[0] = 0; return rp; }
  snprintf(rp, sizeof(rp), " (herhaling %u)", (unsigned)repeats);
  return rp;
}

const char* MonitorSensors::monitorAlertText(int slot) const {
  if (!monitorUsed(slot)) { s_alert_buf[0] = 0; return s_alert_buf; }

  const char* rp = repeatSuffix(_mon[slot].rep.repeats);

  /* Gesimuleerd? Dan gaat de gewone tekst tussen het merkteken en de uitleg. Het
   * middenstuk is hetzelfde als bij een echte storing, en dat is de bedoeling:
   * de ontvanger leest dezelfde melding en ziet erbij dat hij niet echt is. Het
   * herhaalachtervoegsel staat vóór de uitleg, zodat "(herhaling 3)" bij de
   * melding hoort en niet achter de disclaimer bengelt. */
  const bool sim = _sim[SIM_MON_FIRST + slot].mode != SIM_OFF;
  if (sim) {
    snprintf(s_alert_buf, sizeof(s_alert_buf), "%s%s %s (%s)%s%s",
             SIM_MARK, _cfg.mons[slot].name,
             monitorIsPush(slot) ? "gemeld als neer" : "onbereikbaar",
             monitorIsPush(slot) ? "gemeld" : _cfg.mons[slot].host,
             rp, SIM_TAIL);
    return s_alert_buf;
  }

  if (monitorIsPush(slot)) {
    /* Twee heel verschillende boodschappen, en het verschil is voor de ontvanger
     * het halve bericht: bij het eerste ligt de DIENST plat, bij het tweede is
     * de MELDER stil en weten wij niets. Wie dat door elkaar haalt, gaat de
     * verkeerde kant op zoeken. Het adres staat er niet bij -- dat is "-" en zegt
     * niets. */
    if (_mon[slot].stale) {
      snprintf(s_alert_buf, sizeof(s_alert_buf), "%s: geen melding meer (>%lus)%s",
               _cfg.mons[slot].name, (unsigned long)monitorStaleSecs(slot), rp);
    } else {
      snprintf(s_alert_buf, sizeof(s_alert_buf), "%s gemeld als neer%s",
               _cfg.mons[slot].name, rp);
    }
    return s_alert_buf;
  }

  snprintf(s_alert_buf, sizeof(s_alert_buf), "%s onbereikbaar (%s)%s",
           _cfg.mons[slot].name, _cfg.mons[slot].host, rp);
  return s_alert_buf;
}

/* ===================== het bytebudget =====================
 *
 * De uitleg staat in de header bij HET BYTEBUDGET VAN DE TELEMETRIE. Kort: de
 * echte grens zijn bytes en niet monitors, en dat getal hoort zichtbaar te zijn
 * VOORDAT iemand de ruimte opmaakt.
 */

bool MonitorSensors::monitorSendsMs(int slot) const {
  return monitorUsed(slot) && _cfg.mons[slot].send_ms != 0;
}

/* Wat dit vakje in het pakket kost.
 *
 * DE DUURSTE STAND EN NIET DE HUIDIGE. Een monitor die nu neer is kost 3 byte,
 * want zonder 'up' gaat er geen pingtijd mee -- maar zodra hij weer op komt kost
 * hij 9. Zou het budget op de huidige stand rekenen, dan beloofde de pagina
 * ruimte die verdwijnt op het moment dat alles weer werkt. Dat is de verkeerde
 * kant om fout te zitten: er zou dan precies bij HERSTEL afgekapt worden, dus op
 * het moment dat het dashboard weer moet gaan kloppen. */
uint8_t MonitorSensors::monitorTelemBytes(int slot) const {
  if (!monitorUsed(slot)) return 0;
  return TELEM_BYTES_SWITCH_PUB
       + (_cfg.mons[slot].send_ms ? TELEM_BYTES_GENERIC_PUB : 0);
}

bool MonitorSensors::monitorDropped(int slot) const {
  if (slot < 0 || slot >= MAX_MONITORS) return false;
  return (_telem_dropped & ((uint32_t)1 << slot)) != 0;
}

void MonitorSensors::telemBudget(TelemBudget& out) const {
  memset(&out, 0, sizeof(out));
  out.total    = TELEM_BUDGET;
  out.base     = _telem_base;
  out.measured = _telem_measured;

  /* De drie vaste schakelaars van deze klasse: netvoeding, batterijvoeding,
   * wifi. Die staan er altijd, ook als er geen enkele monitor is. */
  const uint16_t fixed = (uint16_t)_telem_base + 3 * TELEM_BYTES_SWITCH_PUB;
  out.fixed = fixed > 255 ? 255 : (uint8_t)fixed;

  uint16_t mons = 0;
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (!monitorUsed(i)) continue;
    mons += monitorTelemBytes(i);
    if (_cfg.mons[i].send_ms) out.num_ms++;
    if (monitorDropped(i)) out.dropped++;
  }
  out.mons = mons > 255 ? 255 : (uint8_t)mons;

  const uint16_t used = fixed + mons;
  out.used = used > 255 ? 255 : (uint8_t)used;
  out.left = used >= out.total ? 0 : (uint8_t)(out.total - used);
}

bool MonitorSensors::telemFits(uint8_t extra_bytes) const {
  TelemBudget b;
  telemBudget(b);
  return (uint16_t)b.used + extra_bytes <= (uint16_t)b.total;
}

/* ===================== herstelmeldingen =====================
 *
 * Waarom dit bestaat en welke drie voorwaarden er gelden, staat in de header bij
 * HERSTELMELDINGEN. Hier staat het uitvoerende deel.
 */

/* "Wij hebben deze storing gemeld." Eén regel, maar wel een eigen functie: hij
 * wordt op vier plaatsen gezet (monitor, gesimuleerde monitor, en de twee vaste
 * kanalen) en het moment waarop down_since gezet wordt bepaalt de duur in het
 * bericht. Vier keer dezelfde twee regels is vier kansen om er één te vergeten. */
void MonitorSensors::noteDownSent(bool& down_sent, unsigned long& down_since) {
  if (!down_sent) {
    down_sent  = true;
    down_since = millis();
  }
}

/* De teller van de rustperiode. Voor één ingang, en dezelfde regel voor de
 * monitors als voor de vaste kanalen -- vandaar losse verwijzingen in plaats van
 * een struct: de twee soorten bewaren hun velden op een andere plek maar volgen
 * hetzelfde recept. */
void MonitorSensors::trackRecovery(bool up_now, bool& down_sent,
                                   unsigned long& down_since,
                                   unsigned long& up_since,
                                   unsigned long& rec_until) {
  unsigned long now = millis();

  if (!up_now) {
    /* Weer neer. De klok van de rustperiode begint straks opnieuw, en een
     * herstelmelding die nog niet de deur uit was gaat NIET meer: die zou
     * beweren dat het opgelost is terwijl het dat niet is. Dit is de rem tegen
     * flappen -- een dienst die elke minuut op en neer gaat, haalt de
     * rustperiode nooit en stuurt dus ook nooit een herstelmelding. */
    up_since  = 0;
    rec_until = 0;
    return;
  }

  if (up_since == 0) up_since = now;      /* begin van aaneengesloten 'op' */

  if (!down_sent) return;                 /* niets gemeld, niets te herstellen */
  if (rec_until != 0) return;             /* venster loopt al of is al geweest */

  /* rhold_s == 0 betekent "meteen melden", en dan is deze vergelijking meteen
   * waar. Geen aparte tak nodig. */
  if ((unsigned long)(now - up_since) < (unsigned long)_cfg.rhold_s * 1000UL) return;

  rec_until = now + RECOVER_HOLD_MS;
  MESH_DEBUG_PRINTLN("MonitorSensors: herstel gemeld na %lus storing",
                     (unsigned long)((now - down_since) / 1000));
}

void MonitorSensors::loopRecovery() {
  /* De monitors. monitorIsUp() en niet _mon[].up: een forcering hoort ook hier
   * mee te doen, want anders zou "forceer op" een lopende storingsmelding wel
   * opruimen maar geen herstelmelding geven -- en dan is de helft van de keten
   * nog steeds niet te testen. */
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (!monitorUsed(i)) {
      /* Vakje verdwenen: de boekhouding mee opruimen, anders erft een nieuwe
       * monitor op dit vakje de storing van zijn voorganger. */
      _mon[i].down_sent = false;
      _mon[i].rec_until = 0;
      _mon[i].up_since  = 0;
      continue;
    }
    /* Tijdens de pauze wordt er niet geteld: zonder wifi weten wij niet of de
     * dienst op is, en een rustperiode die doortikt terwijl wij blind zijn zou
     * een herstel melden dat wij niet hebben vastgesteld. */
    const bool sim_on = _sim[SIM_MON_FIRST + i].mode != SIM_OFF;
    if (monitorsPaused() && !sim_on) continue;

    trackRecovery(monitorIsUp(i), _mon[i].down_sent, _mon[i].down_since,
                  _mon[i].up_since, _mon[i].rec_until);
  }

  /* De twee vaste kanalen. isMains()/isWifiOnline() en dus mét forcering, om
   * dezelfde reden. */
  trackRecovery(isMains(), _fixed_down_sent[FIXED_POWER],
                _fixed_down_since[FIXED_POWER], _fixed_up_since[FIXED_POWER],
                _fixed_rec_until[FIXED_POWER]);
  trackRecovery(isWifiOnline(), _fixed_down_sent[FIXED_WIFI],
                _fixed_down_since[FIXED_WIFI], _fixed_up_since[FIXED_WIFI],
                _fixed_rec_until[FIXED_WIFI]);
}

/* De voorwaarde die main.cpp aan de TWEEDE Trigger per vakje hangt.
 *
 * Waar voor een kort venster en daarna onwaar: een herstelmelding is een
 * gebeurtenis en geen toestand. Bleef zij waar zolang de dienst op is, dan hield
 * elke gezonde monitor een plaats in de wachtrij bezet -- en die zijn er vier. */
bool MonitorSensors::recoverAlert(int slot) {
  if (slot < 0 || slot >= MAX_MONITORS) return false;
  MonState& m = _mon[slot];

  if (!_cfg.recover_alerts || !monitorUsed(slot) || m.rec_until == 0) {
    m.rec_alerting = false;
    return false;
  }

  if ((long)(millis() - m.rec_until) >= 0) {
    /* Venster voorbij. down_sent gaat NU op onwaar en niet eerder: zolang het
     * venster open staat, is het de reden dat er een herstelmelding loopt. Zonder
     * dit wissen zou de volgende keer dat de dienst even wegvalt meteen weer een
     * herstelmelding opleveren zonder dat er een storing gemeld is. */
    m.rec_alerting = false;
    m.down_sent    = false;
    m.was_sim      = false;
    m.rec_until    = 0;
    return false;
  }

  if (m.rec_alerting) return true;
  if (alertsActive() >= MAX_MONITOR_ALERTS) return false;

  m.rec_alerting = true;
  return true;
}

bool MonitorSensors::fixedRecoverAlert(int which) {
  if (which < 0 || which >= FIXED_ALERT_COUNT) return false;

  if (!_cfg.recover_alerts || _fixed_rec_until[which] == 0) {
    _fixed_rec_alerting[which] = false;
    return false;
  }
  if ((long)(millis() - _fixed_rec_until[which]) >= 0) {
    _fixed_rec_alerting[which] = false;
    _fixed_down_sent[which]    = false;
    _fixed_rec_until[which]    = 0;
    return false;
  }
  if (_fixed_rec_alerting[which]) return true;
  if (alertsActive() >= MAX_MONITOR_ALERTS) return false;

  _fixed_rec_alerting[which] = true;
  return true;
}

/* De DUUR in woorden. Onder een minuut in seconden, daarboven in minuten: "na
 * 247s" laat de lezer rekenen en "na 4 min" niet, en boven het uur is "na 3u12"
 * het enige dat nog leesbaar is. Die duur is het enige wat een herstelmelding
 * boven een geruststelling uittilt. */
static const char* durText(unsigned long secs) {
  static char buf[16];
  if (secs < 60)   snprintf(buf, sizeof(buf), "%lus", secs);
  else if (secs < 3600) snprintf(buf, sizeof(buf), "%lu min", secs / 60);
  else snprintf(buf, sizeof(buf), "%luu%02lu", secs / 3600, (secs % 3600) / 60);
  return buf;
}

const char* MonitorSensors::recoverAlertText(int slot) const {
  if (!monitorUsed(slot)) { s_alert_buf[0] = 0; return s_alert_buf; }

  const MonState& m = _mon[slot];
  const unsigned long secs = (millis() - m.down_since) / 1000;

  /* Gemarkeerd als test wanneer de STORING een simulatie was, of wanneer er nu
   * een forcering op staat. Beide, want beide gevallen bestaan: een forcering die
   * vervalt terwijl het herstelvenster open staat, en een forcering op 'op' die
   * juist de herstelmelding uitlokt. */
  const bool sim = m.was_sim || _sim[SIM_MON_FIRST + slot].mode != SIM_OFF;
  const char* mark = sim ? SIM_MARK : "";
  const char* tail = sim ? SIM_TAIL : "";

  if (monitorIsPush(slot) && m.was_stale) {
    /* De MELDER was stil, niet de dienst. Dan is "weer bereikbaar" het verkeerde
     * bericht: wij wisten niets, en nu weten we weer wel iets. Wie de
     * stilte-waarschuwing gekregen heeft, hoort te horen dat de meldingen terug
     * zijn -- niet dat een dienst hersteld is die misschien nooit plat lag. */
    snprintf(s_alert_buf, sizeof(s_alert_buf), "%s%s meldt weer (was %s stil)%s",
             mark, _cfg.mons[slot].name, durText(secs), tail);
    return s_alert_buf;
  }

  snprintf(s_alert_buf, sizeof(s_alert_buf), "%s%s weer %s na %s%s",
           mark, _cfg.mons[slot].name,
           monitorIsPush(slot) ? "op gemeld" : "bereikbaar",
           durText(secs), tail);
  return s_alert_buf;
}

const char* MonitorSensors::fixedRecoverAlertText(int which) const {
  if (which < 0 || which >= FIXED_ALERT_COUNT) { s_alert_buf[0] = 0; return s_alert_buf; }

  const unsigned long secs = (millis() - _fixed_down_since[which]) / 1000;
  /* De storing op een vast kanaal kan bij deze opzet alleen een simulatie zijn
   * (zie fixedAlert), dus het merkteken staat er altijd. Zou fixedAlert() ooit
   * ook op de gemeten toestand gaan vuren, dan hoort hier dezelfde was_sim-vlag
   * te komen als bij de monitors. */
  snprintf(s_alert_buf, sizeof(s_alert_buf), "%s%s terug na %s%s",
           SIM_MARK, which == FIXED_POWER ? "netvoeding" : "wifi",
           durText(secs), SIM_TAIL);
  return s_alert_buf;
}

/* ===================== simuleren en testen =====================
 *
 * De uitleg over WAAROM en over de nummering staat in de header, bij SIMULEREN.
 * Hier staat alleen het uitvoerende deel.
 */

const char* MonitorSensors::simResultText(SimResult r) {
  switch (r) {
    case SIM_OK:        return "ok";
    case SIM_ERR_INDEX: return "geen bestaande sensor (of een leeg monitorvakje)";
    case SIM_ERR_SECS:  return "vervaltijd: 30-3600 s";
    case SIM_ERR_FULL:  return "er staan al 2 forceringen; hef er eerst een op";
    case SIM_ERR_GAP:   return "te snel achter elkaar; wacht een paar seconden";
    case SIM_ERR_BUSY:  return "er loopt al een testbericht";
  }
  return "onbekende fout";
}

/* Bestaat deze sensor, en valt er iets over te zeggen? Een leeg monitorvakje
 * mag niet geforceerd worden: er is geen kanaal, dus er is niets om over te
 * melden en de waarschuwing zou een naam van niets dragen. */
static bool simIdxUsable(const MonitorSensors* self, uint8_t idx) {
  if (idx >= MonitorSensors::SIM_COUNT) return false;
  if (idx < MonitorSensors::SIM_MON_FIRST) return true;   /* voeding en wifi bestaan altijd */
  return self->monitorUsed((int)(idx - MonitorSensors::SIM_MON_FIRST));
}

MonitorSensors::SimMode MonitorSensors::simMode(uint8_t idx) const {
  if (idx >= SIM_COUNT) return SIM_OFF;
  return (SimMode)_sim[idx].mode;
}

uint32_t MonitorSensors::simSecsLeft(uint8_t idx) const {
  if (idx >= SIM_COUNT || _sim[idx].mode == SIM_OFF) return 0;
  long left = (long)(_sim[idx].until - millis());
  return left > 0 ? (uint32_t)(left / 1000) + 1 : 0;
}

uint8_t MonitorSensors::simActiveCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < SIM_COUNT; i++) if (_sim[i].mode != SIM_OFF) n++;
  return n;
}

/* EEN teller voor de hele wachtrij. Zie de uitleg bij alertsActive() in de
 * header: drie losse tellers van twee zouden zes plaatsen vragen waar er vier
 * zijn, en dan valt er stil een ECHTE batterijwaarschuwing weg. */
uint8_t MonitorSensors::alertsActive() const {
  uint8_t n = 0;
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (_mon[i].alerting) n++;
    /* De HERSTELMELDING telt mee, en dat is geen detail: bij een internetstoring
     * komen alle monitors ongeveer gelijktijdig terug. Zonder deze regel zou die
     * golf naast de storingsmeldingen langs de rem glippen en de wachtrij
     * volzetten -- precies waar de rem voor is. */
    if (_mon[i].rec_alerting) n++;
  }
  for (int k = 0; k < FIXED_ALERT_COUNT; k++) {
    if (_fixed_alerting[k]) n++;
    if (_fixed_rec_alerting[k]) n++;
  }
  if (_test_state == TEST_SENDING) n++;
  return n;
}

MonitorSensors::SimResult MonitorSensors::simSet(uint8_t idx, SimMode mode,
                                                 uint16_t secs) {
  if (!simIdxUsable(this, idx)) return SIM_ERR_INDEX;

  unsigned long now = millis();

  /* Opheffen mag altijd en meteen. Een rem op TERUG naar de waarheid zou de
   * verkeerde kant beveiligen: dat is de handeling die je nooit wil vertragen. */
  if (mode == SIM_OFF) {
    if (_sim[idx].mode != SIM_OFF) {
      _sim[idx].mode = SIM_OFF;
      _sim[idx].until = 0;
      _sim_last_change = now;
      MESH_DEBUG_PRINTLN("MonitorSensors: simulatie %d opgeheven", (int)idx);
    }
    return SIM_OK;
  }

  if (secs == 0) secs = SIM_SECS_DEFAULT;
  if (secs < SIM_SECS_MIN || secs > SIM_SECS_MAX) return SIM_ERR_SECS;

  /* De rem tegen flapperen. Alleen op het AANZETTEN en op het OMZETTEN, want
   * elke overgang is een DM-keten naar alle ontvangers. Een lopende forcering
   * VERLENGEN met dezelfde stand is geen overgang en mag dus wel. */
  const bool same = (_sim[idx].mode == (uint8_t)mode);
  if (!same && _sim_last_change != 0
      && (unsigned long)(now - _sim_last_change) < SIM_GAP_MS) {
    return SIM_ERR_GAP;
  }

  /* De rem op het AANTAL. Een vakje dat al geforceerd staat kost geen nieuwe
   * plaats, dus omzetten en verlengen kan altijd. */
  if (_sim[idx].mode == SIM_OFF && simActiveCount() >= MAX_SIM_ACTIVE) {
    return SIM_ERR_FULL;
  }

  _sim[idx].mode  = (uint8_t)mode;
  _sim[idx].until = now + (unsigned long)secs * 1000UL;
  if (!same) _sim_last_change = now;

  MESH_DEBUG_PRINTLN("MonitorSensors: simulatie %d -> %s, %us",
                     (int)idx, mode == SIM_UP ? "op" : "neer", (unsigned)secs);
  return SIM_OK;
}

void MonitorSensors::simClearAll() {
  bool any = false;
  for (uint8_t i = 0; i < SIM_COUNT; i++) {
    if (_sim[i].mode != SIM_OFF) { any = true; }
    _sim[i].mode = SIM_OFF;
    _sim[i].until = 0;
  }
  if (any) {
    _sim_last_change = millis();
    MESH_DEBUG_PRINTLN("MonitorSensors: alle simulaties opgeheven");
  }
}

/* HET AFLOPEN. Dit is de belangrijkste lus van dit onderdeel.
 *
 * Een forcering die blijft staan zet een monitor stil uit, en dat is erger dan
 * geen testknop hebben: de node meldt dan niet meer wat er echt gebeurt en
 * niemand ziet dat. Daarom loopt elke forcering af, en daarom staat er hieronder
 * een regel in de debug-uitvoer -- zodat het aflopen ook terug te vinden is.
 *
 * Het TERUGVALLEN kost hier geen enkele regel, en dat is precies de winst van de
 * opzet: de meting is nooit overschreven, dus mode op SIM_OFF zetten IS het
 * terugvallen op de waarde die er ondertussen gemeten is.
 */
void MonitorSensors::loopSim() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < SIM_COUNT; i++) {
    if (_sim[i].mode == SIM_OFF) continue;
    if ((long)(now - _sim[i].until) < 0) continue;

    _sim[i].mode  = SIM_OFF;
    _sim[i].until = 0;
    MESH_DEBUG_PRINTLN("MonitorSensors: simulatie %d verlopen, terug naar de meting", (int)i);
  }
}

/* ---------------- de waarschuwing van de vaste kanalen ----------------
 *
 * WAAR ZOLANG ER EEN FORCERING OP STAAT. Zie de uitleg in de header: een echte
 * alarmering op netvoeding en wifi is niet gevraagd en er is geen plaats voor in
 * MAX_CONCURRENT_ALERTS. Wie dat wél wil, verandert hieronder de ene regel die
 * op de forcering kijkt in de gemeten toestand -- bijvoorbeeld
 *
 *     const bool want = (which == FIXED_POWER) ? !isMains() : !isWifiOnline();
 *
 * en moet dan MAX_MONITOR_ALERTS verhogen EN nagaan of de batterij nog twee
 * plaatsen overhoudt. Dat laatste is de reden dat het hier niet zo staat.
 */
bool MonitorSensors::fixedAlert(int which) {
  if (which < 0 || which >= FIXED_ALERT_COUNT) return false;

  const uint8_t idx  = (which == FIXED_POWER) ? SIM_POWER : SIM_WIFI;
  const bool    want = (_sim[idx].mode == SIM_DOWN);

  /* Zelfde recept als de monitors: eerste melding, herhalen tot bevestiging, de
   * bovengrens en de rem -- alleen zonder de was_stale/was_sim-vlaggen, want een
   * vast kanaal alarmeert bij deze opzet uitsluitend via een simulatie en dat is
   * altijd een test (fixedAlertText en de herstelvariant zetten SIM_MARK vast). */
  return stepStoringAlert(want, _fixed_alerting[which], _fixed_down_sent[which],
                          _fixed_down_since[which], _fixed_rep[which],
                          which == FIXED_POWER ? 2 : 4, NULL);
}

const char* MonitorSensors::fixedAlertText(int which) const {
  const char* rp = (which >= 0 && which < FIXED_ALERT_COUNT)
                 ? repeatSuffix(_fixed_rep[which].repeats) : "";
  if (which == FIXED_POWER) {
    snprintf(s_alert_buf, sizeof(s_alert_buf),
             "%snetvoeding weg, node op batterij (%.3fV)%s%s",
             SIM_MARK, _last_volts, rp, SIM_TAIL);
  } else if (which == FIXED_WIFI) {
    /* Erbij dat de monitors bevriezen: dat is het GEVOLG dat de ontvanger moet
     * kennen. Zonder onze wifi meten wij niets over de diensten, en dan is het
     * uitblijven van verdere waarschuwingen geen goed nieuws. */
    snprintf(s_alert_buf, sizeof(s_alert_buf),
             "%swifi weg, monitors bevroren%s%s", SIM_MARK, rp, SIM_TAIL);
  } else {
    s_alert_buf[0] = 0;
  }
  return s_alert_buf;
}

/* ---------------- de menselijke bevestiging ---------------- */

bool MonitorSensors::isAckText(const uint8_t* data, size_t len) {
  if (data == NULL) return false;
  /* Spaties vooraan en achteraan wegknippen. De DM-payload is niet noodzakelijk
   * met een nul afgesloten, dus we werken op lengte. */
  size_t a = 0, b = len;
  while (a < b && (data[a] == ' ' || data[a] == '\t' || data[a] == '\r'
                   || data[a] == '\n')) a++;
  while (b > a && (data[b-1] == ' ' || data[b-1] == '\t' || data[b-1] == '\r'
                   || data[b-1] == '\n')) b--;
  const size_t n = b - a;

  /* Hoofdletterongevoelig, exact "ok" of "ack" -- niet "ok, komt goed", want dan
   * zou elke zin die met ok begint stil alle meldingen doven. Kort en streng. */
  if (n == 2) {
    return (data[a]|0x20) == 'o' && (data[a+1]|0x20) == 'k';
  }
  if (n == 3) {
    return (data[a]|0x20) == 'a' && (data[a+1]|0x20) == 'c' && (data[a+2]|0x20) == 'k';
  }
  return false;
}

uint8_t MonitorSensors::confirmAlerts() {
  uint8_t n = 0;

  /* Een melding is "open" als wij hem GEMELD hebben (down_sent) en de storing nog
   * loopt (monitorWantAlert). down_sent alleen zou ook een net herstelde dienst
   * meetellen waarvan het herstelvenster nog open staat, en die valt niets te
   * bevestigen. */
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (!monitorUsed(i)) continue;
    if (monitorWantAlert(i) && _mon[i].down_sent && !_mon[i].rep.human_ack) {
      _mon[i].rep.human_ack = true;
      n++;
    }
  }

  for (int k = 0; k < FIXED_ALERT_COUNT; k++) {
    const uint8_t idx = (k == FIXED_POWER) ? SIM_POWER : SIM_WIFI;
    const bool want = (_sim[idx].mode == SIM_DOWN);
    if (want && _fixed_down_sent[k] && !_fixed_rep[k].human_ack) {
      _fixed_rep[k].human_ack = true;
      n++;
    }
  }

  if (n) MESH_DEBUG_PRINTLN("MonitorSensors: %u melding(en) door een mens bevestigd, herhalen gestopt", (unsigned)n);
  return n;
}

void MonitorSensors::repeatStatus(uint8_t& nagging, uint8_t& capped) const {
  nagging = 0; capped = 0;
  const bool rep_on = (_cfg.repeat_s > 0);
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (!monitorUsed(i)) continue;
    if (!monitorWantAlert(i) || !_mon[i].down_sent || _mon[i].rep.human_ack) continue;
    if (rep_on && _mon[i].rep.repeats >= MAX_ALERT_REPEATS) capped++;
    else nagging++;
  }
  for (int k = 0; k < FIXED_ALERT_COUNT; k++) {
    const uint8_t idx = (k == FIXED_POWER) ? SIM_POWER : SIM_WIFI;
    if (_sim[idx].mode != SIM_DOWN || !_fixed_down_sent[k] || _fixed_rep[k].human_ack) continue;
    if (rep_on && _fixed_rep[k].repeats >= MAX_ALERT_REPEATS) capped++;
    else nagging++;
  }
}

/* ===================== sensorbeheer per DM =====================
 *
 * Zie de header bij SENSORBEHEER PER DM voor het waarom en de syntax. Alles hier
 * routeert door createMonitor()/deleteMonitor()/setSettingValue() -- dezelfde
 * keuring en schrijfweg als de web-GUI en de CLI.
 */

static bool isAllDigits(const char* s) {
  if (s == NULL || *s == 0) return false;
  for (const char* p = s; *p; p++) if (*p < '0' || *p > '9') return false;
  return true;
}

/* Een naam-of-kanaal-argument naar een vakjenummer. Alles wat alleen cijfers is,
 * is een KANAAL (de afspraak uit de header); anders een naam. Exact, nooit een
 * prefix. -1 als er niets op past. */
int MonitorSensors::resolveTarget(const char* tok) const {
  if (tok == NULL || tok[0] == 0) return -1;
  if (isAllDigits(tok)) {
    int ch = atoi(tok);
    if (ch < CH_MONITOR_FIRST || ch > CH_MONITOR_LAST) return -1;
    return findByChannel((uint8_t)ch);
  }
  return findByName(tok);
}

const char* MonitorSensors::handleDmMonCommand(const char* line) {
  static char s_dm_buf[200];
  if (line == NULL) return NULL;

  /* Een muteerbare kopie om op de spaties te splitsen; de DM-tekst is hooguit
   * 160 byte (zie DmCommands), dus dit past ruim. Geen String, geen allocatie. */
  char buf[164];
  StrHelper::strncpy(buf, line, sizeof(buf));

  char* argv[8];
  int argc = 0;
  char* p = buf;
  while (*p && argc < 8) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) break;
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) *p++ = 0;
  }
  if (argc == 0) return NULL;

  /* ---- add <naam> <adres> [interval] ---- */
  /* strcasecmp en niet strcmp: een telefoontoetsenbord maakt van "ping" vanzelf
   * "Ping", en de gebruiker kreeg daardoor "onbekende opdracht" terug op een
   * commando dat bestond. list/get/status in DmCommands waren al
   * hoofdletterongevoelig; deze vier horen dat dus ook te zijn. Alleen het
   * COMMANDOWOORD -- namen en adressen blijven exact, want "UDM-Pro" en
   * "udm-pro" zijn verschillende monitors. */
  if (strcasecmp(argv[0], "add") == 0) {
    if (argc < 3) {
      snprintf(s_dm_buf, sizeof(s_dm_buf), "add <naam> <adres> [interval]");
      return s_dm_buf;
    }
    unsigned long ivl = MON_INTERVAL_DEFAULT;
    if (argc >= 4) {
      /* Geen eigen grenzencontrole: createMonitor keurt het interval zelf. Hier
       * alleen tekst -> getal, en rommel wordt 0 en dus door createMonitor
       * afgekeurd met MON_ERR_INTERVAL. */
      ivl = strtoul(argv[3], NULL, 10);
    }
    uint8_t ch = 0;
    MonResult r = createMonitor(argv[1], argv[2], (uint16_t)ivl, &ch);
    if (r != MON_OK) {
      snprintf(s_dm_buf, sizeof(s_dm_buf), "add geweigerd: %s", monResultText(r));
      return s_dm_buf;
    }
    snprintf(s_dm_buf, sizeof(s_dm_buf), "ok, '%s' op kanaal %u (elke %lus)",
             argv[1], (unsigned)ch, ivl);
    return s_dm_buf;
  }

  /* ---- del <naam|kanaal> ---- */
  if (strcasecmp(argv[0], "del") == 0) {
    if (argc < 2) { snprintf(s_dm_buf, sizeof(s_dm_buf), "del <naam|kanaal>"); return s_dm_buf; }
    int slot = resolveTarget(argv[1]);
    if (slot < 0) {
      snprintf(s_dm_buf, sizeof(s_dm_buf), "del: geen monitor '%s'", argv[1]);
      return s_dm_buf;
    }
    const uint8_t ch = _cfg.mons[slot].channel;
    /* deleteMonitor werkt op naam; wij hebben de naam van het gevonden vakje. Zo
     * loopt del op kanaalnummer door exact dezelfde weg als del op naam. */
    char nm[MON_NAME_LEN];
    StrHelper::strncpy(nm, _cfg.mons[slot].name, sizeof(nm));
    MonResult r = deleteMonitor(nm);
    if (r != MON_OK) {
      snprintf(s_dm_buf, sizeof(s_dm_buf), "del geweigerd: %s", monResultText(r));
      return s_dm_buf;
    }
    /* Eerlijk over de prijs, en geen bevestigingsdialoog: een DM-ronde over LoRa
     * is traag en kost zendtijd. */
    snprintf(s_dm_buf, sizeof(s_dm_buf),
             "verwijderd; kanaal %u blijft vergeven (komt niet terug zolang er "
             "verse nummers zijn)", (unsigned)ch);
    return s_dm_buf;
  }

  /* ---- edit <naam|kanaal> [host=..] [int=..] [naam=..] [ms=0|1] ---- */
  if (strcasecmp(argv[0], "edit") == 0) {
    if (argc < 3) {
      snprintf(s_dm_buf, sizeof(s_dm_buf),
               "edit <naam|kanaal> [host=<adres>] [int=<s>] [naam=<nieuw>] [ms=0|1]");
      return s_dm_buf;
    }
    int slot = resolveTarget(argv[1]);
    if (slot < 0) {
      snprintf(s_dm_buf, sizeof(s_dm_buf), "edit: geen monitor '%s'", argv[1]);
      return s_dm_buf;
    }
    const uint8_t ch = _cfg.mons[slot].channel;

    int done = 0, failed = 0;
    bool host_changed = false;
    char last_bad[16]; last_bad[0] = 0;

    for (int i = 2; i < argc; i++) {
      char* eq = strchr(argv[i], '=');
      if (eq == NULL) { failed++; StrHelper::strncpy(last_bad, argv[i], sizeof(last_bad)); continue; }
      *eq = 0;
      const char* key = argv[i];
      const char* val = eq + 1;

      /* De sleutelnaam naar de settings-zeef. "naam" (Nederlands, in de DM) wordt
       * "name" (de interne sleutel); de andere vallen samen. */
      const char* field = NULL;
      if      (strcmp(key, "host") == 0) field = "host";
      else if (strcmp(key, "int")  == 0) field = "int";
      else if (strcmp(key, "naam") == 0) field = "name";
      else if (strcmp(key, "ms")   == 0) field = "ms";
      if (field == NULL) { failed++; StrHelper::strncpy(last_bad, key, sizeof(last_bad)); continue; }

      char setting[24];
      snprintf(setting, sizeof(setting), "mon.%u.%s", (unsigned)ch, field);
      if (setSettingValue(setting, val)) {
        done++;
        if (strcmp(field, "host") == 0) host_changed = true;
      } else {
        failed++;
        StrHelper::strncpy(last_bad, key, sizeof(last_bad));
      }
    }

    /* De adreswaarschuwing kort, net als de web-GUI: hetzelfde kanaalnummer wijst
     * daarna naar een andere dienst. */
    int n = snprintf(s_dm_buf, sizeof(s_dm_buf),
                     "kanaal %u: %d gewijzigd%s", (unsigned)ch, done,
                     failed ? "" : "");
    if (failed) n += snprintf(s_dm_buf + n, sizeof(s_dm_buf) - n,
                              ", %d geweigerd (%s?)", failed, last_bad);
    if (host_changed) snprintf(s_dm_buf + n, sizeof(s_dm_buf) - n,
                               ". LET OP: kanaal %u wijst nu een andere dienst aan; "
                               "pas de naam en je dashboard aan", (unsigned)ch);
    return s_dm_buf;
  }

  /* ---- ping <adres> [n] ---- (ad-hoc; uitslag komt LATER, apart)
   *
   * Deze regel geeft alleen de ONMIDDELLIJKE reactie terug ("gestart" / "bezig"
   * / een weigering). De echte uitslag komt via adhocReady()/adhocResultText(),
   * die DmCommands na een paar seconden ophaalt en als eigen DM stuurt. Zo blokt
   * niets: het commando keurt en start, meer niet. */
  if (strcasecmp(argv[0], "ping") == 0) {
    if (argc < 2) { snprintf(s_dm_buf, sizeof(s_dm_buf), "ping <adres> [n]"); return s_dm_buf; }
    /* Zonder wifi meteen een eerlijk antwoord en niets starten -- pingen zou
     * onze eigen verbinding meten, niet het adres. */
    if (!wifiReallyOnline()) {
      snprintf(s_dm_buf, sizeof(s_dm_buf), "geen wifi, niet gepingd");
      return s_dm_buf;
    }
    unsigned long n = ADHOC_DEFAULT_PINGS;
    if (argc >= 3) n = strtoul(argv[2], NULL, 10);
    SimResult r = startAdhocPing(argv[1], (uint8_t)n);
    if (r == SIM_ERR_BUSY) {
      snprintf(s_dm_buf, sizeof(s_dm_buf), "bezig met %s, probeer zo opnieuw", adhocHost());
      return s_dm_buf;
    }
    if (r != SIM_OK) {
      snprintf(s_dm_buf, sizeof(s_dm_buf),
               "adres: 1-40 tekens uit a-z A-Z 0-9 . - _ (geen IPv6)");
      return s_dm_buf;
    }
    uint8_t got = (n == 0 || n > ADHOC_MAX_PINGS) ? (n == 0 ? ADHOC_DEFAULT_PINGS : ADHOC_MAX_PINGS) : (uint8_t)n;
    snprintf(s_dm_buf, sizeof(s_dm_buf),
             "ping naar %s gestart (%ux); de uitslag volgt zo als aparte DM%s",
             argv[1], (unsigned)got, _adhoc.delayed ? " (even wachten, pingmachine was bezig)" : "");
    return s_dm_buf;
  }

  return NULL;   /* niet van ons: DmCommands valt door naar list/get/status/help */
}

const char* MonitorSensors::dmCommandHelp() {
  /* Eén regel per commando, kort want het gaat over LoRa. De admin-eis staat
   * erbij zodat een leescontact begrijpt waarom er niets gebeurt. */
  return "add <naam> <adres> [int] | edit <naam|kanaal> [host= int= naam= ms=] | "
         "del <naam|kanaal> | ping <adres> [n] -- alleen voor admins; "
         "naam-of-kanaal: alleen cijfers = kanaal";
}

/* ===================== ad-hoc ping =====================
 *
 * Zie de header bij AD-HOC PING. Deelt de ene esp_ping-sessie en de fasemachine
 * met de monitorbewaking; elke tak hierboven in loopMonitors is met
 * (_busy_slot == ADHOC_SLOT) bewaakt.
 */
MonitorSensors::SimResult MonitorSensors::startAdhocPing(const char* host, uint8_t n) {
  if (!validHost(host)) return SIM_ERR_INDEX;
  /* PUSH_HOST ("-") is een geldig adres voor de zeef maar niets om te pingen. */
  if (strcmp(host, PUSH_HOST) == 0) return SIM_ERR_INDEX;

  if (_adhoc.state == ADHOC_PENDING || _adhoc.state == ADHOC_BUSY) return SIM_ERR_BUSY;

  if (n == 0) n = ADHOC_DEFAULT_PINGS;
  if (n > ADHOC_MAX_PINGS) n = ADHOC_MAX_PINGS;

  memset(&_adhoc, 0, sizeof(_adhoc));
  StrHelper::strncpy(_adhoc.host, host, sizeof(_adhoc.host));
  _adhoc.want   = n;
  _adhoc.min_ms = 0xFFFFFFFFUL;
  _adhoc.state  = ADHOC_PENDING;

  /* VOORRANG: draait er nu een monitorping, dan afbreken (telt niet als
   * mislukking) zodat de ad-hoc als eerste gaat. _next_ping_at op nu, zodat de
   * IDLE-tak van loopMonitors de ad-hoc meteen oppakt en niet eerst PING_GAP_MS
   * wacht. */
  _adhoc.delayed = (_phase != PING_IDLE);
  if (_phase != PING_IDLE) abortPing();
  _next_ping_at = millis();
  return SIM_OK;
}

void MonitorSensors::startAdhocResolve() {
  _adhoc.state = ADHOC_BUSY;
  StrHelper::strncpy(s_ping.host, _adhoc.host, sizeof(s_ping.host));

  /* Al een IP? Dan geen DNS. Zelfde snelweg als startResolve(). */
  ip4_addr_t lit;
  if (ip4addr_aton(s_ping.host, &lit)) {
    _adhoc.addr_v4 = ip4_addr_get_u32(&lit);
    startAdhocOnePing();
    return;
  }

  s_ping.dns_state = 0;
  s_ping.dns_addr  = 0;
  _busy_slot = ADHOC_SLOT;
  _phase = PING_RESOLVING;
  _phase_deadline = millis() + DNS_DEADLINE_MS;

  if (tcpip_try_callback(mon_do_resolve, this) != ERR_OK) {
    /* lwIP-rij vol: even wachten en opnieuw, net als bij een monitor. */
    _busy_slot = -1;
    _phase = PING_IDLE;
    _adhoc.state = ADHOC_PENDING;
    _next_ping_at = millis() + PING_GAP_MS;
  }
}

void MonitorSensors::startAdhocOnePing() {
  esp_ping_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.count           = 1;
  cfg.interval_ms     = 1000;
  cfg.timeout_ms      = ADHOC_TIMEOUT_MS;   /* ruimer dan de monitor: een mens
                                               wil weten of het TRAAG is, niet
                                               alleen of het er is */
  cfg.data_size       = 32;
  cfg.tos             = 0;
  cfg.ttl             = 64;
  cfg.task_stack_size = 3072;
  cfg.task_prio       = 2;
  cfg.interface       = 0;
  ip_addr_set_ip4_u32(&cfg.target_addr, _adhoc.addr_v4);

  esp_ping_callbacks_t cbs;
  memset(&cbs, 0, sizeof(cbs));
  cbs.cb_args         = NULL;
  cbs.on_ping_success = mon_ping_success;
  cbs.on_ping_timeout = mon_ping_timeout;
  cbs.on_ping_end     = NULL;

  s_ping.state = 0;
  s_ping.ms    = 0;

  if (s_ping.handle != NULL) {
    esp_ping_delete_session(s_ping.handle);
    s_ping.handle = NULL;
  }

  if (esp_ping_new_session(&cfg, &cbs, &s_ping.handle) != ESP_OK) {
    s_ping.handle = NULL;
    finishAdhoc("kon geen ping-sessie maken");
    return;
  }
  if (esp_ping_start(s_ping.handle) != ESP_OK) {
    esp_ping_delete_session(s_ping.handle);
    s_ping.handle = NULL;
    finishAdhoc("kon de ping niet starten");
    return;
  }

  _adhoc.state = ADHOC_BUSY;
  _busy_slot = ADHOC_SLOT;
  _phase = PING_RUNNING;
  _phase_deadline = millis() + PING_DEADLINE_MS;   /* noodrem boven de timeout */
}

void MonitorSensors::recordAdhocResult(bool ok, uint32_t ms) {
  if (_adhoc.done < ADHOC_MAX_PINGS) {
    _adhoc.results[_adhoc.done] = ok ? ms : 0xFFFFFFFFUL;
  }
  _adhoc.done++;
  if (ok) {
    _adhoc.ok++;
    _adhoc.sum_ms += ms;
    if (ms < _adhoc.min_ms) _adhoc.min_ms = ms;
    if (ms > _adhoc.max_ms) _adhoc.max_ms = ms;
  }
}

void MonitorSensors::finishAdhoc(const char* note) {
  _adhoc.note        = note;
  _adhoc.finished_at = millis();
  _adhoc.state       = ADHOC_DONE;
  /* De sessie kan nog open staan als we vroegtijdig eindigen (DNS-fout); opruimen
   * zodat de monitorbewaking een schone machine terugkrijgt. */
  if (s_ping.handle != NULL) {
    esp_ping_stop(s_ping.handle);
    esp_ping_delete_session(s_ping.handle);
    s_ping.handle = NULL;
  }
  _busy_slot = -1;
  _phase = PING_IDLE;
  _next_ping_at = millis() + PING_GAP_MS;   /* monitors weer aan de beurt */
  MESH_DEBUG_PRINTLN("MonitorSensors: ad-hoc ping naar %s klaar (%u/%u ok)",
                     _adhoc.host, (unsigned)_adhoc.ok, (unsigned)_adhoc.done);
}

const char* MonitorSensors::adhocResultText() const {
  static char buf[220];
  int n = snprintf(buf, sizeof(buf), "ping %s:", _adhoc.host);

  if (_adhoc.note) {
    n += snprintf(buf + n, sizeof(buf) - n, " %s", _adhoc.note);
  }
  for (uint8_t i = 0; i < _adhoc.done && i < ADHOC_MAX_PINGS; i++) {
    if (_adhoc.results[i] == 0xFFFFFFFFUL) {
      n += snprintf(buf + n, sizeof(buf) - n, " timeout");
    } else {
      n += snprintf(buf + n, sizeof(buf) - n, " %lums", (unsigned long)_adhoc.results[i]);
    }
  }
  if (_adhoc.done > 0) {
    n += snprintf(buf + n, sizeof(buf) - n, " | %u/%u ok",
                  (unsigned)_adhoc.ok, (unsigned)_adhoc.done);
    if (_adhoc.ok > 0) {
      uint32_t avg = _adhoc.sum_ms / _adhoc.ok;
      n += snprintf(buf + n, sizeof(buf) - n, ", min/gem/max %lu/%lu/%lu ms",
                    (unsigned long)_adhoc.min_ms, (unsigned long)avg,
                    (unsigned long)_adhoc.max_ms);
    }
  }
  /* De ouderdom van de uitslag, zoals de alerts hun duur meegeven: als de
   * bezorging traag was, hoort de vrager te zien hoe oud het antwoord is. */
  const unsigned long age = _adhoc.finished_at ? (millis() - _adhoc.finished_at) / 1000 : 0;
  snprintf(buf + n, sizeof(buf) - n, " (%lus geleden%s)",
           age, _adhoc.delayed ? ", start was uitgesteld" : "");
  return buf;
}

void MonitorSensors::adhocClear() {
  /* Alleen de vlag: de velden worden bij de volgende startAdhocPing gewist. Zo
   * blijft de laatste uitslag leesbaar tot hij echt overschreven wordt. DONE ->
   * NONE betekent bovendien dat een nieuwe ping niet meer op BUSY afketst. */
  _adhoc.state = ADHOC_NONE;
}

/* ---------------- het testbericht ----------------
 *
 * Het bericht zelf. Kort, want het hoeft maar één ding te doen: aankomen en
 * herkenbaar zijn. Het loopnummer staat erin zodat twee tests naast elkaar te
 * leggen zijn -- zonder dat nummer is "ik kreeg een testbericht" niet te
 * koppelen aan "ik heb er om 14:03 een gestuurd".
 */
const char* MonitorSensors::testAlertText() const {
  snprintf(s_alert_buf, sizeof(s_alert_buf),
           "%sTESTBERICHT #%u van MeshUptime -- alleen om te zien of "
           "waarschuwingen aankomen; er is niets stuk",
           SIM_MARK, (unsigned)_test_seq);
  return s_alert_buf;
}

MonitorSensors::SimResult MonitorSensors::testRequest(uint8_t recipients) {
  unsigned long now = millis();

  /* Loopt er nog een? Dan niet nog een keer. Twee testberichten door elkaar
   * maken de ACK-telling onleesbaar -- en dat getal is de hele opbrengst. */
  if (_test_state == TEST_PENDING || _test_state == TEST_SENDING) return SIM_ERR_BUSY;

  /* De rem: hoogstens één per minuut. Zendtijd is gedeeld, en een knop die je
   * vijf keer achter elkaar kunt indrukken is vijf DM-ketens naar iedereen. */
  if (_test_asked_at != 0 && (unsigned long)(now - _test_asked_at) < TEST_GAP_MS) {
    return SIM_ERR_GAP;
  }

  /* Plaats in de wachtrij van alertIf()? Zo niet, dan hier weigeren met de
   * reden, en niet stil aannemen: een aanvraag die nooit verstuurd wordt is
   * precies het soort stilte dat dit hele onderdeel moet wegnemen. */
  if (alertsActive() >= MAX_MONITOR_ALERTS) return SIM_ERR_FULL;

  _test_state      = TEST_PENDING;
  _test_recipients = recipients;
  _test_acks       = 0;
  _test_last_ack   = 0;
  _test_asked_at   = now;
  _test_sent_at    = 0;
  _test_hold_until = 0;
  _test_seq++;

  MESH_DEBUG_PRINTLN("MonitorSensors: testbericht #%u aangevraagd, %u ontvanger(s)",
                     (unsigned)_test_seq, (unsigned)recipients);
  return SIM_OK;
}

/* De voorwaarde voor alertIf(). Waar vanaf het moment dat main.cpp hem in een
 * leesronde oppikt, tot het rondje langs de contacten klaar hoort te zijn.
 *
 * WAAROM ER EEN VENSTER IS EN NIET EEN ENKELE 'true': alertIf() vuurt op de
 * OVERGANG en heeft daarna tijd nodig -- 8 s per ontvanger die het alarmrecht
 * heeft. Zou de voorwaarde meteen weer false worden, dan haalt alertIf() de
 * Trigger halverwege uit de wachtrij en krijgt alleen de eerste ontvanger iets.
 * Zou hij voor altijd waar blijven, dan blijft er een plaats bezet die een echte
 * waarschuwing nodig kan hebben. */
bool MonitorSensors::testAlert() {
  if (_test_state == TEST_PENDING) {
    /* Plaats vrij? Dan is dit de leesronde waarin hij de deur uit gaat. */
    if (alertsActive() >= MAX_MONITOR_ALERTS) return false;

    unsigned long hold = TEST_HOLD_BASE_MS
                       + (unsigned long)_test_recipients * TEST_HOLD_PER_MS;
    if (hold > TEST_HOLD_MAX_MS) hold = TEST_HOLD_MAX_MS;

    _test_state      = TEST_SENDING;
    _test_sent_at    = millis();
    _test_hold_until = _test_sent_at + hold;
    MESH_DEBUG_PRINTLN("MonitorSensors: testbericht #%u gaat de deur uit",
                       (unsigned)_test_seq);
    return true;
  }
  return _test_state == TEST_SENDING;
}

/* Een ACK. main.cpp ziet ze langskomen en geeft ons de expected_acks[] van de
 * Trigger mee; de vergelijking en de telling staan hier.
 *
 * ALLEEN TELLEN ZOLANG DE TEST LOOPT, en niet dezelfde ACK twee keer: een ACK
 * kan over meerdere paden aankomen, en dan zou "3 van 2 bevestigd" op de pagina
 * staan. Een getal dat hoger is dan het maximum maakt het hele overzicht
 * verdacht, ook de delen die wel kloppen.
 *
 * Wat deze telling NIET kan zien: SensorMesh hergebruikt expected_acks[3] voor
 * elke volgende ontvanger, dus een ACK die pas aankomt nadat er al naar de
 * volgende ontvanger gestuurd is, is niet meer te herkennen. De pagina zegt
 * daarom "minstens zoveel bevestigd" en niet "precies zoveel". */
bool MonitorSensors::noteAlertAck(uint32_t ack_crc, const uint32_t* expected,
                                 uint8_t n) {
  if (_test_state != TEST_SENDING || expected == NULL) return false;
  if (ack_crc == 0) return false;
  if (ack_crc == _test_last_ack) return true;   /* al geteld */

  if (n > 4) n = 4;
  for (uint8_t i = 0; i < n; i++) {
    if (expected[i] != ack_crc) continue;
    _test_last_ack = ack_crc;
    if (_test_acks < 255) _test_acks++;
    MESH_DEBUG_PRINTLN("MonitorSensors: testbericht #%u bevestigd (%u van %u)",
                       (unsigned)_test_seq, (unsigned)_test_acks,
                       (unsigned)_test_recipients);
    return true;
  }
  return false;
}

void MonitorSensors::loopTest() {
  if (_test_state == TEST_PENDING) {
    /* Nooit opgepakt (geen plaats vrijgekomen)? Dan vervalt de aanvraag met een
     * reden in de uitvoer. Een aanvraag die eeuwig blijft wachten is een knop
     * die niets deed. */
    if ((unsigned long)(millis() - _test_asked_at) >= TEST_PENDING_MAX_MS) {
      _test_state = TEST_DONE;
      MESH_DEBUG_PRINTLN("MonitorSensors: testbericht #%u vervallen, nooit een vrije plaats",
                         (unsigned)_test_seq);
    }
    return;
  }

  if (_test_state == TEST_SENDING && (long)(millis() - _test_hold_until) >= 0) {
    _test_state = TEST_DONE;
    MESH_DEBUG_PRINTLN("MonitorSensors: testbericht #%u klaar, %u van %u bevestigd",
                       (unsigned)_test_seq, (unsigned)_test_acks,
                       (unsigned)_test_recipients);
  }
}

MonitorSensors::TestState MonitorSensors::testState() const {
  return (TestState)_test_state;
}

uint32_t MonitorSensors::testAgeSecs() const {
  if (_test_asked_at == 0) return 0;
  return (uint32_t)((millis() - _test_asked_at) / 1000);
}

uint32_t MonitorSensors::testWaitSecs() const {
  if (_test_asked_at == 0) return 0;
  long left = (long)(_test_asked_at + TEST_GAP_MS - millis());
  return left > 0 ? (uint32_t)(left / 1000) + 1 : 0;
}

/* ===================== telemetrie ===================== */

bool MonitorSensors::querySensors(uint8_t requester_permissions, CayenneLPP& telemetry) {
  /* EERST de basisklasse: batterij (kanaal 1, door SensorMesh), GPS en de
   * omgevingssensoren blijven werken zoals upstream. */
  EnvironmentSensorManager::querySensors(requester_permissions, telemetry);

  /* HET VASTE DEEL METEN, hier en niet schatten.
   *
   * Op dit punt staat er in het pakket wat SensorMesh en de basisklasse erin
   * gezet hebben: de batterijspanning (4 byte), de GPS als die aanstaat (11 byte)
   * en eventuele omgevingssensoren. Dat getal is het enige eerlijke "vast
   * gebruik" voor het budget op de pagina -- een vast getal in de code zou stil
   * verkeerd worden op de dag dat iemand GPS aanzet, en dan belooft de pagina
   * ruimte die er niet is.
   *
   * Dit is bovendien de enige plek waar het te meten valt: querySensors() loopt
   * in BEIDE paden (de periodieke ronde en het antwoord op een verzoek), en de
   * basisklasse vertelt niet hoeveel zij gaat gebruiken. */
  {
    const int base = (int)telemetry.getSize();
    if (base >= 0 && base <= TELEM_BUDGET) {
      _telem_base = (uint8_t)base;
      _telem_measured = true;
    }
  }

  /* Voeding en wifi vallen onder de basispermissie: dit is de toestand van de
   * node zelf, niet die van een omgevingssensor. Een vraagsteller die bit 0
   * wegmaskeert krijgt ze dus niet -- dat is de bedoeling van het masker.
   *
   * Kanaal 3 is de spiegel van kanaal 2 en dus strikt gezien overtollig. Het
   * staat er toch, omdat "netvoeding" en "batterijvoeding" in de bewaking twee
   * aparte sensoren zijn die elk hun eigen waarschuwing kunnen krijgen, en
   * omdat een LPP_SWITCH 3 byte kost. */
  if (requester_permissions & TELEM_PERM_BASE) {
    /* Het masker van afgekapte vakjes hoort bij DEZE uitlezing en niet bij de
     * vorige: wie een pingtijd uitzet en daarmee ruimte maakt, moet de melding
     * zien verdwijnen. */
    _telem_dropped = 0;

    /* isMains() en niet _mains: een forcering hoort OOK in de telemetrie te
     * staan. Een dashboard aan de andere kant moet hetzelfde zien als deze node,
     * anders is de simulatie een test van de pagina in plaats van van het
     * systeem -- en dan blijft juist de vraag onbeantwoord of dat dashboard er
     * iets van meekrijgt. */
    const bool mains_now = isMains();
    telemetry.addSwitch(CH_MAINS,   mains_now ? 1 : 0);
    telemetry.addSwitch(CH_BATTERY, mains_now ? 0 : 1);
    /* Zonder aangehangen WifiTask is er geen wifi, en dan is "niet online" het
     * eerlijke antwoord. Het kanaal blijft altijd aanwezig, zodat een
     * dashboard geen veld ziet komen en gaan. */
    telemetry.addSwitch(CH_WIFI,    isWifiOnline() ? 1 : 0);

    /* De monitors. Op vakjesvolgorde en dus op de volgorde waarin ze zijn
     * aangemaakt; de kanaalnummers zijn wat telt en die staan vast.
     *
     * Hier staat GEEN onderscheid tussen zelf gepingd en van buiten gemeld, en
     * dat is de bedoeling: beide soorten leveren een schakelaar en, als ze op
     * staan, een tijd in milliseconden. Een dashboard hoort niet te hoeven weten
     * hoe wij aan onze uitslag komen. De verouderingsregel voor gemelde diensten
     * werkt daarom via seeded (zie loopPushStale) -- dan valt zo'n vakje langs
     * exact hetzelfde pad terug op "schakelaar 0, geen tijd" als een
     * ping-monitor waarvan we nog niets weten.
     *
     * De pingtijd gaat alleen mee als de monitor up is. Een tijd bij een dode
     * dienst is geen meting maar een oude waarde, en wie hem toch zou tekenen
     * krijgt een grafiek die tijdens een storing gewoon doorloopt.
     *
     * De schakelaar gaat WEL altijd mee, ook als er nog nooit een uitslag was:
     * LPP_SWITCH kent geen "onbekend", en een kanaal dat komt en gaat is voor
     * een dashboard erger dan een kanaal dat even 0 staat. Kanaal 4 vertelt
     * bovendien of we op dat moment konden meten.
     *
     * Het budget wordt nagerekend en niet aangenomen: de basisklasse kan er
     * naar believen GPS en omgevingssensoren voor gezet hebben. Bij overloop
     * kappen we af -- CayenneLPP zou zelf ook weigeren (LPP_ERROR_OVERFLOW),
     * maar dan zou een monitor half in het pakket staan: schakelaar erin,
     * pingtijd eruit. Zo blijft het per monitor alles of niets. */
    for (int i = 0; i < MAX_MONITORS; i++) {
      if (!monitorUsed(i)) continue;

      /* monitorIsUp()/monitorSeeded() en niet _mon[i] rechtstreeks: dat is het
       * ENIGE punt waar de forcering in de telemetrie komt, en het moet er zijn.
       * Een simulatie die de pagina wel en het mesh niet bereikt, test de helft
       * van de keten -- en juist de andere helft is waar het misgaat. */
      const bool up = monitorSeeded(i) && monitorIsUp(i);
      /* De pingtijd gaat alleen mee als de monitor OP is en als deze monitor hem
       * mag meesturen (send_ms). Dat tweede is de knop waarmee 9 byte 3 byte
       * wordt, en daarmee het verschil tussen zeventien en ruim vijftig monitors
       * in hetzelfde pakket. */
      const bool with_ms = up && _cfg.mons[i].send_ms;
      const uint8_t need = TELEM_BYTES_SWITCH + (with_ms ? TELEM_BYTES_GENERIC : 0);

      if ((int)telemetry.getSize() + need > TELEM_BUDGET) {
        /* AFKAPPEN MAG, STIL AFKAPPEN NIET.
         *
         * Per monitor alles of niets -- een halve monitor (schakelaar erin,
         * pingtijd eruit) zou een dashboard een tijd van nul laten tekenen. En
         * geen 'break': de rest van de lus wordt doorlopen om te MARKEREN welke
         * vakjes buiten het pakket vielen, zodat /status.json en de pagina kunnen
         * zeggen WELKE dat waren. Een monitor die stil uit de telemetrie
         * verdwijnt is de fout die dit project al twee keer gekost heeft.
         *
         * Doorlopen kan bovendien nog iets opleveren: een monitor die verderop
         * staat en zijn pingtijd niet meestuurt, past soms nog wel in de 3 byte
         * die er over zijn. Dat is geen truc maar de bedoeling van het budget. */
        _telem_dropped |= ((uint32_t)1 << i);
        MESH_DEBUG_PRINTLN("MonitorSensors: kanaal %d past niet in de telemetrie (%d/%d byte gebruikt)",
                           (int)_cfg.mons[i].channel, (int)telemetry.getSize(),
                           (int)TELEM_BUDGET);
        continue;
      }

      telemetry.addSwitch(_cfg.mons[i].channel, up ? 1 : 0);
      if (with_ms) {
        /* multiplier van LPP_GENERIC_SENSOR is 1, dus dit zijn exacte hele
         * milliseconden en geen afgeronde schaalwaarde. */
        telemetry.addGenericSensor(_cfg.mons[i].channel, (float)_mon[i].last_ms);
      }
    }
  }

  return true;
}

/* ===================== kanalen uitdelen =====================
 *
 * DIT IS DE BELANGRIJKSTE REGEL VAN DIT BESTAND.
 *
 * Een monitor krijgt bij het aanmaken een kanaal uit 5..12 en houdt dat voor
 * altijd. Wordt hij verwijderd, dan wordt zijn nummer NIET opnieuw uitgedeeld
 * zolang er nog een nummer is dat nog nooit gebruikt is.
 *
 * Waarom zo streng: een node die telemetrie opvraagt bewaart de koppeling
 * "kanaal 6 = google" aan zijn kant. CayenneLPP stuurt geen namen mee, alleen
 * nummers. Deelden we kanaal 6 opnieuw uit aan een nieuwe monitor, dan blijft
 * die bewaarde koppeling gewoon werken -- en wijst STIL naar de verkeerde
 * dienst. Geen foutmelding, geen leeg veld, alleen verkeerde cijfers onder een
 * vertrouwde naam. Dat is de ergste soort fout die dit apparaat kan maken,
 * want de bewaking blijft er gezond uitzien.
 *
 * ER IS PRECIES EEN TOEWIJZER, VOOR BEIDE SOORTEN MONITOR.
 *
 * Een van buiten gemelde dienst (/hook) krijgt zijn kanaal hier, uit dezelfde
 * pot en met dezelfde ch_ever_used-byte als een ping-monitor. Een tweede
 * toewijzer ernaast zou na één verwijdering twee waarheden geven die uit elkaar
 * lopen -- de een weet dat kanaal 7 vergeven is, de ander deelt hem opnieuw uit.
 * Het onderscheid tussen de twee soorten zit in het ADRESVELD (zie PUSH_HOST) en
 * nergens in de kanaalboekhouding, want voor een kanaalnummer maakt het niet uit
 * waar de uitslag vandaan komt.
 *
 * Om diezelfde reden staat ch_ever_used in het opslagbestand: zonder die byte
 * zou de node na een herstart weer bij 5 beginnen uitdelen en was de hele
 * afspraak een afspraak voor de duur van één stroomvoorziening.
 *
 * Zijn alle nummers een keer gebruikt en is er niets meer nieuw uit te
 * delen, dan pas wordt een vrijgekomen nummer hergebruikt -- de laagste. Op dat
 * moment is hergebruik onvermijdelijk; wie zijn monitors ruim dertig keer opnieuw
 * indeelt, moet zijn dashboards nalopen. Dat staat in de melding hieronder.
 */
uint8_t MonitorSensors::allocChannel() {
  /* uint32_t EN NIET uint8_t, en dat is de reden dat deze functie bij het
   * verhogen van MAX_MONITORS moest worden nagelopen: met 32 kanalen past het
   * masker niet in een byte. Een 1 << 31 op een uint8_t compileert zonder
   * klacht en levert stil nul op -- en dan wordt kanaal 36 opnieuw uitgedeeld
   * alsof hij nooit vergeven was. Dat is precies de stille fout waar deze hele
   * toewijzer tegen bedoeld is. De static_assert hieronder houdt het vast. */
  static_assert(MAX_MONITORS <= 32,
                "ch_ever_used is een uint32_t; meer dan 32 kanalen past niet in dat masker");

  uint32_t in_use = 0;
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (_cfg.mons[i].channel != 0) {
      in_use |= ((uint32_t)1 << (_cfg.mons[i].channel - CH_MONITOR_FIRST));
    }
  }

  /* 1. een nummer dat nog nooit is uitgedeeld */
  for (uint8_t b = 0; b < MAX_MONITORS; b++) {
    if (!(_cfg.ch_ever_used & ((uint32_t)1 << b))) {
      _cfg.ch_ever_used |= ((uint32_t)1 << b);
      return (uint8_t)(CH_MONITOR_FIRST + b);
    }
  }

  /* 2. alles is een keer gebruikt: nu pas hergebruiken */
  for (uint8_t b = 0; b < MAX_MONITORS; b++) {
    if (!(in_use & ((uint32_t)1 << b))) {
      MESH_DEBUG_PRINTLN("MonitorSensors: kanaal %d wordt HERGEBRUIKT; dashboards die dit kanaal bewaard hebben, tonen nu een andere dienst", (int)(CH_MONITOR_FIRST + b));
      return (uint8_t)(CH_MONITOR_FIRST + b);
    }
  }

  return 0;   /* alle vakjes bezet */
}

/* ===================== monitors beheren ===================== */

bool MonitorSensors::validName(const char* s) {
  if (s == NULL) return false;
  size_t n = strlen(s);
  if (n == 0 || n > MON_NAME_LEN - 1) return false;

  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

/* Adres: een IPv4-adres of een hostnaam. Dezelfde tekenzeef als bij de naam --
 * die dekt letters, cijfers, punt, streepje en liggend streepje, en dat is
 * precies wat een hostnaam mag bevatten. Een dubbele punt komt er niet door en
 * dat is bedoeld: IPv6 wordt niet ondersteund (zie mon_dns_found). */
bool MonitorSensors::validHost(const char* s) {
  if (s == NULL) return false;
  size_t n = strlen(s);
  if (n == 0 || n > MON_HOST_LEN - 1) return false;

  for (size_t i = 0; i < n; i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

void MonitorSensors::markDirty() {
  _dirty = true;
  /* Twee seconden uitstel: genoeg om een reeks "sensor set"-opdrachten samen te
   * voegen tot één schrijfronde, kort genoeg om niet weg te zijn als iemand
   * meteen daarna de stekker eruit trekt. */
  _save_at = millis() + 2000;
}

/* Aanmaken met losse velden. Dit is de ENIGE plek waar een monitor ontstaat: de
 * CLI (addMonitor), de webinterface en /hook (reportMonitor) komen hier alle
 * drie langs. Zo staan de keuring, de kanaaltoewijzing en het opschonen van de
 * meettoestand één keer beschreven.
 *
 * Een adres gelijk aan PUSH_HOST maakt een GEMELDE dienst; dat werkt hier
 * verder zonder aparte tak, want het enige verschil zit in wat er daarna met het
 * vakje gebeurt (niet pingen, wel verouderen) en dat leest de rest van deze
 * klasse uit monitorIsPush(). */
MonitorSensors::MonResult MonitorSensors::createMonitor(const char* name, const char* host,
                                                       uint16_t interval_s, uint8_t* out_channel) {
  if (out_channel != NULL) *out_channel = 0;

  if (!validName(name)) return MON_ERR_NAME;
  if (!validHost(host)) return MON_ERR_HOST;
  if (interval_s < MON_INTERVAL_MIN || interval_s > MON_INTERVAL_MAX) return MON_ERR_INTERVAL;

  if (findByName(name) >= 0) return MON_ERR_TAKEN;

  int slot = -1;
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (_cfg.mons[i].channel == 0) { slot = i; break; }
  }
  if (slot < 0) return MON_ERR_FULL;           /* alle vakjes bezet */

  /* DE ECHTE GRENS: PAST HIJ NOG IN HET PAKKET?
   *
   * Deze keuring staat HIER en niet alleen in de browser, en dat is het verschil
   * tussen gemak en een slot. De pagina rekent hetzelfde uit om het meteen te
   * kunnen zeggen, maar /hook, de CLI en een script komen daar niet langs. Zonder
   * deze regel zou een achttiende monitor stil worden aangemaakt en dan bij elke
   * uitlezing buiten het pakket vallen -- geen foutmelding, maar wel verkeerde
   * gegevens op een dashboard.
   *
   * Er wordt gerekend met de DUURSTE stand (9 byte, mét pingtijd), want zo wordt
   * een nieuwe monitor aangemaakt. Wie meer monitors wil, zet bij een paar de
   * pingtijd uit; dat staat in de foutmelding en op de pagina. */
  if (!telemFits(TELEM_BYTES_SWITCH_PUB + TELEM_BYTES_GENERIC_PUB)) {
    return MON_ERR_BYTES;
  }

  uint8_t ch = allocChannel();
  if (ch == 0) return MON_ERR_FULL;

  MonitorCfgEntry& e = _cfg.mons[slot];
  StrHelper::strncpy(e.name, name, sizeof(e.name));
  StrHelper::strncpy(e.host, host, sizeof(e.host));
  e.interval_s = interval_s;
  e.channel    = ch;
  /* Pingtijd standaard AAN: dat is wat iemand verwacht die een monitor aanmaakt,
   * en het is wat elke eerdere versie deed. */
  e.send_ms    = 1;

  memset(&_mon[slot], 0, sizeof(_mon[slot]));
  _mon[slot].up = true;               /* nog niet 'seeded'; zie applyResult() */
  _mon[slot].next_check = millis();   /* meteen aan de beurt (gemeld: ongebruikt) */

  markDirty();
  MESH_DEBUG_PRINTLN("MonitorSensors: %s '%s' -> %s op kanaal %d, elke %d s",
                     monitorIsPush(slot) ? "gemelde dienst" : "monitor",
                     e.name, e.host, (int)ch, (int)interval_s);

  if (out_channel != NULL) *out_channel = ch;
  return MON_OK;
}

MonitorSensors::MonResult MonitorSensors::deleteMonitor(const char* name) {
  int slot = findByName(name);
  if (slot < 0) return MON_ERR_UNKNOWN;

  /* Wordt er juist naar dit vakje gepingd? Dan die meting weggooien: de uitslag
   * hoort bij een monitor die niet meer bestaat. */
  if (_busy_slot == slot) abortPing();

  MESH_DEBUG_PRINTLN("MonitorSensors: monitor '%s' verwijderd; kanaal %d komt niet terug zolang er nog een nieuw nummer is", _cfg.mons[slot].name, (int)_cfg.mons[slot].channel);

  memset(&_cfg.mons[slot], 0, sizeof(_cfg.mons[slot]));   /* channel = 0 -> leeg */
  memset(&_mon[slot], 0, sizeof(_mon[slot]));

  /* ch_ever_used blijft staan. Dat IS de afspraak: het nummer is vergeven. */
  markDirty();
  return MON_OK;
}

/* Een melding van buiten. Loopt over dezelfde toewijzer, dezelfde zeef en
 * dezelfde velden als een ping-monitor; het enige eigen werk is de klok van de
 * verouderingsregel bijzetten.
 *
 * GEEN HYSTERESE HIER, en dat is een keuze en geen vergetelheid. Bij een ping
 * meten WIJ, en één verloren ICMP-pakket is geen storing -- vandaar
 * PINGS_TO_DOWN. Een melding is geen meting maar een UITSPRAAK van iets dat zijn
 * eigen pogingen en time-outs al achter zich heeft (Uptime Kuma doet standaard
 * meerdere retries voordat hij "down" meldt). Die uitspraak nog een keer
 * uitstellen zou de reactietijd verdubbelen zonder één fout minder te maken, en
 * zou bovendien meldingen tellen als "rondes" die van ons niet zijn.
 */
MonitorSensors::MonResult MonitorSensors::reportMonitor(const char* name, bool up, uint32_t ms,
                                                       uint16_t every_s, uint8_t* out_channel) {
  if (out_channel != NULL) *out_channel = 0;

  if (!validName(name)) return MON_ERR_NAME;
  if (every_s != 0 && (every_s < MON_INTERVAL_MIN || every_s > MON_INTERVAL_MAX)) {
    return MON_ERR_INTERVAL;
  }

  int slot = findByName(name);

  if (slot < 0) {
    /* Nog niet bekend: aanmaken, met een kanaal uit de gewone toewijzer. Dat
     * kanaal is vanaf nu van deze naam en wordt niet opnieuw uitgedeeld -- zie
     * allocChannel(). Een gemelde dienst is daarin niets bijzonders. */
    MonResult r = createMonitor(name, PUSH_HOST,
                                every_s != 0 ? every_s : MON_INTERVAL_DEFAULT,
                                out_channel);
    if (r != MON_OK) return r;
    slot = findByName(name);
    if (slot < 0) return MON_ERR_FULL;   /* kan niet; vangnet */
  } else {
    /* Bestaat wel, maar pingen wij hem zelf? Dan weigeren. Twee bronnen voor één
     * kanaal betekent dat de laatste schrijver wint en dat niemand nog kan zien
     * welke van de twee dat was -- precies de stille fout die dit bestand op
     * elke andere plek probeert te vermijden. */
    if (!monitorIsPush(slot)) return MON_ERR_KIND;

    if (every_s != 0 && _cfg.mons[slot].interval_s != every_s) {
      _cfg.mons[slot].interval_s = every_s;
      markDirty();
    }
    if (out_channel != NULL) *out_channel = _cfg.mons[slot].channel;
  }

  MonState& m = _mon[slot];

  m.checks++;
  if (!up) m.fails++;
  if (up)  m.last_ms = ms;

  const bool was_known = m.seeded;
  const bool was_up    = m.up;

  m.up          = up;
  m.seeded      = true;
  m.stale       = false;      /* er is net gemeld; de stiltetijd begint opnieuw */
  m.agree       = 0;
  m.last_report = millis();
  /* millis() kan 0 zijn (eens per 49 dagen); 0 betekent hier "nog nooit gemeld",
   * dus die ene tik schuiven we op. Kost niets en haalt een fout weg die je
   * anders nooit terugvindt. */
  if (m.last_report == 0) m.last_report = 1;

  if (!was_known || was_up != up) {
    MESH_DEBUG_PRINTLN("MonitorSensors: '%s' gemeld als %s (kanaal %d, %lu ms)",
                       _cfg.mons[slot].name, up ? "up" : "DOWN",
                       (int)_cfg.mons[slot].channel, (unsigned long)ms);
  }
  return MON_OK;
}

/* spec = "naam,adres[,interval]". Komma's en geen spaties, omdat CommonCLI de
 * regel na "sensor set <sleutel> " op spaties splitst (CommonCLI.cpp:285) en
 * dus alleen een waarde zonder spaties doorgeeft.
 *
 * Alleen het ONTLEDEN staat hier; de keuring en het aanmaken doet
 * createMonitor(). Een adres van "-" maakt hier dus net zo goed een gemelde
 * dienst als /hook dat doet -- handig om er een aan te maken (en zijn kanaal
 * vast te leggen) voordat de melder voor het eerst iets stuurt. */
bool MonitorSensors::addMonitor(const char* spec) {
  char buf[MON_NAME_LEN + MON_HOST_LEN + 8];
  StrHelper::strncpy(buf, spec, sizeof(buf));

  char* name = buf;
  char* host = strchr(buf, ',');
  if (host == NULL) return false;
  *host++ = 0;

  char* ivl = strchr(host, ',');
  if (ivl != NULL) *ivl++ = 0;

  int interval = (ivl != NULL && *ivl) ? atoi(ivl) : MON_INTERVAL_DEFAULT;
  if (interval < 0 || interval > 0xFFFF) return false;   /* voor de cast hieronder */

  return createMonitor(name, host, (uint16_t)interval) == MON_OK;
}

bool MonitorSensors::delMonitor(const char* name) {
  return deleteMonitor(name) == MON_OK;
}

/* ===================== instellingen =====================
 *
 * Bereikbaar via de seriële console en over DM-CLI met de generieke
 * opdrachten van CommonCLI: "sensor list", "sensor get mains.hi",
 * "sensor set mains.hi 4.13".
 *
 * De eigen instellingen komen ACHTER die van de basisklasse, zodat de
 * bestaande indices niet verschuiven.
 *
 * DE VORM
 *
 *   mains.hi              4.120     drempel omhoog
 *   mains.lo              4.090     drempel omlaag
 *   mains.state           1         alleen lezen
 *   mon.count             2         alleen lezen
 *   mon.add   <- naam,adres[,interval]
 *   mon.del   <- naam
 *   mon.5.name            google
 *   mon.5.host            8.8.8.8
 *   mon.5.int             60
 *   mon.5.state           up 23     alleen lezen
 *
 * Het getal in "mon.5.host" is HET KANAALNUMMER en niet een rangnummer.
 * Bewust: een rangnummer schuift op als er een monitor verdwijnt, en dan wijst
 * "mon.2.host" ineens naar een andere dienst -- precies de fout die we bij de
 * telemetriekanalen met zoveel moeite vermijden. Een sleutel die je in een
 * script of een sneltoets zet, moet naar hetzelfde blijven wijzen. Het kanaal
 * is het enige nummer hier dat die belofte kan houden.
 *
 * EEN GRENS DIE NIET VAN ONS IS: CommonCLI kopieert de tekst achter
 * "sensor set " in een buffer van 68 byte (CommonCLI.h:258, tmp[PRV_KEY_SIZE*2+4],
 * gevuld met strcpy zonder maat). Alles bij elkaar -- sleutel, spatie en waarde
 * -- moet dus onder de 67 tekens blijven. Voor "mon.add " houdt dat de waarde op
 * 59 tekens, en dat is genoeg voor een naam van 16 plus een adres van 40 ZONDER
 * interval. Bij een lang adres het interval daarom in een tweede opdracht
 * zetten met mon.<kanaal>.int. Hier is niets aan te doen zonder CommonCLI te
 * wijzigen, en dat is geen bestand van deze opdracht.
 *
 * mon.add en mon.del zijn knoppen en geen waarden. Ze staan in de lijst omdat
 * "sensor list" dan zelf vertelt hoe je een monitor toevoegt; lezen geeft de
 * verwachte vorm terug in plaats van een waarde.
 *
 * EEN ADRES VAN "-" (PUSH_HOST) maakt geen ping-monitor maar een GEMELDE dienst:
 * wij pingen hem niet, /hook levert de uitslag, en hij veroudert als die uitblijft.
 * "mon.add kuma-web,-,120" legt dus alvast naam en kanaal vast voor iets dat
 * elke 2 minuten meldt. Datzelfde geldt de andere kant op: mon.<kanaal>.host van
 * "-" naar een echt adres verandert de soort, en de gemeten toestand gaat daarbij
 * weg -- die hoorde bij de andere bron.
 *
 * Anders dan bij upstream blijven deze instellingen bestaan na een herstart:
 * elke gelukte wijziging zet _dirty, en loop() schrijft ze naar SPIFFS. Zie
 * MonitorStore.h.
 */

#define MON_FIELDS_PER_MONITOR  5     /* name, host, int, state, ms */
#define MON_NUM_GLOBAL_SETTINGS 9     /* mains.hi/lo/state + mon.count/add/del
                                       + alert.recover/alert.rhold/alert.repeat */

/* Vaste buffers, één per instelling: getSettingValue() is const en mag niets
 * alloceren, en met een buffer per instelling kan een aanroeper meerdere
 * waarden naast elkaar vasthouden zonder verrassing. */
static char s_setting_buf[4][12];

/* Voor de monitorinstellingen: één naambuffer en één waardebuffer. Ze moeten
 * apart zijn, want CommonCLI doet sprintf("%s=%s", getSettingName(i),
 * getSettingValue(i)) en die twee aanroepen staan dus tegelijk uit
 * (CommonCLI.cpp:302). Twee monitorwaarden naast elkaar vasthouden kan hier
 * niet, en dat hoeft ook niemand: elke aanroeper in de boom leest er één. */
static char s_mon_name_buf[20];       /* "mon.36.state" + afsluiter */
static char s_mon_val_buf[MON_HOST_LEN];

int MonitorSensors::getNumSettings() const {
  return EnvironmentSensorManager::getNumSettings()
       + MON_NUM_GLOBAL_SETTINGS
       + MON_FIELDS_PER_MONITOR * (int)getNumMonitors();
}

const char* MonitorSensors::getSettingName(int i) const {
  const int base = EnvironmentSensorManager::getNumSettings();
  if (i < base) return EnvironmentSensorManager::getSettingName(i);

  const int idx = i - base;
  switch (idx) {
    case 0: return "mains.hi";      // drempel omhoog: hierboven netspanning
    case 1: return "mains.lo";      // drempel omlaag: hieronder batterij
    case 2: return "mains.state";   // alleen lezen: huidige uitkomst
    case 3: return "mon.count";     // alleen lezen: aantal monitors
    case 4: return "mon.add";       // knop: naam,adres[,interval]
    case 5: return "mon.del";       // knop: naam
    /* De herstelmelding hangt hier en niet aan een eigen webroute, met opzet: zo
     * loopt hij door dezelfde keuring en dezelfde opslag als elke andere
     * instelling, en is hij ook over serieel en over een DM te zetten. Een node
     * zonder wifi hoort zijn alarmgedrag ook te kunnen wijzigen. */
    case 6: return "alert.recover"; // 0/1: melden als iets weer werkt
    case 7: return "alert.rhold";   // s: hoe lang op voordat het herstel gemeld wordt
    case 8: return "alert.repeat";  // s: herhaal tot een mens "ok" stuurt (0 = uit)
  }

  const int k     = idx - MON_NUM_GLOBAL_SETTINGS;
  const int nth   = k / MON_FIELDS_PER_MONITOR;
  const int field = k % MON_FIELDS_PER_MONITOR;
  const int slot  = slotOfNth(nth);
  if (slot < 0) return NULL;

  /* "ms" is de knop uit het bytebudget: 1 = de pingtijd gaat mee over het mesh
   * (9 byte), 0 = alleen de schakelaar (3 byte). De METING loopt in beide gevallen
   * door en blijft in "state" te zien. */
  static const char* names[MON_FIELDS_PER_MONITOR] = { "name", "host", "int", "state", "ms" };
  snprintf(s_mon_name_buf, sizeof(s_mon_name_buf), "mon.%u.%s",
           (unsigned)_cfg.mons[slot].channel, names[field]);
  return s_mon_name_buf;
}

const char* MonitorSensors::getSettingValue(int i) const {
  const int base = EnvironmentSensorManager::getNumSettings();
  if (i < base) return EnvironmentSensorManager::getSettingValue(i);

  const int idx = i - base;
  switch (idx) {
    case 0:
    case 1:
      snprintf(s_setting_buf[idx], sizeof(s_setting_buf[idx]), "%.3f", idx == 0 ? _cfg.mains_hi : _cfg.mains_lo);
      return s_setting_buf[idx];
    case 2:
      snprintf(s_setting_buf[2], sizeof(s_setting_buf[2]), "%d", _mains ? 1 : 0);
      return s_setting_buf[2];
    case 3:
      snprintf(s_setting_buf[3], sizeof(s_setting_buf[3]), "%d", (int)getNumMonitors());
      return s_setting_buf[3];
    case 4:
      return "<naam,adres[,interval]>";   /* knop: de vorm, niet een waarde */
    case 5:
      return "<naam>";
    case 6:
      snprintf(s_setting_buf[2], sizeof(s_setting_buf[2]), "%d",
               _cfg.recover_alerts ? 1 : 0);
      return s_setting_buf[2];
    case 7:
      snprintf(s_setting_buf[3], sizeof(s_setting_buf[3]), "%u",
               (unsigned)_cfg.rhold_s);
      return s_setting_buf[3];
    case 8:
      /* Hergebruik van buffer [2]: elke aanroeper leest er één (zie de noot bij
       * s_mon_val_buf), dus twee waarden hoeven hier niet naast elkaar te staan. */
      snprintf(s_setting_buf[2], sizeof(s_setting_buf[2]), "%u",
               (unsigned)_cfg.repeat_s);
      return s_setting_buf[2];
  }

  const int k     = idx - MON_NUM_GLOBAL_SETTINGS;
  const int nth   = k / MON_FIELDS_PER_MONITOR;
  const int field = k % MON_FIELDS_PER_MONITOR;
  const int slot  = slotOfNth(nth);
  if (slot < 0) return NULL;

  switch (field) {
    case 0:
      StrHelper::strncpy(s_mon_val_buf, _cfg.mons[slot].name, sizeof(s_mon_val_buf));
      break;
    case 1:
      StrHelper::strncpy(s_mon_val_buf, _cfg.mons[slot].host, sizeof(s_mon_val_buf));
      break;
    case 2:
      snprintf(s_mon_val_buf, sizeof(s_mon_val_buf), "%u", (unsigned)_cfg.mons[slot].interval_s);
      break;
    case 4:
      /* Met het BYTEGETAL erbij, want dat is waar deze knop over gaat. Wie
       * 'sensor list' leest, ziet dan meteen wat hij per monitor kost. */
      snprintf(s_mon_val_buf, sizeof(s_mon_val_buf), "%u (%ub)",
               (unsigned)(_cfg.mons[slot].send_ms ? 1 : 0),
               (unsigned)monitorTelemBytes(slot));
      break;
    default:
      /* "pauze" is geen toestand van de dienst maar van ons: zie loopMonitors().
       * Hij staat er zodat iemand die "down" verwacht en "pauze" ziet, weet dat
       * hij naar zijn eigen wifi moet kijken en niet naar de dienst. */
      if (monitorsPaused()) {
        snprintf(s_mon_val_buf, sizeof(s_mon_val_buf), "pauze");
      } else if (monitorIsStale(slot)) {
        /* Eigen woord, want dit is niet hetzelfde als "?" bij het opstarten: hier
         * WAS er een melding en die blijft nu uit. Wie dit ziet, moet naar zijn
         * melder kijken en niet naar de dienst. */
        snprintf(s_mon_val_buf, sizeof(s_mon_val_buf), "stil %lus",
                 (unsigned long)monitorReportAge(slot));
      } else if (!_mon[slot].seeded) {
        snprintf(s_mon_val_buf, sizeof(s_mon_val_buf), "?");
      } else if (_mon[slot].up) {
        snprintf(s_mon_val_buf, sizeof(s_mon_val_buf), "up %lums", (unsigned long)_mon[slot].last_ms);
      } else {
        snprintf(s_mon_val_buf, sizeof(s_mon_val_buf), "down %lu/%lu",
                 (unsigned long)_mon[slot].fails, (unsigned long)_mon[slot].checks);
      }
      break;
  }
  return s_mon_val_buf;
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
    if (is_hi && v <= _cfg.mains_lo) return false;
    if (is_lo && v >= _cfg.mains_hi) return false;

    if (is_hi) _cfg.mains_hi = v; else _cfg.mains_lo = v;

    /* Een nieuwe drempel maakt de lopende teller waardeloos: die is met de
     * oude grenzen opgebouwd. */
    _agree = 0;
    markDirty();
    return true;
  }

  /* mains.state en mon.count zijn uitlezingen, geen knoppen. */
  if (strcmp(name, "mains.state") == 0) return false;
  if (strcmp(name, "mon.count") == 0) return false;

  if (strcmp(name, "alert.recover") == 0) {
    /* Alleen 0 en 1, en niet "alles wat niet nul is": wie "aan" intypt zou
     * anders stil 0 krijgen (atoi geeft 0) en denken dat het aanstaat. */
    if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) return false;
    _cfg.recover_alerts = (value[0] == '1') ? 1 : 0;
    markDirty();
    return true;
  }
  if (strcmp(name, "alert.rhold") == 0) {
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || *end != 0) return false;
    if (v < MON_RHOLD_MIN || v > MON_RHOLD_MAX) return false;
    _cfg.rhold_s = (uint16_t)v;
    markDirty();
    return true;
  }
  if (strcmp(name, "alert.repeat") == 0) {
    /* 0 = uit (geen herhaling), anders binnen de grenzen. 0 apart toegestaan,
     * want die valt buiten [MIN..MAX] en zou anders geweigerd worden. */
    char* end = NULL;
    long v = strtol(value, &end, 10);
    if (end == value || *end != 0) return false;
    if (v != 0 && (v < MON_AREPEAT_MIN || v > MON_AREPEAT_MAX)) return false;
    _cfg.repeat_s = (uint16_t)v;
    markDirty();
    return true;
  }

  if (strcmp(name, "mon.add") == 0) return addMonitor(value);
  if (strcmp(name, "mon.del") == 0) return delMonitor(value);

  /* mon.<kanaal>.<veld> */
  if (memcmp(name, "mon.", 4) == 0) {
    const char* p = name + 4;
    int ch = atoi(p);
    const char* dot = strchr(p, '.');
    if (dot == NULL) return false;
    const char* field = dot + 1;

    if (ch < CH_MONITOR_FIRST || ch > CH_MONITOR_LAST) return false;
    int slot = findByChannel((uint8_t)ch);
    if (slot < 0) return false;

    if (strcmp(field, "ms") == 0) {
      /* AANZETTEN kost 6 byte extra en kan dus niet altijd; UITZETTEN maakt juist
       * ruimte en mag altijd. Dat is geen willekeur maar dezelfde regel als bij
       * het opheffen van een simulatie: de handeling die naar een veiliger stand
       * gaat, hoort nooit geweigerd te worden. */
      if (strcmp(value, "0") != 0 && strcmp(value, "1") != 0) return false;
      const bool want = (value[0] == '1');
      if (want && !_cfg.mons[slot].send_ms
          && !telemFits(TELEM_BYTES_GENERIC_PUB)) return false;
      _cfg.mons[slot].send_ms = want ? 1 : 0;
      markDirty();
      return true;
    }

    if (strcmp(field, "name") == 0) {
      /* Hernoemen mag; het KANAAL verandert daarbij niet. Een naam is voor de
       * mens, het kanaal is voor de machine. */
      if (!validName(value)) return false;
      int other = findByName(value);
      if (other >= 0 && other != slot) return false;    /* naam al in gebruik */
      StrHelper::strncpy(_cfg.mons[slot].name, value, sizeof(_cfg.mons[slot].name));
      markDirty();
      return true;
    }

    if (strcmp(field, "host") == 0) {
      if (!validHost(value)) return false;
      StrHelper::strncpy(_cfg.mons[slot].host, value, sizeof(_cfg.mons[slot].host));

      /* Ander adres: het opgeloste adres vergeten en de gemeten toestand
       * weggooien. Die toestand hoort bij het oude adres, en hem laten staan
       * zou betekenen dat "up" over een andere dienst gaat dan de naam nu
       * belooft. Loopt er juist een meting voor dit vakje, dan gaat die weg. */
      if (_busy_slot == slot) abortPing();
      memset(&_mon[slot], 0, sizeof(_mon[slot]));
      _mon[slot].up = true;
      _mon[slot].next_check = millis();
      markDirty();
      return true;
    }

    if (strcmp(field, "int") == 0) {
      int v = atoi(value);
      if (v < MON_INTERVAL_MIN || v > MON_INTERVAL_MAX) return false;
      _cfg.mons[slot].interval_s = (uint16_t)v;
      /* Meteen op de nieuwe tik zetten in plaats van de oude af te wachten. */
      _mon[slot].next_check = millis() + (unsigned long)v * 1000UL;
      markDirty();
      return true;
    }

    return false;   /* mon.N.state is een uitlezing; onbekend veld idem */
  }

  return EnvironmentSensorManager::setSettingValue(name, value);
}
