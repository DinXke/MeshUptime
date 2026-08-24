#include "MeshUptime.h"
#include "MuTunes.h"
#include "MuBattery.h"
#include "MyMesh.h"                 // the_mesh, board, sensors, radio_driver, rtc_clock
#include <string.h>
#include <ctype.h>
#include <stdio.h>

// MeshUptimeCompanion — interactive ASCII-art serial menu.
//
// A thin, LINE-BASED, non-blocking UX layer over the existing `!`/CLI commands.
// Most items just build a command string and hand it to mu_handle_command(), so
// the menu and the scriptable CLI never drift apart. A few items that have no CLI
// (live radio-parameter change, reboot) are handled directly, always behind a
// confirmation. Navigation: a number picks an item, `b` = back, `q` = quit. The
// mesh loop keeps running between keystrokes; nothing here blocks.

// ---- state -----------------------------------------------------------------
enum { MODE_NAV = 0, MODE_INPUT = 1, MODE_CONFIRM = 2 };
enum { ACT_NONE = 0, ACT_CLI, ACT_RADIO_INPUT, ACT_RADIO_CONFIRM, ACT_REBOOT, ACT_MESHPRESET };

static bool menu_active = false;
static int  menu_cat    = 0;        // 0 = top level, 1..9 = category
static int  menu_mode   = MODE_NAV;
static int  pending_act = ACT_NONE;
static char pending_prefix[24];     // CLI prefix to prepend to the typed value

// stashed radio-param change (between input and confirm)
static double rq_freq; static double rq_bw; static int rq_sf, rq_cr, rq_tx;

// ---- small output helpers --------------------------------------------------
static void P(const char* s)  { Serial.println(s); }
static void run_cli(const char* line) {
  MuCmdCtx ctx;
  ctx.from_serial = true;
  ctx.sender_admin = true;
  memset(ctx.sender_pub, 0, MU_PUB_LEN);
  mu_handle_command(line, ctx);
}

static bool ieq(const char* a, const char* b) {   // case-insensitive equal
  while (*a && *b) {
    char ca = *a, cb = *b;
    if (ca >= 'A' && ca <= 'Z') ca += 32;
    if (cb >= 'A' && cb <= 'Z') cb += 32;
    if (ca != cb) return false;
    a++; b++;
  }
  return *a == 0 && *b == 0;
}

static const char* trim(const char* s, char* out, int out_sz) {
  while (*s == ' ' || *s == '\t') s++;
  int n = 0; while (*s && n < out_sz - 1) out[n++] = *s++;
  while (n > 0 && (out[n-1] == ' ' || out[n-1] == '\t' || out[n-1] == '\r')) n--;
  out[n] = 0;
  return out;
}

// ---- screens ---------------------------------------------------------------
static void draw_top() {
  P("");
  P(".-----------------------------------------------.");
  P("|   MeshUptimeCompanion  (*)  serieel-menu      |");
  P("'-----------------------------------------------'");
  P(" 1) Geluiden & deuntjes");
  P(" 2) Alerts & ernst-mapping");
  P(" 3) Knop & presets");
  P(" 4) Find-me");
  P(" 5) Val-detectie (backup, niet-gecertificeerd)");
  P(" 6) Radio & Power");
  P(" 7) Allowlist & beveiliging");
  P(" 8) Info / status / batterij / pubkey");
  P(" 9) Opslaan & herstart");
  P(" kies nummer, q=sluiten");
}

