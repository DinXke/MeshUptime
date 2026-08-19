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

bool MonitorSensors::isWifiOnline() const {
  return _wifi != NULL && _wifi->isOnline();
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
  const bool online = isWifiOnline();
  unsigned long now = millis();

  if (online != _wifi_was_online) {
    _wifi_was_online = online;

    if (online) {
      _wifi_ok_since = now;
      /* Opnieuw uitsmeren: anders staan na de insteltijd alle acht monitors
       * tegelijk klaar en dringen ze achter elkaar in de rij. */
      for (int i = 0; i < MAX_MONITORS; i++) {
        _mon[i].next_check = now + WIFI_GRACE_MS + (unsigned long)i * PING_GAP_MS;
        _mon[i].addr_expiry = now;   /* adressen opnieuw opzoeken: het netwerk kan een ander zijn */
      }
      MESH_DEBUG_PRINTLN("MonitorSensors: wifi terug, bewaking start over %lu s", (unsigned long)(WIFI_GRACE_MS / 1000));
    } else {
      /* Lopende meting weggooien. abortPing() rekent hem NIET als mislukking. */
      if (_phase != PING_IDLE) abortPing();
      MESH_DEBUG_PRINTLN("MonitorSensors: wifi weg, bewaking bevroren (toestanden blijven staan)");
    }
  }

  if (!online) return;
  if ((long)(now - (_wifi_ok_since + WIFI_GRACE_MS)) < 0) return;   /* insteltijd */

  switch (_phase) {
    case PING_RESOLVING: {
      uint8_t st = s_ping.dns_state;
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
        _phase = PING_IDLE;
        _next_ping_at = now + PING_GAP_MS;
      }
      break;

    case PING_IDLE:
      if ((long)(now - _next_ping_at) >= 0) startNextPing();
      break;
  }
}

/* Kiest het vakje dat het langst op zijn ronde wacht. Eén ping tegelijk, dus
 * hier wordt gekozen en niet gestapeld. Het meest achterlopende vakje eerst
 * houdt de intervallen eerlijk als er meer monitors zijn dan er tijd is. */
