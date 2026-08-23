#include "MeshUptime.h"
#include "MuTunes.h"
#include "MuHooks.h"
#include "MyMesh.h"                 // the_mesh, rtc_clock, board, sensors (via target.h)
#include <helpers/ui/buzzer.h>

// Green LED (T1000-E: LED_GREEN = P0.24, active HIGH).
#ifndef LED_PIN
  #define LED_PIN 24
#endif
#ifndef LED_STATE_ON
  #define LED_STATE_ON HIGH
#endif

#define MU_FIND_TIMEOUT_MS (5UL * 60UL * 1000UL)

// ---- LED pattern state -----------------------------------------------------
static const uint16_t* led_steps = nullptr;
static uint8_t  led_count = 0;
static uint8_t  led_idx   = 0;
static bool     led_on    = false;
static bool     led_repeat= false;
static unsigned long led_next = 0;

// ---- find-me state ---------------------------------------------------------
static bool     find_active = false;
static bool     find_from_dm = false;
static uint8_t  find_initiator[MU_PUB_LEN];
static unsigned long find_deadline = 0;

// ---- serial CLI line buffer ------------------------------------------------
static char     ser_line[160];
static uint8_t  ser_len = 0;

static void led_write(bool on) {
  digitalWrite(LED_PIN, on ? LED_STATE_ON : (LED_STATE_ON == HIGH ? LOW : HIGH));
  led_on = on;
}

static void led_start(int tune_slot, bool repeat) {
  MuLedPattern p = mu_led_pattern(tune_slot);
  led_steps = p.steps;
  led_count = p.count;
  led_idx = 0;
  led_repeat = repeat;
  if (led_count > 0) {
    led_write(true);
    led_next = millis() + led_steps[0];
  }
}

static void led_stop() {
  led_steps = nullptr;
  led_count = 0;
  led_repeat = false;
  led_write(false);
}

static void led_loop() {
  if (!led_steps || led_count == 0) return;
  if ((int32_t)(millis() - led_next) < 0) return;
  led_idx++;
  if (led_idx >= led_count) {
    if (led_repeat) {
      led_idx = 0;
    } else {
      led_stop();
      return;
    }
  }
  led_write((led_idx % 2) == 0);   // even step index = ON
  led_next = millis() + led_steps[led_idx];
}

// ---- buzzer helpers --------------------------------------------------------
static void buzzer_stop() {
  // With _is_quiet set first, genericBuzzer::play() still calls rtttl::stop()
  // (when a note is playing) and then returns without starting anything — so
  // this halts the tune AND drops BUZZER_EN, with no transient re-enable. The
  // melody argument is never used on this path.
  mu_buzzer.quiet(true);
  if (mu_buzzer.isPlaying()) mu_buzzer.play("x");
}

// ---- quiet hours -----------------------------------------------------------
bool mu_in_quiet_hours() {
  if (mu_cfg.quiet_start == 0xFF) return false;
  uint32_t t = rtc_clock.getCurrentTime();
  if (t < 1000000UL) return false;    // clock not set -> never suppress
  int hour = (int)((t / 3600UL) % 24UL);
  int s = mu_cfg.quiet_start, e = mu_cfg.quiet_end;
  if (s == e) return false;
  if (s < e)  return hour >= s && hour < e;
  return hour >= s || hour < e;       // wraps past midnight
}

// ---- public tune API -------------------------------------------------------
void mu_play_tune(int tune_slot) {
  if (tune_slot < 0 || tune_slot >= MU_TUNE_COUNT) return;

  bool audio_ok = !mu_cfg.mute && mu_cfg.vol > 0 && !mu_in_quiet_hours();

  // LED shows for alert severities even when audio is suppressed; the plain
  // "message" tick follows the audio gate so a muted device stays fully dark
  // for ordinary chatter.
  bool led_ok = (tune_slot != MU_TUNE_MSG) || audio_ok;
  if (led_ok) led_start(tune_slot, tune_slot == MU_TUNE_FIND);

  if (audio_ok) {
    mu_buzzer.quiet(false);
    mu_buzzer.play(mu_cfg.tunes[tune_slot]);
  }
}

bool mu_is_playing() {
  return mu_buzzer.isPlaying() || find_active;
}

void mu_stop_all() {
  find_active = false;
  find_from_dm = false;
  buzzer_stop();
  led_stop();
}

// ---- find-me ---------------------------------------------------------------
void mu_find_start(const uint8_t* dm_initiator_pub) {
  find_active = true;
  find_deadline = millis() + MU_FIND_TIMEOUT_MS;
  if (dm_initiator_pub) {
    find_from_dm = true;
    memcpy(find_initiator, dm_initiator_pub, MU_PUB_LEN);
  } else {
    find_from_dm = false;
  }
  // Find melody ignores mute (it is an explicit request) but honours quiet-hours
  // only for the audio; LED beacon always runs.
  led_start(MU_TUNE_FIND, true);
  if (!mu_in_quiet_hours()) {
    mu_buzzer.quiet(false);
    mu_buzzer.play(mu_cfg.tunes[MU_TUNE_FIND]);
  }
}

