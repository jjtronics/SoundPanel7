#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include "AppConfig.h"

static constexpr uint32_t SETTINGS_MAGIC   = 0x53503730; // "SP70"
static constexpr uint16_t SETTINGS_VERSION = 8;
static constexpr uint32_t MS_PER_SECOND = 1000UL;
static constexpr uint32_t MS_PER_MINUTE = 60UL * MS_PER_SECOND;
static constexpr uint32_t DEFAULT_NTP_SYNC_INTERVAL_MS = 10800000UL; // 3h, valeur par defaut ESP32
static constexpr uint8_t CALIBRATION_POINT_MAX = 5;
static constexpr uint8_t CALIBRATION_PROFILE_COUNT = 3;
static constexpr uint8_t DEFAULT_GREEN_MAX = 55;
static constexpr uint8_t DEFAULT_ORANGE_MAX = 70;
static constexpr uint8_t DEFAULT_HISTORY_MINUTES = 5;
static constexpr uint32_t MIN_HISTORY_SAMPLE_PERIOD_MS = 250;
static constexpr uint32_t DEFAULT_WARNING_HOLD_MS = 3000UL;
static constexpr uint32_t DEFAULT_CRITICAL_HOLD_MS = 2000UL;
static constexpr uint32_t DEFAULT_CALIBRATION_CAPTURE_MS = 3000UL;
static constexpr uint32_t MAX_ALERT_HOLD_MS = 60UL * MS_PER_SECOND;
static constexpr uint32_t MAX_PEAK_HOLD_MS = 30UL * MS_PER_SECOND;
static constexpr uint32_t MIN_CALIBRATION_CAPTURE_MS = 1UL * MS_PER_SECOND;
static constexpr uint32_t MAX_CALIBRATION_CAPTURE_MS = 30UL * MS_PER_SECOND;
static constexpr uint32_t MIN_NTP_SYNC_INTERVAL_MS = 1UL * MS_PER_MINUTE;
static constexpr uint32_t MAX_NTP_SYNC_INTERVAL_MS = 24UL * 60UL * MS_PER_MINUTE;
static constexpr uint16_t DEFAULT_OTA_PORT = 3232;
static constexpr uint16_t DEFAULT_MQTT_PORT = 1883;
static constexpr uint16_t DEFAULT_MQTT_PUBLISH_PERIOD_MS = 1000;
static constexpr uint16_t MIN_MQTT_PUBLISH_PERIOD_MS = 250;
static constexpr uint16_t MAX_MQTT_PUBLISH_PERIOD_MS = 60000;
static constexpr uint8_t PIN_CODE_MIN_LENGTH = 4;
static constexpr uint8_t PIN_CODE_MAX_LENGTH = 8;
static constexpr uint8_t PIN_HASH_SALT_LENGTH = 32;
static constexpr uint8_t PIN_HASH_LENGTH = 64;
static constexpr uint32_t PIN_HASH_ROUNDS_LEGACY = 100000;
// Touch PIN saves happen on the UI thread, so keep the KDF cost bounded enough to
// avoid multi-second freezes on the ESP32-S3 while still storing a salted hash.
static constexpr uint32_t PIN_HASH_ROUNDS = 8000;
static constexpr size_t PIN_STORAGE_MAX_LENGTH = 3 + PIN_HASH_SALT_LENGTH + 1 + PIN_HASH_LENGTH;
static constexpr uint8_t WEB_USER_MAX_COUNT = 4;
static constexpr uint8_t WEB_USERNAME_MAX_LENGTH = 24;
static constexpr uint8_t WEB_PASSWORD_MIN_LENGTH = 10;
static constexpr uint8_t WEB_PASSWORD_MAX_LENGTH = 64;
static constexpr uint8_t WEB_PASSWORD_SALT_LENGTH = 32;
static constexpr uint8_t WEB_PASSWORD_HASH_LENGTH = 64;
static constexpr uint8_t HOME_ASSISTANT_TOKEN_MAX_LENGTH = 64;
static constexpr uint8_t WIFI_CREDENTIAL_MAX_COUNT = 4;
static constexpr uint8_t WIFI_SSID_MAX_LENGTH = 32;
static constexpr uint8_t WIFI_PASSWORD_MAX_LENGTH = 64;
static constexpr uint8_t SLACK_WEBHOOK_URL_MAX_LENGTH = 192;
static constexpr uint8_t SLACK_CHANNEL_MAX_LENGTH = 80;
static constexpr uint8_t TELEGRAM_BOT_TOKEN_MAX_LENGTH = 96;
static constexpr uint8_t TELEGRAM_CHAT_ID_MAX_LENGTH = 48;
static constexpr uint8_t WHATSAPP_ACCESS_TOKEN_MAX_LENGTH = 192;
static constexpr uint8_t WHATSAPP_PHONE_NUMBER_ID_MAX_LENGTH = 32;
static constexpr uint8_t WHATSAPP_RECIPIENT_MAX_LENGTH = 32;
static constexpr uint8_t WHATSAPP_API_VERSION_MAX_LENGTH = 12;
static constexpr uint8_t LIVE_DISABLED = 0;
static constexpr uint8_t LIVE_ENABLED = 1;
static constexpr uint8_t TARDIS_INTERIOR_RGB_MODE_ALERT = 0;
static constexpr uint8_t TARDIS_INTERIOR_RGB_MODE_FIXED = 1;
static constexpr uint8_t TARDIS_INTERIOR_RGB_MODE_TAKEOFF = 2;
static constexpr uint8_t TARDIS_INTERIOR_RGB_MODE_TAKEOFF_MECHANICAL = TARDIS_INTERIOR_RGB_MODE_TAKEOFF;
static constexpr uint8_t TARDIS_INTERIOR_RGB_MODE_TAKEOFF_SINE = 3;
static constexpr uint8_t TARDIS_INTERIOR_RGB_MODE_MAX = TARDIS_INTERIOR_RGB_MODE_TAKEOFF_SINE;
static constexpr uint32_t TARDIS_INTERIOR_RGB_DEFAULT_COLOR = 0x2D9CDBUL;
static constexpr uint8_t ALERT_NOTIFY_LEVEL_CRITICAL = 0;
static constexpr uint8_t ALERT_NOTIFY_LEVEL_WARNING = 1;
static constexpr uint8_t DASHBOARD_PAGE_OVERVIEW = 0;
static constexpr uint8_t DASHBOARD_PAGE_CLOCK = 1;
static constexpr uint8_t DASHBOARD_PAGE_LIVE = 2;
static constexpr uint8_t DASHBOARD_PAGE_SOUND = 3;
static constexpr uint8_t DEFAULT_DASHBOARD_PAGE = DASHBOARD_PAGE_OVERVIEW;
static constexpr uint8_t MAX_WEB_DASHBOARD_PAGE = DASHBOARD_PAGE_SOUND;
static constexpr uint8_t DASHBOARD_FULLSCREEN_OVERVIEW = 1U << 0;
static constexpr uint8_t DASHBOARD_FULLSCREEN_CLOCK = 1U << 1;
static constexpr uint8_t DASHBOARD_FULLSCREEN_LIVE = 1U << 2;
static constexpr uint8_t DASHBOARD_FULLSCREEN_SOUND = 1U << 3;
static constexpr uint8_t DASHBOARD_FULLSCREEN_SUPPORTED_MASK =
  DASHBOARD_FULLSCREEN_OVERVIEW |
  DASHBOARD_FULLSCREEN_CLOCK |
  DASHBOARD_FULLSCREEN_LIVE |
  DASHBOARD_FULLSCREEN_SOUND;
