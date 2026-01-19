#pragma once

#ifdef USE_M5UNIFIED

#include <cstdint>
#include <optional>

#include "TouchEvent.h"

class TouchManager {
 public:
  TouchManager() = default;

  void ignoreFor(uint32_t durationMs);
  std::optional<TouchEvent> poll();

 private:
  bool tracking = false;
  TouchPoint start{0, 0};
  uint32_t startMs = 0;
  uint32_t ignoreUntilMs = 0;
  uint8_t maxFingers = 1;
};

#endif
