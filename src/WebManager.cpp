#include "WebManager.h"

#include <WiFi.h>
#include <AsyncTCP.h>
#include <esp_sntp.h>
#include <ctime>
#include <math.h>
#include <cstring>

#include "AudioEngine.h"
#include "AppConfig.h"

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
                       SharedHistory* history,
                       OtaManager* ota,
                       MqttManager* mqtt) {
  _store = store;
  _s = settings;
  _net = net;
  _board = board;
  _history = history;
  _ota = ota;
  _mqtt = mqtt;

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

  _srv.on("/api/status", HTTP_GET, [this]() { handleStatus(); });

  _srv.on("/api/ui", HTTP_POST, [this]() { handleUiSave(); });

  _srv.on("/api/time", HTTP_GET,  [this]() { handleTimeGet(); });
  _srv.on("/api/time", HTTP_POST, [this]() { handleTimeSave(); });
  _srv.on("/api/config/export", HTTP_GET, [this]() { handleConfigExport(); });
  _srv.on("/api/config/import", HTTP_POST, [this]() { handleConfigImport(); });
  _srv.on("/api/config/backup", HTTP_POST, [this]() { handleConfigBackup(); });
  _srv.on("/api/config/restore", HTTP_POST, [this]() { handleConfigRestore(); });
  _srv.on("/api/config/reset_partial", HTTP_POST, [this]() { handleConfigResetPartial(); });

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
  const float mcuTempC = temperatureRead();
  const bool mcuTempOk = !isnan(mcuTempC);

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
  json += "\"version\":\""; json += String(SOUNDPANEL7_VERSION); json += "\",";
  json += "\"buildDate\":\""; json += String(SOUNDPANEL7_BUILD_DATE); json += "\",";
  json += "\"buildEnv\":\""; json += String(SOUNDPANEL7_BUILD_ENV); json += "\",";
  json += "\"mcuTempOk\":"; json += (mcuTempOk ? "true" : "false"); json += ",";
  json += "\"mcuTempC\":"; json += (mcuTempOk ? String(mcuTempC, 1) : String("0")); json += ",";
  json += "\"otaEnabled\":"; json += (_ota && _ota->enabled() ? "true" : "false"); json += ",";
  json += "\"otaStarted\":"; json += (_ota && _ota->started() ? "true" : "false"); json += ",";
  json += "\"mqttEnabled\":"; json += (_mqtt && _mqtt->enabled() ? "true" : "false"); json += ",";
  json += "\"mqttConnected\":"; json += (_mqtt && _mqtt->connected() ? "true" : "false"); json += ",";
  json += "\"mqttLastError\":\""; json += String((_mqtt && _mqtt->lastError()) ? _mqtt->lastError() : ""); json += "\",";
  json += "\"backlight\":"; json += String(_s ? _s->backlight : 0); json += ",";
  json += "\"greenMax\":"; json += String(_s ? _s->th.greenMax : 55); json += ",";
  json += "\"orangeMax\":"; json += String(_s ? _s->th.orangeMax : 70); json += ",";
  json += "\"historyMinutes\":"; json += String(_s ? _s->historyMinutes : 5); json += ",";
  json += "\"audioResponseMode\":"; json += String(_s ? _s->audioResponseMode : 0); json += ",";
  json += "\"historyCapacity\":"; json += String(SharedHistory::POINT_COUNT); json += ",";
  json += "\"historySamplePeriodMs\":"; json += String(_history ? _history->samplePeriodMs() : 3000); json += ",";
  json += "\"warningHoldSec\":"; json += String(_s ? (_s->orangeAlertHoldMs / 1000UL) : 3); json += ",";
  json += "\"criticalHoldSec\":"; json += String(_s ? (_s->redAlertHoldMs / 1000UL) : 2); json += ",";
  json += "\"calibrationCaptureSec\":"; json += String(_s ? (_s->calibrationCaptureMs / 1000UL) : 3); json += ",";
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
  const float mcuTempC = temperatureRead();
  const bool mcuTempOk = !isnan(mcuTempC);
  struct tm ti;
  bool hasTime = getLocalTime(&ti, 0);
  char tbuf[32] = {0};
  if (hasTime) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);
  const AudioMetrics& am = g_audio.metrics();

  String json;
  json.reserve(512);
  json += "{";
  json += "\"db\":"; json += String(g_webDbInstant, 1); json += ",";
  json += "\"leq\":"; json += String(g_webLeq, 1); json += ",";
  json += "\"peak\":"; json += String(g_webPeak, 1); json += ",";
  json += "\"backlight\":"; json += String(_s ? _s->backlight : 0); json += ",";
  json += "\"greenMax\":"; json += String(_s ? _s->th.greenMax : 55); json += ",";
  json += "\"orangeMax\":"; json += String(_s ? _s->th.orangeMax : 70); json += ",";
  json += "\"historyMinutes\":"; json += String(_s ? _s->historyMinutes : 5); json += ",";
  json += "\"audioResponseMode\":"; json += String(_s ? _s->audioResponseMode : 0); json += ",";
  json += "\"historyCapacity\":"; json += String(SharedHistory::POINT_COUNT); json += ",";
  json += "\"historySamplePeriodMs\":"; json += String(_history ? _history->samplePeriodMs() : 3000); json += ",";
  json += "\"warningHoldSec\":"; json += String(_s ? (_s->orangeAlertHoldMs / 1000UL) : 3); json += ",";
  json += "\"criticalHoldSec\":"; json += String(_s ? (_s->redAlertHoldMs / 1000UL) : 2); json += ",";
  json += "\"calibrationCaptureSec\":"; json += String(_s ? (_s->calibrationCaptureMs / 1000UL) : 3); json += ",";
  json += "\"wifi\":"; json += (WiFi.isConnected() ? "true" : "false"); json += ",";
  json += "\"ip\":\""; json += ip; json += "\",";
  json += "\"rssi\":"; json += String(rssi); json += ",";
  json += "\"version\":\""; json += String(SOUNDPANEL7_VERSION); json += "\",";
  json += "\"buildDate\":\""; json += String(SOUNDPANEL7_BUILD_DATE); json += "\",";
  json += "\"buildEnv\":\""; json += String(SOUNDPANEL7_BUILD_ENV); json += "\",";
  json += "\"mcuTempOk\":"; json += (mcuTempOk ? "true" : "false"); json += ",";
  json += "\"mcuTempC\":"; json += (mcuTempOk ? String(mcuTempC, 1) : String("0")); json += ",";
  json += "\"otaEnabled\":"; json += (_ota && _ota->enabled() ? "true" : "false"); json += ",";
  json += "\"otaStarted\":"; json += (_ota && _ota->started() ? "true" : "false"); json += ",";
  json += "\"mqttEnabled\":"; json += (_mqtt && _mqtt->enabled() ? "true" : "false"); json += ",";
  json += "\"mqttConnected\":"; json += (_mqtt && _mqtt->connected() ? "true" : "false"); json += ",";
  json += "\"mqttLastError\":\""; json += String((_mqtt && _mqtt->lastError()) ? _mqtt->lastError() : ""); json += "\",";
  json += "\"rawRms\":"; json += String(am.rawRms, 2); json += ",";
  json += "\"rawPseudoDb\":"; json += String(am.rawPseudoDb, 1); json += ",";
  json += "\"rawAdcMean\":"; json += String(am.rawAdcMean); json += ",";
  json += "\"rawAdcLast\":"; json += String(am.rawAdcLast); json += ",";
  json += "\"analogOk\":"; json += (am.analogOk ? "true" : "false"); json += ",";
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

void WebManager::applySettingsRuntimeState() {
  if (!_s) return;

  applyBacklightNow(_s->backlight);

  if (_history) _history->settingsChanged();

  setenv("TZ", _s->tz, 1);
  tzset();
  configTzTime(_s->tz, _s->ntpServer);
  sntp_set_sync_interval(_s->ntpSyncIntervalMs);
  sntp_restart();
  WiFi.setHostname(_s->hostname);
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
  int arm = jsonInt(body, "audioResponseMode", (int)_s->audioResponseMode);
  int whs = jsonInt(body, "warningHoldSec", (int)(_s->orangeAlertHoldMs / 1000UL));
  int chs = jsonInt(body, "criticalHoldSec", (int)(_s->redAlertHoldMs / 1000UL));
  int ccs = jsonInt(body, "calibrationCaptureSec", (int)(_s->calibrationCaptureMs / 1000UL));

  if (bl < 0) bl = 0;
  if (bl > 100) bl = 100;
  if (g < 0) g = 0;
  if (g > 100) g = 100;
  if (o < g) o = g;
  if (o > 100) o = 100;
  if (hm < 1) hm = 1;
  if (hm > 60) hm = 60;
  if (arm < 0) arm = 0;
  if (arm > 1) arm = 1;
  if (whs < 0) whs = 0;
  if (whs > 60) whs = 60;
  if (chs < 0) chs = 0;
  if (chs > 60) chs = 60;
  if (ccs < 1) ccs = 1;
  if (ccs > 30) ccs = 30;

  _s->backlight = (uint8_t)bl;
  _s->th.greenMax = (uint8_t)g;
  _s->th.orangeMax = (uint8_t)o;
  _s->historyMinutes = (uint8_t)hm;
  _s->audioResponseMode = (uint8_t)arm;
  _s->orangeAlertHoldMs = (uint32_t)whs * 1000UL;
  _s->redAlertHoldMs = (uint32_t)chs * 1000UL;
  _s->calibrationCaptureMs = (uint32_t)ccs * 1000UL;
  if (_history) _history->settingsChanged();

  _store->save(*_s);
  applyBacklightNow(_s->backlight);

  Serial0.printf("[WEB] UI saved: backlight=%d green=%d orange=%d hist=%d mode=%s warn=%ds crit=%ds cal=%ds\n",
                 bl, g, o, hm, AudioEngine::responseModeLabel(_s->audioResponseMode), whs, chs, ccs);
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
  applySettingsRuntimeState();

  Serial0.printf("[WEB] TIME saved: tz='%s' ntp='%s' interval=%lu ms hostname='%s'\n",
                 _s->tz, _s->ntpServer, (unsigned long)_s->ntpSyncIntervalMs, _s->hostname);

  replyJson(200, "{\"ok\":true}\n");
}

