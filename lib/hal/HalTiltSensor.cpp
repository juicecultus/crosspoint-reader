#include "HalTiltSensor.h"

#include <Logging.h>

HalTiltSensor halTiltSensor;  // Singleton instance

bool HalTiltSensor::writeReg(uint8_t reg, uint8_t val) const {
  Wire.beginTransmission(_i2cAddr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

bool HalTiltSensor::readReg(uint8_t reg, uint8_t* val) const {
  Wire.beginTransmission(_i2cAddr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  Wire.requestFrom(_i2cAddr, (uint8_t)1);
  if (Wire.available() < 1) {
    return false;
  }
  *val = Wire.read();
  return true;
}

bool HalTiltSensor::readAccel(float& ax, float& ay, float& az) const {
  Wire.beginTransmission(_i2cAddr);
  Wire.write(REG_AX_L);
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  Wire.requestFrom(_i2cAddr, (uint8_t)6);
  if (Wire.available() < 6) {
    return false;
  }

  auto readInt16 = [&]() -> int16_t {
    const uint8_t lo = Wire.read();
    const uint8_t hi = Wire.read();
    return static_cast<int16_t>((hi << 8) | lo);
  };

  // ±2g scale: 16384 LSB/g
  constexpr float SCALE = 1.0f / 16384.0f;
  ax = readInt16() * SCALE;
  ay = readInt16() * SCALE;
  az = readInt16() * SCALE;
  return true;
}

void HalTiltSensor::begin() {
  if (!gpio.deviceIsX3()) {
    _available = false;
    return;
  }

  // Try primary address, then alternate
  uint8_t whoami = 0;
  _i2cAddr = I2C_ADDR_QMI8658;
  if (!readReg(REG_WHO_AM_I, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
    _i2cAddr = I2C_ADDR_QMI8658_ALT;
    if (!readReg(REG_WHO_AM_I, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
      LOG_INF("TILT", "QMI8658 IMU not found");
      _available = false;
      return;
    }
  }

  LOG_INF("TILT", "QMI8658 IMU found at 0x%02X", _i2cAddr);

  // CTRL1: enable address auto-increment (bit 6)
  writeReg(REG_CTRL1, 0x40);

  // CTRL2: accelerometer config — ±2g full scale (000), ODR 31.25 Hz (1000)
  //   [6:4]=000 (±2g), [3:0]=1000 (31.25 Hz)
  writeReg(REG_CTRL2, 0x08);

  // CTRL7: enable accelerometer only (bit 0), gyro disabled (bit 1 = 0)
  writeReg(REG_CTRL7, 0x01);

  _available = true;
  _lastPollMs = millis();
  LOG_INF("TILT", "QMI8658 accelerometer initialized (±2g, 31.25 Hz)");
}

void HalTiltSensor::update() {
  if (!_available) {
    return;
  }

  const unsigned long now = millis();
  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  float ax, ay, az;
  if (!readAccel(ax, ay, az)) {
    return;
  }

  // On the X3 PCB the accelerometer Y axis is aligned with left/right tilt
  // when the device is held upright in portrait mode.
  // Positive Y = tilted right, negative Y = tilted left.
  const float tiltAxis = ay;

  if (_inTilt) {
    // Wait for device to return to neutral before allowing next trigger
    if (fabsf(tiltAxis) < NEUTRAL_THRESHOLD_G) {
      _inTilt = false;
    }
  } else {
    // Check for new tilt gesture (with cooldown)
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > TILT_THRESHOLD_G) {
        _tiltForwardEvent = true;
        _inTilt = true;
        _lastTiltMs = now;
      } else if (tiltAxis < -TILT_THRESHOLD_G) {
        _tiltBackEvent = true;
        _inTilt = true;
        _lastTiltMs = now;
      }
    }
  }
}

bool HalTiltSensor::wasTiltedForward() {
  const bool val = _tiltForwardEvent;
  _tiltForwardEvent = false;
  return val;
}

bool HalTiltSensor::wasTiltedBack() {
  const bool val = _tiltBackEvent;
  _tiltBackEvent = false;
  return val;
}
