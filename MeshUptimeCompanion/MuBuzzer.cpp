#include "MuBuzzer.h"
#include <ctype.h>

#if defined(NRF52_PLATFORM)
  #include <HardwarePWM.h>
#endif

MuBuzzer mu_buzzer;

#ifndef PIN_BUZZER
  #define PIN_BUZZER 25
#endif

// Base clock for HwPWM at DIV16 = 16 MHz / 16 = 1 MHz. period(ticks) = 1e6/freq.
#define MU_PWM_BASE_HZ 1000000UL

// Semitone base frequencies for octave 4 (index 1..12 as RTTTL: c c# d d# e f f# g g# a a# b).
static const uint16_t MU_BASE4[13] = {
  0, 262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494
};

// Coarse volume -> PWM HIGH-fraction (%). 50% is the loudest a symmetric square
// drives this buzzer; lower duty = quieter (coarse only on a magnetic buzzer).
static uint8_t vol_duty_pct(uint8_t vol) {
  switch (vol) {
    case 1:  return 4;    // soft
    case 2:  return 15;   // normal
    case 3:  return 50;   // loud
    default: return 0;    // silent
  }
}

void MuBuzzer::begin() {
#ifdef PIN_BUZZER_EN
  pinMode(PIN_BUZZER_EN, OUTPUT);
  digitalWrite(PIN_BUZZER_EN, LOW);   // idle: rail off
#endif
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
#if defined(NRF52_PLATFORM)
  // Own HwPWM0 for the buzzer pin (Arduino tone() uses HwPWM2, so no clash).
  HwPWM0.addPin(PIN_BUZZER);
  HwPWM0.setClockDiv(PWM_PRESCALER_PRESCALER_DIV_16);   // 1 MHz timebase
  _pwmReady = true;
#endif
}

void MuBuzzer::enable(bool on) {
#ifdef PIN_BUZZER_EN
  digitalWrite(PIN_BUZZER_EN, on ? HIGH : LOW);
#else
  (void)on;
#endif
}

void MuBuzzer::toneOn(uint16_t freq, uint8_t volume) {
  if (freq == 0) { toneOff(); return; }
#if defined(NRF52_PLATFORM)
  if (!_pwmReady) return;
  uint32_t top = MU_PWM_BASE_HZ / (uint32_t)freq;
  if (top < 8)      top = 8;
  if (top > 32767)  top = 32767;
  uint32_t duty = top * vol_duty_pct(volume) / 100;
  if (duty == 0) duty = 1;
  HwPWM0.setMaxValue((uint16_t)top);
  HwPWM0.writePin(PIN_BUZZER, (uint16_t)duty, false);
#else
  (void)freq; (void)volume;
#endif
}

void MuBuzzer::toneOff() {
#if defined(NRF52_PLATFORM)
  if (_pwmReady) HwPWM0.writePin(PIN_BUZZER, 0, false);
#endif
  digitalWrite(PIN_BUZZER, LOW);
}

void MuBuzzer::stop() {
  _playing = false;
  _p = nullptr;
  toneOff();
  enable(false);
}

void MuBuzzer::play(const char* melody, uint8_t volume) {
  if (!melody || volume == 0) return;      // silent -> do not sound
  stop();
  _vol = volume;
  _p = melody;
  _defDur = 4;
  _defOct = 6;
  _bpm = 63;

  // Header:  name:d=N,o=N,b=NNN:
  while (*_p && *_p != ':') _p++;
  if (*_p == ':') _p++;
  if (*_p == 'd' && _p[1] == '=') {
    _p += 2; int n = 0; while (isdigit((unsigned char)*_p)) n = n*10 + (*_p++ - '0');
    if (n > 0) _defDur = (uint8_t)n;
    if (*_p == ',') _p++;
  }
  if (*_p == 'o' && _p[1] == '=') {
    _p += 2; int n = *_p++ - '0';
    if (n >= 3 && n <= 7) _defOct = (uint8_t)n;
    if (*_p == ',') _p++;
  }
  if (*_p == 'b' && _p[1] == '=') {
    _p += 2; int n = 0; while (isdigit((unsigned char)*_p)) n = n*10 + (*_p++ - '0');
    if (n > 0) _bpm = (uint16_t)n;
    if (*_p == ':') _p++;
  }
  _wholenote = (60UL * 1000UL / _bpm) * 4;
  _firstNote = _p;

  _playing = true;
  enable(true);
  startNote();
}

void MuBuzzer::startNote() {
  if (!_playing || !_p) return;
  if (*_p == 0) { stop(); return; }        // end of melody

  // duration
  int num = 0;
  while (isdigit((unsigned char)*_p)) num = num*10 + (*_p++ - '0');
  uint32_t duration = num ? (_wholenote / num) : (_wholenote / _defDur);

  // note letter
  int note;
  switch (tolower((unsigned char)*_p)) {
    case 'c': note = 1;  break;
    case 'd': note = 3;  break;
    case 'e': note = 5;  break;
    case 'f': note = 6;  break;
    case 'g': note = 8;  break;
    case 'a': note = 10; break;
    case 'b': note = 12; break;
    default:  note = 0;  break;             // 'p' or unknown -> rest
  }
  if (*_p) _p++;

  if (*_p == '#') { note++; _p++; }
  if (*_p == '.') { duration += duration/2; _p++; }

  int oct = _defOct;
  if (isdigit((unsigned char)*_p)) { oct = *_p - '0'; _p++; }
  if (*_p == '.') { duration += duration/2; _p++; }
  if (*_p == ',') _p++;                     // step over separator

  if (note) {
    // freq = base(octave 4) shifted by (oct-4) octaves
    long f = MU_BASE4[note];
    int shift = oct - 4;
    while (shift > 0) { f *= 2; shift--; }
    while (shift < 0) { f /= 2; shift++; }
    if (f < 30)   f = 30;
    if (f > 12000) f = 12000;
    toneOn((uint16_t)f, _vol);
  } else {
    toneOff();                              // rest
  }
  _noteEnd = millis() + duration + 1;
}

void MuBuzzer::loop() {
  if (!_playing) return;
  if ((int32_t)(millis() - _noteEnd) < 0) return;   // still on current note
  if (!_p || *_p == 0) { stop(); return; }
  startNote();
}
