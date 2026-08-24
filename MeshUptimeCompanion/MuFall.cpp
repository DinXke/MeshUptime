#include "MeshUptime.h"
#include "QMA6100P.h"
#include "MyMesh.h"                 // sensors
#include <math.h>

// Detection tuning. The free-fall + impact thresholds now come from the active
// sensitivity preset (mu_cfg.fall_sens) instead of fixed #defines, so they are
// runtime-configurable (`!fall sens low|med|high`). The rest stay fixed.
#define FALL_SAMPLE_MS       40      // ~25 Hz (only during a motion burst / poll)
#define FF_WINDOW_MS         1200    // impact must follow free-fall within this
#define MOTION_DELTA_G       0.20f   // |mag-1g| above this = "moving"
#define PREALARM_REBEEP_MS   4000UL

// --- interrupt-driven sampling -----------------------------------------------
// Idle behaviour: the QMA6100P runs a low-power any-motion interrupt and the
// firmware does NOT poll. When the INT fires (there is movement) we open a short
// SAMPLING BURST during which the fine free-fall+impact detector runs at
// FALL_SAMPLE_MS; if no fall is seen we drop back to motion-wait (zero I2C).
// This keeps the existing algorithm intact but only runs it while moving, which
// both removes the 40 ms bus traffic from the idle loop (no stall exposure) and
// lets the CPU light-sleep between movements (battery).
#define MU_FALL_BURST_MS     1800UL  // fine-sampling window after a motion INT

// Auto-disable after this many CONSECUTIVE I2C read failures: a flaky/absent
// sensor can then never keep costing bus transactions or degrade the device.
#define MU_FALL_MAX_READ_FAIL 10

#ifndef QMA_6100P_INT_PIN
  // Variant without a wired INT pin -> stay in polling mode (still timeout-safe).
  #define MU_FALL_NO_INT_PIN
#endif

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

// Set by the ISR ONLY (no I2C, no allocation) — a bare volatile flag.
static volatile bool s_motion_irq = false;

enum { MU_FALL_MODE_POLL = 0, MU_FALL_MODE_INT = 1 };
static uint8_t mode      = MU_FALL_MODE_POLL;   // safe default; INT when confirmed
static bool    faulted   = false;               // auto-disabled after read failures
static uint8_t read_fail = 0;                   // consecutive I2C read failures

static unsigned long next_sample = 0;
static unsigned long burst_until = 0;      // INT mode: fine-sample until this (0=idle)
static unsigned long ff_start = 0;         // when current free-fall began (0=none)
static unsigned long ff_seen_until = 0;    // deadline to see an impact after free-fall
static unsigned long last_motion = 0;

static bool          prealarm = false;
static unsigned long prealarm_deadline = 0;
static unsigned long prealarm_next_beep = 0;
static bool          trigger_was_fall = true;

#ifndef MU_FALL_NO_INT_PIN
static void mu_fall_isr() { s_motion_irq = true; }   // flag only — never touch I2C here
#endif

void mu_fall_begin() {
  accel.begin();               // safe even if absent; module stays inert
  last_motion = millis();
  mode = MU_FALL_MODE_POLL;    // conservative default

#ifndef MU_FALL_NO_INT_PIN
  // Try the low-power motion interrupt. Only switch to INT mode when the sensor
  // confirmed the config (read-back verified); otherwise stay in poll mode so a
  // flaky/mis-guessed motion engine degrades to "poll with timeout-safe reads"
  // rather than "no detection".
  if (accel.isPresent() && accel.configMotionInterrupt()) {
    pinMode(QMA_6100P_INT_PIN, INPUT);   // QMA drives push-pull active-high
    attachInterrupt(digitalPinToInterrupt(QMA_6100P_INT_PIN), mu_fall_isr, RISING);
    s_motion_irq = false;
    mode = MU_FALL_MODE_INT;
  }
#endif
}

bool mu_fall_prealarm_active() { return prealarm; }
bool mu_fall_accel_present() { return accel.isPresent() && !faulted; }

// Diagnostics for the serial-capture / `!fall status`.
const char* mu_fall_mode_str() {
  if (faulted) return "fault";
  return (mode == MU_FALL_MODE_INT) ? "int" : "poll";
}
// True only while a motion-triggered fine-sampling burst is running, so the main
// loop stays awake at the 40 ms cadence for that short window (see mu_wants_cpu).
bool mu_fall_sampling_active() {
  return mode == MU_FALL_MODE_INT && burst_until != 0 &&
         (int32_t)(millis() - burst_until) < 0;
}

static void enter_prealarm(bool was_fall) {
  if (prealarm) return;
  prealarm = true;
  trigger_was_fall = was_fall;
  prealarm_deadline = millis() + mu_fall_prealarm_ms();
  prealarm_next_beep = millis();
  ff_start = 0;
  burst_until = 0;
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

// One I2C sample with fault accounting. Returns false (and skips) on any read
// error; after MU_FALL_MAX_READ_FAIL consecutive failures the whole module
// auto-disables (logged once) so a bad sensor can't keep hammering the bus.
static bool sample_once(float& mag) {
  float x, y, z;
  if (!accel.read(x, y, z)) {
    if (read_fail < 255) read_fail++;
    if (!faulted && read_fail >= MU_FALL_MAX_READ_FAIL) {
      faulted = true;
      burst_until = 0;
#ifndef MU_FALL_NO_INT_PIN
      if (mode == MU_FALL_MODE_INT)
        detachInterrupt(digitalPinToInterrupt(QMA_6100P_INT_PIN));
#endif
      Serial.println("FALL: accelerometer I2C faalt herhaaldelijk -> val-detectie automatisch uit");
    }
    return false;
  }
  read_fail = 0;
  mag = sqrtf(x * x + y * y + z * z);
  return true;
}

// Run the existing free-fall + impact signature on one magnitude sample.
static void process_sample(float mag) {
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

  if (!mu_cfg.fall_enabled || faulted || !accel.isPresent()) return;

  // --- no-motion / dead-man (mode-independent, NO I2C) ---
  // In INT mode the any-motion interrupt is the motion signal, so "no motion"
  // is simply the absence of INTs for the configured time — this needs zero bus
  // traffic while the person is still (asleep / fallen). In poll mode last_motion
  // is refreshed by the samples below. Chosen over a QMA no-motion interrupt for
  // simplicity/robustness (one INT source, no extra unvalidated registers).
  if (mu_cfg.fall_nomotion_min > 0) {
    unsigned long limit = (unsigned long)mu_cfg.fall_nomotion_min * 60000UL;
    if (millis() - last_motion >= limit) { enter_prealarm(false); return; }
  }

#ifndef MU_FALL_NO_INT_PIN
  if (mode == MU_FALL_MODE_INT) {
    // A motion INT (edge caught while awake or having woken us from light-sleep)
    // opens / extends a fine-sampling burst. Idle => no sampling, no I2C.
    if (s_motion_irq) {
      s_motion_irq = false;
      last_motion = millis();
      burst_until = millis() + MU_FALL_BURST_MS;
    }
    if (burst_until == 0 || (int32_t)(millis() - burst_until) >= 0) {
      burst_until = 0;                 // burst over -> back to motion-wait
      return;
    }
    // inside an active burst -> fall through to the fine sampler
  }
#endif

  // Poll mode samples continuously; INT mode only during a burst (reached above).
  if ((int32_t)(millis() - next_sample) < 0) return;
  next_sample = millis() + FALL_SAMPLE_MS;

  float mag;
  if (!sample_once(mag)) return;       // skip on I2C error (may auto-disable)
  process_sample(mag);
}
