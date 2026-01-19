#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncActivity.h"
#include "MappedInputManager.h"
#include "fontIds.h"

#ifdef USE_M5UNIFIED
#include "touch/TouchEvent.h"
#endif

namespace {
// Time threshold for treating a long press as a page-up/page-down
constexpr int SKIP_PAGE_MS = 700;
}  // namespace

bool EpubReaderChapterSelectionActivity::hasSyncOption() const { return KOREADER_STORE.hasCredentials(); }

int EpubReaderChapterSelectionActivity::getTotalItems() const {
  // Add 2 for sync options (top and bottom) if credentials are configured
  const int syncCount = hasSyncOption() ? 2 : 0;
  return epub->getTocItemsCount() + syncCount;
}

bool EpubReaderChapterSelectionActivity::isSyncItem(int index) const {
  if (!hasSyncOption()) return false;
  // First item and last item are sync options
  return index == 0 || index == getTotalItems() - 1;
}

int EpubReaderChapterSelectionActivity::tocIndexFromItemIndex(int itemIndex) const {
  // Account for the sync option at the top
  const int offset = hasSyncOption() ? 1 : 0;
  return itemIndex - offset;
}

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
  // Two-finger tap: go back
  if (event.type == TouchEvent::Type::TwoFingerTap) {
    onGoBack();
    return true;
  }

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

  const int y = event.end.y;

  // Single-finger tap on row: select item
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
  ActivityWithSubactivity::onEnter();

  if (!epub) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();

  // Account for sync option offset when finding current TOC index
  const int syncOffset = hasSyncOption() ? 1 : 0;
  selectorIndex = epub->getTocIndexForSpineIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }
  selectorIndex += syncOffset;  // Offset for top sync option

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
  ActivityWithSubactivity::onExit();

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

void EpubReaderChapterSelectionActivity::launchSyncActivity() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  exitActivity();
  enterNewActivity(new KOReaderSyncActivity(
      renderer, mappedInput, epub, epubPath, currentSpineIndex, currentPage, totalPagesInSpine,
      [this]() {
        // On cancel
        exitActivity();
        updateRequired = true;
      },
      [this](int newSpineIndex, int newPage) {
        // On sync complete
        exitActivity();
        onSyncPosition(newSpineIndex, newPage);
      }));
  xSemaphoreGive(renderingMutex);
}

void EpubReaderChapterSelectionActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

#ifdef USE_M5UNIFIED
  if (pendingActivate) {
    pendingActivate = false;
    if (isSyncItem(selectorIndex)) {
      launchSyncActivity();
      return;
    }
    const int tocIndex = tocIndexFromItemIndex(selectorIndex);
    const auto newSpineIndex = epub->getSpineIndexForTocIndex(tocIndex);
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
  const int totalItems = getTotalItems();

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Check if sync option is selected (first or last item)
    if (isSyncItem(selectorIndex)) {
      launchSyncActivity();
      return;
    }

    // Get TOC index (account for top sync offset)
    const int tocIndex = tocIndexFromItemIndex(selectorIndex);
    const auto newSpineIndex = epub->getSpineIndexForTocIndex(tocIndex);
    if (newSpineIndex == -1) {
      onGoBack();
    } else {
      onSelectSpineIndex(newSpineIndex);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoBack();
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + totalItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + totalItems - 1) % totalItems;
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % totalItems;
    } else {
      selectorIndex = (selectorIndex + 1) % totalItems;
    }
    updateRequired = true;
  }
}

void EpubReaderChapterSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
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
  const int totalItems = getTotalItems();

  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, epub->getTitle().c_str(), pageWidth - 40, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, 15, title.c_str(), true, EpdFontFamily::BOLD);

  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  constexpr int lineHeight = 60;
  renderer.fillRect(0, 60 + (selectorIndex % pageItems) * lineHeight - 2, pageWidth - 1, lineHeight);
  const int textYOffset = (lineHeight - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

  for (int itemIndex = pageStartIndex; itemIndex < totalItems && itemIndex < pageStartIndex + pageItems; itemIndex++) {
    const int displayY = 60 + (itemIndex % pageItems) * lineHeight + textYOffset;
    const bool isSelected = (itemIndex == selectorIndex);

    if (isSyncItem(itemIndex)) {
      // Draw sync option (at top or bottom)
      renderer.drawText(UI_10_FONT_ID, 20, displayY, ">> Sync Progress", !isSelected);
    } else {
      // Draw TOC item (account for top sync offset)
      const int tocIndex = tocIndexFromItemIndex(itemIndex);
      auto item = epub->getTocItem(tocIndex);
      const int indentX = 20 + (item.level - 1) * 15;
      const int maxTextWidth = (pageWidth - 1) - indentX;
      const std::string clipped = renderer.truncatedText(UI_10_FONT_ID, item.title.c_str(), maxTextWidth);
      renderer.drawText(UI_10_FONT_ID, indentX, displayY, clipped.c_str(), !isSelected);
    }
  }

  renderer.displayBuffer();
}