static constexpr uint8_t DEFAULT_DASHBOARD_FULLSCREEN_MASK = 0;
static constexpr float RECOMMENDED_CALIBRATION_3[CALIBRATION_POINT_MAX] = {45.0f, 65.0f, 85.0f, 95.0f, 105.0f};
static constexpr float RECOMMENDED_CALIBRATION_5[CALIBRATION_POINT_MAX] = {40.0f, 55.0f, 70.0f, 85.0f, 100.0f};

static inline uint8_t normalizedCalibrationPointCount(uint8_t count) {
  return (count >= CALIBRATION_POINT_MAX) ? CALIBRATION_POINT_MAX : 3;
}

static inline uint8_t calibrationProfileIndexForAudioSource(uint8_t audioSource) {
  switch (audioSource) {
    case 1: return 0; // Analog Mic
    case 2: return 1; // PDM MEMS
    case 3: return 2; // INMP441
    default: return 0xFF;
  }
}

static inline size_t pinCodeLength(const char* pin) {
  if (!pin) return 0;
  size_t len = 0;
  while (pin[len]) len++;
  return len;
}

static inline bool pinCodeIsValid(const char* pin) {
  const size_t len = pinCodeLength(pin);
  if (len < PIN_CODE_MIN_LENGTH || len > PIN_CODE_MAX_LENGTH) return false;
  for (size_t i = 0; i < len; i++) {
    if (pin[i] < '0' || pin[i] > '9') return false;
  }
  return true;
}

