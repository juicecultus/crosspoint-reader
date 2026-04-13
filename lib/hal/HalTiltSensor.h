#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "HalGPIO.h"

// Forward-declare orientation enum to avoid circular include
namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;  // Singleton

class HalTiltSensor {
  bool _available = false;
  uint8_t _i2cAddr = 0;

  // Tilt gesture state machine
  bool _tiltForwardEvent = false;  // Consumed by wasTiltedForward()
  bool _tiltBackEvent = false;     // Consumed by wasTiltedBack()
  bool _hadActivity = false;       // Non-consuming flag for sleep timer
  bool _inTilt = false;            // Currently tilted past threshold
  bool _isAwake = false;           // Tracks power state
  unsigned long _lastTiltMs = 0;   // Debounce / cooldown

  // Tuning constants
  static constexpr float RATE_THRESHOLD_DPS = 270.0f;    // Deg/sec speed to trigger flick
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;       // Must stop moving below this rate before next trigger
  static constexpr unsigned long COOLDOWN_MS = 600;      // Minimum ms between triggers
  static constexpr unsigned long POLL_INTERVAL_MS = 50;  // 20 Hz polling

  mutable unsigned long _lastPollMs = 0;

  // QMI8658 registers
  static constexpr uint8_t REG_CTRL1 = 0x02;
  static constexpr uint8_t REG_CTRL3 = 0x04;
  static constexpr uint8_t REG_CTRL7 = 0x08;
  static constexpr uint8_t REG_GX_L = 0x3B;

  bool writeReg(uint8_t reg, uint8_t val) const;
  bool readReg(uint8_t reg, uint8_t* val) const;
  bool readGyro(float& gx, float& gy, float& gz) const;

 public:
  // Call after gpio.begin() and powerManager.begin() (I2C already initialised for X3)
  void begin();

  // Enables the QMI8658 internal sensor engine
  void wake();

  // Puts the QMI8658 into a low-power standby state
  void deepSleep();

  // True if the QMI8658 IMU is present on this device
  bool isAvailable() const { return _available; }

  // Poll the accelerometer and update tilt gesture state.
  // Only polls when enabled. Pass current reader orientation for correct axis mapping.
  void update(bool enabled, uint8_t orientation = CrossPointOrientation::PORTRAIT);

  // Returns true once per tilt-forward gesture (next page direction).
  // Consumed on read — subsequent calls return false until next gesture.
  bool wasTiltedForward();

  // Returns true once per tilt-back gesture (previous page direction).
  // Consumed on read.
  bool wasTiltedBack();

  // Non-consuming: true if any tilt activity occurred since last call.
  // Used to reset the auto-sleep inactivity timer.
  bool hadActivity();

  // Discard any pending tilt events (call when leaving reader or disabling tilt).
  void clearPendingEvents();
};
