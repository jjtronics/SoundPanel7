#pragma once

#include <Arduino.h>

#include "SettingsStore.h"

class SharedHistory {
public:
  static constexpr uint16_t POINT_COUNT = 96;

  void begin(SettingsV1* settings) {
    _settings = settings;
  }

  void settingsChanged() {
    _lastSampleMs = 0;
    _revision++;
  }

  void update(float db, uint32_t now) {
    if (_lastSampleMs == 0 || (now - _lastSampleMs) >= samplePeriodMs()) {
      _lastSampleMs = now;
      _values[_head] = db;
      _head = (_head + 1) % POINT_COUNT;
      if (_count < POINT_COUNT) _count++;
      _revision++;
    }
  }

  uint32_t samplePeriodMs() const {
    uint8_t minutes = (_settings ? _settings->historyMinutes : 5);
    if (minutes < 1) minutes = 1;
    if (minutes > 60) minutes = 60;

    uint32_t totalMs = (uint32_t)minutes * 60UL * 1000UL;
    uint32_t period = totalMs / POINT_COUNT;
    if (period < 250) period = 250;
    return period;
  }

  uint16_t count() const {
    return _count;
  }

  uint32_t revision() const {
    return _revision;
  }

  float valueAt(uint16_t index) const {
    if (index >= _count) return 0.0f;
    uint16_t start = (_count < POINT_COUNT) ? 0 : _head;
    uint16_t idx = (start + index) % POINT_COUNT;
    return _values[idx];
  }

  String toJson() const {
    String json;
    json.reserve((size_t)_count * 8U + 4U);
    json += "[";

    for (uint16_t i = 0; i < _count; i++) {
      if (i) json += ",";
      json += String(valueAt(i), 1);
    }

    json += "]";
    return json;
  }

private:
  SettingsV1* _settings = nullptr;
  float _values[POINT_COUNT] = {0.0f};
  uint16_t _count = 0;
  uint16_t _head = 0;
  uint32_t _lastSampleMs = 0;
  uint32_t _revision = 0;
};
