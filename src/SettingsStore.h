#pragma once
#include <Arduino.h>
#include <Preferences.h>

static constexpr uint32_t SETTINGS_MAGIC   = 0x53503730; // "SP70"
static constexpr uint16_t SETTINGS_VERSION = 5;
static constexpr uint32_t DEFAULT_NTP_SYNC_INTERVAL_MS = 10800000UL; // 3h, valeur par defaut ESP32

struct ThresholdsV1 {
  uint8_t greenMax  = 55;
  uint8_t orangeMax = 70;
};

struct SettingsV1 {
  uint32_t magic   = SETTINGS_MAGIC;
  uint16_t version = SETTINGS_VERSION;

  // UI
  uint8_t backlight = 80;
  ThresholdsV1 th;
  uint8_t historyMinutes = 5;

  // Time / locale
  char tz[64]        = "CET-1CEST,M3.5.0/2,M10.5.0/3";
  char ntpServer[64] = "fr.pool.ntp.org";
  uint32_t ntpSyncIntervalMs = DEFAULT_NTP_SYNC_INTERVAL_MS;

  // WiFi portal
  char hostname[32]  = "soundpanel7";

  // Audio source
  // 0 = Demo
  // 1 = Sensor Analog (GPIO6 / Sensor AD)
  uint8_t audioSource = 1;

  // Analog mic on Sensor AD = GPIO6
  uint8_t analogPin = 6;
  uint16_t analogRmsSamples = 256;
  float emaAlpha = 0.12f;
  uint32_t peakHoldMs = 5000;

  // fallback calibration
  float analogBaseOffsetDb = 0.0f;
  float analogExtraOffsetDb = 15.0f;

  // calibration points
  float calPointRefDb[3] = {45.0f, 65.0f, 85.0f};
  float calPointRawLogRms[3] = {0.0f, 0.0f, 0.0f};
  uint8_t calPointValid[3] = {0, 0, 0};

    // OTA
  uint8_t otaEnabled = 1;
  uint16_t otaPort = 3232;
  char otaHostname[32] = "soundpanel7";
  char otaPassword[128] = "";

    // MQTT
  uint8_t mqttEnabled = 0;
  char mqttHost[64] = "";
  uint16_t mqttPort = 1883;
  char mqttUsername[64] = "";
  char mqttPassword[128] = "";
  char mqttClientId[64] = "soundpanel7";
  char mqttBaseTopic[128] = "soundpanel7";
  uint16_t mqttPublishPeriodMs = 1000;
  uint8_t mqttRetain = 0;
};

class SettingsStore {
public:
  bool begin(const char* nvsNamespace = "sp7");
  void load(SettingsV1 &out);
  void save(const SettingsV1 &s);
  void factoryReset();

private:
  Preferences _prefs;
  String _ns;
};
