#include "MqttManager.h"
#include "AppConfig.h"

static constexpr uint16_t kMqttClientBufferSize = 2048;
static constexpr uint32_t kMqttReconnectIntervalMs = 15000;

MqttManager* MqttManager::_instance = nullptr;

bool MqttManager::begin(SettingsStore* store, SettingsV1* settings) {
  _store = store;
  _s = settings;
  _instance = this;
  _client.setBufferSize(kMqttClientBufferSize);
  _client.setCallback(MqttManager::onMessageStatic);

  if (!_s) {
    _lastError = "settings=null";
    return false;
  }

  if (!_s->mqttEnabled) {
    _lastError = "disabled";
    Serial0.println("[MQTT] status: disabled");
    return false;
  }

  _client.setServer(_s->mqttHost, _s->mqttPort);
  Serial0.printf("[MQTT] configured host=%s port=%u clientId=%s base=%s\n",
                 _s->mqttHost,
                 (unsigned)_s->mqttPort,
                 _s->mqttClientId,
                 _s->mqttBaseTopic);
  return true;
}

void MqttManager::updateMetrics(float dbInstant, float leq, float peak) {
  _db = dbInstant;
  _leq = leq;
  _peak = peak;
}

String MqttManager::jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);
  for (size_t i = 0; i < in.length(); ++i) {
    const char c = in[i];
    if (c == '\\' || c == '"') {
      out += '\\';
      out += c;
      continue;
    }
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    if (c == '\r') {
      out += "\\r";
      continue;
    }
    if (c == '\t') {
      out += "\\t";
      continue;
    }
    out += c;
  }
  return out;
}

String MqttManager::topic(const char* suffix) const {
  String t = _s ? String(_s->mqttBaseTopic) : String("soundpanel7");
  if (!t.endsWith("/")) t += "/";
  t += suffix;
  return t;
}

String MqttManager::availabilityTopic() const {
  return topic("availability");
}

String MqttManager::commandTopic(const char* suffix) const {
  return topic(suffix);
}

String MqttManager::deviceId() const {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();

  String base = (_s && _s->mqttClientId[0] != '\0') ? String(_s->mqttClientId) : String("soundpanel7");
  base.replace(" ", "_");
  base.toLowerCase();
  return base + "_" + mac;
}

String MqttManager::deviceName() const {
  if (_s && _s->hostname[0] != '\0') return String(_s->hostname);
  if (_s && _s->mqttClientId[0] != '\0') return String(_s->mqttClientId);
  return String("soundpanel7");
}

String MqttManager::uniqueId(const char* objectId) const {
  return deviceId() + "_" + objectId;
}

String MqttManager::discoveryTopic(const char* component, const char* objectId) const {
  String t = "homeassistant/";
  t += component;
  t += "/";
  t += deviceId();
  t += "/";
  t += objectId;
  t += "/config";
  return t;
}

bool MqttManager::publishDiscoverySensor(const char* objectId,
                                         const char* name,
                                         const char* stateTopic,
                                         const char* unit,
                                         const char* deviceClass,
                                         const char* stateClass,
                                         const char* entityCategory,
                                         const char* icon) {
  String payload;
  payload.reserve(1024);
  payload += "{";
  payload += "\"name\":\""; payload += jsonEscape(String(name)); payload += "\",";
  payload += "\"unique_id\":\""; payload += jsonEscape(uniqueId(objectId)); payload += "\",";
  payload += "\"state_topic\":\""; payload += jsonEscape(String(stateTopic)); payload += "\",";
  payload += "\"availability_topic\":\""; payload += jsonEscape(availabilityTopic()); payload += "\",";
  payload += "\"payload_available\":\"online\",";
  payload += "\"payload_not_available\":\"offline\",";
  if (unit && unit[0] != '\0') {
    payload += "\"unit_of_measurement\":\""; payload += jsonEscape(String(unit)); payload += "\",";
  }
  if (deviceClass && deviceClass[0] != '\0') {
    payload += "\"device_class\":\""; payload += jsonEscape(String(deviceClass)); payload += "\",";
  }
  if (stateClass && stateClass[0] != '\0') {
    payload += "\"state_class\":\""; payload += jsonEscape(String(stateClass)); payload += "\",";
  }
  if (entityCategory && entityCategory[0] != '\0') {
    payload += "\"entity_category\":\""; payload += jsonEscape(String(entityCategory)); payload += "\",";
  }
  if (icon && icon[0] != '\0') {
    payload += "\"icon\":\""; payload += jsonEscape(String(icon)); payload += "\",";
  }
  payload += "\"device\":{";
  payload += "\"identifiers\":[\""; payload += jsonEscape(deviceId()); payload += "\"],";
  payload += "\"name\":\""; payload += jsonEscape(deviceName()); payload += "\",";
  payload += "\"manufacturer\":\"JJ\",";
  payload += "\"model\":\"SoundPanel 7\",";
  payload += "\"sw_version\":\""; payload += jsonEscape(String(SOUNDPANEL7_VERSION)); payload += "\",";
  payload += "\"serial_number\":\""; payload += jsonEscape(WiFi.macAddress()); payload += "\",";
  payload += "\"configuration_url\":\"http://"; payload += WiFi.localIP().toString(); payload += "\"";
  payload += "}";
  payload += "}";

  return _client.publish(discoveryTopic("sensor", objectId).c_str(), payload.c_str(), true);
}