void MonitorSensors::startNextPing() {
  unsigned long now = millis();
  int  best = -1;
  long best_overdue = -1;

  for (int i = 0; i < MAX_MONITORS; i++) {
    if (!monitorUsed(i)) continue;
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
bool        MonitorSensors::monitorIsUp(int slot) const     { return monitorUsed(slot) && _mon[slot].seeded && _mon[slot].up; }
bool        MonitorSensors::monitorSeeded(int slot) const   { return monitorUsed(slot) && _mon[slot].seeded; }
uint32_t    MonitorSensors::monitorPingMs(int slot) const   { return monitorUsed(slot) ? _mon[slot].last_ms : 0; }
uint32_t    MonitorSensors::monitorChecks(int slot) const   { return monitorUsed(slot) ? _mon[slot].checks : 0; }
uint32_t    MonitorSensors::monitorFails(int slot) const    { return monitorUsed(slot) ? _mon[slot].fails : 0; }

bool MonitorSensors::monitorsPaused() const {
  if (!isWifiOnline()) return true;
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
 * Geen eigen lus, geen eigen herhalingen: alles gaat via alertIf() van
 * SensorMesh, dat één keer per overgang vuurt, vier pogingen doet, vier
 * expected_acks bijhoudt en de waarschuwing opruimt zodra de storing over is.
 * Hier staat alleen de VOORWAARDE, en de begrenzing op MAX_MONITOR_ALERTS.
 */
bool MonitorSensors::monitorAlert(int slot) {
  if (!monitorUsed(slot)) {
    /* Vakje is leeg (of net verwijderd): voorwaarde uit, zodat main.cpp met
     * alertIf(false, ...) de bijbehorende Trigger opruimt. */
    if (slot >= 0 && slot < MAX_MONITORS) _mon[slot].alerting = false;
    return false;
  }

  MonState& m = _mon[slot];

  /* Bevestigd down, en we waren op dat moment niet blind. monitorsPaused()
   * dekt zowel "geen wifi" als de insteltijd erna. */
  const bool want = m.seeded && !m.up && !monitorsPaused();

  if (!want) {
    m.alerting = false;
    return false;
  }
  if (m.alerting) return true;    /* loopt al; niet nog eens gaan tellen */

  uint8_t active = 0;
  for (int i = 0; i < MAX_MONITORS; i++) if (_mon[i].alerting) active++;
  if (active >= MAX_MONITOR_ALERTS) {
    /* Rem, geen verlies: zodra er een plaats vrijkomt komt deze monitor bij
     * een volgende leesronde aan de beurt, want de voorwaarde blijft waar
     * zolang hij down is. */
    return false;
  }

  m.alerting = true;
  return true;
}

/* Vaste buffer. alertIf() kopieert de tekst meteen in de Trigger, dus hij hoeft
 * maar tot het einde van die aanroep te bestaan. */
static char s_alert_buf[80];

const char* MonitorSensors::monitorAlertText(int slot) const {
  if (!monitorUsed(slot)) { s_alert_buf[0] = 0; return s_alert_buf; }

  snprintf(s_alert_buf, sizeof(s_alert_buf), "%s onbereikbaar (%s)",
           _cfg.mons[slot].name, _cfg.mons[slot].host);
  return s_alert_buf;
}

/* ===================== telemetrie ===================== */

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

    /* De monitors. Op vakjesvolgorde en dus op de volgorde waarin ze zijn
     * aangemaakt; de kanaalnummers zijn wat telt en die staan vast.
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

      const bool up = _mon[i].seeded && _mon[i].up;
      const uint8_t need = TELEM_BYTES_SWITCH + (up ? TELEM_BYTES_GENERIC : 0);

      if ((int)telemetry.getSize() + need > TELEM_BUDGET) {
        MESH_DEBUG_PRINTLN("MonitorSensors: telemetrie vol bij kanaal %d, rest afgekapt", (int)_cfg.mons[i].channel);
        break;
      }

      telemetry.addSwitch(_cfg.mons[i].channel, up ? 1 : 0);
      if (up) {
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
 * Om diezelfde reden staat ch_ever_used in het opslagbestand: zonder die byte
 * zou de node na een herstart weer bij 5 beginnen uitdelen en was de hele
 * afspraak een afspraak voor de duur van één stroomvoorziening.
 *
 * Zijn alle acht nummers een keer gebruikt en is er niets meer nieuw uit te
 * delen, dan pas wordt een vrijgekomen nummer hergebruikt -- de laagste. Op dat
 * moment is hergebruik onvermijdelijk; wie zijn monitors acht keer opnieuw
 * indeelt, moet zijn dashboards nalopen. Dat staat in de melding hieronder.
 */
uint8_t MonitorSensors::allocChannel() {
  uint8_t in_use = 0;
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (_cfg.mons[i].channel != 0) {
      in_use |= (uint8_t)(1 << (_cfg.mons[i].channel - CH_MONITOR_FIRST));
    }
  }

  /* 1. een nummer dat nog nooit is uitgedeeld */
  for (uint8_t b = 0; b < MAX_MONITORS; b++) {
    if (!(_cfg.ch_ever_used & (1 << b))) {
      _cfg.ch_ever_used |= (uint8_t)(1 << b);
      return (uint8_t)(CH_MONITOR_FIRST + b);
    }
  }

  /* 2. alles is een keer gebruikt: nu pas hergebruiken */
  for (uint8_t b = 0; b < MAX_MONITORS; b++) {
    if (!(in_use & (1 << b))) {
      MESH_DEBUG_PRINTLN("MonitorSensors: kanaal %d wordt HERGEBRUIKT; dashboards die dit kanaal bewaard hebben, tonen nu een andere dienst", (int)(CH_MONITOR_FIRST + b));
      return (uint8_t)(CH_MONITOR_FIRST + b);
    }
  }

  return 0;   /* alle acht bezet */
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

/* spec = "naam,adres[,interval]". Komma's en geen spaties, omdat CommonCLI de
 * regel na "sensor set <sleutel> " op spaties splitst (CommonCLI.cpp:285) en
 * dus alleen een waarde zonder spaties doorgeeft. */
bool MonitorSensors::addMonitor(const char* spec) {
  char buf[MON_NAME_LEN + MON_HOST_LEN + 8];
  StrHelper::strncpy(buf, spec, sizeof(buf));

  char* name = buf;
  char* host = strchr(buf, ',');
  if (host == NULL) return false;
  *host++ = 0;

  char* ivl = strchr(host, ',');
  if (ivl != NULL) *ivl++ = 0;

  if (!validName(name)) return false;
  if (!validHost(host)) return false;

  int interval = (ivl != NULL && *ivl) ? atoi(ivl) : MON_INTERVAL_DEFAULT;
  if (interval < MON_INTERVAL_MIN || interval > MON_INTERVAL_MAX) return false;

  if (findByName(name) >= 0) return false;    /* naam al in gebruik */

  int slot = -1;
  for (int i = 0; i < MAX_MONITORS; i++) {
    if (_cfg.mons[i].channel == 0) { slot = i; break; }
  }
  if (slot < 0) return false;                 /* alle acht vakjes bezet */

  uint8_t ch = allocChannel();
  if (ch == 0) return false;

  MonitorCfgEntry& e = _cfg.mons[slot];
  StrHelper::strncpy(e.name, name, sizeof(e.name));
  StrHelper::strncpy(e.host, host, sizeof(e.host));
  e.interval_s = (uint16_t)interval;
  e.channel    = ch;

  memset(&_mon[slot], 0, sizeof(_mon[slot]));
  _mon[slot].up = true;               /* nog niet 'seeded'; zie applyResult() */
  _mon[slot].next_check = millis();   /* meteen aan de beurt */

  markDirty();
  MESH_DEBUG_PRINTLN("MonitorSensors: monitor '%s' -> %s op kanaal %d, elke %d s", e.name, e.host, (int)ch, interval);
  return true;
}

bool MonitorSensors::delMonitor(const char* name) {
  int slot = findByName(name);
  if (slot < 0) return false;

  /* Wordt er juist naar dit vakje gepingd? Dan die meting weggooien: de uitslag
   * hoort bij een monitor die niet meer bestaat. */
  if (_busy_slot == slot) abortPing();

  MESH_DEBUG_PRINTLN("MonitorSensors: monitor '%s' verwijderd; kanaal %d komt niet terug zolang er nog een nieuw nummer is", _cfg.mons[slot].name, (int)_cfg.mons[slot].channel);

  memset(&_cfg.mons[slot], 0, sizeof(_cfg.mons[slot]));   /* channel = 0 -> leeg */
  memset(&_mon[slot], 0, sizeof(_mon[slot]));

  /* ch_ever_used blijft staan. Dat IS de afspraak: het nummer is vergeven. */
  markDirty();
  return true;
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
 * Anders dan bij upstream blijven deze instellingen bestaan na een herstart:
 * elke gelukte wijziging zet _dirty, en loop() schrijft ze naar SPIFFS. Zie
 * MonitorStore.h.
 */

#define MON_FIELDS_PER_MONITOR  4     /* name, host, int, state */
#define MON_NUM_GLOBAL_SETTINGS 6     /* mains.hi/lo/state + mon.count/add/del */

/* Vaste buffers, één per instelling: getSettingValue() is const en mag niets
 * alloceren, en met een buffer per instelling kan een aanroeper meerdere
 * waarden naast elkaar vasthouden zonder verrassing. */
static char s_setting_buf[4][12];

/* Voor de monitorinstellingen: één naambuffer en één waardebuffer. Ze moeten
 * apart zijn, want CommonCLI doet sprintf("%s=%s", getSettingName(i),
 * getSettingValue(i)) en die twee aanroepen staan dus tegelijk uit
 * (CommonCLI.cpp:302). Twee monitorwaarden naast elkaar vasthouden kan hier
 * niet, en dat hoeft ook niemand: elke aanroeper in de boom leest er één. */
static char s_mon_name_buf[20];       /* "mon.12.state" + afsluiter */
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
  }

  const int k     = idx - MON_NUM_GLOBAL_SETTINGS;
  const int nth   = k / MON_FIELDS_PER_MONITOR;
  const int field = k % MON_FIELDS_PER_MONITOR;
  const int slot  = slotOfNth(nth);
  if (slot < 0) return NULL;

  static const char* names[MON_FIELDS_PER_MONITOR] = { "name", "host", "int", "state" };
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
    default:
      /* "pauze" is geen toestand van de dienst maar van ons: zie loopMonitors().
       * Hij staat er zodat iemand die "down" verwacht en "pauze" ziet, weet dat
       * hij naar zijn eigen wifi moet kijken en niet naar de dienst. */
      if (monitorsPaused()) {
        snprintf(s_mon_val_buf, sizeof(s_mon_val_buf), "pauze");
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
