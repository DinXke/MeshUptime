#include "WifiTask.h"
#include <WiFi.h>
#include <time.h>
#include "esp_sntp.h"
#include "target.h"

/* Logging. Een waakhond die bestaat om een moeilijk te betrappen storing te
 * repareren, moet kunnen vertellen dat hij iets gedaan heeft -- anders weet je
 * na een week niet of hij werkt of of het probleem gewoon niet optrad. Elke
 * toestandsovergang en elke harde reset gaat naar de console, met de reden
 * erbij. 202 is WIFI_REASON_AUTH_FAIL; dat getal opzoeken kost anders tijd
 * precies op het moment dat je het niet hebt. */
#ifndef WIFI_LOG
  #define WIFI_LOG(...) Serial.printf("[wifi] " __VA_ARGS__)
#endif

static const char* stateName(int s) {
  switch (s) {
    case 0: return "uit";
    case 1: return "verbinden";
    case 2: return "online";
    case 3: return "opnieuw";
    case 4: return "eigen-ap";
  }
  return "?";
}

/* Tijden. Bewust ruim: de radio van het mesh gaat voor, en een node die elke
 * vijf seconden zijn WiFi opnieuw opstart stoort zijn eigen LoRa-timing. */
#define CONNECT_TIMEOUT_MS     20000
#define RETRY_DELAY_MS         15000
#define WATCHDOG_MS           180000   // 3 min zonder verbinding -> radio omlaag
#define FAILS_BEFORE_RESET         3
#define FAILS_BEFORE_AP            6

/* Het toegangspunt waarop je terugvalt. Geen wachtwoord: wie fysiek bij het
 * toestel kan, kan het ook met een kabel instellen, en een half onthouden
 * wachtwoord op een terugvalnetwerk is precies wat je niet wil op het moment
 * dat je het nodig hebt. De webinterface erachter heeft WEL inloggegevens. */
#define AP_SSID_PREFIX      "MeshUptime-"

/* Ruim: de klok is nergens dringend voor, en een SNTP-poging die de radio in de
 * weg zit is erger dan een klok die een minuut later goed staat. */
#define SNTP_WAIT_MS         45000

static volatile uint8_t g_last_reason = 0;
static volatile bool    g_got_ip = false;
static volatile bool    g_dropped = false;

void WifiTask::begin(const char* ssid, const char* pwd) {
  strncpy(_ssid, ssid ? ssid : "", sizeof(_ssid) - 1);
  strncpy(_pwd,  pwd  ? pwd  : "", sizeof(_pwd)  - 1);

  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
      g_last_reason = info.wifi_sta_disconnected.reason;
      g_dropped = true;
    } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      g_got_ip = true;
    }
  });

  if (_ssid[0] == 0) { startAP(); return; }   // niets ingesteld -> meteen bereikbaar zijn
  startConnect();
}

void WifiTask::setState(State s) {
  if (_state != s) {
    WIFI_LOG("%s -> %s (reden %u, mislukt %u, resets %u)\n",
             stateName(_state), stateName(s), _last_reason, _fails, _hard_resets);
    _state = s;
    _state_since = millis();
  }
}

uint32_t WifiTask::secsInState() const {
  return (millis() - _state_since) / 1000;
}

void WifiTask::startConnect() {
  g_got_ip = g_dropped = false;
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin(_ssid, _pwd);
  setState(CONNECTING);
  _next_action = millis() + CONNECT_TIMEOUT_MS;
}

/* De reparatie van het gemeten gedrag: niet reconnect(), maar de radio echt
 * omlaag en opnieuw op. Bij AUTH_FAIL bleef de stapel anders in een toestand
 * hangen waar hij niet uit kwam, ook niet toen de oorzaak weg was. */
void WifiTask::hardReset() {
  _hard_resets++;
  WIFI_LOG("harde reset van de radio (nr %u), laatste reden %u\n",
           _hard_resets, _last_reason);
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  delay(200);
  startConnect();
}

void WifiTask::startAP() {
  char ap[40];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(ap, sizeof(ap), "%s%02X%02X", AP_SSID_PREFIX,
           (uint8_t)(mac >> 8), (uint8_t)mac);
  WIFI_LOG("verbinden lukt niet, eigen toegangspunt %s\n", ap);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap);
  setState(AP_MODE);
  _next_action = 0;
}

/* De ondergrens waaronder een tijd niet echt kan zijn.
 *
 * NIET 1.7e9, en dat was mijn eerste fout: de vaste terugvalwaarde van deze
 * firmware is 1715770351 (15 mei 2024, CommonCLI.cpp:188) en die ligt DAARBOVEN.
 * Een drempel die de standaardwaarde doorlaat, keurt precies het geval goed dat
 * hij moest tegenhouden. 1 januari 2025 ligt na elke terugvalwaarde in deze
 * broncode en ver voor elke echte tijd die we ooit zullen zien. */
#define TIME_FLOOR   1735689600UL