static void draw_cat(int c) {
  P("");
  switch (c) {
    case 1:
      P("== 1 Geluiden & deuntjes ==");
      P(" 1) lijst deuntjes-bibliotheek");
      P(" 2) speel deuntje af (preview)");
      P(" 3) wijs deuntje toe aan slot");
      P(" 4) globaal volume (0-3)");
      P(" 5) volume per slot");
      P(" 6) mute aan/uit");
      P(" 7) stille/zachtere-uren");
      break;
    case 2:
      P("== 2 Alerts & ernst-mapping ==");
      P(" 1) toon slot -> deuntje");
      P(" 2) preset -> slot (H|M|L|find|msg)");
      P(" 3) bericht-deuntje aan/uit");
      break;
    case 3:
      P("== 3 Knop & presets ==");
      P(" 1) toon/zet preset (1|2|3)");
      P(" 2) target-pubkey (kort drukken)");
      P(" 3) sos-pubkey (lang drukken/val)");
      break;
    case 4:
      P("== 4 Find-me ==");
      P(" 1) start find (deuntje+LED)");
      P(" 2) stop find");
      break;
    case 5:
      P("== 5 Val-detectie (BACKUP v/d backup) ==");
      P(" LET OP: niet-gecertificeerd, best-effort,");
      P(" geen vervanging voor een intern alarmsysteem.");
      P(" 1) val-detectie aan/uit");
      P(" 2) gevoeligheid (low|med|high)");
      P(" 3) geen-beweging (dead-man) minuten");
      P(" 4) pre-alarm/annuleer-venster (sec)");
      P(" 5) alarm-doel toevoegen (64 hex)");
      P(" 6) alarm-doel verwijderen (hex-prefix)");
      P(" 7) toon alarm-doelen");
      P(" 8) ook naar MeshManager (mm on|off)");
      P(" 9) TEST pre-alarm nu (knop annuleert)");
      P(" 0) status");
      break;
    case 6:
      P("== 6 Radio & Power ==");
      P(" 1) toon radio-parameters");
      P(" 2) GPS-modus (on|off|ondemand)");
      P(" 3) RXPS (off|conservative|balanced)");
      P(" 4) wijzig radio-parameters (!! valt v/d mesh)");
      P(" 5) herstel mesh-preset 869.618/BW62.5/SF8/CR8");
      break;
    case 7:
      P("== 7 Allowlist & beveiliging ==");
      P(" 1) toon allowlist");
      P(" 2) voeg pubkey toe (64 hex)");
      P(" 3) verwijder op hex-prefix");
      break;
    case 8:
      P("== 8 Info / status / batterij / pubkey ==");
      P(" 1) volledige cfg");
      P(" 2) batterij (echte mV + %)");
      P(" 3) pubkey");
      P(" 4) locatie");
      break;
    case 9:
      P("== 9 Opslaan & herstart ==");
      P(" (instellingen worden al automatisch bewaard)");
      P(" 1) forceer opslaan");
      P(" 2) herstart node");
      break;
    default: draw_top(); return;
  }
  P(" nummer=kies, b=terug, q=sluiten");
}

static void begin_input(const char* prompt, const char* cli_prefix) {
  menu_mode = MODE_INPUT;
  pending_act = ACT_CLI;
  strncpy(pending_prefix, cli_prefix, sizeof(pending_prefix) - 1);
  pending_prefix[sizeof(pending_prefix) - 1] = 0;
  Serial.println(prompt);
  Serial.println(" (b=annuleer)");
}

// ---- info helpers ----------------------------------------------------------
static void show_radio() {
  NodePrefs* pr = the_mesh.getNodePrefs();
  char b[128];
  snprintf(b, sizeof(b), "radio: freq=%.3f BW=%.1f SF=%u CR=%u TX=%ddBm",
           (double)pr->freq, (double)pr->bw, (unsigned)pr->sf, (unsigned)pr->cr,
           (int)pr->tx_power_dbm);
  P(b);
}
static void show_batt() {
  uint16_t mv = board.getBattMilliVolts();
  char b[64];
  snprintf(b, sizeof(b), "batt: %dmV (%d%%) [echte waarde]", (int)mv, battery_percent_from_mv(mv));
  P(b);
}
static void show_pubkey() {
  char hex[2 * MU_PUB_LEN + 1];
  mu_bytes_to_hex(the_mesh.self_id.pub_key, MU_PUB_LEN, hex);
  P(hex);
}

static void apply_radio() {
  NodePrefs* pr = the_mesh.getNodePrefs();
  pr->freq = (float)rq_freq;
  pr->bw   = (float)rq_bw;
  pr->sf   = (uint8_t)rq_sf;
  pr->cr   = (uint8_t)rq_cr;
  pr->tx_power_dbm = (int8_t)rq_tx;
  radio_driver.setParams(pr->freq, pr->bw, pr->sf, pr->cr);
  radio_driver.setTxPower(pr->tx_power_dbm);
  the_mesh.savePrefs();
  mu_rxps_apply(mu_cfg.rxps_level);   // re-apply RXPS with the new SF/BW
  P("radio toegepast + bewaard.");
}

