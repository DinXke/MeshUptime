#include "MuBattery.h"

// >>> BATTERY CURVE SEAM <<<
// PLACEHOLDER linear map, 3300 mV = 0 %, 4200 mV = 100 %. Replace the body of
// this one function with the corrected LiPo discharge curve when it lands.
int battery_percent_from_mv(uint16_t mv) {
  if (mv == 0) return -1;            // unknown / no reading
  const int lo = 3300, hi = 4200;
  if (mv <= lo) return 0;
  if (mv >= hi) return 100;
  return (int)(((long)(mv - lo) * 100) / (hi - lo));
}
