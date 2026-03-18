#include "WebManager.h"
#include "DebugLog.h"
#include "LiveEventServer.h"
#include "JsonHelpers.h"
#include "ReleaseUpdateManager.h"
#include "lvgl_v8_port.h"
#include "ui/UiManager.h"

#include <WiFi.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <esp_random.h>
#include <mbedtls/sha256.h>
#include <ctime>
#include <sys/time.h>
#include <math.h>
#include <cstring>
#include <ctype.h>

#include "AudioEngine.h"
#include "AppConfig.h"
#include "AppRuntimeStats.h"

#define Serial0 DebugSerial0

extern AudioEngine g_audio;

static uint32_t g_bootMs = 0;
static float g_webDbInstant = 0.0f;
static float g_webLeq = 0.0f;
static float g_webPeak = 0.0f;

static constexpr char SP7_SESSION_COOKIE_NAME[] = "sp7_session";
static constexpr HTTPMethod kSyncHttpGet = static_cast<HTTPMethod>(::HTTP_GET);
static constexpr HTTPMethod kSyncHttpPost = static_cast<HTTPMethod>(::HTTP_POST);
static constexpr uint32_t kNotificationHttpConnectTimeoutMs = 5000UL;
static constexpr uint32_t kNotificationHttpTimeoutMs = 8000UL;

static uint32_t tardisColorHex(uint8_t r, uint8_t g, uint8_t b) {
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static float clamp01f(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}

static float smoothStep01(float t) {
  t = clamp01f(t);
  return t * t * (3.0f - 2.0f * t);
}

static float mixf(float a, float b, float amount) {
  amount = clamp01f(amount);
  return a + ((b - a) * amount);
}

static float tardisPulseWithPlateaus01(float phase,
                                       float riseSpan,
                                       float highHoldSpan,
                                       float fallSpan,
                                       bool mechanicalRamp) {
  phase = phase - floorf(phase);
  if (phase < 0.0f) phase += 1.0f;

  riseSpan = clamp01f(riseSpan);
  highHoldSpan = clamp01f(highHoldSpan);
  fallSpan = clamp01f(fallSpan);
  const float total = riseSpan + highHoldSpan + fallSpan;
  if (total >= 1.0f || riseSpan <= 0.0f || fallSpan <= 0.0f) {
    return 0.0f;
  }

  if (phase < riseSpan) {
    const float t = phase / riseSpan;
    if (mechanicalRamp) {
      return mixf(t, smoothStep01(t), 0.42f);
    }
    return 0.5f - 0.5f * cosf(t * PI);
  }

  if (phase < (riseSpan + highHoldSpan)) {
    return 1.0f;
  }

  const float fallStart = riseSpan + highHoldSpan;
  if (phase < (fallStart + fallSpan)) {
    const float t = (phase - fallStart) / fallSpan;
    if (mechanicalRamp) {
      return 1.0f - mixf(t, smoothStep01(t), 0.42f);
    }
    return 0.5f + 0.5f * cosf(t * PI);
  }

  return 0.0f;
}

struct NotificationVisualStyle {
  const char* emoji;
  const char* title;
  const char* slackColor;
  const char* slackIconEmoji;
};

static NotificationVisualStyle notificationVisualStyle(uint8_t alertState, bool isTest) {
  if (isTest) return {"🧪", "Test de notification", "#1E88E5", ":test_tube:"};
  if (alertState == 2) return {"🚨", "Alerte critique", "#E53935", ":rotating_light:"};
  if (alertState == 1) return {"⚠️", "Alerte warning", "#FB8C00", ":warning:"};
  return {"✅", "Retour a la normale", "#43A047", ":white_check_mark:"};
}

static String formatAlertDuration(uint32_t durationMs) {
  uint32_t totalSeconds = durationMs / 1000UL;
  const uint32_t hours = totalSeconds / 3600UL;
  totalSeconds %= 3600UL;
  const uint32_t minutes = totalSeconds / 60UL;
  const uint32_t seconds = totalSeconds % 60UL;

  String out;
  if (hours > 0) {
    out += String(hours);
    out += " h ";
  }
  if (minutes > 0 || hours > 0) {
    out += String(minutes);
    out += " min ";
  }
  out += String(seconds);
  out += " s";
  return out;
}

static void appendBoolField(String& json, const char* key, bool value, bool trailingComma = true) {
  json += "\"";
  json += key;
  json += "\":";
  json += value ? "true" : "false";
  if (trailingComma) json += ",";
}

static void appendWifiJson(String& json, bool wifiConnected, const String& ip, int rssi, const String& ssid) {
  json += "\"wifi\":"; json += (wifiConnected ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "ip", ip.c_str());
  sp7json::appendEscapedField(json, "ssid", ssid.c_str());
  json += "\"rssi\":"; json += String(rssi); json += ",";
}

static uint64_t currentUnixTimeMs(bool* hasTimeOut = nullptr) {
  struct timeval tv;
  const bool hasTime = gettimeofday(&tv, nullptr) == 0 && tv.tv_sec > 946684800;
  if (hasTimeOut) *hasTimeOut = hasTime;
  if (!hasTime) return 0;
  return ((uint64_t)tv.tv_sec * 1000ULL) + (uint64_t)(tv.tv_usec / 1000);
}

static void appendTimeJson(String& json, bool hasTime, const char* timeText) {
  const uint64_t timeUnixMs = hasTime ? currentUnixTimeMs(nullptr) : 0;
  json += "\"time_ok\":"; json += (hasTime ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "time", hasTime ? timeText : "");
  json += "\"timeUnixMs\":";
  if (timeUnixMs > 0) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)timeUnixMs);
    json += buf;
  } else {
    json += "0";
  }
  json += ",";
}

static void appendDeviceJson(String& json,
                             const float mcuTempC,
                             const bool mcuTempOk,
                             const OtaManager* ota,
                             MqttManager* mqtt) {
  json += "\"version\":\""; json += String(SOUNDPANEL7_VERSION); json += "\",";
  json += "\"buildDate\":\""; json += String(SOUNDPANEL7_BUILD_DATE); json += "\",";
  json += "\"buildEnv\":\""; json += String(SOUNDPANEL7_BUILD_ENV); json += "\",";
  json += "\"mcuTempOk\":"; json += (mcuTempOk ? "true" : "false"); json += ",";
  json += "\"mcuTempC\":"; json += (mcuTempOk ? String(mcuTempC, 1) : String("0")); json += ",";
  json += "\"otaEnabled\":"; json += (ota && ota->enabled() ? "true" : "false"); json += ",";
  json += "\"otaStarted\":"; json += (ota && ota->started() ? "true" : "false"); json += ",";
  json += "\"mqttEnabled\":"; json += (mqtt && mqtt->enabled() ? "true" : "false"); json += ",";
  json += "\"mqttConnected\":"; json += (mqtt && mqtt->connected() ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "mqttLastError", (mqtt && mqtt->lastError()) ? mqtt->lastError() : "");
}

static void appendReleaseUpdateJson(String& json, const ReleaseUpdateManager* releaseUpdate) {
  json += "\"releaseUpdateChecked\":";
  json += (releaseUpdate && releaseUpdate->hasChecked() ? "true" : "false");
  json += ",";
  json += "\"releaseUpdateOk\":";
  json += (releaseUpdate && releaseUpdate->lastCheckOk() ? "true" : "false");
  json += ",";
  json += "\"releaseUpdateAvailable\":";
  json += (releaseUpdate && releaseUpdate->updateAvailable() ? "true" : "false");
  json += ",";
  json += "\"releaseUpdateCheckedAt\":";
  json += String(releaseUpdate ? releaseUpdate->lastCheckUnix() : 0U);
  json += ",";
  json += "\"releaseHttpCode\":";
  json += String(releaseUpdate ? releaseUpdate->lastHttpCode() : 0);
  json += ",";
  sp7json::appendEscapedField(json, "releaseManifestUrl",
                              (releaseUpdate && releaseUpdate->manifestUrl()) ? releaseUpdate->manifestUrl() : "");
  sp7json::appendEscapedField(json, "releaseCurrentVersion",
                              (releaseUpdate && releaseUpdate->currentVersion()) ? releaseUpdate->currentVersion() : "");
  sp7json::appendEscapedField(json, "releaseLatestVersion",
                              (releaseUpdate && releaseUpdate->latestVersion()[0]) ? releaseUpdate->latestVersion() : "");
  sp7json::appendEscapedField(json, "releasePublishedAt",
                              (releaseUpdate && releaseUpdate->publishedAt()[0]) ? releaseUpdate->publishedAt() : "");
  sp7json::appendEscapedField(json, "releaseReleaseUrl",
                              (releaseUpdate && releaseUpdate->releaseUrl()[0]) ? releaseUpdate->releaseUrl() : "");
  sp7json::appendEscapedField(json, "releaseOtaUrl",
                              (releaseUpdate && releaseUpdate->otaUrl()[0]) ? releaseUpdate->otaUrl() : "");
  sp7json::appendEscapedField(json, "releaseOtaSha256",
                              (releaseUpdate && releaseUpdate->otaSha256()[0]) ? releaseUpdate->otaSha256() : "");
  json += "\"releaseInstallInProgress\":";
  json += (releaseUpdate && releaseUpdate->installInProgress() ? "true" : "false");
  json += ",";
  json += "\"releaseInstallFinished\":";
  json += (releaseUpdate && releaseUpdate->installFinished() ? "true" : "false");
  json += ",";
  json += "\"releaseInstallSucceeded\":";
  json += (releaseUpdate && releaseUpdate->installSucceeded() ? "true" : "false");
  json += ",";
  json += "\"releaseInstallStartedAt\":";
  json += String(releaseUpdate ? releaseUpdate->installStartedUnix() : 0U);
  json += ",";
  json += "\"releaseInstallFinishedAt\":";
  json += String(releaseUpdate ? releaseUpdate->installFinishedUnix() : 0U);
  json += ",";
  json += "\"releaseInstallTotalBytes\":";
  json += String(releaseUpdate ? releaseUpdate->installTotalBytes() : 0U);
  json += ",";
  json += "\"releaseInstallWrittenBytes\":";
  json += String(releaseUpdate ? releaseUpdate->installWrittenBytes() : 0U);
  json += ",";
  json += "\"releaseInstallProgressPct\":";
  json += String(releaseUpdate ? releaseUpdate->installProgressPct() : 0U);
  json += ",";
  sp7json::appendEscapedField(json, "releaseInstallStatus",
                              (releaseUpdate && releaseUpdate->installStatus()[0]) ? releaseUpdate->installStatus() : "");
  sp7json::appendEscapedField(json, "releaseInstallError",
                              (releaseUpdate && releaseUpdate->installError()[0]) ? releaseUpdate->installError() : "");
  sp7json::appendEscapedField(json, "releaseLastError",
                              (releaseUpdate && releaseUpdate->lastError()[0]) ? releaseUpdate->lastError() : "");
}

static void appendUiStateJson(String& json, const SettingsV1* s, const SharedHistory* history, bool includeCalibrationPointCount) {
  json += "\"backlight\":"; json += String(s ? s->backlight : 0); json += ",";
  json += "\"greenMax\":"; json += String(s ? s->th.greenMax : DEFAULT_GREEN_MAX); json += ",";
  json += "\"orangeMax\":"; json += String(s ? s->th.orangeMax : DEFAULT_ORANGE_MAX); json += ",";
  json += "\"historyMinutes\":"; json += String(s ? s->historyMinutes : DEFAULT_HISTORY_MINUTES); json += ",";
  json += "\"liveEnabled\":"; json += (s && s->liveEnabled ? "true" : "false"); json += ",";
  json += "\"touchEnabled\":"; json += (s && s->touchEnabled ? "true" : "false"); json += ",";
  json += "\"hasScreen\":"; json += (SOUNDPANEL7_HAS_SCREEN ? "true" : "false"); json += ",";
  json += "\"dashboardPage\":"; json += String(s ? s->dashboardPage : DEFAULT_DASHBOARD_PAGE); json += ",";
  json += "\"dashboardFullscreenMask\":"; json += String(s ? s->dashboardFullscreenMask : DEFAULT_DASHBOARD_FULLSCREEN_MASK); json += ",";
  json += "\"audioSource\":"; json += String(s ? s->audioSource : 1); json += ",";
  json += "\"audioSourceSupportsCalibration\":"; json += (AudioEngine::sourceSupportsCalibration(s ? s->audioSource : 1) ? "true" : "false"); json += ",";
  json += "\"audioSourceUsesAnalog\":"; json += (AudioEngine::sourceUsesAnalog(s ? s->audioSource : 1) ? "true" : "false"); json += ",";
  json += "\"supportsDashboardDisplay\":"; json += (SOUNDPANEL7_HAS_SCREEN ? "true" : "false"); json += ",";
  json += "\"supportsDashboardPin\":"; json += (SOUNDPANEL7_HAS_SCREEN ? "true" : "false"); json += ",";
  json += "\"supportsTardisControl\":"; json += (SOUNDPANEL7_TARDIS_SUPPORTED ? "true" : "false"); json += ",";
  json += "\"supportsTardisInteriorRgb\":"; json += (SOUNDPANEL7_TARDIS_INTERIOR_RGB_SUPPORTED ? "true" : "false"); json += ",";
  json += "\"tardisModeEnabled\":"; json += (s && s->tardisModeEnabled ? "true" : "false"); json += ",";
  json += "\"tardisInteriorLedEnabled\":"; json += (s && s->tardisInteriorLedEnabled ? "true" : "false"); json += ",";
  json += "\"tardisExteriorLedEnabled\":"; json += (s && s->tardisExteriorLedEnabled ? "true" : "false"); json += ",";
  json += "\"tardisInteriorLedPin\":"; json += String((int)SOUNDPANEL7_TARDIS_INTERIOR_LED_PIN); json += ",";
  json += "\"tardisExteriorLedPin\":"; json += String((int)SOUNDPANEL7_TARDIS_EXTERIOR_LED_PIN); json += ",";
  json += "\"tardisInteriorRgbPin\":"; json += String((int)SOUNDPANEL7_TARDIS_INTERIOR_RGB_PIN); json += ",";
  json += "\"tardisInteriorRgbMode\":"; json += String(s ? s->tardisInteriorRgbMode : TARDIS_INTERIOR_RGB_MODE_ALERT); json += ",";
  json += "\"tardisInteriorRgbColor\":"; json += String(s ? s->tardisInteriorRgbColor : TARDIS_INTERIOR_RGB_DEFAULT_COLOR); json += ",";
  json += "\"audioResponseMode\":"; json += String(s ? s->audioResponseMode : 0); json += ",";
  json += "\"historyCapacity\":"; json += String(SharedHistory::POINT_COUNT); json += ",";
  json += "\"historySamplePeriodMs\":"; json += String(history ? history->samplePeriodMs() : 3000); json += ",";
  json += "\"warningHoldSec\":"; json += String(s ? (s->orangeAlertHoldMs / MS_PER_SECOND) : (DEFAULT_WARNING_HOLD_MS / MS_PER_SECOND)); json += ",";
  json += "\"criticalHoldSec\":"; json += String(s ? (s->redAlertHoldMs / MS_PER_SECOND) : (DEFAULT_CRITICAL_HOLD_MS / MS_PER_SECOND)); json += ",";
  if (includeCalibrationPointCount) {
    json += "\"calibrationPointCount\":"; json += String(s ? s->calibrationPointCount : 3); json += ",";
  }
  json += "\"calibrationCaptureSec\":"; json += String(s ? (s->calibrationCaptureMs / MS_PER_SECOND) : (DEFAULT_CALIBRATION_CAPTURE_MS / MS_PER_SECOND)); json += ",";
}

static bool tardisPinConflictsWithAudioPin(uint8_t pin, const SettingsV1* s) {
  if (pin == SOUNDPANEL7_DEFAULT_ANALOG_PIN
      || pin == SOUNDPANEL7_DEFAULT_PDM_CLK_PIN
      || pin == SOUNDPANEL7_DEFAULT_PDM_DATA_PIN
      || pin == SOUNDPANEL7_DEFAULT_INMP441_BCLK_PIN
      || pin == SOUNDPANEL7_DEFAULT_INMP441_WS_PIN
      || pin == SOUNDPANEL7_DEFAULT_INMP441_DATA_PIN) {
    return true;
  }
  if (!s) return false;
  return pin == s->analogPin
      || pin == s->pdmClkPin
      || pin == s->pdmDataPin
      || pin == s->inmp441BclkPin
      || pin == s->inmp441WsPin
      || pin == s->inmp441DataPin;
}

static void appendPinStateJson(String& json, bool configured) {
  json += "\"pinConfigured\":";
  json += configured ? "true" : "false";
  json += ",";
}

static void appendAudioMetricsJson(String& json, const AudioMetrics& am) {
  json += "\"db\":"; json += String(g_webDbInstant, 1); json += ",";
  json += "\"leq\":"; json += String(g_webLeq, 1); json += ",";
  json += "\"peak\":"; json += String(g_webPeak, 1); json += ",";
  json += "\"rawRms\":"; json += String(am.rawRms, 2); json += ",";
  json += "\"rawPseudoDb\":"; json += String(am.rawPseudoDb, 1); json += ",";
  json += "\"rawAdcMean\":"; json += String(am.rawAdcMean); json += ",";
  json += "\"rawAdcLast\":"; json += String(am.rawAdcLast); json += ",";
  json += "\"analogOk\":"; json += (am.analogOk ? "true" : "false"); json += ",";
}

static void appendRuntimeStatsJson(String& json, const RuntimeStats& stats) {
  json += "\"cpuIdlePct\":"; json += String(stats.cpuIdlePct); json += ",";
  json += "\"cpuLoadPct\":"; json += String(stats.cpuLoadPct); json += ",";
  json += "\"lvglIdlePct\":"; json += String(stats.lvglIdlePct); json += ",";
  json += "\"lvglLoadPct\":"; json += String(stats.lvglLoadPct); json += ",";
  json += "\"lvglUiWorkUs\":"; json += String(stats.uiWorkLastUs); json += ",";
  json += "\"lvglUiWorkMaxUs\":"; json += String(stats.uiWorkMaxUs); json += ",";
  json += "\"lvglHandlerUs\":"; json += String(stats.lvHandlerLastUs); json += ",";
  json += "\"lvglHandlerMaxUs\":"; json += String(stats.lvHandlerMaxUs); json += ",";
  json += "\"lvglObjCount\":"; json += String(stats.lvObjCount); json += ",";
  json += "\"heapInternalFree\":"; json += String(stats.heapInternalFree); json += ",";
  json += "\"heapInternalTotal\":"; json += String(stats.heapInternalTotal); json += ",";
  json += "\"heapInternalMin\":"; json += String(stats.heapInternalMin); json += ",";
  json += "\"heapPsramFree\":"; json += String(stats.heapPsramFree); json += ",";
  json += "\"heapPsramTotal\":"; json += String(stats.heapPsramTotal); json += ",";
  json += "\"heapPsramMin\":"; json += String(stats.heapPsramMin); json += ",";
  sp7json::appendEscapedField(json, "activePage", stats.activePage);
}

static uint32_t currentUnixTimestamp() {
  time_t now = time(nullptr);
  return now > 946684800 ? (uint32_t)now : 0U;
}

static String trimmedHttpResponse(String value) {
  value.trim();
  if (value.length() > 96) {
    value.remove(96);
    value += "...";
  }
  return value;
}

void WebManager::addCommonSecurityHeaders(bool noStore) {
  _srv.sendHeader("X-Frame-Options", "DENY");
  _srv.sendHeader("X-Content-Type-Options", "nosniff");
  _srv.sendHeader("Referrer-Policy", "same-origin");
  if (noStore) {
    _srv.sendHeader("Cache-Control", "no-store, max-age=0");
    _srv.sendHeader("Pragma", "no-cache");
  }
}

bool WebManager::secureEquals(const char* a, const char* b) {
  if (!a || !b) return false;
  const size_t lenA = strlen(a);
  const size_t lenB = strlen(b);
  const size_t maxLen = lenA > lenB ? lenA : lenB;
  uint8_t diff = (uint8_t)(lenA ^ lenB);
  for (size_t i = 0; i < maxLen; i++) {
    const uint8_t ca = i < lenA ? (uint8_t)a[i] : 0;
    const uint8_t cb = i < lenB ? (uint8_t)b[i] : 0;
    diff |= (uint8_t)(ca ^ cb);
  }
  return diff == 0;
}

bool WebManager::normalizeUsername(String& username) {
  username.trim();
  username.toLowerCase();
  if (username.length() < 3 || username.length() > WEB_USERNAME_MAX_LENGTH) return false;
  for (size_t i = 0; i < username.length(); i++) {
    const char c = username[i];
    const bool allowed = (c >= 'a' && c <= 'z')
      || (c >= '0' && c <= '9')
      || c == '.'
      || c == '_'
      || c == '-';
    if (!allowed) return false;
  }
  return true;
}

bool WebManager::homeAssistantTokenIsValid(const String& token) {
  const size_t len = token.length();
  if (len < 16 || len > HOME_ASSISTANT_TOKEN_MAX_LENGTH) return false;
  for (size_t i = 0; i < len; i++) {
    const char c = token[i];
    const bool allowed = (c >= 'a' && c <= 'z')
      || (c >= 'A' && c <= 'Z')
      || (c >= '0' && c <= '9')
      || c == '-'
      || c == '_';
    if (!allowed) return false;
  }
  return true;
}

bool WebManager::passwordIsStrongEnough(const String& password, String* reason) {
  const size_t len = password.length();
  if (len < WEB_PASSWORD_MIN_LENGTH || len > WEB_PASSWORD_MAX_LENGTH) {
    if (reason) *reason = "password must be 10 to 64 chars";
    return false;
  }

  bool hasLower = false;
  bool hasUpper = false;
  bool hasDigit = false;
  bool hasSymbol = false;
  for (size_t i = 0; i < len; i++) {
    const char c = password[i];
    if (c >= 'a' && c <= 'z') hasLower = true;
    else if (c >= 'A' && c <= 'Z') hasUpper = true;
    else if (c >= '0' && c <= '9') hasDigit = true;
    else hasSymbol = true;
  }

  const uint8_t classCount = (hasLower ? 1 : 0) + (hasUpper ? 1 : 0) + (hasDigit ? 1 : 0) + (hasSymbol ? 1 : 0);
  if (classCount < 3) {
    if (reason) *reason = "password needs 3 character classes";
    return false;
  }
  return true;
}

String WebManager::randomHex(size_t hexChars) {
  static const char kHexChars[] = "0123456789abcdef";
  String out;
  out.reserve(hexChars);
  while (out.length() < hexChars) {
    const uint32_t value = esp_random();
    for (int shift = 28; shift >= 0 && out.length() < hexChars; shift -= 4) {
      out += kHexChars[(value >> shift) & 0x0F];
    }
  }
  return out;
}

String WebManager::hashPassword(const char* username, const char* password, const char* saltHex) {
  uint8_t digest[32] = {0};
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);

  auto runRound = [&](bool firstRound) {
    mbedtls_sha256_starts(&ctx, 0);
    if (!firstRound) {
      mbedtls_sha256_update(&ctx, digest, sizeof(digest));
    }
    if (saltHex) mbedtls_sha256_update(&ctx, (const uint8_t*)saltHex, strlen(saltHex));
    if (username) mbedtls_sha256_update(&ctx, (const uint8_t*)username, strlen(username));
    if (password) mbedtls_sha256_update(&ctx, (const uint8_t*)password, strlen(password));
    mbedtls_sha256_finish(&ctx, digest);
  };

  runRound(true);
  for (uint16_t i = 1; i < WEB_PASSWORD_HASH_ROUNDS; i++) runRound(false);
  mbedtls_sha256_free(&ctx);

  static const char kHexChars[] = "0123456789abcdef";
  char out[(32 * 2) + 1];
  for (size_t i = 0; i < sizeof(digest); i++) {
    out[i * 2] = kHexChars[(digest[i] >> 4) & 0x0F];
    out[(i * 2) + 1] = kHexChars[digest[i] & 0x0F];
  }
  out[sizeof(out) - 1] = '\0';
  return String(out);
}

bool WebManager::webUsersConfigured() const {
  return _store && _store->webUserCount() > 0;
}

void WebManager::cleanupExpiredSessions() {
  const uint32_t now = millis();
  for (WebSession& session : _sessions) {
    if (!session.active) continue;
    if ((uint32_t)(now - session.lastSeenMs) > WEB_SESSION_IDLE_TIMEOUT_MS) {
      session = WebSession{};
    }
  }
}

const WebManager::WebSession* WebManager::findSessionByToken(const char* token, bool touch) {
  if (!token || !token[0]) return nullptr;
  cleanupExpiredSessions();

  const uint32_t now = millis();
  for (WebSession& session : _sessions) {
    if (!session.active) continue;
    if (!secureEquals(session.sessionToken, token)) continue;
    if (touch) session.lastSeenMs = now;
    return &session;
  }
  return nullptr;
}

const WebManager::WebSession* WebManager::findSessionByLiveToken(const char* token, bool touch) {
  if (!token || !token[0]) return nullptr;
  cleanupExpiredSessions();

  const uint32_t now = millis();
  for (WebSession& session : _sessions) {
    if (!session.active) continue;
    if (!secureEquals(session.liveToken, token)) continue;
    if (touch) session.lastSeenMs = now;
    return &session;
  }
  return nullptr;
}

const WebManager::WebSession* WebManager::currentSession(bool touch) {
  return findSessionByToken(extractCookieValue(SP7_SESSION_COOKIE_NAME).c_str(), touch);
}

WebManager::WebSession* WebManager::newSessionSlot() {
  cleanupExpiredSessions();
  for (WebSession& session : _sessions) {
    if (!session.active) return &session;
  }

  WebSession* oldest = &_sessions[0];
  for (WebSession& session : _sessions) {
    if (session.lastSeenMs < oldest->lastSeenMs) oldest = &session;
  }
  return oldest;
}

void WebManager::invalidateSessionToken(const char* token) {
  if (!token || !token[0]) return;
  for (WebSession& session : _sessions) {
    if (session.active && secureEquals(session.sessionToken, token)) {
      session = WebSession{};
      if (_live) _live->close();
      return;
    }
  }
}

void WebManager::invalidateSessionsForUser(const char* username) {
  if (!username || !username[0]) return;
  bool changed = false;
  for (WebSession& session : _sessions) {
    if (session.active && secureEquals(session.username, username)) {
      session = WebSession{};
      changed = true;
    }
  }
  if (changed && _live) _live->close();
}

void WebManager::clearAllSessions() {
  for (WebSession& session : _sessions) session = WebSession{};
  if (_live) _live->close();
}

bool WebManager::issueSessionForUser(const char* username, String& liveTokenOut) {
  if (!username || !username[0]) return false;

  WebSession* session = newSessionSlot();
  if (!session) return false;

  *session = WebSession{};
  session->active = true;
  const String sessionToken = randomHex(sizeof(session->sessionToken) - 1);
  const String liveToken = randomHex(sizeof(session->liveToken) - 1);
  if (!sp7json::safeCopy(session->sessionToken, sizeof(session->sessionToken), sessionToken)) return false;
  if (!sp7json::safeCopy(session->liveToken, sizeof(session->liveToken), liveToken)) return false;
  if (!sp7json::safeCopy(session->username, sizeof(session->username), String(username))) return false;
  session->lastSeenMs = millis();

  liveTokenOut = liveToken;
  _srv.sendHeader("Set-Cookie", String(SP7_SESSION_COOKIE_NAME) + "=" + sessionToken + "; Path=/; HttpOnly; SameSite=Strict");
  return true;
}

String WebManager::extractCookieValue(const char* cookieName) const {
  if (!cookieName || !_srv.hasHeader("Cookie")) return "";
  const String cookieHeader = _srv.header("Cookie");
  const String prefix = String(cookieName) + "=";

  int start = 0;
  while (start < (int)cookieHeader.length()) {
    while (start < (int)cookieHeader.length() && (cookieHeader[start] == ' ' || cookieHeader[start] == ';')) start++;
    const int end = cookieHeader.indexOf(';', start);
    const String part = cookieHeader.substring(start, end >= 0 ? end : cookieHeader.length());
    if (part.startsWith(prefix)) return part.substring(prefix.length());
    if (end < 0) break;
    start = end + 1;
  }
  return "";
}

String WebManager::extractAuthorizationBearer() const {
  if (!_srv.hasHeader("Authorization")) return "";
  String header = _srv.header("Authorization");
  header.trim();
  if (header.length() < 7) return "";
  if (!header.substring(0, 7).equalsIgnoreCase("Bearer ")) return "";
  String token = header.substring(7);
  token.trim();
  return token;
}

bool WebManager::requireWebAuth() {
  if (currentSession()) return true;
  addCommonSecurityHeaders();
  _srv.sendHeader("Set-Cookie", String(SP7_SESSION_COOKIE_NAME) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  replyErrorJson(401, "auth required");
  return false;
}

bool WebManager::homeAssistantTokenConfigured() const {
  return _s && _s->homeAssistantToken[0] != '\0';
}

bool WebManager::requireHomeAssistantToken() {
  if (!_s) {
    replyErrorJson(500, "settings missing");
    return false;
  }
  if (!homeAssistantTokenConfigured()) {
    replyErrorJson(403, "home assistant token not configured");
    return false;
  }

  const String token = extractAuthorizationBearer();
  if (homeAssistantTokenIsValid(token) && secureEquals(_s->homeAssistantToken, token.c_str())) {
    return true;
  }

  addCommonSecurityHeaders();
  replyErrorJson(401, "invalid home assistant token");
  return false;
}

bool WebManager::begin(SettingsStore* store,
                       SettingsV1* settings,
                       NetManager* net,
                       esp_panel::board::Board* board,
                       SharedHistory* history,
                       OtaManager* ota,
                       ReleaseUpdateManager* releaseUpdate,
                       MqttManager* mqtt,
                       UiManager* ui) {
  _store = store;
  _s = settings;
  _net = net;
  _board = board;
  _history = history;
  _ota = ota;
  _releaseUpdate = releaseUpdate;
  _mqtt = mqtt;
  _ui = ui;
  if (!_live) _live = new LiveEventServer(81, "/api/events");
  if (_net) {
    _net->setConfigPortalStateCallback([this](bool active) {
      if (active) {
        stopHttpServer();
      } else {
        syncHttpAvailability();
      }
    });
  }

  if (!g_bootMs) g_bootMs = millis();
  if (_started) return true;

  const char* headers[] = {"Cookie", "Authorization"};
  _srv.collectHeaders(headers, 2);
  routes();
  setupLiveStream();
  _started = true;
  applyTardisNow();
  Serial0.println("[WEB] LIVE SSE on 81");
  syncHttpAvailability();
  return true;
}

static bool dueOnFixedPeriod(uint32_t& anchorMs, uint32_t periodMs, uint32_t nowMs, bool force = false) {
  if (force) {
    anchorMs = nowMs;
    return true;
  }
  if (anchorMs == 0) {
    anchorMs = nowMs;
    return true;
  }
  if ((uint32_t)(nowMs - anchorMs) < periodMs) return false;

  do {
    anchorMs += periodMs;
  } while ((uint32_t)(nowMs - anchorMs) >= periodMs);
  return true;
}

void WebManager::updateMetrics(float dbInstant, float leq, float peak) {
  g_webDbInstant = dbInstant;
  g_webLeq = leq;
  g_webPeak = peak;

  pushLiveMetrics();
  updateAlertState(dbInstant, leq, peak);
}

void WebManager::routes() {
  _srv.on("/", kSyncHttpGet, [this]() { handleRoot(); });

  _srv.on("/api/auth/status", kSyncHttpGet, [this]() { handleAuthStatus(); });
  _srv.on("/api/auth/login", kSyncHttpPost, [this]() { handleAuthLogin(); });
  _srv.on("/api/auth/logout", kSyncHttpPost, [this]() { handleAuthLogout(); });
  _srv.on("/api/auth/bootstrap", kSyncHttpPost, [this]() { handleAuthBootstrap(); });
  _srv.on("/api/users", kSyncHttpGet, [this]() { handleUsersGet(); });
  _srv.on("/api/users/create", kSyncHttpPost, [this]() { handleUsersCreate(); });
  _srv.on("/api/users/password", kSyncHttpPost, [this]() { handleUsersPassword(); });
  _srv.on("/api/users/delete", kSyncHttpPost, [this]() { handleUsersDelete(); });

  _srv.on("/api/ha/status", kSyncHttpGet, [this]() { handleHomeAssistantStatus(); });
  _srv.on("/api/homeassistant", kSyncHttpGet, [this]() { handleHomeAssistantGet(); });
  _srv.on("/api/homeassistant", kSyncHttpPost, [this]() { handleHomeAssistantSave(); });
  _srv.on("/api/status", kSyncHttpGet, [this]() { handleStatus(); });
  _srv.on("/api/system", kSyncHttpGet, [this]() { handleSystemSummary(); });

  _srv.on("/api/pin", kSyncHttpPost, [this]() { handlePinSave(); });

  _srv.on("/api/ui", kSyncHttpPost, [this]() { handleUiSave(); });
  _srv.on("/api/live", kSyncHttpGet, [this]() { handleLiveGet(); });
  _srv.on("/api/live", kSyncHttpPost, [this]() { handleLiveSave(); });
  _srv.on("/api/wifi", kSyncHttpGet, [this]() { handleWifiGet(); });
  _srv.on("/api/wifi", kSyncHttpPost, [this]() { handleWifiSave(); });

  _srv.on("/api/time", kSyncHttpGet,  [this]() { handleTimeGet(); });
  _srv.on("/api/time", kSyncHttpPost, [this]() { handleTimeSave(); });
  _srv.on("/api/config/export", kSyncHttpGet, [this]() { handleConfigExport(); });
  _srv.on("/api/config/export_full", kSyncHttpGet, [this]() { handleConfigExportFull(); });
  _srv.on("/api/config/import", kSyncHttpPost, [this]() { handleConfigImport(); });
  _srv.on("/api/config/backup", kSyncHttpPost, [this]() { handleConfigBackup(); });
  _srv.on("/api/config/restore", kSyncHttpPost, [this]() { handleConfigRestore(); });
  _srv.on("/api/config/reset_partial", kSyncHttpPost, [this]() { handleConfigResetPartial(); });

  _srv.on("/api/ota", kSyncHttpGet,  [this]() { handleOtaGet(); });
  _srv.on("/api/ota", kSyncHttpPost, [this]() { handleOtaSave(); });
  _srv.on("/api/release", kSyncHttpGet,  [this]() { handleReleaseGet(); });
  _srv.on("/api/release/check", kSyncHttpPost, [this]() { handleReleaseCheck(); });
  _srv.on("/api/release/install", kSyncHttpPost, [this]() { handleReleaseInstall(); });

  _srv.on("/api/mqtt", kSyncHttpGet,  [this]() { handleMqttGet(); });
  _srv.on("/api/mqtt", kSyncHttpPost, [this]() { handleMqttSave(); });
  _srv.on("/api/notifications", kSyncHttpGet,  [this]() { handleNotificationsGet(); });
  _srv.on("/api/notifications", kSyncHttpPost, [this]() { handleNotificationsSave(); });
  _srv.on("/api/notifications/test", kSyncHttpPost, [this]() { handleNotificationsTest(); });
  _srv.on("/api/debug/logs", kSyncHttpGet, [this]() { handleDebugLogsGet(); });
  _srv.on("/api/debug/logs/clear", kSyncHttpPost, [this]() { handleDebugLogsClear(); });

  _srv.on("/api/calibrate", kSyncHttpPost, [this]() { handleCalPoint(); });
  _srv.on("/api/calibrate/clear", kSyncHttpPost, [this]() { handleCalClear(); });
  _srv.on("/api/calibrate/mode", kSyncHttpPost, [this]() { handleCalMode(); });

  _srv.on("/api/reboot", kSyncHttpPost, [this]() { handleReboot(); });
  _srv.on("/api/shutdown", kSyncHttpPost, [this]() { handleShutdown(); });
  _srv.on("/api/factory_reset", kSyncHttpPost, [this]() { handleFactoryReset(); });

  _srv.onNotFound([this]() { replyText(404, "404\n"); });
}

void WebManager::loop() {
  if (!_started) return;
  updateTardisAnimationNow();

  const bool localOtaInProgress = _ota && _ota->inProgress();
  const bool releaseInstallInProgress = _releaseUpdate && _releaseUpdate->installInProgress();

  if (localOtaInProgress) {
    stopHttpServer();
    if (_live && _live->clientCount() > 0) {
      _live->close();
    }
    return;
  }

  if (releaseInstallInProgress && _live && _live->clientCount() > 0) {
    _live->close();
  }

  processPendingNotification();
  syncHttpAvailability();
  if (!_httpListening) return;
  _srv.handleClient();
}

const char* WebManager::alertStateName(uint8_t alertState) {
  switch (alertState) {
    case 1: return "warning";
    case 2: return "critical";
    default: return "recovery";
  }
}

void WebManager::updateAlertState(float dbInstant, float leq, float peak) {
  if (!_s) return;

  const uint32_t now = millis();
  const bool orangeZone = dbInstant > _s->th.greenMax;
  const bool redZone = dbInstant > _s->th.orangeMax;

  if (redZone) {
    if (_redZoneSinceMs == 0) _redZoneSinceMs = now;
  }

  if (orangeZone) {
    if (_orangeZoneSinceMs == 0) _orangeZoneSinceMs = now;
  } else {
    _redZoneSinceMs = 0;
    _orangeZoneSinceMs = 0;
  }

  const bool orangeAlert = _orangeZoneSinceMs != 0 && (now - _orangeZoneSinceMs) >= _s->orangeAlertHoldMs;
  const bool redAlert = _redZoneSinceMs != 0 && (now - _redZoneSinceMs) >= _s->redAlertHoldMs;
  const uint8_t nextAlertState = redAlert ? 2 : (orangeAlert ? 1 : 0);
  const uint8_t previousAlertState = _alertState;
  _alertState = nextAlertState;
  if (SOUNDPANEL7_TARDIS_SUPPORTED && SOUNDPANEL7_TARDIS_INTERIOR_RGB_SUPPORTED
      && _s->tardisModeEnabled && _s->tardisInteriorLedEnabled) {
    applyTardisInteriorRgbColor(tardisInteriorRgbColorForCurrentState());
  }

  if (nextAlertState == previousAlertState) return;

  if (nextAlertState > previousAlertState) {
    if (previousAlertState == 0 && _activeAlertStartedMs == 0) _activeAlertStartedMs = now;
    if (nextAlertState == 2) {
      enqueueNotification(2, false, dbInstant, leq, peak);
    } else if (_s->notifyOnWarning == ALERT_NOTIFY_LEVEL_WARNING) {
      enqueueNotification(1, false, dbInstant, leq, peak);
    }
    return;
  }

  if (nextAlertState == 0 && previousAlertState > 0 && _s->notifyOnRecovery && _lastNotifiedAlertState > 0) {
    const uint32_t durationMs = _activeAlertStartedMs != 0 ? (now - _activeAlertStartedMs) : 0;
    enqueueNotification(0, false, dbInstant, leq, peak, durationMs);
  }
  if (nextAlertState == 0) _activeAlertStartedMs = 0;
}

void WebManager::enqueueNotification(uint8_t alertState, bool isTest, float dbInstant, float leq, float peak, uint32_t durationMs) {
  _notificationPending = true;
  _notificationPendingTest = isTest;
  _pendingNotificationState = alertState;
  _pendingNotificationDb = dbInstant;
  _pendingNotificationLeq = leq;
  _pendingNotificationPeak = peak;
  _pendingNotificationDurationMs = durationMs;
}

void WebManager::processPendingNotification() {
  if (!_notificationPending) return;

  const bool isTest = _notificationPendingTest;
  const uint8_t alertState = _pendingNotificationState;
  const float dbInstant = _pendingNotificationDb;
  const float leq = _pendingNotificationLeq;
  const float peak = _pendingNotificationPeak;
  const uint32_t durationMs = _pendingNotificationDurationMs;

  _notificationPending = false;
  _notificationPendingTest = false;
  _pendingNotificationDurationMs = 0;

  dispatchNotification(alertState, isTest, dbInstant, leq, peak, durationMs, !isTest);
}

void WebManager::startHttpServer() {
  if (_httpListening) return;
  _srv.begin();
  _httpListening = true;
  Serial0.println("[WEB] LISTEN on 80");
  if (WiFi.isConnected()) {
    Serial0.printf("[WEB] URL: http://%s/\n", WiFi.localIP().toString().c_str());
  } else {
    Serial0.println("[WEB] WiFi not connected yet (URL available after connect)");
  }
}

void WebManager::stopHttpServer() {
  if (!_httpListening) return;
  Serial0.println("[WEB] stopping main HTTP server");
  _srv.stop();
  _httpListening = false;
}

void WebManager::syncHttpAvailability() {
  const bool portalActive = _net && _net->isConfigPortalActive();
  if (portalActive) {
    stopHttpServer();
    return;
  }
  startHttpServer();
}

void WebManager::setupLiveStream() {
  if (!_live) return;
  _live->begin(
      [this](const String& token) -> bool {
        if (liveTrafficPaused()) return false;
        return findSessionByLiveToken(token.c_str()) != nullptr;
      },
      [this]() -> String {
        return liveMetricsJson();
      });
}

void WebManager::pushLiveMetrics(bool force) {
  if (liveTrafficPaused()) return;
  if (!_live || _live->clientCount() == 0) return;

  const uint32_t now = millis();
  if (!dueOnFixedPeriod(_lastLivePushMs, LIVE_PUSH_PERIOD_MS, now, force)) return;
  const String payload = liveMetricsJson();
  _live->sendMetrics(payload, now);
}

void WebManager::pushLiveSystem(bool force) {
  if (liveTrafficPaused()) return;
  if (!_live || _live->clientCount() == 0) return;

  const uint32_t now = millis();
  if (!dueOnFixedPeriod(_lastLiveSystemPushMs, LIVE_SYSTEM_PUSH_PERIOD_MS, now, force)) return;
  const String payload = systemSummaryJson();
  _live->sendEvent("system", payload, now);
}

bool WebManager::liveTrafficPaused() const {
  return (_ota && _ota->inProgress())
      || (_releaseUpdate && _releaseUpdate->installInProgress());
}

void WebManager::replyText(int code, const String& txt, const char* contentType) {
  addCommonSecurityHeaders();
  _srv.send(code, contentType, txt);
}

void WebManager::replyJson(int code, const String& json) {
  addCommonSecurityHeaders();
  _srv.send(code, "application/json", json);
}

void WebManager::replyOkJson(bool trailingNewline) {
  replyJson(200, trailingNewline ? "{\"ok\":true}\n" : "{\"ok\":true}");
}

void WebManager::replyOkJsonRebootRecommended() {
  replyJson(200, "{\"ok\":true,\"rebootRecommended\":true}");
}

void WebManager::replyOkJsonRebootRequired() {
  replyJson(200, "{\"ok\":true,\"rebootRequired\":true}");
}

void WebManager::replyErrorJson(int code, const String& error, bool trailingNewline) {
  String json = "{\"ok\":false,\"error\":\"";
  json += error;
  json += "\"}";
  if (trailingNewline) json += "\n";
  replyJson(code, json);
}

bool WebManager::requireSettingsText() {
  if (_s) return true;
  replyText(500, "settings missing\n");
  return false;
}

bool WebManager::requireSettingsJson() {
  if (_s) return true;
  replyErrorJson(500, "settings missing");
  return false;
}

bool WebManager::requireStoreAndSettingsText() {
  if (_store && _s) return true;
  replyText(500, "store/settings missing\n");
  return false;
}

bool WebManager::requireStoreAndSettingsJson() {
  if (_store && _s) return true;
  replyErrorJson(500, "store/settings missing");
  return false;
}

bool WebManager::pinConfigured() const {
#if !SOUNDPANEL7_HAS_SCREEN
  return false;
#else
  return _s && pinCodeIsConfigured(_s->dashboardPin);
#endif
}

String WebManager::statusJson() const {
  const bool wifiConnected = WiFi.isConnected();
  String ip = wifiConnected ? WiFi.localIP().toString() : String("");
  String ssid = wifiConnected ? WiFi.SSID() : String("");
  int rssi = wifiConnected ? WiFi.RSSI() : 0;
  uint32_t up = (millis() - g_bootMs) / 1000;
  uint32_t backupTs = _store ? _store->backupTimestamp() : 0;
  const float mcuTempC = temperatureRead();
  const bool mcuTempOk = !isnan(mcuTempC);

  struct tm ti;
  bool hasTime = getLocalTime(&ti, 0);
  char tbuf[32] = {0};
  if (hasTime) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);

  const AudioMetrics& am = g_audio.metrics();

  String json;
  json.reserve(1500);
  json += "{";
  appendWifiJson(json, wifiConnected, ip, rssi, ssid);
  json += "\"uptime_s\":"; json += String(up); json += ",";
  json += "\"backupTs\":"; json += String(backupTs); json += ",";
  appendTimeJson(json, hasTime, tbuf);
  appendDeviceJson(json, mcuTempC, mcuTempOk, _ota, _mqtt);
  appendReleaseUpdateJson(json, _releaseUpdate);
  appendUiStateJson(json, _s, _history, true);
  appendPinStateJson(json, pinConfigured());
  appendAudioMetricsJson(json, am);
  appendRuntimeStatsJson(json, g_runtimeStats);

  json += "\"cal\":[";
  for (int i = 0; i < CALIBRATION_POINT_MAX; i++) {
    if (i) json += ",";
    json += "{";
    json += "\"valid\":"; json += (_s && _s->calPointValid[i] ? "true" : "false"); json += ",";
    json += "\"refDb\":"; json += String(_s ? _s->calPointRefDb[i] : 0.0f, 1); json += ",";
    json += "\"rawLogRms\":"; json += String(_s ? _s->calPointRawLogRms[i] : 0.0f, 4);
    json += "}";
  }
  json += "]";
  json += ",";
  json += "\"history\":"; json += historyJson();
  json += "}";

  return json;
}

