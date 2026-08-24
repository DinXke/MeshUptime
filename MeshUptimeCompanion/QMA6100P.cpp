#include "QMA6100P.h"
#include <Wire.h>

// QMA6100P registers
#define QMA_REG_CHIP_ID   0x00   // -> 0x90
#define QMA_REG_DX_L      0x01   // X..Z, 6 bytes, 14-bit left-justified in 16
#define QMA_REG_FSR       0x0F   // full-scale range
#define QMA_REG_BW        0x10   // bandwidth / ODR
#define QMA_REG_PM        0x11   // power mode
#define QMA_REG_INT_EN0   0x16   // any-motion X/Y/Z enable
#define QMA_REG_INT_MAP0  0x19   // route interrupts to the INT1 pin
#define QMA_REG_INT_CFG   0x20   // INT pin electrical config (level / drive)
#define QMA_REG_INT_LATCH 0x21   // interrupt latch mode
#define QMA_REG_MOT_DUR   0x2C   // any/no-motion duration (samples)
#define QMA_REG_MOT_TH    0x2E   // any-motion threshold (counts)
#define QMA_REG_SR        0x36   // soft reset

#define QMA_CHIP_ID       0x90
#define QMA_FSR_8G        0x04   // +/-8g  -> 1024 counts/g (14-bit)
#define QMA_BW_DEFAULT    0x05
#define QMA_PM_ACTIVE     0x80   // MODE bit = active

// Motion-interrupt config values. DATASHEET-DERIVED AND UNVALIDATED — every one
// that matters is read-back verified in configMotionInterrupt(); a mismatch
// makes the caller fall back to polling, so a wrong guess is always safe.
#define QMA_ANYMOT_XYZ      0x07   // enable any-motion on X, Y and Z
#define QMA_MAP_ANYMOT_INT1 0x04   // map any-motion -> INT1 pin
#define QMA_INT_CFG_PP_AH   0x01   // INT1 push-pull, active-high
#define QMA_INT_LATCH_NONE  0x00   // non-latched (auto-clearing pulse)
#define QMA_MOT_DUR_DEF     0x00   // shortest motion dwell (most responsive wake)

#ifndef PIN_3V3_ACC_EN
  #define PIN_3V3_ACC_EN 39
#endif

bool QMA6100P::writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(_addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;   // non-zero = NACK/bus error -> false
}

bool QMA6100P::writeRegVerify(uint8_t reg, uint8_t val) {
  if (!writeReg(reg, val)) return false;
  uint8_t rb = 0;
  if (!readRegs(reg, &rb, 1)) return false;
  return rb == val;
}

bool QMA6100P::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(_addr);
  Wire.write(reg);
  // Repeated-start; on NACK/bus error bail immediately rather than requesting.
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((int)_addr, (int)len);
  if (got != len) {
    while (Wire.available()) (void)Wire.read();   // drain any partial data
    return false;
  }
  for (uint8_t i = 0; i < len; i++) {
    if (!Wire.available()) return false;          // NEVER block on read()
    buf[i] = Wire.read();
  }
  return true;
}

bool QMA6100P::probeAt(uint8_t addr) {
  _addr = addr;
  uint8_t id = 0;
  if (!readRegs(QMA_REG_CHIP_ID, &id, 1)) return false;
  return id == QMA_CHIP_ID;
}

bool QMA6100P::begin() {
  // Power the accelerometer rail.
  pinMode(PIN_3V3_ACC_EN, OUTPUT);
  digitalWrite(PIN_3V3_ACC_EN, HIGH);
  delay(20);

  Wire.begin();

  // Bound the bus if this core exposes a Wire timeout. The Adafruit nRF52 core
  // does NOT (no WIRE_HAS_TIMEOUT), so this is a no-op there today but stays
  // correct if a future core adds it; the real hang defence is the fast-fail
  // checks above plus MuFall's interrupt-driven, auto-disabling sampling.
#if defined(WIRE_HAS_TIMEOUT)
  Wire.setWireTimeout(3000 /*us*/, true /*reset_on_timeout*/);
#endif

  _present = probeAt(0x12) || probeAt(0x13);
  if (!_present) return false;

  // Soft reset, then configure.
  writeReg(QMA_REG_SR, 0xB6);
  delay(5);
  writeReg(QMA_REG_SR, 0x00);
  delay(5);

  writeReg(QMA_REG_FSR, QMA_FSR_8G);
  writeReg(QMA_REG_BW,  QMA_BW_DEFAULT);
  writeReg(QMA_REG_PM,  QMA_PM_ACTIVE);
  delay(10);
  _counts_per_g = 1024.0f;   // matches +/-8g, 14-bit
  return true;
}

bool QMA6100P::configMotionInterrupt(uint8_t threshold) {
  _motion_int_ok = false;
  if (!_present) return false;

  // Any-motion detection -> INT1, non-latched. See the register comments above:
  // these are datasheet-derived and unvalidated, so the two gating registers
  // (map + enable) are read-back verified and any mismatch returns false.
  writeReg(QMA_REG_MOT_TH,    threshold);
  writeReg(QMA_REG_MOT_DUR,   QMA_MOT_DUR_DEF);
  writeReg(QMA_REG_INT_CFG,   QMA_INT_CFG_PP_AH);
  writeReg(QMA_REG_INT_LATCH, QMA_INT_LATCH_NONE);

  if (!writeRegVerify(QMA_REG_INT_MAP0, QMA_MAP_ANYMOT_INT1)) return false;
  if (!writeRegVerify(QMA_REG_INT_EN0,  QMA_ANYMOT_XYZ))      return false;

  _motion_int_ok = true;
  return true;
}

bool QMA6100P::read(float& gx, float& gy, float& gz) {
  if (!_present) return false;
  uint8_t b[6];
  if (!readRegs(QMA_REG_DX_L, b, 6)) return false;
  // 14-bit signed, left-justified in the 16-bit word (LSB first).
  int16_t rx = (int16_t)((b[1] << 8) | b[0]) >> 2;
  int16_t ry = (int16_t)((b[3] << 8) | b[2]) >> 2;
  int16_t rz = (int16_t)((b[5] << 8) | b[4]) >> 2;
  gx = rx / _counts_per_g;
  gy = ry / _counts_per_g;
  gz = rz / _counts_per_g;
  return true;
}
