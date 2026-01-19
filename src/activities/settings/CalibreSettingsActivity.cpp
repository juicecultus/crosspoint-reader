#include "CalibreSettingsActivity.h"

#include <GfxRenderer.h>
#include <WiFi.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/CalibreWirelessActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "fontIds.h"

#ifdef USE_M5UNIFIED
#include "touch/TouchEvent.h"
#endif

namespace {
constexpr int MENU_ITEMS = 2;
constexpr int LINE_HEIGHT = 60;
constexpr int START_Y = 60;
const char* menuNames[MENU_ITEMS] = {"Calibre Web URL", "Connect as Wireless Device"};
}  // namespace

#ifdef USE_M5UNIFIED
bool CalibreSettingsActivity::onTouch(const TouchEvent& event) {
  if (subActivity) {
    return subActivity->onTouch(event);
  }

  if (event.type == TouchEvent::Type::SwipeUp) {
    const int step = MENU_ITEMS;
    selectedIndex = (selectedIndex + MENU_ITEMS - step) % MENU_ITEMS;
    updateRequired = true;
    return true;
  }

  if (event.type == TouchEvent::Type::SwipeDown) {
    const int step = MENU_ITEMS;
    selectedIndex = (selectedIndex + step) % MENU_ITEMS;
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
    onBack();
    return true;
  }

  // Menu rows
  if (y >= START_Y) {
    const int idx = (y - START_Y) / LINE_HEIGHT;
    if (idx >= 0 && idx < MENU_ITEMS) {
      selectedIndex = idx;
      handleSelection();
      updateRequired = true;
      return true;
    }
  }

  return false;
}
#endif

void CalibreSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<CalibreSettingsActivity*>(param);
  self->displayTaskLoop();
}

void CalibreSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  updateRequired = true;

  xTaskCreate(&CalibreSettingsActivity::taskTrampoline, "CalibreSettingsTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void CalibreSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void CalibreSettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    updateRequired = true;
  }
}

void CalibreSettingsActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  if (selectedIndex == 0) {
    // Calibre Web URL
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Calibre Web URL", SETTINGS.opdsServerUrl, 10,
        127,    // maxLength
        false,  // not password
        [this](const std::string& url) {
          strncpy(SETTINGS.opdsServerUrl, url.c_str(), sizeof(SETTINGS.opdsServerUrl) - 1);
          SETTINGS.opdsServerUrl[sizeof(SETTINGS.opdsServerUrl) - 1] = '\0';
          SETTINGS.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 1) {
    // Wireless Device - launch the activity (handles WiFi connection internally)
    exitActivity();
    if (WiFi.status() != WL_CONNECTED) {
      enterNewActivity(new WifiSelectionActivity(renderer, mappedInput, [this](bool connected) {
        exitActivity();
        if (connected) {
          enterNewActivity(new CalibreWirelessActivity(renderer, mappedInput, [this] {
            exitActivity();
            updateRequired = true;
          }));
        } else {
          updateRequired = true;
        }
      }));
    } else {
      enterNewActivity(new CalibreWirelessActivity(renderer, mappedInput, [this] {
        exitActivity();
        updateRequired = true;
      }));
    }
  }

  xSemaphoreGive(renderingMutex);
}

void CalibreSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void CalibreSettingsActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Calibre", true, EpdFontFamily::BOLD);

  // Draw selection highlight
  const int textYOffset = (LINE_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.fillRect(0, START_Y + selectedIndex * LINE_HEIGHT - 2, pageWidth - 1, LINE_HEIGHT);

  // Draw menu items
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int settingY = START_Y + i * LINE_HEIGHT + textYOffset;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, menuNames[i], !isSelected);

    // Draw status for URL setting
    if (i == 0) {
      const char* status = (strlen(SETTINGS.opdsServerUrl) > 0) ? "[Set]" : "[Not Set]";
      const auto width = renderer.getTextWidth(UI_10_FONT_ID, status);
      renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
    }
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels("", "", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
