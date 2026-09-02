#include "MeshUptime.h"
#include "MuBattery.h"
#include "MuTunes.h"                // built-in tune library
#include "MyMesh.h"                 // the_mesh, board, sensors, rtc_clock
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <math.h>

// ---- reply -----------------------------------------------------------------
void mu_reply(const MuCmdCtx& ctx, const char* fmt, ...) {
  char buf[180];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (ctx.from_serial) {
    Serial.println(buf);
  } else {
    mu_send_dm(ctx.sender_pub, buf, false);
  }
}

// ---- small parse helpers ---------------------------------------------------
static const char* skip_ws(const char* s) { while (*s == ' ' || *s == '\t') s++; return s; }

// copy next token (lowercased) into out; return pointer just past it
static const char* next_tok(const char* s, char* out, int out_sz, bool lower) {
  s = skip_ws(s);
  int n = 0;
  while (*s && *s != ' ' && *s != '\t' && n < out_sz - 1) {
    char c = *s++;
    out[n++] = lower ? (char)tolower((unsigned char)c) : c;
  }
  out[n] = 0;
  return s;
}

static int tune_slot_from_name(const char* name) {
  if (!strcmp(name, "h") || !strcmp(name, "high"))   return MU_TUNE_HIGH;
  if (!strcmp(name, "m") || !strcmp(name, "med"))    return MU_TUNE_MED;
  if (!strcmp(name, "l") || !strcmp(name, "low"))    return MU_TUNE_LOW;
  if (!strcmp(name, "find"))                          return MU_TUNE_FIND;
  if (!strcmp(name, "msg"))                           return MU_TUNE_MSG;
  return -1;
}
static const char* tune_name(int slot) {
  static const char* n[] = {"H","M","L","find","msg"};
  return (slot >= 0 && slot < MU_TUNE_COUNT) ? n[slot] : "?";
}

// ---- individual commands ---------------------------------------------------
static void cmd_cfg(const MuCmdCtx& ctx) {
  uint16_t mv = board.getBattMilliVolts();
  int pct = battery_percent_from_mv(mv);
  int allow_n = 0;
  for (int i = 0; i < MU_ALLOW_MAX; i++) if (mu_cfg.allow_used[i]) allow_n++;

  const char* gps = mu_cfg.gps_mode == MU_GPS_ON ? "on"
                  : mu_cfg.gps_mode == MU_GPS_ONDEMAND ? "ondemand" : "off";
  const char* rxps = mu_cfg.rxps_level == 1 ? "conservative"
                   : mu_cfg.rxps_level == 2 ? "balanced" : "off";

  // NOTE: our OWN output reports the REAL voltage + TRUE curve %. Only the app's
  // standard battery frame gets the encoded value (see PROTOCOL.md §7).
  mu_reply(ctx, "cfg: mute=%d vol=%d msgtune=%d batt=%dmV(%d%%)",
           mu_cfg.mute, mu_cfg.vol, mu_cfg.msg_tune_enabled, mv, pct);
  mu_reply(ctx, "slotvol H=%d M=%d L=%d find=%d msg=%d (255=default)",
           mu_cfg.tune_vol[MU_TUNE_HIGH], mu_cfg.tune_vol[MU_TUNE_MED],
           mu_cfg.tune_vol[MU_TUNE_LOW], mu_cfg.tune_vol[MU_TUNE_FIND],
           mu_cfg.tune_vol[MU_TUNE_MSG]);
  if (mu_cfg.quiet_start == 0xFF)
    mu_reply(ctx, "quiet=off gps=%s rxps=%s fall=%d nomotion=%dmin",
             gps, rxps, mu_cfg.fall_enabled, mu_cfg.fall_nomotion_min);
  else
    mu_reply(ctx, "quiet=%02d-%02d(%s) gps=%s rxps=%s fall=%d nomotion=%dmin",
             mu_cfg.quiet_start, mu_cfg.quiet_end,
             mu_cfg.quiet_level == 0 ? "mute" : "reduced",
             gps, rxps, mu_cfg.fall_enabled, mu_cfg.fall_nomotion_min);
  mu_reply(ctx, "allow=%d target=%d sos=%d loc=%.5f,%.5f",
           allow_n, mu_cfg.has_target, mu_cfg.has_sos,
           (double)sensors.node_lat, (double)sensors.node_lon);
}

