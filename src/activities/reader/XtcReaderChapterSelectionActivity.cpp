#include "XtcReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

#ifdef USE_M5UNIFIED
#include "touch/TouchEvent.h"
#endif

namespace {
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

int XtcReaderChapterSelectionActivity::getPageItems() const {
  constexpr int startY = 60;
  constexpr int lineHeight = 30;

  const int screenHeight = renderer.getScreenHeight();
  const int endY = screenHeight - lineHeight;

  const int availableHeight = endY - startY;
  int items = availableHeight / lineHeight;
  if (items < 1) {
    items = 1;
  }

  return items;
}

#ifdef USE_M5UNIFIED
bool XtcReaderChapterSelectionActivity::onTouch(const TouchEvent& event) {
  if (!xtc) {
    return false;
  }

  if (event.type == TouchEvent::Type::SwipeUp) {
    const int total = static_cast<int>(xtc->getChapters().size());
    if (total <= 0) {
      return true;
    }
    selectorIndex = (selectorIndex + total - 1) % total;
    updateRequired = true;
    return true;
  }

  if (event.type == TouchEvent::Type::SwipeDown) {
    const int total = static_cast<int>(xtc->getChapters().size());
    if (total <= 0) {
      return true;
    }
    selectorIndex = (selectorIndex + 1) % total;
    updateRequired = true;
    return true;
  }

  if (event.type != TouchEvent::Type::Tap) {
    return false;
  }

  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int x = event.end.x;
  const int y = event.end.y;

  // Bottom-left: Back
  if (y > h - 80 && x < w / 3) {
    onGoBack();
    return true;
  }

  // Rows: startY=60, lineHeight=30
  constexpr int startY = 60;
  constexpr int lineHeight = 30;
  if (y < startY) {
    return false;
  }

  const int pageItems = getPageItems();
  const int row = (y - startY) / lineHeight;
  if (row < 0 || row >= pageItems) {
    return false;
  }

  const int pageStartIndex = selectorIndex / pageItems * pageItems;
  const int tappedIndex = pageStartIndex + row;
  const int total = static_cast<int>(xtc->getChapters().size());
  if (tappedIndex >= 0 && tappedIndex < total) {
    selectorIndex = tappedIndex;
    pendingActivate = true;
    updateRequired = true;
    return true;
  }

  return false;
}
#endif

int XtcReaderChapterSelectionActivity::findChapterIndexForPage(uint32_t page) const {
  if (!xtc) {
    return 0;
  }

  const auto& chapters = xtc->getChapters();
  for (size_t i = 0; i < chapters.size(); i++) {
    if (page >= chapters[i].startPage && page <= chapters[i].endPage) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

void XtcReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void XtcReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = findChapterIndexForPage(currentPage);

  updateRequired = true;
  xTaskCreate(&XtcReaderChapterSelectionActivity::taskTrampoline, "XtcReaderChapterSelectionActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void XtcReaderChapterSelectionActivity::onExit() {
  Activity::onExit();

  if (renderingMutex) {
    const bool locked = xSemaphoreTake(renderingMutex, pdMS_TO_TICKS(2000)) == pdTRUE;
    if (displayTaskHandle) {
      vTaskDelete(displayTaskHandle);
      displayTaskHandle = nullptr;
    }
    if (locked) {
      vSemaphoreDelete(renderingMutex);
    }
    renderingMutex = nullptr;
  } else if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
}

void XtcReaderChapterSelectionActivity::requestRedraw() {
  updateRequired = true;
}

void XtcReaderChapterSelectionActivity::loop() {
#ifdef USE_M5UNIFIED
  if (pendingActivate) {
    pendingActivate = false;
    const auto& chapters = xtc->getChapters();
    if (!chapters.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(chapters.size())) {
      onSelectPage(chapters[selectorIndex].startPage);
    }
    return;
  }
#endif

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;
  const int pageItems = getPageItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto& chapters = xtc->getChapters();
    if (!chapters.empty() && selectorIndex >= 0 && selectorIndex < static_cast<int>(chapters.size())) {
      onSelectPage(chapters[selectorIndex].startPage);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    const int total = static_cast<int>(xtc->getChapters().size());
    if (total == 0) {
      return;
    }
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + total) % total;
    } else {
      selectorIndex = (selectorIndex + total - 1) % total;
    }
    updateRequired = true;
  } else if (nextReleased) {
    const int total = static_cast<int>(xtc->getChapters().size());
    if (total == 0) {
      return;
    }
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % total;
    } else {
      selectorIndex = (selectorIndex + 1) % total;
    }
    updateRequired = true;
  }
}

void XtcReaderChapterSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void XtcReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Select Chapter", true, EpdFontFamily::BOLD);

  const auto& chapters = xtc->getChapters();
  if (chapters.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, 120, "No chapters");
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(0, 60 + (selectorIndex % pageItems) * 30 - 2, pageWidth - 1, 30);
  for (int i = pageStartIndex; i < static_cast<int>(chapters.size()) && i < pageStartIndex + pageItems; i++) {
    const auto& chapter = chapters[i];
    const char* title = chapter.name.empty() ? "Unnamed" : chapter.name.c_str();
    const int indentX = 20;
    const int maxTextWidth = (pageWidth - 1) - indentX;
    const std::string clipped = renderer.truncatedText(UI_10_FONT_ID, title, maxTextWidth);
    renderer.drawText(UI_10_FONT_ID, indentX, 60 + (i % pageItems) * 30, clipped.c_str(), i != selectorIndex);
  }

  const auto labels = mappedInput.mapLabels("« Back", "Select", "Up", "Down");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
