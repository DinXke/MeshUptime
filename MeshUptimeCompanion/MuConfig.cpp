#include "MuConfig.h"
#include "MuTunes.h"
#include <string.h>
#include <stdio.h>   // snprintf voor de bootnotitie

#if defined(NRF52_PLATFORM)
  #include <Adafruit_LittleFS.h>
  #include <InternalFileSystem.h>
  using namespace Adafruit_LittleFS_Namespace;
#endif

MuConfig mu_cfg;

// RX power-saving is unavailable on the standard MeshCore base: the radio stays
// in continuous RX so the pager never misses an alert DM. Kept as a no-op so the
// rxps command/menu still compile. (RXPS existed only in the PowerSaving-v17
// fork, which we intentionally no longer build on.)
void mu_rxps_apply(uint8_t /*level*/) {}

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
  mu_cfg.loc_push_min    = 0;      // auto-locatie-push UIT by default
  mu_cfg.loc_push_target_used = 0; // geen push-doel
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
//
// Twee bestanden en één regel: het echte bestand wordt NOOIT weggehaald voordat
// het nieuwe volledig op de flash staat. Een save schrijft eerst naar MU_CFG_TMP
// en schuift dat pas over MU_CFG_PATH heen (rename) als alle bytes er zijn. Een
// reset of lege accu midden in een save laat dan hoogstens een halve .tmp
// achter, en de vorige config staat er nog. De oude volgorde -- remove() en
// dán open(WRITE) -- had een venster waarin er even GEEN bestand bestond; één
// reset daar betekende fabrieksinstellingen bij de volgende boot, zonder dat
// iets daar ooit melding van maakte.
static const char* MU_CFG_TMP = "/mu_cfg.tmp";

// Wat er bij het opstarten met het cfg-bestand gebeurde, voor `cfg`/`status`.
// Bootuitvoer over USB-CDC gaat meestal verloren voordat een host de poort
// opent; dit blijft staan zodat de vraag "waarom staat alles op default?" ook
// een uur later nog te beantwoorden is, over serieel of over de mesh.
static char s_boot_note[64] = "nog niet geladen";

const char* mu_config_boot_note() { return s_boot_note; }

#if defined(NRF52_PLATFORM)
// Een blob volledig wegschrijven, of helemaal niet: een half bestand is erger
// dan geen, want mu_config_begin() zou het bij de volgende boot proberen te
// lezen.
static bool write_whole(const char* path, const void* data, size_t len) {
  InternalFS.remove(path);
  File f = InternalFS.open(path, FILE_O_WRITE);
  if (!f) return false;
  size_t n = f.write((const uint8_t*)data, len);
  f.close();
  if (n != len) { InternalFS.remove(path); return false; }
  return true;
}
#endif

void mu_config_save() {
  mu_cfg.magic = MU_CFG_MAGIC;
  mu_cfg.version = MU_CFG_VERSION;
  mu_cfg.size = (uint16_t)sizeof(MuConfig);
#if defined(NRF52_PLATFORM)
  if (!write_whole(MU_CFG_TMP, &mu_cfg, sizeof(mu_cfg))) return;  // de oude blijft staan
  // littlefs vervangt een bestaand doel bij rename. Lukt dat hier toch niet,
  // dan pas de oude weghalen en opnieuw -- het venster is dan zo klein als het
  // kan, en mu_config_begin() kent de .tmp als terugvalpad.
  if (!InternalFS.rename(MU_CFG_TMP, MU_CFG_PATH)) {
    InternalFS.remove(MU_CFG_PATH);
    InternalFS.rename(MU_CFG_TMP, MU_CFG_PATH);
  }
#endif
}

void mu_config_reset_defaults() {
  set_defaults();
  mu_config_save();
  strncpy(s_boot_note, "DEFAULTS (bewust gereset)", sizeof(s_boot_note) - 1);
}

