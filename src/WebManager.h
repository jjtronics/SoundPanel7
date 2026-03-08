#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <ESPAsyncWebServer.h>
#include <esp_display_panel.hpp>

#include "SettingsStore.h"
#include "NetManager.h"
#include "SharedHistory.h"
#include "OtaManager.h"
#include "MqttManager.h"

class WebManager {
public:
  bool begin(SettingsStore* store,
             SettingsV1* settings,
             NetManager* net,
             esp_panel::board::Board* board,
             SharedHistory* history,
             OtaManager* ota,
             MqttManager* mqtt);
  void loop();

  void updateMetrics(float db, float leq, float peak);

private:
  static constexpr uint32_t LIVE_PUSH_PERIOD_MS = 100;

  SettingsStore* _store = nullptr;
  SettingsV1* _s = nullptr;
  NetManager* _net = nullptr;
  esp_panel::board::Board* _board = nullptr;
  OtaManager* _ota = nullptr;
  MqttManager* _mqtt = nullptr;

  bool _started = false;
  WebServer _srv = WebServer(80);
  AsyncWebServer _liveSrv = AsyncWebServer(81);
  AsyncEventSource _liveEvents = AsyncEventSource("/api/events");

  uint32_t _lastLivePushMs = 0;
  SharedHistory* _history = nullptr;

  void routes();
  void setupLiveStream();
  void pushLiveMetrics(bool force = false);

  void replyText(int code, const String& txt, const char* contentType = "text/plain");
  void replyJson(int code, const String& json);

  static int jsonInt(const String& body, const char* key, int def);
  static String jsonStr(const String& body, const char* key, const String& def);
  static bool safeCopy(char* dst, size_t dstSize, const String& src);
  String statusJson() const;
  String liveMetricsJson() const;

  void handleRoot();
  void handleStatus();
  void handleUiSave();

  void handleTimeGet();
  void handleTimeSave();
  void handleConfigExport();
  void handleConfigImport();
  void handleConfigBackup();
  void handleConfigRestore();
  void handleConfigResetPartial();

  void handleReboot();
  void handleFactoryReset();

  void applyBacklightNow(uint8_t percent);
  void applySettingsRuntimeState();
  String historyJson() const;

  void handleCalPoint();
  void handleCalClear();
  void handleOtaGet();
  void handleOtaSave();
  void handleMqttGet();
  void handleMqttSave();
};
