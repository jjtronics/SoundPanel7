#include "WebManager.h"

#include <WiFi.h>
#include <AsyncTCP.h>
#include <esp_sntp.h>
#include <ctime>
#include <math.h>
#include <cstring>

#include "AudioEngine.h"

extern AudioEngine g_audio;

static uint32_t g_bootMs = 0;
static float g_webDbInstant = 0.0f;
static float g_webLeq = 0.0f;
static float g_webPeak = 0.0f;

static float jsonFloatLocal(const String& body, const char* key, float def) {
  String k = String("\"") + key + "\":";
  int p = body.indexOf(k);
  if (p < 0) return def;
  p += k.length();

  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;

  bool neg = false;
  if (p < (int)body.length() && body[p] == '-') {
    neg = true;
    p++;
  }

  String num;
  bool dotSeen = false;
  while (p < (int)body.length()) {
    char c = body[p];
    if (c >= '0' && c <= '9') {
      num += c;
      p++;
      continue;
    }
    if (c == '.' && !dotSeen) {
      dotSeen = true;
      num += c;
      p++;
      continue;
    }
    break;
  }

  if (num.isEmpty()) return def;
  float v = num.toFloat();
  return neg ? -v : v;
}

bool WebManager::begin(SettingsStore* store,
                       SettingsV1* settings,
                       NetManager* net,
                       esp_panel::board::Board* board,
                       SharedHistory* history) {
  _store = store;
  _s = settings;
  _net = net;
  _board = board;
  _history = history;

  if (!g_bootMs) g_bootMs = millis();
  if (_started) return true;

  routes();
  _srv.begin();
  setupLiveStream();
  _liveSrv.begin();
  _started = true;

  Serial0.println("[WEB] LISTEN on 80");
  Serial0.println("[WEB] LIVE SSE on 81");
  if (WiFi.isConnected()) {
    Serial0.printf("[WEB] URL: http://%s/\n", WiFi.localIP().toString().c_str());
  } else {
    Serial0.println("[WEB] WiFi not connected yet (URL available after connect)");
  }
  return true;
}

void WebManager::updateMetrics(float dbInstant, float leq, float peak) {
  g_webDbInstant = dbInstant;
  g_webLeq = leq;
  g_webPeak = peak;

  pushLiveMetrics();
}

void WebManager::routes() {
  _srv.on("/", HTTP_GET, [this]() { handleRoot(); });
  _srv.on("/admin", HTTP_GET, [this]() { handleAdmin(); });

  _srv.on("/api/status", HTTP_GET, [this]() { handleStatus(); });

  _srv.on("/api/ui", HTTP_POST, [this]() { handleUiSave(); });

  _srv.on("/api/time", HTTP_GET,  [this]() { handleTimeGet(); });
  _srv.on("/api/time", HTTP_POST, [this]() { handleTimeSave(); });

  _srv.on("/api/ota", HTTP_GET,  [this]() { handleOtaGet(); });
  _srv.on("/api/ota", HTTP_POST, [this]() { handleOtaSave(); });

  _srv.on("/api/mqtt", HTTP_GET,  [this]() { handleMqttGet(); });
  _srv.on("/api/mqtt", HTTP_POST, [this]() { handleMqttSave(); });

  _srv.on("/api/calibrate", HTTP_POST, [this]() {
    if (!_store || !_s) {
      replyJson(500, "{\"ok\":false,\"error\":\"store/settings missing\"}");
      return;
    }

    String body = _srv.arg("plain");
    int index = jsonInt(body, "index", -1);
    float refDb = jsonFloatLocal(body, "refDb", -1.0f);

    if (index < 0 || index > 2) {
      replyJson(400, "{\"ok\":false,\"error\":\"bad index\"}");
      return;
    }
    if (refDb <= 0.0f || refDb > 140.0f) {
      replyJson(400, "{\"ok\":false,\"error\":\"bad refDb\"}");
      return;
    }

    bool ok = g_audio.captureCalibrationPoint(*_s, (uint8_t)index, refDb);
    if (!ok) {
      replyJson(500, "{\"ok\":false,\"error\":\"capture failed\"}");
      return;
    }

    _store->save(*_s);
    Serial0.printf("[WEB] CAL point %d saved @ %.1f dB\n", index + 1, refDb);
    replyJson(200, "{\"ok\":true}");
  });

  _srv.on("/api/calibrate/clear", HTTP_POST, [this]() {
    if (!_store || !_s) {
      replyJson(500, "{\"ok\":false,\"error\":\"store/settings missing\"}");
      return;
    }

    g_audio.clearCalibration(*_s);
    _store->save(*_s);
    Serial0.println("[WEB] CAL cleared");
    replyJson(200, "{\"ok\":true}");
  });

  _srv.on("/api/reboot", HTTP_POST, [this]() { handleReboot(); });
  _srv.on("/api/factory_reset", HTTP_POST, [this]() { handleFactoryReset(); });

  _srv.onNotFound([this]() { replyText(404, "404\n"); });
}

void WebManager::loop() {
  if (!_started) return;
  _srv.handleClient();
}

void WebManager::setupLiveStream() {
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Cache-Control", "no-cache");

  _liveEvents.onConnect([this](AsyncEventSourceClient* client) {
    if (!client) return;
    const String payload = statusJson();
    client->send(payload.c_str(), "metrics", millis(), 1500);
  });
  _liveSrv.addHandler(&_liveEvents);
}

void WebManager::pushLiveMetrics(bool force) {
  if (_liveEvents.count() == 0) return;

  const uint32_t now = millis();
  if (!force && (now - _lastLivePushMs) < LIVE_PUSH_PERIOD_MS) return;

  _lastLivePushMs = now;
  const String payload = liveMetricsJson();
  _liveEvents.send(payload.c_str(), "metrics", now);
}

void WebManager::replyText(int code, const String& txt, const char* contentType) {
  _srv.send(code, contentType, txt);
}

void WebManager::replyJson(int code, const String& json) {
  _srv.send(code, "application/json", json);
}