static void cmd_allow(const MuCmdCtx& ctx, const char* args) {
  // From a DM, allowlist management requires the sender to be an admin.
  if (!ctx.from_serial && !ctx.sender_admin) {
    mu_reply(ctx, "allow: geen rechten"); return;
  }
  char sub[16];
  const char* p = next_tok(args, sub, sizeof(sub), true);

  if (!strcmp(sub, "list")) {
    int shown = 0;
    for (int i = 0; i < MU_ALLOW_MAX; i++) {
      if (!mu_cfg.allow_used[i]) continue;
      char hex[2 * MU_PUB_LEN + 1];
      mu_bytes_to_hex(mu_cfg.allow_pub[i], MU_PUB_LEN, hex);
      hex[16] = 0;   // show first 8 bytes prefix
      mu_reply(ctx, "%d %s%s%s", i, hex, "...",
               mu_cfg.allow_admin[i] ? " admin" : "");
      shown++;
    }
    if (!shown) mu_reply(ctx, "allow: leeg");
    return;
  }
  if (!strcmp(sub, "add")) {
    char hex[80];
    next_tok(p, hex, sizeof(hex), false);
    uint8_t pub[MU_PUB_LEN];
    if (!mu_hex_to_bytes(hex, pub, MU_PUB_LEN)) { mu_reply(ctx, "allow add: 64 hex nodig"); return; }
    if (mu_allow_add(pub, false)) { mu_config_save(); mu_reply(ctx, "allow add: ok"); }
    else mu_reply(ctx, "allow add: vol");
    return;
  }
  if (!strcmp(sub, "del")) {
    char hex[80];
    next_tok(p, hex, sizeof(hex), false);
    int hl = strlen(hex);
    if (hl < 2 || (hl & 1)) { mu_reply(ctx, "allow del: hex-prefix nodig"); return; }
    uint8_t pre[MU_PUB_LEN];
    int plen = hl / 2;
    if (plen > MU_PUB_LEN) plen = MU_PUB_LEN;
    char tmp[2 * MU_PUB_LEN + 1];
    strncpy(tmp, hex, plen * 2); tmp[plen * 2] = 0;
    if (!mu_hex_to_bytes(tmp, pre, plen)) { mu_reply(ctx, "allow del: bad hex"); return; }
    int n = mu_allow_del_prefix(pre, plen);
    mu_config_save();
    mu_reply(ctx, "allow del: %d verwijderd", n);
    return;
  }
  mu_reply(ctx, "allow add|list|del");
}

static void assign_slot_rtttl(const MuCmdCtx& ctx, int slot, const char* rtttl) {
  if ((int)strlen(rtttl) >= MU_RTTTL_MAX) {
    mu_reply(ctx, "tune: te lang (max %d)", MU_RTTTL_MAX - 1); return;
  }
  strncpy(mu_cfg.tunes[slot], rtttl, MU_RTTTL_MAX - 1);
  mu_cfg.tunes[slot][MU_RTTTL_MAX - 1] = 0;
  mu_config_save();
  mu_reply(ctx, "tune %s opgeslagen", tune_name(slot));
  mu_play_tune(slot);               // preview
}

static void cmd_tune(const MuCmdCtx& ctx, const char* args) {
  char slotname[16];
  const char* p = next_tok(args, slotname, sizeof(slotname), true);
  int slot = tune_slot_from_name(slotname);
  if (slot < 0) { mu_reply(ctx, "tune <H|M|L|find|msg> [preset <naam>|RTTTL]"); return; }
  p = skip_ws(p);
  if (*p == 0) {                    // report current
    mu_reply(ctx, "tune %s: %s", tune_name(slot), mu_cfg.tunes[slot]);
    return;
  }
  // `tune <slot> preset <name|nr>` -> assign a built-in library tune.
  char kw[12];
  const char* after = next_tok(p, kw, sizeof(kw), true);
  if (!strcmp(kw, "preset")) {
    char name[24];
    next_tok(after, name, sizeof(name), false);
    int idx = mu_tune_resolve(name);
    if (idx < 0) { mu_reply(ctx, "onbekende preset (zie !tunes)"); return; }
    assign_slot_rtttl(ctx, slot, MU_TUNES[idx].rtttl);
    return;
  }
  // Otherwise treat the remainder as a raw RTTTL string (paste).
  assign_slot_rtttl(ctx, slot, p);
}

static void cmd_tunes(const MuCmdCtx& ctx) {
  mu_reply(ctx, "deuntjes-bibliotheek (%d):", MU_TUNES_COUNT);
  for (int i = 0; i < MU_TUNES_COUNT; i++)
    mu_reply(ctx, "%d %s", i + 1, MU_TUNES[i].name);
}

static void cmd_play(const MuCmdCtx& ctx, const char* args) {
  char name[24];
  next_tok(args, name, sizeof(name), false);
  if (name[0] == 0) { mu_reply(ctx, "play <naam|nr> (zie !tunes)"); return; }
  int idx = mu_tune_resolve(name);
  if (idx < 0) { mu_reply(ctx, "onbekend deuntje (zie !tunes)"); return; }
  uint8_t vol = mu_cfg.vol ? mu_cfg.vol : 2;   // preview at global vol (or normal)
  mu_play_rtttl(MU_TUNES[idx].rtttl, vol);
  mu_reply(ctx, "play: %s", MU_TUNES[idx].name);
}