void WebManager::handleConfigExport() {
  if (!_store || !_s) {
    replyJson(500, "{\"ok\":false,\"error\":\"store/settings missing\"}");
    return;
  }

  replyJson(200, _store->exportJson(*_s));
}

void WebManager::handleConfigImport() {
  if (!_store || !_s) {
    replyJson(500, "{\"ok\":false,\"error\":\"store/settings missing\"}");
    return;
  }

  String err;
  String body = _srv.arg("plain");
  if (!_store->importJson(*_s, body, &err)) {
    String json = "{\"ok\":false,\"error\":\"" + err + "\"}";
    replyJson(400, json);
    return;
  }

  _store->save(*_s);
  applySettingsRuntimeState();
  pushLiveMetrics(true);
  replyJson(200, "{\"ok\":true,\"rebootRecommended\":true}");
}

void WebManager::handleConfigBackup() {
  if (!_store || !_s) {
    replyJson(500, "{\"ok\":false,\"error\":\"store/settings missing\"}");
    return;
  }

  if (!_store->saveBackup(*_s)) {
    replyJson(500, "{\"ok\":false,\"error\":\"backup failed\"}");
    return;
  }

  replyJson(200, "{\"ok\":true}");
}

void WebManager::handleConfigRestore() {
  if (!_store || !_s) {
    replyJson(500, "{\"ok\":false,\"error\":\"store/settings missing\"}");
    return;
  }

  String err;
  if (!_store->restoreBackup(*_s, &err)) {
    String json = "{\"ok\":false,\"error\":\"" + err + "\"}";
    replyJson(400, json);
    return;
  }

  _store->save(*_s);
  applySettingsRuntimeState();
  pushLiveMetrics(true);
  replyJson(200, "{\"ok\":true,\"rebootRecommended\":true}");
}