String WebManager::statusJson() const {
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  int rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  uint32_t up = (millis() - g_bootMs) / 1000;

  struct tm ti;
  bool hasTime = getLocalTime(&ti, 0);
  char tbuf[32] = {0};
  if (hasTime) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);

  const AudioMetrics& am = g_audio.metrics();

  String json;
  json.reserve(768);
  json += "{";
  json += "\"wifi\":"; json += (WiFi.isConnected() ? "true" : "false"); json += ",";
  json += "\"ip\":\""; json += ip; json += "\",";
  json += "\"rssi\":"; json += String(rssi); json += ",";
  json += "\"uptime_s\":"; json += String(up); json += ",";
  json += "\"time_ok\":"; json += (hasTime ? "true" : "false"); json += ",";
  json += "\"time\":\""; json += (hasTime ? String(tbuf) : String("")); json += "\",";
  json += "\"backlight\":"; json += String(_s ? _s->backlight : 0); json += ",";
  json += "\"greenMax\":"; json += String(_s ? _s->th.greenMax : 55); json += ",";
  json += "\"orangeMax\":"; json += String(_s ? _s->th.orangeMax : 70); json += ",";
  json += "\"historyMinutes\":"; json += String(_s ? _s->historyMinutes : 5); json += ",";
  json += "\"historyCapacity\":"; json += String(SharedHistory::POINT_COUNT); json += ",";
  json += "\"historySamplePeriodMs\":"; json += String(_history ? _history->samplePeriodMs() : 3000); json += ",";
  json += "\"warningHoldSec\":"; json += String(_s ? (_s->orangeAlertHoldMs / 1000UL) : 3); json += ",";
  json += "\"criticalHoldSec\":"; json += String(_s ? (_s->redAlertHoldMs / 1000UL) : 2); json += ",";
  json += "\"db\":"; json += String(g_webDbInstant, 1); json += ",";
  json += "\"leq\":"; json += String(g_webLeq, 1); json += ",";
  json += "\"peak\":"; json += String(g_webPeak, 1); json += ",";
  json += "\"rawRms\":"; json += String(am.rawRms, 2); json += ",";
  json += "\"rawPseudoDb\":"; json += String(am.rawPseudoDb, 1); json += ",";
  json += "\"rawAdcMean\":"; json += String(am.rawAdcMean); json += ",";
  json += "\"rawAdcLast\":"; json += String(am.rawAdcLast); json += ",";
  json += "\"analogOk\":"; json += (am.analogOk ? "true" : "false"); json += ",";

  json += "\"cal\":[";
  for (int i = 0; i < 3; i++) {
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

String WebManager::liveMetricsJson() const {
  String ip = WiFi.isConnected() ? WiFi.localIP().toString() : String("");
  int rssi = WiFi.isConnected() ? WiFi.RSSI() : 0;
  struct tm ti;
  bool hasTime = getLocalTime(&ti, 0);
  char tbuf[32] = {0};
  if (hasTime) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);

  String json;
  json.reserve(288);
  json += "{";
  json += "\"db\":"; json += String(g_webDbInstant, 1); json += ",";
  json += "\"leq\":"; json += String(g_webLeq, 1); json += ",";
  json += "\"peak\":"; json += String(g_webPeak, 1); json += ",";
  json += "\"greenMax\":"; json += String(_s ? _s->th.greenMax : 55); json += ",";
  json += "\"orangeMax\":"; json += String(_s ? _s->th.orangeMax : 70); json += ",";
  json += "\"historyMinutes\":"; json += String(_s ? _s->historyMinutes : 5); json += ",";
  json += "\"historyCapacity\":"; json += String(SharedHistory::POINT_COUNT); json += ",";
  json += "\"historySamplePeriodMs\":"; json += String(_history ? _history->samplePeriodMs() : 3000); json += ",";
  json += "\"wifi\":"; json += (WiFi.isConnected() ? "true" : "false"); json += ",";
  json += "\"ip\":\""; json += ip; json += "\",";
  json += "\"rssi\":"; json += String(rssi); json += ",";
  json += "\"time_ok\":"; json += (hasTime ? "true" : "false"); json += ",";
  json += "\"time\":\""; json += (hasTime ? String(tbuf) : String("")); json += "\"";
  json += "}";
  return json;
}

int WebManager::jsonInt(const String& body, const char* key, int def) {
  String k = String("\"") + key + "\":";
  int p = body.indexOf(k);
  if (p < 0) return def;
  p += k.length();

  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;

  bool neg = false;
  if (p < (int)body.length() && body[p] == '-') {
    neg = true;
    p++;
  }

  long v = 0;
  bool ok = false;
  while (p < (int)body.length()) {
    char c = body[p];
    if (c < '0' || c > '9') break;
    ok = true;
    v = v * 10 + (c - '0');
    p++;
  }
  if (!ok) return def;
  return neg ? (int)-v : (int)v;
}

String WebManager::jsonStr(const String& body, const char* key, const String& def) {
  String k = String("\"") + key + "\":";
  int p = body.indexOf(k);
  if (p < 0) return def;
  p += k.length();

  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
  if (p >= (int)body.length() || body[p] != '"') return def;
  p++;

  String out;
  out.reserve(64);

  while (p < (int)body.length()) {
    char c = body[p++];
    if (c == '\\') {
      if (p >= (int)body.length()) break;
      char n = body[p++];
      if (n == 'n') out += '\n';
      else if (n == 't') out += '\t';
      else out += n;
      continue;
    }
    if (c == '"') break;
    out += c;
  }

  if (out.length() == 0) return def;
  return out;
}

bool WebManager::safeCopy(char* dst, size_t dstSize, const String& src) {
  if (!dst || dstSize == 0) return false;
  if (src.length() >= dstSize) return false;
  memcpy(dst, src.c_str(), src.length() + 1);
  return true;
}

void WebManager::applyBacklightNow(uint8_t percent) {
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
}

String WebManager::historyJson() const {
  return _history ? _history->toJson() : String("[]");
}

void WebManager::handleStatus() {
  replyJson(200, statusJson());
}

void WebManager::handleUiSave() {
  if (!_store || !_s) {
    replyText(500, "store/settings missing\n");
    return;
  }

  String body = _srv.arg("plain");

  int bl = jsonInt(body, "backlight", (int)_s->backlight);
  int g  = jsonInt(body, "greenMax",  (int)_s->th.greenMax);
  int o  = jsonInt(body, "orangeMax", (int)_s->th.orangeMax);
  int hm = jsonInt(body, "historyMinutes", (int)_s->historyMinutes);
  int whs = jsonInt(body, "warningHoldSec", (int)(_s->orangeAlertHoldMs / 1000UL));
  int chs = jsonInt(body, "criticalHoldSec", (int)(_s->redAlertHoldMs / 1000UL));

  if (bl < 0) bl = 0;
  if (bl > 100) bl = 100;
  if (g < 0) g = 0;
  if (g > 100) g = 100;
  if (o < g) o = g;
  if (o > 100) o = 100;
  if (hm < 1) hm = 1;
  if (hm > 60) hm = 60;
  if (whs < 0) whs = 0;
  if (whs > 60) whs = 60;
  if (chs < 0) chs = 0;
  if (chs > 60) chs = 60;

  _s->backlight = (uint8_t)bl;
  _s->th.greenMax = (uint8_t)g;
  _s->th.orangeMax = (uint8_t)o;
  _s->historyMinutes = (uint8_t)hm;
  _s->orangeAlertHoldMs = (uint32_t)whs * 1000UL;
  _s->redAlertHoldMs = (uint32_t)chs * 1000UL;
  if (_history) _history->settingsChanged();

  _store->save(*_s);
  applyBacklightNow(_s->backlight);

  Serial0.printf("[WEB] UI saved: backlight=%d green=%d orange=%d hist=%d warn=%ds crit=%ds\n",
                 bl, g, o, hm, whs, chs);
  replyJson(200, "{\"ok\":true}\n");
}

void WebManager::handleTimeGet() {
  if (!_s) {
    replyText(500, "settings missing\n");
    return;
  }

  String json;
  json.reserve(256);
  json += "{";
  json += "\"tz\":\""; json += String(_s->tz); json += "\",";
  json += "\"ntpServer\":\""; json += String(_s->ntpServer); json += "\",";
  json += "\"ntpSyncIntervalMs\":"; json += String(_s->ntpSyncIntervalMs); json += ",";
  json += "\"ntpSyncMinutes\":"; json += String(_s->ntpSyncIntervalMs / 60000UL); json += ",";
  json += "\"hostname\":\""; json += String(_s->hostname); json += "\"";
  json += "}";

  replyJson(200, json);
}

