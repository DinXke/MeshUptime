#include "MuConfig.h"
#include "MuTunes.h"
#include <string.h>

#if defined(NRF52_PLATFORM)
  #include <Adafruit_LittleFS.h>
  #include <InternalFileSystem.h>
  using namespace Adafruit_LittleFS_Namespace;
#endif

MuConfig mu_cfg;

static const char* MU_CFG_PATH = "/mu_cfg.dat";

// Seed identities (from the project spec).
static const char* MU_OWNER_HEX =
  "2CB0C5EB473757805EAB00F9DD0594C229D6E50B2ACEC6B57403B6259C9E126F";
static const char* MU_BOT_HEX =
  "E0841BF76B70FC407EF556531B610BB54584568A2560A4318B074181AE177146";

// ---- hex helpers -----------------------------------------------------------
static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool mu_hex_to_bytes(const char* hex, uint8_t* out, int out_len) {
  int n = 0;
  while (hex[n]) n++;
  if (n != out_len * 2) return false;
  for (int i = 0; i < out_len; i++) {
    int hi = hexval(hex[2 * i]);
    int lo = hexval(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

void mu_bytes_to_hex(const uint8_t* in, int len, char* out) {
  static const char* H = "0123456789ABCDEF";
  for (int i = 0; i < len; i++) {
    out[2 * i]     = H[in[i] >> 4];
    out[2 * i + 1] = H[in[i] & 0xF];
  }
  out[2 * len] = 0;
}

// ---- allowlist -------------------------------------------------------------
int mu_allow_find(const uint8_t* pub) {
  for (int i = 0; i < MU_ALLOW_MAX; i++) {
    if (mu_cfg.allow_used[i] &&
        memcmp(mu_cfg.allow_pub[i], pub, MU_PUB_LEN) == 0) return i;
  }
  return -1;
}

bool mu_allow_is_admin(const uint8_t* pub) {
  int i = mu_allow_find(pub);
  return i >= 0 && mu_cfg.allow_admin[i];
}

bool mu_allow_add(const uint8_t* pub, bool admin) {
  int i = mu_allow_find(pub);
  if (i >= 0) { mu_cfg.allow_admin[i] = admin ? 1 : 0; return true; }
  for (int j = 0; j < MU_ALLOW_MAX; j++) {
    if (!mu_cfg.allow_used[j]) {
      mu_cfg.allow_used[j] = 1;
      mu_cfg.allow_admin[j] = admin ? 1 : 0;
      memcpy(mu_cfg.allow_pub[j], pub, MU_PUB_LEN);
      return true;
    }
  }
  return false;
}

int mu_allow_del_prefix(const uint8_t* prefix, int prefix_len) {
  if (prefix_len <= 0 || prefix_len > MU_PUB_LEN) return 0;
  int removed = 0;
  for (int i = 0; i < MU_ALLOW_MAX; i++) {
    if (mu_cfg.allow_used[i] &&
        memcmp(mu_cfg.allow_pub[i], prefix, prefix_len) == 0) {
      mu_cfg.allow_used[i] = 0;
      mu_cfg.allow_admin[i] = 0;
      memset(mu_cfg.allow_pub[i], 0, MU_PUB_LEN);
      removed++;
    }
  }
  return removed;
}

// ---- fall-target list ------------------------------------------------------
int mu_fall_target_count() {
  int n = 0;
  for (int i = 0; i < MU_FALL_TARGET_MAX; i++) if (mu_cfg.fall_target_used[i]) n++;
  return n;
}

bool mu_fall_target_add(const uint8_t* pub) {
  for (int i = 0; i < MU_FALL_TARGET_MAX; i++)   // dedup
    if (mu_cfg.fall_target_used[i] &&
        memcmp(mu_cfg.fall_target_pub[i], pub, MU_PUB_LEN) == 0) return true;
  for (int i = 0; i < MU_FALL_TARGET_MAX; i++) {
    if (!mu_cfg.fall_target_used[i]) {
      mu_cfg.fall_target_used[i] = 1;
      memcpy(mu_cfg.fall_target_pub[i], pub, MU_PUB_LEN);
      return true;
    }
  }
  return false;   // list full
}

int mu_fall_target_del_prefix(const uint8_t* prefix, int prefix_len) {
  if (prefix_len <= 0 || prefix_len > MU_PUB_LEN) return 0;
  int removed = 0;
  for (int i = 0; i < MU_FALL_TARGET_MAX; i++) {
    if (mu_cfg.fall_target_used[i] &&
        memcmp(mu_cfg.fall_target_pub[i], prefix, prefix_len) == 0) {
      mu_cfg.fall_target_used[i] = 0;
      memset(mu_cfg.fall_target_pub[i], 0, MU_PUB_LEN);
      removed++;
    }
  }
  return removed;
}

// ---- node bot pubkey -------------------------------------------------------
const uint8_t* mu_node_bot_pubkey() {
  static uint8_t bot[MU_PUB_LEN];
  static bool ready = false;
  if (!ready) { mu_hex_to_bytes(MU_BOT_HEX, bot, MU_PUB_LEN); ready = true; }
  return bot;
}

// ---- defaults --------------------------------------------------------------
static void set_defaults() {
  memset(&mu_cfg, 0, sizeof(mu_cfg));
  mu_cfg.magic   = MU_CFG_MAGIC;
  mu_cfg.version = MU_CFG_VERSION;
  mu_cfg.size    = (uint16_t)sizeof(MuConfig);

  mu_cfg.mute            = 0;
  mu_cfg.vol             = 3;
  mu_cfg.quiet_start     = 0xFF;   // quiet-hours off
  mu_cfg.quiet_end       = 0xFF;
  mu_cfg.fall_enabled    = 0;      // default OFF (needs on-device tuning)
  mu_cfg.gps_mode        = MU_GPS_ONDEMAND;
  mu_cfg.msg_tune_enabled= 0;      // ordinary-message tune OFF by default (respects buzzer_quiet)
  mu_cfg.fall_nomotion_min = 0;    // dead-man off by default
  mu_cfg.quiet_level     = 0;      // quiet-hours = full mute by default
  mu_cfg.rxps_level      = 0;      // RXPS OFF by default (continuous RX, never miss an alert)
  mu_cfg.mute_follow_app = 0;      // default: our alerts are INDEPENDENT of the app's buzzer_quiet
  mu_cfg.fall_sens         = MU_FALL_SENS_MED;   // medium sensitivity by default
  mu_cfg.fall_mm           = 0;    // do NOT auto-forward to MeshManager unless enabled
  mu_cfg.fall_prealarm_sec = 20;   // 20 s buzzer/cancel window before the alert is sent
  // fall_target_used[]/fall_target_pub[] cleared by the memset above (no direct
  // targets -> fall alert falls back to the sos/target recipient).
  for (int i = 0; i < MU_TUNE_COUNT; i++)
    mu_cfg.tune_vol[i] = MU_VOL_DEFAULT;   // follow the global default

  strncpy(mu_cfg.tunes[MU_TUNE_HIGH], MU_DEF_TUNE_HIGH, MU_RTTTL_MAX - 1);
  strncpy(mu_cfg.tunes[MU_TUNE_MED],  MU_DEF_TUNE_MED,  MU_RTTTL_MAX - 1);
  strncpy(mu_cfg.tunes[MU_TUNE_LOW],  MU_DEF_TUNE_LOW,  MU_RTTTL_MAX - 1);
  strncpy(mu_cfg.tunes[MU_TUNE_FIND], MU_DEF_TUNE_FIND, MU_RTTTL_MAX - 1);
  strncpy(mu_cfg.tunes[MU_TUNE_MSG],  MU_DEF_TUNE_MSG,  MU_RTTTL_MAX - 1);

  // Preset #1 = ok/ack (button short-press default), #2 = SOS, #3 = spare.
  strncpy(mu_cfg.presets[0], "OK", MU_PRESET_LEN - 1);
  strncpy(mu_cfg.presets[1], "SOS - hulp nodig", MU_PRESET_LEN - 1);
  strncpy(mu_cfg.presets[2], "Onderweg", MU_PRESET_LEN - 1);

  // Seed allowlist: owner (admin) + MeshUptime bot (non-admin).
  uint8_t pub[MU_PUB_LEN];
  if (mu_hex_to_bytes(MU_OWNER_HEX, pub, MU_PUB_LEN)) mu_allow_add(pub, true);
  if (mu_hex_to_bytes(MU_BOT_HEX,   pub, MU_PUB_LEN)) mu_allow_add(pub, false);
}

// ---- persistence -----------------------------------------------------------
void mu_config_save() {
  mu_cfg.magic = MU_CFG_MAGIC;
  mu_cfg.version = MU_CFG_VERSION;
  mu_cfg.size = (uint16_t)sizeof(MuConfig);
#if defined(NRF52_PLATFORM)
  InternalFS.remove(MU_CFG_PATH);
  File f = InternalFS.open(MU_CFG_PATH, FILE_O_WRITE);
  if (f) {
    f.write((const uint8_t*)&mu_cfg, sizeof(mu_cfg));
    f.close();
  }
#endif
}

void mu_config_reset_defaults() {
  set_defaults();
  mu_config_save();
}

void mu_config_begin() {
  bool loaded = false;
#if defined(NRF52_PLATFORM)
  File f = InternalFS.open(MU_CFG_PATH, FILE_O_READ);
  if (f) {
    MuConfig tmp;
    memset(&tmp, 0, sizeof(tmp));
    int n = f.read((uint8_t*)&tmp, sizeof(tmp));
    f.close();
    // Forward-compatible load: accept the current layout, OR an older/shorter
    // blob (v1 had no trailing volume fields). We read whatever was stored into
    // the front of the struct and back-fill the new fields with sane defaults.
    if (n > 0 && n <= (int)sizeof(tmp) && tmp.magic == MU_CFG_MAGIC &&
        tmp.version >= 1 && tmp.version <= MU_CFG_VERSION) {
      memcpy(&mu_cfg, &tmp, sizeof(mu_cfg));
      // Forward migration: fields appended in newer versions arrived as zero (tmp
      // was zeroed before the short read); back-fill them with documented defaults.
      bool migrated = false;
      if (tmp.version < 2) {
        // v1 -> v2: quiet = full mute, per-slot volume follows the global default.
        mu_cfg.quiet_level = 0;
        mu_cfg.rxps_level  = 0;
        mu_cfg.mute_follow_app = 0;
        for (int i = 0; i < MU_TUNE_COUNT; i++) mu_cfg.tune_vol[i] = MU_VOL_DEFAULT;
        migrated = true;
      }
      if (tmp.version < 3) {
        // v2 -> v3: fall-detection config defaults (medium sensitivity, 20 s
        // pre-alarm, no direct targets/mm -> falls back to sos/target).
        mu_cfg.fall_sens         = MU_FALL_SENS_MED;
        mu_cfg.fall_mm           = 0;
        mu_cfg.fall_prealarm_sec = 20;
        memset(mu_cfg.fall_target_used, 0, sizeof(mu_cfg.fall_target_used));
        memset(mu_cfg.fall_target_pub, 0, sizeof(mu_cfg.fall_target_pub));
        migrated = true;
      }
      if (tmp.version < 4) {
        // v3 -> v4: no persisted radio override -> boot on the compiled mesh
        // defaults (tmp was zeroed, so these are already 0; set explicitly).
        mu_cfg.radio_override = 0;
        mu_cfg.radio_sf = mu_cfg.radio_cr = 0;
        mu_cfg.radio_tx_dbm = 0;
        mu_cfg.radio_freq = mu_cfg.radio_bw = 0.0f;
        migrated = true;
      }
      if (migrated) {
        mu_cfg.version = MU_CFG_VERSION;
        mu_cfg.size    = (uint16_t)sizeof(MuConfig);
        mu_config_save();   // rewrite in the new layout
      }
      loaded = true;
    }
  }
#endif
  if (!loaded) {
    set_defaults();
    mu_config_save();
  }
}
