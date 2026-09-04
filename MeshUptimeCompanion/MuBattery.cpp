#include "MuBattery.h"

// >>> BATTERY CURVE SEAM <<<
// Corrected LiPo discharge curve for the T1000-E cell (from analysis-agent
// a17c9acb, see battery_fix_t1000e.md). Piecewise-linear interpolation between
// measured (mV, %) knees. Replaces the old naive linear 3300..4200 map that made
// the app "stick at 50-60% then cliff". Curve is for light load / rest; under
// heavy LoRa-TX or GPS the voltage sags transiently -> validate with an on-device
// discharge log.
int battery_percent_from_mv(uint16_t mv) {
  if (mv == 0) return -1;            // unknown / no reading
  static const struct { uint16_t mv; uint8_t pct; } curve[] = {
    {3300,0},{3450,5},{3500,10},{3550,15},{3600,20},{3650,30},{3700,40},{3750,50},
    {3800,60},{3850,70},{3900,80},{3950,85},{4000,90},{4050,95},{4100,98},{4150,99},{4200,100}
  };
  const int n = (int)(sizeof(curve) / sizeof(curve[0]));
  if (mv <= curve[0].mv)   return curve[0].pct;
  if (mv >= curve[n-1].mv) return curve[n-1].pct;
  for (int i = 1; i < n; i++) {
    if (mv <= curve[i].mv) {
      uint16_t lo = curve[i-1].mv, hi = curve[i].mv;
      uint8_t  lp = curve[i-1].pct, hp = curve[i].pct;
      return lp + (int)((uint32_t)(mv - lo) * (hp - lp) / (hi - lo));
    }
  }
  return 100;
}

uint16_t battery_app_mv(uint16_t real_mv) {
  int pct = battery_percent_from_mv(real_mv);
  if (pct < 0) return real_mv;       // unknown -> pass through unchanged
  // Invert the app's own linear formula so it recovers our true %.
  long app_mv = 3000L + (long)pct * 12L;
  if (app_mv < 3000) app_mv = 3000;
  if (app_mv > 4200) app_mv = 4200;
  return (uint16_t)app_mv;
}
