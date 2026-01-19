#include "OpdsBookBrowserActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "activities/network/WifiSelectionActivity.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"
#include "util/UrlUtils.h"

#ifdef USE_M5UNIFIED
#include "touch/TouchEvent.h"
#endif

namespace {
constexpr int START_Y = 60;
constexpr int LINE_HEIGHT = 60;
constexpr int SKIP_PAGE_MS = 700;
constexpr char OPDS_ROOT_PATH[] = "opds";  // No leading slash - relative to server URL

int getPageItems(const GfxRenderer& renderer) {
  const int screenHeight = renderer.getScreenHeight();
  const int endY = screenHeight - 60;  // Leave space for button hints
  const int availableHeight = endY - START_Y;
  int items = availableHeight / LINE_HEIGHT;
  if (items < 1) items = 1;
  return items;
}
}  // namespace

void OpdsBookBrowserActivity::taskTrampoline(void* param) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(param);
  self->displayTaskLoop();
}

void OpdsBookBrowserActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = BrowserState::CHECK_WIFI;
  entries.clear();
  navigationHistory.clear();
  currentPath = OPDS_ROOT_PATH;
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = "Checking WiFi...";
  updateRequired = true;

  xTaskCreate(&OpdsBookBrowserActivity::taskTrampoline, "OpdsBookBrowserTask",
              4096,               // Stack size (larger for HTTP operations)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Check WiFi and connect if needed, then fetch feed
  checkAndConnectWifi();
}

void OpdsBookBrowserActivity::onExit() {
  ActivityWithSubactivity::onExit();

  // Turn off WiFi when exiting
  WiFi.mode(WIFI_OFF);

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
  entries.clear();
  navigationHistory.clear();
}

void OpdsBookBrowserActivity::requestRedraw() {
  if (subActivity) {
    subActivity->requestRedraw();
  }
  updateRequired = true;
}

void OpdsBookBrowserActivity::loop() {
  // Handle WiFi selection subactivity
  if (state == BrowserState::WIFI_SELECTION) {
    ActivityWithSubactivity::loop();
    return;
  }

  // Handle error state - Confirm retries, Back goes back or home
  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Check if WiFi is still connected
      if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
        // WiFi connected - just retry fetching the feed
        Serial.printf("[%lu] [OPDS] Retry: WiFi connected, retrying fetch\n", millis());
        state = BrowserState::LOADING;
        statusMessage = "Loading...";
        updateRequired = true;
        fetchFeed(currentPath);
      } else {
        // WiFi not connected - launch WiFi selection
        Serial.printf("[%lu] [OPDS] Retry: WiFi not connected, launching selection\n", millis());
        launchWifiSelection();
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  // Handle WiFi check state - only Back works
  if (state == BrowserState::CHECK_WIFI) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  // Handle loading state - only Back works
  if (state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    }
    return;
  }

  // Handle downloading state - no input allowed
  if (state == BrowserState::DOWNLOADING) {
    return;
  }

  // Handle browsing state
  if (state == BrowserState::BROWSING) {
#ifdef USE_M5UNIFIED
    if (pendingActivate) {
      pendingActivate = false;
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        if (entry.type == OpdsEntryType::BOOK) {
          downloadBook(entry);
        } else {
          navigateToEntry(entry);
        }
      }
      return;
    }
#endif

    const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!entries.empty()) {
        const auto& entry = entries[selectorIndex];
        if (entry.type == OpdsEntryType::BOOK) {
          downloadBook(entry);
        } else {
          navigateToEntry(entry);
        }
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      navigateBack();
    } else if (prevReleased && !entries.empty()) {
      const int pageItems = getPageItems(renderer);
      if (skipPage) {
        selectorIndex = ((selectorIndex / pageItems - 1) * pageItems + entries.size()) % entries.size();
      } else {
        selectorIndex = (selectorIndex + entries.size() - 1) % entries.size();
      }
      updateRequired = true;
    } else if (nextReleased && !entries.empty()) {
      const int pageItems = getPageItems(renderer);
      if (skipPage) {
        selectorIndex = ((selectorIndex / pageItems + 1) * pageItems) % entries.size();
      } else {
        selectorIndex = (selectorIndex + 1) % entries.size();
      }
      updateRequired = true;
    }
  }
}

