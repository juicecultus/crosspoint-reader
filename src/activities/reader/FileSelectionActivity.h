#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

#ifdef USE_M5UNIFIED
struct TouchEvent;
#endif

class FileSelectionActivity final : public Activity {
  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::string basepath = "/";
  std::vector<std::string> files;
  size_t selectorIndex = 0;
  bool updateRequired = false;
  bool pendingActivate = false;
  const std::function<void(const std::string&)> onSelect;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void loadFiles();
  int getPageItems() const;

  size_t findEntry(const std::string& name) const;

 public:
  explicit FileSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                const std::function<void(const std::string&)>& onSelect,
                                const std::function<void()>& onGoHome, std::string basepath = "/")
      : Activity("FileSelection", renderer, mappedInput),
        onSelect(onSelect),
        onGoHome(onGoHome),
        basepath(std::move(basepath)) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void requestRedraw() override;

#ifdef USE_M5UNIFIED
  bool onTouch(const TouchEvent& event) override;
#endif
};
