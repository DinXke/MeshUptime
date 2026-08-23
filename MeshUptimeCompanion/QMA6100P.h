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
#include <Arduino.h>

class QMA6100P {
public:
  bool  begin();                        // power rail + probe + configure; false if absent
  bool  isPresent() const { return _present; }
  uint8_t address() const { return _addr; }
  bool  read(float& gx, float& gy, float& gz);   // acceleration in g
private:
  bool    _present = false;
  uint8_t _addr = 0x12;
  float   _counts_per_g = 1024.0f;      // set from full-scale range
  bool    writeReg(uint8_t reg, uint8_t val);
  bool    readRegs(uint8_t reg, uint8_t* buf, uint8_t len);
  bool    probeAt(uint8_t addr);
};
