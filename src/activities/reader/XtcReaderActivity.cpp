/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>

#ifdef USE_M5UNIFIED
#include <M5Unified.h>

#include "touch/TouchEvent.h"
#endif

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "FileSelectionActivity.h"
#include "MappedInputManager.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "fontIds.h"

namespace {
constexpr unsigned long skipPageMs = 700;
constexpr unsigned long goHomeMs = 1000;
}  // namespace

void XtcReaderActivity::taskTrampoline(void* param) {
  auto* self = static_cast<XtcReaderActivity*>(param);
  self->displayTaskLoop();
}

#ifdef USE_M5UNIFIED
namespace {
void drawXtcReaderChrome(GfxRenderer& renderer, const bool hasChapters, const int selectionIndex) {
  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();

  constexpr int fontId = UI_12_FONT_ID;
  constexpr int rowH = 50;
  constexpr int padY = 20;

  const int itemCount = 2;
  const int boxW = w * 2 / 3;
  const int boxH = padY * 2 + rowH * itemCount;
  const int boxX = (w - boxW) / 2;
  const int boxY = (h - boxH) / 2;

  renderer.fillRect(boxX, boxY, boxW, boxH, false);
  renderer.drawRect(boxX, boxY, boxW, boxH);

  const char* items[2] = {"Files", "Home"};
  for (int i = 0; i < 2; i++) {
    const int rowTop = boxY + padY + i * rowH;
    const int textW = renderer.getTextWidth(fontId, items[i]);
    const int textH = renderer.getLineHeight(fontId);
    const int textX = boxX + (boxW - textW) / 2;
    const int textY = rowTop + (rowH - textH) / 2;
    if (i == selectionIndex) {
      renderer.fillRect(boxX + 10, rowTop, boxW - 20, rowH, true);
      renderer.drawText(fontId, textX, textY, items[i], false);
    } else {
      renderer.drawText(fontId, textX, textY, items[i], true);
    }
  }
}
}  // namespace
#endif

#ifdef USE_M5UNIFIED
bool XtcReaderActivity::onTouch(const TouchEvent& event) {
  if (subActivity) {
    return subActivity->onTouch(event);
  }

  const bool hasChapters = xtc && xtc->hasChapters() && !xtc->getChapters().empty();
  const int itemCount = 2;

  const int w = renderer.getScreenWidth();
  const int h = renderer.getScreenHeight();
  const int x = event.end.x;
  const int y = event.end.y;
  const bool inCenter = (x >= w / 3 && x <= (w * 2) / 3 && y >= h / 3 && y <= (h * 2) / 3);

  if (event.type == TouchEvent::Type::LongPress) {
    if (!inCenter) {
      return false;
    }
    chromeVisible = !chromeVisible;
    chromeSelectionIndex = 0;
    updateRequired = true;
    return true;
  }

  if (chromeVisible) {
    constexpr int rowH = 50;
    constexpr int padY = 20;
    const int boxW = w * 2 / 3;
    const int boxH = padY * 2 + rowH * itemCount;
    const int boxX = (w - boxW) / 2;
    const int boxY = (h - boxH) / 2;

    if (event.type == TouchEvent::Type::SwipeUp) {
      chromeSelectionIndex = (chromeSelectionIndex + itemCount - 1) % itemCount;
      updateRequired = true;
      return true;
    }

    if (event.type == TouchEvent::Type::SwipeDown) {
      chromeSelectionIndex = (chromeSelectionIndex + 1) % itemCount;
      updateRequired = true;
      return true;
    }

    if (event.type == TouchEvent::Type::Tap) {
      const int tx = event.end.x;
      const int ty = event.end.y;
      const bool inside = (tx >= boxX && tx < boxX + boxW && ty >= boxY && ty < boxY + boxH);
      if (!inside) {
        chromeVisible = false;
        updateRequired = true;
        return true;
      }

      const int relY = ty - (boxY + padY);
      if (relY >= 0) {
        const int rowIndex = relY / rowH;
        if (rowIndex >= 0 && rowIndex < itemCount) {
          chromeSelectionIndex = rowIndex;
        }
      }

      if (chromeSelectionIndex == 0) {
        pendingGoBackToFiles = true;
      } else {
        pendingGoHomeFromChrome = true;
      }
      chromeVisible = false;
      updateRequired = true;
      return true;
    }

    return true;
  }

  if (event.type == TouchEvent::Type::SwipeUp) {
    onGoHome();
    return true;
  }

  if (event.type == TouchEvent::Type::SwipeLeft) {
    currentPage++;
    if (xtc && currentPage > xtc->getPageCount()) {
      currentPage = xtc->getPageCount();
    }
    updateRequired = true;
    return true;
  }

  if (event.type == TouchEvent::Type::SwipeRight) {
    if (currentPage > 0) {
      currentPage--;
      updateRequired = true;
    }
    return true;
  }

  if (event.type != TouchEvent::Type::Tap) {
    return false;
  }

  if (inCenter) {
    if (hasChapters) {
      pendingOpenChapterSelection = true;
    } else {
      pendingGoBackToFiles = true;
    }
    return true;
  }

  if (x < w / 3) {
    if (currentPage > 0) {
      currentPage--;
      updateRequired = true;
    }
    return true;
  }

  if (x > (w * 2) / 3) {
    currentPage++;
    if (xtc && currentPage > xtc->getPageCount()) {
      currentPage = xtc->getPageCount();
    }
    updateRequired = true;
    return true;
  }

  return true;
}
#endif

void XtcReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (!xtc) {
    return;
  }

  renderingMutex = xSemaphoreCreateMutex();
  stopRequested = false;

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&XtcReaderActivity::taskTrampoline, "XtcReaderActivityTask",
              4096,               // Stack size (smaller than EPUB since no parsing needed)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void XtcReaderActivity::onExit() {
  ActivityWithSubactivity::onExit();

  stopRequested = true;

  const uint32_t start = millis();
  while (displayTaskHandle && millis() - start < 2500) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (renderingMutex) {
    const bool locked = xSemaphoreTake(renderingMutex, pdMS_TO_TICKS(2000)) == pdTRUE;
    if (locked) {
      xSemaphoreGive(renderingMutex);
      vSemaphoreDelete(renderingMutex);
      renderingMutex = nullptr;
    }
  }

  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  xtc.reset();
}

void XtcReaderActivity::requestRedraw() {
  if (subActivity) {
    subActivity->requestRedraw();
  }
  updateRequired = true;
}

void XtcReaderActivity::loop() {
  // Pass input responsibility to sub activity if exists
  if (subActivity) {
    subActivity->loop();
    return;
  }

#ifdef USE_M5UNIFIED
  if (pendingGoHomeFromChrome) {
    pendingGoHomeFromChrome = false;
    onGoHome();
    return;
  }

  if (pendingGoBackToFiles) {
    pendingGoBackToFiles = false;
    onGoBack();
    return;
  }
#endif

  // Enter chapter selection activity
  if (
#ifdef USE_M5UNIFIED
      pendingOpenChapterSelection ||
#endif
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
#ifdef USE_M5UNIFIED
    pendingOpenChapterSelection = false;
#endif
    if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
      updateRequired = false;
      exitActivity();
      enterNewActivity(new XtcReaderChapterSelectionActivity(
          this->renderer, this->mappedInput, xtc, currentPage,
          [this] {
            exitActivity();
            updateRequired = true;
          },
          [this](const uint32_t newPage) {
            currentPage = newPage;
            exitActivity();
            updateRequired = true;
          }));
      return;
    }
  }

  // Long press BACK (1s+) goes directly to home
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    onGoHome();
    return;
  }

  // Short press BACK goes to file selection
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoBack();
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::PageBack) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
                            (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::PAGE_TURN &&
                             mappedInput.wasReleased(MappedInputManager::Button::Power)) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  if (!prevReleased && !nextReleased) {
    return;
  }

  // Handle end of book
  if (currentPage >= xtc->getPageCount()) {
    currentPage = xtc->getPageCount() - 1;
    updateRequired = true;
    return;
  }

  const bool skipPages = SETTINGS.longPressChapterSkip && mappedInput.getHeldTime() > skipPageMs;
  const int skipAmount = skipPages ? 10 : 1;

  if (prevReleased) {
    if (currentPage >= static_cast<uint32_t>(skipAmount)) {
      currentPage -= skipAmount;
    } else {
      currentPage = 0;
    }
    updateRequired = true;
  } else if (nextReleased) {
    currentPage += skipAmount;
    if (currentPage >= xtc->getPageCount()) {
      currentPage = xtc->getPageCount();  // Allow showing "End of book"
    }
    updateRequired = true;
  }
}

