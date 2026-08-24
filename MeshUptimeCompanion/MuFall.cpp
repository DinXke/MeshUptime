#include "MeshUptime.h"
#include "QMA6100P.h"
#include "MyMesh.h"                 // sensors
#include <math.h>

// Detection tuning. The free-fall + impact thresholds now come from the active
// sensitivity preset (mu_cfg.fall_sens) instead of fixed #defines, so they are
// runtime-configurable (`!fall sens low|med|high`). The rest stay fixed.
#define FALL_SAMPLE_MS       40      // ~25 Hz
#define FF_WINDOW_MS         1200    // impact must follow free-fall within this
#define MOTION_DELTA_G       0.20f   // |mag-1g| above this = "moving"
#define PREALARM_REBEEP_MS   4000UL

// Sensitivity presets: {free-fall threshold (g), sustained free-fall (ms),
// impact spike (g)}. HIGH = easier to trigger (higher FF gate, shorter dwell,
// lower impact); LOW = harder; MED = the original conservative defaults. Still
// placeholders that want on-device tuning.
struct MuFallSens { float ff_g; uint16_t ff_min_ms; float impact_g; };
static const MuFallSens MU_FALL_SENS_TAB[3] = {
  { 0.40f, 90, 3.0f },   // MU_FALL_SENS_LOW
  { 0.50f, 70, 2.5f },   // MU_FALL_SENS_MED  (original defaults)
  { 0.60f, 50, 2.0f },   // MU_FALL_SENS_HIGH
};
static inline const MuFallSens& mu_fall_cur_sens() {
  uint8_t s = mu_cfg.fall_sens;
  if (s > MU_FALL_SENS_HIGH) s = MU_FALL_SENS_MED;
  return MU_FALL_SENS_TAB[s];
}
static inline unsigned long mu_fall_prealarm_ms() {
  uint16_t s = mu_cfg.fall_prealarm_sec;
  if (s < 5)   s = 5;
  if (s > 120) s = 120;
  return (unsigned long)s * 1000UL;
}

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
bool mu_fall_accel_present() { return accel.isPresent(); }

static void enter_prealarm(bool was_fall) {
  if (prealarm) return;
  prealarm = true;
  trigger_was_fall = was_fall;
  prealarm_deadline = millis() + mu_fall_prealarm_ms();
  prealarm_next_beep = millis();
  ff_start = 0;
}

// Desk test: run the pre-alarm sequence NOW (buzzer + button-cancel window)
// exactly like a real fall, without needing an accelerometer signature.
void mu_fall_test() { enter_prealarm(true); }

void mu_fall_cancel() {
  if (!prealarm) return;
  prealarm = false;
  mu_stop_all();
  last_motion = millis();      // treat the cancelling press as motion
}

static void fire_alert() {
  // Machine-readable "#LOC" report to every configured direct target (+ the node
  // bot when `!fall mm on`, else fall back to sos/target). See mu_send_alert_report.
  mu_send_alert_report(trigger_was_fall ? "(val) VAL gedetecteerd"
                                        : "(geen beweging) GEEN BEWEGING");
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

  // Thresholds from the active sensitivity preset (runtime-configurable).
  const MuFallSens& sens = mu_fall_cur_sens();

  // --- motion tracking (for the dead-man timer) ---
  if (fabsf(mag - 1.0f) > MOTION_DELTA_G) last_motion = millis();

  // --- free-fall + impact signature ---
  if (mag < sens.ff_g) {
    if (ff_start == 0) ff_start = millis();
    if (millis() - ff_start >= sens.ff_min_ms) ff_seen_until = millis() + FF_WINDOW_MS;
  } else {
    ff_start = 0;
  }
  if (ff_seen_until && (int32_t)(millis() - ff_seen_until) < 0) {
    if (mag > sens.impact_g) { ff_seen_until = 0; enter_prealarm(true); return; }
  }

  // --- no-motion / dead-man ---
  if (mu_cfg.fall_nomotion_min > 0) {
    unsigned long limit = (unsigned long)mu_cfg.fall_nomotion_min * 60000UL;
    if (millis() - last_motion >= limit) { enter_prealarm(false); return; }
  }
}