bool pinCodeIsConfigured(const char* pin);
bool pinCodeMatches(const char* storedPin, const char* candidate);
bool encodePinCode(const char* pin, char* out, size_t outSize);

static inline uint8_t normalizedDashboardPage(uint8_t page) {
  return page <= MAX_WEB_DASHBOARD_PAGE ? page : DEFAULT_DASHBOARD_PAGE;
}

static inline uint8_t dashboardFullscreenFlagForPage(uint8_t page) {
  switch (page) {
    case DASHBOARD_PAGE_OVERVIEW: return DASHBOARD_FULLSCREEN_OVERVIEW;
    case DASHBOARD_PAGE_CLOCK: return DASHBOARD_FULLSCREEN_CLOCK;
    case DASHBOARD_PAGE_LIVE: return DASHBOARD_FULLSCREEN_LIVE;
    case DASHBOARD_PAGE_SOUND: return DASHBOARD_FULLSCREEN_SOUND;
    default: return 0;
  }
}

static inline uint8_t normalizedDashboardFullscreenMask(uint8_t mask) {
  return mask & DASHBOARD_FULLSCREEN_SUPPORTED_MASK;
}

static inline bool dashboardPageSupportsFullscreen(uint8_t page) {
  return dashboardFullscreenFlagForPage(page) != 0;
}

static inline bool dashboardPageFullscreenEnabled(uint8_t mask, uint8_t page) {
  return (normalizedDashboardFullscreenMask(mask) & dashboardFullscreenFlagForPage(page)) != 0;
}

struct WebUserRecord {
  uint8_t active = 0;
  char username[WEB_USERNAME_MAX_LENGTH + 1] = "";
  char passwordSalt[WEB_PASSWORD_SALT_LENGTH + 1] = "";
  char passwordHash[WEB_PASSWORD_HASH_LENGTH + 1] = "";
};

struct WifiCredentialRecord {
  char ssid[WIFI_SSID_MAX_LENGTH + 1] = "";
  char password[WIFI_PASSWORD_MAX_LENGTH + 1] = "";
};

struct ThresholdsV1 {
  uint8_t greenMax  = DEFAULT_GREEN_MAX;
  uint8_t orangeMax = DEFAULT_ORANGE_MAX;
};

struct CalibrationProfile {
  float baseOffsetDb = 0.0f;
  float extraOffsetDb = 15.0f;
  uint8_t pointCount = 3;
  uint32_t captureMs = DEFAULT_CALIBRATION_CAPTURE_MS;
  float pointRefDb[CALIBRATION_POINT_MAX] = {
    RECOMMENDED_CALIBRATION_3[0],
    RECOMMENDED_CALIBRATION_3[1],
    RECOMMENDED_CALIBRATION_3[2],
    RECOMMENDED_CALIBRATION_3[3],
    RECOMMENDED_CALIBRATION_3[4]
  };
  float pointRawLogRms[CALIBRATION_POINT_MAX] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  uint8_t pointValid[CALIBRATION_POINT_MAX] = {0, 0, 0, 0, 0};
};

struct SettingsV1 {
  uint32_t magic   = SETTINGS_MAGIC;
  uint16_t version = SETTINGS_VERSION;

  // UI
  uint8_t backlight = 80;
  ThresholdsV1 th;
  uint8_t historyMinutes = DEFAULT_HISTORY_MINUTES;
  uint32_t orangeAlertHoldMs = DEFAULT_WARNING_HOLD_MS;
  uint32_t redAlertHoldMs = DEFAULT_CRITICAL_HOLD_MS;
  uint8_t liveEnabled = LIVE_DISABLED;
  uint8_t touchEnabled = 1;
  uint8_t dashboardPage = DEFAULT_DASHBOARD_PAGE;
  uint8_t dashboardFullscreenMask = DEFAULT_DASHBOARD_FULLSCREEN_MASK;
  uint8_t tardisModeEnabled = 0;
  uint8_t tardisInteriorLedEnabled = 0;
  uint8_t tardisExteriorLedEnabled = 0;
  uint8_t tardisInteriorRgbMode = TARDIS_INTERIOR_RGB_MODE_ALERT;
  uint32_t tardisInteriorRgbColor = TARDIS_INTERIOR_RGB_DEFAULT_COLOR;
  char dashboardPin[PIN_STORAGE_MAX_LENGTH + 1] = "";
  char homeAssistantToken[HOME_ASSISTANT_TOKEN_MAX_LENGTH + 1] = "";

