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
  void replyOkJson(bool trailingNewline = false);
  void replyOkJsonRebootRecommended();
  void replyOkJsonRebootRequired();
  void replyErrorJson(int code, const String& error, bool trailingNewline = false);
  bool requireSettingsText();
  bool requireSettingsJson();
  bool requireStoreAndSettingsText();
  bool requireStoreAndSettingsJson();
  String statusJson() const;
  String liveMetricsJson() const;
  bool pinConfigured() const;

  void handleRoot();
  void handleStatus();
  void handleUiSave();
  void handlePinSave();

  void handleTimeGet();
  void handleTimeSave();
  void handleConfigExport();
  void handleConfigImport();
  void handleConfigBackup();
  void handleConfigRestore();
  void handleConfigResetPartial();

  void handleReboot();
  void handleShutdown();
  void handleFactoryReset();

  void applyBacklightNow(uint8_t percent);
  void applySettingsRuntimeState();
  String historyJson() const;

  void handleCalPoint();
  void handleCalClear();
  void handleCalMode();
  void handleOtaGet();
  void handleOtaSave();
  void handleMqttGet();
  void handleMqttSave();
};
