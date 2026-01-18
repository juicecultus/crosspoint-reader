#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "MappedInputManager.h"
#include "fontIds.h"

#ifdef USE_M5UNIFIED
#include "touch/TouchEvent.h"
#endif

namespace {
// Time threshold for treating a long press as a page-up/page-down
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

int EpubReaderChapterSelectionActivity::getPageItems() const {
  // Layout constants used in renderScreen
  constexpr int startY = 60;
  constexpr int lineHeight = 60;

  const int screenHeight = renderer.getScreenHeight();
  const int endY = screenHeight;

  const int availableHeight = endY - startY;
  int items = availableHeight / lineHeight;

  // Ensure we always have at least one item per page to avoid division by zero
  if (items < 1) {
    items = 1;
  }

  return items;
}

#ifdef USE_M5UNIFIED
bool EpubReaderChapterSelectionActivity::onTouch(const TouchEvent& event) {
  if (event.type == TouchEvent::Type::SwipeUp) {
    const int total = epub ? epub->getTocItemsCount() : 0;
    if (total <= 0) {
      return true;
    }
    const int pageItems = getPageItems();
    selectorIndex = (selectorIndex + total - pageItems) % total;
    updateRequired = true;
    return true;
  }

  if (event.type == TouchEvent::Type::SwipeDown) {
    const int total = epub ? epub->getTocItemsCount() : 0;
    if (total <= 0) {
      return true;
    }
    const int pageItems = getPageItems();
    selectorIndex = (selectorIndex + pageItems) % total;
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
  constexpr int lineHeight = 60;
  if (!epub || y < startY) {
    return false;
  }

  const int pageItems = getPageItems();
  const int row = (y - startY) / lineHeight;
  if (row < 0 || row >= pageItems) {
    return false;
  }

  const int pageStartIndex = selectorIndex / pageItems * pageItems;
  const int tappedIndex = pageStartIndex + row;
  if (tappedIndex >= 0 && tappedIndex < epub->getTocItemsCount()) {
    selectorIndex = tappedIndex;
    pendingActivate = true;
    updateRequired = true;
    return true;
  }

  return false;
}
#endif

void EpubReaderChapterSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<EpubReaderChapterSelectionActivity*>(param);
  self->displayTaskLoop();
}

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  // Trigger first update
  updateRequired = true;
  xTaskCreate(&EpubReaderChapterSelectionActivity::taskTrampoline, "EpubReaderChapterSelectionActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void EpubReaderChapterSelectionActivity::onExit() {
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

void EpubReaderChapterSelectionActivity::requestRedraw() {
  updateRequired = true;
}

void EpubReaderChapterSelectionActivity::loop() {
#ifdef USE_M5UNIFIED
  if (pendingActivate) {
    pendingActivate = false;
    const auto newSpineIndex = epub->getSpineIndexForTocIndex(selectorIndex);
    if (newSpineIndex == -1) {
      onGoBack();
    } else {
      onSelectSpineIndex(newSpineIndex);
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
    const auto newSpineIndex = epub->getSpineIndexForTocIndex(selectorIndex);
    if (newSpineIndex == -1) {
      onGoBack();
    } else {
      onSelectSpineIndex(newSpineIndex);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex =
          ((selectorIndex / pageItems - 1) * pageItems + epub->getTocItemsCount()) % epub->getTocItemsCount();
    } else {
      selectorIndex = (selectorIndex + epub->getTocItemsCount() - 1) % epub->getTocItemsCount();
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % epub->getTocItemsCount();
    } else {
      selectorIndex = (selectorIndex + 1) % epub->getTocItemsCount();
    }
    updateRequired = true;
  }
}

void EpubReaderChapterSelectionActivity::displayTaskLoop() {
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

void EpubReaderChapterSelectionActivity::renderScreen() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const int pageItems = getPageItems();

  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, epub->getTitle().c_str(), pageWidth - 40, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title.c_str(), true, EpdFontFamily::BOLD);

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  renderer.fillRect(0, 60 + (selectorIndex % pageItems) * 60 - 2, pageWidth - 1, 60);
  const int textYOffset = (60 - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  for (int tocIndex = pageStartIndex; tocIndex < epub->getTocItemsCount() && tocIndex < pageStartIndex + pageItems;
       tocIndex++) {
    auto item = epub->getTocItem(tocIndex);
    const int indentX = 20 + (item.level - 1) * 15;
    const int maxTextWidth = (pageWidth - 1) - indentX;
    const std::string clipped = renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), maxTextWidth);
    renderer.drawText(UI_10_FONT_ID, indentX, 60 + (tocIndex % pageItems) * 60 + textYOffset, clipped.c_str(),
                      tocIndex != selectorIndex);
  }

  renderer.displayBuffer();
}
