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

bool HalTiltSensor::readGyro(float& gx, float& gy, float& gz) const {
  Wire.beginTransmission(_i2cAddr);
  Wire.write(REG_GX_L);  // Start reading at Gyro X Low
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

  // If Full Scale is ±512 dps, the scale factor is 32768 / 512 = 64 LSB/dps
  constexpr float SCALE = 1.0f / 64.0f;
  gx = readInt16() * SCALE;
  gy = readInt16() * SCALE;
  gz = readInt16() * SCALE;
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
  if (!readReg(QMI8658_WHO_AM_I_REG, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
    _i2cAddr = I2C_ADDR_QMI8658_ALT;
    if (!readReg(QMI8658_WHO_AM_I_REG, &whoami) || whoami != QMI8658_WHO_AM_I_VALUE) {
      LOG_INF("TILT", "QMI8658 IMU not found");
      _available = false;
      return;
    }
  }

  LOG_INF("TILT", "QMI8658 IMU found at 0x%02X", _i2cAddr);

  // CTRL1: enable address auto-increment (bit 6) AND set SensorDisable (bit 0)
  if (!writeReg(REG_CTRL1, 0x41) ||
      // CTRL3: gyro config — ±512dps full scale (101), ODR 28.025 Hz (1000)
      !writeReg(REG_CTRL3, 0x58) ||
      // CTRL7: Initialize in SLEEP mode (0x00)
      !writeReg(REG_CTRL7, 0x00)) {
    LOG_INF("TILT", "QMI8658 register configuration failed");
    _available = false;
    return;
  }

  _available = true;
  _lastPollMs = millis();
  LOG_INF("TILT", "QMI8658 gyro initialized and put to sleep");
}

void HalTiltSensor::wake() {
  if (!_available) {
    return;
  }

  // REG_CTRL1 (0x02): Writing 0x40 clears SensorDisable (bit 0)
  // REG_CTRL7 (0x08): Write 0x02 to re-enable the Gyroscope
  if (writeReg(REG_CTRL1, 0x40) && writeReg(REG_CTRL7, 0x02)) {
    _lastPollMs = millis();
    _lastTiltMs = millis();  // Reset cooldown
    LOG_INF("TILT", "QMI8658 woke up");
  } else {
    LOG_INF("TILT", "Failed to wake QMI8658");
  }
}

void HalTiltSensor::deepSleep() {
  if (!_available) {
    return;
  }

  // REG_CTRL7 (0x08): Writing 0x00 disables both Accel and Gyro
  // REG_CTRL1 (0x02): Writing 0x41 enables SensorDisable (bit 0)
  if (writeReg(REG_CTRL7, 0x00) && writeReg(REG_CTRL1, 0x41)) {
    LOG_INF("TILT", "QMI8658 entered sleep mode");
  } else {
    LOG_INF("TILT", "Failed to put QMI8658 to sleep");
  }

  // Clear any residual state so it doesn't immediately trigger upon waking
  clearPendingEvents();
  _inTilt = false;
}

void HalTiltSensor::update(bool enabled, uint8_t orientation) {
  if (!_available) {
    return;
  }

  // State machine: wake up or sleep based on the enabled flag
  if (enabled && !_isAwake) {
    wake();
    _isAwake = true;
    return;
  } else if (!enabled && _isAwake) {
    deepSleep();
    _isAwake = false;
    return;
  }

  // If disabled, skip the rest of the polling logic
  if (!enabled) {
    return;
  }

  const unsigned long now = millis();
  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  float gx, gy, gz;
  if (!readGyro(gx, gy, gz)) {
    return;
  }

  // Map the gyro axis to left/right tilt based on reader orientation.
  // On the X3 PCB: X axis = left/right in portrait, Y axis = left/right in landscape.
  float tiltAxis;
  switch (orientation) {
    case CrossPointOrientation::PORTRAIT:
      tiltAxis = gx;
      break;
    case CrossPointOrientation::INVERTED:
      tiltAxis = -gx;
      break;
    case CrossPointOrientation::LANDSCAPE_CW:
      tiltAxis = -gy;
      break;
    case CrossPointOrientation::LANDSCAPE_CCW:
      tiltAxis = gy;
      break;
    default:
      tiltAxis = gx;
      break;
  }

  if (_inTilt) {
    // Wait for device to return to neutral before allowing next trigger
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) {
      _inTilt = false;
    }
  } else {
    // Check for new tilt gesture (with cooldown)
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > RATE_THRESHOLD_DPS) {
        _tiltForwardEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
      } else if (tiltAxis < -RATE_THRESHOLD_DPS) {
        _tiltBackEvent = true;
        _hadActivity = true;
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

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _tiltForwardEvent = false;
  _tiltBackEvent = false;
  _hadActivity = false;
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}
