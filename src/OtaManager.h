#pragma once

#include <Arduino.h>
#include "SettingsStore.h"

class OtaManager {
public:
  bool begin(SettingsV1* settings);
  void loop();
  bool started() const { return _started; }

private:
  SettingsV1* _s = nullptr;
  bool _started = false;
};