String WebManager::systemSummaryJson() const {
  const bool wifiConnected = WiFi.isConnected();
  const String ip = wifiConnected ? WiFi.localIP().toString() : String("");
  const String ssid = wifiConnected ? WiFi.SSID() : String("");
  const int rssi = wifiConnected ? WiFi.RSSI() : 0;
  const uint32_t up = (millis() - g_bootMs) / 1000;
  const uint32_t backupTs = _store ? _store->backupTimestamp() : 0;
  const float mcuTempC = temperatureRead();
  const bool mcuTempOk = !isnan(mcuTempC);
  const size_t fsTotalBytes = LittleFS.totalBytes();
  const size_t fsUsedBytes = LittleFS.usedBytes();

  struct tm ti;
  const bool hasTime = getLocalTime(&ti, 0);
  char tbuf[32] = {0};
  if (hasTime) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);

  String json;
  json.reserve(768);
  json += "{";
  appendWifiJson(json, wifiConnected, ip, rssi, ssid);
  json += "\"uptime_s\":"; json += String(up); json += ",";
  json += "\"backupTs\":"; json += String(backupTs); json += ",";
  appendTimeJson(json, hasTime, tbuf);
  json += "\"mcuTempOk\":"; json += (mcuTempOk ? "true" : "false"); json += ",";
  json += "\"mcuTempC\":"; json += (mcuTempOk ? String(mcuTempC, 1) : String("0")); json += ",";
  json += "\"fsTotalBytes\":"; json += String((uint32_t)fsTotalBytes); json += ",";
  json += "\"fsUsedBytes\":"; json += String((uint32_t)fsUsedBytes); json += ",";
  json += "\"otaEnabled\":"; json += (_ota && _ota->enabled() ? "true" : "false"); json += ",";
  json += "\"otaStarted\":"; json += (_ota && _ota->started() ? "true" : "false"); json += ",";
  json += "\"mqttEnabled\":"; json += (_mqtt && _mqtt->enabled() ? "true" : "false"); json += ",";
  json += "\"mqttConnected\":"; json += (_mqtt && _mqtt->connected() ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "mqttLastError", (_mqtt && _mqtt->lastError()) ? _mqtt->lastError() : "");
  sp7json::appendEscapedField(json, "activePage", g_runtimeStats.activePage);
  appendRuntimeStatsJson(json, g_runtimeStats);
  if (json.endsWith(",")) {
    json.remove(json.length() - 1);
  }
  json += "}";
  return json;
}

String WebManager::homeAssistantStatusJson() const {
  const bool wifiConnected = WiFi.isConnected();
  const String hostname = (_s && _s->hostname[0] != '\0') ? String(_s->hostname) : String("soundpanel7");
  const String mac = WiFi.macAddress();

  String json;
  json.reserve(320);
  json += "{";
  sp7json::appendEscapedField(json, "name", hostname.c_str());
  sp7json::appendEscapedField(json, "mac", mac.c_str());
  sp7json::appendEscapedField(json, "model", "SoundPanel 7");
  sp7json::appendEscapedField(json, "manufacturer", "JJ");
  sp7json::appendEscapedField(json, "version", SOUNDPANEL7_VERSION);
  json += "\"db\":"; json += String(g_webDbInstant, 1); json += ",";
  json += "\"leq\":"; json += String(g_webLeq, 1); json += ",";
  json += "\"peak\":"; json += String(g_webPeak, 1); json += ",";
  json += "\"rssi\":"; json += String(wifiConnected ? WiFi.RSSI() : 0); json += ",";
  sp7json::appendEscapedField(json, "ip", wifiConnected ? WiFi.localIP().toString().c_str() : "");
  json += "\"uptime_s\":"; json += String((millis() - g_bootMs) / 1000);
  json += "}";
  return json;
}

String WebManager::liveMetricsJson() const {
  struct tm ti;
  const bool hasTime = getLocalTime(&ti, 0);
  char tbuf[32] = {0};
  if (hasTime) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);
  const uint32_t sentMs = millis();
  const AudioMetrics& am = g_audio.metrics();

  String json;
  json.reserve(320);
  json += "{";
  json += "\"sent_ms\":"; json += String(sentMs); json += ",";
  appendAudioMetricsJson(json, am);
  appendTimeJson(json, hasTime, tbuf);
  json += "}";
  return json;
}

void WebManager::applyBacklightNow(uint8_t percent) {
#if !SOUNDPANEL7_HAS_SCREEN
  (void)percent;
  return;
#else
  if (!_board) {
    Serial0.println("[WEB] Backlight apply skipped: board=null");
    return;
  }

  auto bl = _board->getBacklight();
  if (!bl) {
    Serial0.println("[WEB] Backlight apply skipped: driver=null");
    return;
  }

  if (percent > 100) percent = 100;

  if (percent == 0) {
    bl->off();
    Serial0.println("[WEB] Backlight OFF");
    return;
  }

  bl->on();
  bl->setBrightness((int)percent);
  Serial0.printf("[WEB] Backlight ON (%u%%)\n", percent);
#endif
}

void WebManager::applyTouchNow(bool enabled) {
#if SOUNDPANEL7_HAS_SCREEN
  lvgl_port_set_touch_enabled(enabled);
  Serial0.printf("[WEB] Touch %s\n", enabled ? "ENABLED" : "DISABLED");
#else
  (void)enabled;
#endif
}

void WebManager::applyTardisPinNow(uint8_t pin, bool enabled, const char* label) {
#if !SOUNDPANEL7_TARDIS_SUPPORTED
  (void)pin;
  (void)enabled;
  (void)label;
  return;
#else
  if (tardisPinConflictsWithAudioPin(pin, _s)) {
    Serial0.printf("[TARDIS] %s skipped on GPIO%u (audio conflict)\n",
                   label ? label : "led",
                   (unsigned)pin);
    return;
  }

  pinMode(pin, OUTPUT);
#if SOUNDPANEL7_TARDIS_LED_ACTIVE_HIGH
  digitalWrite(pin, enabled ? HIGH : LOW);
#else
  digitalWrite(pin, enabled ? LOW : HIGH);
#endif
  Serial0.printf("[TARDIS] %s GPIO%u -> %s\n",
                 label ? label : "led",
                 (unsigned)pin,
                 enabled ? "ON" : "OFF");
#endif
}

void WebManager::ensureTardisInteriorRgbReady() {
#if !SOUNDPANEL7_TARDIS_INTERIOR_RGB_SUPPORTED
  return;
#else
  if (_tardisInteriorRgbReady) return;
  _tardisInteriorRgb.begin();
  _tardisInteriorRgb.setBrightness(SOUNDPANEL7_TARDIS_INTERIOR_RGB_BRIGHTNESS);
  _tardisInteriorRgb.clear();
  _tardisInteriorRgb.show();
  _tardisInteriorRgbReady = true;
#endif
}

void WebManager::applyTardisInteriorRgbColor(uint32_t color) {
#if !SOUNDPANEL7_TARDIS_INTERIOR_RGB_SUPPORTED
  (void)color;
  return;
#else
  ensureTardisInteriorRgbReady();
  color &= 0x00FFFFFFUL;
  if (_tardisInteriorRgbAppliedColor == color) return;

  const uint8_t red = (uint8_t)((color >> 16) & 0xFFU);
  const uint8_t green = (uint8_t)((color >> 8) & 0xFFU);
  const uint8_t blue = (uint8_t)(color & 0xFFU);
  const uint32_t packed = _tardisInteriorRgb.Color(red, green, blue);
  for (uint16_t i = 0; i < SOUNDPANEL7_TARDIS_INTERIOR_RGB_PIXEL_COUNT; i++) {
    _tardisInteriorRgb.setPixelColor(i, packed);
  }
  _tardisInteriorRgb.show();
  _tardisInteriorRgbAppliedColor = color;
  Serial0.printf("[TARDIS] interior RGB GPIO%u -> #%06lX\n",
                 (unsigned)SOUNDPANEL7_TARDIS_INTERIOR_RGB_PIN,
                 (unsigned long)color);
#endif
}

uint32_t WebManager::tardisInteriorRgbColorForCurrentState() const {
  if (!_s) return 0;
  if (_s->tardisInteriorRgbMode == TARDIS_INTERIOR_RGB_MODE_FIXED) {
    return _s->tardisInteriorRgbColor & 0x00FFFFFFUL;
  }
  if (_s->tardisInteriorRgbMode == TARDIS_INTERIOR_RGB_MODE_TAKEOFF
      || _s->tardisInteriorRgbMode == TARDIS_INTERIOR_RGB_MODE_TAKEOFF_SINE) {
    return _tardisInteriorRgbAppliedColor == 0xFFFFFFFFUL ? 0U : _tardisInteriorRgbAppliedColor;
  }

  switch (_alertState) {
    case 2: return tardisColorHex(0xFF, 0x00, 0x00);
    case 1: return tardisColorHex(0xF0, 0xA2, 0x02);
    default: return tardisColorHex(0x00, 0xFF, 0x00);
  }
}

void WebManager::updateTardisAnimationNow(bool force) {
#if !SOUNDPANEL7_TARDIS_INTERIOR_RGB_SUPPORTED
  (void)force;
  return;
#else
  if (!_s) return;

  const bool active = _s->tardisModeEnabled
    && _s->tardisInteriorLedEnabled
    && (_s->tardisInteriorRgbMode == TARDIS_INTERIOR_RGB_MODE_TAKEOFF
        || _s->tardisInteriorRgbMode == TARDIS_INTERIOR_RGB_MODE_TAKEOFF_SINE);
  if (!active) {
    _tardisAnimationMode = 0xFF;
    _tardisAnimationCycleStartMs = 0;
    _tardisAnimationLastFrameMs = 0;
    return;
  }

  const uint32_t now = millis();
  if (!force && _tardisAnimationLastFrameMs != 0 && (uint32_t)(now - _tardisAnimationLastFrameMs) < 24U) {
    return;
  }
  const uint8_t animationMode = _s->tardisInteriorRgbMode;
  if (_tardisAnimationMode != animationMode || _tardisAnimationCycleStartMs == 0) {
    _tardisAnimationMode = animationMode;
    _tardisAnimationCycleStartMs = now;
  }
  _tardisAnimationLastFrameMs = now;

  // Deterministic TARDIS envelopes:
  // - mechanical: closer to the reference, with a firmer ramp and brief plateaus
  // - sine: smoother and more lamp-like while keeping the exact same tempo every cycle
  const bool mechanical = animationMode == TARDIS_INTERIOR_RGB_MODE_TAKEOFF_MECHANICAL;
  const uint32_t cycleMs = mechanical ? 1760U : 1920U;
  const float cycleT = (float)((now - _tardisAnimationCycleStartMs) % cycleMs) / (float)cycleMs;
  const float envelope = mechanical
    ? tardisPulseWithPlateaus01(cycleT, 0.30f, 0.09f, 0.48f, true)
    : tardisPulseWithPlateaus01(cycleT, 0.35f, 0.08f, 0.44f, false);

  const float floor = mechanical ? 0.04f : 0.05f;
  const float ceiling = mechanical ? 0.96f : 0.91f;
  const float electric = clamp01f(floor + ((ceiling - floor) * envelope));
  const float whiteBoost = mechanical
    ? powf(clamp01f((electric - 0.80f) / 0.20f), 1.7f)
    : powf(clamp01f((electric - 0.84f) / 0.16f), 2.1f);

  // Keep a deep blue body, then add just a small icy lift near the peak.
  const uint8_t red = (uint8_t)(2.0f + (8.0f * electric) + (16.0f * whiteBoost));
  const uint8_t green = (uint8_t)(0.0f + (3.0f * electric) + (11.0f * whiteBoost));
  const uint8_t blue = (uint8_t)(14.0f + (168.0f * electric) + (34.0f * whiteBoost));
  applyTardisInteriorRgbColor(tardisColorHex(red, green, blue));
#endif
}

void WebManager::applyTardisNow() {
#if !SOUNDPANEL7_TARDIS_SUPPORTED
  return;
#else
  const bool modeEnabled = _s && _s->tardisModeEnabled;
  const bool interiorEnabled = modeEnabled && _s && _s->tardisInteriorLedEnabled;
  const bool exteriorEnabled = modeEnabled && _s && _s->tardisExteriorLedEnabled;
#if SOUNDPANEL7_TARDIS_INTERIOR_RGB_SUPPORTED
  if (interiorEnabled
      && (_s->tardisInteriorRgbMode == TARDIS_INTERIOR_RGB_MODE_TAKEOFF
          || _s->tardisInteriorRgbMode == TARDIS_INTERIOR_RGB_MODE_TAKEOFF_SINE)) {
    _tardisAnimationMode = 0xFF;
    _tardisInteriorRgbAppliedColor = 0xFFFFFFFFUL;
    updateTardisAnimationNow(true);
  } else {
    applyTardisInteriorRgbColor(interiorEnabled ? tardisInteriorRgbColorForCurrentState() : 0U);
  }
#else
  applyTardisPinNow((uint8_t)SOUNDPANEL7_TARDIS_INTERIOR_LED_PIN, interiorEnabled, "interior");
#endif
  applyTardisPinNow((uint8_t)SOUNDPANEL7_TARDIS_EXTERIOR_LED_PIN, exteriorEnabled, "exterior");
#endif
}

void WebManager::applySettingsRuntimeState() {
  if (!_s) return;

  applyBacklightNow(_s->backlight);
  applyTouchNow(_s->touchEnabled != 0);
  applyTardisNow();

  if (_history) _history->settingsChanged();

  setenv("TZ", _s->tz, 1);
  tzset();
  configTzTime(_s->tz, _s->ntpServer);
  sntp_set_sync_interval(_s->ntpSyncIntervalMs);
  sntp_restart();
  WiFi.setHostname(_s->hostname);
}

String WebManager::historyJson() const {
  return _history ? _history->toJson() : String("[]");
}

void WebManager::handleAuthStatus() {
  const WebSession* session = currentSession(false);

  String json;
  json.reserve(320);
  json += "{";
  appendBoolField(json, "authenticated", session != nullptr);
  appendBoolField(json, "bootstrapRequired", !webUsersConfigured());
  json += "\"userCount\":"; json += String(_store ? _store->webUserCount() : 0); json += ",";
  sp7json::appendEscapedField(json, "currentUser", session ? session->username : "");
  sp7json::appendEscapedField(json, "liveToken", session ? session->liveToken : "", false);
  json += "}";
  replyJson(200, json);
}

void WebManager::handleAuthLogin() {
  if (!_store) {
    replyErrorJson(500, "store missing");
    return;
  }
  if (!webUsersConfigured()) {
    replyErrorJson(403, "bootstrap required");
    return;
  }

  const uint32_t now = millis();
  if (_loginLockUntilMs && (int32_t)(_loginLockUntilMs - now) > 0) {
    replyErrorJson(429, "login temporarily locked");
    return;
  }
  _loginLockUntilMs = 0;

  const String body = _srv.arg("plain");
  String username = sp7json::parseString(body, "username", "", false);
  const String password = sp7json::parseString(body, "password", "", false);
  Serial0.printf("[WEB][AUTH] login attempt user='%s' body=%uB password=%uB\n",
    username.c_str(),
    (unsigned)body.length(),
    (unsigned)password.length());
  if (!normalizeUsername(username)) {
    Serial0.println("[WEB][AUTH] login rejected: bad username");
    replyErrorJson(400, "bad username");
    return;
  }

  WebUserRecord users[WEB_USER_MAX_COUNT];
  _store->loadWebUsers(users);

  const WebUserRecord* match = nullptr;
  for (const WebUserRecord& user : users) {
    if (user.active && strcmp(user.username, username.c_str()) == 0) {
      match = &user;
      break;
    }
  }

  const String computedHash = match ? hashPassword(match->username, password.c_str(), match->passwordSalt) : String("");
  if (!match || !secureEquals(match->passwordHash, computedHash.c_str())) {
    Serial0.printf("[WEB][AUTH] login failed for '%s' (match=%u)\n", username.c_str(), match ? 1U : 0U);
    _loginFailureCount++;
    if (_loginFailureCount >= WEB_LOGIN_MAX_FAILURES) {
      _loginLockUntilMs = now + WEB_LOGIN_LOCK_MS;
      _loginFailureCount = 0;
    }
    delay(120);
    replyErrorJson(401, "invalid credentials");
    return;
  }

  _loginFailureCount = 0;
  _loginLockUntilMs = 0;
  Serial0.printf("[WEB][AUTH] login success for '%s'\n", match->username);
  String liveToken;
  if (!issueSessionForUser(match->username, liveToken)) {
    replyErrorJson(500, "session failed");
    return;
  }

  String json = "{\"ok\":true,\"authenticated\":true,\"currentUser\":\"";
  json += sp7json::escape(match->username);
  json += "\",\"liveToken\":\"";
  json += sp7json::escape(liveToken.c_str());
  json += "\"}";
  replyJson(200, json);
}

void WebManager::handleAuthLogout() {
  invalidateSessionToken(extractCookieValue(SP7_SESSION_COOKIE_NAME).c_str());
  addCommonSecurityHeaders();
  _srv.sendHeader("Set-Cookie", String(SP7_SESSION_COOKIE_NAME) + "=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict");
  replyOkJson();
}

void WebManager::handleAuthBootstrap() {
  if (!_store) {
    replyErrorJson(500, "store missing");
    return;
  }
  if (webUsersConfigured()) {
    replyErrorJson(403, "bootstrap closed");
    return;
  }

  const String body = _srv.arg("plain");
  String username = sp7json::parseString(body, "username", "", false);
  const String password = sp7json::parseString(body, "password", "", false);
  Serial0.printf("[WEB][AUTH] bootstrap attempt user='%s' body=%uB password=%uB\n",
    username.c_str(),
    (unsigned)body.length(),
    (unsigned)password.length());
  if (!normalizeUsername(username)) {
    Serial0.println("[WEB][AUTH] bootstrap rejected: bad username");
    replyErrorJson(400, "bad username");
    return;
  }

  String passwordReason;
  if (!passwordIsStrongEnough(password, &passwordReason)) {
    Serial0.printf("[WEB][AUTH] bootstrap rejected: %s\n", passwordReason.c_str());
    replyErrorJson(400, passwordReason);
    return;
  }

  WebUserRecord user;
  user.active = 1;
  const String salt = randomHex(WEB_PASSWORD_SALT_LENGTH);
  const String hash = hashPassword(username.c_str(), password.c_str(), salt.c_str());
  if (!sp7json::safeCopy(user.username, sizeof(user.username), username)
      || !sp7json::safeCopy(user.passwordSalt, sizeof(user.passwordSalt), salt)
      || !sp7json::safeCopy(user.passwordHash, sizeof(user.passwordHash), hash)) {
    replyErrorJson(500, "bootstrap failed");
    return;
  }

  String err;
  if (!_store->upsertWebUser(user, &err)) {
    Serial0.printf("[WEB][AUTH] bootstrap store failed: %s\n", err.c_str());
    replyErrorJson(400, err);
    return;
  }
  Serial0.printf("[WEB][AUTH] bootstrap created '%s'\n", user.username);

  String liveToken;
  if (!issueSessionForUser(user.username, liveToken)) {
    replyErrorJson(500, "session failed");
    return;
  }
  String json = "{\"ok\":true,\"bootstrap\":true,\"liveToken\":\"";
  json += sp7json::escape(liveToken.c_str());
  json += "\"}";
  replyJson(200, json);
}

void WebManager::handleUsersGet() {
  if (!requireWebAuth()) return;

  const WebSession* session = currentSession(false);
  WebUserRecord users[WEB_USER_MAX_COUNT];
  _store->loadWebUsers(users);

  String json;
  json.reserve(512);
  json += "{";
  sp7json::appendEscapedField(json, "currentUser", session ? session->username : "");
  json += "\"userCount\":"; json += String(_store->webUserCount()); json += ",";
  json += "\"maxUsers\":"; json += String(WEB_USER_MAX_COUNT); json += ",";
  json += "\"users\":[";
  bool first = true;
  for (const WebUserRecord& user : users) {
    if (!user.active) continue;
    if (!first) json += ",";
    first = false;
    json += "{";
    sp7json::appendEscapedField(json, "username", user.username, false);
    json += "}";
  }
  json += "]";
  json += "}";
  replyJson(200, json);
}

void WebManager::handleUsersCreate() {
  if (!requireWebAuth()) return;

  const String body = _srv.arg("plain");
  String username = sp7json::parseString(body, "username", "", false);
  const String password = sp7json::parseString(body, "password", "", false);
  Serial0.printf("[WEB][AUTH] create user attempt user='%s' body=%uB password=%uB\n",
    username.c_str(),
    (unsigned)body.length(),
    (unsigned)password.length());
  if (!normalizeUsername(username)) {
    Serial0.println("[WEB][AUTH] create user rejected: bad username");
    replyErrorJson(400, "bad username");
    return;
  }

  String passwordReason;
  if (!passwordIsStrongEnough(password, &passwordReason)) {
    Serial0.printf("[WEB][AUTH] create user rejected: %s\n", passwordReason.c_str());
    replyErrorJson(400, passwordReason);
    return;
  }

  WebUserRecord existingUsers[WEB_USER_MAX_COUNT];
  _store->loadWebUsers(existingUsers);
  for (const WebUserRecord& existing : existingUsers) {
    if (existing.active && strcmp(existing.username, username.c_str()) == 0) {
      Serial0.printf("[WEB][AUTH] create user rejected: '%s' already exists\n", username.c_str());
      replyErrorJson(409, "user already exists");
      return;
    }
  }

  WebUserRecord user;
  user.active = 1;
  const String salt = randomHex(WEB_PASSWORD_SALT_LENGTH);
  const String hash = hashPassword(username.c_str(), password.c_str(), salt.c_str());
  if (!sp7json::safeCopy(user.username, sizeof(user.username), username)
      || !sp7json::safeCopy(user.passwordSalt, sizeof(user.passwordSalt), salt)
      || !sp7json::safeCopy(user.passwordHash, sizeof(user.passwordHash), hash)) {
    replyErrorJson(500, "user create failed");
    return;
  }

  String err;
  if (!_store->upsertWebUser(user, &err)) {
    Serial0.printf("[WEB][AUTH] create user store failed: %s\n", err.c_str());
    replyErrorJson(400, err);
    return;
  }

  Serial0.printf("[WEB][AUTH] user created '%s'\n", user.username);
  replyOkJson();
}

void WebManager::handleUsersPassword() {
  if (!requireWebAuth()) return;

  const String body = _srv.arg("plain");
  String username = sp7json::parseString(body, "username", "", false);
  const String password = sp7json::parseString(body, "password", "", false);
  Serial0.printf("[WEB][AUTH] password update attempt user='%s' body=%uB password=%uB\n",
    username.c_str(),
    (unsigned)body.length(),
    (unsigned)password.length());
  if (!normalizeUsername(username)) {
    Serial0.println("[WEB][AUTH] password update rejected: bad username");
    replyErrorJson(400, "bad username");
    return;
  }

  String passwordReason;
  if (!passwordIsStrongEnough(password, &passwordReason)) {
    Serial0.printf("[WEB][AUTH] password update rejected: %s\n", passwordReason.c_str());
    replyErrorJson(400, passwordReason);
    return;
  }

  WebUserRecord users[WEB_USER_MAX_COUNT];
  _store->loadWebUsers(users);

  int index = -1;
  for (uint8_t i = 0; i < WEB_USER_MAX_COUNT; i++) {
    if (users[i].active && strcmp(users[i].username, username.c_str()) == 0) {
      index = i;
      break;
    }
  }
  if (index < 0) {
    Serial0.printf("[WEB][AUTH] password update failed: user '%s' not found\n", username.c_str());
    replyErrorJson(404, "user not found");
    return;
  }

  const String salt = randomHex(WEB_PASSWORD_SALT_LENGTH);
  const String hash = hashPassword(username.c_str(), password.c_str(), salt.c_str());
  if (!sp7json::safeCopy(users[index].passwordSalt, sizeof(users[index].passwordSalt), salt)
      || !sp7json::safeCopy(users[index].passwordHash, sizeof(users[index].passwordHash), hash)) {
    replyErrorJson(500, "password update failed");
    return;
  }

  String err;
  if (!_store->upsertWebUser(users[index], &err)) {
    Serial0.printf("[WEB][AUTH] password update store failed: %s\n", err.c_str());
    replyErrorJson(400, err);
    return;
  }

  Serial0.printf("[WEB][AUTH] password updated for '%s'\n", username.c_str());
  invalidateSessionsForUser(username.c_str());
  replyOkJson();
}

void WebManager::handleUsersDelete() {
  if (!requireWebAuth()) return;

  const String body = _srv.arg("plain");
  String username = sp7json::parseString(body, "username", "", false);
  if (!normalizeUsername(username)) {
    replyErrorJson(400, "bad username");
    return;
  }

  String err;
  if (!_store->deleteWebUser(username.c_str(), &err)) {
    replyErrorJson(400, err);
    return;
  }

  invalidateSessionsForUser(username.c_str());
  replyOkJson();
}

void WebManager::handleHomeAssistantGet() {
  if (!requireWebAuth()) return;
  if (!requireSettingsJson()) return;

  String json;
  json.reserve(160);
  json += "{";
  json += "\"tokenConfigured\":"; json += (homeAssistantTokenConfigured() ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "statusPath", "/api/ha/status");
  sp7json::appendEscapedField(json, "authScheme", "Bearer", false);
  json += "}";
  replyJson(200, json);
}

void WebManager::handleHomeAssistantSave() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  const String body = _srv.arg("plain");
  const bool clear = sp7json::parseBool(body, "clear", false);
  const bool generate = sp7json::parseBool(body, "generate", false);
  const bool keepToken = sp7json::parseBool(body, "keepToken", false);

  String token;
  if (clear) token = "";
  else if (generate) token = randomHex(HOME_ASSISTANT_TOKEN_MAX_LENGTH);
  else {
    token = sp7json::parseString(body, "token", keepToken ? String(_s->homeAssistantToken) : String(""), true);
    token.trim();
    if (!homeAssistantTokenIsValid(token)) {
      replyErrorJson(400, "bad home assistant token");
      return;
    }
  }

  if (!sp7json::safeCopy(_s->homeAssistantToken, sizeof(_s->homeAssistantToken), token)) {
    replyErrorJson(400, "home assistant token too long");
    return;
  }

  _store->save(*_s);

  String json;
  json.reserve(128);
  json += "{";
  json += "\"ok\":true,";
  json += "\"tokenConfigured\":"; json += (homeAssistantTokenConfigured() ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "statusPath", "/api/ha/status");
  sp7json::appendEscapedField(json, "authScheme", "Bearer", false);
  json += "}";
  replyJson(200, json);
}

void WebManager::handleHomeAssistantStatus() {
  if (!requireHomeAssistantToken()) return;
  replyJson(200, homeAssistantStatusJson());
}

void WebManager::handleStatus() {
  if (!requireWebAuth()) return;
  replyJson(200, statusJson());
}

void WebManager::handleSystemSummary() {
  if (!requireWebAuth()) return;
  replyJson(200, systemSummaryJson());
}

void WebManager::handlePinSave() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

#if !SOUNDPANEL7_HAS_SCREEN
  replyErrorJson(400, "pin unsupported without screen");
  return;
#endif

  String body = _srv.arg("plain");
  String pin = sp7json::parseString(body, "pin", "", true);
  pin.trim();

  if (pin.length() > 0 && !pinCodeIsValid(pin.c_str())) {
    replyErrorJson(400, "bad pin");
    return;
  }
  if (!encodePinCode(pin.c_str(), _s->dashboardPin, sizeof(_s->dashboardPin))) {
    replyErrorJson(400, "pin encode failed");
    return;
  }

  _store->save(*_s);
  pushLiveMetrics(true);

  String json = "{\"ok\":true,\"pinConfigured\":";
  json += pinConfigured() ? "true" : "false";
  json += "}";
  replyJson(200, json);
}