  // Time / locale
  char tz[64]        = "CET-1CEST,M3.5.0/2,M10.5.0/3";
  char ntpServer[64] = "fr.pool.ntp.org";
  uint32_t ntpSyncIntervalMs = DEFAULT_NTP_SYNC_INTERVAL_MS;

  // WiFi portal
  char hostname[32]  = "soundpanel7";
  WifiCredentialRecord wifiCredentials[WIFI_CREDENTIAL_MAX_COUNT];

  // Audio source
  // 0 = Demo
  // 1 = Sensor Analog (GPIO6 / Sensor AD)
  // 2 = PDM MEMS
  // 3 = INMP441
  uint8_t audioSource = 1;

  uint8_t analogPin = SOUNDPANEL7_DEFAULT_ANALOG_PIN;
  uint8_t pdmClkPin = SOUNDPANEL7_DEFAULT_PDM_CLK_PIN;
  uint8_t pdmDataPin = SOUNDPANEL7_DEFAULT_PDM_DATA_PIN;
  uint8_t inmp441BclkPin = SOUNDPANEL7_DEFAULT_INMP441_BCLK_PIN;
  uint8_t inmp441WsPin = SOUNDPANEL7_DEFAULT_INMP441_WS_PIN;
  uint8_t inmp441DataPin = SOUNDPANEL7_DEFAULT_INMP441_DATA_PIN;
  uint16_t analogRmsSamples = 256;
  // 0 = Fast, 1 = Slow
  uint8_t audioResponseMode = 0;
  float emaAlpha = 0.12f;
  uint32_t peakHoldMs = 5000;

  // fallback calibration
  float analogBaseOffsetDb = 0.0f;
  float analogExtraOffsetDb = 15.0f;

  // calibration points
  uint8_t calibrationPointCount = 3;
  uint32_t calibrationCaptureMs = DEFAULT_CALIBRATION_CAPTURE_MS;
  float calPointRefDb[CALIBRATION_POINT_MAX] = {
    RECOMMENDED_CALIBRATION_3[0],
    RECOMMENDED_CALIBRATION_3[1],
    RECOMMENDED_CALIBRATION_3[2],
    RECOMMENDED_CALIBRATION_3[3],
    RECOMMENDED_CALIBRATION_3[4]
  };
  float calPointRawLogRms[CALIBRATION_POINT_MAX] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
  uint8_t calPointValid[CALIBRATION_POINT_MAX] = {0, 0, 0, 0, 0};
  CalibrationProfile calibrationProfiles[CALIBRATION_PROFILE_COUNT];

  // OTA
  uint8_t otaEnabled = 1;
  uint16_t otaPort = DEFAULT_OTA_PORT;
  char otaHostname[32] = "soundpanel7";
  char otaPassword[128] = "";

    // MQTT
  uint8_t mqttEnabled = 0;
  char mqttHost[64] = "";
  uint16_t mqttPort = DEFAULT_MQTT_PORT;
  char mqttUsername[64] = "";
  char mqttPassword[128] = "";
  char mqttClientId[64] = "soundpanel7";
  char mqttBaseTopic[128] = "soundpanel7";
  uint16_t mqttPublishPeriodMs = DEFAULT_MQTT_PUBLISH_PERIOD_MS;
  uint8_t mqttRetain = 0;

  // Alert notifications
  uint8_t notifyOnWarning = ALERT_NOTIFY_LEVEL_CRITICAL;
  uint8_t notifyOnRecovery = 1;

  uint8_t slackEnabled = 0;
  char slackWebhookUrl[SLACK_WEBHOOK_URL_MAX_LENGTH + 1] = "";
  char slackChannel[SLACK_CHANNEL_MAX_LENGTH + 1] = "";

  uint8_t telegramEnabled = 0;
  char telegramBotToken[TELEGRAM_BOT_TOKEN_MAX_LENGTH + 1] = "";
  char telegramChatId[TELEGRAM_CHAT_ID_MAX_LENGTH + 1] = "";

