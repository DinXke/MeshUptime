#pragma once
// MeshUptimeCompanion — persisted configuration.
//
// KEY-SAFE: this is stored ADDITIVELY in its own LittleFS file ("/mu_cfg.dat")
// on the SAME InternalFS the stock companion uses. It never touches the identity
// file, NodePrefs, contacts or channels, so an app-region reflash (nRF52 UF2)
// leaves both the MeshCore identity/prefs AND this config intact.

#include <Arduino.h>

#define MU_TUNE_HIGH   0
#define MU_TUNE_MED    1
#define MU_TUNE_LOW    2
#define MU_TUNE_FIND   3
#define MU_TUNE_MSG    4
#define MU_TUNE_COUNT  5

#define MU_RTTTL_MAX   176   // max stored RTTTL string length (incl NUL)
#define MU_ALLOW_MAX   8     // allowlisted pubkeys
#define MU_PRESET_MAX  3
#define MU_PRESET_LEN  64
#define MU_PUB_LEN     32

#define MU_GPS_OFF      0
#define MU_GPS_ON       1
#define MU_GPS_ONDEMAND 2

#define MU_CFG_MAGIC   0x3143554DUL   // "MUC1"
#define MU_CFG_VERSION 1

struct MuConfig {
  uint32_t magic;
  uint16_t version;
  uint16_t size;

  uint8_t  mute;            // 1 = alert tunes muted (LED still blinks)
  uint8_t  vol;             // 0..3 coarse; 0 = silent. HW has no true volume.
  uint8_t  quiet_start;     // hour 0..23 (UTC), 0xFF = quiet-hours disabled
  uint8_t  quiet_end;       // hour 0..23 (UTC)
  uint8_t  fall_enabled;    // 1 = fall/no-motion detection armed
  uint8_t  gps_mode;        // MU_GPS_*
  uint8_t  msg_tune_enabled;// 1 = play subtle tune on plain (non-marked) DMs
  uint8_t  has_target;      // target_pub valid (button short-press / preset dest)
  uint8_t  has_sos;         // sos_pub valid (button long-press / fall dest)
  uint8_t  _pad[3];
  uint16_t fall_nomotion_min;   // 0 = dead-man timer off, else minutes

  uint8_t  target_pub[MU_PUB_LEN];
  uint8_t  sos_pub[MU_PUB_LEN];

  char     tunes[MU_TUNE_COUNT][MU_RTTTL_MAX];

  uint8_t  allow_used[MU_ALLOW_MAX];
  uint8_t  allow_admin[MU_ALLOW_MAX];
  uint8_t  allow_pub[MU_ALLOW_MAX][MU_PUB_LEN];

  char     presets[MU_PRESET_MAX][MU_PRESET_LEN];
};

extern MuConfig mu_cfg;

void   mu_config_begin();          // load or create defaults, seed allowlist
void   mu_config_save();           // persist current mu_cfg
void   mu_config_reset_defaults(); // reset (keeps nothing) then save

// Allowlist helpers. prefix_len<MU_PUB_LEN allowed for del-by-prefix matching.
int    mu_allow_find(const uint8_t* pub);           // index or -1
bool   mu_allow_is_admin(const uint8_t* pub);
bool   mu_allow_add(const uint8_t* pub, bool admin); // false if full
int    mu_allow_del_prefix(const uint8_t* prefix, int prefix_len); // count removed

// hex helpers
bool   mu_hex_to_bytes(const char* hex, uint8_t* out, int out_len);
void   mu_bytes_to_hex(const uint8_t* in, int len, char* out); // out >= 2*len+1