// ---- category numeric handlers ---------------------------------------------
static void handle_cat_item(int c, int n) {
  switch (c) {
    case 1:
      if (n == 1) { run_cli("tunes"); }
      else if (n == 2) begin_input(" naam of nr:", "play ");
      else if (n == 3) begin_input(" slot+preset, bv 'H preset coin':", "tune ");
      else if (n == 4) begin_input(" globaal volume 0-3:", "vol ");
      else if (n == 5) begin_input(" slot + vol, bv 'H 2':", "vol ");
      else if (n == 6) begin_input(" mute on|off:", "mute ");
      else if (n == 7) begin_input(" quiet <sH>-<eH> [mute|0..3] | off:", "quiet ");
      else P(" ?");
      break;
    case 2:
      if (n == 1) {
        run_cli("tune H"); run_cli("tune M"); run_cli("tune L");
        run_cli("tune find"); run_cli("tune msg");
      } else if (n == 2) begin_input(" slot preset <naam>, bv 'H preset mario-die':", "tune ");
      else if (n == 3) begin_input(" msgtune on|off:", "msgtune ");
      else P(" ?");
      break;
    case 3:
      if (n == 1) begin_input(" preset <1|2|3> [tekst]:", "preset ");
      else if (n == 2) begin_input(" target <64hex>:", "target ");
      else if (n == 3) begin_input(" sos <64hex>:", "sos ");
      else P(" ?");
      break;
    case 4:
      if (n == 1) run_cli("find");
      else if (n == 2) run_cli("findstop");
      else P(" ?");
      break;
    case 5:
      if (n == 1) begin_input(" fall on|off:", "fall ");
      else if (n == 2) begin_input(" gevoeligheid low|med|high:", "fall sens ");
      else if (n == 3) begin_input(" minuten (0=uit):", "fall nomotion ");
      else if (n == 4) begin_input(" seconden 5-120:", "fall prealarm ");
      else if (n == 5) begin_input(" pubkey 64 hex:", "fall target add ");
      else if (n == 6) begin_input(" hex-prefix:", "fall target del ");
      else if (n == 7) run_cli("fall target list");
      else if (n == 8) begin_input(" mm on|off (ook naar MeshManager):", "fall mm ");
      else if (n == 9) run_cli("fall test");
      else if (n == 0) run_cli("fall status");
      else P(" ?");
      break;
    case 6:
      if (n == 1) show_radio();
      else if (n == 2) begin_input(" gps on|off|ondemand:", "gps ");
      else if (n == 3) {
        P(" WAARSCHUWING: RXPS spaart accu maar kan alert-DM's");
        P(" MISSEN. 'off' = continu RX (altijd bereikbaar).");
        begin_input(" rxps off|conservative|balanced:", "rxps ");
      } else if (n == 4) {
        P(" !!! WAARSCHUWING radio-parameters !!!");
        P(" Als freq/BW/SF/CR/TX niet matchen met de mesh");
        P(" (869.618 / BW62.5 / SF8 / CR8) valt deze node");
        P(" van het netwerk en is hij onbereikbaar.");
        P(" Typ: freq bw sf cr tx   (bv: 869.618 62.5 8 8 22)");
        P(" (b=annuleer)");
        menu_mode = MODE_INPUT; pending_act = ACT_RADIO_INPUT;
      } else if (n == 5) {
        show_radio();
        P(" Herstel mesh-preset 869.618/BW62.5/SF8/CR8 (TX blijft).");
        P(" Typ JA om toe te passen, b=annuleer.");
        menu_mode = MODE_CONFIRM; pending_act = ACT_MESHPRESET;
      } else P(" ?");
      break;
    case 7:
      if (n == 1) run_cli("allow list");
      else if (n == 2) begin_input(" pubkey 64 hex:", "allow add ");
      else if (n == 3) begin_input(" hex-prefix:", "allow del ");
      else P(" ?");
      break;
    case 8:
      if (n == 1) run_cli("cfg");
      else if (n == 2) show_batt();
      else if (n == 3) show_pubkey();
      else if (n == 4) run_cli("loc");
      else P(" ?");
      break;
    case 9:
      if (n == 1) { mu_config_save(); P("opgeslagen."); }
      else if (n == 2) {
        P(" Node herstarten? Typ JA om te bevestigen, b=annuleer.");
        menu_mode = MODE_CONFIRM; pending_act = ACT_REBOOT;
      } else P(" ?");
      break;
  }
}

