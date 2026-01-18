#pragma once

#ifdef USE_M5UNIFIED
#include <M5Unified.h>

#include <cstdint>

class BatteryMonitor {
 public:
  explicit BatteryMonitor(uint8_t /*adcPin*/, float /*dividerMultiplier*/ = 2.0f) {}

  uint16_t readPercentage() const {
    const int lvl = M5.Power.getBatteryLevel();
    if (lvl < 0) return 0;
    if (lvl > 100) return 100;
    return static_cast<uint16_t>(lvl);
  }

  uint16_t readMillivolts() const {
    const int mv = M5.Power.getBatteryVoltage();
    return static_cast<uint16_t>(mv < 0 ? 0 : mv);
  }
  uint16_t readRawMillivolts() const { return readMillivolts(); }
  double readVolts() const { return static_cast<double>(readMillivolts()) / 1000.0; }

  static uint16_t percentageFromMillivolts(uint16_t /*millivolts*/) { return 0; }
  static uint16_t millivoltsFromRawAdc(uint16_t /*adc_raw*/) { return 0; }
};

#define BAT_GPIO0 0  // Battery voltage

static BatteryMonitor battery(BAT_GPIO0);

#else

#include <BatteryMonitor.h>

#define BAT_GPIO0 0  // Battery voltage

static BatteryMonitor battery(BAT_GPIO0);

#endif
