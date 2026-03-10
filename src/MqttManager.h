#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "SettingsStore.h"

class MqttManager {
public:
  bool begin(SettingsStore* store, SettingsV1* settings);
  void loop();

  void updateMetrics(float dbInstant, float leq, float peak);

  bool enabled() const { return _s && _s->mqttEnabled; }
  bool connected() { return _client.connected(); }
  const char* lastError() const { return _lastError.c_str(); }

private:
  SettingsStore* _store = nullptr;
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
  uint8_t _lastPublishedLiveEnabled = 255;

  bool connectIfNeeded();
  bool publishState();
  bool publishDiscovery();
  bool publishLiveState();
  bool publishDiscoverySensor(const char* objectId,
                              const char* name,
                              const char* stateTopic,
                              const char* unit,
                              const char* deviceClass,
                              const char* stateClass,
                              const char* entityCategory,
                              const char* icon);
  bool publishDiscoverySwitch(const char* objectId,
                              const char* name,
                              const char* stateTopic,
                              const char* commandTopic,
                              const char* icon);
  String topic(const char* suffix) const;
  String availabilityTopic() const;
  String commandTopic(const char* suffix) const;
  String discoveryTopic(const char* component, const char* objectId) const;
  String deviceId() const;
  String deviceName() const;
  String uniqueId(const char* objectId) const;
  void handleMessage(char* topic, uint8_t* payload, unsigned int length);
  static void onMessageStatic(char* topic, uint8_t* payload, unsigned int length);
  static String jsonEscape(const String& in);

  static MqttManager* _instance;
};
