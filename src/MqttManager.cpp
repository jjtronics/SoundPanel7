#include "MqttManager.h"

bool MqttManager::begin(SettingsV1* settings) {
  _s = settings;
  _client.setBufferSize(1024);

  if (!_s) {
    _lastError = "settings=null";
    return false;
  }

  if (!_s->mqttEnabled) {
    _lastError = "disabled";
    Serial0.println("[MQTT] disabled");
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

String MqttManager::topic(const char* suffix) const {
  String t = _s ? String(_s->mqttBaseTopic) : String("soundpanel7");
  if (!t.endsWith("/")) t += "/";
  t += suffix;
  return t;
}

bool MqttManager::connectIfNeeded() {
  if (!_s || !_s->mqttEnabled) return false;
  if (!WiFi.isConnected()) {
    _lastError = "wifi disconnected";
    return false;
  }
  if (_client.connected()) return true;

  uint32_t now = millis();
  if (now - _lastConnectAttemptMs < 5000) return false;
  _lastConnectAttemptMs = now;

  _client.setServer(_s->mqttHost, _s->mqttPort);

  bool ok = false;
  if (strlen(_s->mqttUsername) > 0) {
    ok = _client.connect(_s->mqttClientId, _s->mqttUsername, _s->mqttPassword);
  } else {
    ok = _client.connect(_s->mqttClientId);
  }

  if (ok) {
    _lastError = "";
    Serial0.println("[MQTT] connected");
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
  payload += "\"wifi\":"; payload += (WiFi.isConnected() ? "true" : "false"); payload += ",";
  payload += "\"ip\":\""; payload += WiFi.localIP().toString(); payload += "\",";
  payload += "\"rssi\":"; payload += String(WiFi.RSSI());
  payload += "}";

  bool retain = _s->mqttRetain ? true : false;

  bool ok = true;
  ok &= _client.publish(topic("state").c_str(), payload.c_str(), retain);
  ok &= _client.publish(topic("db").c_str(), String(_db, 1).c_str(), retain);
  ok &= _client.publish(topic("leq").c_str(), String(_leq, 1).c_str(), retain);
  ok &= _client.publish(topic("peak").c_str(), String(_peak, 1).c_str(), retain);
  ok &= _client.publish(topic("wifi/rssi").c_str(), String(WiFi.RSSI()).c_str(), retain);
  ok &= _client.publish(topic("wifi/ip").c_str(), WiFi.localIP().toString().c_str(), retain);

  return ok;
}

void MqttManager::loop() {
  if (!_s || !_s->mqttEnabled) return;

  if (!connectIfNeeded()) return;
  _client.loop();

  uint32_t period = _s->mqttPublishPeriodMs;
  if (period < 250) period = 250;

  uint32_t now = millis();
  if (now - _lastPublishMs >= period) {
    _lastPublishMs = now;
    publishState();
  }
}