bool MqttManager::publishDiscoverySwitch(const char* objectId,
                                         const char* name,
                                         const char* stateTopic,
                                         const char* commandTopicValue,
                                         const char* icon) {
  String payload;
  payload.reserve(1024);
  payload += "{";
  payload += "\"name\":\""; payload += jsonEscape(String(name)); payload += "\",";
  payload += "\"unique_id\":\""; payload += jsonEscape(uniqueId(objectId)); payload += "\",";
  payload += "\"state_topic\":\""; payload += jsonEscape(String(stateTopic)); payload += "\",";
  payload += "\"command_topic\":\""; payload += jsonEscape(String(commandTopicValue)); payload += "\",";
  payload += "\"availability_topic\":\""; payload += jsonEscape(availabilityTopic()); payload += "\",";
  payload += "\"payload_available\":\"online\",";
  payload += "\"payload_not_available\":\"offline\",";
  payload += "\"payload_on\":\"ON\",";
  payload += "\"payload_off\":\"OFF\",";
  payload += "\"state_on\":\"ON\",";
  payload += "\"state_off\":\"OFF\",";
  if (icon && icon[0] != '\0') {
    payload += "\"icon\":\""; payload += jsonEscape(String(icon)); payload += "\",";
  }
  payload += "\"device\":{";
  payload += "\"identifiers\":[\""; payload += jsonEscape(deviceId()); payload += "\"],";
  payload += "\"name\":\""; payload += jsonEscape(deviceName()); payload += "\",";
  payload += "\"manufacturer\":\"JJ\",";
  payload += "\"model\":\"SoundPanel 7\",";
  payload += "\"sw_version\":\""; payload += jsonEscape(String(SOUNDPANEL7_VERSION)); payload += "\",";
  payload += "\"serial_number\":\""; payload += jsonEscape(WiFi.macAddress()); payload += "\",";
  payload += "\"configuration_url\":\"http://"; payload += WiFi.localIP().toString(); payload += "\"";
  payload += "}";
  payload += "}";

  return _client.publish(discoveryTopic("switch", objectId).c_str(), payload.c_str(), true);
}

bool MqttManager::publishDiscovery() {
  if (!_client.connected() || !_s) return false;

  bool ok = true;
  ok &= publishDiscoverySensor("db", "dB Instant", topic("db").c_str(), "dB", "sound_pressure", "measurement", "", "mdi:waveform");
  ok &= publishDiscoverySensor("leq", "Leq", topic("leq").c_str(), "dB", "sound_pressure", "measurement", "", "mdi:chart-bell-curve");
  ok &= publishDiscoverySensor("peak", "Peak", topic("peak").c_str(), "dB", "sound_pressure", "measurement", "", "mdi:signal-peak");
  ok &= publishDiscoverySensor("wifi_rssi", "WiFi RSSI", topic("wifi/rssi").c_str(), "dBm", "signal_strength", "measurement", "diagnostic", "mdi:wifi");
  ok &= publishDiscoverySensor("wifi_ip", "WiFi IP", topic("wifi/ip").c_str(), "", "", "", "diagnostic", "mdi:ip-network");
  ok &= publishDiscoverySwitch("live", "LIVE", topic("live/state").c_str(), commandTopic("live/set").c_str(), "mdi:record-rec");
  _discoveryPublished = ok;
  return ok;
}

bool MqttManager::publishLiveState() {
  if (!_client.connected() || !_s) return false;
  const bool retain = _s->mqttRetain ? true : false;
  const char* payload = _s->liveEnabled ? "ON" : "OFF";
  const bool ok = _client.publish(topic("live/state").c_str(), payload, retain);
  if (ok) _lastPublishedLiveEnabled = _s->liveEnabled ? LIVE_ENABLED : LIVE_DISABLED;
  return ok;
}