// ---- input / confirm processing --------------------------------------------
static void process_input(const char* val) {
  if (pending_act == ACT_CLI) {
    char line[160];
    snprintf(line, sizeof(line), "%s%s", pending_prefix, val);
    run_cli(line);
    menu_mode = MODE_NAV; pending_act = ACT_NONE;
    return;
  }
  if (pending_act == ACT_RADIO_INPUT) {
    // Tokenise into 5 fields and use atof/atoi (float sscanf is not linked with
    // the nano newlib specs this build uses; float *printf* is, hence %.3f below).
    char toks[5][24];
    int nt = 0; const char* s = val;
    while (*s && nt < 5) {
      while (*s == ' ' || *s == '\t') s++;
      if (!*s) break;
      int k = 0; while (*s && *s != ' ' && *s != '\t' && k < 23) toks[nt][k++] = *s++;
      toks[nt][k] = 0; nt++;
    }
    double f  = nt > 0 ? atof(toks[0]) : 0;
    double bw = nt > 1 ? atof(toks[1]) : 0;
    int sf = nt > 2 ? atoi(toks[2]) : 0;
    int cr = nt > 3 ? atoi(toks[3]) : 0;
    int tx = nt > 4 ? atoi(toks[4]) : 0;
    if (nt == 5 &&
        f > 100 && f < 1000 && bw > 0 && sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8 &&
        tx >= -9 && tx <= 30) {
      rq_freq = f; rq_bw = bw; rq_sf = sf; rq_cr = cr; rq_tx = tx;
      char b[128];
      snprintf(b, sizeof(b), " NIEUW: freq=%.3f BW=%.1f SF=%d CR=%d TX=%ddBm",
               f, bw, sf, cr, tx);
      P(b);
      P(" Typ JA om toe te passen (node kan v/d mesh vallen!), b=annuleer.");
      menu_mode = MODE_CONFIRM; pending_act = ACT_RADIO_CONFIRM;
    } else {
      P(" ongeldig. Formaat: freq bw sf cr tx  (SF 5-12, CR 5-8, TX -9..30)");
    }
    return;
  }
}

static void process_confirm(const char* val) {
  bool yes = (!strcmp(val, "JA") || !strcmp(val, "ja") || !strcmp(val, "yes"));
  if (!yes) { P(" geannuleerd."); menu_mode = MODE_NAV; pending_act = ACT_NONE; return; }
  if (pending_act == ACT_RADIO_CONFIRM) { apply_radio(); }
  else if (pending_act == ACT_MESHPRESET) {
    rq_freq = 869.618; rq_bw = 62.5; rq_sf = 8; rq_cr = 8;
    rq_tx = the_mesh.getNodePrefs()->tx_power_dbm;   // keep current TX
    apply_radio();
  } else if (pending_act == ACT_REBOOT) {
    P(" herstarten...");
    board.reboot();
  }
  menu_mode = MODE_NAV; pending_act = ACT_NONE;
}

// ---- public API ------------------------------------------------------------
bool mu_menu_is_active() { return menu_active; }

void mu_menu_open() { menu_active = true; menu_cat = 0; menu_mode = MODE_NAV; draw_top(); }

bool mu_menu_handle_line(const char* raw) {
  char line[160];
  trim(raw, line, sizeof(line));

  if (!menu_active) {
    // Open on a bare Enter or the word "menu"; otherwise let the CLI have it.
    if (line[0] == 0 || ieq(line, "menu")) { mu_menu_open(); return true; }
    return false;
  }

  // While the menu owns input:
  if (!strcmp(line, "q")) { menu_active = false; menu_mode = MODE_NAV; P("menu gesloten."); return true; }

  if (menu_mode == MODE_INPUT) {
    if (!strcmp(line, "b")) { menu_mode = MODE_NAV; pending_act = ACT_NONE; draw_cat(menu_cat); return true; }
    process_input(line);
    if (menu_mode == MODE_NAV) draw_cat(menu_cat);
    return true;
  }
  if (menu_mode == MODE_CONFIRM) {
    if (!strcmp(line, "b")) { P(" geannuleerd."); menu_mode = MODE_NAV; pending_act = ACT_NONE; draw_cat(menu_cat); return true; }
    process_confirm(line);
    if (menu_mode == MODE_NAV) draw_cat(menu_cat);
    return true;
  }

  // MODE_NAV
  if (!strcmp(line, "b")) {
    if (menu_cat == 0) { menu_active = false; P("menu gesloten."); }
    else { menu_cat = 0; draw_top(); }
    return true;
  }
  if (line[0] == 0) { menu_cat ? draw_cat(menu_cat) : draw_top(); return true; }

  int n = atoi(line);
  if (menu_cat == 0) {
    if (n >= 1 && n <= 9) { menu_cat = n; draw_cat(n); }
    else draw_top();
  } else {
    handle_cat_item(menu_cat, n);
    if (menu_mode == MODE_NAV) draw_cat(menu_cat);
  }
  return true;
}
