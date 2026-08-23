#pragma once
// MeshUptimeCompanion — public glue API wired into the (copied) companion
// main.cpp. Everything the firmware adds on top of stock MeshCore hangs off
// mu_begin() / mu_loop().

#include <Arduino.h>
#include "MuConfig.h"
#include "MuBuzzer.h"     // MuBuzzer mu_buzzer (custom PWM-duty player, extern)

// Lifecycle (called from app_main.cpp)
void mu_begin();
void mu_loop();
bool mu_wants_cpu();   // true while a tune/LED/find/fall-prealarm is active -> no light sleep

// ---- Tune / LED / find engine (MuAlert.cpp) --------------------------------
void mu_play_tune(int tune_slot);   // respects mute + quiet-hours + per-slot vol; MU_TUNE_*
void mu_play_rtttl(const char* rtttl, uint8_t volume); // raw preview (ignores mute/quiet)
void mu_stop_all();                 // stop tune + LED + find (button/any-press priority)
bool mu_is_playing();               // tune or find currently sounding
void mu_find_start(const uint8_t* dm_initiator_pub /*NULL if from serial*/);
void mu_find_stop();
bool mu_in_quiet_hours();
// Quiet-period volume cap: 0..3 while inside the window, or -1 when not (no cap).
int  mu_quiet_cap();

// ---- RXPS opt-in (default OFF = continuous RX) — bridge into MyMesh.cpp ------
// level: 0 = off (continuous RX), 1 = conservative, 2 = balanced. Sets the
// desired level and re-applies it to the radio immediately.
void mu_rxps_apply(uint8_t level);

// ---- Command context + shared parser (MuCommand.cpp) -----------------------
struct MuCmdCtx {
  bool    from_serial;
  bool    sender_admin;          // meaningful when !from_serial
  uint8_t sender_pub[MU_PUB_LEN];// meaningful when !from_serial
};

// Parse and execute one command line ("!..." — leading '!' optional on serial).
// Returns true if it was recognised as a command.
bool mu_handle_command(const char* line, const MuCmdCtx& ctx);

// Reply helper: DM back to sender (if !from_serial) or print to Serial.
void mu_reply(const MuCmdCtx& ctx, const char* fmt, ...);

// Send a plain DM to an arbitrary pubkey (used by button/fall). Returns false
// if no matching contact is known. `also_gps` appends " @lat,lon" when a fix
// is available.
bool mu_send_dm(const uint8_t* pub, const char* text, bool also_gps);

// ---- Button (MuButton.cpp) -------------------------------------------------
void mu_button_begin();
void mu_button_loop();

// ---- Fall / no-motion detection (MuFall.cpp) -------------------------------
void mu_fall_begin();
void mu_fall_loop();
bool mu_fall_prealarm_active();
void mu_fall_cancel();       // button-cancel during the pre-alarm window

// ---- Serial CLI (MuAlert.cpp drives it) ------------------------------------
void mu_serial_loop();

// ---- Interactive ASCII serial menu (MuMenu.cpp) ----------------------------
// Returns true if the menu consumed the line (so the CLI parser should skip it).
bool mu_menu_is_active();
bool mu_menu_handle_line(const char* line);   // feed one serial line
void mu_menu_open();                          // print the top-level menu
