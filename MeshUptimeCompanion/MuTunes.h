#pragma once
// MeshUptimeCompanion — default RTTTL ringtones (Super Mario) and LED blink
// patterns per severity. All strings are standard RTTTL and parse with
// end2endzone/NonBlockingRTTTL. They are only defaults: every slot can be
// overwritten at runtime with `!tune <slot> <RTTTL>` and is then persisted.

#include <Arduino.h>

// --- Default melodies -------------------------------------------------------
// High / danger  -> Mario "death" jingle (game over feel)
#define MU_DEF_TUNE_HIGH \
  "mariodie:d=4,o=5,b=180:32c6,32c6,32c6,8p,16b5,16f6,16p,16f6,16f.6,16e.6,16d.6,16c6"

// Medium / warning -> descending Mario "power-down" motif
#define MU_DEF_TUNE_MED \
  "marioword:d=4,o=5,b=160:16e6,16c6,16g5,8p,16e6,16c6,16g5,8p,16d6,16a5,16f5"

// Low / recovery -> Mario "1-up" (good news)
#define MU_DEF_TUNE_LOW \
  "mario1up:d=4,o=5,b=250:8e6,8g6,8e7,8c7,8d7,8g7"

// Find-me -> Mario main theme (long, loops well)
#define MU_DEF_TUNE_FIND \
  "smb:d=4,o=5,b=200:8e6,8e6,8p,8e6,8p,8c6,8e6,8p,8g6,8p,8p,8g,8p,8p,8c6,8p,8g,8p,8e,8p,8a,8b,8a#,8a,8g6,8e6,8g6,8a6,8f6,8g6,8p,8e6,8c6,8d6,8b,8p"

// Message -> Mario "coin" (short)
#define MU_DEF_TUNE_MSG \
  "coin:d=4,o=6,b=180:16b5,8e6"

// --- LED blink patterns -----------------------------------------------------
// Each pattern is a list of ON/OFF durations in ms, starting with ON. The
// engine plays it once alongside the tune (find-me repeats it while looping).
struct MuLedPattern {
  const uint16_t* steps;
  uint8_t         count;
};

static const uint16_t MU_LED_HIGH[]  = {80,80,80,80,80,80,80,600};   // frantic burst
static const uint16_t MU_LED_MED[]   = {150,150,150,600};            // double blink
static const uint16_t MU_LED_LOW[]   = {400,200,400,600};            // calm double
static const uint16_t MU_LED_FIND[]  = {60,940};                     // slow beacon
static const uint16_t MU_LED_MSG[]   = {60,120};                     // single tick

static inline MuLedPattern mu_led_pattern(int tune_slot) {
  switch (tune_slot) {
    case 0:  return { MU_LED_HIGH, (uint8_t)(sizeof(MU_LED_HIGH)/2) };
    case 1:  return { MU_LED_MED,  (uint8_t)(sizeof(MU_LED_MED)/2)  };
    case 2:  return { MU_LED_LOW,  (uint8_t)(sizeof(MU_LED_LOW)/2)  };
    case 3:  return { MU_LED_FIND, (uint8_t)(sizeof(MU_LED_FIND)/2) };
    default: return { MU_LED_MSG,  (uint8_t)(sizeof(MU_LED_MSG)/2)  };
  }
}