void WebManager::handleTimeSave() {
  if (!_store || !_s) {
    replyText(500, "store/settings missing\n");
    return;
  }

  String body = _srv.arg("plain");

  String tz  = jsonStr(body, "tz", String(_s->tz));
  String ntp = jsonStr(body, "ntpServer", String(_s->ntpServer));
  String hn  = jsonStr(body, "hostname", String(_s->hostname));
  int ntpSyncMinutes = jsonInt(body, "ntpSyncMinutes", (int)(_s->ntpSyncIntervalMs / 60000UL));

  tz.trim();
  ntp.trim();
  hn.trim();

  if (tz.length() < 3) {
    replyJson(400, "{\"ok\":false,\"error\":\"bad tz\"}\n");
    return;
  }
  if (ntp.length() < 3) {
    replyJson(400, "{\"ok\":false,\"error\":\"bad ntpServer\"}\n");
    return;
  }
  if (hn.length() < 1) {
    replyJson(400, "{\"ok\":false,\"error\":\"bad hostname\"}\n");
    return;
  }
  if (ntpSyncMinutes < 1 || ntpSyncMinutes > 1440) {
    replyJson(400, "{\"ok\":false,\"error\":\"bad ntpSyncMinutes\"}\n");
    return;
  }

  if (!safeCopy(_s->tz, sizeof(_s->tz), tz)) {
    replyJson(400, "{\"ok\":false,\"error\":\"tz too long\"}\n");
    return;
  }
  if (!safeCopy(_s->ntpServer, sizeof(_s->ntpServer), ntp)) {
    replyJson(400, "{\"ok\":false,\"error\":\"ntpServer too long\"}\n");
    return;
  }
  if (!safeCopy(_s->hostname, sizeof(_s->hostname), hn)) {
    replyJson(400, "{\"ok\":false,\"error\":\"hostname too long\"}\n");
    return;
  }
  _s->ntpSyncIntervalMs = (uint32_t)ntpSyncMinutes * 60000UL;

  _store->save(*_s);

  setenv("TZ", _s->tz, 1);
  tzset();
  configTzTime(_s->tz, _s->ntpServer);
  sntp_set_sync_interval(_s->ntpSyncIntervalMs);
  sntp_restart();
  WiFi.setHostname(_s->hostname);

  Serial0.printf("[WEB] TIME saved: tz='%s' ntp='%s' interval=%lu ms hostname='%s'\n",
                 _s->tz, _s->ntpServer, (unsigned long)_s->ntpSyncIntervalMs, _s->hostname);

  replyJson(200, "{\"ok\":true}\n");
}

void WebManager::handleReboot() {
  replyJson(200, "{\"ok\":true}\n");
  delay(150);
  ESP.restart();
}

void WebManager::handleFactoryReset() {
  if (_store) _store->factoryReset();
  replyJson(200, "{\"ok\":true}\n");
  delay(150);
  ESP.restart();
}

void WebManager::handleOtaGet() {
  if (!_s) {
    replyJson(500, "{\"ok\":false,\"error\":\"settings missing\"}");
    return;
  }

  String json;
  json.reserve(256);
  json += "{";
  json += "\"enabled\":"; json += (_s->otaEnabled ? "true" : "false"); json += ",";
  json += "\"port\":"; json += String(_s->otaPort); json += ",";
  json += "\"hostname\":\""; json += String(_s->otaHostname); json += "\"";
  json += "}";

  replyJson(200, json);
}