#ifdef USE_M5UNIFIED
bool OpdsBookBrowserActivity::onTouch(const TouchEvent& event) {
  if (state == BrowserState::WIFI_SELECTION) {
    return ActivityWithSubactivity::onTouch(event);
  }

  if (event.type == TouchEvent::Type::Tap) {
    const int w = renderer.getScreenWidth();
    const int h = renderer.getScreenHeight();
    const int x = event.end.x;
    const int y = event.end.y;

    // Bottom-left: Back
    if (y > h - 80 && x < w / 3) {
      if (state == BrowserState::BROWSING || state == BrowserState::ERROR || state == BrowserState::LOADING) {
        navigateBack();
      } else if (state == BrowserState::CHECK_WIFI) {
        onGoHome();
      }
      return true;
    }
  }

  if (state != BrowserState::BROWSING) {
    return false;
  }

  if (event.type == TouchEvent::Type::SwipeUp) {
    if (entries.empty()) {
      return true;
    }
    const int pageItems = getPageItems(renderer);
    const int total = static_cast<int>(entries.size());
    const int step = std::min(pageItems, total);
    selectorIndex = (selectorIndex + total - step) % total;
    updateRequired = true;
    return true;
  }

  if (event.type == TouchEvent::Type::SwipeDown) {
    if (entries.empty()) {
      return true;
    }
    const int pageItems = getPageItems(renderer);
    const int total = static_cast<int>(entries.size());
    const int step = std::min(pageItems, total);
    selectorIndex = (selectorIndex + step) % total;
    updateRequired = true;
    return true;
  }

  if (event.type != TouchEvent::Type::Tap) {
    return false;
  }

  const int pageItems = getPageItems(renderer);
  const int y = event.end.y;
  if (y < START_Y || entries.empty()) {
    return false;
  }

  const int row = (y - START_Y) / LINE_HEIGHT;
  if (row < 0 || row >= pageItems) {
    return false;
  }

  const int pageStartIndex = selectorIndex / pageItems * pageItems;
  const int tappedIndex = pageStartIndex + row;
  if (tappedIndex >= 0 && tappedIndex < static_cast<int>(entries.size())) {
    selectorIndex = tappedIndex;
    pendingActivate = true;
    updateRequired = true;
    return true;
  }

  return false;
}
#endif

void OpdsBookBrowserActivity::displayTaskLoop() {
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

void OpdsBookBrowserActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Calibre Library", true, EpdFontFamily::BOLD);

  if (state == BrowserState::CHECK_WIFI) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "Error:");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels("« Back", "Retry", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 40, "Downloading...");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 10, statusMessage.c_str());
    if (downloadTotal > 0) {
      const int barWidth = pageWidth - 100;
      constexpr int barHeight = 20;
      constexpr int barX = 50;
      const int barY = pageHeight / 2 + 20;
      ScreenComponents::drawProgressBar(renderer, barX, barY, barWidth, barHeight, downloadProgress, downloadTotal);
    }
    renderer.displayBuffer();
    return;
  }

  // Browsing state
  // Show appropriate button hint based on selected entry type
  const char* confirmLabel = "Open";
  if (!entries.empty() && entries[selectorIndex].type == OpdsEntryType::BOOK) {
    confirmLabel = "Download";
  }
  const auto labels = mappedInput.mapLabels("« Back", confirmLabel, "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (entries.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "No entries found");
    renderer.displayBuffer();
    return;
  }

  const int pageItems = getPageItems(renderer);
  const auto pageStartIndex = selectorIndex / pageItems * pageItems;
  const int textYOffset = (LINE_HEIGHT - renderer.getLineHeight(UI_10_FONT_ID)) / 2;

  renderer.fillRect(0, START_Y + (selectorIndex % pageItems) * LINE_HEIGHT - 2, pageWidth - 1, LINE_HEIGHT);

  for (size_t i = pageStartIndex; i < entries.size() && i < static_cast<size_t>(pageStartIndex + pageItems); i++) {
    const auto& entry = entries[i];

    // Format display text with type indicator
    std::string displayText;
    if (entry.type == OpdsEntryType::NAVIGATION) {
      displayText = "> " + entry.title;  // Folder/navigation indicator
    } else {
      // Book: "Title - Author" or just "Title"
      displayText = entry.title;
      if (!entry.author.empty()) {
        displayText += " - " + entry.author;
      }
    }

    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), renderer.getScreenWidth() - 40);
    renderer.drawText(UI_10_FONT_ID, 20, START_Y + (i % pageItems) * LINE_HEIGHT + textYOffset, item.c_str(),
                      i != static_cast<size_t>(selectorIndex));
  }

  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchFeed(const std::string& path) {
  const char* serverUrl = SETTINGS.opdsServerUrl;
  if (strlen(serverUrl) == 0) {
    state = BrowserState::ERROR;
    errorMessage = "No server URL configured";
    updateRequired = true;
    return;
  }

  std::string url = UrlUtils::buildUrl(serverUrl, path);
  Serial.printf("[%lu] [OPDS] Fetching: %s\n", millis(), url.c_str());

  std::string content;
  if (!HttpDownloader::fetchUrl(url, content)) {
    state = BrowserState::ERROR;
    errorMessage = "Failed to fetch feed";
    updateRequired = true;
    return;
  }

  OpdsParser parser;
  if (!parser.parse(content.c_str(), content.size())) {
    state = BrowserState::ERROR;
    errorMessage = "Failed to parse feed";
    updateRequired = true;
    return;
  }

  entries = parser.getEntries();
  selectorIndex = 0;

  if (entries.empty()) {
    state = BrowserState::ERROR;
    errorMessage = "No entries found";
    updateRequired = true;
    return;
  }

  state = BrowserState::BROWSING;
  updateRequired = true;
}

