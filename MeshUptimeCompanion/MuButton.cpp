#include "MeshUptime.h"
#include "MyMesh.h"                 // board, sensors

#define MU_LONGPRESS_MS 2000

static unsigned long press_t0 = 0;
static bool press_consumed = false;

void mu_button_begin() {
  // Prime the board's edge detector so the very first real press is a clean edge.
  board.buttonStateChanged();
}

static void do_short_press() {
  // Short press -> send preset #1 ("ok/ack") to the configured target.
  if (mu_cfg.has_target) {
    mu_send_dm(mu_cfg.target_pub, mu_cfg.presets[0], false);
  }
}

static void do_long_press() {
  // Long press (>2s) -> SOS. Sends the machine-readable "#LOC" report so the
  // MeshUptime node can map the SOS: `#LOC <lat>,<lon> (SOS) <preset #2>`.
  if (mu_cfg.has_sos) {
    char note[MU_PRESET_LEN + 8];
    snprintf(note, sizeof(note), "(SOS) %s", mu_cfg.presets[1]);
    mu_send_loc_dm(mu_cfg.sos_pub, note);
  }
}

void mu_button_loop() {
  int e = board.buttonStateChanged();   // 1 = pressed, -1 = released, 0 = no change
  if (e == 1) {
    press_t0 = millis();
    press_consumed = false;
    // Highest priority: a press silences a playing tune / find / cancels a fall
    // pre-alarm. It then does nothing else on release.
    if (mu_fall_prealarm_active()) {
      mu_fall_cancel();
      press_consumed = true;
    } else if (mu_is_playing()) {
      mu_find_stop();   // stops find (and sends "gevonden" if DM-initiated) or any tune
      press_consumed = true;
    }
  } else if (e == -1) {
    if (!press_consumed) {
      unsigned long dur = millis() - press_t0;
      if (dur >= MU_LONGPRESS_MS) do_long_press();
      else                        do_short_press();
    }
  }
}