void WebManager::handleUiSave() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsText()) return;

  String body = _srv.arg("plain");
  const bool dashboardPageRequested = sp7json::findValueStart(body, "dashboardPage") >= 0;
  const bool dashboardFullscreenMaskRequested = sp7json::findValueStart(body, "dashboardFullscreenMask") >= 0;

  int bl = sp7json::parseInt(body, "backlight", (int)_s->backlight);
  int g  = sp7json::parseInt(body, "greenMax",  (int)_s->th.greenMax);
  int o  = sp7json::parseInt(body, "orangeMax", (int)_s->th.orangeMax);
  int hm = sp7json::parseInt(body, "historyMinutes", (int)_s->historyMinutes);
  int audioSource = sp7json::parseInt(body, "audioSource", (int)_s->audioSource);
  int arm = sp7json::parseInt(body, "audioResponseMode", (int)_s->audioResponseMode);
  const bool touchEnabled = sp7json::parseBool(body, "touchEnabled", _s->touchEnabled != 0);
  const bool tardisModeEnabled = sp7json::parseBool(body, "tardisModeEnabled", _s->tardisModeEnabled != 0);
  const bool tardisInteriorLedEnabled = sp7json::parseBool(body, "tardisInteriorLedEnabled", _s->tardisInteriorLedEnabled != 0);
  const bool tardisExteriorLedEnabled = sp7json::parseBool(body, "tardisExteriorLedEnabled", _s->tardisExteriorLedEnabled != 0);
  int tardisInteriorRgbMode = sp7json::parseInt(body, "tardisInteriorRgbMode", (int)_s->tardisInteriorRgbMode);
  String tardisInteriorRgbColorText = sp7json::parseString(body, "tardisInteriorRgbColorHex", "");
  int dashboardPage = sp7json::parseInt(body, "dashboardPage", (int)_s->dashboardPage);
  int dashboardFullscreenMask = sp7json::parseInt(body, "dashboardFullscreenMask", (int)_s->dashboardFullscreenMask);
  int whs = sp7json::parseInt(body, "warningHoldSec", (int)(_s->orangeAlertHoldMs / MS_PER_SECOND));
  int chs = sp7json::parseInt(body, "criticalHoldSec", (int)(_s->redAlertHoldMs / MS_PER_SECOND));
  int calCount = sp7json::parseInt(body, "calibrationPointCount", (int)_s->calibrationPointCount);
  int ccs = sp7json::parseInt(body, "calibrationCaptureSec", (int)(_s->calibrationCaptureMs / MS_PER_SECOND));

  if (bl < 0) bl = 0;
  if (bl > 100) bl = 100;
  if (g < 0) g = 0;
  if (g > 100) g = 100;
  if (o < g) o = g;
  if (o > 100) o = 100;
  if (hm < 1) hm = 1;
  if (hm > 60) hm = 60;
  if (audioSource < 0) audioSource = 0;
  if (audioSource > 3) audioSource = 3;
  if (arm < 0) arm = 0;
  if (arm > 1) arm = 1;
  if (tardisInteriorRgbMode < (int)TARDIS_INTERIOR_RGB_MODE_ALERT) tardisInteriorRgbMode = (int)TARDIS_INTERIOR_RGB_MODE_ALERT;
  if (tardisInteriorRgbMode > (int)TARDIS_INTERIOR_RGB_MODE_MAX) tardisInteriorRgbMode = (int)TARDIS_INTERIOR_RGB_MODE_MAX;
  dashboardPage = (int)normalizedDashboardPage((uint8_t)dashboardPage);
  dashboardFullscreenMask = (int)normalizedDashboardFullscreenMask((uint8_t)dashboardFullscreenMask);
  if (whs < 0) whs = 0;
  if (whs > 60) whs = 60;
  if (chs < 0) chs = 0;
  if (chs > 60) chs = 60;
  calCount = normalizedCalibrationPointCount((uint8_t)calCount);
  if (ccs < 1) ccs = 1;
  if (ccs > 30) ccs = 30;

  const uint8_t previousAudioSource = _s->audioSource;
  const uint8_t nextAudioSource = (uint8_t)audioSource;
  if (nextAudioSource != previousAudioSource) {
    SettingsStore::switchCalibrationProfile(*_s, nextAudioSource);
  } else {
    _s->audioSource = nextAudioSource;
  }

  if (SOUNDPANEL7_HAS_SCREEN) {
    _s->backlight = (uint8_t)bl;
  }
  _s->th.greenMax = (uint8_t)g;
  _s->th.orangeMax = (uint8_t)o;
  _s->historyMinutes = (uint8_t)hm;
  _s->audioResponseMode = (uint8_t)arm;
  if (SOUNDPANEL7_HAS_SCREEN) {
    _s->touchEnabled = touchEnabled ? 1 : 0;
    _s->dashboardPage = (uint8_t)dashboardPage;
    _s->dashboardFullscreenMask = (uint8_t)dashboardFullscreenMask;
  }
  if (SOUNDPANEL7_TARDIS_SUPPORTED) {
    _s->tardisModeEnabled = tardisModeEnabled ? 1 : 0;
    _s->tardisInteriorLedEnabled = tardisInteriorLedEnabled ? 1 : 0;
    _s->tardisExteriorLedEnabled = tardisExteriorLedEnabled ? 1 : 0;
    _s->tardisInteriorRgbMode = (uint8_t)tardisInteriorRgbMode;
    if (tardisInteriorRgbColorText.length() == 7 && tardisInteriorRgbColorText[0] == '#') {
      _s->tardisInteriorRgbColor = (uint32_t)strtoul(tardisInteriorRgbColorText.c_str() + 1, nullptr, 16) & 0x00FFFFFFUL;
    }
  }
  _s->orangeAlertHoldMs = (uint32_t)whs * MS_PER_SECOND;
  _s->redAlertHoldMs = (uint32_t)chs * MS_PER_SECOND;
  if (AudioEngine::sourceSupportsCalibration(_s->audioSource)) {
    _s->calibrationPointCount = (uint8_t)calCount;
    _s->calibrationCaptureMs = (uint32_t)ccs * MS_PER_SECOND;
  }
  if (_history) _history->settingsChanged();
  if (SOUNDPANEL7_HAS_SCREEN) {
    if (dashboardPageRequested && _ui) _ui->requestDashboardPage(_s->dashboardPage);
    else if (dashboardFullscreenMaskRequested && _ui) _ui->refreshDashboardLayout();
  }

  SettingsStore::syncActiveCalibrationProfile(*_s);
  _store->save(*_s);
  applyBacklightNow(_s->backlight);
  applyTouchNow(_s->touchEnabled != 0);
  applyTardisNow();

  Serial0.printf("[WEB] UI saved: backlight=%d touch=%d tardis=%d int=%d ext=%d intRgbMode=%d intRgbColor=#%06lX green=%d orange=%d hist=%d page=%d fsm=%d mic=%s mode=%s warn=%ds crit=%ds cal=%ds\n",
                 bl, touchEnabled ? 1 : 0,
                 tardisModeEnabled ? 1 : 0, tardisInteriorLedEnabled ? 1 : 0, tardisExteriorLedEnabled ? 1 : 0,
                 tardisInteriorRgbMode, (unsigned long)(_s->tardisInteriorRgbColor & 0x00FFFFFFUL),
                 g, o, hm, dashboardPage, dashboardFullscreenMask,
                 AudioEngine::sourceLabel(_s->audioSource),
                 AudioEngine::responseModeLabel(_s->audioResponseMode), whs, chs, ccs);
  replyOkJson(true);
}

void WebManager::handleLiveGet() {
  if (!requireWebAuth()) return;
  if (!requireSettingsJson()) return;

  String json;
  json.reserve(64);
  json += "{";
  json += "\"enabled\":";
  json += (_s->liveEnabled ? "true" : "false");
  json += "}";
  replyJson(200, json);
}

void WebManager::handleLiveSave() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  String body = _srv.arg("plain");
  const bool enabled = sp7json::parseBool(body, "enabled", _s->liveEnabled != 0);
  _s->liveEnabled = enabled ? LIVE_ENABLED : LIVE_DISABLED;
  _store->save(*_s);
  pushLiveMetrics(true);

  String json;
  json.reserve(80);
  json += "{\"ok\":true,\"enabled\":";
  json += (_s->liveEnabled ? "true" : "false");
  json += "}";
  replyJson(200, json);
}

void WebManager::handleCalPoint() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  String body = _srv.arg("plain");
  int index = sp7json::parseInt(body, "index", -1);
  float refDb = sp7json::parseFloat(body, "refDb", -1.0f);

  if (index < 0 || index >= _s->calibrationPointCount) {
    replyErrorJson(400, "bad index");
    return;
  }
  if (refDb <= 0.0f || refDb > 140.0f) {
    replyErrorJson(400, "bad refDb");
    return;
  }
  if (!AudioEngine::sourceSupportsCalibration(_s->audioSource)) {
    replyErrorJson(400, "source not calibratable");
    return;
  }

  if (!g_audio.captureCalibrationPoint(*_s, (uint8_t)index, refDb)) {
    replyErrorJson(500, "capture failed");
    return;
  }

  SettingsStore::syncActiveCalibrationProfile(*_s);
  _store->save(*_s);
  Serial0.printf("[WEB] CAL point %d/%d saved @ %.1f dB\n", index + 1, _s->calibrationPointCount, refDb);
  replyOkJson();
}

void WebManager::handleCalClear() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;
  if (!AudioEngine::sourceSupportsCalibration(_s->audioSource)) {
    replyErrorJson(400, "source not calibratable");
    return;
  }

  g_audio.clearCalibration(*_s);
  SettingsStore::syncActiveCalibrationProfile(*_s);
  _store->save(*_s);
  Serial0.println("[WEB] CAL cleared");
  replyOkJson();
}

void WebManager::handleCalMode() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;
  if (!AudioEngine::sourceSupportsCalibration(_s->audioSource)) {
    replyErrorJson(400, "source not calibratable");
    return;
  }

  String body = _srv.arg("plain");
  int pointCount = sp7json::parseInt(body, "calibrationPointCount", (int)_s->calibrationPointCount);
  pointCount = normalizedCalibrationPointCount((uint8_t)pointCount);

  if ((uint8_t)pointCount != _s->calibrationPointCount) {
    _s->calibrationPointCount = (uint8_t)pointCount;
    g_audio.clearCalibration(*_s);
    SettingsStore::syncActiveCalibrationProfile(*_s);
    _store->save(*_s);
  }

  Serial0.printf("[WEB] CAL mode set to %d points\n", pointCount);
  replyOkJson();
}

void WebManager::handleTimeGet() {
  if (!requireWebAuth()) return;
  if (!requireSettingsJson()) return;

  String json;
  json.reserve(256);
  json += "{";
  sp7json::appendEscapedField(json, "tz", _s->tz);
  sp7json::appendEscapedField(json, "ntpServer", _s->ntpServer);
  json += "\"ntpSyncIntervalMs\":"; json += String(_s->ntpSyncIntervalMs); json += ",";
  json += "\"ntpSyncMinutes\":"; json += String(_s->ntpSyncIntervalMs / MS_PER_MINUTE); json += ",";
  sp7json::appendEscapedField(json, "hostname", _s->hostname, false);
  json += "}";

  replyJson(200, json);
}

void WebManager::handleWifiGet() {
  if (!requireWebAuth()) return;
  if (!requireSettingsJson()) return;

  const String currentSsid = _net ? _net->currentSsid() : String("");
  String json;
  json.reserve(640);
  json += "{";
  json += "\"connected\":"; json += (_net && _net->isWifiConnected() ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "currentSsid", currentSsid.c_str());
  json += "\"networks\":[";
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    if (i) json += ",";
    json += "{";
    json += "\"slot\":"; json += String(i + 1); json += ",";
    sp7json::appendEscapedField(json, "ssid", _s->wifiCredentials[i].ssid);
    json += "\"passwordConfigured\":";
    json += strlen(_s->wifiCredentials[i].password) ? "true" : "false";
    json += "}";
  }
  json += "]";
  json += "}";

  replyJson(200, json);
}

void WebManager::handleWifiSave() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  String body = _srv.arg("plain");
  SettingsV1 next = *_s;

  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    const String slot = String(i + 1);
    const String ssidKey = String("wifi") + slot + "Ssid";
    const String passwordKey = String("wifi") + slot + "Password";
    const String keepPasswordKey = String("wifi") + slot + "KeepPassword";

    String ssid = sp7json::parseString(body, ssidKey.c_str(), String(next.wifiCredentials[i].ssid), false);
    String password = sp7json::parseString(body, passwordKey.c_str(), "", true);
    const bool keepPassword = sp7json::parseBool(body, keepPasswordKey.c_str(), false);

    ssid.trim();
    if (!ssid.length()) {
      next.wifiCredentials[i].ssid[0] = '\0';
      next.wifiCredentials[i].password[0] = '\0';
      continue;
    }

    if (keepPassword && strcmp(ssid.c_str(), _s->wifiCredentials[i].ssid) == 0) {
      password = String(_s->wifiCredentials[i].password);
    }

    if (!sp7json::safeCopy(next.wifiCredentials[i].ssid, sizeof(next.wifiCredentials[i].ssid), ssid)) {
      replyErrorJson(400, String("wifi") + slot + " ssid too long");
      return;
    }
    if (!sp7json::safeCopy(next.wifiCredentials[i].password, sizeof(next.wifiCredentials[i].password), password)) {
      replyErrorJson(400, String("wifi") + slot + " password too long");
      return;
    }
  }

  *_s = next;
  _store->save(*_s);
  if (_net) _net->reloadWifiConfig();

  Serial0.printf("[WEB] WiFi saved: %u slot(s)\n", (unsigned)(_net ? _net->wifiCredentialCount() : 0));
  replyOkJson(true);
}

void WebManager::handleTimeSave() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsText()) return;

  String body = _srv.arg("plain");

  String tz  = sp7json::parseString(body, "tz", String(_s->tz), false);
  String ntp = sp7json::parseString(body, "ntpServer", String(_s->ntpServer), false);
  String hn  = sp7json::parseString(body, "hostname", String(_s->hostname), false);
  int ntpSyncMinutes = sp7json::parseInt(body, "ntpSyncMinutes", (int)(_s->ntpSyncIntervalMs / MS_PER_MINUTE));

  tz.trim();
  ntp.trim();
  hn.trim();

  if (tz.length() < 3) {
    replyErrorJson(400, "bad tz", true);
    return;
  }
  if (ntp.length() < 3) {
    replyErrorJson(400, "bad ntpServer", true);
    return;
  }
  if (hn.length() < 1) {
    replyErrorJson(400, "bad hostname", true);
    return;
  }
  if (ntpSyncMinutes < 1 || ntpSyncMinutes > 1440) {
    replyErrorJson(400, "bad ntpSyncMinutes", true);
    return;
  }

  if (!sp7json::safeCopy(_s->tz, sizeof(_s->tz), tz)) {
    replyErrorJson(400, "tz too long", true);
    return;
  }
  if (!sp7json::safeCopy(_s->ntpServer, sizeof(_s->ntpServer), ntp)) {
    replyErrorJson(400, "ntpServer too long", true);
    return;
  }
  if (!sp7json::safeCopy(_s->hostname, sizeof(_s->hostname), hn)) {
    replyErrorJson(400, "hostname too long", true);
    return;
  }
  _s->ntpSyncIntervalMs = (uint32_t)ntpSyncMinutes * MS_PER_MINUTE;

  _store->save(*_s);
  applySettingsRuntimeState();

  Serial0.printf("[WEB] TIME saved: tz='%s' ntp='%s' interval=%lu ms hostname='%s'\n",
                 _s->tz, _s->ntpServer, (unsigned long)_s->ntpSyncIntervalMs, _s->hostname);

  replyOkJson(true);
}

void WebManager::handleConfigExport() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  String err;
  const String json = _store->exportJson(*_s, SettingsStore::EXPORT_SECRETS_OMIT, &err);
  if (!json.length()) {
    replyErrorJson(500, err.length() ? err : "config export failed");
    return;
  }
  replyJson(200, json);
}

void WebManager::handleConfigExportFull() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  String err;
  const String json = _store->exportJson(*_s, SettingsStore::EXPORT_SECRETS_CLEAR, &err);
  if (!json.length()) {
    replyErrorJson(500, err.length() ? err : "config export failed");
    return;
  }
  replyJson(200, json);
}

void WebManager::handleConfigImport() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  String err;
  String body = _srv.arg("plain");
  if (!_store->importJson(*_s, body, &err)) {
    replyErrorJson(400, err);
    return;
  }

  _store->save(*_s);
  applySettingsRuntimeState();
  pushLiveMetrics(true);
  replyOkJsonRebootRecommended();
}

void WebManager::handleConfigBackup() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  if (!_store->saveBackup(*_s)) {
    replyErrorJson(500, "backup failed");
    return;
  }

  replyOkJson();
}

void WebManager::handleConfigRestore() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  String err;
  if (!_store->restoreBackup(*_s, &err)) {
    replyErrorJson(400, err);
    return;
  }

  _store->save(*_s);
  applySettingsRuntimeState();
  pushLiveMetrics(true);
  replyOkJsonRebootRecommended();
}

void WebManager::handleConfigResetPartial() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  String body = _srv.arg("plain");
  String scope = sp7json::parseString(body, "scope", "", false);
  scope.trim();
  scope.toLowerCase();

  String err;
  if (!_store->resetSection(*_s, scope, &err)) {
    replyErrorJson(400, err);
    return;
  }

  if (scope == "security") {
    _store->clearWebUsers();
    clearAllSessions();
  }

  _store->save(*_s);
  applySettingsRuntimeState();
  if (_net) _net->reloadWifiConfig();
  pushLiveMetrics(true);
  replyOkJsonRebootRecommended();
}

void WebManager::handleReboot() {
  if (!requireWebAuth()) return;
  replyOkJson(true);
  delay(150);
  ESP.restart();
}

void WebManager::handleShutdown() {
  if (!requireWebAuth()) return;
  replyOkJson(true);
  delay(150);

  applyBacklightNow(0);
  applyTardisPinNow((uint8_t)SOUNDPANEL7_TARDIS_INTERIOR_LED_PIN, false, "interior");
  applyTardisPinNow((uint8_t)SOUNDPANEL7_TARDIS_EXTERIOR_LED_PIN, false, "exterior");
  delay(80);

  gpio_hold_dis(GPIO_NUM_0);
  rtc_gpio_pullup_dis(GPIO_NUM_0);
  rtc_gpio_pulldown_en(GPIO_NUM_0);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

  Serial0.println("[PWR] Deep sleep - wake on BOOT(GPIO0)");
  Serial0.flush();
  delay(50);

  esp_deep_sleep_start();
}

void WebManager::handleFactoryReset() {
  if (!requireWebAuth()) return;
  if (_store) _store->factoryReset();
  if (_store) _store->clearWebUsers();
  clearAllSessions();
  replyOkJson(true);
  delay(150);
  ESP.restart();
}

void WebManager::handleOtaGet() {
  if (!requireWebAuth()) return;
  if (!requireSettingsJson()) return;

  String json;
  json.reserve(256);
  json += "{";
  json += "\"enabled\":"; json += (_s->otaEnabled ? "true" : "false"); json += ",";
  json += "\"port\":"; json += String(_s->otaPort); json += ",";
  sp7json::appendEscapedField(json, "hostname", _s->otaHostname);
  json += "\"passwordConfigured\":"; json += (strlen(_s->otaPassword) ? "true" : "false");
  json += "}";

  replyJson(200, json);
}

void WebManager::handleOtaSave() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  String body = _srv.arg("plain");

  int enabled = sp7json::parseInt(body, "enabled", _s->otaEnabled ? 1 : 0);
  int port = sp7json::parseInt(body, "port", (int)_s->otaPort);
  String hn = sp7json::parseString(body, "hostname", String(_s->otaHostname), false);
  String pwd = sp7json::parseString(body, "password", String(_s->otaPassword), false);

  hn.trim();
  pwd.trim();

  if (port < 1 || port > 65535) {
    replyErrorJson(400, "bad port");
    return;
  }

  if (!sp7json::safeCopy(_s->otaHostname, sizeof(_s->otaHostname), hn)) {
    replyErrorJson(400, "hostname too long");
    return;
  }

  if (!sp7json::safeCopy(_s->otaPassword, sizeof(_s->otaPassword), pwd)) {
    replyErrorJson(400, "password too long");
    return;
  }

  _s->otaEnabled = enabled ? 1 : 0;
  _s->otaPort = (uint16_t)port;

  _store->save(*_s);

  Serial0.printf("[WEB] OTA saved: enabled=%u port=%u hostname=%s pwd=%s\n",
                 (unsigned)_s->otaEnabled,
                 (unsigned)_s->otaPort,
                 _s->otaHostname,
                 strlen(_s->otaPassword) ? "<set>" : "<empty>");

  replyOkJsonRebootRequired();
}

void WebManager::handleReleaseGet() {
  if (!requireWebAuth()) return;

  String json;
  json.reserve(960);
  json += "{";
  sp7json::appendEscapedField(json, "manifestUrl",
                              (_releaseUpdate && _releaseUpdate->manifestUrl()) ? _releaseUpdate->manifestUrl() : "");
  sp7json::appendEscapedField(json, "currentVersion",
                              (_releaseUpdate && _releaseUpdate->currentVersion()) ? _releaseUpdate->currentVersion() : "");
  json += "\"checked\":";
  json += (_releaseUpdate && _releaseUpdate->hasChecked() ? "true" : "false");
  json += ",";
  json += "\"ok\":";
  json += (_releaseUpdate && _releaseUpdate->lastCheckOk() ? "true" : "false");
  json += ",";
  json += "\"busy\":";
  json += (_releaseUpdate && _releaseUpdate->busy() ? "true" : "false");
  json += ",";
  json += "\"available\":";
  json += (_releaseUpdate && _releaseUpdate->updateAvailable() ? "true" : "false");
  json += ",";
  json += "\"httpCode\":";
  json += String(_releaseUpdate ? _releaseUpdate->lastHttpCode() : 0);
  json += ",";
  json += "\"checkedAt\":";
  json += String(_releaseUpdate ? _releaseUpdate->lastCheckUnix() : 0U);
  json += ",";
  sp7json::appendEscapedField(json, "latestVersion",
                              (_releaseUpdate && _releaseUpdate->latestVersion()[0]) ? _releaseUpdate->latestVersion() : "");
  sp7json::appendEscapedField(json, "publishedAt",
                              (_releaseUpdate && _releaseUpdate->publishedAt()[0]) ? _releaseUpdate->publishedAt() : "");
  sp7json::appendEscapedField(json, "releaseUrl",
                              (_releaseUpdate && _releaseUpdate->releaseUrl()[0]) ? _releaseUpdate->releaseUrl() : "");
  sp7json::appendEscapedField(json, "otaUrl",
                              (_releaseUpdate && _releaseUpdate->otaUrl()[0]) ? _releaseUpdate->otaUrl() : "");
  sp7json::appendEscapedField(json, "otaSha256",
                              (_releaseUpdate && _releaseUpdate->otaSha256()[0]) ? _releaseUpdate->otaSha256() : "");
  json += "\"installing\":";
  json += (_releaseUpdate && _releaseUpdate->installInProgress() ? "true" : "false");
  json += ",";
  json += "\"installFinished\":";
  json += (_releaseUpdate && _releaseUpdate->installFinished() ? "true" : "false");
  json += ",";
  json += "\"installSucceeded\":";
  json += (_releaseUpdate && _releaseUpdate->installSucceeded() ? "true" : "false");
  json += ",";
  json += "\"installStartedAt\":";
  json += String(_releaseUpdate ? _releaseUpdate->installStartedUnix() : 0U);
  json += ",";
  json += "\"installFinishedAt\":";
  json += String(_releaseUpdate ? _releaseUpdate->installFinishedUnix() : 0U);
  json += ",";
  json += "\"installTotalBytes\":";
  json += String(_releaseUpdate ? _releaseUpdate->installTotalBytes() : 0U);
  json += ",";
  json += "\"installWrittenBytes\":";
  json += String(_releaseUpdate ? _releaseUpdate->installWrittenBytes() : 0U);
  json += ",";
  json += "\"installProgressPct\":";
  json += String(_releaseUpdate ? _releaseUpdate->installProgressPct() : 0U);
  json += ",";
  sp7json::appendEscapedField(json, "installStatus",
                              (_releaseUpdate && _releaseUpdate->installStatus()[0]) ? _releaseUpdate->installStatus() : "");
  sp7json::appendEscapedField(json, "installError",
                              (_releaseUpdate && _releaseUpdate->installError()[0]) ? _releaseUpdate->installError() : "");
  sp7json::appendEscapedField(json, "error",
                              (_releaseUpdate && _releaseUpdate->lastError()[0]) ? _releaseUpdate->lastError() : "",
                              false);
  json += "}";

  replyJson(200, json);
}

void WebManager::handleReleaseCheck() {
  if (!requireWebAuth()) return;
  if (!_releaseUpdate) {
    replyErrorJson(500, "release manager unavailable");
    return;
  }
  if (_releaseUpdate->busy()) {
    replyErrorJson(409, "release check already running");
    return;
  }
  const bool ok = _releaseUpdate->checkNow();
  handleReleaseGet();
  if (!ok) {
    Serial0.printf("[WEB] Release check failed: %s\n", _releaseUpdate->lastError());
  }
}

void WebManager::handleReleaseInstall() {
  if (!requireWebAuth()) return;
  if (!_releaseUpdate) {
    replyErrorJson(500, "release manager unavailable");
    return;
  }
  const String body = _srv.arg("plain");
  const bool force = sp7json::parseBool(body, "force", false);
  if (!_releaseUpdate->startInstall(force)) {
    const char* error = _releaseUpdate->installError();
    replyErrorJson(409, (error && error[0]) ? String(error) : String("install start failed"));
    return;
  }
  handleReleaseGet();
}

void WebManager::handleMqttGet() {
  if (!requireWebAuth()) return;
  if (!requireSettingsJson()) return;

  String json;
  json.reserve(512);
  json += "{";
  json += "\"enabled\":"; json += (_s->mqttEnabled ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "host", _s->mqttHost);
  json += "\"port\":"; json += String(_s->mqttPort); json += ",";
  sp7json::appendEscapedField(json, "username", _s->mqttUsername);
  sp7json::appendEscapedField(json, "clientId", _s->mqttClientId);
  sp7json::appendEscapedField(json, "baseTopic", _s->mqttBaseTopic);
  json += "\"publishPeriodMs\":"; json += String(_s->mqttPublishPeriodMs); json += ",";
  json += "\"retain\":"; json += (_s->mqttRetain ? "true" : "false"); json += ",";
  json += "\"passwordConfigured\":"; json += (strlen(_s->mqttPassword) ? "true" : "false");
  json += "}";

  replyJson(200, json);
}

void WebManager::handleMqttSave() {
  Serial0.println("[WEB] /api/mqtt POST received");

  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) {
    Serial0.println("[WEB] MQTT save failed: store/settings missing");
    return;
  }

  String body = _srv.arg("plain");

  int enabled = sp7json::parseInt(body, "enabled", _s->mqttEnabled ? 1 : 0);
  int port = sp7json::parseInt(body, "port", (int)_s->mqttPort);
  int publishMs = sp7json::parseInt(body, "publishPeriodMs", (int)_s->mqttPublishPeriodMs);
  int retain = sp7json::parseInt(body, "retain", _s->mqttRetain ? 1 : 0);

  String host = sp7json::parseString(body, "host", String(_s->mqttHost), false);
  String username = sp7json::parseString(body, "username", String(_s->mqttUsername), false);
  String password = sp7json::parseString(body, "password", "", true);
  String clientId = sp7json::parseString(body, "clientId", String(_s->mqttClientId), false);
  String baseTopic = sp7json::parseString(body, "baseTopic", String(_s->mqttBaseTopic), false);
  const bool keepPassword = sp7json::parseBool(body, "keepPassword", false);

  host.trim();
  username.trim();
  password.trim();
  clientId.trim();
  baseTopic.trim();

  if (keepPassword && !password.length()) password = String(_s->mqttPassword);

    if (enabled) {
    if (host.length() < 1) {
      Serial0.println("[WEB] MQTT save error: bad host");
      replyErrorJson(400, "bad host");
      return;
    }
    if (port < 1 || port > 65535) {
      Serial0.println("[WEB] MQTT save error: bad port");
      replyErrorJson(400, "bad port");
      return;
    }
    if (clientId.length() < 1) {
      Serial0.println("[WEB] MQTT save error: bad clientId");
      replyErrorJson(400, "bad clientId");
      return;
    }
    if (baseTopic.length() < 1) {
      Serial0.println("[WEB] MQTT save error: bad baseTopic");
      replyErrorJson(400, "bad baseTopic");
      return;
    }
  }

  if (publishMs < MIN_MQTT_PUBLISH_PERIOD_MS) publishMs = MIN_MQTT_PUBLISH_PERIOD_MS;
  if (publishMs > MAX_MQTT_PUBLISH_PERIOD_MS) publishMs = MAX_MQTT_PUBLISH_PERIOD_MS;

  if (!sp7json::safeCopy(_s->mqttHost, sizeof(_s->mqttHost), host)) {
    Serial0.println("[WEB] MQTT save error: host too long");
    replyErrorJson(400, "host too long");
    return;
  }
  if (!sp7json::safeCopy(_s->mqttUsername, sizeof(_s->mqttUsername), username)) {
    Serial0.println("[WEB] MQTT save error: username too long");
    replyErrorJson(400, "username too long");
    return;
  }
  if (!sp7json::safeCopy(_s->mqttPassword, sizeof(_s->mqttPassword), password)) {
    Serial0.println("[WEB] MQTT save error: password too long");
    replyErrorJson(400, "password too long");
    return;
  }
  if (!sp7json::safeCopy(_s->mqttClientId, sizeof(_s->mqttClientId), clientId)) {
    Serial0.println("[WEB] MQTT save error: clientId too long");
    replyErrorJson(400, "clientId too long");
    return;
  }
  if (!sp7json::safeCopy(_s->mqttBaseTopic, sizeof(_s->mqttBaseTopic), baseTopic)) {
    Serial0.println("[WEB] MQTT save error: baseTopic too long");
    replyErrorJson(400, "baseTopic too long");
    return;
  }

  _s->mqttEnabled = enabled ? 1 : 0;
  _s->mqttPort = (uint16_t)port;
  _s->mqttPublishPeriodMs = (uint16_t)publishMs;
  _s->mqttRetain = retain ? 1 : 0;

  _store->save(*_s);

  Serial0.printf("[WEB] MQTT saved: enabled=%u host=%s port=%u clientId=%s base=%s\n",
                 (unsigned)_s->mqttEnabled,
                 _s->mqttHost,
                 (unsigned)_s->mqttPort,
                 _s->mqttClientId,
                 _s->mqttBaseTopic);

  replyOkJsonRebootRecommended();
}

String WebManager::buildNotificationMessage(uint8_t alertState, bool isTest, float dbInstant, float leq, float peak, uint32_t durationMs) const {
  const String hostname = (_s && _s->hostname[0] != '\0') ? String(_s->hostname) : String("soundpanel7");
  const NotificationVisualStyle style = notificationVisualStyle(alertState, isTest);
  const float triggerThreshold = (alertState == 2) ? (_s ? _s->th.orangeMax : DEFAULT_ORANGE_MAX)
                                                   : (_s ? _s->th.greenMax : DEFAULT_GREEN_MAX);
  const char* triggerLabel = (alertState == 2) ? "Seuil rouge" : "Seuil warning";

  struct tm ti;
  char tbuf[32] = {0};
  const bool hasTime = getLocalTime(&ti, 0);
  if (hasTime) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);

  String message;
  message.reserve(512);
  message += style.emoji;
  message += " SoundPanel 7";
  message += "\n";
  message += "• ";
  message += style.title;
  message += " - ";
  message += String(dbInstant, 1);
  message += " dB";
  if (!isTest && alertState > 0) {
    message += "\n";
    message += "• ";
    message += triggerLabel;
    message += ": > ";
    message += String(triggerThreshold, 0);
    message += " dB";
  } else if (!isTest && alertState == 0 && durationMs > 0) {
    message += "\n";
    message += "• Duree alerte: ";
    message += formatAlertDuration(durationMs);
  }
  message += "\n";
  message += "• Equipement: ";
  message += hostname;
  message += "\n";
  message += "\n📊 Mesures";
  message += "\n• dB instantane: ";
  message += String(dbInstant, 1);
  message += " dB";
  message += "\n• Leq: ";
  message += String(leq, 1);
  message += " dB";
  message += "\n• Peak: ";
  message += String(peak, 1);
  message += " dB";
  message += "\n\n🎯 Seuils";
  message += "\n• Vert <= ";
  message += String(_s ? _s->th.greenMax : DEFAULT_GREEN_MAX);
  message += " dB";
  message += "\n• Orange <= ";
  message += String(_s ? _s->th.orangeMax : DEFAULT_ORANGE_MAX);
  message += " dB";
  if (hasTime) {
    message += "\n\n🕒 Horodatage";
    message += "\n• ";
    message += tbuf;
  }
  return message;
}