void OpdsBookBrowserActivity::navigateToEntry(const OpdsEntry& entry) {
  // Push current path to history before navigating
  navigationHistory.push_back(currentPath);
  currentPath = entry.href;

  state = BrowserState::LOADING;
  statusMessage = "Loading...";
  entries.clear();
  selectorIndex = 0;
  updateRequired = true;

  fetchFeed(currentPath);
}

void OpdsBookBrowserActivity::navigateBack() {
  if (navigationHistory.empty()) {
    // At root, go home
    onGoHome();
  } else {
    // Go back to previous catalog
    currentPath = navigationHistory.back();
    navigationHistory.pop_back();

    state = BrowserState::LOADING;
    statusMessage = "Loading...";
    entries.clear();
    selectorIndex = 0;
    updateRequired = true;

    fetchFeed(currentPath);
  }
}

void OpdsBookBrowserActivity::downloadBook(const OpdsEntry& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = 0;
  downloadTotal = 0;
  updateRequired = true;

  // Build full download URL
  std::string downloadUrl = UrlUtils::buildUrl(SETTINGS.opdsServerUrl, book.href);

  // Create sanitized filename: "Title - Author.epub" or just "Title.epub" if no author
  std::string baseName = book.title;
  if (!book.author.empty()) {
    baseName += " - " + book.author;
  }
  std::string filename = "/" + StringUtils::sanitizeFilename(baseName) + ".epub";

  Serial.printf("[%lu] [OPDS] Downloading: %s -> %s\n", millis(), downloadUrl.c_str(), filename.c_str());

  const auto result =
      HttpDownloader::downloadToFile(downloadUrl, filename, [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        updateRequired = true;
      });

  if (result == HttpDownloader::OK) {
    Serial.printf("[%lu] [OPDS] Download complete: %s\n", millis(), filename.c_str());
    state = BrowserState::BROWSING;
    updateRequired = true;
  } else {
    state = BrowserState::ERROR;
    errorMessage = "Download failed";
    updateRequired = true;
  }
}

void OpdsBookBrowserActivity::checkAndConnectWifi() {
  // Already connected? Verify connection is valid by checking IP
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = BrowserState::LOADING;
    statusMessage = "Loading...";
    updateRequired = true;
    fetchFeed(currentPath);
    return;
  }

  // Not connected - launch WiFi selection screen directly
  launchWifiSelection();
}

void OpdsBookBrowserActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  updateRequired = true;

  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](const bool connected) { onWifiSelectionComplete(connected); }));
}

void OpdsBookBrowserActivity::onWifiSelectionComplete(const bool connected) {
  exitActivity();

  if (connected) {
    Serial.printf("[%lu] [OPDS] WiFi connected via selection, fetching feed\n", millis());
    state = BrowserState::LOADING;
    statusMessage = "Loading...";
    updateRequired = true;
    fetchFeed(currentPath);
  } else {
    Serial.printf("[%lu] [OPDS] WiFi selection cancelled/failed\n", millis());
    // Force disconnect to ensure clean state for next retry
    // This prevents stale connection status from interfering
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = BrowserState::ERROR;
    errorMessage = "WiFi connection failed";
    updateRequired = true;
  }
}