static void cmd_rxps(const MuCmdCtx& ctx, const char* args) {
  char a[16];
  next_tok(args, a, sizeof(a), true);
  uint8_t lvl;
  if      (!strcmp(a, "off"))          lvl = 0;
  else if (!strcmp(a, "conservative")) lvl = 1;
  else if (!strcmp(a, "balanced"))     lvl = 2;
  else { mu_reply(ctx, "rxps off|conservative|balanced (off=continu RX)"); return; }
  mu_cfg.rxps_level = lvl;
  mu_config_save();
  mu_rxps_apply(lvl);
  if (lvl == 0) mu_reply(ctx, "rxps: off (continu RX)");
  else mu_reply(ctx, "rxps: %s - LET OP: kan alert-DM's missen", a);
}

static void cmd_quiet(const MuCmdCtx& ctx, const char* args) {
  // Forms:
  //   quiet off
  //   quiet <sH>-<eH>            (full mute inside window)
  //   quiet <sH>-<eH> mute       (full mute)
  //   quiet <sH>-<eH> <0-3>      (reduced-volume "quieter period"; 0 = mute)
  char a[24];
  const char* p = next_tok(args, a, sizeof(a), true);
  if (!strcmp(a, "off") || a[0] == 0) {
    mu_cfg.quiet_start = mu_cfg.quiet_end = 0xFF;
    mu_config_save();
    mu_reply(ctx, "quiet: off");
    return;
  }
  int s, e;
  if (sscanf(a, "%d-%d", &s, &e) == 2 && s >= 0 && s < 24 && e >= 0 && e < 24) {
    char mode[8];
    next_tok(p, mode, sizeof(mode), true);
    uint8_t level = 0;               // default = full mute
    if (mode[0] == 0 || !strcmp(mode, "mute")) {
      level = 0;
    } else if (mode[0] >= '0' && mode[0] <= '3' && mode[1] == 0) {
      level = (uint8_t)(mode[0] - '0');
    } else {
      mu_reply(ctx, "quiet <sH>-<eH> [mute|0..3]"); return;
    }
    mu_cfg.quiet_start = (uint8_t)s;
    mu_cfg.quiet_end   = (uint8_t)e;
    mu_cfg.quiet_level = level;
    mu_config_save();
    if (level == 0) mu_reply(ctx, "quiet: %02d-%02d mute (UTC)", s, e);
    else            mu_reply(ctx, "quiet: %02d-%02d vol<=%d (UTC)", s, e, level);
  } else {
    mu_reply(ctx, "quiet <startH>-<endH> [mute|0..3] | off");
  }
}

static void cmd_gps(const MuCmdCtx& ctx, const char* args) {
  char a[16];
  next_tok(args, a, sizeof(a), true);
  if (!strcmp(a, "on"))            { mu_cfg.gps_mode = MU_GPS_ON;       sensors.setSettingValue("gps", "1"); }
  else if (!strcmp(a, "off"))      { mu_cfg.gps_mode = MU_GPS_OFF;      sensors.setSettingValue("gps", "0"); }
  else if (!strcmp(a, "ondemand")) { mu_cfg.gps_mode = MU_GPS_ONDEMAND; sensors.setSettingValue("gps", "0"); }
  else { mu_reply(ctx, "gps on|off|ondemand"); return; }
  mu_config_save();
  mu_reply(ctx, "gps: %s", a);
}

static void cmd_loc(const MuCmdCtx& ctx) {
  if (sensors.node_lat != 0.0 || sensors.node_lon != 0.0) {
    // Machine-readable "#LOC" contract: the reply STARTS with `#LOC <lat>,<lon>`
    // (5 decimals) so the receiving MeshUptime node can parse & map it.
    mu_reply(ctx, "#LOC %.5f,%.5f", (double)sensors.node_lat, (double)sensors.node_lon);
  } else {
    if (mu_cfg.gps_mode != MU_GPS_ON) sensors.setSettingValue("gps", "1"); // wake for a fix
    mu_reply(ctx, "loc: geen fix (GPS wakker, probeer straks opnieuw)");
  }
}

