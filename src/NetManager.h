// src/NetManager.h
#pragma once
#include <Arduino.h>
#include <WiFi.h>

#include "SettingsStore.h"

class NetManager {
public:
  bool begin(SettingsV1* settings);
  void loop();

  bool isWifiConnected() const;
  String ipString() const;
  int32_t rssi() const;

  // NTP / time
  bool timeIsValid() const;
  bool localTime(struct tm* out) const;
  String timeStringLocal() const;

  // info strings
  const char* ntpServer() const;
  const char* tz() const;

private:
  SettingsV1* _s = nullptr;

  bool _started = false;
  bool _ntpConfigured = false;
  bool _mdnsStarted = false;

  // quick cache to avoid spamming logs
  uint32_t _lastPrint = 0;
  mutable time_t _lastValidEpoch = 0;

  void ensureMdns();
};