bool WebManager::postJsonToUrl(const String& url,
                               const String& payload,
                               const String& authorization,
                               int& statusCodeOut,
                               String& responseOut) {
  statusCodeOut = -1;
  responseOut = "";

  if (!url.length()) return false;
  WiFi.setSleep(false);

  HTTPClient http;
  http.setConnectTimeout(kNotificationHttpConnectTimeoutMs);
  http.setTimeout(kNotificationHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setUserAgent(String("SoundPanel7/") + SOUNDPANEL7_VERSION);

  bool started = false;
  if (url.startsWith("https://")) {
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(kNotificationHttpTimeoutMs);
    started = http.begin(client, url);
    if (!started) return false;
    http.addHeader("Content-Type", "application/json");
    if (authorization.length()) http.addHeader("Authorization", authorization);
    statusCodeOut = http.POST((uint8_t*)payload.c_str(), payload.length());
    if (statusCodeOut <= 0) {
      responseOut = http.errorToString(statusCodeOut);
      Serial0.printf("[WEB] HTTPS fail url=%s code=%d wifi=%d ip=%s rssi=%ld heap=%lu min=%lu\n",
                     url.c_str(),
                     statusCodeOut,
                     (int)WiFi.status(),
                     WiFi.localIP().toString().c_str(),
                     (long)WiFi.RSSI(),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    } else {
      responseOut = http.getString();
    }
    http.end();
    return statusCodeOut > 0;
  }

  WiFiClient client;
  client.setTimeout(kNotificationHttpTimeoutMs);
  started = http.begin(client, url);
  if (!started) return false;
  http.addHeader("Content-Type", "application/json");
  if (authorization.length()) http.addHeader("Authorization", authorization);
  statusCodeOut = http.POST((uint8_t*)payload.c_str(), payload.length());
  if (statusCodeOut <= 0) {
    responseOut = http.errorToString(statusCodeOut);
    Serial0.printf("[WEB] HTTP fail url=%s code=%d wifi=%d ip=%s rssi=%ld heap=%lu min=%lu\n",
                   url.c_str(),
                   statusCodeOut,
                   (int)WiFi.status(),
                   WiFi.localIP().toString().c_str(),
                   (long)WiFi.RSSI(),
                   (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  } else {
    responseOut = http.getString();
  }
  http.end();

  return statusCodeOut > 0;
}

bool WebManager::sendSlackNotification(uint8_t alertState, bool isTest, const String& message, String& summary) {
  if (!_s || !_s->slackEnabled) return true;
  if (!_s->slackWebhookUrl[0]) {
    summary = "Slack: webhook manquant";
    return false;
  }

  const NotificationVisualStyle style = notificationVisualStyle(alertState, isTest);
  const int firstBreak = message.indexOf('\n');
  const String preview = firstBreak >= 0 ? message.substring(0, firstBreak) : message;

  String payloadRich;
  payloadRich.reserve(256 + (message.length() * 2));
  payloadRich += "{\"text\":\"";
  payloadRich += sp7json::escape(preview.c_str());
  payloadRich += "\",\"attachments\":[{\"color\":\"";
  payloadRich += style.slackColor;
  payloadRich += "\",\"text\":\"";
  payloadRich += sp7json::escape(message.c_str());
  payloadRich += "\"}]";
  if (_s->slackChannel[0]) {
    payloadRich += ",\"channel\":\"";
    payloadRich += sp7json::escape(_s->slackChannel);
    payloadRich += "\"";
  }
  payloadRich += "}";

  String payloadPlain = String("{\"text\":\"") + sp7json::escape(message.c_str()) + "\"";
  if (_s->slackChannel[0]) {
    payloadPlain += ",\"channel\":\"";
    payloadPlain += sp7json::escape(_s->slackChannel);
    payloadPlain += "\"";
  }
  payloadPlain += "}";

  int statusCode = -1;
  String response;
  if (!postJsonToUrl(_s->slackWebhookUrl, payloadRich, "", statusCode, response)) {
    summary = "Slack: echec HTTP";
    if (response.length()) summary += " (" + trimmedHttpResponse(response) + ")";
    return false;
  }
  if (statusCode >= 200 && statusCode < 300) {
    summary = "Slack OK (barre couleur)";
    return true;
  }

  int fallbackStatusCode = -1;
  String fallbackResponse;
  if (!postJsonToUrl(_s->slackWebhookUrl, payloadPlain, "", fallbackStatusCode, fallbackResponse)) {
    summary = "Slack: echec HTTP";
    if (fallbackResponse.length()) summary += " (" + trimmedHttpResponse(fallbackResponse) + ")";
    return false;
  }
  if (fallbackStatusCode >= 200 && fallbackStatusCode < 300) {
    summary = "Slack OK (sans barre couleur)";
    return true;
  }

  summary = "Slack " + String(fallbackStatusCode) + " " + trimmedHttpResponse(fallbackResponse);
  return false;
}

bool WebManager::sendTelegramNotification(const String& message, String& summary) {
  if (!_s || !_s->telegramEnabled) return true;
  if (!_s->telegramBotToken[0] || !_s->telegramChatId[0]) {
    summary = "Telegram: token/chat manquant";
    return false;
  }

  String url = "https://api.telegram.org/bot";
  url += _s->telegramBotToken;
  url += "/sendMessage";

  String payload;
  payload.reserve(256 + message.length());
  payload += "{";
  payload += "\"chat_id\":\""; payload += sp7json::escape(_s->telegramChatId); payload += "\",";
  payload += "\"text\":\""; payload += sp7json::escape(message.c_str()); payload += "\",";
  payload += "\"disable_web_page_preview\":true";
  payload += "}";

  int statusCode = -1;
  String response;
  if (!postJsonToUrl(url, payload, "", statusCode, response)) {
    summary = "Telegram: echec HTTP";
    if (response.length()) summary += " (" + trimmedHttpResponse(response) + ")";
    return false;
  }
  if (statusCode >= 200 && statusCode < 300) {
    summary = "Telegram OK";
    return true;
  }

  summary = "Telegram " + String(statusCode) + " " + trimmedHttpResponse(response);
  return false;
}

bool WebManager::sendWhatsappNotification(const String& message, String& summary) {
  if (!_s || !_s->whatsappEnabled) return true;
  if (!_s->whatsappAccessToken[0] || !_s->whatsappPhoneNumberId[0] || !_s->whatsappRecipient[0]) {
    summary = "WhatsApp: config incomplete";
    return false;
  }

  String url = "https://graph.facebook.com/";
  url += (_s->whatsappApiVersion[0] ? _s->whatsappApiVersion : "v22.0");
  url += "/";
  url += _s->whatsappPhoneNumberId;
  url += "/messages";

  String payload;
  payload.reserve(320 + message.length());
  payload += "{";
  payload += "\"messaging_product\":\"whatsapp\",";
  payload += "\"to\":\""; payload += sp7json::escape(_s->whatsappRecipient); payload += "\",";
  payload += "\"type\":\"text\",";
  payload += "\"text\":{\"preview_url\":false,\"body\":\""; payload += sp7json::escape(message.c_str()); payload += "\"}";
  payload += "}";

  const String authorization = String("Bearer ") + _s->whatsappAccessToken;
  int statusCode = -1;
  String response;
  if (!postJsonToUrl(url, payload, authorization, statusCode, response)) {
    summary = "WhatsApp: echec HTTP";
    if (response.length()) summary += " (" + trimmedHttpResponse(response) + ")";
    return false;
  }
  if (statusCode >= 200 && statusCode < 300) {
    summary = "WhatsApp OK";
    return true;
  }

  summary = "WhatsApp " + String(statusCode) + " " + trimmedHttpResponse(response);
  return false;
}

bool WebManager::dispatchNotification(uint8_t alertState,
                                      bool isTest,
                                      float dbInstant,
                                      float leq,
                                      float peak,
                                      uint32_t durationMs,
                                      bool updateAlertTracking) {
  if (!_s) return false;

  const bool slackActive = _s->slackEnabled != 0;
  const bool telegramActive = _s->telegramEnabled != 0;
  const bool whatsappActive = _s->whatsappEnabled != 0;
  if (!slackActive && !telegramActive && !whatsappActive) {
    _notificationLastOk = false;
    _notificationLastEvent = isTest ? "test" : String(alertStateName(alertState));
    _notificationLastResult = "Aucune cible active";
    _notificationLastAttemptTs = currentUnixTimestamp();
    return false;
  }

  if (!WiFi.isConnected()) {
    _notificationLastOk = false;
    _notificationLastEvent = isTest ? "test" : String(alertStateName(alertState));
    _notificationLastResult = "WiFi indisponible";
    _notificationLastAttemptTs = currentUnixTimestamp();
    return false;
  }

  const String message = buildNotificationMessage(alertState, isTest, dbInstant, leq, peak, durationMs);

  bool overallOk = true;
  bool attempted = false;
  String summary;

  if (slackActive) {
    String result;
    attempted = true;
    if (!sendSlackNotification(alertState, isTest, message, result)) overallOk = false;
    if (summary.length()) summary += " | ";
    summary += result;
  }
  if (telegramActive) {
    String result;
    attempted = true;
    if (!sendTelegramNotification(message, result)) overallOk = false;
    if (summary.length()) summary += " | ";
    summary += result;
  }
  if (whatsappActive) {
    String result;
    attempted = true;
    if (!sendWhatsappNotification(message, result)) overallOk = false;
    if (summary.length()) summary += " | ";
    summary += result;
  }

  _notificationLastAttemptTs = currentUnixTimestamp();
  _notificationLastOk = attempted && overallOk;
  _notificationLastEvent = isTest ? "test" : String(alertStateName(alertState));
  _notificationLastResult = summary.length() ? summary : String("Aucune cible active");
  if (_notificationLastOk) {
    _notificationLastSuccessTs = _notificationLastAttemptTs;
    if (updateAlertTracking) _lastNotifiedAlertState = alertState;
  }

  Serial0.printf("[WEB][NOTIFY] event=%s result=%s\n",
                 _notificationLastEvent.c_str(),
                 _notificationLastResult.c_str());
  return _notificationLastOk;
}

String WebManager::notificationsJson(bool includeSecrets) const {
  String json;
  json.reserve(1024);
  json += "{";
  json += "\"notifyOnWarning\":"; json += (_s && _s->notifyOnWarning == ALERT_NOTIFY_LEVEL_WARNING ? "true" : "false"); json += ",";
  json += "\"notifyOnRecovery\":"; json += (_s && _s->notifyOnRecovery ? "true" : "false"); json += ",";
  json += "\"slackEnabled\":"; json += (_s && _s->slackEnabled ? "true" : "false"); json += ",";
  json += "\"slackWebhookConfigured\":"; json += (_s && strlen(_s->slackWebhookUrl) ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "slackChannel", _s ? _s->slackChannel : "");
  if (includeSecrets) sp7json::appendEscapedField(json, "slackWebhookUrl", _s ? _s->slackWebhookUrl : "");
  sp7json::appendEscapedField(json, "telegramChatId", _s ? _s->telegramChatId : "");
  json += "\"telegramEnabled\":"; json += (_s && _s->telegramEnabled ? "true" : "false"); json += ",";
  json += "\"telegramTokenConfigured\":"; json += (_s && strlen(_s->telegramBotToken) ? "true" : "false"); json += ",";
  if (includeSecrets) sp7json::appendEscapedField(json, "telegramBotToken", _s ? _s->telegramBotToken : "");
  json += "\"whatsappEnabled\":"; json += (_s && _s->whatsappEnabled ? "true" : "false"); json += ",";
  sp7json::appendEscapedField(json, "whatsappPhoneNumberId", _s ? _s->whatsappPhoneNumberId : "");
  sp7json::appendEscapedField(json, "whatsappRecipient", _s ? _s->whatsappRecipient : "");
  sp7json::appendEscapedField(json, "whatsappApiVersion", _s ? _s->whatsappApiVersion : "v22.0");
  json += "\"whatsappAccessTokenConfigured\":"; json += (_s && strlen(_s->whatsappAccessToken) ? "true" : "false"); json += ",";
  if (includeSecrets) sp7json::appendEscapedField(json, "whatsappAccessToken", _s ? _s->whatsappAccessToken : "");
  json += "\"currentAlertState\":"; json += String(_alertState); json += ",";
  json += "\"lastOk\":"; json += (_notificationLastOk ? "true" : "false"); json += ",";
  json += "\"lastAttemptTs\":"; json += String(_notificationLastAttemptTs); json += ",";
  json += "\"lastSuccessTs\":"; json += String(_notificationLastSuccessTs); json += ",";
  sp7json::appendEscapedField(json, "lastEvent", _notificationLastEvent.c_str());
  sp7json::appendEscapedField(json, "lastResult", _notificationLastResult.c_str(), false);
  json += "}";
  return json;
}

void WebManager::handleNotificationsGet() {
  if (!requireWebAuth()) return;
  if (!requireSettingsJson()) return;
  replyJson(200, notificationsJson(false));
}

void WebManager::handleDebugLogsGet() {
  if (!requireWebAuth()) return;

  const String tail = DebugLog::snapshotText();
  String json;
  json.reserve(tail.length() + 160);
  json += "{";
  json += "\"lineCount\":"; json += String(DebugLog::lineCount()); json += ",";
  json += "\"wifiConnected\":"; json += (WiFi.isConnected() ? "true" : "false"); json += ",";
  json += "\"uptime_s\":"; json += String((millis() - g_bootMs) / 1000UL); json += ",";
  sp7json::appendEscapedField(json, "tail", tail.c_str(), false);
  json += "}";
  replyJson(200, json);
}

void WebManager::handleDebugLogsClear() {
  if (!requireWebAuth()) return;
  DebugLog::clear();
  DebugLog::println("[WEB] Debug log buffer cleared");
  replyOkJson();
}

void WebManager::handleNotificationsSave() {
  if (!requireWebAuth()) return;
  if (!requireStoreAndSettingsJson()) return;

  const String body = _srv.arg("plain");
  SettingsV1 next = *_s;

  next.notifyOnWarning = sp7json::parseBool(body, "notifyOnWarning", next.notifyOnWarning == ALERT_NOTIFY_LEVEL_WARNING)
    ? ALERT_NOTIFY_LEVEL_WARNING
    : ALERT_NOTIFY_LEVEL_CRITICAL;
  next.notifyOnRecovery = sp7json::parseBool(body, "notifyOnRecovery", next.notifyOnRecovery != 0) ? 1 : 0;
  next.slackEnabled = sp7json::parseBool(body, "slackEnabled", next.slackEnabled != 0) ? 1 : 0;
  next.telegramEnabled = sp7json::parseBool(body, "telegramEnabled", next.telegramEnabled != 0) ? 1 : 0;
  next.whatsappEnabled = sp7json::parseBool(body, "whatsappEnabled", next.whatsappEnabled != 0) ? 1 : 0;

  String slackWebhookUrl = sp7json::parseString(body, "slackWebhookUrl", "", true);
  String slackChannel = sp7json::parseString(body, "slackChannel", String(next.slackChannel), false);
  String telegramBotToken = sp7json::parseString(body, "telegramBotToken", "", true);
  String telegramChatId = sp7json::parseString(body, "telegramChatId", String(next.telegramChatId), false);
  String whatsappAccessToken = sp7json::parseString(body, "whatsappAccessToken", "", true);
  String whatsappPhoneNumberId = sp7json::parseString(body, "whatsappPhoneNumberId", String(next.whatsappPhoneNumberId), false);
  String whatsappRecipient = sp7json::parseString(body, "whatsappRecipient", String(next.whatsappRecipient), false);
  String whatsappApiVersion = sp7json::parseString(body, "whatsappApiVersion", String(next.whatsappApiVersion), false);

  const bool slackKeepWebhook = sp7json::parseBool(body, "slackKeepWebhook", false);
  const bool telegramKeepToken = sp7json::parseBool(body, "telegramKeepToken", false);
  const bool whatsappKeepAccessToken = sp7json::parseBool(body, "whatsappKeepAccessToken", false);

  slackWebhookUrl.trim();
  slackChannel.trim();
  telegramBotToken.trim();
  telegramChatId.trim();
  whatsappAccessToken.trim();
  whatsappPhoneNumberId.trim();
  whatsappRecipient.trim();
  whatsappApiVersion.trim();

  if (slackKeepWebhook && !slackWebhookUrl.length()) slackWebhookUrl = String(_s->slackWebhookUrl);
  if (telegramKeepToken && !telegramBotToken.length()) telegramBotToken = String(_s->telegramBotToken);
  if (whatsappKeepAccessToken && !whatsappAccessToken.length()) whatsappAccessToken = String(_s->whatsappAccessToken);

  if (next.slackEnabled && !(slackWebhookUrl.startsWith("https://") || slackWebhookUrl.startsWith("http://"))) {
    replyErrorJson(400, "bad slack webhook");
    return;
  }
  if (next.telegramEnabled && (!telegramBotToken.length() || !telegramChatId.length())) {
    replyErrorJson(400, "telegram token/chat required");
    return;
  }
  if (next.whatsappEnabled && (!whatsappAccessToken.length() || !whatsappPhoneNumberId.length() || !whatsappRecipient.length())) {
    replyErrorJson(400, "whatsapp config incomplete");
    return;
  }
  if (next.whatsappEnabled && (!whatsappApiVersion.startsWith("v") || whatsappApiVersion.length() < 2)) {
    replyErrorJson(400, "bad whatsapp api version");
    return;
  }

  if (!sp7json::safeCopy(next.slackWebhookUrl, sizeof(next.slackWebhookUrl), slackWebhookUrl)) {
    replyErrorJson(400, "slack webhook too long");
    return;
  }
  if (!sp7json::safeCopy(next.slackChannel, sizeof(next.slackChannel), slackChannel)) {
    replyErrorJson(400, "slack channel too long");
    return;
  }
  if (!sp7json::safeCopy(next.telegramBotToken, sizeof(next.telegramBotToken), telegramBotToken)) {
    replyErrorJson(400, "telegram token too long");
    return;
  }
  if (!sp7json::safeCopy(next.telegramChatId, sizeof(next.telegramChatId), telegramChatId)) {
    replyErrorJson(400, "telegram chat too long");
    return;
  }
  if (!sp7json::safeCopy(next.whatsappAccessToken, sizeof(next.whatsappAccessToken), whatsappAccessToken)) {
    replyErrorJson(400, "whatsapp token too long");
    return;
  }
  if (!sp7json::safeCopy(next.whatsappPhoneNumberId, sizeof(next.whatsappPhoneNumberId), whatsappPhoneNumberId)) {
    replyErrorJson(400, "whatsapp phone id too long");
    return;
  }
  if (!sp7json::safeCopy(next.whatsappRecipient, sizeof(next.whatsappRecipient), whatsappRecipient)) {
    replyErrorJson(400, "whatsapp recipient too long");
    return;
  }
  if (!sp7json::safeCopy(next.whatsappApiVersion, sizeof(next.whatsappApiVersion), whatsappApiVersion.length() ? whatsappApiVersion : String("v22.0"))) {
    replyErrorJson(400, "whatsapp api version too long");
    return;
  }

  *_s = next;
  _store->save(*_s);
  replyJson(200, String("{\"ok\":true,\"config\":") + notificationsJson(false) + "}");
}

void WebManager::handleNotificationsTest() {
  if (!requireWebAuth()) return;
  if (!requireSettingsJson()) return;

  if (!dispatchNotification(_alertState, true, g_webDbInstant, g_webLeq, g_webPeak, 0, false)) {
    replyErrorJson(502, _notificationLastResult.length() ? _notificationLastResult : String("notification failed"));
    return;
  }

  replyJson(200, String("{\"ok\":true,\"lastOk\":true,\"lastResult\":\"") + sp7json::escape(_notificationLastResult.c_str()) + "\"}");
}

void WebManager::handleRoot() {
  static const char html[] PROGMEM =
R"HTML(
<!doctype html>
<html lang="fr">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>SoundPanel 7</title>
  <style>
    :root{
      --bg:#0B0F14; --screenRed:#22080B; --panel:#111824; --panel2:#0F1722; --panel3:#0E141C;
      --txt:#DFE7EF; --muted:#8EA1B3; --line:#1E2A38;
      --green:#23C552; --orange:#F0A202; --red:#E53935; --accent:#7A1E2C;
      --radius:22px;
      font-family: system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    }
    *{box-sizing:border-box}
    body{margin:0;background:var(--bg);color:var(--txt);transition:background .2s ease}
    .shell{max-width:1120px;margin:0 auto;padding:0 16px 24px}
    .top{
      position:sticky;top:0;z-index:20;
      background:rgba(15,23,34,.92);backdrop-filter:blur(14px);
      border-bottom:1px solid var(--line);
    }
    .topInner{
      max-width:1120px;margin:0 auto;padding:14px 16px 12px;
      display:grid;grid-template-columns:auto 1fr;gap:14px;align-items:center;
    }
    .brand{display:flex;flex-direction:column;gap:3px}
    .title{font-size:22px;font-weight:700;letter-spacing:.2px}
    .subtitle{font-size:12px;color:var(--muted)}
    .tabs{display:flex;gap:8px;justify-content:center;flex-wrap:wrap}
    .tab{
      appearance:none;border:0;cursor:pointer;
      padding:10px 14px;border-radius:12px;
      background:var(--panel3);color:var(--muted);font-weight:700;font-size:13px;
      transition:background .15s ease,color .15s ease,transform .15s ease;
    }
    .tab.active{background:var(--accent);color:#fff}
    .tab:active{transform:translateY(1px)}
    .pages{padding-top:18px}
    .page{display:none}
    .page.active{display:block}
    .stack{display:flex;flex-direction:column;gap:14px}
    .grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px}
    .gridOverview{
      display:grid;grid-template-columns:245px minmax(280px,1fr) 245px;gap:14px;align-items:stretch;
    }
    .rightCol{display:grid;grid-template-rows:1fr 1fr;gap:14px}
    .card{
      position:relative;
      background:var(--panel);border:1px solid var(--line);border-radius:var(--radius);padding:16px;
      box-shadow:0 12px 34px rgba(0,0,0,.24);
    }
    .card.clockCard{background:#101A28}
    .card.soundCard.alertOrange{
      background:#3D2810;border-color:#F0A202;box-shadow:0 0 0 2px rgba(240,162,2,.18), 0 12px 34px rgba(0,0,0,.24);
    }
    .card.soundCard.alertRed{
      background:#4A161B;border-color:#FF5A5F;box-shadow:0 0 0 3px rgba(255,90,95,.28), 0 12px 34px rgba(0,0,0,.3);
    }
    .sectionTitle{font-size:24px;font-weight:800;margin:0}
    .k{font-size:14px;color:var(--muted)}
    .v{font-size:30px;font-weight:800;margin-top:10px}
    .metricCard{
      background:var(--panel3);border:1px solid rgba(255,255,255,.03);
      border-radius:22px;padding:18px;
    }
    #soundLeq,#soundPeak{
      display:block;width:100%;text-align:center;
    }
    .clockMiniDate,.clockDate{color:var(--muted);font-variant-numeric:tabular-nums}
    .clockMiniDate{font-size:16px;text-align:center}
    .clockMiniMain{
      font-size:56px;font-weight:800;letter-spacing:-2px;text-align:center;
      margin-top:34px;font-variant-numeric:tabular-nums;
    }
    .clockMiniSec{
      width:78px;height:34px;border-radius:12px;background:var(--accent);color:#fff;
      display:grid;place-items:center;margin:30px 0 0 auto;font-weight:800;font-variant-numeric:tabular-nums;
    }
    .clockHero{
      min-height:362px;padding:28px;background:#101A28;
      display:flex;flex-direction:column;justify-content:center;overflow:hidden;
    }
    .clockRule{height:2px;background:#243244;border-radius:99px;margin-top:12px}
    .clockHeroDate{position:absolute;top:24px;right:28px;font-size:24px}
    .clockHeroRow{
      display:flex;align-items:center;justify-content:center;gap:12px;
      margin-top:18px;
    }
    .clockHeroMain{
      font-size:clamp(72px,14vw,170px);font-weight:800;line-height:.9;letter-spacing:-5px;
      font-variant-numeric:tabular-nums;
    }
    .clockHeroSec{
      width:min(29vw,262px);height:min(17vw,156px);border-radius:24px;background:var(--accent);
      display:grid;place-items:center;font-size:clamp(42px,9vw,120px);font-weight:800;
      font-variant-numeric:tabular-nums;line-height:1;color:#fff;flex:0 0 auto;
    }
    .gaugeWrap{
      display:flex;flex-direction:column;align-items:center;justify-content:center;gap:10px;
      height:100%;
    }
    .gauge{
      width:min(72vw,240px);height:min(72vw,240px);border-radius:50%;
      display:grid;place-items:center;position:relative;flex:0 0 auto;
      background:
        radial-gradient(circle at center, #0B0F14 0 57%, transparent 58%),
        conic-gradient(var(--gaugeColor, var(--green)) calc(var(--pct)*1%), #1A2332 0);
      transform:rotate(135deg);
    }
    .gauge.large{width:min(58vw,240px);height:min(58vw,240px)}
    .gauge::before{
      content:"";position:absolute;inset:18px;border-radius:50%;
      background:radial-gradient(circle at center, #0B0F14 0 61%, transparent 62%);
    }
    .gaugeInner{
      position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;
      transform:rotate(-135deg);z-index:2;
    }
    .db{font-size:clamp(52px,10vw,88px);font-weight:800;line-height:1;color:var(--txt);}
    .db.large{font-size:56px}
    .unit{font-size:clamp(18px,3vw,24px);color:var(--muted);margin-top:4px;}
    .dot{width:14px;height:14px;border-radius:50%;background:var(--gaugeColor, var(--green));margin-top:14px;}
    .soundHero{
      display:grid;grid-template-columns:280px 1fr;gap:20px;align-items:center;min-height:240px;
    }
    .soundMetrics{display:grid;grid-template-columns:1fr 1fr;gap:14px}
    .alertBadge{
      position:absolute;top:18px;left:258px;
      min-width:112px;height:34px;padding:0 16px;border-radius:17px;background:var(--red);
      display:none;align-items:center;justify-content:center;color:#fff;font-size:14px;font-weight:800;
    }
    .alertBadge.show{display:flex}
    .histCard{overflow:hidden}
    .histTop{
      display:grid;grid-template-columns:1fr auto 1fr;align-items:center;gap:12px;margin-bottom:10px;
    }
    .histMeta{font-size:12px;color:var(--muted)}
    .histMeta.center{text-align:center}
    .histMeta.right{text-align:right}
    .chartWrap{
      position:relative;width:100%;height:220px;overflow:visible;
      padding:0;
      display:flex;align-items:stretch;gap:8px;
    }
    .chartPlot{
      flex:1 1 auto;height:100%;
      border-radius:14px;
      background:#0E141C;
      border:1px solid rgba(255,255,255,.04);
      padding:10px 10px 14px;
      display:flex;align-items:flex-end;
      overflow:hidden;
      min-width:0;
    }
    .histScale{
      position:relative;width:64px;height:100%;
      color:#90A1B2;font-size:12px;line-height:1;
      flex:0 0 64px;
      box-sizing:border-box;
      padding-top:10px;
      padding-bottom:14px;
    }
    .histScaleTopLabel{
      position:absolute;right:0;bottom:calc(100% + 6px);
      color:#90A1B2;font-size:12px;font-weight:400;line-height:1;
      white-space:nowrap;font-variant-numeric:tabular-nums;
      background:var(--panel);
      padding:0 2px 0 6px;
    }
    .histScaleTopTick{
      position:absolute;right:52px;bottom:100%;
      width:10px;height:1px;background:#74879A;opacity:.95;
    }
    .histScaleRow{
      position:absolute;right:0;width:100%;
      display:flex;align-items:center;justify-content:flex-end;gap:8px;
      white-space:nowrap;
    }
    .histScaleRow.mid{transform:translateY(50%)}
    .histScaleRow.bottom{transform:translateY(50%)}
    .histScaleRow.bottom35{
      bottom:14px !important;
      right:0;
      transform:none;
    }
    .histScaleTick{
      width:10px;height:1px;background:#74879A;opacity:.95;flex:0 0 10px;
    }
    .histScaleLabel{
      min-width:42px;text-align:right;font-size:12px;font-weight:400;
      font-variant-numeric:tabular-nums;
    }
    .histBars{
      width:100%;height:100%;display:flex;align-items:flex-end;gap:2px;min-width:0;
    }
    .histBar{
      flex:1 1 0;min-width:0;height:4px;border-radius:3px 3px 0 0;
      background:#1A2332;
    }
    .histAxis{
      display:flex;justify-content:space-between;align-items:center;
      color:#6F8192;font-size:12px;padding:8px 4px 0;
      position:relative;
    }
    .histAxisMid{
      position:absolute;left:50%;transform:translateX(-50%);
    }
    .settingsPage{
      display:grid;grid-template-columns:minmax(0,1.18fr) 360px;grid-template-areas:"main rail";
      gap:16px;align-items:start
    }
    .settingsMain,.settingsRail{display:flex;flex-direction:column;gap:16px}
    .settingsMain{grid-area:main}
    .settingsRail{grid-area:rail;position:sticky;top:16px}
    .settingsMain{gap:20px}
    .settingsHero{
      background:
        radial-gradient(circle at top right, rgba(122,30,44,.34), transparent 34%),
        linear-gradient(135deg, #131d2b 0%, #111824 58%, #0f1621 100%);
    }
    .settingsMega{
      overflow:hidden;
      background:
        radial-gradient(circle at top right, rgba(255,255,255,.05), transparent 28%),
        linear-gradient(180deg, #121b27 0%, #101722 100%);
    }
    .settingsMegaWarm{
      background:
        radial-gradient(circle at top right, rgba(240,162,2,.13), transparent 30%),
        radial-gradient(circle at top left, rgba(122,30,44,.18), transparent 24%),
        linear-gradient(180deg, #131d29 0%, #101722 100%);
    }
    .settingsMegaCool{
      background:
        radial-gradient(circle at top right, rgba(72,149,239,.15), transparent 30%),
        radial-gradient(circle at bottom left, rgba(35,197,82,.08), transparent 28%),
        linear-gradient(180deg, #111a27 0%, #101722 100%);
    }
    .settingsMegaNeutral{
      background:
        radial-gradient(circle at top right, rgba(122,30,44,.14), transparent 30%),
        linear-gradient(180deg, #121b27 0%, #101722 100%);
    }
    .settingsMegaGrid{
      display:grid;grid-template-columns:minmax(0,1fr) minmax(0,1fr);gap:18px;margin-top:14px;
    }
    .settingsColumn{
      display:flex;flex-direction:column;gap:18px;min-width:0;
    }
    .settingsColumn .settingsSubsection{
      padding:16px 16px 0;
      border:1px solid rgba(255,255,255,.04);
      border-radius:18px;
      background:rgba(10,16,24,.34);
    }
    .settingsColumn .settingsSubsection + .settingsSubsection{
      margin-top:0;padding-top:16px;border-top:1px solid rgba(255,255,255,.04);
    }
    .settingsGroup{
      display:flex;flex-direction:column;gap:14px;
    }
    .settingsGroupHead{
      display:flex;justify-content:space-between;align-items:flex-end;gap:14px;flex-wrap:wrap;
      padding:2px 4px 0;
    }
    .settingsGroupIntro{
      display:flex;flex-direction:column;gap:6px;
    }
    .settingsGroupTitle{
      margin:0;font-size:21px;font-weight:800;letter-spacing:-.02em;
    }
    .settingsGroupLead{
      max-width:60ch;font-size:13px;line-height:1.5;color:var(--muted);
    }
    .settingsGroupTag{
      display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;
      border:1px solid rgba(255,255,255,.05);background:rgba(255,255,255,.03);
      color:#d8e0e8;font-size:12px;font-weight:700;
    }
    .settingsDeck{
      display:grid;grid-template-columns:1fr 1fr;gap:14px;
    }
    .cardSpan2{grid-column:1 / -1}
    .settingsSubsection{
      display:flex;flex-direction:column;gap:12px;
    }
    .settingsSubsection + .settingsSubsection{
      margin-top:18px;padding-top:18px;border-top:1px solid rgba(255,255,255,.05);
    }
    .settingsSubhead{
      display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap;
    }
    .settingsSubtitle{
      margin:0;font-size:18px;font-weight:800;letter-spacing:-.02em;
    }
    .settingsSublead{
      font-size:12px;line-height:1.45;color:var(--muted);max-width:56ch;
    }
    .settingsNav{
      background:
        radial-gradient(circle at top right, rgba(35,197,82,.14), transparent 34%),
        linear-gradient(180deg, #131d2b 0%, #101722 100%);
    }
    .settingsJumpList{
      display:grid;grid-template-columns:1fr;gap:10px;margin-top:14px;
    }
    .settingsJumpLink{
      display:flex;justify-content:space-between;align-items:center;gap:10px;
      min-height:72px;padding:12px 14px;border-radius:16px;text-decoration:none;color:var(--txt);
      border:1px solid rgba(255,255,255,.04);background:var(--panel3);
      transition:.18s ease;
    }
    .settingsJumpLink:hover{
      border-color:rgba(255,255,255,.08);transform:translateY(-1px);
      background:#182231;
    }
    .settingsJumpMain{
      display:flex;flex-direction:column;gap:4px;min-width:0;
    }
    .settingsJumpTitle{font-size:14px;font-weight:800}
    .settingsJumpMeta{font-size:12px;color:var(--muted);line-height:1.4}
    .settingsJumpArrow{color:#93a4b6;font-size:16px;font-weight:800}
    .settingsHeroPrime{
      padding-bottom:22px;
      background:
        radial-gradient(circle at top right, rgba(122,30,44,.26), transparent 34%),
        radial-gradient(circle at bottom left, rgba(35,197,82,.08), transparent 28%),
        linear-gradient(135deg, #131d2b 0%, #111824 58%, #0f1621 100%);
    }
    .settingsHeroPrime .sectionLead{max-width:64ch}
    .settingsTopline{
      display:flex;justify-content:space-between;align-items:flex-start;gap:14px;flex-wrap:wrap;
      margin-top:18px;
    }
    .settingsPillStrip{
      display:flex;flex-wrap:wrap;gap:10px;
    }
    .settingsPillMetric{
      min-width:136px;
      padding:11px 13px;
      border-radius:16px;
      background:rgba(7,11,17,.34);
      border:1px solid rgba(255,255,255,.06);
    }
    .settingsPillMetric .k{
      font-size:11px;font-weight:800;letter-spacing:.08em;text-transform:uppercase;color:#93a4b6;
    }
    .settingsPillMetric .v{
      margin-top:6px;font-size:16px;font-weight:800;line-height:1.25;color:#edf3f8;
    }
    .settingsHeroCallout{
      max-width:320px;
      padding:14px 16px;
      border-radius:18px;
      background:rgba(7,11,17,.36);
      border:1px solid rgba(255,255,255,.06);
      color:#d8e0e8;
      font-size:13px;
      line-height:1.5;
    }
    .settingsSnapshotGrid{
      display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin-top:18px;
    }
    .settingsSnapshot{
      min-width:0;
      padding:12px 0 0;
      border-top:1px solid rgba(255,255,255,.08);
    }
    .settingsSnapshot .k{
      font-size:11px;font-weight:800;letter-spacing:.08em;text-transform:uppercase;color:#93a4b6;
    }
    .settingsSnapshot .v{
      margin-top:7px;
      font-size:15px;
      line-height:1.4;
      font-weight:700;
      color:#edf3f8;
      overflow-wrap:anywhere;
      word-break:break-word;
    }
    .settingsSection{
      padding:22px;
      border-radius:28px;
      border:1px solid rgba(255,255,255,.05);
      overflow:hidden;
    }
    .settingsSectionWarm{
      background:
        radial-gradient(circle at top right, rgba(240,162,2,.12), transparent 30%),
        radial-gradient(circle at left, rgba(122,30,44,.16), transparent 24%),
        linear-gradient(180deg, #121b28 0%, #101722 100%);
    }
    .settingsSectionCool{
      background:
        radial-gradient(circle at top right, rgba(72,149,239,.14), transparent 32%),
        radial-gradient(circle at bottom left, rgba(35,197,82,.08), transparent 28%),
        linear-gradient(180deg, #111a27 0%, #101722 100%);
    }
    .settingsSectionNeutral{
      background:
        radial-gradient(circle at top right, rgba(122,30,44,.11), transparent 30%),
        linear-gradient(180deg, #121b27 0%, #101722 100%);
    }
    .settingsSectionHeader{
      display:flex;justify-content:space-between;align-items:flex-start;gap:14px;flex-wrap:wrap;
    }
    .settingsSectionStamp{
      display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;
      border:1px solid rgba(255,255,255,.06);background:rgba(255,255,255,.03);
      color:#d8e0e8;font-size:12px;font-weight:700;
    }
    .settingsSectionGrid{
      display:grid;grid-template-columns:minmax(0,1.14fr) minmax(0,.86fr);gap:16px;margin-top:18px;
    }
    .settingsLane{
      display:grid;gap:16px;min-width:0;
    }
    #settings-controls .settingsSectionGrid{
      grid-template-columns:1fr;
    }
    #settings-network .settingsSectionGrid,
    #settings-maintenance .settingsSectionGrid{
      grid-template-columns:1fr;
    }
    #settings-security .settingsSurface{
      display:grid;
      grid-template-columns:minmax(260px,320px) minmax(0,1fr);
      gap:18px;
      align-items:start;
    }
    #settings-security .settingsSurfaceHeader{
      grid-column:1 / -1;
    }
    #settings-pin{
      grid-column:1;
    }
    #settings-users{
      grid-column:1 / -1;
    }
    .settingsSurface{
      min-width:0;
      padding:18px;
      border-radius:22px;
      background:rgba(8,12,18,.34);
      border:1px solid rgba(255,255,255,.05);
      backdrop-filter:blur(6px);
    }
    .settingsSurfaceHeader{
      display:flex;justify-content:space-between;align-items:flex-start;gap:12px;flex-wrap:wrap;
    }
    .settingsSurfaceTitle{
      margin:0;font-size:20px;font-weight:800;letter-spacing:-.02em;
    }
    .settingsSurfaceLead{
      margin-top:6px;font-size:12px;line-height:1.5;color:var(--muted);max-width:58ch;
    }
    .settingsOverviewBlock{
      margin-top:18px;
      padding-top:18px;
      border-top:1px solid rgba(255,255,255,.07);
    }
    .settingsSurfaceStat{
      display:inline-flex;align-items:center;gap:8px;padding:7px 11px;border-radius:999px;
      border:1px solid rgba(255,255,255,.05);background:rgba(255,255,255,.03);
      color:#d8e0e8;font-size:12px;font-weight:700;
    }
    .settingsSurfaceBlock{
      padding-top:16px;
    }
    .settingsSurfaceBlock + .settingsSurfaceBlock{
      margin-top:16px;
      border-top:1px solid rgba(255,255,255,.07);
    }
    .settingsSurfaceBlock .settingsSubhead{
      margin-bottom:10px;
    }
    .settingsSurfaceBlock .settingsSubtitle{
      font-size:17px;
    }
    .settingsSurfaceBlock .statusList.compact{
      margin-top:10px;
    }
    .settingsNavSlim{
      background:
        radial-gradient(circle at top right, rgba(35,197,82,.12), transparent 34%),
        linear-gradient(180deg, #131d2b 0%, #101722 100%);
    }
    .settingsNavSlim .sectionHead{
      margin-bottom:10px;
    }
    .settingsNavSlim .hint{
      max-width:28ch;
    }
    .sectionHead{display:flex;justify-content:space-between;align-items:flex-start;gap:14px;flex-wrap:wrap}
    .sectionKicker{
      font-size:11px;font-weight:800;letter-spacing:.14em;text-transform:uppercase;color:#93a4b6;
    }
    .tabStatusBadge{
      display:inline-flex;align-items:center;justify-content:center;
      min-height:28px;
      margin-left:8px;
      padding:4px 10px;
      border-radius:999px;
      font-size:11px;
      font-weight:800;
      letter-spacing:.02em;
      vertical-align:middle;
    }
    .sectionLead{font-size:13px;color:var(--muted);max-width:52ch;line-height:1.5}
    .sectionMeta{
      display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;
      background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.05);font-size:12px;color:#d8e0e8;
    }
    .settingsCardSoft{background:linear-gradient(180deg, #111824 0%, #0f1621 100%)}
    .settingsCardFlat{background:#101722}
    .field{display:flex;flex-direction:column;gap:6px}
    .field + .field{margin-top:12px}
    label{font-size:12px;color:var(--muted)}
    input[type="range"]{width:100%}
    .switchRow{display:flex;align-items:center;justify-content:space-between;gap:14px}
    .switchText{display:flex;flex-direction:column;gap:4px}
    .switchState{font-size:12px;color:#d8e0e8}
    .switch{
      position:relative;display:inline-block;width:58px;height:32px;flex:0 0 auto;
      padding:0;border:1px solid var(--line);border-radius:999px;background:#16202E;cursor:pointer;
      transition:.18s ease;
    }
    .switchTrack{
      position:absolute;inset:0;border-radius:999px;transition:.18s ease;
    }
    .switchTrack::after{
      content:"";position:absolute;left:3px;top:3px;width:24px;height:24px;border-radius:50%;
      background:#d8e0e8;transition:.18s ease;
    }
    .switch.active{background:var(--accent);border-color:transparent}
    .switch.active .switchTrack::after{transform:translateX(26px);background:#fff}
    input[type="number"], input[type="text"], input[type="file"], select, textarea{
      width:100%;border:1px solid var(--line);background:var(--panel3);color:var(--txt);
      border-radius:12px;padding:10px 12px;outline:none;font-weight:700;
    }
    textarea{min-height:220px;resize:vertical;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px}
    .btnRow{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}
    .btn{
      appearance:none;border:1px solid var(--line);background:#172133;color:var(--txt);
      padding:10px 14px;border-radius:12px;font-weight:800;cursor:pointer;
    }
    .btn[disabled], .switch[disabled], input[disabled], select[disabled], textarea[disabled]{
      opacity:.5;cursor:not-allowed;
    }
    .btn.accent{background:var(--accent);border-color:transparent}
    .btn.danger{background:#341419;border-color:#4A161B}
    .btn.ghost{background:transparent}
    .choiceRow{display:flex;gap:10px;flex-wrap:wrap}
    .choice{flex:1 1 120px}
    .choice.active{background:var(--accent);color:#fff;border-color:transparent}
    .statusList{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:4px}
    .statusItem{
      background:var(--panel3);border:1px solid rgba(255,255,255,.03);border-radius:16px;padding:14px;
      min-width:0;
    }
    .statusItem .v{
      font-size:18px;
      line-height:1.2;
      margin-top:8px;
      font-weight:700;
      letter-spacing:-.01em;
    }
    .statusItem .v.metricOk{color:#7ee2a0}
    .statusItem .v.metricWarn{color:#f6c86b}
    .statusItem .v.metricBad{color:#ff8f8a}
    .healthBadge{
      display:inline-flex;align-items:center;justify-content:center;
      min-height:32px;padding:6px 12px;border-radius:999px;
      font-size:12px;font-weight:800;letter-spacing:.02em;
      border:1px solid transparent;
      width:fit-content;margin:10px auto 0;
    }
    .healthBadge.ok{background:rgba(35,197,82,.12);color:#7ee2a0;border-color:rgba(35,197,82,.2)}
    .healthBadge.bad{background:rgba(229,57,53,.12);color:#ff8f8a;border-color:rgba(229,57,53,.2)}
    .statusList.compact .statusItem{padding:12px 13px}
    .releaseCard{
      overflow:hidden;
      background:
        radial-gradient(circle at top right, rgba(122,30,44,.18), transparent 34%),
        linear-gradient(180deg, #121b27 0%, #101722 100%);
    }
    .releaseCard .statusList{
      grid-template-columns:1fr;
    }
    .releaseCard .statusItem .v,
    .releaseCard .hint{
      white-space:normal;
      overflow-wrap:anywhere;
      word-break:break-word;
    }
    .releaseCard .statusItem .v{
      font-size:15px;
      line-height:1.35;
      letter-spacing:0;
    }
    .releaseCard .btnRow{
      margin-top:14px;
    }
    #settings-updates .statusList{
      grid-template-columns:1fr;
    }
    #settings-updates .statusItem{
      min-width:0;
    }
    #settings-updates .statusItem .v,
    #settings-updates .hint{
      min-width:0;
      white-space:normal;
      overflow-wrap:anywhere;
      word-break:break-word;
    }
    #settings-updates .statusItem .v{
      font-size:15px;
      line-height:1.35;
      letter-spacing:0;
    }
    .settingsSplit{display:grid;grid-template-columns:1fr 1fr;gap:14px}
    .actionsCard .btnRow{margin-top:10px}
    .liveActionWrap{display:grid;gap:10px}
    .liveAction{
      width:100%;min-height:110px;border-radius:999px;border:0;
      background:#596270;color:#fff;font-size:42px;letter-spacing:.08em;
      box-shadow:0 10px 26px rgba(0,0,0,.24);
    }
    .liveAction.active{
      background:var(--red);
      box-shadow:0 0 0 3px rgba(229,57,53,.2), 0 18px 36px rgba(229,57,53,.28);
    }
    .liveActionMeta{
      display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;
      color:var(--muted);font-size:12px;
    }
    .liveActionState{
      font-weight:800;letter-spacing:.08em;text-transform:uppercase;color:#d8e0e8;
    }
    .hint,.toast,.footerLine{font-size:12px;color:var(--muted);line-height:1.45}
    .calHeader{
      display:flex;justify-content:space-between;align-items:flex-start;gap:14px;flex-wrap:wrap;
    }
    .calModeRow{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin-top:16px}
    .calModeSwitch{display:flex;gap:10px;flex-wrap:wrap}
    .calRows{display:flex;flex-direction:column;gap:10px;margin-top:16px}
    .calRow{
      display:grid;grid-template-columns:minmax(0,1fr) auto auto auto auto;gap:10px;align-items:center;
      background:var(--panel3);border-radius:18px;padding:12px 14px;
    }
    .calRow[hidden]{display:none}
    .calTitle{font-size:15px;font-weight:800}
    .badge{
      min-width:74px;height:34px;border-radius:12px;background:#16202E;
      display:grid;place-items:center;font-size:14px;font-weight:800;
    }
    .mono{font-variant-numeric:tabular-nums}
    .pill{
      display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;
      border:1px solid var(--line);background:var(--panel3);font-size:12px;color:var(--muted);
    }
    .pinBadge{
      padding:5px 10px;
      background:rgba(255,255,255,.03);
      border-color:rgba(255,255,255,.05);
      color:#93a4b6;
      font-size:11px;
      letter-spacing:.03em;
      text-transform:uppercase;
    }
    .pinBadge.active{
      background:rgba(122,30,44,.12);
      border-color:rgba(122,30,44,.22);
      color:#c8d3de;
    }
    body.authLocked{overflow:hidden}
    body.authLocked .shell,
    body.authLocked .top{
      filter:blur(10px);
      pointer-events:none;
      user-select:none;
    }
    .hidden{display:none !important}
    .authGate{
      position:fixed;inset:0;z-index:50;
      display:flex;align-items:center;justify-content:center;padding:20px;
      background:
        radial-gradient(circle at top, rgba(122,30,44,.22), transparent 30%),
        rgba(4,8,13,.9);
      backdrop-filter:blur(20px);
    }
    .authCard{
      width:min(100%, 460px);
      background:linear-gradient(180deg, #111824 0%, #0f1621 100%);
      border:1px solid var(--line);
      border-radius:28px;
      padding:22px;
      box-shadow:0 18px 48px rgba(0,0,0,.38);
    }
    .authTitle{font-size:28px;font-weight:800;margin:0}
    .authLead{font-size:13px;color:var(--muted);line-height:1.55;margin-top:8px}
    .authActions{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px}
    .authHintStrong{color:#d7e2eb;font-size:12px;line-height:1.5}
    .usersCardHeader{
      display:flex;justify-content:space-between;align-items:flex-start;gap:14px;flex-wrap:wrap;
    }
    .userList{display:flex;flex-direction:column;gap:10px;margin-top:16px}
    .userRow{
      display:grid;grid-template-columns:minmax(0,1fr) auto auto;gap:10px;align-items:center;
      background:var(--panel3);border-radius:16px;padding:12px 14px;
    }
    .debugConsole{
      margin-top:14px;
      border:1px solid var(--line);
      border-radius:18px;
      background:#09111A;
      overflow:hidden;
    }
    .debugConsoleMeta{
      display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;
      padding:10px 14px;
      border-bottom:1px solid rgba(255,255,255,.05);
      color:var(--muted);
      font-size:12px;
    }
    .debugConsoleBox{
      margin:0;
      padding:14px;
      min-height:220px;
      max-height:420px;
      overflow:auto;
      background:#050B11;
      color:#D7E2EB;
      font:12px/1.5 ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,monospace;
      white-space:pre-wrap;
      word-break:break-word;
    }
    .userRow > :first-child{min-width:0}
    .userRowName{
      font-size:15px;font-weight:800;
      overflow:hidden;text-overflow:ellipsis;white-space:nowrap;min-width:0;
    }
    .userRowMeta{
      font-size:12px;color:var(--muted);
      overflow-wrap:anywhere;word-break:break-word;
    }
    #settings-users{min-width:0}
    #settings-users .settingsSplit{grid-template-columns:minmax(0,1fr) minmax(0,1fr)}
    #settings-users .settingsSplit > div{min-width:0}
    #settings-users .btnRow .btn{min-width:0}
    #settings-users select,
    #settings-users input{min-width:0}
    @media (max-width:1024px){
      .gridOverview{grid-template-columns:1fr}
      .rightCol{grid-template-columns:1fr 1fr;grid-template-rows:none}
      .settingsPage{grid-template-columns:1fr;grid-template-areas:"rail" "main"}
      .settingsRail{position:static}
      .settingsJumpList{grid-template-columns:repeat(2,minmax(0,1fr))}
      .settingsMegaGrid{grid-template-columns:1fr}
      .settingsSectionGrid{grid-template-columns:1fr}
      #settings-security .settingsSurface{grid-template-columns:1fr}
      #settings-security .settingsSurfaceHeader,
      #settings-pin,
      #settings-users{grid-column:auto}
      .settingsSnapshotGrid{grid-template-columns:repeat(2,minmax(0,1fr))}
    }
    @media (max-width:760px){
      .topInner{grid-template-columns:1fr;justify-items:start}
      .tabs{justify-content:flex-start}
      .soundHero{grid-template-columns:1fr}
      .soundMetrics{grid-template-columns:1fr}
      .statusList{grid-template-columns:1fr}
      .settingsSplit{grid-template-columns:1fr}
      .settingsDeck{grid-template-columns:1fr}
      .settingsJumpList{grid-template-columns:1fr}
      .settingsColumn .settingsSubsection{padding:14px 14px 0}
      .settingsSection{padding:18px}
      .settingsSurface{padding:15px}
      .settingsSnapshotGrid{grid-template-columns:1fr}
      .settingsTopline{flex-direction:column}
      .settingsPillMetric{min-width:0;flex:1 1 140px}
      .cardSpan2{grid-column:auto}
      .grid2{grid-template-columns:1fr}
      .calRow{grid-template-columns:1fr 34px 74px 34px 110px}
      .userRow{grid-template-columns:1fr}
      .userRow .btn{width:100%}
      .clockHeroDate{position:static;margin-top:6px}
      .clockHeroRow{justify-content:flex-start;flex-wrap:wrap}
      .alertBadge{left:auto;right:18px;top:18px}
    }
    @media (max-width:700px){
      .title{font-size:18px}
      .chartWrap{height:180px}
      .histScale{width:52px;flex-basis:52px}
      .histBars{gap:1px}
    }
  </style>
</head>
<body>
  <div class="authGate hidden" id="authGate">
    <div class="authCard">
      <div class="sectionKicker">Acces Web</div>
      <h1 class="authTitle" id="authTitle">Connexion</h1>
      <div class="authLead" id="authLead">Connecte-toi avec un compte web local pour acceder au dashboard.</div>

      <div class="field">
        <label>Login</label>
        <input id="authUsername" type="text" autocomplete="username" placeholder="admin"/>
      </div>
      <div class="field">
        <label>Mot de passe</label>
        <input id="authPassword" type="password" autocomplete="current-password" placeholder="Mot de passe"/>
      </div>
      <div class="authHintStrong" id="authPasswordHint">Mot de passe conseille: 10+ caracteres avec 3 types parmi majuscule, minuscule, chiffre, symbole.</div>
      <div class="authActions">
        <button class="btn accent" id="authSubmitBtn">Se connecter</button>
      </div>
      <div class="toast" id="authToast"></div>
    </div>
  </div>
  <div class="top">
    <div class="topInner">
      <div class="brand">
        <div class="title">SoundPanel 7</div>
        <div class="subtitle">Sonometre & Horloge connecte</div>
      </div>
      <div class="tabs">
        <button class="tab active" data-page="overview">Principal</button>
        <button class="tab" data-page="clock">Horloge</button>
        <button class="tab" data-page="sound">Sonometre</button>
        <button class="tab" data-page="calibration">Calibration</button>
        <button class="tab" data-page="settings">Parametres</button>
        <span class="healthBadge bad tabStatusBadge" id="topSystemBadge">--</span>
      </div>
    </div>
  </div>

  <main class="shell">
    <div class="pages">
      <section class="page active" data-page="overview">
        <div class="stack">
          <div class="gridOverview">
            <article class="card clockCard">
              <div class="clockMiniDate mono" id="overviewClockDate">--/--/----</div>
              <div class="clockMiniMain mono" id="overviewClockMain">--:--</div>
              <div class="clockMiniSec mono" id="overviewClockSec">:--</div>
            </article>

            <article class="card soundCard" id="overviewSoundCard">
              <div class="gaugeWrap">
                <div class="gauge large" id="overviewGauge" style="--pct:0;--gaugeColor:var(--green);">
                  <div class="gaugeInner">
                    <div class="db large mono" id="overviewDb">--.-</div>
                    <div class="unit">dB</div>
                    <div class="dot" id="overviewDot"></div>
                  </div>
                </div>
              </div>
            </article>

            <div class="rightCol">
              <article class="card">
                <div class="k">Leq</div>
                <div class="v mono" id="overviewLeq">--.-</div>
              </article>
              <article class="card">
                <div class="k">Peak</div>
                <div class="v mono" id="overviewPeak">--.-</div>
              </article>
            </div>
          </div>

          <article class="card histCard">
            <div class="histTop">
              <div class="histMeta" id="overviewHistMeta">Historique 5 min</div>
              <div class="histMeta center" id="overviewAlertTime">Rouge 0 s / 5m</div>
              <div class="histMeta right"></div>
            </div>
            <div class="chartWrap">
              <div class="chartPlot">
                <div class="histBars" id="overviewHistBars"></div>
              </div>
              <div class="histScale">
                <div class="histScaleTopTick"></div>
                <div class="histScaleTopLabel">130 dB</div>
                <div class="histScaleRow mid" style="bottom:68.4%">
                  <span class="histScaleTick"></span><span class="histScaleLabel">100 dB</span>
                </div>
                <div class="histScaleRow mid" style="bottom:36.8%">
                  <span class="histScaleTick"></span><span class="histScaleLabel">70 dB</span>
                </div>
                <div class="histScaleRow bottom35" style="bottom:0%">
                  <span class="histScaleTick"></span><span class="histScaleLabel">35 dB</span>
                </div>
              </div>
            </div>
            <div class="histAxis">
              <div id="overviewHistLeft">-5m</div>
              <div class="histAxisMid" id="overviewHistMid">-2m</div>
              <div id="overviewHistRight">0</div>
            </div>
          </article>
        </div>
      </section>

      <section class="page" data-page="clock">
        <article class="card clockHero">
          <div class="clockRule"></div>
          <div class="clockDate clockHeroDate mono" id="clockDate">--/--/----</div>
          <div class="clockHeroRow">
            <div class="clockHeroMain mono" id="clockMain">--:--</div>
            <div class="clockHeroSec mono" id="clockSec">--</div>
          </div>
        </article>
      </section>

      <section class="page" data-page="sound">
        <div class="stack">
          <article class="card soundCard" id="soundCard">
            <div class="alertBadge" id="soundAlertBadge">ALERTE</div>
            <div class="soundHero">
              <div class="gaugeWrap">
                <div class="gauge large" id="soundGauge" style="--pct:0;--gaugeColor:var(--green);">
                  <div class="gaugeInner">
                    <div class="db large mono" id="soundDb">--.-</div>
                    <div class="unit">dB</div>
                    <div class="dot" id="soundDot"></div>
                  </div>
                </div>
              </div>
              <div class="soundMetrics">
                <article class="metricCard">
                  <div class="k">Leq</div>
                  <div class="v mono" id="soundLeq">--.-</div>
                </article>
                <article class="metricCard">
                  <div class="k">Peak</div>
                  <div class="v mono" id="soundPeak">--.-</div>
                </article>
              </div>
            </div>
          </article>

          <article class="card histCard">
            <div class="histTop">
              <div class="histMeta" id="soundHistMeta">Historique 5 min</div>
              <div class="histMeta center" id="soundAlertTime">Rouge 0 s / 5m</div>
              <div class="histMeta right"></div>
            </div>
            <div class="chartWrap">
              <div class="chartPlot">
                <div class="histBars" id="soundHistBars"></div>
              </div>
              <div class="histScale">
                <div class="histScaleTopTick"></div>
                <div class="histScaleTopLabel">130 dB</div>
                <div class="histScaleRow mid" style="bottom:68.4%">
                  <span class="histScaleTick"></span><span class="histScaleLabel">100 dB</span>
                </div>
                <div class="histScaleRow mid" style="bottom:36.8%">
                  <span class="histScaleTick"></span><span class="histScaleLabel">70 dB</span>
                </div>
                <div class="histScaleRow bottom35" style="bottom:0%">
                  <span class="histScaleTick"></span><span class="histScaleLabel">35 dB</span>
                </div>
              </div>
            </div>
            <div class="histAxis">
              <div id="soundHistLeft">-5m</div>
              <div class="histAxisMid" id="soundHistMid">-2m</div>
              <div id="soundHistRight">0</div>
            </div>
          </article>
        </div>
      </section>

      <section class="page" data-page="calibration">
        <article class="card">
          <div class="calHeader">
            <div>
              <h2 class="sectionTitle">Calibration</h2>
              <div class="hint" id="calLiveMic">Micro live: --</div>
              <div class="hint" id="calLiveLog">Log calibration live: --</div>
            </div>
            <div class="pill mono" id="calStatus">0 / 3 points valides</div>
          </div>

          <div class="calModeRow">
            <div>
              <div class="calTitle">Mode de calibration</div>
              <div class="hint">3 points pour une calibration rapide, 5 points pour une courbe plus fine.</div>
            </div>
            <div class="calModeSwitch">
              <button class="btn choice active" id="calMode3" data-cal-mode="3">3 points</button>
              <button class="btn choice" id="calMode5" data-cal-mode="5">5 points</button>
            </div>
          </div>

          <div class="calRows">
            <div class="calRow">
              <div>
                <div class="calTitle">Point 1</div>
                <div class="hint mono" id="calState1">Point 1 non capture</div>
              </div>
              <button class="btn" data-cal-adjust="0:-5">-</button>
              <div class="badge mono" id="calRef1">40</div>
              <button class="btn" data-cal-adjust="0:5">+</button>
              <button class="btn accent" data-cal-capture="0">Capturer</button>
            </div>

            <div class="calRow">
              <div>
                <div class="calTitle">Point 2</div>
                <div class="hint mono" id="calState2">Point 2 non capture</div>
              </div>
              <button class="btn" data-cal-adjust="1:-5">-</button>
              <div class="badge mono" id="calRef2">65</div>
              <button class="btn" data-cal-adjust="1:5">+</button>
              <button class="btn accent" data-cal-capture="1">Capturer</button>
            </div>

            <div class="calRow">
              <div>
                <div class="calTitle">Point 3</div>
                <div class="hint mono" id="calState3">Point 3 non capture</div>
              </div>
              <button class="btn" data-cal-adjust="2:-5">-</button>
              <div class="badge mono" id="calRef3">85</div>
              <button class="btn" data-cal-adjust="2:5">+</button>
              <button class="btn accent" data-cal-capture="2">Capturer</button>
            </div>

            <div class="calRow" hidden>
              <div>
                <div class="calTitle">Point 4</div>
                <div class="hint mono" id="calState4">Point 4 non capture</div>
              </div>
              <button class="btn" data-cal-adjust="3:-5">-</button>
              <div class="badge mono" id="calRef4">85</div>
              <button class="btn" data-cal-adjust="3:5">+</button>
              <button class="btn accent" data-cal-capture="3">Capturer</button>
            </div>

            <div class="calRow" hidden>
              <div>
                <div class="calTitle">Point 5</div>
                <div class="hint mono" id="calState5">Point 5 non capture</div>
              </div>
              <button class="btn" data-cal-adjust="4:-5">-</button>
              <div class="badge mono" id="calRef5">100</div>
              <button class="btn" data-cal-adjust="4:5">+</button>
              <button class="btn accent" data-cal-capture="4">Capturer</button>
            </div>
          </div>

          <div class="btnRow">
            <button class="btn danger" id="clearCalibration">Effacer calibration</button>
          </div>
          <div class="toast" id="calToast"></div>
        </article>
      </section>

      <section class="page" data-page="settings">
        <div class="settingsPage">
          <div class="settingsMain">
            <article class="card settingsHero settingsHeroPrime" id="settings-overview">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Vue Systeme</div>
                  <h2 class="sectionTitle">Parametres, sans patchwork</h2>
                </div>
              </div>
              <div class="settingsTopline">
                <div class="settingsPillStrip">
                  <div class="settingsPillMetric">
                    <div class="k">Etat systeme</div>
                    <div class="v"><span class="healthBadge bad" id="settingsSystemBadge">--</span></div>
                  </div>
                  <div class="settingsPillMetric">
                    <div class="k">Version</div>
                    <div class="v mono" id="settingsVersion">--</div>
                  </div>
                  <div class="settingsPillMetric">
                    <div class="k">IP / RSSI</div>
                    <div class="v mono" id="settingsIp">--</div>
                  </div>
                  <div class="settingsPillMetric">
                    <div class="k">Heure</div>
                    <div class="v mono" id="settingsTime">--</div>
                  </div>
                </div>
              </div>
              <div class="settingsSnapshotGrid">
                <div class="settingsSnapshot">
                  <div class="k">Uptime</div>
                  <div class="v mono" id="settingsUptime">--</div>
                </div>
                <div class="settingsSnapshot">
                  <div class="k">Historique</div>
                  <div class="v mono" id="settingsHistory">--</div>
                </div>
                <div class="settingsSnapshot">
                  <div class="k">Date de build</div>
                  <div class="v mono" id="settingsBuildDate">--</div>
                </div>
                <div class="settingsSnapshot">
                  <div class="k">Env compile</div>
                  <div class="v mono" id="settingsBuildEnv">--</div>
                </div>
                <div class="settingsSnapshot">
                  <div class="k">Temp MCU</div>
                  <div class="v mono" id="settingsMcuTemp">--</div>
                </div>
                <div class="settingsSnapshot">
                  <div class="k">Charge CPU</div>
                  <div class="v mono" id="settingsCpu">--</div>
                </div>
                <div class="settingsSnapshot">
                  <div class="k">Etat OTA / MQTT</div>
                  <div class="v mono" id="settingsOtaMqtt">--</div>
                </div>
                <div class="settingsSnapshot">
                  <div class="k">Ecran actif</div>
                  <div class="v mono" id="settingsActivePage">--</div>
                </div>
                <div class="settingsSnapshot">
                  <div class="k">LVGL load / idle</div>
                  <div class="v mono" id="settingsLvglLoad">--</div>
                </div>
                <div class="settingsSnapshot">
                  <div class="k">UI / handler</div>
                  <div class="v mono" id="settingsLvglTiming">--</div>
                </div>
                <div class="settingsSnapshot">
                <div class="k">Heap / disque</div>
                <div class="v mono" id="settingsLvglHeap">--</div>
              </div>
              </div>
              <div class="settingsOverviewBlock">
                <div class="settingsSubhead">
                  <div>
                    <h3 class="settingsSubtitle">Audio brut</h3>
                  </div>
                </div>
                <div class="statusList compact">
                  <div class="statusItem">
                    <div class="k">rawRms</div>
                    <div class="v mono" id="rawRmsAdv">--</div>
                  </div>
                  <div class="statusItem">
                    <div class="k">pseudo dB</div>
                    <div class="v mono" id="rawPseudoDbAdv">--</div>
                  </div>
                  <div class="statusItem">
                    <div class="k">ADC Mean</div>
                    <div class="v mono" id="rawAdcMeanAdv">--</div>
                  </div>
                  <div class="statusItem">
                    <div class="k">ADC Last</div>
                    <div class="v mono" id="rawAdcLastAdv">--</div>
                  </div>
                </div>
              </div>
            </article>

            <article class="card settingsSection settingsSectionWarm" id="settings-controls">
              <div class="settingsSectionHeader">
                <div>
                  <div class="sectionKicker">Exploitation</div>
                  <h3 class="settingsGroupTitle">Faire vivre le panneau au quotidien</h3>
                </div>
                <div class="settingsSectionStamp">Pilotage quotidien</div>
              </div>
              <div class="settingsSectionGrid">
                <div class="settingsLane">
                  <section class="settingsSurface">
                    <div class="settingsSurfaceHeader">
                      <div>
                        <h3 class="settingsSurfaceTitle">Mesure & affichage</h3>
                        <div class="settingsSurfaceLead">Les reglages qui changent directement la perception du sonometre et l'experience sur l'ecran tactile.</div>
                      </div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-ui">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Seuils, historique et reponse</h3>
                          <div class="settingsSublead">La zone qui regle le comportement du sonometre avant toute integration externe.</div>
                        </div>
                      </div>
                      <div class="field">
                        <label>Seuil vert max <span class="mono" id="gVal">--</span>dB</label>
                        <input id="g" type="range" min="0" max="100" value="55"/>
                      </div>
                      <div class="field">
                        <label>Seuil orange max <span class="mono" id="oVal">--</span>dB</label>
                        <input id="o" type="range" min="0" max="100" value="70"/>
                      </div>
                      <div class="field">
                        <label>Historique <span class="mono" id="hVal">--</span> min</label>
                        <input id="hist" type="range" min="1" max="60" value="5"/>
                      </div>
                      <div class="settingsSplit">
                        <div class="field">
                          <label>Reponse sonometre</label>
                          <div class="choiceRow">
                            <button class="btn choice" id="modeFast" data-mode="0">Fast</button>
                            <button class="btn choice" id="modeSlow" data-mode="1">Slow</button>
                          </div>
                          <div class="hint">Fast reste reactif. Slow stabilise l'affichage.</div>
                          <div class="field">
                            <label>Micro</label>
                            <select id="audioSourceSelect">
                              <option value="0">Demo</option>
                              <option value="1">Analog Mic</option>
                              <option value="2">PDM MEMS</option>
                              <option value="3">INMP441</option>
                            </select>
                            <div class="hint" id="audioSourceHint">Choisis le type de micro cable sur la carte.</div>
                          </div>
                        </div>
                        <div>
                          <div class="field">
                            <label>Delai warning (orange)</label>
                            <input id="warnHoldSec" type="number" min="0" max="60" step="1" value="3"/>
                          </div>
                          <div class="field">
                            <label>Delai critique (rouge)</label>
                            <input id="critHoldSec" type="number" min="0" max="60" step="1" value="2"/>
                          </div>
                          <div class="field">
                            <label>Delai tampon calibration (s)</label>
                            <input id="calCaptureSec" type="number" min="1" max="30" step="1" value="3"/>
                            <div class="hint">Duree de capture utilisee pour chaque point de calibration.</div>
                          </div>
                        </div>
                      </div>
                      <div class="btnRow">
                        <button class="btn accent" id="saveUi">Sauver UI</button>
                      </div>
                      <div class="toast" id="uiToast"></div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-display">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Ecran tactile & diffusion LIVE</h3>
                        </div>
                      </div>
                      <div class="settingsSplit">
                        <div>
                          <div class="field">
                            <div class="switchRow">
                              <div class="switchText">
                                <label>Backlight</label>
                                <div class="switchState mono" id="blVal">--</div>
                              </div>
                              <button class="switch" id="bl" type="button" aria-label="Activer ou couper le backlight" aria-pressed="true">
                                <span class="switchTrack"></span>
                              </button>
                            </div>
                          </div>
                          <div class="field">
                            <label>Vue a afficher</label>
                            <select id="dashboardPageAdv">
                              <option value="0">Principal</option>
                              <option value="1">Horloge</option>
                              <option value="2">LIVE</option>
                              <option value="3">Sonometre</option>
                            </select>
                            <div class="hint">Change immediatement l'ecran affiche sur le panneau tactile.</div>
                          </div>
                          <div class="field">
                            <div class="switchRow">
                              <div class="switchText">
                                <label>Tactile actif</label>
                                <div class="switchState mono" id="touchEnabledAdvVal">--</div>
                              </div>
                              <button class="switch" id="touchEnabledAdv" type="button" aria-label="Activer ou desactiver l'ecran tactile" aria-pressed="true">
                                <span class="switchTrack"></span>
                                <span class="switchThumb"></span>
                              </button>
                            </div>
                            <div class="hint">Coupe toutes les interactions tactiles du panneau 7 pouces jusqu'a reactivation depuis l'interface web.</div>
                          </div>
                          <div class="field">
                            <label>Mode plein ecran tactile</label>
                            <div class="choiceRow">
                              <button class="btn choice" id="dashboardFsOverview" data-dashboard-fullscreen="0" type="button" aria-pressed="false">Principal</button>
                              <button class="btn choice" id="dashboardFsClock" data-dashboard-fullscreen="1" type="button" aria-pressed="false">Horloge</button>
                              <button class="btn choice" id="dashboardFsLive" data-dashboard-fullscreen="2" type="button" aria-pressed="false">LIVE</button>
                              <button class="btn choice" id="dashboardFsSound" data-dashboard-fullscreen="3" type="button" aria-pressed="false">Sonometre</button>
                            </div>
                            <div class="hint">Masque la barre du haut uniquement sur l'ecran tactile 7 pouces pour les vues selectionnees.</div>
                          </div>
                        </div>
                        <div>
                          <div class="liveActionWrap">
                            <button class="btn liveAction" id="liveActionBtn" aria-pressed="false">LIVE</button>
                            <div class="liveActionMeta">
                              <div>Flag broadcast tactile, API et MQTT</div>
                              <div class="liveActionState" id="liveActionState">OFF AIR</div>
                            </div>
                          </div>
                        </div>
                      </div>
                      <div class="btnRow">
                        <button class="btn accent" id="saveDashboardPageAdv">Appliquer sur l'ecran</button>
                      </div>
                      <div class="toast" id="dashboardPageToast"></div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-tardis">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Mode TARDIS</h3>
                          <div class="settingsSublead">Carte reservee au build headless pour piloter les LED de cabine sans toucher aux GPIO audio, avec la LED RGB integree pour l'interieur.</div>
                        </div>
                        <div class="pill mono" id="tardisPinsBadge">GPIO -- / --</div>
                      </div>
                      <div class="settingsSplit">
                        <div>
                          <div class="field">
                            <div class="switchRow">
                              <div class="switchText">
                                <label>Mode TARDIS</label>
                                <div class="switchState mono" id="tardisModeEnabledVal">--</div>
                              </div>
                              <button class="switch" id="tardisModeEnabled" type="button" aria-label="Activer ou desactiver le mode TARDIS" aria-pressed="false">
                                <span class="switchTrack"></span>
                                <span class="switchThumb"></span>
                              </button>
                            </div>
                          </div>
                          <div class="field">
                            <div class="switchRow">
                              <div class="switchText">
                                <label>LED cabine interieur</label>
                                <div class="switchState mono" id="tardisInteriorLedEnabledVal">--</div>
                              </div>
                              <button class="switch" id="tardisInteriorLedEnabled" type="button" aria-label="Activer ou desactiver la LED interieur" aria-pressed="false">
                                <span class="switchTrack"></span>
                                <span class="switchThumb"></span>
                              </button>
                            </div>
                          </div>
                          <div class="field" id="tardisInteriorRgbModeField">
                            <label for="tardisInteriorRgbMode">Mode LED interieur</label>
                            <select id="tardisInteriorRgbMode">
                              <option value="0">Alerte sonore</option>
                              <option value="1">Couleur fixe</option>
                              <option value="2">Decollage TARDIS mecanique</option>
                              <option value="3">Decollage TARDIS sinusoidal</option>
                            </select>
                            <div class="hint">Alerte: vert, orange ou rouge selon le niveau du sonometre. Mecanique: rampe bleue reguliere avec plateaux courts. Sinusoidal: pulsation plus douce, mais strictement repetitive.</div>
                          </div>
                          <div class="field" id="tardisInteriorRgbColorField">
                            <label for="tardisInteriorRgbColor">Couleur fixe</label>
                            <input id="tardisInteriorRgbColor" type="color" value="#2D9CDB">
                          </div>
                          <div class="field">
                            <div class="switchRow">
                              <div class="switchText">
                                <label>LED exterieur</label>
                                <div class="switchState mono" id="tardisExteriorLedEnabledVal">--</div>
                              </div>
                              <button class="switch" id="tardisExteriorLedEnabled" type="button" aria-label="Activer ou desactiver la LED exterieur" aria-pressed="false">
                                <span class="switchTrack"></span>
                                <span class="switchThumb"></span>
                              </button>
                            </div>
                          </div>
                        </div>
                        <div>
                          <div class="hint" id="tardisPinsHint">GPIO interieur -- / exterieur --. Broches dediees pour rester hors des pins micro 4, 11, 12 et 13.</div>
                        </div>
                      </div>
                      <div class="btnRow">
                        <button class="btn accent" id="saveTardisAdv">Appliquer le mode TARDIS</button>
                      </div>
                      <div class="toast" id="tardisToast"></div>
                    </div>
                  </section>
                </div>

                <div class="settingsLane" id="settings-security">
                  <section class="settingsSurface">
                    <div class="settingsSurfaceHeader">
                      <div>
                        <h3 class="settingsSurfaceTitle">Acces & securite</h3>
                      </div>
                      <div class="settingsSurfaceStat mono" id="usersSummary">0 / 4 utilisateurs</div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-pin">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Verrou PIN</h3>
                          <div class="settingsSublead">Protege Calibration et Parametres sur le tactile et sur l'interface web.</div>
                        </div>
                        <div class="pill pinBadge mono" id="pinStatusAdv">PIN tactile: --</div>
                      </div>
                      <div class="field">
                        <label>Code PIN numerique</label>
                        <input id="accessPinAdv" type="password" inputmode="numeric" pattern="[0-9]*" maxlength="8" placeholder="4 a 8 chiffres"/>
                      </div>
                      <div class="btnRow">
                        <button class="btn accent" id="savePinAdv">Sauver PIN</button>
                        <button class="btn" id="clearPinAdv">Desactiver PIN</button>
                      </div>
                      <div class="toast" id="toastPinAdv"></div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-users">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Comptes web</h3>
                        </div>
                      </div>
                      <div class="settingsSplit">
                        <div>
                          <div class="field">
                            <label>Nouveau login</label>
                            <input id="newWebUsername" type="text" autocomplete="off" placeholder="admin"/>
                          </div>
                          <div class="field">
                            <label>Mot de passe</label>
                            <input id="newWebPassword" type="password" autocomplete="new-password" placeholder="Creer un mot de passe robuste"/>
                          </div>
                          <div class="btnRow">
                            <button class="btn accent" id="createWebUserBtn">Creer utilisateur</button>
                            <button class="btn" id="logoutBtn">Deconnexion</button>
                          </div>
                        </div>
                        <div>
                          <div class="field">
                            <label>Utilisateur a mettre a jour</label>
                            <select id="manageWebUserSelect"></select>
                          </div>
                          <div class="field">
                            <label>Nouveau mot de passe</label>
                            <input id="manageWebPassword" type="password" autocomplete="new-password" placeholder="Nouveau mot de passe"/>
                          </div>
                          <div class="btnRow">
                            <button class="btn" id="updateWebPasswordBtn">Changer mot de passe</button>
                            <button class="btn danger" id="deleteWebUserBtn">Supprimer utilisateur</button>
                          </div>
                        </div>
                      </div>
                      <div class="userList" id="usersList"></div>
                      <div class="toast" id="usersToast"></div>
                    </div>
                  </section>
                </div>
              </div>
            </article>

            <article class="card settingsSection settingsSectionCool" id="settings-network">
              <div class="settingsSectionHeader">
                <div>
                  <div class="sectionKicker">Connexion & alertes</div>
                  <h3 class="settingsGroupTitle">Relier le panneau au reste du monde</h3>
                </div>
                <div class="settingsSectionStamp">Connectivite vivante</div>
              </div>
              <div class="settingsSectionGrid">
                <div class="settingsLane">
                  <section class="settingsSurface">
                    <div class="settingsSurfaceBlock" id="settings-time">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Heure, NTP & timezone</h3>
                        </div>
                      </div>
                      <div class="settingsSplit">
                        <div>
                          <div class="field">
                            <label>NTP Server</label>
                            <input id="ntpServerAdv" type="text" placeholder="fr.pool.ntp.org"/>
                          </div>
                          <div class="field">
                            <label>Sync NTP (minutes)</label>
                            <input id="ntpSyncMinAdv" type="number" min="1" max="1440" step="1" value="180"/>
                          </div>
                        </div>
                        <div>
                          <div class="field">
                            <label>TZ (POSIX)</label>
                            <input id="tzAdv" type="text" placeholder="CET-1CEST,M3.5.0/2,M10.5.0/3"/>
                          </div>
                          <div class="field">
                            <label>Hostname</label>
                            <input id="hostnameAdv" type="text" placeholder="soundpanel7"/>
                          </div>
                        </div>
                      </div>
                      <div class="btnRow">
                        <button class="btn accent" id="saveTimeAdv">Sauver heure</button>
                      </div>
                      <div class="toast" id="toastTimeAdv"></div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-wifi">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Wi-Fi multi-AP</h3>
                          <div class="settingsSublead mono" id="wifiSummaryAdv">Connexion: --</div>
                        </div>
                      </div>
                      <div class="settingsSplit">
                        <div>
                          <div class="field">
                            <label>Reseau 1 SSID</label>
                            <input id="wifiSsid1" type="text" maxlength="32" placeholder="Maison"/>
                          </div>
                          <div class="field">
                            <label>Reseau 1 mot de passe</label>
                            <input id="wifiPassword1" type="password" maxlength="64" placeholder="laisser vide = reseau ouvert"/>
                          </div>
                          <div class="field">
                            <label>Reseau 2 SSID</label>
                            <input id="wifiSsid2" type="text" maxlength="32" placeholder="Maison-5G"/>
                          </div>
                          <div class="field">
                            <label>Reseau 2 mot de passe</label>
                            <input id="wifiPassword2" type="password" maxlength="64" placeholder="laisser vide = reseau ouvert"/>
                          </div>
                        </div>
                        <div>
                          <div class="field">
                            <label>Reseau 3 SSID</label>
                            <input id="wifiSsid3" type="text" maxlength="32" placeholder="Partage smartphone"/>
                          </div>
                          <div class="field">
                            <label>Reseau 3 mot de passe</label>
                            <input id="wifiPassword3" type="password" maxlength="64" placeholder="laisser vide = reseau ouvert"/>
                          </div>
                          <div class="field">
                            <label>Reseau 4 SSID</label>
                            <input id="wifiSsid4" type="text" maxlength="32" placeholder="Bureau"/>
                          </div>
                          <div class="field">
                            <label>Reseau 4 mot de passe</label>
                            <input id="wifiPassword4" type="password" maxlength="64" placeholder="laisser vide = reseau ouvert"/>
                          </div>
                        </div>
                      </div>
                      <div class="hint">Laisse le mot de passe vide pour un reseau ouvert. Si tu ne modifies pas un champ mot de passe deja configure, il est conserve.</div>
                      <div class="btnRow">
                        <button class="btn accent" id="saveWifiAdv">Sauver Wi-Fi</button>
                      </div>
                      <div class="toast" id="toastWifiAdv"></div>
                    </div>
                  </section>
                </div>

                <div class="settingsLane">
                  <section class="settingsSurface">
                    <div class="settingsSurfaceHeader">
                      <div>
                        <h3 class="settingsSurfaceTitle">Integrations & alertes</h3>
                      </div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-homeassistant">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Home Assistant</h3>
                          <div class="settingsSublead">Token dedie pour l'integration HTTP native.</div>
                        </div>
                      </div>
                      <div class="field">
                        <label>Bearer token Home Assistant</label>
                        <input id="homeAssistantTokenAdv" type="text" autocomplete="off" maxlength="64" placeholder="genere ou colle un token"/>
                      </div>
                      <div class="hint mono" id="homeAssistantStatusAdv">Token: --</div>
                      <div class="hint mono" id="homeAssistantPathAdv">Endpoint: /api/ha/status</div>
                      <div class="btnRow">
                        <button class="btn accent" id="saveHomeAssistantAdv">Sauver token</button>
                        <button class="btn" id="generateHomeAssistantTokenBtn">Generer</button>
                        <button class="btn danger" id="clearHomeAssistantTokenBtn">Revoquer</button>
                      </div>
                      <div class="toast" id="toastHomeAssistantAdv"></div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-mqtt">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">MQTT</h3>
                        </div>
                      </div>
                      <div class="settingsSplit">
                        <div>
                          <div class="field">
                            <label>Activer MQTT</label>
                            <select id="mqttEnabledAdv">
                              <option value="1">Oui</option>
                              <option value="0">Non</option>
                            </select>
                          </div>
                          <div class="field">
                            <label>Broker / Host</label>
                            <input id="mqttHostAdv" type="text" placeholder="192.168.1.10"/>
                          </div>
                          <div class="field">
                            <label>Port</label>
                            <input id="mqttPortAdv" type="number" min="1" max="65535" placeholder="1883"/>
                          </div>
                          <div class="field">
                            <label>Username</label>
                            <input id="mqttUsernameAdv" type="text" placeholder="laisser vide si non utilise"/>
                          </div>
                          <div class="field">
                            <label>Password</label>
                            <input id="mqttPasswordAdv" type="password" placeholder="laisser vide si non utilise"/>
                          </div>
                        </div>
                        <div>
                          <div class="field">
                            <label>Client ID</label>
                            <input id="mqttClientIdAdv" type="text" placeholder="soundpanel7"/>
                          </div>
                          <div class="field">
                            <label>Base topic</label>
                            <input id="mqttBaseTopicAdv" type="text" placeholder="soundpanel7"/>
                          </div>
                          <div class="field">
                            <label>Publish period (ms)</label>
                            <input id="mqttPublishPeriodMsAdv" type="number" min="250" max="60000" placeholder="1000"/>
                          </div>
                          <div class="field">
                            <label>Retain</label>
                            <select id="mqttRetainAdv">
                              <option value="0">Non</option>
                              <option value="1">Oui</option>
                            </select>
                          </div>
                        </div>
                      </div>
                      <div class="btnRow">
                        <button class="btn accent" id="saveMqttAdv">Sauver MQTT</button>
                      </div>
                      <div class="toast" id="toastMqttAdv"></div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-notifications">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Slack, Telegram & WhatsApp</h3>
                          <div class="settingsSublead">Les differents cannaux d'alerting</div>
                        </div>
                      </div>
                      <div class="settingsSplit">
                        <div>
                          <div class="field">
                            <label>Notifier des le niveau warning</label>
                            <select id="notifyWarningAdv">
                              <option value="0">Non, critique seulement</option>
                              <option value="1">Oui, warning + critique</option>
                            </select>
                          </div>
                          <div class="field">
                            <label>Notifier le retour a la normale</label>
                            <select id="notifyRecoveryAdv">
                              <option value="1">Oui</option>
                              <option value="0">Non</option>
                            </select>
                          </div>
                          <div class="field">
                            <label>Etat courant</label>
                            <div class="pill mono" id="notificationsCurrentStateAdv">--</div>
                          </div>
                        </div>
                        <div>
                          <div class="field">
                            <label>Cibles actives</label>
                            <div class="hint mono" id="notificationsSummaryAdv">--</div>
                          </div>
                          <div class="field">
                            <label>Dernier resultat</label>
                            <div class="hint mono" id="notificationsLastResultAdv">Aucun envoi.</div>
                          </div>
                          <div class="field">
                            <label>Dernier succes</label>
                            <div class="hint mono" id="notificationsLastSuccessAdv">--</div>
                          </div>
                        </div>
                      </div>
                      <div class="settingsSplit">
                        <div>
                          <div class="field">
                            <label>Slack actif</label>
                            <select id="slackEnabledAdv">
                              <option value="0">Non</option>
                              <option value="1">Oui</option>
                            </select>
                          </div>
                          <div class="field">
                            <label>Slack incoming webhook</label>
                            <input id="slackWebhookUrlAdv" type="password" placeholder="https://hooks.slack.com/services/..."/>
                            <div class="hint">Laisse vide pour conserver l'URL deja stockee.</div>
                          </div>
                          <div class="field">
                            <label>Room / channel Slack</label>
                            <input id="slackChannelAdv" type="text" placeholder="#jeedom"/>
                            <div class="hint">Option legacy. Les webhooks Slack modernes gardent souvent le canal defini cote Slack.</div>
                          </div>
                        </div>
                        <div>
                          <div class="field">
                            <label>Telegram actif</label>
                            <select id="telegramEnabledAdv">
                              <option value="0">Non</option>
                              <option value="1">Oui</option>
                            </select>
                          </div>
                          <div class="field">
                            <label>Telegram chat ID</label>
                            <input id="telegramChatIdAdv" type="text" placeholder="-1001234567890"/>
                          </div>
                          <div class="field">
                            <label>Telegram bot token</label>
                            <input id="telegramBotTokenAdv" type="password" placeholder="123456:ABCDEF..."/>
                          </div>
                        </div>
                      </div>
                      <div class="settingsSplit">
                        <div>
                          <div class="field">
                            <label>WhatsApp actif</label>
                            <select id="whatsappEnabledAdv">
                              <option value="0">Non</option>
                              <option value="1">Oui</option>
                            </select>
                          </div>
                          <div class="field">
                            <label>Meta Graph API version</label>
                            <input id="whatsappApiVersionAdv" type="text" placeholder="v22.0"/>
                          </div>
                          <div class="field">
                            <label>Phone Number ID</label>
                            <input id="whatsappPhoneIdAdv" type="text" placeholder="123456789012345"/>
                          </div>
                        </div>
                        <div>
                          <div class="field">
                            <label>Destinataire WhatsApp</label>
                            <input id="whatsappRecipientAdv" type="text" placeholder="33612345678"/>
                          </div>
                          <div class="field">
                            <label>Access token Meta</label>
                            <input id="whatsappAccessTokenAdv" type="password" placeholder="EAA..."/>
                            <div class="hint">Selon la politique Meta, certains envois WhatsApp peuvent necessiter des templates approuves.</div>
                          </div>
                        </div>
                      </div>
                      <div class="btnRow">
                        <button class="btn accent" id="saveNotificationsAdv">Sauver alertes</button>
                        <button class="btn" id="testNotificationsAdv">Envoyer un test</button>
                      </div>
                      <div class="toast" id="toastNotificationsAdv"></div>
                    </div>
                  </section>
                </div>
              </div>
            </article>

            <article class="card settingsSection settingsSectionNeutral" id="settings-maintenance">
              <div class="settingsSectionHeader">
                <div>
                  <div class="sectionKicker">Maintenance</div>
                  <h3 class="settingsGroupTitle">Mettre a jour, sauvegarder, intervenir</h3>
                  <div class="settingsGroupLead">Le dernier panneau est reserve aux operations sensibles: firmware, sauvegarde, service local et diagnostic.</div>
                </div>
                <div class="settingsSectionStamp">Cycle de vie</div>
              </div>
              <div class="settingsSectionGrid">
                <div class="settingsLane">
                  <section class="settingsSurface">
                    <div class="settingsSurfaceHeader">
                      <div>
                        <h3 class="settingsSurfaceTitle">Mises a jour</h3>
                      </div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-updates">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">GitHub Releases</h3>
                        </div>
                      </div>
                      <div class="statusList compact">
                        <div class="statusItem">
                          <div class="k">Firmware actuel</div>
                          <div class="v mono" id="releaseCurrentVersion">--</div>
                        </div>
                        <div class="statusItem">
                          <div class="k">Dernier check</div>
                          <div class="v mono" id="releaseLastCheck">Jamais</div>
                        </div>
                        <div class="statusItem">
                          <div class="k">Etat</div>
                          <div class="v" id="releaseState">En attente</div>
                        </div>
                        <div class="statusItem">
                          <div class="k">Derniere version</div>
                          <div class="v mono" id="releaseLatestVersion">--</div>
                        </div>
                        <div class="statusItem">
                          <div class="k">Publiee le</div>
                          <div class="v mono" id="releasePublishedAt">--</div>
                        </div>
                        <div class="statusItem">
                          <div class="k">Manifest</div>
                          <div class="v mono" id="releaseManifestUrl">--</div>
                        </div>
                        <div class="statusItem">
                          <div class="k">Installation</div>
                          <div class="v mono" id="releaseInstallState">Inactive</div>
                        </div>
                        <div class="statusItem">
                          <div class="k">Progression</div>
                          <div class="v mono" id="releaseInstallProgress">0%</div>
                        </div>
                      </div>
                      <div class="hint" id="releaseHint">Check manuel du dernier firmware publie sur GitHub.</div>
                      <div class="btnRow">
                        <button class="btn accent" id="checkReleaseBtn">Verifier les mises a jour</button>
                        <button class="btn" id="installReleaseBtn" disabled>Installer</button>
                        <button class="btn warn" id="forceInstallReleaseBtn" disabled>Forcer l'installation</button>
                      </div>
                      <div class="toast" id="toastReleaseAdv"></div>
                    </div>

                    <div class="settingsSurfaceBlock" id="settings-ota">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">OTA locale</h3>
                        </div>
                      </div>
                      <div class="field">
                        <label>Activer OTA</label>
                        <select id="otaEnabledAdv">
                          <option value="1">Oui</option>
                          <option value="0">Non</option>
                        </select>
                      </div>
                      <div class="field">
                        <label>Hostname OTA</label>
                        <input id="otaHostnameAdv" type="text" placeholder="soundpanel7"/>
                      </div>
                      <div class="field">
                        <label>Port OTA</label>
                        <input id="otaPortAdv" type="number" min="1" max="65535" placeholder="3232"/>
                      </div>
                      <div class="field">
                        <label>Mot de passe OTA</label>
                        <input id="otaPasswordAdv" type="password" placeholder="laisser vide = aucun mot de passe"/>
                      </div>
                      <div class="btnRow">
                        <button class="btn accent" id="saveOtaAdv">Sauver OTA</button>
                      </div>
                      <div class="toast" id="toastOtaAdv"></div>
                    </div>
                  </section>
                </div>

                <div class="settingsLane">
                  <section class="settingsSurface">
                    <div class="settingsSurfaceHeader">
                      <div>
                        <h3 class="settingsSurfaceTitle">Sauvegarde & service</h3>
                      </div>
                    </div>

                    <div class="settingsSurfaceBlock">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Sauvegarde & configuration</h3>
                        </div>
                      </div>
                      <div class="field">
                        <label>JSON config</label>
                        <textarea id="configJsonBox" placeholder='{"type":"soundpanel7-config", ...}'></textarea>
                      </div>
                      <div class="field">
                        <label>Importer depuis un fichier JSON</label>
                        <input id="configFile" type="file" accept=".json,application/json"/>
                      </div>
                      <div class="hint">Export JSON: version safe sans secrets par defaut. L'export complet en clair est volontairement marque comme dangereux. Le backup local conserve une copie chiffree restaurable sur l'appareil.</div>
                      <div class="btnRow">
                        <button class="btn" id="exportConfigBtn">Exporter JSON</button>
                        <button class="btn warn" id="exportFullConfigBtn">Exporter complet dangereux</button>
                        <button class="btn accent" id="importConfigBtn">Importer JSON</button>
                      </div>
                      <div class="field">
                        <label>Reset partiel</label>
                        <select id="configResetScope">
                          <option value="ui">UI</option>
                          <option value="security">Securite web + HA</option>
                          <option value="time">Heure / hostname</option>
                          <option value="wifi">Wi-Fi</option>
                          <option value="audio">Audio</option>
                          <option value="calibration">Calibration</option>
                          <option value="ota">OTA</option>
                          <option value="mqtt">MQTT</option>
                          <option value="notifications">Notifications</option>
                        </select>
                      </div>
                      <div class="btnRow">
                        <button class="btn" id="backupConfigBtn">Backup</button>
                        <button class="btn" id="restoreConfigBtn">Restore</button>
                        <button class="btn danger" id="partialResetBtn">Reset partiel</button>
                      </div>
                      <div class="hint mono" id="backupInfo">Dernier backup: --</div>
                      <div class="toast" id="configToast"></div>
                    </div>

                    <div class="settingsSurfaceBlock">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Actions systeme</h3>
                        </div>
                      </div>
                      <div class="btnRow">
                        <button class="btn" id="rebootBtn">Reboot</button>
                        <button class="btn" id="shutdownBtn">Shutdown</button>
                        <button class="btn danger" id="factoryResetBtn">Factory reset</button>
                      </div>
                      <div class="toast" id="actionsToast"></div>
                    </div>

                    <div class="settingsSurfaceBlock">
                      <div class="settingsSubhead">
                        <div>
                          <h3 class="settingsSubtitle">Logs debug</h3>
                        </div>
                      </div>
                      <div class="hint">Ouvre les derniers logs firmware utiles pour diagnostiquer les soucis reseau, OTA, MQTT ou releases sans brancher le port serie.</div>
                      <div class="btnRow">
                        <button class="btn accent" id="toggleDebugLogsBtn">Logs</button>
                        <button class="btn" id="refreshDebugLogsBtn">Rafraichir</button>
                        <button class="btn" id="clearDebugLogsBtn">Vider</button>
                      </div>
                      <div class="debugConsole hidden" id="debugConsoleWrap">
                        <div class="debugConsoleMeta">
                          <div class="mono" id="debugLogsMeta">Aucun log charge.</div>
                          <div class="mono" id="debugLogsHint">Mode debug web</div>
                        </div>
                        <pre class="debugConsoleBox" id="debugLogsBox">(ouvre les logs pour charger le buffer)</pre>
                      </div>
                      <div class="toast" id="debugLogsToast"></div>
                    </div>

                  </section>
                </div>
              </div>
            </article>
          </div>

          <div class="settingsRail">
            <article class="card settingsNav settingsNavSlim">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Sommaire</div>
                  <h2 class="sectionTitle">Acces rapide</h2>
                  <div class="hint">Le rail sert de carte mentale. Tu sais ou tu vas avant meme de scroller.</div>
                </div>
              </div>
              <div class="settingsJumpList">
                <a class="settingsJumpLink" href="#settings-overview">
                  <div class="settingsJumpMain">
                    <div class="settingsJumpTitle">Vue systeme</div>
                    <div class="settingsJumpMeta">Cockpit global, version, charge, uptime, moteur UI</div>
                  </div>
                  <div class="settingsJumpArrow">+</div>
                </a>
                <a class="settingsJumpLink" href="#settings-controls">
                  <div class="settingsJumpMain">
                    <div class="settingsJumpTitle">Exploitation</div>
                    <div class="settingsJumpMeta">Seuils, LIVE, ecran tactile, PIN et comptes web</div>
                  </div>
                  <div class="settingsJumpArrow">+</div>
                </a>
                <a class="settingsJumpLink" href="#settings-network">
                  <div class="settingsJumpMain">
                    <div class="settingsJumpTitle">Connexion & alertes</div>
                    <div class="settingsJumpMeta">Heure, Wi-Fi, Home Assistant, MQTT, Slack, Telegram, WhatsApp</div>
                  </div>
                  <div class="settingsJumpArrow">+</div>
                </a>
                <a class="settingsJumpLink" href="#settings-maintenance">
                  <div class="settingsJumpMain">
                    <div class="settingsJumpTitle">Maintenance</div>
                    <div class="settingsJumpMeta">Releases, OTA, sauvegarde, reset, diagnostic et service local</div>
                  </div>
                  <div class="settingsJumpArrow">+</div>
                </a>
              </div>
            </article>
          </div>
        </div>
      </section>

    </div>
  </main>

<script>
  const $ = (id) => document.getElementById(id);

  const state = {
    status: null,
    authenticated: false,
    bootstrapRequired: false,
    currentUser: "",
    liveToken: "",
    userCount: 0,
    maxUsers: 4,
    users: [],
    historyValues: [],
    historyInitialized: false,
    historyMinutes: 5,
    historyCapacity: 96,
    historySamplePeriodMs: 3000,
    lastHistorySampleClientMs: 0,
    lastLiveEventMs: 0,
    rawAudioSeededFromStatus: false,
    rawAudioLiveStarted: false,
    clockBaseMs: 0,
    clockSyncPerfMs: 0,
    clockDisplayedSecond: -1,
    clockRaf: 0,
    currentPage: "overview",
    settingsPanelsLoaded: false,
    settingsPanelsLoading: false,
    lastSettingsSummaryRenderMs: 0,
    lastBackgroundStatusRefreshMs: 0,
    events: null,
    reconnectTimer: null,
    releaseInstallPollTimer: null,
    releaseInstallPollPending: false,
    debugLogsOpen: false,
    debugLogsLoadedOnce: false,
    debugLogsPollTimer: null,
    hasLiveFeed: false,
    lastContactMs: 0,
    lastStatusContactMs: 0,
    orangeSinceMs: 0,
    redSinceMs: 0,
    liveEnabled: false,
    touchEnabled: true,
    hasScreen: true,
    supportsDashboardDisplay: true,
    supportsDashboardPin: true,
    supportsTardisControl: false,
    supportsTardisInteriorRgb: false,
    dashboardPage: 0,
    dashboardFullscreenMask: 0,
    dashboardDisplayDirty: false,
    tardisModeEnabled: false,
    tardisInteriorLedEnabled: false,
    tardisExteriorLedEnabled: false,
    tardisInteriorLedPin: 15,
    tardisExteriorLedPin: 16,
    tardisInteriorRgbPin: 48,
    tardisInteriorRgbMode: 0,
    tardisInteriorRgbColor: 0x2D9CDB,
    tardisDirty: false,
    uiDirty: false,
    uiAudioSource: 1,
    uiAudioSourceSupportsCalibration: true,
    uiAudioSourceUsesAnalog: true,
    uiResponseMode: 0,
    pinConfigured: false,
    calibrationPointCount: 3,
    calRefs: [45, 65, 85, 95, 105],
    calRefsDirty: [false, false, false, false, false],
  };

  const gaugeViews = [
    { page: "overview", gauge: $("overviewGauge"), db: $("overviewDb"), dot: $("overviewDot"), card: $("overviewSoundCard") },
    { page: "sound", gauge: $("soundGauge"), db: $("soundDb"), dot: $("soundDot"), card: $("soundCard") },
  ];

  const metricViews = [
    { page: "overview", leq: $("overviewLeq"), peak: $("overviewPeak") },
    { page: "sound", leq: $("soundLeq"), peak: $("soundPeak") },
  ];

  const historyViews = [
    {
      page: "overview",
      meta: $("overviewHistMeta"),
      alert: $("overviewAlertTime"),
      bars: $("overviewHistBars"),
      left: $("overviewHistLeft"),
      mid: $("overviewHistMid"),
      right: $("overviewHistRight"),
    },
    {
      page: "sound",
      meta: $("soundHistMeta"),
      alert: $("soundAlertTime"),
      bars: $("soundHistBars"),
      left: $("soundHistLeft"),
      mid: $("soundHistMid"),
      right: $("soundHistRight"),
    },
  ];

  const calibrationRecommendations = {
    3: [45, 65, 85, 95, 105],
    5: [40, 55, 70, 85, 100],
  };

  function pad2(v) {
    return String(v).padStart(2, "0");
  }

  function sanitizePinValue(value) {
    return String(value || "").replace(/[^0-9]/g, "").slice(0, 8);
  }

  function getDashboardPageValue(value) {
    const page = Number(value);
    return page >= 1 && page <= 3 ? page : 0;
  }

  function getDashboardFullscreenMaskValue(value) {
    return Number(value) & 0x0F;
  }

  function dashboardFullscreenButtonId(page) {
    switch (Number(page)) {
      case 0: return "dashboardFsOverview";
      case 1: return "dashboardFsClock";
      case 2: return "dashboardFsLive";
      case 3: return "dashboardFsSound";
      default: return "";
    }
  }

  function applyDashboardFullscreenMask(mask) {
    const normalizedMask = getDashboardFullscreenMaskValue(mask);
    [0, 1, 2, 3].forEach((page) => {
      const id = dashboardFullscreenButtonId(page);
      if (!id) return;
      const enabled = (normalizedMask & (1 << page)) !== 0;
      $(id).classList.toggle("active", enabled);
      $(id).setAttribute("aria-pressed", enabled ? "true" : "false");
    });
  }

  function readDashboardFullscreenMask() {
    let mask = 0;
    [0, 1, 2, 3].forEach((page) => {
      const id = dashboardFullscreenButtonId(page);
      if (id && $(id).classList.contains("active")) mask |= (1 << page);
    });
    return getDashboardFullscreenMaskValue(mask);
  }

  function setDashboardTouchEnabled(enabled) {
    const active = Boolean(enabled);
    $("touchEnabledAdv").classList.toggle("active", active);
    $("touchEnabledAdv").setAttribute("aria-pressed", active ? "true" : "false");
    $("touchEnabledAdvVal").textContent = active ? "ACTIF" : "OFF";
  }

  function readDashboardTouchEnabled() {
    return $("touchEnabledAdv").classList.contains("active");
  }

  function setTardisSwitch(id, valueId, enabled, onLabel) {
    const active = Boolean(enabled);
    $(id).classList.toggle("active", active);
    $(id).setAttribute("aria-pressed", active ? "true" : "false");
    $(valueId).textContent = active ? onLabel : "OFF";
  }

  function tardisColorToHex(value) {
    const numeric = Math.max(0, Number(value || 0)) >>> 0;
    return `#${(numeric & 0xFFFFFF).toString(16).padStart(6, "0").toUpperCase()}`;
  }

  function syncTardisUi() {
    setTardisSwitch("tardisModeEnabled", "tardisModeEnabledVal", state.tardisModeEnabled, "ACTIF");
    setTardisSwitch("tardisInteriorLedEnabled", "tardisInteriorLedEnabledVal", state.tardisInteriorLedEnabled, "ON");
    setTardisSwitch("tardisExteriorLedEnabled", "tardisExteriorLedEnabledVal", state.tardisExteriorLedEnabled, "ON");

    const pinsBadge = state.supportsTardisInteriorRgb
      ? `RGB ${state.tardisInteriorRgbPin} / GPIO ${state.tardisExteriorLedPin}`
      : `GPIO ${state.tardisInteriorLedPin} / ${state.tardisExteriorLedPin}`;
    $("tardisPinsBadge").textContent = pinsBadge;
    $("tardisPinsHint").textContent = state.supportsTardisInteriorRgb
      ? `LED RGB integree sur GPIO ${state.tardisInteriorRgbPin} pour l'interieur, GPIO exterieur ${state.tardisExteriorLedPin}. Broches dediees pour rester hors des pins micro 4, 11, 12 et 13.`
      : `GPIO interieur ${state.tardisInteriorLedPin} / exterieur ${state.tardisExteriorLedPin}. Broches dediees pour rester hors des pins micro 4, 11, 12 et 13.`;

    const ledsEnabled = Boolean(state.tardisModeEnabled);
    $("tardisInteriorLedEnabled").disabled = !ledsEnabled;
    $("tardisExteriorLedEnabled").disabled = !ledsEnabled;
    $("tardisInteriorRgbModeField").style.display = state.supportsTardisInteriorRgb ? "" : "none";
    $("tardisInteriorRgbColorField").style.display = state.supportsTardisInteriorRgb ? "" : "none";
    $("tardisInteriorRgbMode").value = String(Number(state.tardisInteriorRgbMode || 0));
    $("tardisInteriorRgbColor").value = tardisColorToHex(state.tardisInteriorRgbColor);
    $("tardisInteriorRgbMode").disabled = !ledsEnabled || !state.tardisInteriorLedEnabled;
    $("tardisInteriorRgbColor").disabled = !ledsEnabled
      || !state.tardisInteriorLedEnabled
      || Number(state.tardisInteriorRgbMode || 0) !== 1;
  }

  function sanitizeUsernameValue(value) {
    return String(value || "").trim().toLowerCase().replace(/[^a-z0-9._-]/g, "").slice(0, 24);
  }

  function sanitizeHomeAssistantTokenValue(value) {
    return String(value || "").trim().replace(/[^A-Za-z0-9_-]/g, "").slice(0, 64);
  }

  function passwordPolicyHint(password) {
    const value = String(password || "");
    let classes = 0;
    if (/[a-z]/.test(value)) classes++;
    if (/[A-Z]/.test(value)) classes++;
    if (/[0-9]/.test(value)) classes++;
    if (/[^A-Za-z0-9]/.test(value)) classes++;
    if (value.length < 10) return "10 caracteres minimum.";
    if (classes < 3) return "Utilise 3 types: majuscule, minuscule, chiffre, symbole.";
    return "";
  }

  function getCalibrationPointCount(value) {
    return Number(value) >= 5 ? 5 : 3;
  }

  function calibrationRows() {
    return Array.from(document.querySelectorAll(".calRow"));
  }

  function syncCalibrationModeButtons() {
    const pointCount = getCalibrationPointCount(state.calibrationPointCount);
    $("calMode3").classList.toggle("active", pointCount === 3);
    $("calMode5").classList.toggle("active", pointCount === 5);
    calibrationRows().forEach((row, index) => {
      row.hidden = index >= pointCount;
    });
  }

  function formatUptime(totalSeconds) {
    const uptime = Math.max(0, Math.floor(Number(totalSeconds) || 0));
    const days = Math.floor(uptime / 86400);
    const hours = Math.floor((uptime % 86400) / 3600);
    const minutes = Math.floor((uptime % 3600) / 60);
    const seconds = uptime % 60;

    if (days > 0) {
      return `${days}j ${pad2(hours)}:${pad2(minutes)}:${pad2(seconds)}`;
    }
    return `${pad2(hours)}:${pad2(minutes)}:${pad2(seconds)}`;
  }

  function formatBackupDate(ts) {
    const seconds = Number(ts || 0);
    if (!seconds) return "--";
    const d = new Date(seconds * 1000);
    if (Number.isNaN(d.getTime())) return "--";
    return `${pad2(d.getDate())}/${pad2(d.getMonth() + 1)}/${d.getFullYear()} ${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  }

  function zoneColor(db, greenMax, orangeMax) {
    if (db <= greenMax) return "#23C552";
    if (db <= orangeMax) return "#F0A202";
    return "#E53935";
  }

  function historyHeightPercent(db) {
    const histDbMin = 35;
    const histDbMax = 130;
    const clamped = Math.max(histDbMin, Math.min(histDbMax, Number(db) || 0));
    return ((clamped - histDbMin) / (histDbMax - histDbMin)) * 100;
  }

  const CLOCK_DISPLAY_PHASE_MS = 900;

  function stopClockLoop() {
    if (state.clockRaf) {
      cancelAnimationFrame(state.clockRaf);
      state.clockRaf = 0;
    }
  }

  function clockNowMs() {
    if (!state.clockBaseMs || !state.clockSyncPerfMs) return 0;
    return state.clockBaseMs + (performance.now() - state.clockSyncPerfMs);
  }

  function syncClock(serverTime, serverTimeMs = 0, source = "status") {
    const epochMs = Number(serverTimeMs || 0);
    if (!serverTime && epochMs <= 0) {
      if (state.clockBaseMs) return;
      state.clockBaseMs = 0;
      state.clockSyncPerfMs = 0;
      state.clockDisplayedSecond = -1;
      stopClockLoop();
      renderClock();
      return;
    }

    const parsed = epochMs > 0 ? epochMs : Date.parse(String(serverTime || "").replace(" ", "T"));
    if (Number.isNaN(parsed)) {
      if (state.clockBaseMs) return;
      state.clockBaseMs = 0;
      state.clockSyncPerfMs = 0;
      state.clockDisplayedSecond = -1;
      stopClockLoop();
      renderClock();
      return;
    }

    if (state.clockBaseMs && state.clockSyncPerfMs) {
      const predictedMs = clockNowMs();
      const driftMs = parsed - predictedMs;
      // This UI clock must be monotonic. No source is allowed to move it
      // backwards because that is visibly unacceptable on a broadcast clock.
      if (driftMs <= 0) return;

      // Keep the display stable and only accept meaningful forward corrections.
      if (driftMs < 80) return;
    }

    state.clockBaseMs = parsed;
    state.clockSyncPerfMs = performance.now();
    state.clockDisplayedSecond = -1;
    renderClock();
  }

  function ensureClockLoop() {
    if (state.clockRaf || !state.clockBaseMs) return;
    const step = () => {
      state.clockRaf = 0;
      const nowMs = clockNowMs();
      if (!nowMs) return;
      const displaySecond = Math.floor((nowMs + CLOCK_DISPLAY_PHASE_MS) / 1000);
      if (displaySecond !== state.clockDisplayedSecond) {
        renderClock();
      }
      state.clockRaf = requestAnimationFrame(step);
    };
    state.clockRaf = requestAnimationFrame(step);
  }

  function renderClock() {
    if (!state.clockBaseMs) {
      $("overviewClockDate").textContent = "--/--/----";
      $("overviewClockMain").textContent = "--:--";
      $("overviewClockSec").textContent = ":--";
      $("clockDate").textContent = "--/--/----";
      $("clockMain").textContent = "--:--";
      $("clockSec").textContent = "--";
      $("settingsTime").textContent = "NTP...";
      state.clockDisplayedSecond = -1;
      stopClockLoop();
      return;
    }

    const nowMs = clockNowMs() + CLOCK_DISPLAY_PHASE_MS;
    state.clockDisplayedSecond = Math.floor(nowMs / 1000);

    const d = new Date(nowMs);
    const date = `${pad2(d.getDate())}/${pad2(d.getMonth() + 1)}/${d.getFullYear()}`;
    const hhmm = `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
    const sec = pad2(d.getSeconds());
    const hhmmss = `${hhmm}:${sec}`;

    $("overviewClockDate").textContent = date;
    $("overviewClockMain").textContent = hhmm;
    $("overviewClockSec").textContent = `:${sec}`;
    $("clockDate").textContent = date;
    $("clockMain").textContent = hhmm;
    $("clockSec").textContent = sec;
    $("settingsTime").textContent = hhmmss;
    ensureClockLoop();
  }

  function applyPinState() {
    const configured = Boolean(state.pinConfigured);
    $("pinStatusAdv").textContent = configured ? "PIN tactile: actif" : "PIN tactile: --";
    $("pinStatusAdv").classList.toggle("active", configured);
    $("clearPinAdv").disabled = !configured;
  }

  function setAuthLocked(locked) {
    document.body.classList.toggle("authLocked", locked);
    $("authGate").classList.toggle("hidden", !locked);
  }

  function renderUsersPanel() {
    $("usersSummary").textContent = `${state.userCount} / ${state.maxUsers} utilisateurs`;
    $("logoutBtn").textContent = state.currentUser ? `Deconnexion (${state.currentUser})` : "Deconnexion";

    const options = state.users.map((user) => {
      const selected = user.username === state.currentUser ? " selected" : "";
      return `<option value="${user.username}"${selected}>${user.username}</option>`;
    }).join("");
    $("manageWebUserSelect").innerHTML = options || '<option value="">Aucun utilisateur</option>';

    $("updateWebPasswordBtn").disabled = !state.users.length;
    $("deleteWebUserBtn").disabled = state.userCount <= 1 || !state.users.length;
    $("createWebUserBtn").disabled = state.userCount >= state.maxUsers;

    if (!state.users.length) {
      $("usersList").innerHTML = '<div class="hint">Aucun utilisateur web configure.</div>';
      return;
    }

    $("usersList").innerHTML = state.users.map((user) => {
      const current = user.username === state.currentUser ? "Session active" : "Compte actif";
      return `
        <div class="userRow">
          <div>
            <div class="userRowName mono">${user.username}</div>
            <div class="userRowMeta">${current}</div>
          </div>
          <button class="btn" data-user-select="${user.username}">Selectionner</button>
          <button class="btn danger" data-user-delete="${user.username}" ${state.userCount <= 1 ? "disabled" : ""}>Supprimer</button>
        </div>
      `;
    }).join("");

    document.querySelectorAll("[data-user-select]").forEach((btn) => {
      btn.addEventListener("click", () => {
        $("manageWebUserSelect").value = btn.dataset.userSelect;
      });
    });
    document.querySelectorAll("[data-user-delete]").forEach((btn) => {
      btn.addEventListener("click", () => deleteWebUser(btn.dataset.userDelete));
    });
  }

  function renderAuthState() {
    setAuthLocked(!state.authenticated);
    const bootstrap = Boolean(state.bootstrapRequired);
    $("authTitle").textContent = bootstrap ? "Creer le premier compte" : "Connexion";
    $("authLead").textContent = bootstrap
      ? "Aucun utilisateur web n'est configure. Cree un compte administrateur local pour initialiser l'acces."
      : "Connecte-toi avec un compte web local pour acceder au dashboard.";
    $("authSubmitBtn").textContent = bootstrap ? "Creer le compte" : "Se connecter";
    $("authPassword").setAttribute("autocomplete", bootstrap ? "new-password" : "current-password");
    $("authPasswordHint").textContent = "Mot de passe conseille: 10+ caracteres avec 3 types parmi majuscule, minuscule, chiffre, symbole.";
  }

  function setActivePage(page) {
    state.currentPage = page;
    document.querySelectorAll(".tab").forEach((el) => {
      el.classList.toggle("active", el.dataset.page === page);
    });
    document.querySelectorAll(".page").forEach((el) => {
      el.classList.toggle("active", el.dataset.page === page);
    });
    applyPinState();
    if (page === "settings") {
      ensureSettingsPanelsLoaded();
      if (state.status) updateStatusSummary(state.status, true);
      refreshSystemSummary(true);
    }
    refreshActiveLiveViews();
  }

  function formatRedSeconds(redSeconds, historyMinutes) {
    if (redSeconds >= 60) {
      return `Rouge ${Math.floor(redSeconds / 60)}m${pad2(redSeconds % 60)}s / ${historyMinutes}m`;
    }
    return `Rouge ${redSeconds}s / ${historyMinutes}m`;
  }

  function updateHistoryLabels() {
    const mid = Math.max(1, Math.floor(state.historyMinutes / 2));
    historyViews.forEach((view) => {
      view.meta.textContent = `Historique ${state.historyMinutes} min`;
      view.left.textContent = `-${state.historyMinutes}m`;
      view.mid.textContent = `-${mid}m`;
      view.right.textContent = "0";
    });
  }

  function isLiveViewVisible(page) {
    return state.currentPage === page;
  }

  function isDashboardLivePage(page = state.currentPage) {
    return page === "overview" || page === "sound";
  }

  function drawHistory() {
    const values = state.historyValues.slice(-state.historyCapacity);
    const greenMax = Number(state.status?.greenMax ?? 55);
    const orangeMax = Number(state.status?.orangeMax ?? 70);
    const renderCapacity = getHistoryRenderCapacity();
    const renderedValues = compressHistory(values, renderCapacity);
    const missing = Math.max(0, renderCapacity - renderedValues.length);

    let html = "";
    for (let i = 0; i < missing; i++) html += '<div class="histBar"></div>';
    for (const value of renderedValues) {
      html += `<div class="histBar" style="height:${historyHeightPercent(value).toFixed(1)}%;background:${zoneColor(value, greenMax, orangeMax)}"></div>`;
    }

    const redSamples = values.filter((value) => value > orangeMax).length;
    const redSeconds = Math.round((redSamples * state.historySamplePeriodMs) / 1000);
    const alertText = formatRedSeconds(redSeconds, state.historyMinutes);

    historyViews.forEach((view) => {
      if (!isLiveViewVisible(view.page)) return;
      view.bars.innerHTML = html;
      view.alert.textContent = alertText;
    });
  }

  function getHistoryRenderCapacity() {
    const widths = historyViews
      .map((view) => view.bars && view.bars.parentElement ? view.bars.parentElement.clientWidth : 0)
      .filter((width) => width > 0);

    if (!widths.length) return state.historyCapacity;

    const minWidth = Math.min(...widths);
    const slotPx = window.innerWidth <= 700 ? 3 : 5;
    const capacity = Math.floor(minWidth / slotPx);
    return Math.max(24, Math.min(state.historyCapacity, capacity));
  }

  function compressHistory(values, targetCount) {
    if (!Array.isArray(values) || values.length <= targetCount) return values;

    const out = [];
    const bucketSize = values.length / targetCount;

    for (let i = 0; i < targetCount; i++) {
      const start = Math.floor(i * bucketSize);
      const end = Math.max(start + 1, Math.floor((i + 1) * bucketSize));
      let bucketMax = Number(values[start] ?? 0);

      for (let j = start + 1; j < end && j < values.length; j++) {
        const value = Number(values[j] ?? 0);
        if (value > bucketMax) bucketMax = value;
      }

      out.push(bucketMax);
    }

    return out;
  }

  function setHistory(values) {
    state.historyValues = Array.isArray(values) ? values.map((value) => Number(value) || 0) : [];
    if (state.historyValues.length > state.historyCapacity) {
      state.historyValues = state.historyValues.slice(-state.historyCapacity);
    }
    state.historyInitialized = true;
    state.lastHistorySampleClientMs = Date.now();
    updateHistoryLabels();
    drawHistory();
  }

  function appendHistory(db, force = false) {
    const now = Date.now();
    if (!force && state.lastHistorySampleClientMs && (now - state.lastHistorySampleClientMs) < state.historySamplePeriodMs) {
      return;
    }

    state.lastHistorySampleClientMs = now;
    state.historyValues.push(Number(db) || 0);
    if (state.historyValues.length > state.historyCapacity) {
      state.historyValues = state.historyValues.slice(-state.historyCapacity);
    }
    drawHistory();
  }

  function updateAlertState(db, greenMax, orangeMax, warningHoldSec, criticalHoldSec) {
    const now = Date.now();
    const orange = db > greenMax;
    const red = db > orangeMax;

    if (orange) {
      if (!state.orangeSinceMs) state.orangeSinceMs = now;
    } else {
      state.orangeSinceMs = 0;
    }

    if (red) {
      if (!state.redSinceMs) state.redSinceMs = now;
    } else {
      state.redSinceMs = 0;
    }

    const orangeAlert = state.orangeSinceMs && (now - state.orangeSinceMs) >= (warningHoldSec * 1000);
    const redAlert = state.redSinceMs && (now - state.redSinceMs) >= (criticalHoldSec * 1000);

    document.body.style.background = redAlert ? "var(--screenRed)" : "var(--bg)";
    gaugeViews.forEach((view, index) => {
      view.card.classList.toggle("alertOrange", Boolean(orangeAlert && !redAlert));
      view.card.classList.toggle("alertRed", Boolean(redAlert));
      if (index === 1) $("soundAlertBadge").classList.toggle("show", Boolean(redAlert));
    });
  }

  function updateGauge(db, greenMax, orangeMax) {
    const pct = Math.max(0, Math.min(100, db));
    const color = zoneColor(db, greenMax, orangeMax);

    gaugeViews.forEach((view) => {
      if (!isLiveViewVisible(view.page)) return;
      view.gauge.style.setProperty("--pct", pct);
      view.gauge.style.setProperty("--gaugeColor", color);
      view.dot.style.background = color;
      view.db.textContent = db.toFixed(1);
    });
  }

  function updateMetrics(leq, peak) {
    metricViews.forEach((view) => {
      if (!isLiveViewVisible(view.page)) return;
      view.leq.textContent = leq.toFixed(1);
      view.peak.textContent = peak.toFixed(1);
    });
  }

  function applyRawAudio(st) {
    $("rawRmsAdv").textContent = Number(st.rawRms ?? 0).toFixed(2);
    $("rawPseudoDbAdv").textContent = Number(st.rawPseudoDb ?? 0).toFixed(1);
    $("rawAdcMeanAdv").textContent = String(st.rawAdcMean ?? "--");
    $("rawAdcLastAdv").textContent = String(st.rawAdcLast ?? "--");
  }

  function refreshActiveLiveViews() {
    if (!state.status) return;
    if (!isDashboardLivePage()) return;
    const db = Number(state.status.db ?? 0);
    const leq = Number(state.status.leq ?? 0);
    const peak = Number(state.status.peak ?? 0);
    const greenMax = Number(state.status.greenMax ?? 55);
    const orangeMax = Number(state.status.orangeMax ?? 70);
    const warningHoldSec = Number(state.status.warningHoldSec ?? 3);
    const criticalHoldSec = Number(state.status.criticalHoldSec ?? 2);

    updateGauge(db, greenMax, orangeMax);
    updateMetrics(leq, peak);
    updateAlertState(db, greenMax, orangeMax, warningHoldSec, criticalHoldSec);
    drawHistory();
  }

  function applyLiveActionState() {
    const active = Boolean(state.liveEnabled);
    $("liveActionBtn").classList.toggle("active", active);
    $("liveActionBtn").setAttribute("aria-pressed", active ? "true" : "false");
    $("liveActionState").textContent = active ? "ON AIR" : "OFF AIR";
  }

  function applySystemPayload(st) {
    state.status = { ...(state.status || {}), ...st };
    const merged = state.status;

    if ("time_ok" in st || "timeUnixMs" in st) syncClock(merged.time_ok ? merged.time : "", merged.timeUnixMs, "status");
    if (state.currentPage !== "settings") return;

    updateStatusSummary(merged, true);
  }

  function updateStatusSummary(st, force = false) {
    if (state.currentPage !== "settings") return;
    const now = Date.now();
    if (!force && state.lastSettingsSummaryRenderMs && (now - state.lastSettingsSummaryRenderMs) < 4500) return;
    state.lastSettingsSummaryRenderMs = now;
    const setMetricTone = (id, tone) => {
      const el = $(id);
      el.classList.remove("metricOk", "metricWarn", "metricBad");
      if (tone === "bad") el.classList.add("metricBad");
      else if (tone === "warn") el.classList.add("metricWarn");
      else el.classList.add("metricOk");
    };
    const worstTone = (...tones) => {
      if (tones.includes("bad")) return "bad";
      if (tones.includes("warn")) return "warn";
      return "ok";
    };
    const loadTone = (load) => load >= 50 ? "bad" : (load >= 25 ? "warn" : "ok");
    const timingTone = (ms) => ms >= 20 ? "bad" : (ms >= 10 ? "warn" : "ok");
    const heapTone = (freeBytes, minBytes, objCount) => {
      if (freeBytes < 65536 || minBytes < 49152 || objCount >= 700) return "bad";
      if (freeBytes < 98304 || minBytes < 81920 || objCount >= 450) return "warn";
      return "ok";
    };
    const usageTone = (usedPct) => usedPct >= 95 ? "bad" : (usedPct >= 85 ? "warn" : "ok");
    const kb = (value) => `${Math.round(Number(value || 0) / 1024)}k`;
    const usToMs = (value) => `${(Number(value || 0) / 1000).toFixed(1)} ms`;
    const pctUsed = (freeBytes, totalBytes) => {
      const total = Number(totalBytes || 0);
      if (total <= 0) return null;
      const free = Math.max(0, Number(freeBytes || 0));
      return Math.max(0, Math.min(100, Math.round(((total - free) * 100) / total)));
    };
    const pctFromUsage = (usedBytes, totalBytes) => {
      const total = Number(totalBytes || 0);
      if (total <= 0) return null;
      const used = Math.max(0, Number(usedBytes || 0));
      return Math.max(0, Math.min(100, Math.round((used * 100) / total)));
    };
    const cpuLoadPct = Number(st.cpuLoadPct ?? 0);
    const lvglLoadPct = Number(st.lvglLoadPct ?? 0);
    const lvglIdlePct = Number(st.lvglIdlePct ?? 100);
    const lvglUiWorkUs = Number(st.lvglUiWorkUs ?? 0);
    const lvglUiWorkMaxUs = Number(st.lvglUiWorkMaxUs ?? 0);
    const lvglHandlerUs = Number(st.lvglHandlerUs ?? 0);
    const lvglHandlerMaxUs = Number(st.lvglHandlerMaxUs ?? 0);
    const lvglObjCount = Number(st.lvglObjCount ?? 0);
    const heapInternalFree = Number(st.heapInternalFree ?? 0);
    const heapInternalTotal = Number(st.heapInternalTotal ?? 0);
    const heapInternalMin = Number(st.heapInternalMin ?? 0);
    const heapPsramFree = Number(st.heapPsramFree ?? 0);
    const heapPsramTotal = Number(st.heapPsramTotal ?? 0);
    const heapPsramMin = Number(st.heapPsramMin ?? 0);
    const fsTotalBytes = Number(st.fsTotalBytes ?? 0);
    const fsUsedBytes = Number(st.fsUsedBytes ?? 0);
    const heapInternalUsedPct = pctUsed(heapInternalFree, heapInternalTotal);
    const heapPsramUsedPct = pctUsed(heapPsramFree, heapPsramTotal);
    const fsUsedPct = pctFromUsage(fsUsedBytes, fsTotalBytes);
    $("settingsIp").textContent = st.wifi ? `${st.ip || "-"} / ${st.rssi ?? 0} dBm` : "--";
    $("settingsUptime").textContent = formatUptime(st.uptime_s);
    $("settingsHistory").textContent = `${state.historyMinutes} min / ${state.historyCapacity} points`;
    $("settingsVersion").textContent = st.version || "--";
    $("settingsBuildDate").textContent = st.buildDate || "--";
    $("settingsBuildEnv").textContent = st.buildEnv || "--";
    $("settingsMcuTemp").textContent = st.mcuTempOk ? `${Number(st.mcuTempC ?? 0).toFixed(1)} C` : "--";
    $("settingsCpu").textContent = `${cpuLoadPct}%`;
    const otaState = st.otaEnabled
      ? (st.otaStarted ? "actif" : "configure")
      : "off";
    const mqttState = st.mqttEnabled
      ? (st.mqttConnected ? "connecte" : (st.mqttLastError ? `erreur (${st.mqttLastError})` : "en attente"))
      : "off";
    const releaseState = st.releaseUpdateChecked
      ? (st.releaseUpdateAvailable
          ? `update ${st.releaseLatestVersion || "dispo"}`
          : (st.releaseUpdateOk ? "a jour" : "check en echec"))
      : "update non verifiee";
    const installState = st.releaseInstallInProgress
      ? ` / install ${Number(st.releaseInstallProgressPct ?? 0)}%`
      : "";
    $("settingsOtaMqtt").textContent = `OTA ${otaState} / MQTT ${mqttState} / ${releaseState}${installState}`;
    $("settingsActivePage").textContent = st.activePage || "--";
    $("settingsLvglLoad").textContent = `${lvglLoadPct}% / ${lvglIdlePct}%`;
    $("settingsLvglTiming").textContent =
      `UI ${usToMs(lvglUiWorkUs)} max ${usToMs(lvglUiWorkMaxUs)} | H ${usToMs(lvglHandlerUs)} max ${usToMs(lvglHandlerMaxUs)}`;
    const intText = heapInternalUsedPct === null
      ? `INT ${kb(heapInternalFree)} min ${kb(heapInternalMin)}`
      : `INT ${kb(heapInternalFree)} min ${kb(heapInternalMin)} util ${heapInternalUsedPct}%`;
    const psramText = heapPsramFree > 0
      ? (heapPsramUsedPct === null
          ? ` / PS ${kb(heapPsramFree)} min ${kb(heapPsramMin)}`
          : ` / PS ${kb(heapPsramFree)} min ${kb(heapPsramMin)} util ${heapPsramUsedPct}%`)
      : "";
    const fsText = fsTotalBytes > 0
      ? ` / FS ${kb(fsUsedBytes)}/${kb(fsTotalBytes)} util ${fsUsedPct ?? 0}%`
      : "";
    $("settingsLvglHeap").textContent =
      `${intText} / OBJ ${lvglObjCount}${psramText}${fsText}`;
    setMetricTone("settingsLvglLoad", loadTone(lvglLoadPct));
    setMetricTone("settingsLvglTiming", worstTone(timingTone(lvglUiWorkUs / 1000), timingTone(lvglHandlerUs / 1000)));
    setMetricTone("settingsLvglHeap", worstTone(
      heapTone(heapInternalFree, heapInternalMin, lvglObjCount),
      usageTone(heapInternalUsedPct ?? 0),
      fsUsedPct === null ? "ok" : usageTone(fsUsedPct)
    ));
    $("backupInfo").textContent = `Dernier backup: ${formatBackupDate(st.backupTs)}`;

  }

  function updateSystemBadges(label, tone) {
    ["settingsSystemBadge", "topSystemBadge"].forEach((id) => {
      const badge = $(id);
      if (!badge) return;
      badge.textContent = label;
      badge.classList.toggle("ok", tone === "ok");
      badge.classList.toggle("bad", tone === "bad");
    });
  }

  function setSystemBadgeOnline(source = "live") {
    const now = Date.now();
    if (source === "status") {
      state.lastStatusContactMs = now;
      if (state.hasLiveFeed) return;
      state.lastContactMs = now;
    } else {
      state.lastContactMs = now;
    }
    updateSystemBadges("En ligne", "ok");
  }

  function setSystemBadgeError() {
    updateSystemBadges("Erreur", "bad");
  }

  function checkSystemHeartbeat() {
    if (!state.authenticated) return;
    const now = Date.now();
    const maxSilenceMs = state.hasLiveFeed ? 3000 : 12000;
    const lastSeenMs = state.hasLiveFeed
      ? state.lastContactMs
      : Math.max(Number(state.lastContactMs || 0), Number(state.lastStatusContactMs || 0));
    if (!lastSeenMs || (now - lastSeenMs) > maxSilenceMs) {
      setSystemBadgeError();
    }
  }

  function applyLivePayload(st) {
    const liveEventMs = Number(st.sent_ms ?? 0);
    if (liveEventMs > 0) {
      // If the device rebooted and millis restarted near zero, accept the new stream.
      if (state.lastLiveEventMs > 60000 && liveEventMs < 5000) {
        state.lastLiveEventMs = 0;
      }
      if (state.lastLiveEventMs && liveEventMs <= state.lastLiveEventMs) {
        return;
      }
      state.lastLiveEventMs = liveEventMs;
    }

    state.status = { ...(state.status || {}), ...st };
    state.rawAudioLiveStarted = true;
    const merged = state.status;

    const db = Number(st.db ?? merged.db ?? 0);
    const leq = Number(st.leq ?? merged.leq ?? 0);
    const peak = Number(st.peak ?? merged.peak ?? 0);
    const greenMax = Number(merged.greenMax ?? 55);
    const orangeMax = Number(merged.orangeMax ?? 70);
    const warningHoldSec = Number(merged.warningHoldSec ?? 3);
    const criticalHoldSec = Number(merged.criticalHoldSec ?? 2);
    if ("time_ok" in st || "timeUnixMs" in st) syncClock(merged.time_ok ? merged.time : "", merged.timeUnixMs, "live");
    applyRawAudio(merged);

    $("calLiveMic").textContent = merged.analogOk === false
      ? "Micro live: indisponible"
      : `Micro live: ${db.toFixed(1)} dB`;

    if (!isDashboardLivePage()) {
      return;
    }

    updateGauge(db, greenMax, orangeMax);
    updateMetrics(leq, peak);
    updateAlertState(db, greenMax, orangeMax, warningHoldSec, criticalHoldSec);
    appendHistory(db);
  }

  function applyStatus(st, options = {}) {
    state.status = { ...(state.status || {}), ...st };
    const merged = state.status;
    state.historyMinutes = Number(merged.historyMinutes ?? state.historyMinutes ?? 5);
    state.historyCapacity = Number(merged.historyCapacity ?? state.historyCapacity ?? 96);
    state.historySamplePeriodMs = Number(merged.historySamplePeriodMs ?? state.historySamplePeriodMs ?? 3000);
    state.pinConfigured = Boolean(merged.pinConfigured);
    state.liveEnabled = Boolean(merged.liveEnabled);
    if ("dashboardPage" in merged && !state.dashboardDisplayDirty) {
      state.dashboardPage = getDashboardPageValue(merged.dashboardPage);
      $("dashboardPageAdv").value = String(state.dashboardPage);
    }
    if ("touchEnabled" in merged && !state.dashboardDisplayDirty) {
      state.touchEnabled = Boolean(merged.touchEnabled);
      setDashboardTouchEnabled(state.touchEnabled);
    }
    if ("dashboardFullscreenMask" in merged && !state.dashboardDisplayDirty) {
      state.dashboardFullscreenMask = getDashboardFullscreenMaskValue(merged.dashboardFullscreenMask);
      applyDashboardFullscreenMask(state.dashboardFullscreenMask);
    }
    if ("supportsTardisControl" in merged) state.supportsTardisControl = Boolean(merged.supportsTardisControl);
    if ("supportsTardisInteriorRgb" in merged) state.supportsTardisInteriorRgb = Boolean(merged.supportsTardisInteriorRgb);
    if ("tardisInteriorLedPin" in merged) state.tardisInteriorLedPin = Number(merged.tardisInteriorLedPin ?? state.tardisInteriorLedPin ?? 15);
    if ("tardisExteriorLedPin" in merged) state.tardisExteriorLedPin = Number(merged.tardisExteriorLedPin ?? state.tardisExteriorLedPin ?? 16);
    if ("tardisInteriorRgbPin" in merged) state.tardisInteriorRgbPin = Number(merged.tardisInteriorRgbPin ?? state.tardisInteriorRgbPin ?? 48);
    if (!state.tardisDirty) {
      if ("tardisModeEnabled" in merged) state.tardisModeEnabled = Boolean(merged.tardisModeEnabled);
      if ("tardisInteriorLedEnabled" in merged) state.tardisInteriorLedEnabled = Boolean(merged.tardisInteriorLedEnabled);
      if ("tardisExteriorLedEnabled" in merged) state.tardisExteriorLedEnabled = Boolean(merged.tardisExteriorLedEnabled);
      if ("tardisInteriorRgbMode" in merged) state.tardisInteriorRgbMode = Number(merged.tardisInteriorRgbMode ?? state.tardisInteriorRgbMode ?? 0);
      if ("tardisInteriorRgbColor" in merged) state.tardisInteriorRgbColor = Number(merged.tardisInteriorRgbColor ?? state.tardisInteriorRgbColor ?? 0x2D9CDB);
      syncTardisUi();
    }

    const db = Number(merged.db ?? 0);
    const leq = Number(merged.leq ?? 0);
    const peak = Number(merged.peak ?? 0);
    const greenMax = Number(merged.greenMax ?? 55);
    const orangeMax = Number(merged.orangeMax ?? 70);
    const warningHoldSec = Number(merged.warningHoldSec ?? 3);
    const criticalHoldSec = Number(merged.criticalHoldSec ?? 2);

    if (!state.rawAudioLiveStarted && ("rawRms" in merged || "rawPseudoDb" in merged || "rawAdcMean" in merged || "rawAdcLast" in merged)) {
      applyRawAudio(merged);
      state.rawAudioSeededFromStatus = true;
    }

    updateGauge(db, greenMax, orangeMax);
    updateMetrics(leq, peak);
    applyLiveActionState();
    updateAlertState(db, greenMax, orangeMax, warningHoldSec, criticalHoldSec);

    if ("time_ok" in st || "timeUnixMs" in st) syncClock(merged.time_ok ? merged.time : "", merged.timeUnixMs, "status");
    updateStatusSummary(merged);
    if ("releaseUpdateChecked" in st || "releaseInstallStatus" in st) {
      applyReleaseStatus(merged);
    }
    applyPinState();
    updateHistoryLabels();
    if ("calibrationPointCount" in merged) {
      state.calibrationPointCount = getCalibrationPointCount(merged.calibrationPointCount);
      syncCalibrationModeButtons();
    }

    if (Array.isArray(merged.history) && (options.useHistorySnapshot || !state.historyInitialized)) {
      setHistory(merged.history);
    } else if (!options.skipAppendHistory && state.historyInitialized) {
      appendHistory(db);
    }

    $("calLiveMic").textContent = merged.analogOk
      ? `Micro live: ${Number(merged.db ?? 0).toFixed(1)} dB`
      : "Micro live: indisponible";
    $("calLiveLog").textContent = merged.analogOk
      ? `Log calibration live: ${Math.log10((Number(merged.rawRms ?? 0) + 0.0001)).toFixed(4)}`
      : (state.uiAudioSourceSupportsCalibration ? "Log calibration live: --" : "Log calibration live: indisponible en mode Demo");

    if (Array.isArray(merged.cal)) {
      let validCount = 0;
      merged.cal.forEach((point, index) => {
        const i = index + 1;
        if (!state.calRefsDirty[index]) {
          state.calRefs[index] = Number(point.refDb ?? state.calRefs[index] ?? 0);
        }
        const refEl = $(`calRef${i}`);
        const stateEl = $(`calState${i}`);
        if (!refEl || !stateEl) return;
        refEl.textContent = Number(state.calRefs[index] ?? 0).toFixed(0);
        if (index < state.calibrationPointCount && point.valid) {
          validCount++;
          stateEl.textContent = `Point ${i} capture ${Number(point.rawLogRms ?? 0).toFixed(3)}`;
        } else {
          stateEl.textContent = `Point ${i} non capture`;
        }
      });
      $("calStatus").textContent = `${validCount} / ${state.calibrationPointCount} points valides`;
    }

    if (!state.uiDirty) {
      if ("backlight" in merged) {
        const backlightOn = Number(merged.backlight) > 0;
        $("bl").classList.toggle("active", backlightOn);
        $("bl").setAttribute("aria-pressed", backlightOn ? "true" : "false");
      }
      if ("greenMax" in merged) $("g").value = greenMax;
      if ("orangeMax" in merged) $("o").value = orangeMax;
      if ("historyMinutes" in merged) $("hist").value = state.historyMinutes;
      if ("warningHoldSec" in merged) $("warnHoldSec").value = warningHoldSec;
      if ("criticalHoldSec" in merged) $("critHoldSec").value = criticalHoldSec;
      if ("calibrationCaptureSec" in merged) $("calCaptureSec").value = Number(merged.calibrationCaptureSec ?? 3);
      if ("hasScreen" in merged) state.hasScreen = Boolean(merged.hasScreen);
      if ("supportsDashboardDisplay" in merged) state.supportsDashboardDisplay = Boolean(merged.supportsDashboardDisplay);
      if ("supportsDashboardPin" in merged) state.supportsDashboardPin = Boolean(merged.supportsDashboardPin);
      if ("audioSource" in merged) state.uiAudioSource = Number(merged.audioSource ?? 1);
      if ("audioSourceSupportsCalibration" in merged) state.uiAudioSourceSupportsCalibration = Boolean(merged.audioSourceSupportsCalibration);
      if ("audioSourceUsesAnalog" in merged) state.uiAudioSourceUsesAnalog = Boolean(merged.audioSourceUsesAnalog);
      if ("audioResponseMode" in merged) state.uiResponseMode = Number(merged.audioResponseMode ?? 0);
      syncUiLabels();
      applyHardwareCapabilities();
      syncCalibrationAvailability();
    }
  }

  function syncUiLabels() {
    const g = Number($("g").value);
    const o = Math.max(g, Number($("o").value));
    $("o").value = o;

    const backlightOn = $("bl").classList.contains("active");
    $("bl").setAttribute("aria-pressed", backlightOn ? "true" : "false");
    $("blVal").textContent = backlightOn ? "ON" : "OFF";
    $("gVal").textContent = g;
    $("oVal").textContent = o;
    $("hVal").textContent = $("hist").value;
    $("audioSourceSelect").value = String(state.uiAudioSource);
    if (state.uiAudioSource === 0) {
      $("audioSourceHint").textContent = "Demo simule des valeurs et desactive la calibration micro.";
    } else if (state.uiAudioSource === 1) {
      $("audioSourceHint").textContent = "Analog Mic utilise l'entree micro analogique configuree sur la carte.";
    } else if (state.uiAudioSource === 2) {
      $("audioSourceHint").textContent = "PDM MEMS utilise 2 fils: CLK et DATA.";
    } else {
      $("audioSourceHint").textContent = "INMP441 utilise 3 fils: BCLK, WS/LRCLK et SD.";
    }
    $("modeFast").classList.toggle("active", state.uiResponseMode === 0);
    $("modeSlow").classList.toggle("active", state.uiResponseMode === 1);
  }

  function applyHardwareCapabilities() {
    const displayBlock = $("settings-display");
    const pinBlock = $("settings-pin");
    const tardisBlock = $("settings-tardis");
    if (displayBlock) displayBlock.style.display = state.supportsDashboardDisplay ? "" : "none";
    if (pinBlock) pinBlock.style.display = state.supportsDashboardPin ? "" : "none";
    if (tardisBlock) tardisBlock.style.display = state.supportsTardisControl ? "" : "none";
  }

  function syncCalibrationAvailability() {
    const enabled = Boolean(state.uiAudioSourceSupportsCalibration);
    document.querySelectorAll("[data-cal-capture]").forEach((btn) => {
      btn.disabled = !enabled;
    });
  }

  function markUiDirty() {
    state.uiDirty = true;
  }

  class ApiError extends Error {
    constructor(message, status) {
      super(message);
      this.status = status;
    }
  }

  async function parseApiError(res) {
    const raw = await res.text();
    let message = raw || `HTTP ${res.status}`;
    try {
      const parsed = JSON.parse(raw);
      if (parsed && parsed.error) message = parsed.error;
    } catch (err) {
    }
    return new ApiError(message, res.status);
  }

  async function handleUnauthorized() {
    if (state.events) {
      state.events.close();
      state.events = null;
    }
    stopDebugLogsPolling();
    state.debugLogsOpen = false;
    state.debugLogsLoadedOnce = false;
    updateDebugLogUi();
    state.authenticated = false;
    state.currentUser = "";
    state.liveToken = "";
    state.users = [];
    renderAuthState();
    try {
      await loadAuthStatus();
    } catch (err) {
    }
  }

  async function apiGet(url) {
    const res = await fetch(url, { cache: "no-store" });
    if (!res.ok) {
      const err = await parseApiError(res);
      if (err.status === 401) await handleUnauthorized();
      throw err;
    }
    return await res.json();
  }

  async function apiPost(url, payload) {
    const res = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload || {}),
    });
    if (!res.ok) {
      const err = await parseApiError(res);
      if (err.status === 401) await handleUnauthorized();
      throw err;
    }
    return await res.json();
  }

  function setToast(id, message) {
    $(id).textContent = message;
  }

  function setToastError(id, err) {
    setToast(id, `Erreur: ${err.message}`);
  }

  function updateDebugLogUi() {
    const open = Boolean(state.debugLogsOpen);
    $("debugConsoleWrap").classList.toggle("hidden", !open);
    $("toggleDebugLogsBtn").textContent = open ? "Masquer logs" : "Logs";
  }

  function stopDebugLogsPolling() {
    if (!state.debugLogsPollTimer) return;
    clearInterval(state.debugLogsPollTimer);
    state.debugLogsPollTimer = null;
  }

  function startDebugLogsPolling() {
    stopDebugLogsPolling();
    state.debugLogsPollTimer = setInterval(() => {
      if (!state.authenticated || !state.debugLogsOpen) return;
      loadDebugLogs(false).catch(() => {});
    }, 2500);
  }

  async function loadDebugLogs(showToast = false) {
    const payload = await apiGet("/api/debug/logs");
    const box = $("debugLogsBox");
    const stickToBottom = !state.debugLogsLoadedOnce || (box.scrollTop + box.clientHeight + 32 >= box.scrollHeight);
    box.textContent = payload.tail || "(aucun log capture pour le moment)";
    $("debugLogsMeta").textContent = `${payload.lineCount || 0} ligne(s) | uptime ${Math.max(0, Number(payload.uptime_s || 0))} s | Wi-Fi ${payload.wifiConnected ? "connecte" : "deconnecte"}`;
    $("debugLogsHint").textContent = `Dernier refresh: ${new Date().toLocaleTimeString()}`;
    if (stickToBottom) box.scrollTop = box.scrollHeight;
    state.debugLogsLoadedOnce = true;
    if (showToast) setToast("debugLogsToast", "Logs charges.");
    return payload;
  }

  async function toggleDebugLogs() {
    state.debugLogsOpen = !state.debugLogsOpen;
    updateDebugLogUi();
    if (!state.debugLogsOpen) {
      stopDebugLogsPolling();
      return;
    }
    await runToastRequest("debugLogsToast", "Chargement logs...", () => loadDebugLogs(false), "Logs prets.");
    startDebugLogsPolling();
  }

  async function refreshDebugLogs() {
    if (!state.debugLogsOpen) {
      state.debugLogsOpen = true;
      updateDebugLogUi();
    }
    await runToastRequest("debugLogsToast", "Rafraichissement logs...", () => loadDebugLogs(false), "Logs mis a jour.");
    startDebugLogsPolling();
  }

  async function clearDebugLogs() {
    await runToastRequest("debugLogsToast", "Vidage logs...", () => apiPost("/api/debug/logs/clear", {}), "Logs vides.");
    state.debugLogsLoadedOnce = false;
    if (state.debugLogsOpen) {
      await loadDebugLogs(false);
      startDebugLogsPolling();
    } else {
      $("debugLogsBox").textContent = "(buffer vide)";
      $("debugLogsMeta").textContent = "0 ligne | buffer vide";
      $("debugLogsHint").textContent = "Mode debug web";
    }
  }

  function setFieldValue(id, value, fallback = "") {
    $(id).value = value || fallback;
  }

  function setNumericFieldValue(id, value, fallback) {
    $(id).value = value || fallback;
  }

  function setBoolSelectValue(id, enabled) {
    $(id).value = enabled ? "1" : "0";
  }

  function getFieldValue(id) {
    return $(id).value || "";
  }

  function getTrimmedValue(id) {
    return getFieldValue(id).trim();
  }

  function getIntValue(id, fallback = 0) {
    return parseInt(getFieldValue(id) || String(fallback), 10);
  }

  function getNumberValue(id, fallback = 0) {
    return Number(getFieldValue(id) || fallback);
  }

  function resetPasswordField(id, configuredPlaceholder, emptyPlaceholder, configured) {
    $(id).value = "";
    $(id).placeholder = configured ? configuredPlaceholder : emptyPlaceholder;
  }

  async function postAction(url, successMessage, toastId = "actionsToast") {
    try {
      await apiPost(url, {});
      setToast(toastId, successMessage);
    } catch (err) {
      setToastError(toastId, err);
    }
  }

  async function runToastRequest(toastId, pendingMessage, request, successMessage, afterSuccess = null) {
    setToast(toastId, pendingMessage);
    try {
      const result = await request();
      setToast(toastId, typeof successMessage === "function" ? successMessage(result) : successMessage);
      if (afterSuccess) await afterSuccess(result);
      return result;
    } catch (err) {
      setToastError(toastId, err);
      return null;
    }
  }

  async function runConfigRequest(pendingMessage, request, successMessage, reloadPanels = false) {
    return await runToastRequest(
      "configToast",
      pendingMessage,
      request,
      successMessage,
      reloadPanels ? () => refreshSettingsPanels() : () => refreshStatus()
    );
  }

  async function loadAuthStatus() {
    const auth = await apiGet("/api/auth/status");
    state.authenticated = Boolean(auth.authenticated);
    state.bootstrapRequired = Boolean(auth.bootstrapRequired);
    state.currentUser = auth.currentUser || "";
    state.liveToken = auth.liveToken || "";
    state.userCount = Number(auth.userCount || 0);
    if (!state.authenticated) {
      state.settingsPanelsLoaded = false;
      state.settingsPanelsLoading = false;
      state.rawAudioSeededFromStatus = false;
      state.rawAudioLiveStarted = false;
    }
    renderAuthState();
    if (state.authenticated && state.liveToken) connectLiveFeed();
    return auth;
  }

  async function submitAuth() {
    const username = sanitizeUsernameValue(getFieldValue("authUsername"));
    const password = getFieldValue("authPassword");
    $("authUsername").value = username;

    if (!username) {
      setToast("authToast", "Login requis.");
      return;
    }
    const passwordHint = passwordPolicyHint(password);
    if (state.bootstrapRequired && passwordHint) {
      setToast("authToast", passwordHint);
      return;
    }

    await runToastRequest(
      "authToast",
      state.bootstrapRequired ? "Creation du compte..." : "Connexion...",
      () => apiPost(state.bootstrapRequired ? "/api/auth/bootstrap" : "/api/auth/login", { username, password }),
      state.bootstrapRequired ? "Compte cree." : "Connexion reussie.",
      (result) => {
        $("authPassword").value = "";
        if (state.events) {
          state.events.close();
          state.events = null;
        }
        state.settingsPanelsLoaded = false;
        state.settingsPanelsLoading = false;
        state.rawAudioSeededFromStatus = false;
        state.rawAudioLiveStarted = false;
        state.authenticated = true;
        state.bootstrapRequired = false;
        state.currentUser = result?.currentUser || username;
        state.liveToken = result?.liveToken || "";
        renderAuthState();
        if (state.liveToken) connectLiveFeed();
        setTimeout(() => {
          refreshStatus();
          loadAuthStatus();
          if (state.currentPage === "settings") {
            ensureSettingsPanelsLoaded();
          }
        }, 0);
      }
    );
  }

  async function logout() {
    stopDebugLogsPolling();
    state.debugLogsOpen = false;
    state.debugLogsLoadedOnce = false;
    updateDebugLogUi();
    await runToastRequest("usersToast", "Deconnexion...", () => apiPost("/api/auth/logout", {}), "Session fermee.", async () => {
      if (state.events) {
        state.events.close();
        state.events = null;
      }
      state.authenticated = false;
      state.currentUser = "";
      state.liveToken = "";
      state.users = [];
      state.hasLiveFeed = false;
      state.historyInitialized = false;
      state.rawAudioSeededFromStatus = false;
      state.rawAudioLiveStarted = false;
      renderAuthState();
      await loadAuthStatus();
    });
  }

  async function loadUsers() {
    if (!state.authenticated) return;
    try {
      const data = await apiGet("/api/users");
      state.currentUser = data.currentUser || state.currentUser || "";
      state.userCount = Number(data.userCount || 0);
      state.maxUsers = Number(data.maxUsers || 4);
      state.users = Array.isArray(data.users) ? data.users : [];
      renderUsersPanel();
    } catch (err) {
      setToastError("usersToast", err);
    }
  }

  async function createWebUser() {
    const username = sanitizeUsernameValue(getFieldValue("newWebUsername"));
    const password = getFieldValue("newWebPassword");
    $("newWebUsername").value = username;
    const passwordHint = passwordPolicyHint(password);
    if (!username) {
      setToast("usersToast", "Login invalide.");
      return;
    }
    if (passwordHint) {
      setToast("usersToast", passwordHint);
      return;
    }

    await runToastRequest("usersToast", "Creation utilisateur...", () => apiPost("/api/users/create", { username, password }),
      "Utilisateur cree.",
      async () => {
        $("newWebUsername").value = "";
        $("newWebPassword").value = "";
        await loadUsers();
      });
  }

  async function updateWebPassword() {
    const username = sanitizeUsernameValue(getFieldValue("manageWebUserSelect"));
    const password = getFieldValue("manageWebPassword");
    const passwordHint = passwordPolicyHint(password);
    if (!username) {
      setToast("usersToast", "Choisis un utilisateur.");
      return;
    }
    if (passwordHint) {
      setToast("usersToast", passwordHint);
      return;
    }

    await runToastRequest("usersToast", "Mise a jour mot de passe...", () => apiPost("/api/users/password", { username, password }),
      "Mot de passe mis a jour. Les anciennes sessions de cet utilisateur sont fermees.",
      async () => {
        $("manageWebPassword").value = "";
        if (username === state.currentUser) {
          if (state.events) {
            state.events.close();
            state.events = null;
          }
          state.authenticated = false;
          state.currentUser = "";
          state.liveToken = "";
          state.users = [];
          state.rawAudioSeededFromStatus = false;
          state.rawAudioLiveStarted = false;
          renderAuthState();
          await loadAuthStatus();
          return;
        }
        await loadUsers();
      });
  }

  async function deleteWebUser(usernameOverride = "") {
    const username = sanitizeUsernameValue(usernameOverride || getFieldValue("manageWebUserSelect"));
    if (!username) {
      setToast("usersToast", "Choisis un utilisateur.");
      return;
    }
    if (!confirm(`Supprimer l'utilisateur ${username} ?`)) return;

    await runToastRequest("usersToast", "Suppression utilisateur...", () => apiPost("/api/users/delete", { username }),
      "Utilisateur supprime.",
      async () => {
        if (username === state.currentUser) {
          if (state.events) {
            state.events.close();
            state.events = null;
          }
          state.authenticated = false;
          state.currentUser = "";
          state.liveToken = "";
          state.users = [];
          state.rawAudioSeededFromStatus = false;
          state.rawAudioLiveStarted = false;
          renderAuthState();
          await loadAuthStatus();
          return;
        }
        await loadUsers();
        await loadAuthStatus();
      });
  }

  async function refreshStatus(force = true) {
    if (!state.authenticated) return;
    const backgroundRefresh = !force;
    const now = Date.now();
    if (backgroundRefresh) {
      if (state.currentPage !== "settings") return;
      if (state.settingsPanelsLoading) return;
      if (state.hasLiveFeed && (now - state.lastBackgroundStatusRefreshMs) < 20000) return;
      state.lastBackgroundStatusRefreshMs = now;
    }
    try {
      const st = await apiGet("/api/status");
      setSystemBadgeOnline("status");
      applyStatus(st, {
        skipAppendHistory: true,
        useHistorySnapshot: !state.historyInitialized
      });
    } catch (err) {
      // Let the heartbeat decide when the system is truly stale.
    }
  }

  async function refreshSystemSummary(force = false) {
    if (!state.authenticated) return;
    if (!force && state.currentPage !== "settings") return;
    try {
      const st = await apiGet("/api/system");
      setSystemBadgeOnline("status");
      applySystemPayload(st);
    } catch (err) {
    }
  }

  function scheduleReconnect() {
    if (!state.authenticated || !state.liveToken || state.reconnectTimer) return;
    state.reconnectTimer = setTimeout(() => {
      state.reconnectTimer = null;
      connectLiveFeed();
    }, 500);
  }

  function connectLiveFeed() {
    if (!state.authenticated || !state.liveToken) return;
    if (state.events) {
      state.events.close();
      state.events = null;
    }

    const baseUrl = `${location.protocol}//${location.hostname}:81/api/events`;
    const url = `${baseUrl}?t=${encodeURIComponent(state.liveToken)}`;
    state.events = new EventSource(url);
    state.events.onopen = () => {
      state.hasLiveFeed = true;
      setSystemBadgeOnline("live");
    };
    state.events.addEventListener("metrics", (ev) => {
      try {
        const payload = JSON.parse(ev.data);
        state.hasLiveFeed = true;
        setSystemBadgeOnline("live");
        applyLivePayload(payload);
      } catch (err) {
        state.hasLiveFeed = false;
        refreshStatus();
      }
    });
    state.events.onerror = () => {
      state.hasLiveFeed = false;
      if (!state.events) return;
      if (state.events.readyState === EventSource.CLOSED) {
        state.events.close();
        state.events = null;
        scheduleReconnect();
      }
    };
  }

  async function saveUi() {
    const payload = {
      backlight: $("bl").classList.contains("active") ? 100 : 0,
      greenMax: getNumberValue("g"),
      orangeMax: getNumberValue("o"),
      historyMinutes: getNumberValue("hist"),
      audioSource: state.uiAudioSource,
      audioResponseMode: state.uiResponseMode,
      warningHoldSec: getNumberValue("warnHoldSec", 0),
      criticalHoldSec: getNumberValue("critHoldSec", 0),
      calibrationCaptureSec: getNumberValue("calCaptureSec", 3),
    };

    try {
      await apiPost("/api/ui", payload);
      setToast("uiToast", "UI sauvee.");
      state.uiDirty = false;
      await refreshStatus();
    } catch (err) {
      setToastError("uiToast", err);
    }
  }

  async function saveDashboardPage() {
    const dashboardPage = getDashboardPageValue(getFieldValue("dashboardPageAdv"));
    const touchEnabled = readDashboardTouchEnabled();
    const dashboardFullscreenMask = readDashboardFullscreenMask();
    $("dashboardPageAdv").value = String(dashboardPage);

    try {
      await apiPost("/api/ui", { dashboardPage, dashboardFullscreenMask, touchEnabled });
      state.dashboardPage = dashboardPage;
      state.touchEnabled = touchEnabled;
      state.dashboardFullscreenMask = dashboardFullscreenMask;
      state.dashboardDisplayDirty = false;
      setToast("dashboardPageToast", "Affichage tactile mis a jour.");
      await refreshStatus();
    } catch (err) {
      setToastError("dashboardPageToast", err);
    }
  }

  async function saveTardisSettings() {
    const payload = {
      tardisModeEnabled: state.tardisModeEnabled,
      tardisInteriorLedEnabled: state.tardisInteriorLedEnabled,
      tardisExteriorLedEnabled: state.tardisExteriorLedEnabled,
      tardisInteriorRgbMode: Number(state.tardisInteriorRgbMode || 0),
      tardisInteriorRgbColorHex: $("tardisInteriorRgbColor").value || "#2D9CDB",
    };

    try {
      await apiPost("/api/ui", payload);
      state.tardisDirty = false;
      setToast("tardisToast", "Mode TARDIS mis a jour.");
      await refreshStatus();
    } catch (err) {
      setToastError("tardisToast", err);
    }
  }

  function clearProtectedSettingsFields() {
    $("accessPinAdv").value = "";
    $("newWebUsername").value = "";
    $("newWebPassword").value = "";
    $("manageWebPassword").value = "";
  }

  async function savePinSettings() {
    const pin = sanitizePinValue(getFieldValue("accessPinAdv"));
    $("accessPinAdv").value = pin;
    if (pin.length < 4 || pin.length > 8) {
      setToast("toastPinAdv", "PIN: 4 a 8 chiffres.");
      return;
    }

    await runToastRequest("toastPinAdv", "Sauvegarde...", () => apiPost("/api/pin", { pin }),
      () => "PIN sauve.",
      async () => {
        $("accessPinAdv").value = "";
        await refreshSettingsPanels();
      });
  }

  async function clearPinSettings() {
    if (!state.pinConfigured) {
      setToast("toastPinAdv", "Aucun PIN actif.");
      return;
    }
    if (!confirm("Desactiver le code PIN ?")) return;

    await runToastRequest("toastPinAdv", "Desactivation...", () => apiPost("/api/pin", { pin: "" }),
      () => "PIN desactive.",
      async () => {
        $("accessPinAdv").value = "";
        await refreshSettingsPanels();
      });
  }

  async function refreshSettingsPanels() {
    if (!state.authenticated) return;
    state.settingsPanelsLoading = true;
    try {
      state.uiDirty = false;
      state.dashboardDisplayDirty = false;
      state.calRefsDirty = [false, false, false, false, false];
      await refreshStatus();
      clearProtectedSettingsFields();
      await loadUsers();
      await loadHomeAssistantSettings();
      await loadTimeSettings();
      await loadWifiSettings();
      await loadOtaSettings();
      await loadReleaseStatus();
      await loadMqttSettings();
      await loadNotificationSettings();
      state.settingsPanelsLoaded = true;
    } finally {
      state.settingsPanelsLoading = false;
    }
  }

  async function ensureSettingsPanelsLoaded() {
    if (!state.authenticated || state.settingsPanelsLoaded || state.settingsPanelsLoading) return;
    await refreshSettingsPanels();
  }

  async function toggleBacklight() {
    const wasOn = $("bl").classList.contains("active");
    const nextOn = !wasOn;

    $("bl").classList.toggle("active", nextOn);
    syncUiLabels();
    setToast("uiToast", "Sauvegarde...");

    try {
      await apiPost("/api/ui", { backlight: nextOn ? 100 : 0 });
      setToast("uiToast", "Backlight sauve.");
      await refreshStatus();
    } catch (err) {
      $("bl").classList.toggle("active", wasOn);
      syncUiLabels();
      setToastError("uiToast", err);
    }
  }

  function downloadTextFile(filename, text) {
    const blob = new Blob([text], { type: "application/json;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  }

  function exportFilename() {
    const d = new Date();
    const pad = (v) => String(v).padStart(2, "0");
    return `soundpanel7-config-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.json`;
  }

  function exportFullFilename() {
    const d = new Date();
    const pad = (v) => String(v).padStart(2, "0");
    return `soundpanel7-config-full-clear-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.json`;
  }

  async function exportConfig() {
    setToast("configToast", "Export...");
    try {
      const cfg = await apiGet("/api/config/export");
      const text = JSON.stringify(cfg, null, 2);
      $("configJsonBox").value = text;
      downloadTextFile(exportFilename(), text);
      setToast("configToast", "Config exportee.");
    } catch (err) {
      setToastError("configToast", err);
    }
  }

  async function exportConfigFull() {
    const confirmed = confirm(
      "Exporter la configuration complete en clair ?\n\n"
      + "Danger: ce fichier contiendra les mots de passe et tokens en texte lisible. "
      + "Ne le partage pas et ne le stocke pas dans Git, dans le cloud ou dans un ticket."
    );
    if (!confirmed) return;

    setToast("configToast", "Export complet dangereux...");
    try {
      const cfg = await apiGet("/api/config/export_full");
      const text = JSON.stringify(cfg, null, 2);
      $("configJsonBox").value = text;
      downloadTextFile(exportFullFilename(), text);
      setToast("configToast", "Config complete exportee en clair. Manipule ce fichier avec prudence.");
    } catch (err) {
      setToastError("configToast", err);
    }
  }

  async function importConfig() {
    const raw = ($("configJsonBox").value || "").trim();
    if (!raw) {
      setToast("configToast", "Colle un JSON ou charge un fichier.");
      return;
    }

    let payload;
    try {
      payload = JSON.parse(raw);
    } catch (err) {
      setToast("configToast", `JSON invalide: ${err.message}`);
      return;
    }

    await runConfigRequest(
      "Import...",
      () => apiPost("/api/config/import", payload),
      (r) => r.rebootRecommended ? "Config importee. Reboot recommande." : "Config importee.",
      true
    );
  }

  async function backupConfig() {
    await runConfigRequest("Backup...", () => apiPost("/api/config/backup", {}), "Backup enregistre.");
  }

  async function restoreConfig() {
    const backupDate = formatBackupDate(state.status?.backupTs);
    if (!confirm(backupDate === "--"
      ? "Restaurer le dernier backup ?"
      : `Restaurer le dernier backup ?\n${backupDate}`)) return;
    await runConfigRequest(
      "Restore...",
      () => apiPost("/api/config/restore", {}),
      (r) => r.rebootRecommended ? "Backup restaure. Reboot recommande." : "Backup restaure.",
      true
    );
  }

  async function partialReset() {
    const scope = $("configResetScope").value;
    if (!confirm(`Reset partiel ${scope} ?`)) return;
    await runConfigRequest(
      "Reset...",
      () => apiPost("/api/config/reset_partial", { scope }),
      (r) => r.rebootRecommended ? `Section ${scope} reset. Reboot recommande.` : `Section ${scope} reset.`,
      true
    );
  }

  async function loadTimeSettings() {
    try {
      const t = await apiGet("/api/time");
      setFieldValue("tzAdv", t.tz);
      setFieldValue("ntpServerAdv", t.ntpServer);
      setNumericFieldValue("ntpSyncMinAdv", String(t.ntpSyncMinutes || 180), 180);
      setFieldValue("hostnameAdv", t.hostname);
    } catch (err) {
      setToastError("toastTimeAdv", err);
    }
  }

  function applyHomeAssistantSettings(config) {
    const configured = Boolean(config.tokenConfigured);
    setSecretField("homeAssistantTokenAdv", configured, "********", "genere ou colle un token");
    $("homeAssistantStatusAdv").textContent = configured
      ? "Token actif"
      : "Token: non configure";
    $("homeAssistantPathAdv").textContent = `Endpoint HA: ${config.statusPath || "/api/ha/status"} (${config.authScheme || "Bearer"})`;
    $("clearHomeAssistantTokenBtn").disabled = !configured;
  }

  async function loadHomeAssistantSettings() {
    try {
      const ha = await apiGet("/api/homeassistant");
      applyHomeAssistantSettings(ha);
    } catch (err) {
      setToastError("toastHomeAssistantAdv", err);
    }
  }

  async function saveHomeAssistantSettings() {
    const token = sanitizeHomeAssistantTokenValue(getFieldValue("homeAssistantTokenAdv"));
    $("homeAssistantTokenAdv").value = token;
    const keepToken = !token && $("homeAssistantTokenAdv").dataset.keepSecret === "1";
    if (!token && !keepToken) {
      setToast("toastHomeAssistantAdv", "Token requis ou utilise Generer.");
      return;
    }

    await runToastRequest("toastHomeAssistantAdv", "Sauvegarde...", () => apiPost("/api/homeassistant", { token, keepToken }),
      "Token Home Assistant sauve.",
      async (result) => {
        applyHomeAssistantSettings(result);
      });
  }

  async function generateHomeAssistantToken() {
    const hasToken = $("homeAssistantTokenAdv").dataset.keepSecret === "1" || Boolean(getTrimmedValue("homeAssistantTokenAdv"));
    if (hasToken && !confirm("Regenerer le token Home Assistant ? L'integration actuelle devra etre mise a jour.")) return;

    await runToastRequest("toastHomeAssistantAdv", "Generation...", () => apiPost("/api/homeassistant", { generate: true }),
      "Token Home Assistant genere.",
      async (result) => {
        applyHomeAssistantSettings(result);
      });
  }

  async function clearHomeAssistantToken() {
    if (!confirm("Revoquer le token Home Assistant ? L'integration native cessera de fonctionner.")) return;

    await runToastRequest("toastHomeAssistantAdv", "Revocation...", () => apiPost("/api/homeassistant", { clear: true }),
      "Token Home Assistant revoque.",
      async (result) => {
        applyHomeAssistantSettings(result);
      });
  }

  async function saveTimeSettings() {
    await runToastRequest("toastTimeAdv", "Sauvegarde...", () => apiPost("/api/time", {
        tz: getTrimmedValue("tzAdv"),
        ntpServer: getTrimmedValue("ntpServerAdv"),
        ntpSyncMinutes: getIntValue("ntpSyncMinAdv"),
        hostname: getTrimmedValue("hostnameAdv")
      }), "Heure sauvee.", () => refreshStatus());
  }

  function setWifiPasswordField(slot, configured, ssid) {
    const id = `wifiPassword${slot}`;
    resetPasswordField(id, "********", "laisser vide = reseau ouvert", configured);
    $(id).dataset.keepPassword = configured ? "1" : "0";
    $(id).dataset.initialSsid = ssid || "";
  }

  async function loadWifiSettings() {
    try {
      const w = await apiGet("/api/wifi");
      const networks = Array.isArray(w.networks) ? w.networks : [];
      for (let slot = 1; slot <= 4; slot++) {
        const network = networks[slot - 1] || {};
        const ssid = String(network.ssid || "");
        setFieldValue(`wifiSsid${slot}`, ssid);
        setWifiPasswordField(slot, Boolean(network.passwordConfigured), ssid);
      }
      const current = w.connected ? (w.currentSsid || "(SSID inconnu)") : "hors ligne";
      $("wifiSummaryAdv").textContent = `Connexion: ${current}`;
    } catch (err) {
      setToastError("toastWifiAdv", err);
    }
  }

  async function saveWifiSettings() {
    const payload = {};
    for (let slot = 1; slot <= 4; slot++) {
      const ssid = getTrimmedValue(`wifiSsid${slot}`);
      const passwordEl = $(`wifiPassword${slot}`);
      payload[`wifi${slot}Ssid`] = ssid;
      payload[`wifi${slot}Password`] = getFieldValue(`wifiPassword${slot}`);
      payload[`wifi${slot}KeepPassword`] =
        !payload[`wifi${slot}Password`]
        && passwordEl.dataset.keepPassword === "1"
        && ssid === (passwordEl.dataset.initialSsid || "");
    }

    await runToastRequest("toastWifiAdv", "Sauvegarde...", () => apiPost("/api/wifi", payload),
      "Wi-Fi sauve. Reconnexion en cours.");
  }

  async function loadOtaSettings() {
    try {
      const o = await apiGet("/api/ota");
      setBoolSelectValue("otaEnabledAdv", o.enabled);
      setFieldValue("otaHostnameAdv", o.hostname);
      setNumericFieldValue("otaPortAdv", o.port, 3232);
      resetPasswordField("otaPasswordAdv", "********", "laisser vide = aucun mot de passe", o.passwordConfigured);
    } catch (err) {
      setToastError("toastOtaAdv", err);
    }
  }

  function applyReleaseStatus(release) {
    if (!$("releaseCurrentVersion")) return;

    const checked = Boolean(release && (release.checked ?? release.releaseUpdateChecked));
    const ok = Boolean(release && (release.ok ?? release.releaseUpdateOk));
    const available = Boolean(release && (release.available ?? release.releaseUpdateAvailable));
    const latestVersion = String((release && (release.latestVersion ?? release.releaseLatestVersion)) || "");
    const error = String((release && (release.error ?? release.releaseLastError)) || "");
    const httpCode = Number((release && (release.httpCode ?? release.releaseHttpCode)) || 0);
    const manifestUrl = String((release && (release.manifestUrl ?? release.releaseManifestUrl)) || "");
    const currentVersion = String((release && (release.currentVersion ?? release.releaseCurrentVersion)) || (state.status && state.status.version) || "--");
    const publishedAt = String((release && (release.publishedAt ?? release.releasePublishedAt)) || "");
    const otaUrl = String((release && (release.otaUrl ?? release.releaseOtaUrl)) || "");
    const otaSha256 = String((release && (release.otaSha256 ?? release.releaseOtaSha256)) || "");
    const installing = Boolean(release && (release.installing ?? release.releaseInstallInProgress));
    const installFinished = Boolean(release && (release.installFinished ?? release.releaseInstallFinished));
    const installSucceeded = Boolean(release && (release.installSucceeded ?? release.releaseInstallSucceeded));
    const installStatus = String((release && (release.installStatus ?? release.releaseInstallStatus)) || "idle");
    const installError = String((release && (release.installError ?? release.releaseInstallError)) || "");
    const installProgressPct = Number((release && (release.installProgressPct ?? release.releaseInstallProgressPct)) || 0);
    const installWrittenBytes = Number((release && (release.installWrittenBytes ?? release.releaseInstallWrittenBytes)) || 0);
    const installTotalBytes = Number((release && (release.installTotalBytes ?? release.releaseInstallTotalBytes)) || 0);
    const hasInstallPayload = otaUrl.length > 0 && otaSha256.length === 64;

    setTextIfPresent("releaseCurrentVersion", currentVersion || "--");
    setTextIfPresent("releaseLastCheck", checked ? formatEpoch(release.checkedAt ?? release.releaseUpdateCheckedAt) : "Jamais");
    setTextIfPresent("releaseLatestVersion", latestVersion || "--");
    setTextIfPresent("releasePublishedAt", publishedAt || "--");
    setTextIfPresent("releaseManifestUrl", manifestUrl || "--");

    let stateText = "En attente";
    if (release && release.busy && !installing) {
      stateText = "Verification en cours";
    } else if (!checked) {
      stateText = "Aucune verification encore";
    } else if (!ok) {
      stateText = error ? `Erreur: ${error}` : "Echec de verification";
      if (httpCode > 0) stateText += ` (HTTP ${httpCode})`;
    } else if (available) {
      stateText = latestVersion ? `Mise a jour dispo: ${latestVersion}` : "Mise a jour disponible";
    } else {
      stateText = "Firmware a jour";
    }
    setTextIfPresent("releaseState", stateText);

    let installStateText = "Inactive";
    if (installing) {
      installStateText = installStatus === "verifying"
        ? "Verification SHA-256"
        : (installStatus === "rebooting" ? "Installation terminee, reboot..." : "Installation en cours");
    } else if (installFinished) {
      installStateText = installSucceeded ? "Installation terminee" : `Echec: ${installError || installStatus}`;
    }
    setTextIfPresent("releaseInstallState", installStateText);

    const installProgressText = installTotalBytes > 0
      ? `${installProgressPct}% (${formatBytes(installWrittenBytes)} / ${formatBytes(installTotalBytes)})`
      : `${installProgressPct}%`;
    setTextIfPresent("releaseInstallProgress", installProgressText);

    let hint = "Check manuel du dernier firmware publie sur GitHub.";
    if (installing) {
      hint = installTotalBytes > 0
        ? `Installation ${installProgressPct}% - ${formatBytes(installWrittenBytes)} / ${formatBytes(installTotalBytes)}`
        : "Installation en cours...";
    } else if (installFinished && installSucceeded) {
      hint = "Firmware installe. Redemarrage automatique en cours.";
    } else if (installFinished && installError) {
      hint = installError;
    } else if (ok && available) {
      hint = otaUrl
        ? `Firmware pret a installer: ${otaUrl}`
        : "Une release plus recente est disponible.";
    } else if (ok && !available) {
      hint = hasInstallPayload
        ? "Aucune release plus recente detectee. Tu peux forcer la reinstallation du firmware GitHub."
        : "Aucune release plus recente detectee.";
    } else if (checked && error) {
      hint = error;
    }
    setTextIfPresent("releaseHint", hint);

    const installBtn = $("installReleaseBtn");
    if (installBtn) {
      const canInstall = ok && available && !installing;
      installBtn.disabled = !canInstall;
      installBtn.textContent = installing ? "Installation..." : "Installer";
    }

    const forceInstallBtn = $("forceInstallReleaseBtn");
    if (forceInstallBtn) {
      const canForceInstall = ok && hasInstallPayload && !installing;
      forceInstallBtn.disabled = !canForceInstall;
      forceInstallBtn.textContent = installing ? "Installation..." : "Forcer l'installation";
    }

    syncReleaseInstallPolling(release);
  }

  function stopReleaseInstallPolling() {
    if (state.releaseInstallPollTimer) {
      clearInterval(state.releaseInstallPollTimer);
      state.releaseInstallPollTimer = null;
    }
    state.releaseInstallPollPending = false;
  }

  async function pollReleaseInstallStatusOnce() {
    if (!state.authenticated) {
      stopReleaseInstallPolling();
      return;
    }
    if (state.releaseInstallPollPending) return;

    state.releaseInstallPollPending = true;
    try {
      const release = await apiGet("/api/release");
      applyReleaseStatus(release);
      const installing = Boolean(release && (release.installing ?? release.releaseInstallInProgress));
      if (!installing) {
        stopReleaseInstallPolling();
        await refreshStatus();
      }
    } catch (err) {
    }
    state.releaseInstallPollPending = false;
  }

  function syncReleaseInstallPolling(release) {
    const installing = Boolean(release && (release.installing ?? release.releaseInstallInProgress));
    if (!installing) {
      stopReleaseInstallPolling();
      return;
    }
    if (state.releaseInstallPollTimer) return;
    state.releaseInstallPollTimer = setInterval(pollReleaseInstallStatusOnce, 250);
    pollReleaseInstallStatusOnce();
  }

  async function loadReleaseStatus() {
    try {
      const release = await apiGet("/api/release");
      applyReleaseStatus(release);
    } catch (err) {
      setToastError("toastReleaseAdv", err);
    }
  }

  async function checkReleaseNow() {
    await runToastRequest(
      "toastReleaseAdv",
      "Verification GitHub...",
      () => apiPost("/api/release/check", {}),
      (release) => {
        applyReleaseStatus(release);
        if (release && release.ok && release.available) {
          return `Nouvelle version detectee: ${release.latestVersion || "inconnue"}.`;
        }
        if (release && release.ok) return "Firmware deja a jour.";
        return "Verification terminee.";
      },
      async (release) => {
        applyReleaseStatus(release);
        await refreshStatus();
      }
    );
  }

  async function installReleaseNow() {
    if (!confirm("Installer le firmware trouve sur GitHub puis redemarrer le panneau ?")) return;
    applyReleaseStatus({
      ...(state.status || {}),
      checked: true,
      ok: true,
      available: true,
      installing: true,
      installFinished: false,
      installSucceeded: false,
      installStatus: "starting",
      installError: "",
      installProgressPct: 0,
      installWrittenBytes: 0,
      installTotalBytes: 0,
    });
    await runToastRequest(
      "toastReleaseAdv",
      "Lancement installation...",
      () => apiPost("/api/release/install", {}),
      (release) => {
        applyReleaseStatus(release);
        return "Installation OTA demarree.";
      },
      async (release) => {
        applyReleaseStatus(release);
        await refreshStatus();
      }
    );
  }

  async function forceInstallReleaseNow() {
    if (!confirm("Forcer la reinstallation du firmware GitHub verifie puis redemarrer le panneau, meme si la version est deja a jour ?")) return;
    applyReleaseStatus({
      ...(state.status || {}),
      checked: true,
      ok: true,
      available: true,
      installing: true,
      installFinished: false,
      installSucceeded: false,
      installStatus: "starting",
      installError: "",
      installProgressPct: 0,
      installWrittenBytes: 0,
      installTotalBytes: 0,
    });
    await runToastRequest(
      "toastReleaseAdv",
      "Lancement reinstallation...",
      () => apiPost("/api/release/install", { force: true }),
      (release) => {
        applyReleaseStatus(release);
        return "Reinstallation OTA forcee demarree.";
      },
      async (release) => {
        applyReleaseStatus(release);
        await refreshStatus();
      }
    );
  }

  async function saveOtaSettings() {
    await runToastRequest("toastOtaAdv", "Sauvegarde...", () => apiPost("/api/ota", {
        enabled: getIntValue("otaEnabledAdv"),
        hostname: getTrimmedValue("otaHostnameAdv"),
        port: getIntValue("otaPortAdv"),
        password: getFieldValue("otaPasswordAdv")
      }), (r) => r.rebootRequired ? "OTA sauvee. Reboot requis." : "OTA sauvee.");
  }

  async function loadMqttSettings() {
    try {
      const m = await apiGet("/api/mqtt");
      setBoolSelectValue("mqttEnabledAdv", m.enabled);
      setFieldValue("mqttHostAdv", m.host);
      setNumericFieldValue("mqttPortAdv", m.port, 1883);
      setFieldValue("mqttUsernameAdv", m.username);
      resetPasswordField("mqttPasswordAdv", "********", "laisser vide si non utilise", m.passwordConfigured);
      $("mqttPasswordAdv").dataset.keepPassword = m.passwordConfigured ? "1" : "0";
      setFieldValue("mqttClientIdAdv", m.clientId, "soundpanel7");
      setFieldValue("mqttBaseTopicAdv", m.baseTopic, "soundpanel7");
      setNumericFieldValue("mqttPublishPeriodMsAdv", m.publishPeriodMs, 1000);
      setBoolSelectValue("mqttRetainAdv", m.retain);
    } catch (err) {
      setToastError("toastMqttAdv", err);
    }
  }

  async function saveMqttSettings() {
    await runToastRequest("toastMqttAdv", "Sauvegarde...", () => apiPost("/api/mqtt", {
        enabled: getIntValue("mqttEnabledAdv", 0),
        host: getTrimmedValue("mqttHostAdv"),
        port: getIntValue("mqttPortAdv", 1883),
        username: getTrimmedValue("mqttUsernameAdv"),
        password: getFieldValue("mqttPasswordAdv"),
        keepPassword: !getFieldValue("mqttPasswordAdv") && $("mqttPasswordAdv").dataset.keepPassword === "1",
        clientId: getTrimmedValue("mqttClientIdAdv"),
        baseTopic: getTrimmedValue("mqttBaseTopicAdv"),
        publishPeriodMs: getIntValue("mqttPublishPeriodMsAdv", 1000),
        retain: getIntValue("mqttRetainAdv", 0)
      }), (r) => r.rebootRecommended ? "MQTT sauve. Reboot recommande." : "MQTT sauve.");
  }

  function formatEpoch(ts) {
    const value = Number(ts || 0);
    if (!value) return "--";
    return new Date(value * 1000).toLocaleString();
  }

  function formatBytes(bytes) {
    const value = Number(bytes || 0);
    if (!value) return "0 B";
    if (value >= 1024 * 1024) return `${(value / (1024 * 1024)).toFixed(2)} MB`;
    if (value >= 1024) return `${(value / 1024).toFixed(1)} KB`;
    return `${value} B`;
  }

  function setTextIfPresent(id, value) {
    const el = $(id);
    if (el) el.textContent = value;
  }

  function alertStateLabel(value) {
    if (value === 2) return "critique";
    if (value === 1) return "warning";
    return "normal";
  }

  function setSecretField(id, configured, configuredPlaceholder, emptyPlaceholder) {
    resetPasswordField(id, configuredPlaceholder, emptyPlaceholder, configured);
    $(id).dataset.keepSecret = configured ? "1" : "0";
  }

  function applyNotificationSettings(config) {
    setBoolSelectValue("notifyWarningAdv", config.notifyOnWarning);
    setBoolSelectValue("notifyRecoveryAdv", config.notifyOnRecovery);
    setBoolSelectValue("slackEnabledAdv", config.slackEnabled);
    setSecretField("slackWebhookUrlAdv", config.slackWebhookConfigured, "********", "https://hooks.slack.com/services/...");
    setFieldValue("slackChannelAdv", config.slackChannel);
    setBoolSelectValue("telegramEnabledAdv", config.telegramEnabled);
    setFieldValue("telegramChatIdAdv", config.telegramChatId);
    setSecretField("telegramBotTokenAdv", config.telegramTokenConfigured, "********", "123456:ABCDEF...");
    setBoolSelectValue("whatsappEnabledAdv", config.whatsappEnabled);
    setFieldValue("whatsappApiVersionAdv", config.whatsappApiVersion || "v22.0");
    setFieldValue("whatsappPhoneIdAdv", config.whatsappPhoneNumberId);
    setFieldValue("whatsappRecipientAdv", config.whatsappRecipient);
    setSecretField("whatsappAccessTokenAdv", config.whatsappAccessTokenConfigured, "********", "EAA...");

    const targets = [];
    if (config.slackEnabled) targets.push("Slack");
    if (config.telegramEnabled) targets.push("Telegram");
    if (config.whatsappEnabled) targets.push("WhatsApp");
    $("notificationsSummaryAdv").textContent = targets.length ? targets.join(" / ") : "Aucune cible active";
    $("notificationsCurrentStateAdv").textContent = alertStateLabel(Number(config.currentAlertState || 0));
    $("notificationsLastResultAdv").textContent = config.lastResult || "Aucun envoi.";
    $("notificationsLastSuccessAdv").textContent = formatEpoch(config.lastSuccessTs);
  }

  async function loadNotificationSettings() {
    try {
      const n = await apiGet("/api/notifications");
      applyNotificationSettings(n);
    } catch (err) {
      setToastError("toastNotificationsAdv", err);
    }
  }

  async function saveNotificationSettings() {
    await runToastRequest("toastNotificationsAdv", "Sauvegarde...", () => apiPost("/api/notifications", {
        notifyOnWarning: getIntValue("notifyWarningAdv", 0),
        notifyOnRecovery: getIntValue("notifyRecoveryAdv", 1),
        slackEnabled: getIntValue("slackEnabledAdv", 0),
        slackWebhookUrl: getFieldValue("slackWebhookUrlAdv"),
        slackChannel: getTrimmedValue("slackChannelAdv"),
        slackKeepWebhook: !getFieldValue("slackWebhookUrlAdv") && $("slackWebhookUrlAdv").dataset.keepSecret === "1",
        telegramEnabled: getIntValue("telegramEnabledAdv", 0),
        telegramChatId: getTrimmedValue("telegramChatIdAdv"),
        telegramBotToken: getFieldValue("telegramBotTokenAdv"),
        telegramKeepToken: !getFieldValue("telegramBotTokenAdv") && $("telegramBotTokenAdv").dataset.keepSecret === "1",
        whatsappEnabled: getIntValue("whatsappEnabledAdv", 0),
        whatsappApiVersion: getTrimmedValue("whatsappApiVersionAdv"),
        whatsappPhoneNumberId: getTrimmedValue("whatsappPhoneIdAdv"),
        whatsappRecipient: getTrimmedValue("whatsappRecipientAdv"),
        whatsappAccessToken: getFieldValue("whatsappAccessTokenAdv"),
        whatsappKeepAccessToken: !getFieldValue("whatsappAccessTokenAdv") && $("whatsappAccessTokenAdv").dataset.keepSecret === "1",
      }), "Notifications sauvees.",
      async () => {
        await loadNotificationSettings();
      });
  }

  async function testNotificationSettings() {
    await runToastRequest("toastNotificationsAdv", "Envoi test...", () => apiPost("/api/notifications/test", {}),
      "Notification de test envoyee.",
      async () => {
        await loadNotificationSettings();
      });
  }

  async function captureCalibration(index) {
    try {
      await apiPost("/api/calibrate", { index, refDb: state.calRefs[index] });
      state.calRefsDirty[index] = false;
      setToast("calToast", `Point ${index + 1} capture.`);
      await refreshStatus();
    } catch (err) {
      setToastError("calToast", err);
    }
  }

  async function setCalibrationMode(pointCount) {
    const nextCount = getCalibrationPointCount(pointCount);
    if (nextCount === state.calibrationPointCount) return;
    try {
      await apiPost("/api/calibrate/mode", { calibrationPointCount: nextCount });
      state.calibrationPointCount = nextCount;
      state.calRefs = [...calibrationRecommendations[nextCount]];
      state.calRefsDirty = [false, false, false, false, false];
      setToast("calToast", `Mode calibration ${nextCount} points.`);
      await refreshStatus();
    } catch (err) {
      setToastError("calToast", err);
      syncCalibrationModeButtons();
    }
  }

  async function clearCalibration() {
    if (!confirm("Effacer la calibration ?")) return;
    try {
      await apiPost("/api/calibrate/clear", {});
      state.calRefsDirty = [false, false, false, false, false];
      setToast("calToast", "Calibration effacee.");
      await refreshStatus();
    } catch (err) {
      setToastError("calToast", err);
    }
  }

  async function reboot() {
    await postAction("/api/reboot", "Reboot demande.");
  }

  async function toggleLive() {
    const nextEnabled = !state.liveEnabled;
    $("liveActionBtn").classList.toggle("active", nextEnabled);
    $("liveActionBtn").setAttribute("aria-pressed", nextEnabled ? "true" : "false");
    $("liveActionState").textContent = nextEnabled ? "ON AIR" : "OFF AIR";

    try {
      const response = await apiPost("/api/live", { enabled: nextEnabled });
      state.liveEnabled = Boolean(response.enabled);
      applyLiveActionState();
      setToast("dashboardPageToast", state.liveEnabled ? "LIVE active." : "LIVE coupe.");
    } catch (err) {
      applyLiveActionState();
      setToastError("dashboardPageToast", err);
    }
  }

  async function shutdown() {
    if (!confirm("Eteindre le systeme ? Reveil via bouton BOOT.")) return;
    await postAction("/api/shutdown", "Shutdown demande.");
  }

  async function factoryReset() {
    if (!confirm("Factory reset ?")) return;
    await postAction("/api/factory_reset", "Factory reset demande.");
  }

  syncTardisUi();

  document.querySelectorAll(".tab").forEach((tab) => {
    tab.addEventListener("click", () => setActivePage(tab.dataset.page));
  });

  $("bl").addEventListener("click", toggleBacklight);

  ["g", "o", "hist", "warnHoldSec", "critHoldSec", "calCaptureSec"].forEach((id) => {
    $(id).addEventListener("input", () => {
      markUiDirty();
      syncUiLabels();
    });
  });

  $("dashboardPageAdv").addEventListener("change", () => {
    state.dashboardPage = getDashboardPageValue(getFieldValue("dashboardPageAdv"));
    state.dashboardDisplayDirty = true;
    $("dashboardPageAdv").value = String(state.dashboardPage);
  });

  $("touchEnabledAdv").addEventListener("click", () => {
    const next = !readDashboardTouchEnabled();
    setDashboardTouchEnabled(next);
    state.touchEnabled = next;
    state.dashboardDisplayDirty = true;
  });

  $("tardisModeEnabled").addEventListener("click", () => {
    state.tardisModeEnabled = !state.tardisModeEnabled;
    state.tardisDirty = true;
    syncTardisUi();
  });

  $("tardisInteriorLedEnabled").addEventListener("click", () => {
    state.tardisInteriorLedEnabled = !state.tardisInteriorLedEnabled;
    state.tardisDirty = true;
    syncTardisUi();
  });

  $("tardisExteriorLedEnabled").addEventListener("click", () => {
    state.tardisExteriorLedEnabled = !state.tardisExteriorLedEnabled;
    state.tardisDirty = true;
    syncTardisUi();
  });

  $("tardisInteriorRgbMode").addEventListener("change", () => {
    state.tardisInteriorRgbMode = Number($("tardisInteriorRgbMode").value || 0);
    state.tardisDirty = true;
    syncTardisUi();
  });

  $("tardisInteriorRgbColor").addEventListener("input", () => {
    const value = $("tardisInteriorRgbColor").value || "#2D9CDB";
    state.tardisInteriorRgbColor = Number.parseInt(value.slice(1), 16) || 0x2D9CDB;
    state.tardisDirty = true;
  });

  document.querySelectorAll("[data-dashboard-fullscreen]").forEach((btn) => {
    btn.addEventListener("click", () => {
      btn.classList.toggle("active");
      const active = btn.classList.contains("active");
      btn.setAttribute("aria-pressed", active ? "true" : "false");
      state.dashboardFullscreenMask = readDashboardFullscreenMask();
      state.dashboardDisplayDirty = true;
    });
  });

  document.querySelectorAll("[data-mode]").forEach((btn) => {
    btn.addEventListener("click", () => {
      state.uiResponseMode = Number(btn.dataset.mode);
      markUiDirty();
      syncUiLabels();
    });
  });

  $("audioSourceSelect").addEventListener("change", () => {
    state.uiAudioSource = Number($("audioSourceSelect").value);
    state.uiAudioSourceSupportsCalibration = state.uiAudioSource !== 0;
    markUiDirty();
    syncUiLabels();
    syncCalibrationAvailability();
  });

  document.querySelectorAll("[data-cal-mode]").forEach((btn) => {
    btn.addEventListener("click", () => setCalibrationMode(Number(btn.dataset.calMode)));
  });

  document.querySelectorAll("[data-cal-adjust]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const [indexStr, deltaStr] = btn.dataset.calAdjust.split(":");
      const index = Number(indexStr);
      const delta = Number(deltaStr);
      state.calRefs[index] = Math.max(35, Math.min(110, Number(state.calRefs[index] || 0) + delta));
      state.calRefsDirty[index] = true;
      $(`calRef${index + 1}`).textContent = state.calRefs[index].toFixed(0);
    });
  });

  document.querySelectorAll("[data-cal-capture]").forEach((btn) => {
    btn.addEventListener("click", () => captureCalibration(Number(btn.dataset.calCapture)));
  });

  window.addEventListener("resize", drawHistory);

  $("saveUi").addEventListener("click", saveUi);
  $("saveDashboardPageAdv").addEventListener("click", saveDashboardPage);
  $("saveTardisAdv").addEventListener("click", saveTardisSettings);
  $("savePinAdv").addEventListener("click", savePinSettings);
  $("clearPinAdv").addEventListener("click", clearPinSettings);
  $("createWebUserBtn").addEventListener("click", createWebUser);
  $("updateWebPasswordBtn").addEventListener("click", updateWebPassword);
  $("deleteWebUserBtn").addEventListener("click", () => deleteWebUser());
  $("logoutBtn").addEventListener("click", logout);
  $("saveHomeAssistantAdv").addEventListener("click", saveHomeAssistantSettings);
  $("generateHomeAssistantTokenBtn").addEventListener("click", generateHomeAssistantToken);
  $("clearHomeAssistantTokenBtn").addEventListener("click", clearHomeAssistantToken);
  $("exportConfigBtn").addEventListener("click", exportConfig);
  $("exportFullConfigBtn").addEventListener("click", exportConfigFull);
  $("importConfigBtn").addEventListener("click", importConfig);
  $("backupConfigBtn").addEventListener("click", backupConfig);
  $("restoreConfigBtn").addEventListener("click", restoreConfig);
  $("partialResetBtn").addEventListener("click", partialReset);
  $("clearCalibration").addEventListener("click", clearCalibration);
  $("liveActionBtn").addEventListener("click", toggleLive);
  $("rebootBtn").addEventListener("click", reboot);
  $("shutdownBtn").addEventListener("click", shutdown);
  $("factoryResetBtn").addEventListener("click", factoryReset);
  $("saveTimeAdv").addEventListener("click", saveTimeSettings);
  $("saveWifiAdv").addEventListener("click", saveWifiSettings);
  $("saveOtaAdv").addEventListener("click", saveOtaSettings);
  if ($("checkReleaseBtn")) $("checkReleaseBtn").addEventListener("click", checkReleaseNow);
  if ($("installReleaseBtn")) $("installReleaseBtn").addEventListener("click", installReleaseNow);
  if ($("forceInstallReleaseBtn")) $("forceInstallReleaseBtn").addEventListener("click", forceInstallReleaseNow);
  $("saveMqttAdv").addEventListener("click", saveMqttSettings);
  $("saveNotificationsAdv").addEventListener("click", saveNotificationSettings);
  $("testNotificationsAdv").addEventListener("click", testNotificationSettings);
  $("toggleDebugLogsBtn").addEventListener("click", toggleDebugLogs);
  $("refreshDebugLogsBtn").addEventListener("click", refreshDebugLogs);
  $("clearDebugLogsBtn").addEventListener("click", clearDebugLogs);
  $("authSubmitBtn").addEventListener("click", submitAuth);
  $("authUsername").addEventListener("input", () => {
    $("authUsername").value = sanitizeUsernameValue($("authUsername").value);
  });
  $("newWebUsername").addEventListener("input", () => {
    $("newWebUsername").value = sanitizeUsernameValue($("newWebUsername").value);
  });
  ["authPassword", "newWebPassword", "manageWebPassword"].forEach((id) => {
    $(id).addEventListener("input", () => {
      const password = getFieldValue(id);
      const hint = passwordPolicyHint(password);
      if (id === "authPassword") {
        $("authPasswordHint").textContent = hint || "Mot de passe robuste detecte.";
      }
    });
  });
  $("authPassword").addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") submitAuth();
  });
  $("authUsername").addEventListener("keydown", (ev) => {
    if (ev.key === "Enter") submitAuth();
  });
  $("accessPinAdv").addEventListener("input", () => {
    $("accessPinAdv").value = sanitizePinValue($("accessPinAdv").value);
  });
  $("homeAssistantTokenAdv").addEventListener("input", () => {
    $("homeAssistantTokenAdv").value = sanitizeHomeAssistantTokenValue($("homeAssistantTokenAdv").value);
    $("homeAssistantTokenAdv").dataset.keepSecret = "0";
  });
  [1, 2, 3, 4].forEach((slot) => {
    $(`wifiPassword${slot}`).addEventListener("input", () => {
      $(`wifiPassword${slot}`).dataset.keepPassword = "0";
    });
  });
  ["slackWebhookUrlAdv", "telegramBotTokenAdv", "whatsappAccessTokenAdv"].forEach((id) => {
    $(id).addEventListener("input", () => {
      $(id).dataset.keepSecret = "0";
    });
  });
  $("mqttPasswordAdv").addEventListener("input", () => {
    $("mqttPasswordAdv").dataset.keepPassword = "0";
  });
  $("configFile").addEventListener("change", async (ev) => {
    const file = ev.target.files && ev.target.files[0];
    if (!file) return;
    try {
      $("configJsonBox").value = await file.text();
      setToast("configToast", `Fichier charge: ${file.name}`);
    } catch (err) {
      setToast("configToast", `Erreur lecture fichier: ${err.message}`);
    }
  });

  async function initPage() {
    syncUiLabels();
    syncCalibrationModeButtons();
    applyLiveActionState();
    updateDebugLogUi();
    renderAuthState();
    await loadAuthStatus();
    if (state.authenticated) {
      await refreshStatus();
    }
    setInterval(() => refreshStatus(false), 5000);
    setInterval(() => refreshSystemSummary(false), 1000);
    setInterval(checkSystemHeartbeat, 1000);
  }

  initPage();
</script>
</body>
</html>
)HTML";

  addCommonSecurityHeaders();
  _srv.send_P(200, PSTR("text/html; charset=utf-8"), html, sizeof(html) - 1);
}
