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

class UiManager;

class WebManager {
public:
  bool begin(SettingsStore* store,
             SettingsV1* settings,
             NetManager* net,
             esp_panel::board::Board* board,
             SharedHistory* history,
             OtaManager* ota,
             MqttManager* mqtt,
             UiManager* ui);
  void loop();

  void updateMetrics(float db, float leq, float peak);

private:
  static constexpr uint32_t LIVE_PUSH_PERIOD_MS = 100;
  static constexpr uint8_t WEB_SESSION_MAX_COUNT = 6;
  static constexpr uint32_t WEB_SESSION_IDLE_TIMEOUT_MS = 12UL * 60UL * 60UL * 1000UL;
  static constexpr uint8_t WEB_LOGIN_MAX_FAILURES = 5;
  static constexpr uint32_t WEB_LOGIN_LOCK_MS = 15UL * 60UL * 1000UL;
  static constexpr uint16_t WEB_PASSWORD_HASH_ROUNDS = 12000;

  struct WebSession {
    bool active = false;
    char sessionToken[49] = "";
    char liveToken[49] = "";
    char username[WEB_USERNAME_MAX_LENGTH + 1] = "";
    uint32_t lastSeenMs = 0;
  };

  SettingsStore* _store = nullptr;
  SettingsV1* _s = nullptr;
  NetManager* _net = nullptr;
  esp_panel::board::Board* _board = nullptr;
  OtaManager* _ota = nullptr;
  MqttManager* _mqtt = nullptr;
  UiManager* _ui = nullptr;

  bool _started = false;
  WebServer _srv = WebServer(80);
  AsyncWebServer _liveSrv = AsyncWebServer(81);
  AsyncEventSource _liveEvents = AsyncEventSource("/api/events");

  uint32_t _lastLivePushMs = 0;
  SharedHistory* _history = nullptr;
  WebSession _sessions[WEB_SESSION_MAX_COUNT];
  uint8_t _loginFailureCount = 0;
  uint32_t _loginLockUntilMs = 0;

  void routes();
  void setupLiveStream();
  void pushLiveMetrics(bool force = false);
  void addCommonSecurityHeaders(bool noStore = true);

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
  bool requireWebAuth();
  String statusJson() const;
  String liveMetricsJson() const;
  bool pinConfigured() const;
  bool webUsersConfigured() const;
  void cleanupExpiredSessions();
  const WebSession* currentSession(bool touch = true);
  const WebSession* findSessionByToken(const char* token, bool touch = true);
  const WebSession* findSessionByLiveToken(const char* token, bool touch = true);
  WebSession* newSessionSlot();
  void invalidateSessionToken(const char* token);
  void invalidateSessionsForUser(const char* username);
  void clearAllSessions();
  bool issueSessionForUser(const char* username, String& liveTokenOut);
  String extractCookieValue(const char* cookieName) const;
  static bool secureEquals(const char* a, const char* b);
  static bool normalizeUsername(String& username);
  static bool passwordIsStrongEnough(const String& password, String* reason = nullptr);
  static String randomHex(size_t hexChars);
  static String hashPassword(const char* username, const char* password, const char* saltHex);

  void handleRoot();
  void handleStatus();
  void handleAuthStatus();
  void handleAuthLogin();
  void handleAuthLogout();
  void handleAuthBootstrap();
  void handleUsersGet();
  void handleUsersCreate();
  void handleUsersPassword();
  void handleUsersDelete();
  void handleUiSave();
  void handleLiveGet();
  void handleLiveSave();
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
  void applyTouchNow(bool enabled);
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