void mu_find_stop() {
  if (!find_active) { mu_stop_all(); return; }
  bool was_dm = find_from_dm;
  uint8_t who[MU_PUB_LEN];
  memcpy(who, find_initiator, MU_PUB_LEN);
  find_active = false;
  find_from_dm = false;
  buzzer_stop();
  led_stop();
  if (was_dm) mu_send_dm(who, "gevonden", false);
}

static void find_loop() {
  if (!find_active) return;
  if ((int32_t)(millis() - find_deadline) >= 0) { mu_stop_all(); return; }
  // Re-trigger the melody when it finishes so it loops until stopped.
  if (!mu_buzzer.isPlaying() && !mu_in_quiet_hours()) {
    mu_buzzer.quiet(false);
    mu_buzzer.play(mu_cfg.tunes[MU_TUNE_FIND]);
  }
}

// ---- send a DM to an arbitrary pubkey --------------------------------------
bool mu_send_dm(const uint8_t* pub, const char* text, bool also_gps) {
  ContactInfo* c = the_mesh.lookupContactByPubKey(pub, MU_PUB_LEN);
  if (!c) return false;

  char buf[160];
  if (also_gps && (sensors.node_lat != 0.0 || sensors.node_lon != 0.0)) {
    snprintf(buf, sizeof(buf), "%s @%.5f,%.5f", text,
             (double)sensors.node_lat, (double)sensors.node_lon);
  } else {
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
  }

  uint32_t expected_ack = 0, est_timeout = 0;
  uint32_t ts = rtc_clock.getCurrentTimeUnique();
  int r = the_mesh.sendMessage(*c, ts, 0, buf, expected_ack, est_timeout);
  return r != MSG_SEND_FAILED;
}

// ---- incoming DM hook (from MyMesh::onMessageRecv) -------------------------
void mu_on_direct_msg(const ContactInfo& from, uint32_t sender_timestamp, const char* text) {
  (void)sender_timestamp;
  if (!text) return;

  const char* p = text;
  while (*p == ' ') p++;

  // '!' command over DM — only from allowlisted pubkeys.
  if (*p == '!') {
    if (mu_allow_find(from.id.pub_key) >= 0) {
      MuCmdCtx ctx;
      ctx.from_serial = false;
      ctx.sender_admin = mu_allow_is_admin(from.id.pub_key);
      memcpy(ctx.sender_pub, from.id.pub_key, MU_PUB_LEN);
      mu_handle_command(p, ctx);
    }
    return;
  }

  // Severity emoji prefix (UTF-8).
  const uint8_t* b = (const uint8_t*)text;
  if (b[0] == 0xF0 && b[1] == 0x9F) {
    if (b[2] == 0x94 && b[3] == 0xB4) { mu_play_tune(MU_TUNE_HIGH); return; }
    if (b[2] == 0x9F && b[3] == 0xA0) { mu_play_tune(MU_TUNE_MED);  return; }
    if (b[2] == 0x9F && b[3] == 0xA2) { mu_play_tune(MU_TUNE_LOW);  return; }
  }

  // Plain, unmarked DM -> optional subtle message tune.
  if (mu_cfg.msg_tune_enabled) mu_play_tune(MU_TUNE_MSG);
}

// ---- serial CLI ------------------------------------------------------------
void mu_serial_loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      ser_line[ser_len] = 0;
      if (ser_len > 0) {
        MuCmdCtx ctx;
        ctx.from_serial = true;
        ctx.sender_admin = true;              // serial is always trusted
        memset(ctx.sender_pub, 0, MU_PUB_LEN);
        mu_handle_command(ser_line, ctx);
      }
      ser_len = 0;
    } else if (ser_len < sizeof(ser_line) - 1) {
      ser_line[ser_len++] = c;
    }
  }
}

// ---- lifecycle -------------------------------------------------------------
void mu_begin() {
  pinMode(LED_PIN, OUTPUT);
  led_write(false);

  mu_buzzer.begin();       // sets pins; also drives BUZZER_EN
  mu_buzzer.quiet(true);   // idle: EN low until we actually play

  mu_config_begin();

  // Apply persisted GPS mode. ON = continuous, ONDEMAND/OFF = leave asleep.
  if (mu_cfg.gps_mode == MU_GPS_ON) sensors.setSettingValue("gps", "1");
  else                             sensors.setSettingValue("gps", "0");

  mu_button_begin();
  mu_fall_begin();

  Serial.println("MeshUptimeCompanion ready. Type !help for commands.");
}

void mu_loop() {
  mu_buzzer.loop();
  led_loop();
  find_loop();

  // Power down BUZZER_EN once a one-shot tune has finished (not during find).
  if (!find_active && !mu_buzzer.isPlaying() && !mu_buzzer.isQuiet()) {
    mu_buzzer.quiet(true);
  }

  mu_button_loop();
  mu_fall_loop();
  mu_serial_loop();
}

bool mu_wants_cpu() {
  return mu_buzzer.isPlaying() || find_active || led_steps != nullptr ||
         mu_fall_prealarm_active();
}
