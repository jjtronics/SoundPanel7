#include "AudioEngine.h"
#include <math.h>

static constexpr float kRmsEpsilon = 0.0001f;
static constexpr float kFastAlpha = 0.45f;
static constexpr float kSlowAlpha = 0.10f;
static constexpr uint32_t kCalibrationCaptureMs = 3000;
static constexpr uint8_t kCalibrationSampleCount = 24;

static uint8_t countValidCalibrationPoints(const SettingsV1& s) {
  uint8_t count = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (s.calPointValid[i]) count++;
  }
  return count;
}

float AudioEngine::clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

const char* AudioEngine::sourceLabel(uint8_t src) {
  switch ((AudioSource)src) {
    case AudioSource::Demo:         return "Demo";
    case AudioSource::SensorAnalog: return "SensorAnalog";
    default:                        return "Unknown";
  }
}

const char* AudioEngine::responseModeLabel(uint8_t mode) {
  switch (mode) {
    case 0: return "Fast";
    case 1: return "Slow";
    default: return "Unknown";
  }
}

void AudioEngine::begin(SettingsV1* settings) {
  _fast = 45.0f;
  _slow = 45.0f;
  _ema = 45.0f;
  _peak = 45.0f;
  _lastPeakReset = millis();

  if (!settings) return;

#if defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(12);
  analogSetPinAttenuation(settings->analogPin, ADC_11db);
#endif
}

float AudioEngine::computeDemoDb() {
  _demoT += 0.04f;
  float raw = 55.0f + 18.0f * sinf(_demoT) + 6.0f * sinf(_demoT * 0.37f);
  return clampf(raw, 30.0f, 95.0f);
}

float AudioEngine::computeAnalogRms(uint8_t pin, uint16_t sampleCount, uint16_t& meanOut, int& lastOut, bool& okOut) {
  okOut = false;
  meanOut = 0;
  lastOut = 0;

  if (sampleCount < 32) sampleCount = 32;
  if (sampleCount > 1024) sampleCount = 1024;

  double sum = 0.0;
  double sum2 = 0.0;

  for (uint16_t i = 0; i < sampleCount; i++) {
    int v = analogRead(pin);
    sum += (double)v;
    sum2 += (double)v * (double)v;
    lastOut = v;
    delayMicroseconds(120);
  }

  double n = (double)sampleCount;
  double mean = sum / n;
  double var = (sum2 / n) - (mean * mean);
  if (var < 0.0) var = 0.0;

  meanOut = (uint16_t)(mean + 0.5);
  okOut = true;
  return (float)sqrt(var);
}

float AudioEngine::computeCalibratedDb(float rms, const SettingsV1& s) const {
  float x = log10f(rms + kRmsEpsilon);

  uint8_t idx[3];
  uint8_t n = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (s.calPointValid[i]) idx[n++] = i;
  }

  if (n == 0) {
    return 20.0f * x + s.analogBaseOffsetDb;
  }

  if (n == 1) {
    uint8_t i = idx[0];
    float x1 = s.calPointRawLogRms[i];
    float pseudoAtX1 = 20.0f * x1 + s.analogBaseOffsetDb;
    float delta = s.calPointRefDb[i] - pseudoAtX1;
    return 20.0f * x + s.analogBaseOffsetDb + delta;
  }

  for (uint8_t a = 0; a < n; a++) {
    for (uint8_t b = a + 1; b < n; b++) {
      if (s.calPointRawLogRms[idx[b]] < s.calPointRawLogRms[idx[a]]) {
        uint8_t t = idx[a];
        idx[a] = idx[b];
        idx[b] = t;
      }
    }
  }

  if (x <= s.calPointRawLogRms[idx[0]]) {
    float x1 = s.calPointRawLogRms[idx[0]];
    float y1 = s.calPointRefDb[idx[0]];
    float x2 = s.calPointRawLogRms[idx[1]];
    float y2 = s.calPointRefDb[idx[1]];
    if (fabsf(x2 - x1) < 0.00001f) return y1;
    float a = (y2 - y1) / (x2 - x1);
    return y1 + a * (x - x1);
  }

  for (uint8_t k = 0; k < n - 1; k++) {
    float x1 = s.calPointRawLogRms[idx[k]];
    float y1 = s.calPointRefDb[idx[k]];
    float x2 = s.calPointRawLogRms[idx[k + 1]];
    float y2 = s.calPointRefDb[idx[k + 1]];
    if (x <= x2 || k == n - 2) {
      if (fabsf(x2 - x1) < 0.00001f) return y1;
      float a = (y2 - y1) / (x2 - x1);
      return y1 + a * (x - x1);
    }
  }

  return 20.0f * x + s.analogBaseOffsetDb;
}