void WebManager::handleOtaSave() {
  if (!_store || !_s) {
    replyJson(500, "{\"ok\":false,\"error\":\"store/settings missing\"}");
    return;
  }

  String body = _srv.arg("plain");

  int enabled = jsonInt(body, "enabled", _s->otaEnabled ? 1 : 0);
  int port = jsonInt(body, "port", (int)_s->otaPort);
  String hn = jsonStr(body, "hostname", String(_s->otaHostname));
  String pwd = jsonStr(body, "password", String(_s->otaPassword));

  hn.trim();
  pwd.trim();

  if (port < 1 || port > 65535) {
    replyJson(400, "{\"ok\":false,\"error\":\"bad port\"}");
    return;
  }

  if (!safeCopy(_s->otaHostname, sizeof(_s->otaHostname), hn)) {
    replyJson(400, "{\"ok\":false,\"error\":\"hostname too long\"}");
    return;
  }

  if (!safeCopy(_s->otaPassword, sizeof(_s->otaPassword), pwd)) {
    replyJson(400, "{\"ok\":false,\"error\":\"password too long\"}");
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

  replyJson(200, "{\"ok\":true,\"rebootRequired\":true}");
}

void WebManager::handleMqttGet() {
  if (!_s) {
    replyJson(500, "{\"ok\":false,\"error\":\"settings missing\"}");
    return;
  }

  String json;
  json.reserve(512);
  json += "{";
  json += "\"enabled\":"; json += (_s->mqttEnabled ? "true" : "false"); json += ",";
  json += "\"host\":\""; json += String(_s->mqttHost); json += "\",";
  json += "\"port\":"; json += String(_s->mqttPort); json += ",";
  json += "\"username\":\""; json += String(_s->mqttUsername); json += "\",";
  json += "\"clientId\":\""; json += String(_s->mqttClientId); json += "\",";
  json += "\"baseTopic\":\""; json += String(_s->mqttBaseTopic); json += "\",";
  json += "\"publishPeriodMs\":"; json += String(_s->mqttPublishPeriodMs); json += ",";
  json += "\"retain\":"; json += (_s->mqttRetain ? "true" : "false");
  json += "}";

  replyJson(200, json);
}

void WebManager::handleMqttSave() {
  Serial0.println("[WEB] /api/mqtt POST received");

  if (!_store || !_s) {
    Serial0.println("[WEB] MQTT save failed: store/settings missing");
    replyJson(500, "{\"ok\":false,\"error\":\"store/settings missing\"}");
    return;
  }

  String body = _srv.arg("plain");
  Serial0.printf("[WEB] MQTT raw body: %s\n", body.c_str());

  int enabled = jsonInt(body, "enabled", _s->mqttEnabled ? 1 : 0);
  int port = jsonInt(body, "port", (int)_s->mqttPort);
  int publishMs = jsonInt(body, "publishPeriodMs", (int)_s->mqttPublishPeriodMs);
  int retain = jsonInt(body, "retain", _s->mqttRetain ? 1 : 0);

  String host = jsonStr(body, "host", String(_s->mqttHost));
  String username = jsonStr(body, "username", String(_s->mqttUsername));
  String password = jsonStr(body, "password", String(_s->mqttPassword));
  String clientId = jsonStr(body, "clientId", String(_s->mqttClientId));
  String baseTopic = jsonStr(body, "baseTopic", String(_s->mqttBaseTopic));

  host.trim();
  username.trim();
  password.trim();
  clientId.trim();
  baseTopic.trim();

  if (enabled) {
    if (host.length() < 1) {
      Serial0.println("[WEB] MQTT save error: bad host");
      replyJson(400, "{\"ok\":false,\"error\":\"bad host\"}");
      return;
    }
    if (port < 1 || port > 65535) {
      Serial0.println("[WEB] MQTT save error: bad port");
      replyJson(400, "{\"ok\":false,\"error\":\"bad port\"}");
      return;
    }
    if (clientId.length() < 1) {
      Serial0.println("[WEB] MQTT save error: bad clientId");
      replyJson(400, "{\"ok\":false,\"error\":\"bad clientId\"}");
      return;
    }
    if (baseTopic.length() < 1) {
      Serial0.println("[WEB] MQTT save error: bad baseTopic");
      replyJson(400, "{\"ok\":false,\"error\":\"bad baseTopic\"}");
      return;
    }
  }

  if (publishMs < 250) publishMs = 250;
  if (publishMs > 60000) publishMs = 60000;

  if (!safeCopy(_s->mqttHost, sizeof(_s->mqttHost), host)) {
    Serial0.println("[WEB] MQTT save error: host too long");
    replyJson(400, "{\"ok\":false,\"error\":\"host too long\"}");
    return;
  }
  if (!safeCopy(_s->mqttUsername, sizeof(_s->mqttUsername), username)) {
    Serial0.println("[WEB] MQTT save error: username too long");
    replyJson(400, "{\"ok\":false,\"error\":\"username too long\"}");
    return;
  }
  if (!safeCopy(_s->mqttPassword, sizeof(_s->mqttPassword), password)) {
    Serial0.println("[WEB] MQTT save error: password too long");
    replyJson(400, "{\"ok\":false,\"error\":\"password too long\"}");
    return;
  }
  if (!safeCopy(_s->mqttClientId, sizeof(_s->mqttClientId), clientId)) {
    Serial0.println("[WEB] MQTT save error: clientId too long");
    replyJson(400, "{\"ok\":false,\"error\":\"clientId too long\"}");
    return;
  }
  if (!safeCopy(_s->mqttBaseTopic, sizeof(_s->mqttBaseTopic), baseTopic)) {
    Serial0.println("[WEB] MQTT save error: baseTopic too long");
    replyJson(400, "{\"ok\":false,\"error\":\"baseTopic too long\"}");
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

  replyJson(200, "{\"ok\":true,\"rebootRecommended\":true}");
}

void WebManager::handleRoot() {
  const char* html =
R"HTML(
<!doctype html>
<html lang="fr">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>SoundPanel 7</title>
  <style>
    :root{
      --bg:#0B0F14; --panel:#111824; --panel2:#0F1722; --txt:#DFE7EF; --muted:#8EA1B3;
      --line:#1E2A38; --green:#23C552; --orange:#F0A202; --red:#E53935;
      --radius:18px;
      font-family: system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    }
    *{box-sizing:border-box}
    body{margin:0;background:var(--bg);color:var(--txt)}
    .wrap{max-width:980px;margin:0 auto;padding:16px}
    .top{
      height:70px;background:var(--panel2);border-bottom:1px solid var(--line);
      display:flex;align-items:center;justify-content:space-between;padding:0 16px;
      position:sticky;top:0;z-index:10;
    }
    .title{font-size:22px;font-weight:700;letter-spacing:.2px}
    .gear{
      width:52px;height:42px;border-radius:14px;border:1px solid var(--line);
      display:flex;align-items:center;justify-content:center;
      background:#172133;color:var(--txt);text-decoration:none;font-size:22px;
    }

    .dash{
      min-height:calc(100vh - 70px);
      display:flex;flex-direction:column;gap:14px;
      padding:18px 0;
    }

    .gaugeWrap{
      display:flex;flex-direction:column;align-items:center;justify-content:center;
      gap:10px;padding-top:8px;
    }

    .gauge{
      width:min(78vw,420px);height:min(78vw,420px);border-radius:50%;
      display:grid;place-items:center;position:relative;
      background:
        radial-gradient(circle at center, #0B0F14 0 57%, transparent 58%),
        conic-gradient(var(--gaugeColor, var(--green)) calc(var(--pct)*1%), #1A2332 0);
      transform:rotate(135deg);
    }
    .gauge::before{
      content:"";position:absolute;inset:18px;border-radius:50%;
      background:radial-gradient(circle at center, #0B0F14 0 61%, transparent 62%);
    }
    .gaugeInner{
      position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;
      transform:rotate(-135deg);z-index:2;
    }
    .db{font-size:clamp(52px,10vw,88px);font-weight:800;line-height:1;color:var(--txt);}
    .unit{font-size:clamp(18px,3vw,24px);color:var(--muted);margin-top:4px;}
    .dot{width:14px;height:14px;border-radius:50%;background:var(--gaugeColor, var(--green));margin-top:14px;}

    .cards{
      display:grid;grid-template-columns:1fr 1fr;gap:12px;width:min(92vw,760px);margin-top:8px;
    }
    .card{
      background:var(--panel);border:1px solid var(--line);border-radius:var(--radius);padding:14px;
      box-shadow:0 10px 30px rgba(0,0,0,.22);
    }
    .card .k{color:var(--muted);font-size:14px}
    .card .v{font-size:30px;font-weight:800;margin-top:10px}

    .histCard{
      width:min(92vw,760px);
      background:var(--panel);border:1px solid var(--line);border-radius:var(--radius);padding:14px;
      box-shadow:0 10px 30px rgba(0,0,0,.22);
      overflow:hidden;
    }
    .histTop{
      display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:10px;
    }
    .histMeta{font-size:12px;color:var(--muted)}
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
      width:100%;height:100%;display:flex;align-items:flex-end;gap:2px;
    }
    .histBar{
      flex:1 1 0;min-width:2px;height:4px;border-radius:3px 3px 0 0;
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

    .bottom{
      display:flex;justify-content:space-between;align-items:center;
      color:#6F8192;font-size:16px;padding:0 6px;margin-top:4px;
    }

    @media (max-width:700px){
      .cards{grid-template-columns:1fr}
      .bottom{flex-direction:column;gap:8px}
      .title{font-size:18px}
      .chartWrap{height:180px}
    }
  </style>
</head>
<body>
  <div class="top">
    <div class="title">SoundPanel 7</div>
    <a class="gear" href="/admin" title="Administration">⚙</a>
  </div>

  <div class="wrap">
    <div class="dash">
      <div class="gaugeWrap">
        <div class="gauge" id="gauge" style="--pct:0;--gaugeColor:var(--green);">
          <div class="gaugeInner">
            <div class="db" id="db">--.-</div>
            <div class="unit">dB</div>
            <div class="dot" id="dot"></div>
          </div>
        </div>

        <div class="cards">
          <div class="card">
            <div class="k">Leq</div>
            <div class="v" id="leq">--.-</div>
          </div>
          <div class="card">
            <div class="k">Peak</div>
            <div class="v" id="peak">--.-</div>
          </div>
        </div>

        <div class="histCard">
          <div class="histTop">
            <div class="histMeta" id="histMeta">Dernières minutes</div>
          </div>
          <div class="chartWrap">
            <div class="chartPlot">
              <div class="histBars" id="histBars"></div>
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
            <div id="histLeft">-5m</div>
            <div class="histAxisMid" id="histMid">-2m</div>
            <div id="histRight">0</div>
          </div>
        </div>
      </div>

      <div class="bottom">
        <div id="wifi">WiFi: --</div>
        <div id="time">--:--:--</div>
      </div>
    </div>
  </div>

<script>
  const gauge = document.getElementById("gauge");
  const dbEl = document.getElementById("db");
  const leqEl = document.getElementById("leq");
  const peakEl = document.getElementById("peak");
  const wifiEl = document.getElementById("wifi");
  const timeEl = document.getElementById("time");
  const dot = document.getElementById("dot");
  const histMeta = document.getElementById("histMeta");
  const histBars = document.getElementById("histBars");
  const histLeft = document.getElementById("histLeft");
  const histMid = document.getElementById("histMid");
  const histRight = document.getElementById("histRight");

  let historyValues = [];
  let historyMinutes = 5;
  let historyCapacity = 96;
  let historySamplePeriodMs = 3000;
  let lastHistorySampleClientMs = 0;
  let events = null;
  let reconnectTimer = null;
  let hasLiveFeed = false;
  let clockBaseMs = 0;
  let clockSyncClientMs = 0;

  function pad2(v){
    return String(v).padStart(2, "0");
  }

  function renderClock(){
    if (!clockBaseMs) {
      timeEl.textContent = "NTP...";
      return;
    }
    const nowMs = clockBaseMs + (Date.now() - clockSyncClientMs);
    const d = new Date(nowMs);
    timeEl.textContent = `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
  }

  function syncClock(serverTime){
    if (!serverTime) {
      clockBaseMs = 0;
      clockSyncClientMs = 0;
      renderClock();
      return;
    }

    const isoLike = serverTime.replace(" ", "T");
    const parsed = Date.parse(isoLike);
    if (Number.isNaN(parsed)) {
      clockBaseMs = 0;
      clockSyncClientMs = 0;
      renderClock();
      return;
    }

    clockBaseMs = parsed;
    clockSyncClientMs = Date.now();
    renderClock();
  }

  function zoneColor(db, greenMax, orangeMax){
    if(db <= greenMax) return "#23C552";
    if(db <= orangeMax) return "#F0A202";
    return "#E53935";
  }

  function historyHeightPercent(db){
    const histDbMin = 35;
    const histDbMax = 130;
    const clamped = Math.max(histDbMin, Math.min(histDbMax, Number(db) || 0));
    const norm = (clamped - histDbMin) / (histDbMax - histDbMin);
    return norm * 100;
  }

  function updateHistoryLabels(){
    histMeta.textContent = `${historyMinutes} min d’historique`;
    histLeft.textContent = `-${historyMinutes}m`;
    histMid.textContent = `-${Math.max(1, Math.floor(historyMinutes / 2))}m`;
    histRight.textContent = "";
  }

  function trimHistory(){
    if (historyValues.length > historyCapacity) {
      historyValues = historyValues.slice(-historyCapacity);
    }
  }

  function setHistory(values){
    historyValues = Array.isArray(values) ? values.map(v => Number(v) || 0) : [];
    trimHistory();
    drawHistory();
  }

  function appendHistory(db, force){
    const now = Date.now();
    if (!force && lastHistorySampleClientMs && (now - lastHistorySampleClientMs) < historySamplePeriodMs) {
      return;
    }

    lastHistorySampleClientMs = now;
    historyValues.push(Number(db) || 0);
    trimHistory();
    drawHistory();
  }

  function drawHistory(){
    const values = historyValues.slice(-historyCapacity);
    const greenMax = window.__greenMax ?? 55;
    const orangeMax = window.__orangeMax ?? 70;

    let html = "";
    const missing = Math.max(0, historyCapacity - values.length);
    for (let i = 0; i < missing; i++) {
      html += '<div class="histBar"></div>';
    }

    for (const value of values) {
      html += `<div class="histBar" style="height:${historyHeightPercent(value).toFixed(1)}%;background:${zoneColor(value, greenMax, orangeMax)}"></div>`;
    }

    histBars.innerHTML = html;
  }

  function applyLiveMetrics(st){
    const db = Number(st.db ?? 0);
    const leq = Number(st.leq ?? 0);
    const peak = Number(st.peak ?? 0);
    const greenMax = Number(st.greenMax ?? window.__greenMax ?? 55);
    const orangeMax = Number(st.orangeMax ?? window.__orangeMax ?? 70);
    historyMinutes = Number(st.historyMinutes ?? historyMinutes ?? 5);
    historyCapacity = Number(st.historyCapacity ?? historyCapacity ?? 96);
    historySamplePeriodMs = Number(st.historySamplePeriodMs ?? historySamplePeriodMs ?? 3000);

    window.__greenMax = greenMax;
    window.__orangeMax = orangeMax;

    const pct = Math.max(0, Math.min(100, db));
    const color = zoneColor(db, greenMax, orangeMax);

    gauge.style.setProperty("--pct", pct);
    gauge.style.setProperty("--gaugeColor", color);
    dot.style.background = color;

    dbEl.textContent = db.toFixed(1);
    leqEl.textContent = leq.toFixed(1);
    peakEl.textContent = peak.toFixed(1);

    if ("time_ok" in st) {
      syncClock(st.time_ok ? st.time : "");
    }

    if ("wifi" in st) {
      wifiEl.textContent = st.wifi ? `WiFi: OK (${st.ip})` : "WiFi: OFF";
    }

    if (Array.isArray(st.history)) {
      setHistory(st.history);
      lastHistorySampleClientMs = Date.now();
    } else {
      appendHistory(db, false);
    }

    updateHistoryLabels();
  }

  async function refreshStatus(){
    try{
      const r = await fetch("/api/status", {cache:"no-store"});
      const st = await r.json();
      applyLiveMetrics(st);
    }catch(e){
      wifiEl.textContent = hasLiveFeed ? wifiEl.textContent : "WiFi: erreur";
    }
  }

  function scheduleReconnect(){
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connectLiveFeed();
    }, 1500);
  }

  function connectLiveFeed(){
    if (events) {
      events.close();
      events = null;
    }

    const liveUrl = `${location.protocol}//${location.hostname}:81/api/events`;
    events = new EventSource(liveUrl);

    events.addEventListener("metrics", (ev) => {
      hasLiveFeed = true;
      applyLiveMetrics(JSON.parse(ev.data));
    });

    events.onerror = () => {
      hasLiveFeed = false;
      if (events) {
        events.close();
        events = null;
      }
      scheduleReconnect();
    };
  }

  refreshStatus();
  connectLiveFeed();
  setInterval(renderClock, 1000);
</script>
</body>
</html>
)HTML";

  _srv.send(200, "text/html; charset=utf-8", html);
}

void WebManager::handleAdmin() {
  const char* html =
R"HTML(
<!doctype html>
<html lang="fr">
<head>
  <meta charset="utf-8"/>
  <meta name="viewport" content="width=device-width,initial-scale=1"/>
  <title>SoundPanel 7 — Admin</title>
  <style>
    :root{
      --bg:#0B0F14; --card:#111824; --muted:#8EA1B3; --txt:#DFE7EF; --line:#1E2A38;
      --ok:#23C552; --bad:#E53935; --btn:#172133;
      --radius:16px;
      font-family: system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    }
    body{margin:0;background:var(--bg);color:var(--txt);}
    header{
      position:sticky;top:0;z-index:9;
      background:rgba(11,15,20,.9);backdrop-filter:blur(10px);
      border-bottom:1px solid var(--line);
    }
    .wrap{max-width:980px;margin:0 auto;padding:18px;}
    .title{display:flex;align-items:center;justify-content:space-between;gap:12px;}
    h1{font-size:18px;margin:0;letter-spacing:.2px;}
    .pill{font-size:12px;color:var(--muted);border:1px solid var(--line);padding:6px 10px;border-radius:999px;}
    .back{
      text-decoration:none;color:var(--txt);border:1px solid var(--line);
      background:#172133;padding:8px 12px;border-radius:12px;font-weight:650;
    }
    .grid{display:grid;grid-template-columns:1fr;gap:12px;margin-top:12px;}
    @media(min-width:900px){ .grid{grid-template-columns:1fr 1fr;} }
    .card{
      background:var(--card);border:1px solid var(--line);border-radius:var(--radius);
      padding:14px;box-shadow:0 10px 30px rgba(0,0,0,.25);
    }
    .card h2{margin:0 0 10px 0;font-size:14px;color:var(--txt);font-weight:650;}
    .row{display:flex;gap:10px;align-items:center;justify-content:space-between;}
    .kv{display:flex;flex-direction:column;gap:4px;}
    .k{color:var(--muted);font-size:12px;}
    .v{font-size:14px;font-weight:600;}
    .badge{font-size:12px;padding:6px 10px;border-radius:999px;border:1px solid var(--line);}
    .ok{color:var(--ok);}
    .bad{color:var(--bad);}
    .btns{display:flex;gap:10px;flex-wrap:wrap;}
    button{
      appearance:none;border:1px solid var(--line);background:var(--btn);color:var(--txt);
      padding:10px 12px;border-radius:12px;font-weight:650;cursor:pointer;
    }
    button.danger{background:#2A1212;border-color:#3B1A1A;}
    .field{display:flex;flex-direction:column;gap:6px;margin-top:10px;}
    label{font-size:12px;color:var(--muted);}
    input[type="range"]{width:100%;}
    input[type="text"], input[type="number"], input[type="password"], select{
      width:100%;
      border:1px solid var(--line);
      background:#0E141C;
      color:var(--txt);
      border-radius:12px;
      padding:10px 12px;
      outline:none;
      font-weight:600;
      box-sizing:border-box;
    }
    .toast{margin-top:10px;font-size:12px;color:var(--muted);white-space:pre-wrap;}
    .hint{font-size:12px;color:var(--muted);line-height:1.35;}
    .calrow{display:grid;grid-template-columns:1fr auto;gap:10px;align-items:end;margin-top:10px;}
    code{color:#B9C7D6;}
  </style>
</head>
<body>
<header>
  <div class="wrap title">
    <h1>SoundPanel 7 — Administration</h1>
    <div style="display:flex;gap:10px;align-items:center">
      <div class="pill" id="pill">…</div>
      <a class="back" href="/">↩ Dashboard</a>
    </div>
  </div>
</header>

<main class="wrap">
  <div class="grid">
    <section class="card">
      <h2>Statut</h2>
      <div class="row">
        <div class="kv">
          <div class="k">Wi-Fi</div>
          <div class="v" id="wifi">—</div>
        </div>
        <div class="badge" id="wifiBadge">—</div>
      </div>
      <div style="height:10px"></div>
      <div class="row">
        <div class="kv">
          <div class="k">IP</div>
          <div class="v" id="ip">—</div>
        </div>
        <div class="kv" style="text-align:right">
          <div class="k">RSSI</div>
          <div class="v" id="rssi">—</div>
        </div>
      </div>
      <div style="height:10px"></div>
      <div class="row">
        <div class="kv">
          <div class="k">Heure</div>
          <div class="v" id="time">—</div>
        </div>
        <div class="kv" style="text-align:right">
          <div class="k">Uptime</div>
          <div class="v" id="up">—</div>
        </div>
      </div>
    </section>

    <section class="card">
      <h2>Audio brut</h2>
      <div class="row">
        <div class="kv">
          <div class="k">rawRms</div>
          <div class="v" id="rawRms">—</div>
        </div>
        <div class="kv" style="text-align:right">
          <div class="k">pseudo dB</div>
          <div class="v" id="rawPseudoDb">—</div>
        </div>
      </div>
      <div style="height:10px"></div>
      <div class="row">
        <div class="kv">
          <div class="k">ADC Mean</div>
          <div class="v" id="rawAdcMean">—</div>
        </div>
        <div class="kv" style="text-align:right">
          <div class="k">ADC Last</div>
          <div class="v" id="rawAdcLast">—</div>
        </div>
      </div>
    </section>

    <section class="card">
      <h2>UI (écran)</h2>

      <div class="field">
        <label>Backlight <span id="blVal">—</span>%</label>
        <input id="bl" type="range" min="0" max="100" value="80"/>
      </div>

      <div class="field">
        <label>Seuil vert max <span id="gVal">—</span>dB</label>
        <input id="g" type="range" min="0" max="100" value="55"/>
      </div>

      <div class="field">
        <label>Seuil orange max <span id="oVal">—</span>dB</label>
        <input id="o" type="range" min="0" max="100" value="70"/>
      </div>

      <div class="field">
        <label>Historique <span id="hVal">—</span> min</label>
        <input id="hist" type="range" min="1" max="60" value="5"/>
      </div>

      <div class="field">
        <label>Délai warning (orange)</label>
        <input id="warnHoldSec" type="number" min="0" max="60" step="1" value="3"/>
        <div class="hint">Temps en secondes avant alerte warning.</div>
      </div>

      <div class="field">
        <label>Délai critique (rouge)</label>
        <input id="critHoldSec" type="number" min="0" max="60" step="1" value="2"/>
        <div class="hint">Temps en secondes avant alerte critique.</div>
      </div>

      <div class="btns" style="margin-top:10px">
        <button id="saveUi">Sauver UI</button>
      </div>

      <div class="toast" id="toastUi"></div>
    </section>

    <section class="card">
      <h2>Heure (NTP / TZ)</h2>

      <div class="field">
        <label>NTP Server</label>
        <input id="ntp" type="text" placeholder="fr.pool.ntp.org"/>
      </div>

      <div class="field">
        <label>Sync NTP (minutes)</label>
        <input id="ntpSyncMin" type="number" min="1" max="1440" step="1" value="180"/>
        <div class="hint">Defaut recommande ESP32: <code>180</code> min (3 h).</div>
      </div>

      <div class="field">
        <label>TZ (POSIX)</label>
        <input id="tz" type="text" placeholder="CET-1CEST,M3.5.0/2,M10.5.0/3"/>
        <div class="hint">Ex Paris : <code>CET-1CEST,M3.5.0/2,M10.5.0/3</code></div>
      </div>

      <div class="field">
        <label>Hostname</label>
        <input id="hn" type="text" placeholder="soundpanel7"/>
      </div>

      <div class="btns" style="margin-top:10px">
        <button id="saveTime">Sauver Heure</button>
      </div>

      <div class="toast" id="toastTime"></div>
    </section>

    <section class="card">
      <h2>Calibration micro</h2>

      <div class="hint">
        Mets le vrai sonomètre à côté, saisis la valeur réelle, puis clique sur Capture.
      </div>

      <div class="calrow">
        <div class="field">
          <label>Point 1 (dB réel)</label>
          <input id="cal1" type="number" step="0.1" placeholder="40.0"/>
        </div>
        <button onclick="captureCal(0)">Capture P1</button>
      </div>
      <div class="hint" id="calState1">P1: —</div>

      <div class="calrow">
        <div class="field">
          <label>Point 2 (dB réel)</label>
          <input id="cal2" type="number" step="0.1" placeholder="65.0"/>
        </div>
        <button onclick="captureCal(1)">Capture P2</button>
      </div>
      <div class="hint" id="calState2">P2: —</div>

      <div class="calrow">
        <div class="field">
          <label>Point 3 (dB réel)</label>
          <input id="cal3" type="number" step="0.1" placeholder="85.0"/>
        </div>
        <button onclick="captureCal(2)">Capture P3</button>
      </div>
      <div class="hint" id="calState3">P3: —</div>

      <div class="btns" style="margin-top:10px">
        <button class="danger" onclick="clearCal()">Effacer calibration</button>
      </div>

      <div class="toast" id="toastCal"></div>
    </section>

    <section class="card">
      <h2>OTA</h2>

      <div class="field">
        <label>Activer OTA</label>
        <select id="otaEnabled">
          <option value="1">Oui</option>
          <option value="0">Non</option>
        </select>
      </div>

      <div class="field">
        <label>Hostname OTA</label>
        <input id="otaHostname" type="text" placeholder="soundpanel7"/>
      </div>

      <div class="field">
        <label>Port OTA</label>
        <input id="otaPort" type="number" min="1" max="65535" placeholder="3232"/>
      </div>

      <div class="field">
        <label>Mot de passe OTA</label>
        <input id="otaPassword" type="password" placeholder="laisser vide = aucun mot de passe"/>
      </div>

      <div class="btns" style="margin-top:10px">
        <button id="saveOta">Sauver OTA</button>
      </div>

      <div class="toast" id="toastOta"></div>
    </section>

    <section class="card">
      <h2>MQTT</h2>

      <div class="field">
        <label>Activer MQTT</label>
        <select id="mqttEnabled">
          <option value="1">Oui</option>
          <option value="0">Non</option>
        </select>
      </div>

      <div class="field">
        <label>Broker / Host</label>
        <input id="mqttHost" type="text" placeholder="192.168.1.10"/>
      </div>

      <div class="field">
        <label>Port</label>
        <input id="mqttPort" type="number" min="1" max="65535" placeholder="1883"/>
      </div>

      <div class="field">
        <label>Username</label>
        <input id="mqttUsername" type="text" placeholder="laisser vide si non utilisé"/>
      </div>

      <div class="field">
        <label>Password</label>
        <input id="mqttPassword" type="password" placeholder="laisser vide si non utilisé"/>
      </div>

      <div class="field">
        <label>Client ID</label>
        <input id="mqttClientId" type="text" placeholder="soundpanel7"/>
      </div>

      <div class="field">
        <label>Base topic</label>
        <input id="mqttBaseTopic" type="text" placeholder="soundpanel7"/>
      </div>

      <div class="field">
        <label>Publish period (ms)</label>
        <input id="mqttPublishPeriodMs" type="number" min="250" max="60000" placeholder="1000"/>
      </div>

      <div class="field">
        <label>Retain</label>
        <select id="mqttRetain">
          <option value="0">Non</option>
          <option value="1">Oui</option>
        </select>
      </div>

      <div class="btns" style="margin-top:10px">
        <button id="saveMqtt">Sauver MQTT</button>
      </div>

      <div class="toast" id="toastMqtt"></div>
    </section>

    <section class="card">
      <h2>Actions</h2>
      <div class="btns">
        <button id="reboot">Reboot</button>
        <button class="danger" id="reset">Factory reset</button>
      </div>
      <div class="toast" id="toastActions"></div>
    </section>
  </div>
</main>

<script>
  const $ = (id)=>document.getElementById(id);

  let uiDirty = false;
  let uiLastInteraction = 0;

  function markUiDirty() {
    uiDirty = true;
    uiLastInteraction = Date.now();
  }

  function clearUiDirty() {
    uiDirty = false;
    uiLastInteraction = 0;
  }

  function fmtUptime(sec){
    sec = Math.max(0, sec|0);
    const h = (sec/3600)|0; sec -= h*3600;
    const m = (sec/60)|0; sec -= m*60;
    return `${h}h ${m}m ${sec}s`;
  }

  async function apiGet(url){
    const r = await fetch(url, {cache:"no-store"});
    if(!r.ok) throw new Error(await r.text());
    return await r.json();
  }

  async function apiPost(url, obj){
    const r = await fetch(url, {
      method:"POST",
      headers: {"Content-Type":"application/json"},
      body: obj ? JSON.stringify(obj) : "{}"
    });
    if(!r.ok) throw new Error(await r.text());
    return await r.json();
  }

  function bindRange(id, outId){
    const el = $(id), out = $(outId);
    const upd = () => {
      out.textContent = el.value;
      markUiDirty();
    };
    el.addEventListener("input", upd);
    el.addEventListener("change", upd);
    out.textContent = el.value;
  }

  bindRange("bl","blVal");
  bindRange("g","gVal");
  bindRange("o","oVal");
  bindRange("hist","hVal");
  $("warnHoldSec").addEventListener("input", markUiDirty);
  $("warnHoldSec").addEventListener("change", markUiDirty);
  $("critHoldSec").addEventListener("input", markUiDirty);
  $("critHoldSec").addEventListener("change", markUiDirty);

  async function loadTime(){
    try{
      const t = await apiGet("/api/time");
      $("tz").value = t.tz || "";
      $("ntp").value = t.ntpServer || "";
      $("ntpSyncMin").value = String(t.ntpSyncMinutes || 180);
      $("hn").value = t.hostname || "";
    } catch(e){}
  }

  function showCalState(st) {
    for (let i = 0; i < 3; i++) {
      const c = st.cal && st.cal[i] ? st.cal[i] : null;
      if (!c || !c.valid) {
        $("calState"+(i+1)).textContent = `P${i+1}: non capturé`;
      } else {
        $("calState"+(i+1)).textContent =
          `P${i+1}: ref=${Number(c.refDb).toFixed(1)} dB | rawLogRms=${Number(c.rawLogRms).toFixed(4)}`;
      }
    }
  }

  async function refresh(){
    try{
      const st = await apiGet("/api/status");
      $("pill").textContent = st.wifi ? "En ligne" : "Hors ligne";
      $("wifi").textContent = st.wifi ? "Connecté" : "Déconnecté";
      $("ip").textContent = st.ip || "—";
      $("rssi").textContent = st.wifi ? (st.rssi + " dBm") : "—";
      $("up").textContent = fmtUptime(st.uptime_s || 0);
      $("time").textContent = st.time_ok ? st.time : "NTP…";
      $("wifiBadge").textContent = st.wifi ? "OK" : "OFF";
      $("wifiBadge").className = "badge " + (st.wifi ? "ok" : "bad");

      $("rawRms").textContent = Number(st.rawRms ?? 0).toFixed(2);
      $("rawPseudoDb").textContent = Number(st.rawPseudoDb ?? 0).toFixed(1);
      $("rawAdcMean").textContent = String(st.rawAdcMean ?? "—");
      $("rawAdcLast").textContent = String(st.rawAdcLast ?? "—");

      showCalState(st);

      const safeToSyncUi = !uiDirty && (Date.now() - uiLastInteraction > 1500);
      if (safeToSyncUi) {
        if (typeof st.backlight === "number") {
          $("bl").value = st.backlight;
          $("blVal").textContent = st.backlight;
        }
        if (typeof st.greenMax === "number") {
          $("g").value = st.greenMax;
          $("gVal").textContent = st.greenMax;
        }
        if (typeof st.orangeMax === "number") {
          $("o").value = st.orangeMax;
          $("oVal").textContent = st.orangeMax;
        }
        if (typeof st.historyMinutes === "number") {
          $("hist").value = st.historyMinutes;
          $("hVal").textContent = st.historyMinutes;
        }
        if (typeof st.warningHoldSec === "number") {
          $("warnHoldSec").value = String(st.warningHoldSec);
        }
        if (typeof st.criticalHoldSec === "number") {
          $("critHoldSec").value = String(st.criticalHoldSec);
        }
      }
    } catch(e){
      $("pill").textContent = "Erreur";
    }
  }

  $("saveUi").onclick = async ()=>{
    $("toastUi").textContent = "Sauvegarde…";
    try{
      let g = parseInt($("g").value,10);
      let o = parseInt($("o").value,10);
      if (o < g) {
        o = g;
        $("o").value = String(o);
        $("oVal").textContent = String(o);
      }

      await apiPost("/api/ui", {
        backlight: parseInt($("bl").value,10),
        greenMax: g,
        orangeMax: o,
        historyMinutes: parseInt($("hist").value,10),
        warningHoldSec: parseInt($("warnHoldSec").value,10),
        criticalHoldSec: parseInt($("critHoldSec").value,10)
      });

      clearUiDirty();
      $("toastUi").textContent = "OK ✅";
      await refresh();
    } catch(e){
      $("toastUi").textContent = "Erreur ❌";
    }
  };

  $("saveTime").onclick = async ()=>{
    $("toastTime").textContent = "Sauvegarde…";
    try{
      await apiPost("/api/time", {
        tz: $("tz").value.trim(),
        ntpServer: $("ntp").value.trim(),
        ntpSyncMinutes: parseInt($("ntpSyncMin").value, 10),
        hostname: $("hn").value.trim()
      });
      $("toastTime").textContent = "OK ✅";
    } catch(e){
      $("toastTime").textContent = "Erreur ❌";
    }
  };

  async function captureCal(index){
    const input = $("cal" + (index + 1));
    const refDb = parseFloat(input.value);
    if (!(refDb > 0)) {
      $("toastCal").textContent = "Entre une valeur dB valide";
      return;
    }

    $("toastCal").textContent = "Capture…";
    try{
      await apiPost("/api/calibrate", { index, refDb });
      $("toastCal").textContent = `Point ${index + 1} capturé ✅`;
      await refresh();
    } catch(e){
      $("toastCal").textContent = "Erreur capture ❌";
    }
  }

  async function loadOta(){
    try{
      const o = await apiGet("/api/ota");
      $("otaEnabled").value = o.enabled ? "1" : "0";
      $("otaHostname").value = o.hostname || "";
      $("otaPort").value = o.port || 3232;
      $("otaPassword").value = "";
    } catch(e){}
  }

  $("saveOta").onclick = async ()=>{
    $("toastOta").textContent = "Sauvegarde…";
    try{
      const r = await apiPost("/api/ota", {
        enabled: parseInt($("otaEnabled").value, 10),
        hostname: $("otaHostname").value.trim(),
        port: parseInt($("otaPort").value, 10),
        password: $("otaPassword").value
      });

      if (r.rebootRequired) {
        $("toastOta").textContent = "OK ✅ Reboot requis";
      } else {
        $("toastOta").textContent = "OK ✅";
      }
    } catch(e){
      $("toastOta").textContent = "Erreur ❌";
    }
  };

  async function loadMqtt(){
    try{
      const m = await apiGet("/api/mqtt");
      $("mqttEnabled").value = m.enabled ? "1" : "0";
      $("mqttHost").value = m.host || "";
      $("mqttPort").value = m.port || 1883;
      $("mqttUsername").value = m.username || "";
      $("mqttPassword").value = "";
      $("mqttClientId").value = m.clientId || "soundpanel7";
      $("mqttBaseTopic").value = m.baseTopic || "soundpanel7";
      $("mqttPublishPeriodMs").value = m.publishPeriodMs || 1000;
      $("mqttRetain").value = m.retain ? "1" : "0";
    } catch(e){}
  }

  $("saveMqtt").onclick = async ()=>{
    $("toastMqtt").textContent = "Sauvegarde…";
    try{
      const payload = {
        enabled: parseInt($("mqttEnabled").value || "0", 10),
        host: ($("mqttHost").value || "").trim(),
        port: parseInt($("mqttPort").value || "1883", 10),
        username: ($("mqttUsername").value || "").trim(),
        password: ($("mqttPassword").value || ""),
        clientId: ($("mqttClientId").value || "").trim(),
        baseTopic: ($("mqttBaseTopic").value || "").trim(),
        publishPeriodMs: parseInt($("mqttPublishPeriodMs").value || "1000", 10),
        retain: parseInt($("mqttRetain").value || "0", 10)
      };

      console.log("MQTT payload", payload);

      const r = await apiPost("/api/mqtt", payload);

      if (r.rebootRecommended) {
        $("toastMqtt").textContent = "OK ✅ Reboot recommandé";
      } else {
        $("toastMqtt").textContent = "OK ✅";
      }
    } catch(e){
      console.error("MQTT save error", e);
      $("toastMqtt").textContent = "Erreur ❌";
    }
  };

  async function clearCal(){
    $("toastCal").textContent = "Effacement…";
    try{
      await apiPost("/api/calibrate/clear");
      $("toastCal").textContent = "Calibration effacée ✅";
      await refresh();
    } catch(e){
      $("toastCal").textContent = "Erreur effacement ❌";
    }
  }

  $("reboot").onclick = async ()=>{
    $("toastActions").textContent = "Reboot…";
    try { await apiPost("/api/reboot"); } catch(e){}
  };

  $("reset").onclick = async ()=>{
    if(!confirm("Factory reset ?")) return;
    $("toastActions").textContent = "Factory reset…";
    try { await apiPost("/api/factory_reset"); } catch(e){}
  };

  refresh();
  loadTime();
  loadOta();
  loadMqtt();
  setInterval(refresh, 1500);
</script>
</body>
</html>
)HTML";

  _srv.send(200, "text/html; charset=utf-8", html);
}
