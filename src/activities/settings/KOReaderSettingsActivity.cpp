#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>

#include <cstring>

#ifdef USE_M5UNIFIED
#include "touch/TouchEvent.h"
#endif

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 5;
constexpr int LINE_HEIGHT = 60;
constexpr int START_Y = 60;
const char* menuNames[MENU_ITEMS] = {"Username", "Password", "Sync Server URL", "Document Matching", "Authenticate"};
}  // namespace

#ifdef USE_M5UNIFIED
bool KOReaderSettingsActivity::onTouch(const TouchEvent& event) {
  if (subActivity) {
    return subActivity->onTouch(event);
  }

  // Two-finger tap: go back
  if (event.type == TouchEvent::Type::TwoFingerTap) {
    onBack();
    return true;
  }

  if (event.type == TouchEvent::Type::SwipeUp) {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    updateRequired = true;
    return true;
  }

  if (event.type == TouchEvent::Type::SwipeDown) {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    updateRequired = true;
    return true;
  }

  if (event.type != TouchEvent::Type::Tap) {
    return false;
  }

  // Single-finger tap on row: select item
  const int y = event.end.y;
  if (y >= START_Y && y < START_Y + MENU_ITEMS * LINE_HEIGHT) {
    const int tappedIndex = (y - START_Y) / LINE_HEIGHT;
    if (tappedIndex == selectedIndex) {
      handleSelection();
    } else {
      selectedIndex = tappedIndex;
      updateRequired = true;
    }
    return true;
  }

  return false;
}
#endif

void KOReaderSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KOReaderSettingsActivity*>(param);
  self->displayTaskLoop();
}

void KOReaderSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  updateRequired = true;

  xTaskCreate(&KOReaderSettingsActivity::taskTrampoline, "KOReaderSettingsTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void KOReaderSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void KOReaderSettingsActivity::loop() {
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

void KOReaderSettingsActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  if (selectedIndex == 0) {
    // Username
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "KOReader Username", KOREADER_STORE.getUsername(), 10,
        64,     // maxLength
        false,  // not password
        [this](const std::string& username) {
          KOREADER_STORE.setCredentials(username, KOREADER_STORE.getPassword());
          KOREADER_STORE.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 1) {
    // Password
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "KOReader Password", KOREADER_STORE.getPassword(), 10,
        64,     // maxLength
        false,  // show characters
        [this](const std::string& password) {
          KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), password);
          KOREADER_STORE.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 2) {
    // Sync Server URL - prefill with https:// if empty to save typing
    const std::string currentUrl = KOREADER_STORE.getServerUrl();
    const std::string prefillUrl = currentUrl.empty() ? "https://" : currentUrl;
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Sync Server URL", prefillUrl, 10,
        128,    // maxLength - URLs can be long
        false,  // not password
        [this](const std::string& url) {
          // Clear if user just left the prefilled https://
          const std::string urlToSave = (url == "https://" || url == "http://") ? "" : url;
          KOREADER_STORE.setServerUrl(urlToSave);
          KOREADER_STORE.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  } else if (selectedIndex == 3) {
    // Document Matching - toggle between Filename and Binary
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    KOREADER_STORE.saveToFile();
    updateRequired = true;
  } else if (selectedIndex == 4) {
    // Authenticate
    if (!KOREADER_STORE.hasCredentials()) {
      // Can't authenticate without credentials - just show message briefly
      xSemaphoreGive(renderingMutex);
      return;
    }
    exitActivity();
    enterNewActivity(new KOReaderAuthActivity(renderer, mappedInput, [this] {
      exitActivity();
      updateRequired = true;
    }));
  }

  xSemaphoreGive(renderingMutex);
}

void KOReaderSettingsActivity::displayTaskLoop() {
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

void KOReaderSettingsActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "KOReader Sync", true, EpdFontFamily::BOLD);

  // Draw selection highlight
  const int textYOffset = (LINE_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2;
  renderer.fillRect(0, START_Y + selectedIndex * LINE_HEIGHT - 2, pageWidth - 1, LINE_HEIGHT);

  // Draw menu items
  for (int i = 0; i < MENU_ITEMS; i++) {
    const int settingY = START_Y + i * LINE_HEIGHT + textYOffset;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, menuNames[i], !isSelected);

    // Draw status for each item
    const char* status = "";
    if (i == 0) {
      status = KOREADER_STORE.getUsername().empty() ? "[Not Set]" : "[Set]";
    } else if (i == 1) {
      status = KOREADER_STORE.getPassword().empty() ? "[Not Set]" : "[Set]";
    } else if (i == 2) {
      status = KOREADER_STORE.getServerUrl().empty() ? "[Not Set]" : "[Set]";
    } else if (i == 3) {
      status = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? "[Filename]" : "[Binary]";
    } else if (i == 4) {
      status = KOREADER_STORE.hasCredentials() ? "" : "[Set credentials first]";
    }

    const auto width = renderer.getTextWidth(UI_10_FONT_ID, status);
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