bool MqttManager::connectIfNeeded() {
  if (!_s || !_s->mqttEnabled) return false;
  if (!WiFi.isConnected()) {
    _lastError = "wifi disconnected";
    return false;
  }
  if (_client.connected()) return true;

  uint32_t now = millis();
  if (now - _lastConnectAttemptMs < kMqttReconnectIntervalMs) return false;
  _lastConnectAttemptMs = now;

  _client.setServer(_s->mqttHost, _s->mqttPort);

  const String willTopic = availabilityTopic();
  bool ok = false;
  if (strlen(_s->mqttUsername) > 0) {
    ok = _client.connect(_s->mqttClientId,
                         _s->mqttUsername,
                         _s->mqttPassword,
                         willTopic.c_str(),
                         0,
                         true,
                         "offline");
  } else {
    ok = _client.connect(_s->mqttClientId,
                         willTopic.c_str(),
                         0,
                         true,
                         "offline");
  }

  if (ok) {
    _lastError = "";
    _discoveryPublished = false;
    _lastPublishedLiveEnabled = 255;
    Serial0.println("[MQTT] connection: established");
    _client.publish(availabilityTopic().c_str(), "online", true);
    const bool subOk = _client.subscribe(commandTopic("live/set").c_str());
    Serial0.printf("[MQTT] subscribe %s %s\n",
                   commandTopic("live/set").c_str(),
                   subOk ? "OK" : "FAIL");
    publishDiscovery();
    publishState();
    return true;
  }

  _lastError = String("connect failed rc=") + _client.state();
  Serial0.printf("[MQTT] %s\n", _lastError.c_str());
  return false;
}

bool MqttManager::publishState() {
  if (!_client.connected() || !_s) return false;

  String payload;
  payload.reserve(256);
  payload += "{";
  payload += "\"db\":"; payload += String(_db, 1); payload += ",";
  payload += "\"leq\":"; payload += String(_leq, 1); payload += ",";
  payload += "\"peak\":"; payload += String(_peak, 1); payload += ",";
  payload += "\"live\":"; payload += (_s->liveEnabled ? "true" : "false"); payload += ",";
  payload += "\"wifi\":"; payload += (WiFi.isConnected() ? "true" : "false"); payload += ",";
  payload += "\"ip\":\""; payload += WiFi.localIP().toString(); payload += "\",";
  payload += "\"rssi\":"; payload += String(WiFi.RSSI());
  payload += "}";

  bool retain = _s->mqttRetain ? true : false;

  bool ok = true;
  ok &= _client.publish(availabilityTopic().c_str(), "online", true);
  ok &= _client.publish(topic("state").c_str(), payload.c_str(), retain);
  ok &= _client.publish(topic("db").c_str(), String(_db, 1).c_str(), retain);
  ok &= _client.publish(topic("leq").c_str(), String(_leq, 1).c_str(), retain);
  ok &= _client.publish(topic("peak").c_str(), String(_peak, 1).c_str(), retain);
  ok &= publishLiveState();
  ok &= _client.publish(topic("wifi/rssi").c_str(), String(WiFi.RSSI()).c_str(), retain);
  ok &= _client.publish(topic("wifi/ip").c_str(), WiFi.localIP().toString().c_str(), retain);

  return ok;
}

void MqttManager::handleMessage(char* topicName, uint8_t* payload, unsigned int length) {
  if (!_s || !topicName || !payload) return;

  String topicValue = String(topicName);
  if (topicValue != commandTopic("live/set")) return;

  String body;
  body.reserve(length);
  for (unsigned int i = 0; i < length; i++) body += (char)payload[i];
  body.trim();
  body.toUpperCase();

  int next = -1;
  if (body == "ON" || body == "1" || body == "TRUE") next = LIVE_ENABLED;
  else if (body == "OFF" || body == "0" || body == "FALSE") next = LIVE_DISABLED;

  if (next < 0) {
    Serial0.printf("[MQTT] ignored LIVE payload=%s\n", body.c_str());
    return;
  }

  const uint8_t previous = _s->liveEnabled ? LIVE_ENABLED : LIVE_DISABLED;
  _s->liveEnabled = (uint8_t)next;
  if (_store && previous != _s->liveEnabled) {
    _store->saveUiSettings(_s->backlight, _s->liveEnabled, _s->touchEnabled,
                          _s->dashboardPage, _s->dashboardFullscreenMask);
  }

  Serial0.printf("[MQTT] LIVE %s via %s\n", _s->liveEnabled ? "ON" : "OFF", topicName);
  if (_client.connected()) {
    _lastPublishMs = millis();
    publishState();
  }
}

void MqttManager::onMessageStatic(char* topicName, uint8_t* payload, unsigned int length) {
  if (_instance) _instance->handleMessage(topicName, payload, length);
}

void MqttManager::loop() {
  if (!_s || !_s->mqttEnabled) return;

  if (!connectIfNeeded()) return;
  _client.loop();
  if (!_discoveryPublished) publishDiscovery();
  if (_lastPublishedLiveEnabled == 255 || _lastPublishedLiveEnabled != (_s->liveEnabled ? LIVE_ENABLED : LIVE_DISABLED)) {
    _lastPublishMs = millis();
    publishState();
  }

  uint32_t period = _s->mqttPublishPeriodMs;
  if (period < MIN_MQTT_PUBLISH_PERIOD_MS) period = MIN_MQTT_PUBLISH_PERIOD_MS;

  uint32_t now = millis();
  if (now - _lastPublishMs >= period) {
    _lastPublishMs = now;
    publishState();
  }
}