void WifiTask::setNtpServer(const char* host) {
  strncpy(_ntp, host ? host : "", sizeof(_ntp) - 1);
  _ntp[sizeof(_ntp) - 1] = 0;
}

void WifiTask::syncNow() {
  if (_state != ONLINE) {
    strncpy(_sync_msg, "geen wifi", sizeof(_sync_msg) - 1);
    return;
  }
  startTimeSync();
}

void WifiTask::startTimeSync() {
  _time_synced = false;
  /* De vlag WISSEN voordat we vragen. Anders leest checkTimeSync() straks de
   * status van de VORIGE poging als bewijs voor deze. */
  /* Consequent de esp_sntp_*-familie: de oudere lwip-namen (sntp_setoperatingmode)
   * nemen een int en de nieuwe een eigen enum, en die twee door elkaar gebruiken
   * gaf 'invalid conversion from int to esp_sntp_operatingmode_t'. */
  esp_sntp_stop();
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, ntpServer());
  esp_sntp_init();
  strncpy(_sync_msg, "bezig", sizeof(_sync_msg) - 1);
  _sntp_deadline = millis() + SNTP_WAIT_MS;
}

void WifiTask::checkTimeSync() {
  if (_sntp_deadline == 0) return;

  /* Vragen of SNTP ANTWOORD HEEFT GEHAD, en niet of time() een getal teruggeeft.
   * time() geeft altijd iets -- de eigen verkeerde klok als er niets binnenkwam,
   * en precies dat maakte mijn eerste poging waardeloos. */
  bool done = (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED);
  time_t t = time(NULL);

  if (done && (uint32_t) t >= TIME_FLOOR) {
    uint32_t curr = rtc_clock.getCurrentTime();
    if ((uint32_t) t > curr) {
      rtc_clock.setCurrentTime((uint32_t) t);
      snprintf(_sync_msg, sizeof(_sync_msg), "gezet via %s", ntpServer());
      WIFI_LOG("klok van %s gezet op %lu (was %lu)\n",
               ntpServer(), (unsigned long) t, (unsigned long) curr);
    } else {
      snprintf(_sync_msg, sizeof(_sync_msg), "eigen klok liep al goed");
    }
    _time_synced = true;
    _synced_at = millis();
    _sntp_deadline = 0;
  } else if ((long)(millis() - _sntp_deadline) >= 0) {
    snprintf(_sync_msg, sizeof(_sync_msg), "geen antwoord van %s", ntpServer());
    WIFI_LOG("geen antwoord van de tijdserver %s; klok blijft zoals hij was\n",
             ntpServer());
    _sntp_deadline = 0;
  }
}

void WifiTask::loop() {
  unsigned long now = millis();
  checkTimeSync();

  /* Wegvallen opvangen, ongeacht in welke toestand we dachten te zitten. */
  if (g_dropped) {
    g_dropped = false;
    /* Alleen een reden onthouden als we werkelijk online waren. Tijdens het
     * associeren vuurt de ESP32 al een DISCONNECTED-event af met reden 202; dat
     * bewaren gaf "online (reden 202)" in het log en reason=202 in status.json
     * terwijl er niets aan de hand was. */
    if (_state == ONLINE) {
      _last_reason = g_last_reason;
      setState(RETRYING);
      _fails = 0;
      _next_action = now + RETRY_DELAY_MS;
    }
  }

  switch (_state) {
    case CONNECTING:
      if (g_got_ip || WiFi.status() == WL_CONNECTED) {
        _fails = 0; _reconnects++;
        setState(ONLINE);
        startTimeSync();
        WIFI_LOG("verbonden, rssi %d, ip %s\n",
                 WiFi.RSSI(), WiFi.localIP().toString().c_str());
      } else if ((long)(now - _next_action) >= 0) {
        _fails++;
        if (_fails >= FAILS_BEFORE_AP) { startAP(); }
        else if (_fails >= FAILS_BEFORE_RESET) { hardReset(); }
        else { setState(RETRYING); _next_action = now + RETRY_DELAY_MS; }
      }
      break;

    case RETRYING:
      if ((long)(now - _next_action) >= 0) startConnect();
      break;

    case ONLINE:
      /* Waakhond ook hier: WiFi.status() kan CONNECTED beweren terwijl er geen
       * verkeer meer door gaat. Zodra de status niet meer klopt, opnieuw. */
      if (WiFi.status() != WL_CONNECTED) {
        setState(RETRYING);
        _next_action = now + RETRY_DELAY_MS;
      }
      break;

    case AP_MODE:
      /* Blijft in AP tot iemand instelt en herstart. Zelf terugvallen naar STA
       * zou de enige weg naar de instellingen onder je handen weghalen. */
      break;

    case OFF:
      break;
  }

  /* De echte waakhond: te lang niet online, wat de toestand ook zegt. */
  if (_state != ONLINE && _state != AP_MODE && secsInState() * 1000UL > WATCHDOG_MS) {
    hardReset();
  }
}