#if defined(NRF52_PLATFORM)
// Eén bestand proberen te laden. true = mu_cfg is gevuld. Anders zegt `reden`
// waarom niet en `nieuwer` of het bestand van een NIEUWERE firmware kwam: dat is
// het ene geval waarin we het niet mogen overschrijven (zie mu_config_begin).
// `van_versie` krijgt de versie die in het bestand stond, voor de bootnotitie.
static bool load_file(const char* path, const char** reden, bool* nieuwer,
                      bool* gemigreerd, uint16_t* van_versie) {
  File f = InternalFS.open(path, FILE_O_READ);
  if (!f) { *reden = "geen bestand"; return false; }
  MuConfig tmp;
  memset(&tmp, 0, sizeof(tmp));
  int n = f.read((uint8_t*)&tmp, sizeof(tmp));
  f.close();
  if (n <= 0)                    { *reden = "leeg bestand";   return false; }
  if (tmp.magic != MU_CFG_MAGIC) { *reden = "verkeerd magic"; return false; }
  *van_versie = tmp.version;
  // Een nieuwere firmware schrijft een hogere versie en/of een grotere struct.
  // `n` kan dat niet zien (we lezen nooit meer dan sizeof(tmp)); het `size`-veld
  // in de kop wel. Zo'n bestand is voor ons onleesbaar maar niet waardeloos: de
  // volgende flash van die firmware moet het gewoon weer kunnen lezen.
  if (tmp.version > MU_CFG_VERSION || tmp.size > (uint16_t)sizeof(MuConfig)) {
    *reden = "van nieuwere firmware"; *nieuwer = true; return false;
  }
  if (tmp.version < 1)           { *reden = "versie 0";       return false; }

  // Forward-compatible load: accept the current layout, OR an older/shorter
  // blob (v1 had no trailing volume fields). We read whatever was stored into
  // the front of the struct and back-fill the new fields with sane defaults.
  memcpy(&mu_cfg, &tmp, sizeof(mu_cfg));
  // Forward migration: fields appended in newer versions arrived as zero (tmp
  // was zeroed before the short read); back-fill them with documented defaults.
  if (tmp.version < 2) {
    // v1 -> v2: quiet = full mute, per-slot volume follows the global default.
    mu_cfg.quiet_level = 0;
    mu_cfg.rxps_level  = 0;
    mu_cfg.mute_follow_app = 0;
    for (int i = 0; i < MU_TUNE_COUNT; i++) mu_cfg.tune_vol[i] = MU_VOL_DEFAULT;
    *gemigreerd = true;
  }
  if (tmp.version < 3) {
    // v2 -> v3: fall-detection config defaults (medium sensitivity, 20 s
    // pre-alarm, no direct targets/mm -> falls back to sos/target).
    mu_cfg.fall_sens         = MU_FALL_SENS_MED;
    mu_cfg.fall_mm           = 0;
    mu_cfg.fall_prealarm_sec = 20;
    memset(mu_cfg.fall_target_used, 0, sizeof(mu_cfg.fall_target_used));
    memset(mu_cfg.fall_target_pub, 0, sizeof(mu_cfg.fall_target_pub));
    *gemigreerd = true;
  }
  if (tmp.version < 4) {
    // v3 -> v4: no persisted radio override -> boot on the compiled mesh
    // defaults (tmp was zeroed, so these are already 0; set explicitly).
    mu_cfg.radio_override = 0;
    mu_cfg.radio_sf = mu_cfg.radio_cr = 0;
    mu_cfg.radio_tx_dbm = 0;
    mu_cfg.radio_freq = mu_cfg.radio_bw = 0.0f;
    *gemigreerd = true;
  }
  if (tmp.version < 5) {
    // v4 -> v5: auto-locatie-push uit, geen doel (tmp was genuld).
    mu_cfg.loc_push_min = 0;
    mu_cfg.loc_push_target_used = 0;
    memset(mu_cfg.loc_push_target, 0, sizeof(mu_cfg.loc_push_target));
    *gemigreerd = true;
  }
  return true;
}
#endif

void mu_config_begin() {
  bool loaded = false;
  bool nieuwer = false;
  bool gemigreerd = false;
  uint16_t van_versie = 0;
  const char* reden = "geen opslag op dit platform";
#if defined(NRF52_PLATFORM)
  loaded = load_file(MU_CFG_PATH, &reden, &nieuwer, &gemigreerd, &van_versie);
  if (loaded) {
    snprintf(s_boot_note, sizeof(s_boot_note), "geladen v%u", (unsigned)van_versie);
  } else if (!nieuwer) {
    // Het echte bestand is weg of stuk. Was er een save die wel de .tmp haalde
    // maar de rename niet meer? Dan is dát de laatste bekende toestand, en die
    // is beter dan fabrieksinstellingen.
    const char* reden_tmp = "";
    bool nieuwer_tmp = false;
    if (load_file(MU_CFG_TMP, &reden_tmp, &nieuwer_tmp, &gemigreerd, &van_versie)) {
      loaded = true;
      snprintf(s_boot_note, sizeof(s_boot_note), "hersteld uit .tmp (%s)", reden);
    }
  }
  if (loaded && gemigreerd) {
    snprintf(s_boot_note, sizeof(s_boot_note), "gemigreerd v%u -> v%u",
             (unsigned)van_versie, (unsigned)MU_CFG_VERSION);
    mu_cfg.version = MU_CFG_VERSION;
    mu_cfg.size    = (uint16_t)sizeof(MuConfig);
    mu_config_save();   // rewrite in the new layout
  } else if (loaded && strncmp(s_boot_note, "hersteld", 8) == 0) {
    mu_config_save();   // de .tmp meteen weer als echt bestand neerzetten
  }
#endif
  if (!loaded) {
    set_defaults();
    if (nieuwer) {
      // NIET opslaan. Dit is het downgrade-geval: een oudere build op een
      // toestel waarvan de config door een nieuwere is geschreven. Draaien op
      // defaults in RAM en het bestand met rust laten, zodat de volgende flash
      // van de nieuwere firmware alles weer gewoon terugvindt. De eerste
      // bewuste wijziging op deze build (elke mutatie bewaart) overschrijft
      // het alsnog -- dat is dan een keuze, geen ongeluk.
      snprintf(s_boot_note, sizeof(s_boot_note), "DEFAULTS (%s; bestand bewaard)", reden);
    } else {
      mu_config_save();
      snprintf(s_boot_note, sizeof(s_boot_note), "DEFAULTS (%s)", reden);
    }
  }
}
