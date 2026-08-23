#pragma once
#include <Arduino.h>

// SINGLE SEAM for the battery %-from-millivolts mapping.
//
// The raw millivolt reading comes from the board (T1000eBoard::getBattMilliVolts,
// unchanged). `battery_percent_from_mv()` maps that raw voltage to a TRUE LiPo
// percentage using a corrected discharge curve (see battery_fix_t1000e.md). This
// is used for ALL of our OWN outputs (DM / !cfg / serial / menu): they report the
// real voltage AND the true %.
int battery_percent_from_mv(uint16_t mv);   // -1 = unknown (mv==0)

// APP-COMPAT ENCODER (see PROTOCOL.md §7).
//
// The official MeshCore companion app derives the battery % from the raw mV it
// receives with its own fixed linear formula:  pct = (mv - 3000) * 100 / 1200.
// That formula is wrong for a real LiPo (the "sticks at 50-60% then cliffs" bug).
// To make the *app* display the correct %, we feed the standard battery field a
// "virtual" millivolt value that, run through the app's own linear formula,
// yields our true curve-based %:
//
//     app_mv = 3000 + true_percent * 12     (clamped to [3000, 4200])
//
// This changes ONLY the value in the standard app-facing battery field, never the
// frame STRUCTURE — it is still a plain uint16 mV. Our own outputs keep the REAL
// voltage. Trade-off: if the app shows raw voltage anywhere, it shows this encoded
// value, but the derived % is correct.
uint16_t battery_app_mv(uint16_t real_mv);