void WebManager::handleConfigResetPartial() {
  if (!_store || !_s) {
    replyJson(500, "{\"ok\":false,\"error\":\"store/settings missing\"}");
    return;
  }

  String body = _srv.arg("plain");
  String scope = jsonStr(body, "scope", "");
  scope.trim();
  scope.toLowerCase();

  String err;
  if (!_store->resetSection(*_s, scope, &err)) {
    String json = "{\"ok\":false,\"error\":\"" + err + "\"}";
    replyJson(400, json);
    return;
  }

  _store->save(*_s);
  applySettingsRuntimeState();
  pushLiveMetrics(true);
  replyJson(200, "{\"ok\":true,\"rebootRecommended\":true}");
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
  json += "\"hostname\":\""; json += String(_s->otaHostname); json += "\",";
  json += "\"passwordConfigured\":"; json += (strlen(_s->otaPassword) ? "true" : "false");
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
  json += "\"retain\":"; json += (_s->mqttRetain ? "true" : "false"); json += ",";
  json += "\"passwordConfigured\":"; json += (strlen(_s->mqttPassword) ? "true" : "false");
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
      --bg:#0B0F14; --screenRed:#22080B; --panel:#111824; --panel2:#0F1722; --panel3:#0E141C;
      --txt:#DFE7EF; --muted:#8EA1B3; --line:#1E2A38;
      --green:#23C552; --orange:#F0A202; --red:#E53935; --accent:#7A1E2C;
      --radius:22px;
      font-family: system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif;
    }
    *{box-sizing:border-box}
    body{margin:0;background:var(--bg);color:var(--txt);transition:background .2s ease}
    .shell{max-width:1120px;margin:0 auto;padding:0 16px 24px}
    .top{
      position:sticky;top:0;z-index:20;
      background:rgba(15,23,34,.92);backdrop-filter:blur(14px);
      border-bottom:1px solid var(--line);
    }
    .topInner{
      max-width:1120px;margin:0 auto;padding:14px 16px 12px;
      display:grid;grid-template-columns:auto 1fr;gap:14px;align-items:center;
    }
    .brand{display:flex;flex-direction:column;gap:3px}
    .title{font-size:22px;font-weight:700;letter-spacing:.2px}
    .subtitle{font-size:12px;color:var(--muted)}
    .tabs{display:flex;gap:8px;justify-content:center;flex-wrap:wrap}
    .tab{
      appearance:none;border:0;cursor:pointer;
      padding:10px 14px;border-radius:12px;
      background:var(--panel3);color:var(--muted);font-weight:700;font-size:13px;
      transition:background .15s ease,color .15s ease,transform .15s ease;
    }
    .tab.active{background:var(--accent);color:#fff}
    .tab:active{transform:translateY(1px)}
    .pages{padding-top:18px}
    .page{display:none}
    .page.active{display:block}
    .stack{display:flex;flex-direction:column;gap:14px}
    .grid2{display:grid;grid-template-columns:1fr 1fr;gap:14px}
    .gridOverview{
      display:grid;grid-template-columns:245px minmax(280px,1fr) 245px;gap:14px;align-items:stretch;
    }
    .rightCol{display:grid;grid-template-rows:1fr 1fr;gap:14px}
    .card{
      position:relative;
      background:var(--panel);border:1px solid var(--line);border-radius:var(--radius);padding:16px;
      box-shadow:0 12px 34px rgba(0,0,0,.24);
    }
    .card.clockCard{background:#101A28}
    .card.soundCard.alertOrange{
      background:#3D2810;border-color:#F0A202;box-shadow:0 0 0 2px rgba(240,162,2,.18), 0 12px 34px rgba(0,0,0,.24);
    }
    .card.soundCard.alertRed{
      background:#4A161B;border-color:#FF5A5F;box-shadow:0 0 0 3px rgba(255,90,95,.28), 0 12px 34px rgba(0,0,0,.3);
    }
    .sectionTitle{font-size:24px;font-weight:800;margin:0}
    .k{font-size:14px;color:var(--muted)}
    .v{font-size:30px;font-weight:800;margin-top:10px}
    .metricCard{
      background:var(--panel3);border:1px solid rgba(255,255,255,.03);
      border-radius:22px;padding:18px;
    }
    #soundLeq,#soundPeak{
      display:block;width:100%;text-align:center;
    }
    .clockMiniDate,.clockDate{color:var(--muted);font-variant-numeric:tabular-nums}
    .clockMiniDate{font-size:16px;text-align:center}
    .clockMiniMain{
      font-size:56px;font-weight:800;letter-spacing:-2px;text-align:center;
      margin-top:34px;font-variant-numeric:tabular-nums;
    }
    .clockMiniSec{
      width:78px;height:34px;border-radius:12px;background:var(--accent);color:#fff;
      display:grid;place-items:center;margin:30px 0 0 auto;font-weight:800;font-variant-numeric:tabular-nums;
    }
    .clockHero{
      min-height:362px;padding:28px;background:#101A28;
      display:flex;flex-direction:column;justify-content:center;overflow:hidden;
    }
    .clockRule{height:2px;background:#243244;border-radius:99px;margin-top:12px}
    .clockHeroDate{position:absolute;top:24px;right:28px;font-size:24px}
    .clockHeroRow{
      display:flex;align-items:center;justify-content:center;gap:12px;
      margin-top:18px;
    }
    .clockHeroMain{
      font-size:clamp(72px,14vw,170px);font-weight:800;line-height:.9;letter-spacing:-5px;
      font-variant-numeric:tabular-nums;
    }
    .clockHeroSec{
      width:min(29vw,262px);height:min(17vw,156px);border-radius:24px;background:var(--accent);
      display:grid;place-items:center;font-size:clamp(42px,9vw,120px);font-weight:800;
      font-variant-numeric:tabular-nums;line-height:1;color:#fff;flex:0 0 auto;
    }
    .gaugeWrap{
      display:flex;flex-direction:column;align-items:center;justify-content:center;gap:10px;
      height:100%;
    }
    .gauge{
      width:min(72vw,240px);height:min(72vw,240px);border-radius:50%;
      display:grid;place-items:center;position:relative;flex:0 0 auto;
      background:
        radial-gradient(circle at center, #0B0F14 0 57%, transparent 58%),
        conic-gradient(var(--gaugeColor, var(--green)) calc(var(--pct)*1%), #1A2332 0);
      transform:rotate(135deg);
    }
    .gauge.large{width:min(58vw,240px);height:min(58vw,240px)}
    .gauge::before{
      content:"";position:absolute;inset:18px;border-radius:50%;
      background:radial-gradient(circle at center, #0B0F14 0 61%, transparent 62%);
    }
    .gaugeInner{
      position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center;
      transform:rotate(-135deg);z-index:2;
    }
    .db{font-size:clamp(52px,10vw,88px);font-weight:800;line-height:1;color:var(--txt);}
    .db.large{font-size:56px}
    .unit{font-size:clamp(18px,3vw,24px);color:var(--muted);margin-top:4px;}
    .dot{width:14px;height:14px;border-radius:50%;background:var(--gaugeColor, var(--green));margin-top:14px;}
    .soundHero{
      display:grid;grid-template-columns:280px 1fr;gap:20px;align-items:center;min-height:240px;
    }
    .soundMetrics{display:grid;grid-template-columns:1fr 1fr;gap:14px}
    .alertBadge{
      position:absolute;top:18px;left:258px;
      min-width:112px;height:34px;padding:0 16px;border-radius:17px;background:var(--red);
      display:none;align-items:center;justify-content:center;color:#fff;font-size:14px;font-weight:800;
    }
    .alertBadge.show{display:flex}
    .histCard{overflow:hidden}
    .histTop{
      display:grid;grid-template-columns:1fr auto 1fr;align-items:center;gap:12px;margin-bottom:10px;
    }
    .histMeta{font-size:12px;color:var(--muted)}
    .histMeta.center{text-align:center}
    .histMeta.right{text-align:right}
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
      min-width:0;
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
      width:100%;height:100%;display:flex;align-items:flex-end;gap:2px;min-width:0;
    }
    .histBar{
      flex:1 1 0;min-width:0;height:4px;border-radius:3px 3px 0 0;
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
    .settingsPage{display:grid;grid-template-columns:minmax(0,1.35fr) 360px;gap:14px}
    .settingsMain,.settingsRail{display:flex;flex-direction:column;gap:14px}
    .settingsHero{
      background:
        radial-gradient(circle at top right, rgba(122,30,44,.34), transparent 34%),
        linear-gradient(135deg, #131d2b 0%, #111824 58%, #0f1621 100%);
    }
    .sectionHead{display:flex;justify-content:space-between;align-items:flex-start;gap:14px;flex-wrap:wrap}
    .sectionKicker{
      font-size:11px;font-weight:800;letter-spacing:.14em;text-transform:uppercase;color:#93a4b6;
    }
    .sectionLead{font-size:13px;color:var(--muted);max-width:52ch;line-height:1.5}
    .sectionMeta{
      display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;
      background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.05);font-size:12px;color:#d8e0e8;
    }
    .settingsCardSoft{background:linear-gradient(180deg, #111824 0%, #0f1621 100%)}
    .settingsCardFlat{background:#101722}
    .field{display:flex;flex-direction:column;gap:6px}
    .field + .field{margin-top:12px}
    label{font-size:12px;color:var(--muted)}
    input[type="range"]{width:100%}
    .switchRow{display:flex;align-items:center;justify-content:space-between;gap:14px}
    .switchText{display:flex;flex-direction:column;gap:4px}
    .switchState{font-size:12px;color:#d8e0e8}
    .switch{
      position:relative;display:inline-block;width:58px;height:32px;flex:0 0 auto;
      padding:0;border:1px solid var(--line);border-radius:999px;background:#16202E;cursor:pointer;
      transition:.18s ease;
    }
    .switchTrack{
      position:absolute;inset:0;border-radius:999px;transition:.18s ease;
    }
    .switchTrack::after{
      content:"";position:absolute;left:3px;top:3px;width:24px;height:24px;border-radius:50%;
      background:#d8e0e8;transition:.18s ease;
    }
    .switch.active{background:var(--accent);border-color:transparent}
    .switch.active .switchTrack::after{transform:translateX(26px);background:#fff}
    input[type="number"], input[type="text"], input[type="file"], select, textarea{
      width:100%;border:1px solid var(--line);background:var(--panel3);color:var(--txt);
      border-radius:12px;padding:10px 12px;outline:none;font-weight:700;
    }
    textarea{min-height:220px;resize:vertical;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px}
    .btnRow{display:flex;gap:10px;flex-wrap:wrap;margin-top:12px}
    .btn{
      appearance:none;border:1px solid var(--line);background:#172133;color:var(--txt);
      padding:10px 14px;border-radius:12px;font-weight:800;cursor:pointer;
    }
    .btn.accent{background:var(--accent);border-color:transparent}
    .btn.danger{background:#341419;border-color:#4A161B}
    .btn.ghost{background:transparent}
    .choiceRow{display:flex;gap:10px;flex-wrap:wrap}
    .choice{flex:1 1 120px}
    .choice.active{background:var(--accent);color:#fff;border-color:transparent}
    .statusList{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:4px}
    .statusItem{
      background:var(--panel3);border:1px solid rgba(255,255,255,.03);border-radius:16px;padding:14px;
    }
    .statusItem .v{
      font-size:18px;
      line-height:1.2;
      margin-top:8px;
      font-weight:700;
      letter-spacing:-.01em;
    }
    .healthBadge{
      display:inline-flex;align-items:center;justify-content:center;
      min-height:32px;padding:6px 12px;border-radius:999px;
      font-size:12px;font-weight:800;letter-spacing:.02em;
      border:1px solid transparent;
      width:fit-content;margin:10px auto 0;
    }
    .healthBadge.ok{background:rgba(35,197,82,.12);color:#7ee2a0;border-color:rgba(35,197,82,.2)}
    .healthBadge.bad{background:rgba(229,57,53,.12);color:#ff8f8a;border-color:rgba(229,57,53,.2)}
    .statusList.compact .statusItem{padding:12px 13px}
    .settingsSplit{display:grid;grid-template-columns:1fr 1fr;gap:14px}
    .actionsCard .btnRow{margin-top:10px}
    .hint,.toast,.footerLine{font-size:12px;color:var(--muted);line-height:1.45}
    .calHeader{
      display:flex;justify-content:space-between;align-items:flex-start;gap:14px;flex-wrap:wrap;
    }
    .calRows{display:flex;flex-direction:column;gap:10px;margin-top:16px}
    .calRow{
      display:grid;grid-template-columns:minmax(0,1fr) auto auto auto auto;gap:10px;align-items:center;
      background:var(--panel3);border-radius:18px;padding:12px 14px;
    }
    .calTitle{font-size:15px;font-weight:800}
    .badge{
      min-width:74px;height:34px;border-radius:12px;background:#16202E;
      display:grid;place-items:center;font-size:14px;font-weight:800;
    }
    .mono{font-variant-numeric:tabular-nums}
    .pill{
      display:inline-flex;align-items:center;gap:8px;padding:8px 12px;border-radius:999px;
      border:1px solid var(--line);background:var(--panel3);font-size:12px;color:var(--muted);
    }
    @media (max-width:1024px){
      .gridOverview{grid-template-columns:1fr}
      .rightCol{grid-template-columns:1fr 1fr;grid-template-rows:none}
      .settingsPage{grid-template-columns:1fr}
    }
    @media (max-width:760px){
      .topInner{grid-template-columns:1fr;justify-items:start}
      .tabs{justify-content:flex-start}
      .soundHero{grid-template-columns:1fr}
      .soundMetrics{grid-template-columns:1fr}
      .statusList{grid-template-columns:1fr}
      .settingsSplit{grid-template-columns:1fr}
      .grid2{grid-template-columns:1fr}
      .calRow{grid-template-columns:1fr 34px 74px 34px 110px}
      .clockHeroDate{position:static;margin-top:6px}
      .clockHeroRow{justify-content:flex-start;flex-wrap:wrap}
      .alertBadge{left:auto;right:18px;top:18px}
    }
    @media (max-width:700px){
      .title{font-size:18px}
      .chartWrap{height:180px}
      .histScale{width:52px;flex-basis:52px}
      .histBars{gap:1px}
    }
  </style>
</head>
<body>
  <div class="top">
    <div class="topInner">
      <div class="brand">
        <div class="title">SoundPanel 7</div>
        <div class="subtitle">Sonometre & Horloge connecte</div>
      </div>
      <div class="tabs">
        <button class="tab active" data-page="overview">Principal</button>
        <button class="tab" data-page="clock">Horloge</button>
        <button class="tab" data-page="sound">Sonometre</button>
        <button class="tab" data-page="calibration">Calibration</button>
        <button class="tab" data-page="settings">Parametres</button>
      </div>
    </div>
  </div>

  <main class="shell">
    <div class="pages">
      <section class="page active" data-page="overview">
        <div class="stack">
          <div class="gridOverview">
            <article class="card clockCard">
              <div class="clockMiniDate mono" id="overviewClockDate">--/--/----</div>
              <div class="clockMiniMain mono" id="overviewClockMain">--:--</div>
              <div class="clockMiniSec mono" id="overviewClockSec">:--</div>
            </article>

            <article class="card soundCard" id="overviewSoundCard">
              <div class="gaugeWrap">
                <div class="gauge large" id="overviewGauge" style="--pct:0;--gaugeColor:var(--green);">
                  <div class="gaugeInner">
                    <div class="db large mono" id="overviewDb">--.-</div>
                    <div class="unit">dB</div>
                    <div class="dot" id="overviewDot"></div>
                  </div>
                </div>
              </div>
            </article>

            <div class="rightCol">
              <article class="card">
                <div class="k">Leq</div>
                <div class="v mono" id="overviewLeq">--.-</div>
              </article>
              <article class="card">
                <div class="k">Peak</div>
                <div class="v mono" id="overviewPeak">--.-</div>
              </article>
            </div>
          </div>

          <article class="card histCard">
            <div class="histTop">
              <div class="histMeta" id="overviewHistMeta">Historique 5 min</div>
              <div class="histMeta center" id="overviewAlertTime">Rouge 0 s / 5m</div>
              <div class="histMeta right"></div>
            </div>
            <div class="chartWrap">
              <div class="chartPlot">
                <div class="histBars" id="overviewHistBars"></div>
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
              <div id="overviewHistLeft">-5m</div>
              <div class="histAxisMid" id="overviewHistMid">-2m</div>
              <div id="overviewHistRight">0</div>
            </div>
          </article>
        </div>
      </section>

      <section class="page" data-page="clock">
        <article class="card clockHero">
          <div class="clockRule"></div>
          <div class="clockDate clockHeroDate mono" id="clockDate">--/--/----</div>
          <div class="clockHeroRow">
            <div class="clockHeroMain mono" id="clockMain">--:--</div>
            <div class="clockHeroSec mono" id="clockSec">--</div>
          </div>
        </article>
      </section>

      <section class="page" data-page="sound">
        <div class="stack">
          <article class="card soundCard" id="soundCard">
            <div class="alertBadge" id="soundAlertBadge">ALERTE</div>
            <div class="soundHero">
              <div class="gaugeWrap">
                <div class="gauge large" id="soundGauge" style="--pct:0;--gaugeColor:var(--green);">
                  <div class="gaugeInner">
                    <div class="db large mono" id="soundDb">--.-</div>
                    <div class="unit">dB</div>
                    <div class="dot" id="soundDot"></div>
                  </div>
                </div>
              </div>
              <div class="soundMetrics">
                <article class="metricCard">
                  <div class="k">Leq</div>
                  <div class="v mono" id="soundLeq">--.-</div>
                </article>
                <article class="metricCard">
                  <div class="k">Peak</div>
                  <div class="v mono" id="soundPeak">--.-</div>
                </article>
              </div>
            </div>
          </article>

          <article class="card histCard">
            <div class="histTop">
              <div class="histMeta" id="soundHistMeta">Historique 5 min</div>
              <div class="histMeta center" id="soundAlertTime">Rouge 0 s / 5m</div>
              <div class="histMeta right"></div>
            </div>
            <div class="chartWrap">
              <div class="chartPlot">
                <div class="histBars" id="soundHistBars"></div>
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
              <div id="soundHistLeft">-5m</div>
              <div class="histAxisMid" id="soundHistMid">-2m</div>
              <div id="soundHistRight">0</div>
            </div>
          </article>
        </div>
      </section>

      <section class="page" data-page="calibration">
        <article class="card">
          <div class="calHeader">
            <div>
              <h2 class="sectionTitle">Calibration</h2>
              <div class="hint" id="calLiveMic">Micro live: --</div>
              <div class="hint" id="calLiveLog">Log calibration live: --</div>
            </div>
            <div class="pill mono" id="calStatus">0 / 3 points valides</div>
          </div>

          <div class="calRows">
            <div class="calRow">
              <div>
                <div class="calTitle">Point 1</div>
                <div class="hint mono" id="calState1">Point 1 non capture</div>
              </div>
              <button class="btn" data-cal-adjust="0:-5">-</button>
              <div class="badge mono" id="calRef1">40</div>
              <button class="btn" data-cal-adjust="0:5">+</button>
              <button class="btn accent" data-cal-capture="0">Capturer</button>
            </div>

            <div class="calRow">
              <div>
                <div class="calTitle">Point 2</div>
                <div class="hint mono" id="calState2">Point 2 non capture</div>
              </div>
              <button class="btn" data-cal-adjust="1:-5">-</button>
              <div class="badge mono" id="calRef2">65</div>
              <button class="btn" data-cal-adjust="1:5">+</button>
              <button class="btn accent" data-cal-capture="1">Capturer</button>
            </div>

            <div class="calRow">
              <div>
                <div class="calTitle">Point 3</div>
                <div class="hint mono" id="calState3">Point 3 non capture</div>
              </div>
              <button class="btn" data-cal-adjust="2:-5">-</button>
              <div class="badge mono" id="calRef3">85</div>
              <button class="btn" data-cal-adjust="2:5">+</button>
              <button class="btn accent" data-cal-capture="2">Capturer</button>
            </div>
          </div>

          <div class="btnRow">
            <button class="btn danger" id="clearCalibration">Effacer calibration</button>
          </div>
          <div class="toast" id="calToast"></div>
        </article>
      </section>

      <section class="page" data-page="settings">
        <div class="settingsPage">
          <div class="settingsMain">
            <article class="card settingsHero">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Vue Systeme</div>
                  <h2 class="sectionTitle">Parametres & supervision</h2>
                  <div class="sectionLead">Les reglages frequents restent au centre. La connectivite, l'OTA et le diagnostic sont ranges a droite pour garder une lecture claire.</div>
                </div>
              </div>
              <div class="statusList compact" style="margin-top:16px">
                <div class="statusItem">
                  <div class="k">Etat systeme</div>
                  <div class="healthBadge bad" id="settingsSystemBadge">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">IP / RSSI</div>
                  <div class="v mono" id="settingsIp">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">Heure</div>
                  <div class="v mono" id="settingsTime">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">Uptime</div>
                  <div class="v mono" id="settingsUptime">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">Historique</div>
                  <div class="v mono" id="settingsHistory">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">Version</div>
                  <div class="v mono" id="settingsVersion">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">Date de build</div>
                  <div class="v mono" id="settingsBuildDate">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">Env compile</div>
                  <div class="v mono" id="settingsBuildEnv">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">Temp MCU</div>
                  <div class="v mono" id="settingsMcuTemp">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">Etat OTA / MQTT</div>
                  <div class="v mono" id="settingsOtaMqtt">--</div>
                </div>
              </div>
            </article>

            <article class="card settingsCardSoft">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Reglages Rapides</div>
                  <h2 class="sectionTitle">Interface & seuils</h2>
                </div>
              </div>

              <div class="field">
                <div class="switchRow">
                  <div class="switchText">
                    <label>Backlight</label>
                    <div class="switchState mono" id="blVal">--</div>
                  </div>
                  <button class="switch" id="bl" type="button" aria-label="Activer ou couper le backlight" aria-pressed="true">
                    <span class="switchTrack"></span>
                  </button>
                </div>
              </div>

              <div class="field">
                <label>Seuil vert max <span class="mono" id="gVal">--</span>dB</label>
                <input id="g" type="range" min="0" max="100" value="55"/>
              </div>

              <div class="field">
                <label>Seuil orange max <span class="mono" id="oVal">--</span>dB</label>
                <input id="o" type="range" min="0" max="100" value="70"/>
              </div>

              <div class="field">
                <label>Historique <span class="mono" id="hVal">--</span> min</label>
                <input id="hist" type="range" min="1" max="60" value="5"/>
              </div>

              <div class="settingsSplit">
                <div class="field">
                  <label>Reponse sonometre</label>
                  <div class="choiceRow">
                    <button class="btn choice" id="modeFast" data-mode="0">Fast</button>
                    <button class="btn choice" id="modeSlow" data-mode="1">Slow</button>
                  </div>
                  <div class="hint">Fast reste reactif. Slow stabilise l'affichage.</div>
                </div>

                <div>
                  <div class="field">
                    <label>Delai warning (orange)</label>
                    <input id="warnHoldSec" type="number" min="0" max="60" step="1" value="3"/>
                  </div>
                  <div class="field">
                    <label>Delai critique (rouge)</label>
                    <input id="critHoldSec" type="number" min="0" max="60" step="1" value="2"/>
                  </div>
                  <div class="field">
                    <label>Delai tampon calibration (s)</label>
                    <input id="calCaptureSec" type="number" min="1" max="30" step="1" value="3"/>
                    <div class="hint">Duree de capture utilisee pour chaque point de calibration.</div>
                  </div>
                </div>
              </div>

              <div class="btnRow">
                <button class="btn accent" id="saveUi">Sauver UI</button>
              </div>
              <div class="toast" id="uiToast"></div>
            </article>

            <article class="card settingsCardFlat">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Temps Reseau</div>
                  <h2 class="sectionTitle">Heure, NTP & timezone</h2>
                </div>
              </div>
              <div class="settingsSplit">
                <div>
                  <div class="field">
                    <label>NTP Server</label>
                    <input id="ntpServerAdv" type="text" placeholder="fr.pool.ntp.org"/>
                  </div>
                  <div class="field">
                    <label>Sync NTP (minutes)</label>
                    <input id="ntpSyncMinAdv" type="number" min="1" max="1440" step="1" value="180"/>
                  </div>
                </div>
                <div>
                  <div class="field">
                    <label>TZ (POSIX)</label>
                    <input id="tzAdv" type="text" placeholder="CET-1CEST,M3.5.0/2,M10.5.0/3"/>
                  </div>
                  <div class="field">
                    <label>Hostname</label>
                    <input id="hostnameAdv" type="text" placeholder="soundpanel7"/>
                  </div>
                </div>
              </div>
              <div class="btnRow">
                <button class="btn accent" id="saveTimeAdv">Sauver heure</button>
              </div>
              <div class="toast" id="toastTimeAdv"></div>
            </article>

            <article class="card settingsCardFlat">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Publication</div>
                  <h2 class="sectionTitle">MQTT</h2>
                </div>
              </div>
              <div class="settingsSplit">
                <div>
                  <div class="field">
                    <label>Activer MQTT</label>
                    <select id="mqttEnabledAdv">
                      <option value="1">Oui</option>
                      <option value="0">Non</option>
                    </select>
                  </div>
                  <div class="field">
                    <label>Broker / Host</label>
                    <input id="mqttHostAdv" type="text" placeholder="192.168.1.10"/>
                  </div>
                  <div class="field">
                    <label>Port</label>
                    <input id="mqttPortAdv" type="number" min="1" max="65535" placeholder="1883"/>
                  </div>
                  <div class="field">
                    <label>Username</label>
                    <input id="mqttUsernameAdv" type="text" placeholder="laisser vide si non utilise"/>
                  </div>
                  <div class="field">
                    <label>Password</label>
                    <input id="mqttPasswordAdv" type="password" placeholder="laisser vide si non utilise"/>
                  </div>
                </div>
                <div>
                  <div class="field">
                    <label>Client ID</label>
                    <input id="mqttClientIdAdv" type="text" placeholder="soundpanel7"/>
                  </div>
                  <div class="field">
                    <label>Base topic</label>
                    <input id="mqttBaseTopicAdv" type="text" placeholder="soundpanel7"/>
                  </div>
                  <div class="field">
                    <label>Publish period (ms)</label>
                    <input id="mqttPublishPeriodMsAdv" type="number" min="250" max="60000" placeholder="1000"/>
                  </div>
                  <div class="field">
                    <label>Retain</label>
                    <select id="mqttRetainAdv">
                      <option value="0">Non</option>
                      <option value="1">Oui</option>
                    </select>
                  </div>
                </div>
              </div>
              <div class="btnRow">
                <button class="btn accent" id="saveMqttAdv">Sauver MQTT</button>
              </div>
              <div class="toast" id="toastMqttAdv"></div>
            </article>
          </div>

          <div class="settingsRail">
            <article class="card settingsCardSoft">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Sauvegarde</div>
                  <h2 class="sectionTitle">Configuration JSON</h2>
                </div>
              </div>
              <div class="field">
                <label>JSON config</label>
                <textarea id="configJsonBox" placeholder='{"type":"soundpanel7-config", ...}'></textarea>
              </div>
              <div class="field">
                <label>Importer depuis un fichier JSON</label>
                <input id="configFile" type="file" accept=".json,application/json"/>
              </div>
              <div class="hint">L'export inclut aussi les mots de passe OTA et MQTT.</div>
              <div class="btnRow">
                <button class="btn" id="exportConfigBtn">Exporter JSON</button>
                <button class="btn accent" id="importConfigBtn">Importer JSON</button>
              </div>
              <div class="field">
                <label>Reset partiel</label>
                <select id="configResetScope">
                  <option value="ui">UI</option>
                  <option value="time">Heure / hostname</option>
                  <option value="audio">Audio</option>
                  <option value="calibration">Calibration</option>
                  <option value="ota">OTA</option>
                  <option value="mqtt">MQTT</option>
                </select>
              </div>
              <div class="btnRow">
                <button class="btn" id="backupConfigBtn">Backup usine</button>
                <button class="btn" id="restoreConfigBtn">Restore</button>
                <button class="btn danger" id="partialResetBtn">Reset partiel</button>
              </div>
              <div class="toast" id="configToast"></div>
            </article>

            <article class="card actionsCard settingsCardSoft">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Maintenance</div>
                  <h2 class="sectionTitle">Actions systeme</h2>
                </div>
              </div>
              <div class="btnRow">
                <button class="btn" id="rebootBtn">Reboot</button>
                <button class="btn danger" id="factoryResetBtn">Factory reset</button>
              </div>
              <div class="toast" id="actionsToast"></div>
            </article>

            <article class="card settingsCardFlat">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Mise A Jour</div>
                  <h2 class="sectionTitle">OTA</h2>
                </div>
              </div>
              <div class="field">
                <label>Activer OTA</label>
                <select id="otaEnabledAdv">
                  <option value="1">Oui</option>
                  <option value="0">Non</option>
                </select>
              </div>
              <div class="field">
                <label>Hostname OTA</label>
                <input id="otaHostnameAdv" type="text" placeholder="soundpanel7"/>
              </div>
              <div class="field">
                <label>Port OTA</label>
                <input id="otaPortAdv" type="number" min="1" max="65535" placeholder="3232"/>
              </div>
              <div class="field">
                <label>Mot de passe OTA</label>
                <input id="otaPasswordAdv" type="password" placeholder="laisser vide = aucun mot de passe"/>
              </div>
              <div class="btnRow">
                <button class="btn accent" id="saveOtaAdv">Sauver OTA</button>
              </div>
              <div class="toast" id="toastOtaAdv"></div>
            </article>

            <article class="card settingsCardFlat">
              <div class="sectionHead">
                <div>
                  <div class="sectionKicker">Diagnostic</div>
                  <h2 class="sectionTitle">Audio brut</h2>
                </div>
              </div>
              <div class="statusList compact">
                <div class="statusItem">
                  <div class="k">rawRms</div>
                  <div class="v mono" id="rawRmsAdv">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">pseudo dB</div>
                  <div class="v mono" id="rawPseudoDbAdv">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">ADC Mean</div>
                  <div class="v mono" id="rawAdcMeanAdv">--</div>
                </div>
                <div class="statusItem">
                  <div class="k">ADC Last</div>
                  <div class="v mono" id="rawAdcLastAdv">--</div>
                </div>
              </div>
            </article>
          </div>
        </div>
      </section>

    </div>
  </main>

<script>
  const $ = (id) => document.getElementById(id);

  const state = {
    status: null,
    historyValues: [],
    historyInitialized: false,
    historyMinutes: 5,
    historyCapacity: 96,
    historySamplePeriodMs: 3000,
    lastHistorySampleClientMs: 0,
    clockBaseMs: 0,
    clockSyncClientMs: 0,
    currentPage: "overview",
    events: null,
    reconnectTimer: null,
    hasLiveFeed: false,
    lastContactMs: 0,
    orangeSinceMs: 0,
    redSinceMs: 0,
    uiDirty: false,
    uiResponseMode: 0,
    calRefs: [40, 65, 85],
    calRefsDirty: [false, false, false],
  };

  const gaugeViews = [
    { gauge: $("overviewGauge"), db: $("overviewDb"), dot: $("overviewDot"), card: $("overviewSoundCard") },
    { gauge: $("soundGauge"), db: $("soundDb"), dot: $("soundDot"), card: $("soundCard") },
  ];

  const metricViews = [
    { leq: $("overviewLeq"), peak: $("overviewPeak") },
    { leq: $("soundLeq"), peak: $("soundPeak") },
  ];

  const historyViews = [
    {
      meta: $("overviewHistMeta"),
      alert: $("overviewAlertTime"),
      bars: $("overviewHistBars"),
      left: $("overviewHistLeft"),
      mid: $("overviewHistMid"),
      right: $("overviewHistRight"),
    },
    {
      meta: $("soundHistMeta"),
      alert: $("soundAlertTime"),
      bars: $("soundHistBars"),
      left: $("soundHistLeft"),
      mid: $("soundHistMid"),
      right: $("soundHistRight"),
    },
  ];

  function pad2(v) {
    return String(v).padStart(2, "0");
  }

  function formatUptime(totalSeconds) {
    const uptime = Math.max(0, Math.floor(Number(totalSeconds) || 0));
    const days = Math.floor(uptime / 86400);
    const hours = Math.floor((uptime % 86400) / 3600);
    const minutes = Math.floor((uptime % 3600) / 60);
    const seconds = uptime % 60;

    if (days > 0) {
      return `${days}j ${pad2(hours)}:${pad2(minutes)}:${pad2(seconds)}`;
    }
    return `${pad2(hours)}:${pad2(minutes)}:${pad2(seconds)}`;
  }

  function zoneColor(db, greenMax, orangeMax) {
    if (db <= greenMax) return "#23C552";
    if (db <= orangeMax) return "#F0A202";
    return "#E53935";
  }

  function historyHeightPercent(db) {
    const histDbMin = 35;
    const histDbMax = 130;
    const clamped = Math.max(histDbMin, Math.min(histDbMax, Number(db) || 0));
    return ((clamped - histDbMin) / (histDbMax - histDbMin)) * 100;
  }

  function syncClock(serverTime) {
    if (!serverTime) {
      state.clockBaseMs = 0;
      state.clockSyncClientMs = 0;
      renderClock();
      return;
    }

    const parsed = Date.parse(serverTime.replace(" ", "T"));
    if (Number.isNaN(parsed)) {
      state.clockBaseMs = 0;
      state.clockSyncClientMs = 0;
      renderClock();
      return;
    }

    state.clockBaseMs = parsed;
    state.clockSyncClientMs = Date.now();
    renderClock();
  }

  function renderClock() {
    if (!state.clockBaseMs) {
      $("overviewClockDate").textContent = "--/--/----";
      $("overviewClockMain").textContent = "--:--";
      $("overviewClockSec").textContent = ":--";
      $("clockDate").textContent = "--/--/----";
      $("clockMain").textContent = "--:--";
      $("clockSec").textContent = "--";
      $("settingsTime").textContent = "NTP...";
      return;
    }

    const nowMs = state.clockBaseMs + (Date.now() - state.clockSyncClientMs);
    const d = new Date(nowMs);
    const date = `${pad2(d.getDate())}/${pad2(d.getMonth() + 1)}/${d.getFullYear()}`;
    const hhmm = `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
    const sec = pad2(d.getSeconds());
    const hhmmss = `${hhmm}:${sec}`;

    $("overviewClockDate").textContent = date;
    $("overviewClockMain").textContent = hhmm;
    $("overviewClockSec").textContent = `:${sec}`;
    $("clockDate").textContent = date;
    $("clockMain").textContent = hhmm;
    $("clockSec").textContent = sec;
    $("settingsTime").textContent = hhmmss;
  }

  function setActivePage(page) {
    state.currentPage = page;
    document.querySelectorAll(".tab").forEach((el) => {
      el.classList.toggle("active", el.dataset.page === page);
    });
    document.querySelectorAll(".page").forEach((el) => {
      el.classList.toggle("active", el.dataset.page === page);
    });
  }

  function formatRedSeconds(redSeconds, historyMinutes) {
    if (redSeconds >= 60) {
      return `Rouge ${Math.floor(redSeconds / 60)}m${pad2(redSeconds % 60)}s / ${historyMinutes}m`;
    }
    return `Rouge ${redSeconds}s / ${historyMinutes}m`;
  }

  function updateHistoryLabels() {
    const mid = Math.max(1, Math.floor(state.historyMinutes / 2));
    historyViews.forEach((view) => {
      view.meta.textContent = `Historique ${state.historyMinutes} min`;
      view.left.textContent = `-${state.historyMinutes}m`;
      view.mid.textContent = `-${mid}m`;
      view.right.textContent = "0";
    });
  }

  function drawHistory() {
    const values = state.historyValues.slice(-state.historyCapacity);
    const greenMax = Number(state.status?.greenMax ?? 55);
    const orangeMax = Number(state.status?.orangeMax ?? 70);
    const renderCapacity = getHistoryRenderCapacity();
    const renderedValues = compressHistory(values, renderCapacity);
    const missing = Math.max(0, renderCapacity - renderedValues.length);

    let html = "";
    for (let i = 0; i < missing; i++) html += '<div class="histBar"></div>';
    for (const value of renderedValues) {
      html += `<div class="histBar" style="height:${historyHeightPercent(value).toFixed(1)}%;background:${zoneColor(value, greenMax, orangeMax)}"></div>`;
    }

    const redSamples = values.filter((value) => value > orangeMax).length;
    const redSeconds = Math.round((redSamples * state.historySamplePeriodMs) / 1000);
    const alertText = formatRedSeconds(redSeconds, state.historyMinutes);

    historyViews.forEach((view) => {
      view.bars.innerHTML = html;
      view.alert.textContent = alertText;
    });
  }

  function getHistoryRenderCapacity() {
    const widths = historyViews
      .map((view) => view.bars && view.bars.parentElement ? view.bars.parentElement.clientWidth : 0)
      .filter((width) => width > 0);

    if (!widths.length) return state.historyCapacity;

    const minWidth = Math.min(...widths);
    const slotPx = window.innerWidth <= 700 ? 3 : 5;
    const capacity = Math.floor(minWidth / slotPx);
    return Math.max(24, Math.min(state.historyCapacity, capacity));
  }

  function compressHistory(values, targetCount) {
    if (!Array.isArray(values) || values.length <= targetCount) return values;

    const out = [];
    const bucketSize = values.length / targetCount;

    for (let i = 0; i < targetCount; i++) {
      const start = Math.floor(i * bucketSize);
      const end = Math.max(start + 1, Math.floor((i + 1) * bucketSize));
      let bucketMax = Number(values[start] ?? 0);

      for (let j = start + 1; j < end && j < values.length; j++) {
        const value = Number(values[j] ?? 0);
        if (value > bucketMax) bucketMax = value;
      }

      out.push(bucketMax);
    }

    return out;
  }

  function setHistory(values) {
    state.historyValues = Array.isArray(values) ? values.map((value) => Number(value) || 0) : [];
    if (state.historyValues.length > state.historyCapacity) {
      state.historyValues = state.historyValues.slice(-state.historyCapacity);
    }
    state.historyInitialized = true;
    state.lastHistorySampleClientMs = Date.now();
    updateHistoryLabels();
    drawHistory();
  }

  function appendHistory(db, force = false) {
    const now = Date.now();
    if (!force && state.lastHistorySampleClientMs && (now - state.lastHistorySampleClientMs) < state.historySamplePeriodMs) {
      return;
    }

    state.lastHistorySampleClientMs = now;
    state.historyValues.push(Number(db) || 0);
    if (state.historyValues.length > state.historyCapacity) {
      state.historyValues = state.historyValues.slice(-state.historyCapacity);
    }
    drawHistory();
  }

  function updateAlertState(db, greenMax, orangeMax, warningHoldSec, criticalHoldSec) {
    const now = Date.now();
    const orange = db > greenMax && db <= orangeMax;
    const red = db > orangeMax;

    if (orange) {
      if (!state.orangeSinceMs) state.orangeSinceMs = now;
    } else {
      state.orangeSinceMs = 0;
    }

    if (red) {
      if (!state.redSinceMs) state.redSinceMs = now;
    } else {
      state.redSinceMs = 0;
    }

    const orangeAlert = state.orangeSinceMs && (now - state.orangeSinceMs) >= (warningHoldSec * 1000);
    const redAlert = state.redSinceMs && (now - state.redSinceMs) >= (criticalHoldSec * 1000);

    document.body.style.background = redAlert ? "var(--screenRed)" : "var(--bg)";
    gaugeViews.forEach((view, index) => {
      view.card.classList.toggle("alertOrange", Boolean(orangeAlert && !redAlert));
      view.card.classList.toggle("alertRed", Boolean(redAlert));
      if (index === 1) $("soundAlertBadge").classList.toggle("show", Boolean(redAlert));
    });
  }

  function updateGauge(db, greenMax, orangeMax) {
    const pct = Math.max(0, Math.min(100, db));
    const color = zoneColor(db, greenMax, orangeMax);

    gaugeViews.forEach((view) => {
      view.gauge.style.setProperty("--pct", pct);
      view.gauge.style.setProperty("--gaugeColor", color);
      view.dot.style.background = color;
      view.db.textContent = db.toFixed(1);
    });
  }

  function updateMetrics(leq, peak) {
    metricViews.forEach((view) => {
      view.leq.textContent = leq.toFixed(1);
      view.peak.textContent = peak.toFixed(1);
    });
  }

  function updateStatusSummary(st) {
    $("settingsIp").textContent = st.wifi ? `${st.ip || "-"} / ${st.rssi ?? 0} dBm` : "--";
    $("settingsUptime").textContent = formatUptime(st.uptime_s);
    $("settingsHistory").textContent = `${state.historyMinutes} min / ${state.historyCapacity} points`;
    $("settingsVersion").textContent = st.version || "--";
    $("settingsBuildDate").textContent = st.buildDate || "--";
    $("settingsBuildEnv").textContent = st.buildEnv || "--";
    $("settingsMcuTemp").textContent = st.mcuTempOk ? `${Number(st.mcuTempC ?? 0).toFixed(1)} C` : "--";
    const otaState = st.otaEnabled
      ? (st.otaStarted ? "actif" : "configure")
      : "off";
    const mqttState = st.mqttEnabled
      ? (st.mqttConnected ? "connecte" : (st.mqttLastError ? `erreur (${st.mqttLastError})` : "en attente"))
      : "off";
    $("settingsOtaMqtt").textContent = `OTA ${otaState} / MQTT ${mqttState}`;
    $("rawRmsAdv").textContent = Number(st.rawRms ?? 0).toFixed(2);
    $("rawPseudoDbAdv").textContent = Number(st.rawPseudoDb ?? 0).toFixed(1);
    $("rawAdcMeanAdv").textContent = String(st.rawAdcMean ?? "--");
    $("rawAdcLastAdv").textContent = String(st.rawAdcLast ?? "--");

    const systemBadge = $("settingsSystemBadge");
    systemBadge.textContent = "En ligne";
    systemBadge.classList.add("ok");
    systemBadge.classList.remove("bad");
  }

  function setSystemBadgeOnline() {
    state.lastContactMs = Date.now();
    const systemBadge = $("settingsSystemBadge");
    systemBadge.textContent = "En ligne";
    systemBadge.classList.add("ok");
    systemBadge.classList.remove("bad");
  }

  function setSystemBadgeError() {
    const systemBadge = $("settingsSystemBadge");
    systemBadge.textContent = "Erreur";
    systemBadge.classList.add("bad");
    systemBadge.classList.remove("ok");
  }

  function checkSystemHeartbeat() {
    const maxSilenceMs = 4000;
    if (!state.lastContactMs || (Date.now() - state.lastContactMs) > maxSilenceMs) {
      setSystemBadgeError();
    }
  }

  function applyStatus(st, options = {}) {
    state.status = { ...(state.status || {}), ...st };
    const merged = state.status;
    state.historyMinutes = Number(merged.historyMinutes ?? state.historyMinutes ?? 5);
    state.historyCapacity = Number(merged.historyCapacity ?? state.historyCapacity ?? 96);
    state.historySamplePeriodMs = Number(merged.historySamplePeriodMs ?? state.historySamplePeriodMs ?? 3000);

    const db = Number(merged.db ?? 0);
    const leq = Number(merged.leq ?? 0);
    const peak = Number(merged.peak ?? 0);
    const greenMax = Number(merged.greenMax ?? 55);
    const orangeMax = Number(merged.orangeMax ?? 70);
    const warningHoldSec = Number(merged.warningHoldSec ?? 3);
    const criticalHoldSec = Number(merged.criticalHoldSec ?? 2);

    updateGauge(db, greenMax, orangeMax);
    updateMetrics(leq, peak);
    updateAlertState(db, greenMax, orangeMax, warningHoldSec, criticalHoldSec);

    if ("time_ok" in st) syncClock(merged.time_ok ? merged.time : "");
    updateStatusSummary(merged);
    updateHistoryLabels();

    if (Array.isArray(merged.history) && (options.useHistorySnapshot || !state.historyInitialized)) {
      setHistory(merged.history);
    } else if (!options.skipAppendHistory && state.historyInitialized) {
      appendHistory(db);
    }

    $("calLiveMic").textContent = merged.analogOk
      ? `Micro live: rms=${Number(merged.rawRms ?? 0).toFixed(2)}`
      : "Micro live: indisponible";
    $("calLiveLog").textContent = merged.analogOk
      ? `Log calibration live: ${Math.log10((Number(merged.rawRms ?? 0) + 0.0001)).toFixed(4)}`
      : "Log calibration live: --";

    if (Array.isArray(merged.cal)) {
      let validCount = 0;
      merged.cal.forEach((point, index) => {
        const i = index + 1;
        if (!state.calRefsDirty[index]) {
          state.calRefs[index] = Number(point.refDb ?? state.calRefs[index] ?? 0);
        }
        $(`calRef${i}`).textContent = Number(state.calRefs[index] ?? 0).toFixed(0);
        if (point.valid) {
          validCount++;
          $(`calState${i}`).textContent = `Point ${i} capture ${Number(point.rawLogRms ?? 0).toFixed(3)}`;
        } else {
          $(`calState${i}`).textContent = `Point ${i} non capture`;
        }
      });
      $("calStatus").textContent = `${validCount} / 3 points valides`;
    }

    if (!state.uiDirty) {
      if ("backlight" in merged) {
        const backlightOn = Number(merged.backlight) > 0;
        $("bl").classList.toggle("active", backlightOn);
        $("bl").setAttribute("aria-pressed", backlightOn ? "true" : "false");
      }
      if ("greenMax" in merged) $("g").value = greenMax;
      if ("orangeMax" in merged) $("o").value = orangeMax;
      if ("historyMinutes" in merged) $("hist").value = state.historyMinutes;
      if ("warningHoldSec" in merged) $("warnHoldSec").value = warningHoldSec;
      if ("criticalHoldSec" in merged) $("critHoldSec").value = criticalHoldSec;
      if ("calibrationCaptureSec" in merged) $("calCaptureSec").value = Number(merged.calibrationCaptureSec ?? 3);
      if ("audioResponseMode" in merged) state.uiResponseMode = Number(merged.audioResponseMode ?? 0);
      syncUiLabels();
    }
  }

  function syncUiLabels() {
    const g = Number($("g").value);
    const o = Math.max(g, Number($("o").value));
    $("o").value = o;

    const backlightOn = $("bl").classList.contains("active");
    $("bl").setAttribute("aria-pressed", backlightOn ? "true" : "false");
    $("blVal").textContent = backlightOn ? "ON" : "OFF";
    $("gVal").textContent = g;
    $("oVal").textContent = o;
    $("hVal").textContent = $("hist").value;
    $("modeFast").classList.toggle("active", state.uiResponseMode === 0);
    $("modeSlow").classList.toggle("active", state.uiResponseMode === 1);
  }

  function markUiDirty() {
    state.uiDirty = true;
  }

  async function apiGet(url) {
    const res = await fetch(url, { cache: "no-store" });
    if (!res.ok) throw new Error(await res.text());
    return await res.json();
  }

  async function apiPost(url, payload) {
    const res = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload || {}),
    });
    if (!res.ok) throw new Error(await res.text());
    return await res.json();
  }

  async function refreshStatus() {
    try {
      const st = await apiGet("/api/status");
      state.hasLiveFeed = true;
      setSystemBadgeOnline();
      applyStatus(st, {
        skipAppendHistory: true,
        useHistorySnapshot: !state.historyInitialized
      });
    } catch (err) {
      setSystemBadgeError();
    }
  }

  function scheduleReconnect() {
    if (state.reconnectTimer) return;
    state.reconnectTimer = setTimeout(() => {
      state.reconnectTimer = null;
      connectLiveFeed();
    }, 1500);
  }

  function connectLiveFeed() {
    if (state.events) {
      state.events.close();
      state.events = null;
    }

    state.events = new EventSource(`${location.protocol}//${location.hostname}:81/api/events`);
    state.events.addEventListener("metrics", (ev) => {
      state.hasLiveFeed = true;
      setSystemBadgeOnline();
      applyStatus(JSON.parse(ev.data));
    });
    state.events.onerror = () => {
      state.hasLiveFeed = false;
      setSystemBadgeError();
      if (state.events) {
        state.events.close();
        state.events = null;
      }
      scheduleReconnect();
    };
  }

  async function saveUi() {
    const payload = {
      backlight: $("bl").classList.contains("active") ? 100 : 0,
      greenMax: Number($("g").value),
      orangeMax: Number($("o").value),
      historyMinutes: Number($("hist").value),
      audioResponseMode: state.uiResponseMode,
      warningHoldSec: Number($("warnHoldSec").value || 0),
      criticalHoldSec: Number($("critHoldSec").value || 0),
      calibrationCaptureSec: Number($("calCaptureSec").value || 3),
    };

    try {
      await apiPost("/api/ui", payload);
      $("uiToast").textContent = "UI sauvee.";
      state.uiDirty = false;
      await refreshStatus();
    } catch (err) {
      $("uiToast").textContent = `Erreur: ${err.message}`;
    }
  }

  async function refreshSettingsPanels() {
    state.uiDirty = false;
    state.calRefsDirty = [false, false, false];
    await refreshStatus();
    await loadTimeSettings();
    await loadOtaSettings();
    await loadMqttSettings();
  }

  async function toggleBacklight() {
    const wasOn = $("bl").classList.contains("active");
    const nextOn = !wasOn;

    $("bl").classList.toggle("active", nextOn);
    syncUiLabels();
    $("uiToast").textContent = "Sauvegarde...";

    try {
      await apiPost("/api/ui", { backlight: nextOn ? 100 : 0 });
      $("uiToast").textContent = "Backlight sauve.";
      await refreshStatus();
    } catch (err) {
      $("bl").classList.toggle("active", wasOn);
      syncUiLabels();
      $("uiToast").textContent = `Erreur: ${err.message}`;
    }
  }

  function downloadTextFile(filename, text) {
    const blob = new Blob([text], { type: "application/json;charset=utf-8" });
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = filename;
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
  }

  function exportFilename() {
    const d = new Date();
    const pad = (v) => String(v).padStart(2, "0");
    return `soundpanel7-config-${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}.json`;
  }

  async function exportConfig() {
    $("configToast").textContent = "Export...";
    try {
      const cfg = await apiGet("/api/config/export");
      const text = JSON.stringify(cfg, null, 2);
      $("configJsonBox").value = text;
      downloadTextFile(exportFilename(), text);
      $("configToast").textContent = "Config exportee.";
    } catch (err) {
      $("configToast").textContent = `Erreur: ${err.message}`;
    }
  }

  async function importConfig() {
    const raw = ($("configJsonBox").value || "").trim();
    if (!raw) {
      $("configToast").textContent = "Colle un JSON ou charge un fichier.";
      return;
    }

    let payload;
    try {
      payload = JSON.parse(raw);
    } catch (err) {
      $("configToast").textContent = `JSON invalide: ${err.message}`;
      return;
    }

    $("configToast").textContent = "Import...";
    try {
      const r = await apiPost("/api/config/import", payload);
      $("configToast").textContent = r.rebootRecommended ? "Config importee. Reboot recommande." : "Config importee.";
      await refreshSettingsPanels();
    } catch (err) {
      $("configToast").textContent = `Erreur: ${err.message}`;
    }
  }

  async function backupConfig() {
    $("configToast").textContent = "Backup...";
    try {
      await apiPost("/api/config/backup", {});
      $("configToast").textContent = "Backup usine enregistre.";
    } catch (err) {
      $("configToast").textContent = `Erreur: ${err.message}`;
    }
  }

  async function restoreConfig() {
    if (!confirm("Restaurer le backup usine ?")) return;
    $("configToast").textContent = "Restore...";
    try {
      const r = await apiPost("/api/config/restore", {});
      $("configToast").textContent = r.rebootRecommended ? "Backup restaure. Reboot recommande." : "Backup restaure.";
      await refreshSettingsPanels();
    } catch (err) {
      $("configToast").textContent = `Erreur: ${err.message}`;
    }
  }

  async function partialReset() {
    const scope = $("configResetScope").value;
    if (!confirm(`Reset partiel ${scope} ?`)) return;
    $("configToast").textContent = "Reset...";
    try {
      const r = await apiPost("/api/config/reset_partial", { scope });
      $("configToast").textContent = r.rebootRecommended ? `Section ${scope} reset. Reboot recommande.` : `Section ${scope} reset.`;
      await refreshSettingsPanels();
    } catch (err) {
      $("configToast").textContent = `Erreur: ${err.message}`;
    }
  }

  async function loadTimeSettings() {
    try {
      const t = await apiGet("/api/time");
      $("tzAdv").value = t.tz || "";
      $("ntpServerAdv").value = t.ntpServer || "";
      $("ntpSyncMinAdv").value = String(t.ntpSyncMinutes || 180);
      $("hostnameAdv").value = t.hostname || "";
    } catch (err) {
      $("toastTimeAdv").textContent = `Erreur: ${err.message}`;
    }
  }

  async function saveTimeSettings() {
    $("toastTimeAdv").textContent = "Sauvegarde...";
    try {
      await apiPost("/api/time", {
        tz: $("tzAdv").value.trim(),
        ntpServer: $("ntpServerAdv").value.trim(),
        ntpSyncMinutes: parseInt($("ntpSyncMinAdv").value, 10),
        hostname: $("hostnameAdv").value.trim()
      });
      $("toastTimeAdv").textContent = "Heure sauvee.";
      await refreshStatus();
    } catch (err) {
      $("toastTimeAdv").textContent = `Erreur: ${err.message}`;
    }
  }

  async function loadOtaSettings() {
    try {
      const o = await apiGet("/api/ota");
      $("otaEnabledAdv").value = o.enabled ? "1" : "0";
      $("otaHostnameAdv").value = o.hostname || "";
      $("otaPortAdv").value = o.port || 3232;
      $("otaPasswordAdv").value = "";
      $("otaPasswordAdv").placeholder = o.passwordConfigured ? "********" : "laisser vide = aucun mot de passe";
    } catch (err) {
      $("toastOtaAdv").textContent = `Erreur: ${err.message}`;
    }
  }

  async function saveOtaSettings() {
    $("toastOtaAdv").textContent = "Sauvegarde...";
    try {
      const r = await apiPost("/api/ota", {
        enabled: parseInt($("otaEnabledAdv").value, 10),
        hostname: $("otaHostnameAdv").value.trim(),
        port: parseInt($("otaPortAdv").value, 10),
        password: $("otaPasswordAdv").value
      });
      $("toastOtaAdv").textContent = r.rebootRequired ? "OTA sauvee. Reboot requis." : "OTA sauvee.";
    } catch (err) {
      $("toastOtaAdv").textContent = `Erreur: ${err.message}`;
    }
  }

  async function loadMqttSettings() {
    try {
      const m = await apiGet("/api/mqtt");
      $("mqttEnabledAdv").value = m.enabled ? "1" : "0";
      $("mqttHostAdv").value = m.host || "";
      $("mqttPortAdv").value = m.port || 1883;
      $("mqttUsernameAdv").value = m.username || "";
      $("mqttPasswordAdv").value = "";
      $("mqttPasswordAdv").placeholder = m.passwordConfigured ? "********" : "laisser vide si non utilise";
      $("mqttClientIdAdv").value = m.clientId || "soundpanel7";
      $("mqttBaseTopicAdv").value = m.baseTopic || "soundpanel7";
      $("mqttPublishPeriodMsAdv").value = m.publishPeriodMs || 1000;
      $("mqttRetainAdv").value = m.retain ? "1" : "0";
    } catch (err) {
      $("toastMqttAdv").textContent = `Erreur: ${err.message}`;
    }
  }

  async function saveMqttSettings() {
    $("toastMqttAdv").textContent = "Sauvegarde...";
    try {
      const r = await apiPost("/api/mqtt", {
        enabled: parseInt($("mqttEnabledAdv").value || "0", 10),
        host: ($("mqttHostAdv").value || "").trim(),
        port: parseInt($("mqttPortAdv").value || "1883", 10),
        username: ($("mqttUsernameAdv").value || "").trim(),
        password: ($("mqttPasswordAdv").value || ""),
        clientId: ($("mqttClientIdAdv").value || "").trim(),
        baseTopic: ($("mqttBaseTopicAdv").value || "").trim(),
        publishPeriodMs: parseInt($("mqttPublishPeriodMsAdv").value || "1000", 10),
        retain: parseInt($("mqttRetainAdv").value || "0", 10)
      });
      $("toastMqttAdv").textContent = r.rebootRecommended ? "MQTT sauve. Reboot recommande." : "MQTT sauve.";
    } catch (err) {
      $("toastMqttAdv").textContent = `Erreur: ${err.message}`;
    }
  }

  async function captureCalibration(index) {
    try {
      await apiPost("/api/calibrate", { index, refDb: state.calRefs[index] });
      state.calRefsDirty[index] = false;
      $("calToast").textContent = `Point ${index + 1} capture.`;
      await refreshStatus();
    } catch (err) {
      $("calToast").textContent = `Erreur: ${err.message}`;
    }
  }

  async function clearCalibration() {
    if (!confirm("Effacer la calibration ?")) return;
    try {
      await apiPost("/api/calibrate/clear", {});
      state.calRefsDirty = [false, false, false];
      $("calToast").textContent = "Calibration effacee.";
      await refreshStatus();
    } catch (err) {
      $("calToast").textContent = `Erreur: ${err.message}`;
    }
  }

  async function reboot() {
    try {
      await apiPost("/api/reboot", {});
      $("actionsToast").textContent = "Reboot demande.";
    } catch (err) {
      $("actionsToast").textContent = `Erreur: ${err.message}`;
    }
  }

  async function factoryReset() {
    if (!confirm("Factory reset ?")) return;
    try {
      await apiPost("/api/factory_reset", {});
      $("actionsToast").textContent = "Factory reset demande.";
    } catch (err) {
      $("actionsToast").textContent = `Erreur: ${err.message}`;
    }
  }

  document.querySelectorAll(".tab").forEach((tab) => {
    tab.addEventListener("click", () => setActivePage(tab.dataset.page));
  });

  $("bl").addEventListener("click", toggleBacklight);

  ["g", "o", "hist", "warnHoldSec", "critHoldSec", "calCaptureSec"].forEach((id) => {
    $(id).addEventListener("input", () => {
      markUiDirty();
      syncUiLabels();
    });
  });

  document.querySelectorAll("[data-mode]").forEach((btn) => {
    btn.addEventListener("click", () => {
      state.uiResponseMode = Number(btn.dataset.mode);
      markUiDirty();
      syncUiLabels();
    });
  });

  document.querySelectorAll("[data-cal-adjust]").forEach((btn) => {
    btn.addEventListener("click", () => {
      const [indexStr, deltaStr] = btn.dataset.calAdjust.split(":");
      const index = Number(indexStr);
      const delta = Number(deltaStr);
      state.calRefs[index] = Math.max(35, Math.min(100, Number(state.calRefs[index] || 0) + delta));
      state.calRefsDirty[index] = true;
      $(`calRef${index + 1}`).textContent = state.calRefs[index].toFixed(0);
    });
  });

  document.querySelectorAll("[data-cal-capture]").forEach((btn) => {
    btn.addEventListener("click", () => captureCalibration(Number(btn.dataset.calCapture)));
  });

  window.addEventListener("resize", drawHistory);

  $("saveUi").addEventListener("click", saveUi);
  $("exportConfigBtn").addEventListener("click", exportConfig);
  $("importConfigBtn").addEventListener("click", importConfig);
  $("backupConfigBtn").addEventListener("click", backupConfig);
  $("restoreConfigBtn").addEventListener("click", restoreConfig);
  $("partialResetBtn").addEventListener("click", partialReset);
  $("clearCalibration").addEventListener("click", clearCalibration);
  $("rebootBtn").addEventListener("click", reboot);
  $("factoryResetBtn").addEventListener("click", factoryReset);
  $("saveTimeAdv").addEventListener("click", saveTimeSettings);
  $("saveOtaAdv").addEventListener("click", saveOtaSettings);
  $("saveMqttAdv").addEventListener("click", saveMqttSettings);
  $("configFile").addEventListener("change", async (ev) => {
    const file = ev.target.files && ev.target.files[0];
    if (!file) return;
    try {
      $("configJsonBox").value = await file.text();
      $("configToast").textContent = `Fichier charge: ${file.name}`;
    } catch (err) {
      $("configToast").textContent = `Erreur lecture fichier: ${err.message}`;
    }
  });

  async function initPage() {
    syncUiLabels();
    await refreshStatus();
    connectLiveFeed();
    setInterval(refreshStatus, 2500);
    setInterval(checkSystemHeartbeat, 1000);
    loadTimeSettings();
    loadOtaSettings();
    loadMqttSettings();
    setInterval(renderClock, 1000);
  }

  initPage();
</script>
</body>
</html>
)HTML";

  _srv.send(200, "text/html; charset=utf-8", html);
}
