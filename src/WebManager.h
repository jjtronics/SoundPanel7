#pragma once

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <WebServer.h>
#include <esp_display_panel.hpp>

#include "SettingsStore.h"
#include "NetManager.h"
#include "SharedHistory.h"
#include "OtaManager.h"
#include "MqttManager.h"

class UiManager;
class LiveEventServer;
class ReleaseUpdateManager;

class WebManager {
public:
  bool begin(SettingsStore* store,
             SettingsV1* settings,
             NetManager* net,
             esp_panel::board::Board* board,
             SharedHistory* history,
             OtaManager* ota,
             ReleaseUpdateManager* releaseUpdate,
             MqttManager* mqtt,
             UiManager* ui);
  void loop();

  void updateMetrics(float db, float leq, float peak);

private:
  static constexpr uint32_t LIVE_PUSH_PERIOD_MS = 240;
  static constexpr uint32_t LIVE_SYSTEM_PUSH_PERIOD_MS = 1000;
  static constexpr uint8_t WEB_SESSION_MAX_COUNT = 6;
  static constexpr uint32_t WEB_SESSION_IDLE_TIMEOUT_MS = 12UL * 60UL * 60UL * 1000UL;
  static constexpr uint8_t WEB_LOGIN_MAX_FAILURES = 3;  // SECURITY: Reduced from 5 to 3 (HIGH-01)
  static constexpr uint32_t WEB_LOGIN_LOCK_MS = 30UL * 60UL * 1000UL;  // SECURITY: Increased from 15min to 30min (HIGH-01)
  static constexpr uint8_t WEB_LOGIN_MAX_LOCKOUT_LEVEL = 5;  // SECURITY: Exponential backoff up to 5 levels (HIGH-01)
  static constexpr uint16_t WEB_PASSWORD_HASH_ROUNDS = 12000;

  // SECURITY: Session token security properties (HIGH-03 documentation)
  // - Token length: 48 hex characters = 192 bits of entropy
  // - Entropy sources: ESP32 hardware RNG (RF noise + boot ROM seed) + timestamp + counter
  // - Token storage: HttpOnly cookie (prevents XSS) + SameSite=Strict (prevents CSRF)
  // - Session timeout: 12 hours idle timeout with automatic cleanup
  // - Threat model: Assumes HTTP-only deployment on trusted local network (no TLS)
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
  ReleaseUpdateManager* _releaseUpdate = nullptr;
  MqttManager* _mqtt = nullptr;
  UiManager* _ui = nullptr;
  LiveEventServer* _live = nullptr;

  bool _started = false;
  bool _httpListening = false;
  WebServer _srv = WebServer(80);

  uint32_t _lastLivePushMs = 0;
  uint32_t _lastLiveSystemPushMs = 0;
  Adafruit_NeoPixel _tardisInteriorRgb = Adafruit_NeoPixel(
    SOUNDPANEL7_TARDIS_INTERIOR_RGB_PIXEL_COUNT,
    SOUNDPANEL7_TARDIS_INTERIOR_RGB_PIN,
    NEO_GRB + NEO_KHZ800
  );
  Adafruit_NeoPixel _tardisExteriorRgbw = Adafruit_NeoPixel(
    SOUNDPANEL7_TARDIS_EXTERIOR_RGBW_PIXEL_COUNT,
    SOUNDPANEL7_TARDIS_EXTERIOR_RGBW_PIN,
    NEO_GRBW + NEO_KHZ800
  );
  bool _tardisInteriorRgbReady = false;
  bool _tardisExteriorRgbwReady = false;
  uint32_t _tardisInteriorRgbAppliedColor = 0xFFFFFFFFUL;
  uint32_t _tardisExteriorRgbwAppliedColor = 0xFFFFFFFFUL;
  uint32_t _tardisAnimationCycleStartMs = 0;
  uint32_t _tardisAnimationLastFrameMs = 0;
  uint8_t _tardisAnimationMode = 0xFF;
  SharedHistory* _history = nullptr;
  WebSession _sessions[WEB_SESSION_MAX_COUNT];
  uint8_t _loginFailureCount = 0;
  uint32_t _loginLockUntilMs = 0;
  uint8_t _loginLockoutLevel = 0;  // SECURITY: Track consecutive lockouts for exponential backoff (HIGH-01)
  SemaphoreHandle_t _bootstrapMutex = nullptr;
  SemaphoreHandle_t _settingsMutex = nullptr;  // SECURITY: Protect settings modifications (MED-02)

