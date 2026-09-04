#pragma once
// MeshUptimeCompanion — built-in named RTTTL tune library + default per-severity
// mapping + LED blink patterns.
//
// All strings are standard RTTTL. The default per-slot melodies stay Super-Mario
// themed and are drawn from the library below. Every slot can still be overwritten
// at runtime with `!tune <slot> <RTTTL>` (paste) or `!tune <slot> preset <name>`
// (pick from this library), and is then persisted.

#include <Arduino.h>

// --- Named library ----------------------------------------------------------
// {name, rtttl}. Names are matched case-insensitively by `!play`/`!tune preset`.
// Kept short so a `tunes` listing fits comfortably over a DM.
struct MuTune {
  const char* name;
  const char* rtttl;
};

static const MuTune MU_TUNES[] = {
  // Mario-themed
  { "mario-main", "smb:d=4,o=5,b=200:8e6,8e6,8p,8e6,8p,8c6,8e6,8p,8g6,8p,8p,8g,8p,8p,8c6,8p,8g,8p,8e,8p,8a,8b,8a#,8a,8g6,8e6,8g6,8a6,8f6,8g6,8p,8e6,8c6,8d6,8b,8p" },
  { "mario-die",  "mariodie:d=4,o=5,b=180:32c6,32c6,32c6,8p,16b5,16f6,16p,16f6,16f.6,16e.6,16d.6,16c6" },
  { "mario-1up",  "mario1up:d=4,o=5,b=250:8e6,8g6,8e7,8c7,8d7,8g7" },
  { "mario-warn", "marioword:d=4,o=5,b=160:16e6,16c6,16g5,8p,16e6,16c6,16g5,8p,16d6,16a5,16f5" },
  { "coin",       "coin:d=4,o=6,b=180:16b5,8e6" },
  { "powerup",    "powerup:d=4,o=5,b=180:16g4,16b4,16d5,16g5,16b5,8d6,8b5,16gs4,16c5,16ds5,16gs5,16c6,8ds6,8c6" },
  { "warning",    "warn:d=4,o=6,b=200:16c,16p,16c,16p,16c,16p,8c" },
  // Neutral
  { "chime",      "chime:d=4,o=6,b=140:8e,8g,8c7" },
  { "alert",      "alert:d=4,o=6,b=200:16a,16p,16a,16p,16a" },
  { "beep",       "beep:d=4,o=6,b=240:16c7" },
};

static const int MU_TUNES_COUNT = (int)(sizeof(MU_TUNES) / sizeof(MU_TUNES[0]));

// Case-insensitive name lookup. Returns index or -1.
static inline int mu_tune_index_by_name(const char* name) {
  for (int i = 0; i < MU_TUNES_COUNT; i++) {
    const char* a = MU_TUNES[i].name; const char* b = name;
    bool eq = true;
    while (*a && *b) {
      char ca = *a, cb = *b;
      if (ca >= 'A' && ca <= 'Z') ca += 32;
      if (cb >= 'A' && cb <= 'Z') cb += 32;
      if (ca != cb) { eq = false; break; }
      a++; b++;
    }
    if (eq && *a == 0 && *b == 0) return i;
  }
  return -1;
}

// Resolve "<name|1-based-nr>" to a library index, or -1.
static inline int mu_tune_resolve(const char* token) {
  if (!token || !token[0]) return -1;
  bool all_digits = true;
  for (const char* p = token; *p; p++) if (*p < '0' || *p > '9') { all_digits = false; break; }
  if (all_digits) {
    int nr = atoi(token);
    if (nr >= 1 && nr <= MU_TUNES_COUNT) return nr - 1;
    return -1;
  }
  return mu_tune_index_by_name(token);
}

// --- Default per-severity slot melodies (drawn from the library above) ------
#define MU_DEF_TUNE_HIGH   MU_TUNES[1].rtttl   // mario-die  (danger / game over)
#define MU_DEF_TUNE_MED    MU_TUNES[3].rtttl   // mario-warn (power-down motif)
#define MU_DEF_TUNE_LOW    MU_TUNES[2].rtttl   // mario-1up  (good news)
#define MU_DEF_TUNE_FIND   MU_TUNES[0].rtttl   // mario-main (long, loops well)
#define MU_DEF_TUNE_MSG    MU_TUNES[4].rtttl   // coin       (short tick)
#define MU_DEF_TUNE_LOWBAT MU_TUNES[6].rtttl   // warning    (low-battery notice)

// --- LED blink patterns -----------------------------------------------------
// Each pattern is a list of ON/OFF durations in ms, starting with ON. The engine
// plays it once alongside the tune (find-me repeats it while looping).
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
