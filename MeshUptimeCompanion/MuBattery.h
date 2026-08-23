#pragma once
#include <Arduino.h>

// SINGLE SEAM for the battery %-from-millivolts mapping.
//
// The raw millivolt reading comes from the board (T1000eBoard::getBattMilliVolts,
// unchanged). A separate effort is analysing the "50-60% then off" discharge-curve
// bug; when the corrected curve is ready it should be dropped in HERE and nowhere
// else. This is a deliberately dumb linear placeholder — do not treat its output
// as accurate.
int battery_percent_from_mv(uint16_t mv);
