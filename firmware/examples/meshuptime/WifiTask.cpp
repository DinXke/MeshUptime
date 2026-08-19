#include "WifiTask.h"
#include <WiFi.h>

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

void WifiTask::loop() {
  unsigned long now = millis();

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
