#pragma once
// MeshUptimeCompanion — public glue API wired into the (copied) companion
// main.cpp. Everything the firmware adds on top of stock MeshCore hangs off
// mu_begin() / mu_loop().

#include <Arduino.h>
#include "MuConfig.h"

class genericBuzzer;
extern genericBuzzer mu_buzzer;   // defined in app_main.cpp

// Lifecycle (called from app_main.cpp)
void mu_begin();
void mu_loop();
bool mu_wants_cpu();   // true while a tune/LED/find/fall-prealarm is active -> no light sleep

// ---- Tune / LED / find engine (MuAlert.cpp) --------------------------------
void mu_play_tune(int tune_slot);   // respects mute + quiet-hours; MU_TUNE_*
void mu_stop_all();                 // stop tune + LED + find (button/any-press priority)
bool mu_is_playing();               // tune or find currently sounding
void mu_find_start(const uint8_t* dm_initiator_pub /*NULL if from serial*/);
void mu_find_stop();
bool mu_in_quiet_hours();

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
