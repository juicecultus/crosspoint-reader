#pragma once

#include <cstdint>

struct TouchPoint {
  int16_t x;
  int16_t y;
};

struct TouchEvent {
  enum class Type {
    Tap,
    TwoFingerTap,
    LongPress,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
  };

  Type type;
  TouchPoint start;
  TouchPoint end;
  uint32_t durationMs;
};
