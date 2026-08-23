#include "MeshUptime.h"
#include "QMA6100P.h"
#include "MyMesh.h"                 // sensors
#include <math.h>

// Detection tuning — CONSERVATIVE placeholders; needs on-device tuning.
#define FALL_SAMPLE_MS       40      // ~25 Hz
#define FF_THRESH_G          0.50f   // free-fall: magnitude below this
#define FF_MIN_MS            70      // sustained free-fall duration
#define FF_WINDOW_MS         1200    // impact must follow within this
#define IMPACT_THRESH_G      2.5f    // impact spike
#define MOTION_DELTA_G       0.20f   // |mag-1g| above this = "moving"
#define PREALARM_MS          30000UL // 30 s button-cancel window
#define PREALARM_REBEEP_MS   4000UL

static QMA6100P accel;

static unsigned long next_sample = 0;
static unsigned long ff_start = 0;         // when current free-fall began (0=none)
static unsigned long ff_seen_until = 0;    // deadline to see an impact after free-fall
static unsigned long last_motion = 0;

static bool          prealarm = false;
static unsigned long prealarm_deadline = 0;
static unsigned long prealarm_next_beep = 0;
static bool          trigger_was_fall = true;

void mu_fall_begin() {
  accel.begin();               // safe even if absent; module stays inert
  last_motion = millis();
}

bool mu_fall_prealarm_active() { return prealarm; }

static void enter_prealarm(bool was_fall) {
  if (prealarm) return;
  prealarm = true;
  trigger_was_fall = was_fall;
  prealarm_deadline = millis() + PREALARM_MS;
  prealarm_next_beep = millis();
  ff_start = 0;
}

void mu_fall_cancel() {
  if (!prealarm) return;
  prealarm = false;
  mu_stop_all();
  last_motion = millis();      // treat the cancelling press as motion
}

static void fire_alert() {
  const uint8_t* dest = mu_cfg.has_sos ? mu_cfg.sos_pub
                      : (mu_cfg.has_target ? mu_cfg.target_pub : nullptr);
  if (dest) {
    mu_send_dm(dest, trigger_was_fall ? "VAL gedetecteerd" : "GEEN BEWEGING", true);
  }
  prealarm = false;
  mu_stop_all();
  last_motion = millis();
}

void mu_fall_loop() {
  // Drive the pre-alarm countdown regardless of sampling cadence.
  if (prealarm) {
    if ((int32_t)(millis() - prealarm_next_beep) >= 0) {
      mu_play_tune(MU_TUNE_HIGH);     // audible + LED; button cancels
      prealarm_next_beep = millis() + PREALARM_REBEEP_MS;
    }
    if ((int32_t)(millis() - prealarm_deadline) >= 0) fire_alert();
    return;                            // don't run detection while alarming
  }

  if (!mu_cfg.fall_enabled || !accel.isPresent()) return;
  if ((int32_t)(millis() - next_sample) < 0) return;
  next_sample = millis() + FALL_SAMPLE_MS;

  float x, y, z;
  if (!accel.read(x, y, z)) return;
  float mag = sqrtf(x * x + y * y + z * z);

  // --- motion tracking (for the dead-man timer) ---
  if (fabsf(mag - 1.0f) > MOTION_DELTA_G) last_motion = millis();

  // --- free-fall + impact signature ---
  if (mag < FF_THRESH_G) {
    if (ff_start == 0) ff_start = millis();
    if (millis() - ff_start >= FF_MIN_MS) ff_seen_until = millis() + FF_WINDOW_MS;
  } else {
    ff_start = 0;
  }
  if (ff_seen_until && (int32_t)(millis() - ff_seen_until) < 0) {
    if (mag > IMPACT_THRESH_G) { ff_seen_until = 0; enter_prealarm(true); return; }
  }

  // --- no-motion / dead-man ---
  if (mu_cfg.fall_nomotion_min > 0) {
    unsigned long limit = (unsigned long)mu_cfg.fall_nomotion_min * 60000UL;
    if (millis() - last_motion >= limit) { enter_prealarm(false); return; }
  }
}
