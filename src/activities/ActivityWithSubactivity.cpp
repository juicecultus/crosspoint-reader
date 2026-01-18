#include "ActivityWithSubactivity.h"

#ifdef USE_M5UNIFIED
#include "touch/TouchEvent.h"
#endif

void ActivityWithSubactivity::exitActivity() {
  if (subActivity) {
    subActivity->onExit();
    subActivity.reset();
  }
}

void ActivityWithSubactivity::enterNewActivity(Activity* activity) {
  subActivity.reset(activity);
  subActivity->onEnter();
}

void ActivityWithSubactivity::loop() {
  if (subActivity) {
    subActivity->loop();
  }
}

void ActivityWithSubactivity::requestRedraw() {
  if (subActivity) {
    subActivity->requestRedraw();
  }
}

void ActivityWithSubactivity::onExit() {
  Activity::onExit();
  exitActivity();
}

#ifdef USE_M5UNIFIED
bool ActivityWithSubactivity::onTouch(const TouchEvent& event) {
  if (subActivity) {
    return subActivity->onTouch(event);
  }
  return false;
}
#endif
