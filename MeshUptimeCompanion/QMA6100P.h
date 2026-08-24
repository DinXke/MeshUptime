#pragma once
// Minimal QMA6100P (QST 3-axis accelerometer) I2C driver for the T1000-E.
//
// The chip sits on the board's I2C bus (SDA P0.26 / SCL P0.27) behind the
// accelerometer power rail PIN_3V3_ACC_EN (P1.7). Address is 0x12 or 0x13
// depending on the SDO strap; begin() probes both by reading the chip-id.
//
// NOT validated on hardware yet — register set follows the QMA6100P datasheet.
// Used only by the default-OFF fall-detection module, so a wrong guess cannot
// affect normal operation.
//
// I2C SAFETY (see .cpp): the Adafruit nRF52 Wire driver busy-waits on the TWIM
// completion events with NO timeout, so a stuck/flaky sensor could otherwise
// hang the main loop forever (radio/BLE/mesh stall). Every transaction here
// therefore checks the Wire return values, never spins on read(), and bails
// FALSE fast on any error — a single sample costs sub-millisecond and returns
// quickly on error. The caller (MuFall) additionally auto-disables the module
// after repeated read failures and drives sampling from the motion interrupt,
// so the bus is touched only during actual movement.
#include <Arduino.h>

// Any-motion detection threshold default (register counts). Datasheet-derived,
// tune on hardware; a low-ish value keeps the wake sensitive so a real fall's
// preceding movement always opens a sampling burst.
#define QMA_MOT_TH_DEFAULT  0x0A

class QMA6100P {
public:
  bool  begin();                        // power rail + probe + configure; false if absent
  bool  isPresent() const { return _present; }
  uint8_t address() const { return _addr; }
  bool  read(float& gx, float& gy, float& gz);   // acceleration in g

  // Configure a low-power any-motion interrupt (X/Y/Z) routed to the INT pin,
  // non-latched (so the INT auto-clears and the ISR never needs an I2C read).
  // Returns true ONLY when the enable + map register writes read back correctly,
  // so the caller can fall back to timeout-safe polling whenever the motion
  // engine can't be trusted. A wrong register guess degrades to "poll", never to
  // "no detection" or a bus hang.
  bool  configMotionInterrupt(uint8_t threshold = QMA_MOT_TH_DEFAULT);
  bool  motionIntConfigured() const { return _motion_int_ok; }

private:
  bool    _present = false;
  uint8_t _addr = 0x12;
  float   _counts_per_g = 1024.0f;      // set from full-scale range
  bool    _motion_int_ok = false;
  bool    writeReg(uint8_t reg, uint8_t val);
  bool    writeRegVerify(uint8_t reg, uint8_t val);  // write + read-back check
  bool    readRegs(uint8_t reg, uint8_t* buf, uint8_t len);
  bool    probeAt(uint8_t addr);
};