// Auto-locatie-push. `locpush` = toon stand; `locpush off` = uit; `locpush <min>
// [pubkey64]` = interval (+ optioneel doel-node). Zonder doel wordt er niets
// verstuurd. Elke tik stuurt de companion zelf een #LOC naar het doel.
static void cmd_locpush(const MuCmdCtx& ctx, const char* args) {
  const char* a = skip_ws(args);
  if (!*a) {
    if (mu_cfg.loc_push_min == 0) { mu_reply(ctx, "locpush: uit"); return; }
    if (!mu_cfg.loc_push_target_used) { mu_reply(ctx, "locpush: %u min - GEEN DOEL", mu_cfg.loc_push_min); return; }
    const uint8_t* t = mu_cfg.loc_push_target;
    mu_reply(ctx, "locpush: %u min -> %02X%02X%02X%02X..", mu_cfg.loc_push_min, t[0], t[1], t[2], t[3]);
    return;
  }
  char tok[24];
  const char* rest = next_tok(a, tok, sizeof(tok), true);
  if (!strcmp(tok, "off")) {
    mu_cfg.loc_push_min = 0; mu_config_save(); mu_loc_push_rearm();
    mu_reply(ctx, "locpush: uit"); return;
  }
  int min = atoi(tok);
  if (min <= 0 || min > 1440) { mu_reply(ctx, "locpush off | <min 1-1440> [pubkey64]"); return; }
  mu_cfg.loc_push_min = (uint16_t)min;
  rest = skip_ws(rest);
  if (*rest) {
    uint8_t pub[MU_PUB_LEN];
    if (mu_hex_to_bytes(rest, pub, MU_PUB_LEN)) {
      memcpy(mu_cfg.loc_push_target, pub, MU_PUB_LEN);
      mu_cfg.loc_push_target_used = 1;
    } else { mu_reply(ctx, "locpush: doel = 64 hex"); return; }
  }
  mu_config_save(); mu_loc_push_rearm();
  if (!mu_cfg.loc_push_target_used)
    mu_reply(ctx, "locpush: %d min - zet nog een doel: locpush %d <pubkey64>", min, min);
  else {
    const uint8_t* t = mu_cfg.loc_push_target;
    mu_reply(ctx, "locpush: %d min -> %02X%02X%02X%02X..", min, t[0], t[1], t[2], t[3]);
  }
}

static void cmd_preset(const MuCmdCtx& ctx, const char* args) {
  char n[8];
  const char* p = next_tok(args, n, sizeof(n), false);
  int idx = atoi(n);
  if (idx < 1 || idx > MU_PRESET_MAX) { mu_reply(ctx, "preset <1|2|3> <tekst>"); return; }
  p = skip_ws(p);
  if (*p == 0) { mu_reply(ctx, "preset %d: %s", idx, mu_cfg.presets[idx - 1]); return; }
  strncpy(mu_cfg.presets[idx - 1], p, MU_PRESET_LEN - 1);
  mu_cfg.presets[idx - 1][MU_PRESET_LEN - 1] = 0;
  mu_config_save();
  mu_reply(ctx, "preset %d opgeslagen", idx);
}

static void cmd_setkey(const MuCmdCtx& ctx, const char* args, bool is_sos) {
  char hex[80];
  next_tok(args, hex, sizeof(hex), false);
  uint8_t pub[MU_PUB_LEN];
  if (!mu_hex_to_bytes(hex, pub, MU_PUB_LEN)) { mu_reply(ctx, "64 hex nodig"); return; }
  if (is_sos) { memcpy(mu_cfg.sos_pub, pub, MU_PUB_LEN); mu_cfg.has_sos = 1; }
  else        { memcpy(mu_cfg.target_pub, pub, MU_PUB_LEN); mu_cfg.has_target = 1; }
  mu_config_save();
  mu_reply(ctx, "%s ingesteld", is_sos ? "sos" : "target");
}

static const char* fall_sens_name(uint8_t s) {
  return s == MU_FALL_SENS_LOW ? "low" : s == MU_FALL_SENS_HIGH ? "high" : "med";
}

static void cmd_fall_status(const MuCmdCtx& ctx) {
  const char* dest;
  int nt = mu_fall_target_count();
  if (nt > 0)                    dest = "lijst";
  else if (mu_cfg.has_sos)       dest = "sos";
  else if (mu_cfg.has_target)    dest = "target";
  else                           dest = "geen";
  mu_reply(ctx, "fall: %s sens=%s nomotion=%dmin prealarm=%ds",
           mu_cfg.fall_enabled ? "on" : "off", fall_sens_name(mu_cfg.fall_sens),
           mu_cfg.fall_nomotion_min, mu_cfg.fall_prealarm_sec);
  mu_reply(ctx, "  targets=%d mm=%s fallback=%s accel=%s mode=%s",
           nt, mu_cfg.fall_mm ? "on" : "off", dest,
           mu_fall_accel_present() ? "aanwezig" : "afwezig", mu_fall_mode_str());
  for (int i = 0; i < MU_FALL_TARGET_MAX; i++) {
    if (!mu_cfg.fall_target_used[i]) continue;
    char hex[2 * MU_PUB_LEN + 1];
    mu_bytes_to_hex(mu_cfg.fall_target_pub[i], MU_PUB_LEN, hex);
    hex[16] = 0;   // 8-byte prefix
    mu_reply(ctx, "  t%d %s..", i, hex);
  }
}

