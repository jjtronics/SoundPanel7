#pragma once

#include <Arduino.h>
#include "SettingsStore.h"

enum class AudioSource : uint8_t {
  Demo = 0,
  SensorAnalog = 1
};

struct AudioMetrics {
  float dbInstant = 0.0f;
  float leq = 0.0f;
  float peak = 0.0f;

  float rawRms = 0.0f;
  float rawPseudoDb = 0.0f;
  uint16_t rawAdcMean = 0;
  int rawAdcLast = 0;
  bool analogOk = false;
};

class AudioEngine {
public:
  void begin(SettingsV1* settings);
  void update(SettingsV1* settings);

  const AudioMetrics& metrics() const { return _m; }

  bool captureCalibrationPoint(SettingsV1& s, uint8_t index, float refDb);
  void clearCalibration(SettingsV1& s);

  static const char* sourceLabel(uint8_t src);
  static const char* responseModeLabel(uint8_t mode);

private:
  AudioMetrics _m;
  float _fast = 45.0f;
  float _slow = 45.0f;
  float _ema = 45.0f;
  float _peak = 45.0f;
  uint32_t _lastPeakReset = 0;
  float _demoT = 0.0f;

  float computeDemoDb();
  float computeAnalogRms(uint8_t pin, uint16_t sampleCount, uint16_t& meanOut, int& lastOut, bool& okOut);
  float computeCalibratedDb(float rms, const SettingsV1& s) const;
  float captureCalibrationLogRms(const SettingsV1& s, bool& okOut);
  static float clampf(float v, float lo, float hi);
};