  void routes();
  void startHttpServer();
  void stopHttpServer();
  void syncHttpAvailability();
  void setupLiveStream();
  void pushLiveMetrics(bool force = false);
  void pushLiveSystem(bool force = false);
  bool liveTrafficPaused() const;
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
  bool requireHomeAssistantToken();
  String statusJson() const;
  String systemSummaryJson() const;
  String homeAssistantStatusJson() const;
  String liveMetricsJson() const;
  const char* secretValueForReveal(const String& name) const;
  bool pinConfigured() const;
  bool webUsersConfigured() const;
  bool homeAssistantTokenConfigured() const;
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
  String extractAuthorizationBearer() const;
  static bool secureEquals(const char* a, const char* b);
  static bool normalizeUsername(String& username);
  static bool homeAssistantTokenIsValid(const String& token);
  static bool passwordIsStrongEnough(const String& password, String* reason = nullptr);
  static String randomHex(size_t hexChars);
  static String hashPassword(const char* username, const char* password, const char* saltHex);

  void handleRoot();
  void handleStatus();
  void handleSystemSummary();
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
  void handleWifiGet();
  void handleWifiSave();

  void handleTimeGet();
  void handleTimeSave();
  void handleConfigExport();
  void handleConfigExportFull();
  void handleConfigImport();
  void handleConfigBackup();
  void handleConfigRestore();
  void handleConfigResetPartial();

  void handleReboot();
  void handleShutdown();
  void handleFactoryReset();
  void handleHomeAssistantGet();
  void handleHomeAssistantSave();
  void handleHomeAssistantReveal();
  void handleSecretReveal();
  void handleHomeAssistantStatus();

  void applyBacklightNow(uint8_t percent);
  void applyTouchNow(bool enabled);
  void applyTardisNow();
  void applyTardisPinNow(uint8_t pin, bool enabled, const char* label);
  void ensureTardisInteriorRgbReady();
  void ensureTardisExteriorRgbwReady();
  void applyTardisInteriorRgbColor(uint32_t color);
  void applyTardisExteriorRgbwColor(uint32_t color);
  uint32_t tardisInteriorRgbColorForCurrentState() const;
  uint32_t tardisExteriorRgbwColorForCurrentState() const;
  void updateTardisAnimationNow(bool force = false);
  void applySettingsRuntimeState();
  void saveSettingsThreadSafe();  // SECURITY: Mutex-protected settings save (MED-02)
  String historyJson() const;

  void handleCalPoint();
  void handleCalClear();
  void handleCalMode();
  void handleOtaGet();
  void handleOtaSave();
  void handleReleaseGet();
  void handleReleaseCheck();
  void handleReleaseInstall();
  void handleMqttGet();
  void handleMqttSave();
  void handleNotificationsGet();
  void handleNotificationsSave();
  void handleNotificationsTest();
  void handleSlackReveal();
  void handleDebugLogsGet();
  void handleDebugLogsClear();

  void updateAlertState(float dbInstant, float leq, float peak);
  void enqueueNotification(uint8_t alertState, bool isTest, float dbInstant, float leq, float peak, uint32_t durationMs = 0);
  void processPendingNotification();
  void syncNotificationResultFromTask();
  bool dispatchNotification(uint8_t alertState,
                            bool isTest,
                            float dbInstant,
                            float leq,
                            float peak,
                            uint32_t durationMs,
                            bool updateAlertTracking);
  bool sendSlackNotification(uint8_t alertState, bool isTest, const String& message, String& summary);
  bool sendTelegramNotification(const String& message, String& summary);
  bool sendWhatsappNotification(const String& message, String& summary);
  bool postJsonToUrl(const String& url,
                     const String& payload,
                     const String& authorization,
                     int& statusCodeOut,
                     String& responseOut);
  String buildNotificationMessage(uint8_t alertState, bool isTest, float dbInstant, float leq, float peak, uint32_t durationMs) const;
  String notificationsJson(bool includeSecrets = false) const;
  static const char* alertStateName(uint8_t alertState);

  uint32_t _orangeZoneSinceMs = 0;
  uint32_t _redZoneSinceMs = 0;
  uint32_t _activeAlertStartedMs = 0;
  uint8_t _alertState = 0;
  uint8_t _lastNotifiedAlertState = 0;
  bool _notificationPending = false;
  bool _notificationPendingTest = false;
  uint8_t _pendingNotificationState = 0;
  float _pendingNotificationDb = 0.0f;
  float _pendingNotificationLeq = 0.0f;
  float _pendingNotificationPeak = 0.0f;
  uint32_t _pendingNotificationDurationMs = 0;
  uint32_t _notificationLastAttemptTs = 0;
  uint32_t _notificationLastSuccessTs = 0;
  bool _notificationLastOk = false;
  String _notificationLastEvent = "idle";
  String _notificationLastResult;
};