  uint8_t whatsappEnabled = 0;
  char whatsappAccessToken[WHATSAPP_ACCESS_TOKEN_MAX_LENGTH + 1] = "";
  char whatsappPhoneNumberId[WHATSAPP_PHONE_NUMBER_ID_MAX_LENGTH + 1] = "";
  char whatsappRecipient[WHATSAPP_RECIPIENT_MAX_LENGTH + 1] = "";
  char whatsappApiVersion[WHATSAPP_API_VERSION_MAX_LENGTH + 1] = "v22.0";
};

class SettingsStore {
public:
  enum SecretExportMode : uint8_t {
    EXPORT_SECRETS_OMIT = 0,
    EXPORT_SECRETS_ENCRYPTED = 1,
    EXPORT_SECRETS_CLEAR = 2
  };

  bool begin(const char* nvsNamespace = "sp7");
  void load(SettingsV1 &out);
  void save(const SettingsV1 &s);
  void loadWifiCredential(uint8_t index, WifiCredentialRecord& out);

  // Granular save methods - only write what changed to reduce NVS fragmentation
  void saveRuntimeSettings(const SettingsV1& s);
  void saveWifiCredentials(const WifiCredentialRecord (&credentials)[WIFI_CREDENTIAL_MAX_COUNT]);
  void saveWifiCredential(uint8_t index, const WifiCredentialRecord& credential);
  void saveThresholds(const ThresholdsV1& th);
  void saveUiSettings(uint8_t backlight, uint8_t liveEnabled, uint8_t touchEnabled, uint8_t dashboardPage, uint8_t dashboardFullscreenMask);
  void saveMqttSettings(bool enabled, const char* host, uint16_t port, const char* username, const char* password, const char* clientId, const char* baseTopic, uint16_t publishPeriodMs, bool retain);
  void saveOtaSettings(bool enabled, uint16_t port, const char* hostname, const char* password);
  void saveAudioSettings(uint8_t audioSource, uint16_t analogRmsSamples, uint8_t audioResponseMode, float emaAlpha, uint32_t peakHoldMs, float analogBaseOffsetDb, float analogExtraOffsetDb);
  void saveTimeSettings(const char* tz, const char* ntpServer, uint32_t ntpSyncIntervalMs, const char* hostname);
  void saveNotificationSettings(uint8_t notifyOnWarning, uint8_t notifyOnRecovery,
                                bool slackEnabled, const char* slackWebhookUrl, const char* slackChannel,
                                bool telegramEnabled, const char* telegramBotToken, const char* telegramChatId,
                                bool whatsappEnabled, const char* whatsappAccessToken, const char* whatsappPhoneNumberId,
                                const char* whatsappRecipient, const char* whatsappApiVersion);
  void saveHomeAssistantToken(const char* token);
  void saveDashboardPin(const char* pin);

  void factoryReset();
  String exportJson(const SettingsV1& s, SecretExportMode secretMode = EXPORT_SECRETS_OMIT, String* err = nullptr) const;
  bool importJson(SettingsV1& s, const String& json, String* err = nullptr);
  bool saveBackup(const SettingsV1& s);
  uint32_t backupTimestamp() const;
  bool restoreBackup(SettingsV1& out, String* err = nullptr);
  bool resetSection(SettingsV1& s, const String& scope, String* err = nullptr);
  static void syncActiveCalibrationProfile(SettingsV1& s);
  static void switchCalibrationProfile(SettingsV1& s, uint8_t nextAudioSource);
  void loadWebUsers(WebUserRecord (&out)[WEB_USER_MAX_COUNT]);
  uint8_t webUserCount();
  bool upsertWebUser(const WebUserRecord& user, String* err = nullptr);
  bool deleteWebUser(const char* username, String* err = nullptr);
  void clearWebUsers();

private:
  Preferences _prefs;
  String _ns;
  bool deriveSecretKey(uint8_t (&outKey)[32]) const;
  bool encryptSecret(const char* purpose, const char* plaintext, String& out) const;
  bool decryptSecret(const char* purpose, const char* stored, char* out, size_t outSize) const;
  bool loadSecret(const char* key, const char* purpose, char* out, size_t outSize, bool* migrated = nullptr);
  bool saveSecret(const char* key, const char* purpose, const char* value);
  static bool isEncryptedSecretRecord(const char* value);
  static bool isPinHashRecord(const char* value);
  static bool normalizePinStorage(char* storage, size_t storageSize);
  static void sanitize(SettingsV1& s);
  static void sanitizeWebUser(WebUserRecord& user);
  static void sanitizeWifiCredential(WifiCredentialRecord& credential);
};
