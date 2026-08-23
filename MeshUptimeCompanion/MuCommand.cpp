#include "MeshUptime.h"
#include "MuBattery.h"
#include "MyMesh.h"                 // the_mesh, board, sensors, rtc_clock
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

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

  mu_reply(ctx, "cfg: mute=%d vol=%d msgtune=%d batt=%dmV(%d%%)",
           mu_cfg.mute, mu_cfg.vol, mu_cfg.msg_tune_enabled, mv, pct);
  if (mu_cfg.quiet_start == 0xFF)
    mu_reply(ctx, "quiet=off gps=%s fall=%d nomotion=%dmin",
             gps, mu_cfg.fall_enabled, mu_cfg.fall_nomotion_min);
  else
    mu_reply(ctx, "quiet=%02d-%02d gps=%s fall=%d nomotion=%dmin",
             mu_cfg.quiet_start, mu_cfg.quiet_end, gps,
             mu_cfg.fall_enabled, mu_cfg.fall_nomotion_min);
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

static void cmd_tune(const MuCmdCtx& ctx, const char* args) {
  char slotname[16];
  const char* p = next_tok(args, slotname, sizeof(slotname), true);
  int slot = tune_slot_from_name(slotname);
  if (slot < 0) { mu_reply(ctx, "tune <H|M|L|find|msg> [RTTTL]"); return; }
  p = skip_ws(p);
  if (*p == 0) {                    // report current
    mu_reply(ctx, "tune %s: %s", tune_name(slot), mu_cfg.tunes[slot]);
    return;
  }
  if ((int)strlen(p) >= MU_RTTTL_MAX) { mu_reply(ctx, "tune: te lang (max %d)", MU_RTTTL_MAX - 1); return; }
  strncpy(mu_cfg.tunes[slot], p, MU_RTTTL_MAX - 1);
  mu_cfg.tunes[slot][MU_RTTTL_MAX - 1] = 0;
  mu_config_save();
  mu_reply(ctx, "tune %s opgeslagen", tune_name(slot));
  mu_play_tune(slot);               // preview
}

static void cmd_quiet(const MuCmdCtx& ctx, const char* args) {
  char a[24];
  next_tok(args, a, sizeof(a), true);
  if (!strcmp(a, "off") || a[0] == 0) {
    mu_cfg.quiet_start = mu_cfg.quiet_end = 0xFF;
    mu_config_save();
    mu_reply(ctx, "quiet: off");
    return;
  }
  int s, e;
  if (sscanf(a, "%d-%d", &s, &e) == 2 && s >= 0 && s < 24 && e >= 0 && e < 24) {
    mu_cfg.quiet_start = (uint8_t)s;
    mu_cfg.quiet_end = (uint8_t)e;
    mu_config_save();
    mu_reply(ctx, "quiet: %02d-%02d (UTC)", s, e);
  } else {
    mu_reply(ctx, "quiet <startH>-<endH> | off");
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
    mu_reply(ctx, "loc %.5f,%.5f", (double)sensors.node_lat, (double)sensors.node_lon);
  } else {
    if (mu_cfg.gps_mode != MU_GPS_ON) sensors.setSettingValue("gps", "1"); // wake for a fix
    mu_reply(ctx, "loc: geen fix (GPS wakker, probeer straks opnieuw)");
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

static void cmd_fall(const MuCmdCtx& ctx, const char* args) {
  char a[16];
  const char* p = next_tok(args, a, sizeof(a), true);
  if (!strcmp(a, "on"))  { mu_cfg.fall_enabled = 1; mu_config_save(); mu_reply(ctx, "fall: on"); }
  else if (!strcmp(a, "off")) { mu_cfg.fall_enabled = 0; mu_config_save(); mu_reply(ctx, "fall: off"); }
  else if (!strcmp(a, "nomotion")) {
    char m[8]; next_tok(p, m, sizeof(m), false);
    int mins = atoi(m);
    if (mins < 0 || mins > 1440) { mu_reply(ctx, "nomotion 0..1440 (0=uit)"); return; }
    mu_cfg.fall_nomotion_min = (uint16_t)mins;
    mu_config_save();
    mu_reply(ctx, "fall nomotion: %d min", mins);
  } else {
    mu_reply(ctx, "fall on|off | fall nomotion <min>");
  }
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
    char a[8]; next_tok(args, a, sizeof(a), true);
    if (!strcmp(a, "on"))  mu_cfg.mute = 1;
    else if (!strcmp(a, "off")) mu_cfg.mute = 0;
    else { mu_reply(ctx, "mute on|off"); return true; }
    mu_config_save(); mu_reply(ctx, "mute: %s", mu_cfg.mute ? "on" : "off"); return true;
  }
  if (!strcmp(cmd, "vol")) {
    char a[8]; next_tok(args, a, sizeof(a), false);
    int v = atoi(a);
    if (a[0] == 0 || v < 0 || v > 3) { mu_reply(ctx, "vol 0..3 (0=stil; HW kent geen echt volume)"); return true; }
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
  if (!strcmp(cmd, "quiet"))  { cmd_quiet(ctx, args); return true; }
  if (!strcmp(cmd, "gps"))    { cmd_gps(ctx, args); return true; }
  if (!strcmp(cmd, "loc"))    { cmd_loc(ctx); return true; }
  if (!strcmp(cmd, "cfg"))    { cmd_cfg(ctx); return true; }
  if (!strcmp(cmd, "allow"))  { cmd_allow(ctx, args); return true; }
  if (!strcmp(cmd, "preset")) { cmd_preset(ctx, args); return true; }
  if (!strcmp(cmd, "target")) { cmd_setkey(ctx, args, false); return true; }
  if (!strcmp(cmd, "sos"))    { cmd_setkey(ctx, args, true); return true; }
  if (!strcmp(cmd, "fall"))   { cmd_fall(ctx, args); return true; }
  if (!strcmp(cmd, "help")) {
    mu_reply(ctx, "cmds: find findstop mute vol tune quiet gps loc cfg");
    mu_reply(ctx, "  allow preset target sos fall msgtune ping");
    return true;
  }
  mu_reply(ctx, "onbekend: %s (!help)", cmd);
  return true;
}
