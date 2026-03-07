#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "SettingsStore.h"

class MqttManager {
public:
  bool begin(SettingsV1* settings);
  void loop();

  void updateMetrics(float dbInstant, float leq, float peak);

  bool connected() { return _client.connected(); }
  const char* lastError() const { return _lastError.c_str(); }

private:
  SettingsV1* _s = nullptr;
  WiFiClient _wifi;
  PubSubClient _client{_wifi};

  float _db = 0.0f;
  float _leq = 0.0f;
  float _peak = 0.0f;

  uint32_t _lastConnectAttemptMs = 0;
  uint32_t _lastPublishMs = 0;
  String _lastError;

  bool connectIfNeeded();
  bool publishState();
  String topic(const char* suffix) const;
};