static void cmd_fall_target(const MuCmdCtx& ctx, const char* args) {
  char sub[16];
  const char* p = next_tok(args, sub, sizeof(sub), true);
  if (!strcmp(sub, "list") || sub[0] == 0) {
    int shown = 0;
    for (int i = 0; i < MU_FALL_TARGET_MAX; i++) {
      if (!mu_cfg.fall_target_used[i]) continue;
      char hex[2 * MU_PUB_LEN + 1];
      mu_bytes_to_hex(mu_cfg.fall_target_pub[i], MU_PUB_LEN, hex);
      hex[16] = 0;
      mu_reply(ctx, "t%d %s..", i, hex);
      shown++;
    }
    if (!shown) mu_reply(ctx, "fall target: leeg (val valt terug op sos/target)");
    return;
  }
  if (!strcmp(sub, "add")) {
    char hex[80];
    next_tok(p, hex, sizeof(hex), false);
    uint8_t pub[MU_PUB_LEN];
    if (!mu_hex_to_bytes(hex, pub, MU_PUB_LEN)) { mu_reply(ctx, "fall target add: 64 hex nodig"); return; }
    if (mu_fall_target_add(pub)) { mu_config_save(); mu_reply(ctx, "fall target add: ok (%d/%d)", mu_fall_target_count(), MU_FALL_TARGET_MAX); }
    else mu_reply(ctx, "fall target add: vol (max %d)", MU_FALL_TARGET_MAX);
    return;
  }
  if (!strcmp(sub, "del")) {
    char hex[80];
    next_tok(p, hex, sizeof(hex), false);
    int hl = strlen(hex);
    if (hl < 2 || (hl & 1)) { mu_reply(ctx, "fall target del: hex-prefix nodig"); return; }
    uint8_t pre[MU_PUB_LEN];
    int plen = hl / 2;
    if (plen > MU_PUB_LEN) plen = MU_PUB_LEN;
    char tmp[2 * MU_PUB_LEN + 1];
    strncpy(tmp, hex, plen * 2); tmp[plen * 2] = 0;
    if (!mu_hex_to_bytes(tmp, pre, plen)) { mu_reply(ctx, "fall target del: bad hex"); return; }
    int n = mu_fall_target_del_prefix(pre, plen);
    mu_config_save();
    mu_reply(ctx, "fall target del: %d verwijderd", n);
    return;
  }
  mu_reply(ctx, "fall target add|list|del <64hex|prefix>");
}

static void cmd_fall(const MuCmdCtx& ctx, const char* args) {
  char a[16];
  const char* p = next_tok(args, a, sizeof(a), true);
  if (a[0] == 0 || !strcmp(a, "status")) { cmd_fall_status(ctx); return; }
  if (!strcmp(a, "on"))  { mu_cfg.fall_enabled = 1; mu_config_save(); mu_reply(ctx, "fall: on"); return; }
  if (!strcmp(a, "off")) { mu_cfg.fall_enabled = 0; mu_config_save(); mu_reply(ctx, "fall: off"); return; }
  if (!strcmp(a, "sens")) {
    char b[8]; next_tok(p, b, sizeof(b), true);
    uint8_t s;
    if      (!strcmp(b, "low"))  s = MU_FALL_SENS_LOW;
    else if (!strcmp(b, "med"))  s = MU_FALL_SENS_MED;
    else if (!strcmp(b, "high")) s = MU_FALL_SENS_HIGH;
    else { mu_reply(ctx, "fall sens low|med|high"); return; }
    mu_cfg.fall_sens = s; mu_config_save();
    mu_reply(ctx, "fall sens: %s", fall_sens_name(s));
    return;
  }
  if (!strcmp(a, "nomotion")) {
    char m[8]; next_tok(p, m, sizeof(m), false);
    int mins = atoi(m);
    if (mins < 0 || mins > 1440) { mu_reply(ctx, "nomotion 0..1440 (0=uit)"); return; }
    mu_cfg.fall_nomotion_min = (uint16_t)mins;
    mu_config_save();
    mu_reply(ctx, "fall nomotion: %d min", mins);
    return;
  }
  if (!strcmp(a, "prealarm")) {
    char s[8]; next_tok(p, s, sizeof(s), false);
    int sec = atoi(s);
    if (sec < 5 || sec > 120) { mu_reply(ctx, "prealarm 5..120 sec"); return; }
    mu_cfg.fall_prealarm_sec = (uint16_t)sec;
    mu_config_save();
    mu_reply(ctx, "fall prealarm: %d sec", sec);
    return;
  }
  if (!strcmp(a, "target")) { cmd_fall_target(ctx, p); return; }
  if (!strcmp(a, "mm")) {
    char b[8]; next_tok(p, b, sizeof(b), true);
    if (!strcmp(b, "on"))  mu_cfg.fall_mm = 1;
    else if (!strcmp(b, "off")) mu_cfg.fall_mm = 0;
    else { mu_reply(ctx, "fall mm on|off (ook naar MeshManager)"); return; }
    mu_config_save();
    mu_reply(ctx, "fall mm: %s", mu_cfg.fall_mm ? "on" : "off");
    return;
  }
  if (!strcmp(a, "test")) {
    mu_fall_test();
    mu_reply(ctx, "fall test: pre-alarm actief (%ds), knop annuleert", mu_cfg.fall_prealarm_sec);
    return;
  }
  mu_reply(ctx, "fall on|off|status|sens|nomotion|prealarm|test|mm on|off");
  mu_reply(ctx, "  fall target add|del|list <64hex>");
}

