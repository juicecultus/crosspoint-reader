#include "TouchManager.h"

#ifdef USE_M5UNIFIED

#include <Arduino.h>
#include <M5Unified.h>

namespace {
constexpr int TAP_MOVE_PX = 18;
constexpr uint32_t TAP_MAX_MS = 300;
constexpr uint32_t LONG_PRESS_MS = 550;
constexpr int SWIPE_MIN_PX = 120;
constexpr int SWIPE_MAX_OFF_AXIS_PX = 90;

static int absInt(const int v) { return v < 0 ? -v : v; }
}  // namespace

void TouchManager::ignoreFor(const uint32_t durationMs) {
  ignoreUntilMs = millis() + durationMs;
  tracking = false;
}

std::optional<TouchEvent> TouchManager::poll() {
  const uint32_t now = millis();
  if (now < ignoreUntilMs) {
    return std::nullopt;
  }

  M5.update();
  auto t = M5.Touch.getDetail();

  const bool down = t.isPressed() || t.isHolding();

  if (!tracking && down) {
    tracking = true;
    start = {static_cast<int16_t>(t.x), static_cast<int16_t>(t.y)};
    startMs = now;
    return std::nullopt;
  }

  if (!tracking) {
    return std::nullopt;
  }

  if (down) {
    return std::nullopt;
  }

  if (!t.isReleased()) {
    return std::nullopt;
  }

  tracking = false;
  const TouchPoint end{static_cast<int16_t>(t.x), static_cast<int16_t>(t.y)};
  const uint32_t dur = now - startMs;

  const int dx = end.x - start.x;
  const int dy = end.y - start.y;
  const int adx = absInt(dx);
  const int ady = absInt(dy);

  if (adx <= TAP_MOVE_PX && ady <= TAP_MOVE_PX) {
    if (dur >= LONG_PRESS_MS) {
      return TouchEvent{TouchEvent::Type::LongPress, start, end, dur};
    }
    if (dur <= TAP_MAX_MS) {
      return TouchEvent{TouchEvent::Type::Tap, start, end, dur};
    }
    return std::nullopt;
  }

  if (ady > adx) {
    if (ady >= SWIPE_MIN_PX && adx <= SWIPE_MAX_OFF_AXIS_PX) {
      return TouchEvent{dy < 0 ? TouchEvent::Type::SwipeUp : TouchEvent::Type::SwipeDown, start, end, dur};
    }
  } else {
    if (adx >= SWIPE_MIN_PX && ady <= SWIPE_MAX_OFF_AXIS_PX) {
      return TouchEvent{dx < 0 ? TouchEvent::Type::SwipeLeft : TouchEvent::Type::SwipeRight, start, end, dur};
    }
  }

  return std::nullopt;
}

#endif
