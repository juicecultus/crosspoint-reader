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
  
  // Check touch count for multi-finger detection
  const uint8_t touchCount = M5.Touch.getCount();
  auto t = M5.Touch.getDetail();

  // Debug: log touch state periodically
  static uint32_t lastDebug = 0;
  static uint32_t lastAnyLog = 0;
  if (t.isPressed() && now - lastDebug > 500) {
    lastDebug = now;
    lastAnyLog = now;
    Serial.printf("[%lu] [TCH] Touch: x=%d y=%d pressed=%d holding=%d released=%d count=%d\n", 
                  now, t.x, t.y, t.isPressed(), t.isHolding(), t.isReleased(), touchCount);
  }
  // Log heartbeat every 30 seconds to confirm touch polling is still running
  if (now - lastAnyLog > 30000) {
    lastAnyLog = now;
    Serial.printf("[%lu] [TCH] Heartbeat: polling active, tracking=%d\n", now, tracking);
  }

  const bool down = t.isPressed() || t.isHolding();

  if (!tracking && down) {
    tracking = true;
    start = {static_cast<int16_t>(t.x), static_cast<int16_t>(t.y)};
    startMs = now;
    maxFingers = touchCount;
    return std::nullopt;
  }

  if (!tracking) {
    return std::nullopt;
  }

  // Track maximum finger count during this gesture
  if (touchCount > maxFingers) {
    maxFingers = touchCount;
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
  const uint8_t fingers = maxFingers;
  maxFingers = 1;

  const int dx = end.x - start.x;
  const int dy = end.y - start.y;
  const int adx = absInt(dx);
  const int ady = absInt(dy);

  if (adx <= TAP_MOVE_PX && ady <= TAP_MOVE_PX) {
    if (dur >= LONG_PRESS_MS) {
      return TouchEvent{TouchEvent::Type::LongPress, start, end, dur};
    }
    if (dur <= TAP_MAX_MS) {
      // Two-finger tap for back navigation
      if (fingers >= 2) {
        return TouchEvent{TouchEvent::Type::TwoFingerTap, start, end, dur};
      }
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
