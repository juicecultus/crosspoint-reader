#include "FileSelectionActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/StringUtils.h"

#ifdef USE_M5UNIFIED
#include "touch/TouchEvent.h"
#endif

namespace {
constexpr int START_Y = 60;
constexpr int LINE_HEIGHT = 60;
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

#ifdef USE_M5UNIFIED
bool FileSelectionActivity::onTouch(const TouchEvent& event) {
  if (event.type == TouchEvent::Type::SwipeUp) {
    if (files.empty()) {
      return true;
    }
    const size_t pageItems = static_cast<size_t>(getPageItems());
    const size_t step = std::min(pageItems, files.size());
    selectorIndex = (selectorIndex + files.size() - step) % files.size();
    updateRequired = true;
    return true;
  }

  if (event.type == TouchEvent::Type::SwipeDown) {
    if (files.empty()) {
      return true;
    }
    const size_t pageItems = static_cast<size_t>(getPageItems());
    const size_t step = std::min(pageItems, files.size());
    selectorIndex = (selectorIndex + step) % files.size();
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

  // Bottom-left: go up one directory (or home if at root)
  if (y > h - 80 && x < w / 3) {
    if (basepath != "/") {
      const std::string oldPath = basepath;
      basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
      if (basepath.empty()) {
        basepath = "/";
      }
      loadFiles();

      const auto pos = oldPath.find_last_of('/');
      const std::string dirName = oldPath.substr(pos + 1) + "/";
      selectorIndex = findEntry(dirName);

      updateRequired = true;
    } else {
      onGoHome();
    }
    return true;
  }

  // Rows: start at START_Y, LINE_HEIGHT pixels each
  if (y >= START_Y && !files.empty()) {
    const int pageItems = getPageItems();
    const int row = (y - START_Y) / LINE_HEIGHT;
    if (row >= 0 && row < pageItems) {
      const size_t pageStartIndex = (selectorIndex / pageItems) * pageItems;
      const size_t tappedIndex = pageStartIndex + static_cast<size_t>(row);
      if (tappedIndex < files.size()) {
        selectorIndex = tappedIndex;
        pendingActivate = true;
        updateRequired = true;
        return true;
      }
    }
  }

  return false;
}
#endif

void FileSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FileSelectionActivity*>(param);
  self->displayTaskLoop();
}

int FileSelectionActivity::getPageItems() const {
  const int screenHeight = renderer.getScreenHeight();
  const int endY = screenHeight - 60;  // Leave space for button hints at bottom
  const int availableHeight = endY - START_Y;
  int items = availableHeight / LINE_HEIGHT;
  if (items < 1) {
    items = 1;
  }
  return items;
}

void FileSelectionActivity::loadFiles() {
  files.clear();

  Serial.printf("[%lu] [FSA] Loading files from: %s\n", millis(), basepath.c_str());

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    Serial.printf("[%lu] [FSA] Failed to open directory or not a directory\n", millis());
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      if (StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
          StringUtils::checkFileExtension(filename, ".xtc") || StringUtils::checkFileExtension(filename, ".txt")) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
  Serial.printf("[%lu] [FSA] Found %zu files\n", millis(), files.size());
}

void FileSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // basepath is set via constructor parameter (defaults to "/" if not specified)
  loadFiles();
  selectorIndex = 0;

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&FileSelectionActivity::taskTrampoline, "FileSelectionActivityTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void FileSelectionActivity::requestRedraw() {
  updateRequired = true;
}

void FileSelectionActivity::onExit() {
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
  files.clear();
}

void FileSelectionActivity::loop() {
#ifdef USE_M5UNIFIED
  if (pendingActivate) {
    pendingActivate = false;
    if (files.empty()) {
      return;
    }

    if (basepath.back() != '/') {
      basepath += "/";
    }

    if (files[selectorIndex].back() == '/') {
      basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
      loadFiles();
      selectorIndex = 0;
      updateRequired = true;
    } else {
      onSelect(basepath + files[selectorIndex]);
    }
    return;
  }
#endif

  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      loadFiles();
      updateRequired = true;
    }
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (files.empty()) {
      return;
    }

    if (basepath.back() != '/') basepath += "/";
    if (files[selectorIndex].back() == '/') {
      basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
      loadFiles();
      selectorIndex = 0;
      updateRequired = true;
    } else {
      onSelect(basepath + files[selectorIndex]);
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        updateRequired = true;
      } else {
        onGoHome();
      }
    }
  } else if (prevReleased) {
    const int pageItems = getPageItems();
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + files.size()) % files.size();
    } else {
      selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    }
    updateRequired = true;
  } else if (nextReleased) {
    const int pageItems = getPageItems();
    if (skipPage) {
      selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % files.size();
    } else {
      selectorIndex = (selectorIndex + 1) % files.size();
    }
    updateRequired = true;
  }
}

void FileSelectionActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void FileSelectionActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Books", true, EpdFontFamily::BOLD);

  // Help text
  const auto labels = mappedInput.mapLabels("", "", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, 20, START_Y, "No books found");
    renderer.displayBuffer();
    return;
  }

  const int pageItems = getPageItems();
  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  const int textYOffset = (LINE_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

  renderer.fillRect(0, START_Y + (selectorIndex % pageItems) * LINE_HEIGHT - 2, pageWidth - 1, LINE_HEIGHT);
  for (size_t i = pageStartIndex; i < files.size() && i < pageStartIndex + static_cast<size_t>(pageItems); i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(), renderer.getScreenWidth() - 40);
    renderer.drawText(UI_10_FONT_ID, 20, START_Y + (i % pageItems) * LINE_HEIGHT + textYOffset, item.c_str(), i != selectorIndex);
  }

  renderer.displayBuffer();
}

size_t FileSelectionActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
