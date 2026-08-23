#include "QMA6100P.h"
#include <Wire.h>

// QMA6100P registers
#define QMA_REG_CHIP_ID   0x00   // -> 0x90
#define QMA_REG_DX_L      0x01   // X..Z, 6 bytes, 14-bit left-justified in 16
#define QMA_REG_FSR       0x0F   // full-scale range
#define QMA_REG_BW        0x10   // bandwidth / ODR
#define QMA_REG_PM        0x11   // power mode
#define QMA_REG_SR        0x36   // soft reset

#define QMA_CHIP_ID       0x90
#define QMA_FSR_8G        0x04   // +/-8g  -> 1024 counts/g (14-bit)
#define QMA_BW_DEFAULT    0x05
#define QMA_PM_ACTIVE     0x80   // MODE bit = active

#ifndef PIN_3V3_ACC_EN
  #define PIN_3V3_ACC_EN 39
#endif

bool QMA6100P::writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(_addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool QMA6100P::readRegs(uint8_t reg, uint8_t* buf, uint8_t len) {
  Wire.beginTransmission(_addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  uint8_t got = Wire.requestFrom((int)_addr, (int)len);
  if (got != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
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
