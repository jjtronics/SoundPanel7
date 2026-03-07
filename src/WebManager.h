#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <esp_display_panel.hpp>

#include "SettingsStore.h"
#include "NetManager.h"

class WebManager {
public:
  bool begin(SettingsStore* store,
             SettingsV1* settings,
             NetManager* net,
             esp_panel::board::Board* board);
  void loop();

  void updateMetrics(float db, float leq, float peak);

private:
  static constexpr uint16_t HISTORY_POINTS = 180;

  SettingsStore* _store = nullptr;
  SettingsV1* _s = nullptr;
  NetManager* _net = nullptr;
  esp_panel::board::Board* _board = nullptr;

  bool _started = false;
  WebServer _srv = WebServer(80);

  float _lastDb = 42.0f;
  float _lastLeq = 42.0f;
  float _lastPeak = 42.0f;

  float _hist[HISTORY_POINTS] = {0};
  uint16_t _histCount = 0;
  uint16_t _histHead = 0;
  uint32_t _lastHistPushMs = 0;

  void routes();

  void replyText(int code, const String& txt, const char* contentType = "text/plain");
  void replyJson(int code, const String& json);

  static int jsonInt(const String& body, const char* key, int def);
  static String jsonStr(const String& body, const char* key, const String& def);
  static bool safeCopy(char* dst, size_t dstSize, const String& src);

  void handleRoot();
  void handleAdmin();
  void handleStatus();
  void handleUiSave();

  void handleTimeGet();
  void handleTimeSave();

  void handleReboot();
  void handleFactoryReset();

  void applyBacklightNow(uint8_t percent);
  uint32_t historySamplePeriodMs() const;
  String historyJson() const;

  void handleCalPoint();
  void handleCalClear();
  void handleOtaGet();
  void handleOtaSave();
  void handleMqttGet();
  void handleMqttSave();
};