// ---- radio parameters ------------------------------------------------------
// Remote RADIO-parameter control for full CLI/DM/menu parity. EACH mutating
// subcommand requires a trailing `confirm` token; without it we WARN and change
// nothing (an over-the-air freq/BW/SF/CR change can drop the companion off the
// mesh — recoverable only over the physical serial port). Applied values are
// mirrored into mu_cfg as an additive override so they survive reboot
// (mu_radio_apply_persisted, called from mu_begin). The COMPILED mesh defaults
// (869.618/BW62.5/SF8/CR8) are never changed and are used whenever no override
// is persisted.

// Mesh-default reference (compiled build defaults for this node).
#define MU_RADIO_DEF_FREQ 869.618
#define MU_RADIO_DEF_BW   62.5
#define MU_RADIO_DEF_SF   8
#define MU_RADIO_DEF_CR   8

// Accepted ranges (validated; out-of-range is rejected with NO change).
#define MU_RADIO_FREQ_MIN 100.0    // MHz (LR1110 sub-GHz; conservative bounds)
#define MU_RADIO_FREQ_MAX 1000.0
#define MU_RADIO_TX_MIN   (-9)     // dBm
#define MU_RADIO_TX_MAX   22       // dBm (T1000-E LR1110 high-power PA ceiling)

static const char* MU_RADIO_WARN =
  "WAARSCHUWING: radio wijzigen kan companion v/d mesh halen (fysiek serieel herstel nodig). Voeg 'confirm' toe om toch toe te passen.";

// Standard LoRa bandwidths (kHz). A change is only accepted for one of these.
static bool radio_bw_valid(double bw) {
  static const double S[] = {7.8,10.4,15.6,20.8,31.25,41.7,62.5,125.0,250.0,500.0};
  for (unsigned i = 0; i < sizeof(S)/sizeof(S[0]); i++)
    if (fabs(bw - S[i]) < 0.05) return true;
  return false;
}

// The next token (lowercased) equals "confirm"?
static bool radio_confirmed(const char* p) {
  char t[12]; next_tok(p, t, sizeof(t), true);
  return !strcmp(t, "confirm");
}

// Apply the CURRENT NodePrefs radio set to the driver, mirror into mu_cfg as a
// persisted override, save, and re-arm RXPS for the new SF/BW. Shared by every
// mutating `radio` path (CLI/DM/menu) so they never drift.
static void radio_commit() {
  NodePrefs* pr = the_mesh.getNodePrefs();
  // setParams -> RadioLib setFrequency/setBandwidth/setSpreadingFactor/setCodingRate
  radio_driver.setParams(pr->freq, pr->bw, pr->sf, pr->cr);
  radio_driver.setTxPower(pr->tx_power_dbm);   // -> RadioLib setOutputPower
  mu_cfg.radio_override = 1;
  mu_cfg.radio_freq   = pr->freq;
  mu_cfg.radio_bw     = pr->bw;
  mu_cfg.radio_sf     = pr->sf;
  mu_cfg.radio_cr     = pr->cr;
  mu_cfg.radio_tx_dbm = pr->tx_power_dbm;
  mu_config_save();
  mu_rxps_apply(mu_cfg.rxps_level);   // re-arm RX duty-cycle with the new SF/BW
}