void XtcReaderActivity::displayTaskLoop() {
  while (true) {
    if (stopRequested) {
      displayTaskHandle = nullptr;
      vTaskDelete(nullptr);
    }

    // If a subactivity is active (e.g. chapter selection), it owns the renderer.
    // Avoid concurrent rendering from the reader task.
    if (subActivity) {
      vTaskDelay(10 / portTICK_PERIOD_MS);
      continue;
    }

    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      renderScreen();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void XtcReaderActivity::renderScreen() {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "End of book", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  // Allocate page buffer
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    Serial.printf("[%lu] [XTR] Failed to allocate page buffer (%lu bytes)\n", millis(), pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Memory error", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    Serial.printf("[%lu] [XTR] Failed to load page %lu\n", millis(), currentPage);
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, "Page load error", true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2
    // - Grayscale: 0=White, 1=Dark Grey, 2=Light Grey, 3=Black

    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;              // Bit1 plane
    const uint8_t* plane2 = pageBuffer + planeSize;  // Bit2 plane
    const size_t colBytes = (pageHeight + 7) / 8;    // Bytes per column (100 for 800 height)

    // Lambda to get pixel value at (x, y)
    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return (bit1 << 1) | bit2;
    };

    // Optimized grayscale rendering without storeBwBuffer (saves 48KB peak memory)
    // Flow: BW display → LSB/MSB passes → grayscale display → re-render BW for next frame

    // Count pixel distribution for debugging
    uint32_t pixelCounts[4] = {0, 0, 0, 0};
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        pixelCounts[getPixelValue(x, y)]++;
      }
    }
    Serial.printf("[%lu] [XTR] Pixel distribution: White=%lu, DarkGrey=%lu, LightGrey=%lu, Black=%lu\n", millis(),
                  pixelCounts[0], pixelCounts[1], pixelCounts[2], pixelCounts[3]);

    // Pass 1: BW buffer - draw all non-white pixels as black
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Display BW with conditional refresh based on pagesUntilFullRefresh
    if (pagesUntilFullRefresh <= 1) {
      renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
      pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
    } else {
      renderer.displayBuffer();
      pagesUntilFullRefresh--;
    }

    // Pass 2: LSB buffer - mark DARK gray only (XTH value 1)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) == 1) {  // Dark grey only
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleLsbBuffers();

    // Pass 3: MSB buffer - mark LIGHT AND DARK gray (XTH value 1 or 2)
    // In LUT: 0 bit = apply gray effect, 1 bit = untouched
    renderer.clearScreen(0x00);
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        const uint8_t pv = getPixelValue(x, y);
        if (pv == 1 || pv == 2) {  // Dark grey or Light grey
          renderer.drawPixel(x, y, false);
        }
      }
    }
    renderer.copyGrayscaleMsbBuffers();

    // Display grayscale overlay
    renderer.displayGrayBuffer();

    // Pass 4: Re-render BW to framebuffer (restore for next frame, instead of restoreBwBuffer)
    renderer.clearScreen();
    for (uint16_t y = 0; y < pageHeight; y++) {
      for (uint16_t x = 0; x < pageWidth; x++) {
        if (getPixelValue(x, y) >= 1) {
          renderer.drawPixel(x, y, true);
        }
      }
    }

    // Cleanup grayscale buffers with current frame buffer
    renderer.cleanupGrayscaleWithFrameBuffer();

    free(pageBuffer);

    Serial.printf("[%lu] [XTR] Rendered page %lu/%lu (2-bit grayscale)\n", millis(), currentPage + 1,
                  xtc->getPageCount());
    return;
  } else {
    // 1-bit mode: 8 pixels per byte, MSB first
    const size_t srcRowBytes = (pageWidth + 7) / 8;  // 60 bytes for 480 width

    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;

      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        // Read source pixel (MSB first, bit 7 = leftmost pixel)
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);  // XTC: 0 = black, 1 = white

        if (isBlack) {
          renderer.drawPixel(srcX, srcY, true);
        }
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  free(pageBuffer);

  // XTC pages already have status bar pre-rendered, no need to add our own

  // Display with appropriate refresh
  if (pagesUntilFullRefresh <= 1) {
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
  } else {
    renderer.displayBuffer();
    pagesUntilFullRefresh--;
  }

#ifdef USE_M5UNIFIED
  if (chromeVisible) {
    const bool hasChapters = xtc && xtc->hasChapters() && !xtc->getChapters().empty();
    drawXtcReaderChrome(renderer, hasChapters, chromeSelectionIndex);
    renderer.displayBuffer(EInkDisplay::FAST_REFRESH);
  }
#endif

  Serial.printf("[%lu] [XTR] Rendered page %lu/%lu (%u-bit)\n", millis(), currentPage + 1, xtc->getPageCount(),
                bitDepth);
}

void XtcReaderActivity::saveProgress() const {
  FsFile f;
  if (SdMan.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    f.write(data, 4);
    f.close();
  }
}

void XtcReaderActivity::loadProgress() {
  FsFile f;
  if (SdMan.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      Serial.printf("[%lu] [XTR] Loaded progress: page %lu\n", millis(), currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}
