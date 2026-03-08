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
  bool _discoveryPublished = false;

  bool connectIfNeeded();
  bool publishState();
  bool publishDiscovery();
  bool publishDiscoverySensor(const char* objectId,
                              const char* name,
                              const char* stateTopic,
                              const char* unit,
                              const char* deviceClass,
                              const char* stateClass,
                              const char* entityCategory,
                              const char* icon);
  String topic(const char* suffix) const;
  String availabilityTopic() const;
  String discoveryTopic(const char* component, const char* objectId) const;
  String deviceId() const;
  String deviceName() const;
  String uniqueId(const char* objectId) const;
  static String jsonEscape(const String& in);
};
