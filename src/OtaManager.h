#pragma once

#include <Arduino.h>
#include "SettingsStore.h"

class UiManager;

class OtaManager {
public:
  bool begin(SettingsV1* settings, UiManager* ui = nullptr);
  void loop();
  bool started() const { return _started; }
  bool enabled() const { return _s && _s->otaEnabled; }
  bool inProgress() const { return _inProgress; }

private:
  static constexpr uint32_t kRebootDelayAfterSuccessMs = 1500;

  SettingsV1* _s = nullptr;
  UiManager* _ui = nullptr;
  bool _started = false;
  bool _inProgress = false;
  bool _rebootPending = false;
  uint32_t _lastStartAttemptMs = 0;
  uint32_t _rebootAtMs = 0;
  uint8_t _lastLoggedProgressPct = 255;
  uint8_t _lastUiProgressPct = 255;

  bool tryStart();
};
