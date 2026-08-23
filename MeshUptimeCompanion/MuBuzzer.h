#pragma once
// MeshUptimeCompanion — custom non-blocking RTTTL player with COARSE VOLUME.
//
// Why not the stock genericBuzzer/NonBlockingRTTTL? Those drive the passive
// magnetic buzzer with Arduino tone() at a fixed 50% duty — no amplitude control.
// This engine drives the buzzer pin from the nRF52 hardware PWM (HwPWM0) and sets
// the PWM DUTY CYCLE per note, so a lower duty makes the buzzer measurably quieter.
// A magnetic buzzer only dims COARSELY this way (it is loudest near 50% duty and
// there is no true analog volume), hence 4 coarse levels, documented as such.
//
// Volume levels: 0 = silent, 1 = soft, 2 = normal, 3 = loud.
//
// Only PIN_BUZZER is used; on nRF52 the buzzer-enable rail PIN_BUZZER_EN is raised
// while a note is sounding and dropped when idle (saves power, mirrors stock).

#include <Arduino.h>

class MuBuzzer {
public:
  void begin();
  // Start playing `rtttl` at coarse `volume` (0..3). volume 0 is a no-op.
  void play(const char* rtttl, uint8_t volume);
  void loop();          // advance the (non-blocking) playback
  void stop();          // stop immediately + drop the enable rail
  bool isPlaying() const { return _playing; }

private:
  void  startNote();
  void  toneOn(uint16_t freq, uint8_t volume);
  void  toneOff();
  void  enable(bool on);

  const char*   _p        = nullptr;   // cursor into the (persistent) rtttl string
  const char*   _firstNote= nullptr;
  bool          _playing  = false;
  uint8_t       _vol      = 0;
  uint8_t       _defDur   = 4;
  uint8_t       _defOct   = 6;
  uint16_t      _bpm      = 63;
  uint32_t      _wholenote= 0;
  unsigned long _noteEnd  = 0;
  bool          _pwmReady = false;
};

extern MuBuzzer mu_buzzer;   // defined in MuBuzzer.cpp