float AudioEngine::captureCalibrationLogRms(const SettingsV1& s, bool& okOut) {
  okOut = false;

  if ((AudioSource)s.audioSource != AudioSource::SensorAnalog) {
    if (_m.rawRms <= 0.0f) return 0.0f;
    okOut = true;
    return log10f(_m.rawRms + kRmsEpsilon);
  }

  float logSamples[kCalibrationSampleCount];
  uint8_t sampleUsed = 0;
  uint32_t startMs = millis();

  while ((millis() - startMs) < kCalibrationCaptureMs && sampleUsed < kCalibrationSampleCount) {
    uint16_t meanAdc = 0;
    int lastAdc = 0;
    bool analogOk = false;
    float rms = computeAnalogRms(s.analogPin, s.analogRmsSamples, meanAdc, lastAdc, analogOk);
    if (analogOk && rms > 0.0f) {
      logSamples[sampleUsed++] = log10f(rms + kRmsEpsilon);
    }
  }

  if (sampleUsed == 0) return 0.0f;

  for (uint8_t i = 0; i < sampleUsed; i++) {
    for (uint8_t j = i + 1; j < sampleUsed; j++) {
      if (logSamples[j] < logSamples[i]) {
        float tmp = logSamples[i];
        logSamples[i] = logSamples[j];
        logSamples[j] = tmp;
      }
    }
  }

  uint8_t trim = sampleUsed / 6;
  if ((trim * 2) >= sampleUsed) trim = 0;

  float sum = 0.0f;
  uint8_t kept = 0;
  for (uint8_t i = trim; i < (sampleUsed - trim); i++) {
    sum += logSamples[i];
    kept++;
  }

  if (kept == 0) return 0.0f;

  okOut = true;
  return sum / (float)kept;
}

bool AudioEngine::captureCalibrationPoint(SettingsV1& s, uint8_t index, float refDb) {
  if (index >= 3) return false;

  bool ok = false;
  float capturedLogRms = captureCalibrationLogRms(s, ok);
  if (!ok) return false;

  s.calPointRefDb[index] = refDb;
  s.calPointRawLogRms[index] = capturedLogRms;
  s.calPointValid[index] = 1;
  return true;
}

void AudioEngine::clearCalibration(SettingsV1& s) {
  static const float kRecommendedRefDb[3] = {45.0f, 65.0f, 85.0f};

  for (uint8_t i = 0; i < 3; i++) {
    s.calPointRefDb[i] = kRecommendedRefDb[i];
    s.calPointRawLogRms[i] = 0.0f;
    s.calPointValid[i] = 0;
  }
}

void AudioEngine::update(SettingsV1* settings) {
  if (!settings) return;

  float dbRaw = 0.0f;
  float dbInst = 0.0f;
  float rawRms = 0.0f;
  uint16_t meanAdc = 0;
  int lastAdc = 0;
  bool analogOk = false;

  switch ((AudioSource)settings->audioSource) {
    case AudioSource::Demo:
      dbRaw = computeDemoDb();
      rawRms = powf(10.0f, dbRaw / 20.0f);
      meanAdc = 2048;
      lastAdc = 2048;
      analogOk = true;
      break;

    case AudioSource::SensorAnalog:
      rawRms = computeAnalogRms(settings->analogPin, settings->analogRmsSamples, meanAdc, lastAdc, analogOk);
      dbRaw = computeCalibratedDb(rawRms, *settings);
      // Keep the legacy global offset only for the uncalibrated fallback path.
      // Once at least one calibration point exists, the captured references define
      // the absolute dB mapping and must not be shifted again.
      if (countValidCalibrationPoints(*settings) == 0) {
        dbRaw += settings->analogExtraOffsetDb;
      }
      dbRaw = clampf(dbRaw, 0.0f, 120.0f);
      break;

    default:
      dbRaw = computeDemoDb();
      rawRms = powf(10.0f, dbRaw / 20.0f);
      meanAdc = 2048;
      lastAdc = 2048;
      analogOk = true;
      break;
  }

  _fast = kFastAlpha * dbRaw + (1.0f - kFastAlpha) * _fast;
  _slow = kSlowAlpha * dbRaw + (1.0f - kSlowAlpha) * _slow;

  float a = clampf(settings->emaAlpha, 0.01f, 0.95f);
  _ema = a * dbRaw + (1.0f - a) * _ema;

  dbInst = (settings->audioResponseMode == 1) ? _slow : _fast;

  if (dbRaw > _peak) _peak = dbRaw;
  uint32_t now = millis();
  if (now - _lastPeakReset > settings->peakHoldMs) {
    _peak = dbRaw;
    _lastPeakReset = now;
  }

  _m.dbInstant = dbInst;
  _m.leq = _ema;
  _m.peak = _peak;
  _m.rawRms = rawRms;
  _m.rawPseudoDb = 20.0f * log10f(rawRms + kRmsEpsilon) + settings->analogBaseOffsetDb;
  _m.rawAdcMean = meanAdc;
  _m.rawAdcLast = lastAdc;
  _m.analogOk = analogOk;
}
