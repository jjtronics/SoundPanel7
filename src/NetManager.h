// src/NetManager.h
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <functional>

#include "SettingsStore.h"

class NetManager {
public:
  bool begin(SettingsV1* settings, SettingsStore* store);
  void loop();

  bool isWifiConnected() const;
  bool isConfigPortalActive() const;
  void setConfigPortalStateCallback(std::function<void(bool)> callback);
  String ipString() const;
  int32_t rssi() const;
  String currentSsid() const;
  void reloadWifiConfig();
  uint8_t wifiCredentialCount() const;

  // NTP / time
  bool timeIsValid() const;
  bool localTime(struct tm* out) const;

  // info strings
  const char* ntpServer() const;
  const char* tz() const;

private:
  SettingsV1* _s = nullptr;
  SettingsStore* _store = nullptr;

  bool _started = false;
  bool _ntpConfigured = false;
  bool _mdnsStarted = false;
  mutable time_t _lastValidEpoch = 0;
  uint32_t _lastWifiAttemptMs = 0;
  uint8_t _wifiAttemptFailures = 0;
  bool _legacyCredentialTried = false;
  char _setupApName[32] = "";
  std::function<void(bool)> _configPortalStateCallback;

  void ensureMdns();
  void rebuildWifiMulti();
  void rebuildSetupApName();
  const char* setupApName();
  void ensureWifiConnection(bool force = false);
  void startConfigPortal();
  void configureHostname();
  bool applyHostnameAndMode(wifi_mode_t mode);
  void logStoredWifiCredentials(const char* label) const;
  void migrateLegacyCredentialIfNeeded();
  void onPortalWifiSaved();
  void rememberWifiCredential(const String& ssid, const String& password);
};