static void cmd_radio(const MuCmdCtx& ctx, const char* args) {
  char sub[12];
  const char* p = next_tok(args, sub, sizeof(sub), true);
  NodePrefs* pr = the_mesh.getNodePrefs();

  if (sub[0] == 0 || !strcmp(sub, "show") || !strcmp(sub, "status")) {
    mu_reply(ctx, "radio: freq=%.3fMHz BW=%.1f SF=%u CR=%u TX=%ddBm%s",
             (double)pr->freq, (double)pr->bw, (unsigned)pr->sf, (unsigned)pr->cr,
             (int)pr->tx_power_dbm, mu_cfg.radio_override ? " (override)" : " (default)");
    mu_reply(ctx, "mesh-default: 869.618/BW62.5/SF8/CR8 (%s)",
             mu_cfg.radio_override ? "persisted override actief" : "in gebruik");
    return;
  }

  // All remaining subcommands are MUTATING: <value> [confirm].
  char valtok[24];
  const char* after = next_tok(p, valtok, sizeof(valtok), false);
  if (valtok[0] == 0) {
    mu_reply(ctx, "radio %s <waarde> confirm", sub);
    return;
  }

  if (!strcmp(sub, "freq")) {
    double f = atof(valtok);
    if (!(f > MU_RADIO_FREQ_MIN && f < MU_RADIO_FREQ_MAX)) {
      mu_reply(ctx, "radio freq: %.0f..%.0f MHz", MU_RADIO_FREQ_MIN, MU_RADIO_FREQ_MAX); return;
    }
    if (!radio_confirmed(after)) { mu_reply(ctx, MU_RADIO_WARN); return; }
    pr->freq = (float)f; radio_commit();
    mu_reply(ctx, "radio freq -> %.3f MHz toegepast+bewaard", (double)pr->freq);
    return;
  }
  if (!strcmp(sub, "bw")) {
    double bw = atof(valtok);
    if (!radio_bw_valid(bw)) {
      mu_reply(ctx, "radio bw: 7.8/10.4/15.6/20.8/31.25/41.7/62.5/125/250/500 kHz"); return;
    }
    if (!radio_confirmed(after)) { mu_reply(ctx, MU_RADIO_WARN); return; }
    pr->bw = (float)bw; radio_commit();
    mu_reply(ctx, "radio bw -> %.1f kHz toegepast+bewaard", (double)pr->bw);
    return;
  }
  if (!strcmp(sub, "sf")) {
    int sf = atoi(valtok);
    if (sf < 7 || sf > 12) { mu_reply(ctx, "radio sf: 7..12"); return; }
    if (!radio_confirmed(after)) { mu_reply(ctx, MU_RADIO_WARN); return; }
    pr->sf = (uint8_t)sf; radio_commit();
    mu_reply(ctx, "radio sf -> %u toegepast+bewaard", (unsigned)pr->sf);
    return;
  }
  if (!strcmp(sub, "cr")) {
    int cr = atoi(valtok);
    if (cr < 5 || cr > 8) { mu_reply(ctx, "radio cr: 5..8"); return; }
    if (!radio_confirmed(after)) { mu_reply(ctx, MU_RADIO_WARN); return; }
    pr->cr = (uint8_t)cr; radio_commit();
    mu_reply(ctx, "radio cr -> %u toegepast+bewaard", (unsigned)pr->cr);
    return;
  }
  if (!strcmp(sub, "txpower") || !strcmp(sub, "tx")) {
    int tx = atoi(valtok);
    if (tx < MU_RADIO_TX_MIN || tx > MU_RADIO_TX_MAX) {
      mu_reply(ctx, "radio txpower: %d..%d dBm", MU_RADIO_TX_MIN, MU_RADIO_TX_MAX); return;
    }
    if (!radio_confirmed(after)) { mu_reply(ctx, MU_RADIO_WARN); return; }
    pr->tx_power_dbm = (int8_t)tx; radio_commit();
    mu_reply(ctx, "radio txpower -> %d dBm toegepast+bewaard", (int)pr->tx_power_dbm);
    return;
  }
  mu_reply(ctx, "radio show|freq <MHz>|bw <kHz>|sf 7-12|cr 5-8|txpower <dBm>");
  mu_reply(ctx, "  mutaties vereisen 'confirm' (bv: radio freq 869.000 confirm)");
}

// Called from mu_begin() AFTER radio init: if a radio override is persisted,
// push it into the live NodePrefs and the LR1110 driver so it survives reboot.
// No-op (compiled mesh defaults kept) when nothing is persisted.
void mu_radio_apply_persisted() {
  if (!mu_cfg.radio_override) return;
  NodePrefs* pr = the_mesh.getNodePrefs();
  pr->freq         = mu_cfg.radio_freq;
  pr->bw           = mu_cfg.radio_bw;
  pr->sf           = mu_cfg.radio_sf;
  pr->cr           = mu_cfg.radio_cr;
  pr->tx_power_dbm = mu_cfg.radio_tx_dbm;
  radio_driver.setParams(pr->freq, pr->bw, pr->sf, pr->cr);
  radio_driver.setTxPower(pr->tx_power_dbm);
  Serial.printf("radio override toegepast: %.3f/BW%.1f/SF%u/CR%u/TX%ddBm\r\n",
                (double)pr->freq, (double)pr->bw, (unsigned)pr->sf,
                (unsigned)pr->cr, (int)pr->tx_power_dbm);
}

