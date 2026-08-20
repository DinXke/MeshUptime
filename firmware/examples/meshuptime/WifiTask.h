#pragma once

#include <Arduino.h>

/* WiFi voor MeshUptime.
 *
 * In een eigen bestand, en niet in main.cpp, omdat main.cpp zo dicht mogelijk
 * bij upstream simple_sensor moet blijven: elke regel die we daar veranderen
 * moet bij een nieuwe MeshCore-versie opnieuw aangebracht worden. Dat heeft in
 * MeshStats tijd gekost en dat hoeft niet nog eens.
 *
 * De waakhond hieronder is geen voorzorg maar een reparatie van gemeten gedrag.
 * Op 19 augustus 2026 raakte de node op batterij van het netwerk met
 * 'WiFi disconnected (reason 202)' -- WIFI_REASON_AUTH_FAIL -- bij een rssi van
 * -52, dus niet door zwak bereik. Hij kwam er NIET uit: ook nadat de USB er weer
 * in zat bleef hij minuten onbereikbaar, en pas een harde reset herstelde de
 * verbinding. De meetreeks staat in docs/meting-voeding-2026-08-19.log.
 *
 * WiFi.reconnect() is daarom niet genoeg. Na een aanhoudende storing moet de
 * radio volledig omlaag en opnieuw op, en lukt dat ook niet, dan een eigen
 * toegangspunt zodat het toestel bereikbaar blijft om in te stellen.
 */
class WifiTask {
public:
  enum State : uint8_t { OFF, CONNECTING, ONLINE, RETRYING, AP_MODE };

  void begin(const char* ssid, const char* pwd);
  void loop();

  bool isOnline() const { return _state == ONLINE; }
  bool isApMode() const { return _state == AP_MODE; }
  State state() const { return _state; }

  /* Voor de sensoren: hoe lang staat WiFi al (niet) op, in seconden. Nul zodra
   * de toestand net gewisseld is. */
  uint32_t secsInState() const;

  /* Laatste reden van wegvallen, zoals de ESP32 hem geeft. 202 is AUTH_FAIL.
   * Wordt in de webinterface getoond, want 'geen wifi' zonder reden kost
   * iemand een avond zoeken. */
  uint8_t lastDisconnectReason() const { return _last_reason; }

  /* Klok van internet. De ESP32 heeft geen batterijgevoede klok, dus na elke
   * herstart staat hij op 15 mei 2024 -- de vaste terugvalwaarde in
   * CommonCLI.cpp:188. Dat is niet cosmetisch: MeshCore gebruikt tijdstempels
   * voor herhaalbeveiliging, en telemetrie, waarschuwingsleeftijden en de
   * verouderingsregel voor gemelde diensten hangen er allemaal aan. Een node
   * die zijn eigen tijd niet weet, liegt over wanneer iets gebeurde.
   *
   * Alleen VOORUIT zetten, nooit achteruit: dezelfde regel die 'time <epoch>'
   * en 'clock sync' aanhouden, en om dezelfde reden -- een klok die terugloopt
   * maakt van geldige pakketten opeens herhalingen. */
  bool timeSynced() const { return _time_synced; }

  /* Beheer van de tijdsynchronisatie, voor de webinterface.
   *
   * De server is instelbaar omdat een node zonder internet maar met een eigen
   * router vaak wel een tijdbron op het LAN heeft, en pool.ntp.org dan nooit
   * antwoordt. syncNow() is er omdat wachten tot de volgende herverbinding geen
   * antwoord is als je nu wil weten of het werkt. */
  void        syncNow();
  void        setNtpServer(const char* host);
  const char* ntpServer() const { return _ntp[0] ? _ntp : "pool.ntp.org"; }
  const char* lastSyncMsg() const { return _sync_msg; }
  uint32_t    secsSinceSync() const { return _synced_at ? (millis() - _synced_at) / 1000 : 0; }

  uint32_t reconnectCount() const { return _reconnects; }
  uint32_t hardResetCount()  const { return _hard_resets; }

private:
  State _state = OFF;
  unsigned long _state_since = 0;
  unsigned long _next_action = 0;
  uint8_t  _last_reason = 0;
  uint8_t  _fails = 0;         // mislukte pogingen achter elkaar
  uint32_t _reconnects = 0;
  uint32_t _hard_resets = 0;
  bool     _time_synced = false;
  unsigned long _sntp_deadline = 0;
  char     _ntp[48] = {0};
  char     _sync_msg[64] = {0};
  unsigned long _synced_at = 0;
  void startTimeSync();
  void checkTimeSync();

  /* Vaste buffers: dit apparaat alloceert niets op de heap voor configuratie.
   * 33 en 65 zijn de maxima uit de 802.11-norm plus afsluiter. */
  char _ssid[33] = {0};
  char _pwd[65]  = {0};

  void setState(State s);
  void startConnect();
  void hardReset();
  void startAP();
};
