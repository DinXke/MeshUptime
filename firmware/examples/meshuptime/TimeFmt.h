#pragma once

/* ============================================================================
 * TimeFmt -- menselijke tijdweergave in LOKALE tijd, protocol/opslag in UTC.
 *
 * MeshCore's protocol (adverts, herhaalbeveiliging, berichtvolgorde) en de RTC
 * draaien in UTC -- dat mag NIET veranderen. Maar een mens die "Received at" leest
 * wil zijn eigen kloktijd zien, met de juiste zone-afkorting (CET in de winter,
 * CEST in de zomer). Deze helper zet een UTC-epoch om naar lokale tijd via de
 * INGESTELDE tijdzone (een POSIX-TZ-string die elders met setenv("TZ",...)+tzset()
 * is toegepast), zodat opslag UTC blijft en alleen de WEERGAVE lokaal is.
 * ==========================================================================*/

#include <Arduino.h>
#include <time.h>

/* Ondergrens waaronder de klok niet echt kan zijn: vóór een geslaagde NTP-sync
 * staat de RTC op de vaste terugval (15 mei 2024, CommonCLI.cpp). Onder deze
 * drempel tonen we GEEN misleidende tijd. 1 januari 2025 UTC -- na elke
 * terugvalwaarde in deze broncode en voor elke echte tijd. Zelfde waarde als
 * WifiTask::TIME_FLOOR (daar lokaal, hier gedeeld voor de weergave). */
#ifndef TIME_FLOOR
  #define TIME_FLOOR   1735689600UL
#endif

/* Standaarden: Europe/Brussels (DST-bewust) en pool.ntp.org. Instelbaar via de
 * web-GUI (/time), bewaard in /time.cfg. */
#ifndef DEFAULT_TZ
  #define DEFAULT_TZ   "CET-1CEST,M3.5.0/2,M10.5.0/3"
#endif
#ifndef DEFAULT_NTP
  #define DEFAULT_NTP  "pool.ntp.org"
#endif

/* UTC-epoch -> lokale "HH:MM:SS ZZZ" (bv. "00:35:11 CEST") via de ingestelde TZ.
 * Onder TIME_FLOOR (niet gesynct): "--:--:-- (niet gesynct)" -- nooit een
 * garbage-tijd. Retour: het aantal geschreven tekens. */
static inline int fmtLocalHMS(uint32_t utc, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) return 0;
  if (utc < TIME_FLOOR) {
    return snprintf(out, out_len, "--:--:-- (niet gesynct)");
  }
  time_t t = (time_t)utc;
  struct tm tmv;
  localtime_r(&t, &tmv);
  int n = (int)strftime(out, out_len, "%H:%M:%S %Z", &tmv);
  if (n <= 0 && out_len > 0) out[0] = 0;
  return n;
}

/* Volledige lokale datum+tijd "YYYY-MM-DD HH:MM:SS ZZZ", voor de web-GUI. */
static inline int fmtLocalDateTime(uint32_t utc, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) return 0;
  if (utc < TIME_FLOOR) {
    return snprintf(out, out_len, "niet gesynct");
  }
  time_t t = (time_t)utc;
  struct tm tmv;
  localtime_r(&t, &tmv);
  int n = (int)strftime(out, out_len, "%Y-%m-%d %H:%M:%S %Z", &tmv);
  if (n <= 0 && out_len > 0) out[0] = 0;
  return n;
}

/* De tijdzone toepassen op het proces (localtime_r/strftime gebruiken de TZ-env).
 * Eenmalig bij boot en bij elke wijziging aanroepen. tz leeg -> de standaard. */
static inline void applyTimeZone(const char* tz) {
  setenv("TZ", (tz && tz[0]) ? tz : DEFAULT_TZ, 1);
  tzset();
}