// ---- dispatcher ------------------------------------------------------------
bool mu_handle_command(const char* line, const MuCmdCtx& ctx) {
  const char* s = skip_ws(line);
  if (*s == '!') s++;               // leading '!' optional (mandatory over DM, stripped here)
  char cmd[16];
  const char* args = next_tok(s, cmd, sizeof(cmd), true);
  if (cmd[0] == 0) return false;

  if (!strcmp(cmd, "ping"))        { mu_reply(ctx, "pong"); return true; }
  if (!strcmp(cmd, "find"))        { mu_find_start(ctx.from_serial ? nullptr : ctx.sender_pub);
                                     mu_reply(ctx, "find: bezig (knop/ !findstop /5min)"); return true; }
  if (!strcmp(cmd, "findstop"))    { mu_find_stop(); mu_reply(ctx, "find: gestopt"); return true; }
  if (!strcmp(cmd, "mute")) {
    char a[12]; const char* p = next_tok(args, a, sizeof(a), true);
    if (!strcmp(a, "followapp")) {
      // Opt-in: also follow the app's buzzer_quiet. Default OFF so alerts always
      // sound even while the app is connected (the app forces buzzer_quiet then).
      char b[8]; next_tok(p, b, sizeof(b), true);
      if (!strcmp(b, "on")) mu_cfg.mute_follow_app = 1;
      else if (!strcmp(b, "off")) mu_cfg.mute_follow_app = 0;
      else { mu_reply(ctx, "mute followapp on|off (default off=onafhankelijk)"); return true; }
      mu_config_save();
      mu_reply(ctx, "mute followapp: %s", mu_cfg.mute_follow_app ? "on" : "off");
      return true;
    }
    if (!strcmp(a, "on"))  mu_cfg.mute = 1;
    else if (!strcmp(a, "off")) mu_cfg.mute = 0;
    else { mu_reply(ctx, "mute on|off | mute followapp on|off"); return true; }
    mu_config_save(); mu_reply(ctx, "mute: %s", mu_cfg.mute ? "on" : "off"); return true;
  }
  if (!strcmp(cmd, "vol")) {
    // `vol <0-3>` = global default; `vol <slot> <0-3>` = per-slot.
    char a[10]; const char* p = next_tok(args, a, sizeof(a), true);
    int slot = tune_slot_from_name(a);
    if (slot >= 0) {                 // per-slot form
      char b[8]; next_tok(p, b, sizeof(b), false);
      if (b[0] == 0) {               // report
        mu_reply(ctx, "vol %s: %d (255=default)", tune_name(slot), mu_cfg.tune_vol[slot]);
        return true;
      }
      int v = atoi(b);
      if (v < 0 || v > 3) { mu_reply(ctx, "vol <slot> 0..3"); return true; }
      mu_cfg.tune_vol[slot] = (uint8_t)v; mu_config_save();
      mu_reply(ctx, "vol %s: %d", tune_name(slot), v); return true;
    }
    if (a[0] == 0) { mu_reply(ctx, "vol 0..3 | vol <H|M|L|find|msg> 0..3"); return true; }
    int v = atoi(a);
    if (v < 0 || v > 3) { mu_reply(ctx, "vol 0..3 (0=stil; magnetische buzzer dimt grof)"); return true; }
    mu_cfg.vol = (uint8_t)v; mu_config_save();
    mu_reply(ctx, "vol: %d%s", v, v == 0 ? " (stil)" : " (best-effort)"); return true;
  }
  if (!strcmp(cmd, "msgtune")) {
    char a[8]; next_tok(args, a, sizeof(a), true);
    if (!strcmp(a, "on")) mu_cfg.msg_tune_enabled = 1;
    else if (!strcmp(a, "off")) mu_cfg.msg_tune_enabled = 0;
    else { mu_reply(ctx, "msgtune on|off"); return true; }
    mu_config_save(); mu_reply(ctx, "msgtune: %s", mu_cfg.msg_tune_enabled ? "on" : "off"); return true;
  }
  if (!strcmp(cmd, "tune"))   { cmd_tune(ctx, args); return true; }
  if (!strcmp(cmd, "tunes"))  { cmd_tunes(ctx); return true; }
  if (!strcmp(cmd, "play"))   { cmd_play(ctx, args); return true; }
  if (!strcmp(cmd, "rxps"))   { cmd_rxps(ctx, args); return true; }
  if (!strcmp(cmd, "quiet"))  { cmd_quiet(ctx, args); return true; }
  if (!strcmp(cmd, "gps"))    { cmd_gps(ctx, args); return true; }
  if (!strcmp(cmd, "loc"))    { cmd_loc(ctx); return true; }
  if (!strcmp(cmd, "locpush")){ cmd_locpush(ctx, args); return true; }
  if (!strcmp(cmd, "radio"))  { cmd_radio(ctx, args); return true; }
  if (!strcmp(cmd, "cfg") || !strcmp(cmd, "status")) { cmd_cfg(ctx); return true; }
  if (!strcmp(cmd, "allow"))  { cmd_allow(ctx, args); return true; }
  if (!strcmp(cmd, "preset")) { cmd_preset(ctx, args); return true; }
  if (!strcmp(cmd, "target")) { cmd_setkey(ctx, args, false); return true; }
  if (!strcmp(cmd, "sos"))    { cmd_setkey(ctx, args, true); return true; }
  if (!strcmp(cmd, "fall"))   { cmd_fall(ctx, args); return true; }
  if (!strcmp(cmd, "help")) {
    mu_reply(ctx, "cmds: find findstop mute vol tune tunes play quiet gps loc locpush cfg status");
    mu_reply(ctx, "  rxps radio allow preset target sos fall msgtune ping (serieel: menu)");
    mu_reply(ctx, "  locpush off | <min> [pubkey64]  (auto-#LOC elke X min naar node)");
    mu_reply(ctx, "  radio show|freq|bw|sf|cr|txpower <v> confirm  (mutatie vereist 'confirm')");
    return true;
  }
  mu_reply(ctx, "onbekend: %s (!help)", cmd);
  return true;
}
