#pragma once
#include <Epub.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <memory>

#include "../Activity.h"

#ifdef USE_M5UNIFIED
struct TouchEvent;
#endif

class EpubReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<Epub> epub;
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  int currentSpineIndex = 0;
  int selectorIndex = 0;
  bool updateRequired = false;
  bool pendingActivate = false;
  const std::function<void()> onGoBack;
  const std::function<void(int newSpineIndex)> onSelectSpineIndex;

  // Number of items that fit on a page, derived from logical screen height.
  // This adapts automatically when switching between portrait and landscape.
  int getPageItems() const;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void renderScreen();

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const int currentSpineIndex,
                                              const std::function<void()>& onGoBack,
                                              const std::function<void(int newSpineIndex)>& onSelectSpineIndex)
      : Activity("EpubReaderChapterSelection", renderer, mappedInput),
        epub(epub),
        currentSpineIndex(currentSpineIndex),
        onGoBack(onGoBack),
        onSelectSpineIndex(onSelectSpineIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void requestRedraw() override;

#ifdef USE_M5UNIFIED
  bool onTouch(const TouchEvent& event) override;
#endif
};
