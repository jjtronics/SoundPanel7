#include "SettingsStore.h"

#include <cstring>
#include <ctime>

static int jsonIntLocal(const String& body, const char* key, int def) {
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

static String jsonStrLocal(const String& body, const char* key, const String& def) {
  String k = String("\"") + key + "\":";
  int p = body.indexOf(k);
  if (p < 0) return def;
  p += k.length();

  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
  if (p >= (int)body.length() || body[p] != '"') return def;
  p++;

  String out;
  while (p < (int)body.length()) {
    char c = body[p++];
    if (c == '\\' && p < (int)body.length()) {
      char n = body[p++];
      if (n == '"' || n == '\\' || n == '/') out += n;
      else if (n == 'n') out += '\n';
      else if (n == 'r') out += '\r';
      else if (n == 't') out += '\t';
      else out += n;
      continue;
    }
    if (c == '"') break;
    out += c;
  }

  return out;
}

static bool jsonBoolLocal(const String& body, const char* key, bool def) {
  String k = String("\"") + key + "\":";
  int p = body.indexOf(k);
  if (p < 0) return def;
  p += k.length();

  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
  if (body.startsWith("true", p)) return true;
  if (body.startsWith("false", p)) return false;
  if (p < (int)body.length() && body[p] == '1') return true;
  if (p < (int)body.length() && body[p] == '0') return false;
  return def;
}

static bool jsonFloatArray3Local(const String& body, const char* key, float out[3]) {
  String k = String("\"") + key + "\":";
  int p = body.indexOf(k);
  if (p < 0) return false;
  p += k.length();

  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
  if (p >= (int)body.length() || body[p] != '[') return false;
  p++;

  for (int i = 0; i < 3; i++) {
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

    if (num.isEmpty()) return false;
    out[i] = neg ? -num.toFloat() : num.toFloat();

    while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (i < 2) {
      if (p >= (int)body.length() || body[p] != ',') return false;
      p++;
    }
  }

  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
  return p < (int)body.length() && body[p] == ']';
}

static bool jsonU8Array3Local(const String& body, const char* key, uint8_t out[3]) {
  float vals[3] = {0.0f, 0.0f, 0.0f};
  if (!jsonFloatArray3Local(body, key, vals)) return false;
  for (int i = 0; i < 3; i++) out[i] = vals[i] > 0.5f ? 1 : 0;
  return true;
}

static String jsonEscapeLocal(const char* src) {
  String out;
  if (!src) return out;
  for (size_t i = 0; src[i]; i++) {
    char c = src[i];
    if (c == '\\' || c == '"') out += '\\';
    out += c;
  }
  return out;
}

static bool safeCopyLocal(char* dst, size_t dstSize, const String& src) {
  if (!dst || dstSize == 0) return false;
  if (src.length() >= dstSize) return false;
  memcpy(dst, src.c_str(), src.length() + 1);
  return true;
}

bool SettingsStore::begin(const char* nvsNamespace) {
  _ns = nvsNamespace ? nvsNamespace : "sp7";
  return _prefs.begin(_ns.c_str(), false);
}

void SettingsStore::load(SettingsV1 &out) {
  uint32_t magic = _prefs.getUInt("magic", 0);
  uint16_t ver   = _prefs.getUShort("ver", 0);

  if (magic != SETTINGS_MAGIC || ver != SETTINGS_VERSION) {
    save(out);
    return;
  }

  out.backlight      = (uint8_t)_prefs.getUChar("ui_bl", out.backlight);
  out.th.greenMax    = (uint8_t)_prefs.getUChar("th_g", out.th.greenMax);
  out.th.orangeMax   = (uint8_t)_prefs.getUChar("th_o", out.th.orangeMax);
  out.historyMinutes = (uint8_t)_prefs.getUChar("hist_m", out.historyMinutes);
  out.orangeAlertHoldMs = _prefs.getUInt("ui_ow_ms", out.orangeAlertHoldMs);
  out.redAlertHoldMs = _prefs.getUInt("ui_rw_ms", out.redAlertHoldMs);

  _prefs.getString("tz", out.tz, sizeof(out.tz));
  _prefs.getString("ntp", out.ntpServer, sizeof(out.ntpServer));
  out.ntpSyncIntervalMs = _prefs.getUInt("ntp_ms", out.ntpSyncIntervalMs);
  _prefs.getString("hn", out.hostname, sizeof(out.hostname));

  out.otaEnabled = (uint8_t)_prefs.getUChar("ota_en", out.otaEnabled);
  out.otaPort = (uint16_t)_prefs.getUShort("ota_pt", out.otaPort);
  _prefs.getString("ota_hn", out.otaHostname, sizeof(out.otaHostname));
  _prefs.getString("ota_pw", out.otaPassword, sizeof(out.otaPassword));

  out.audioSource        = (uint8_t)_prefs.getUChar("a_src", out.audioSource);
  out.analogPin          = (uint8_t)_prefs.getUChar("a_pin", out.analogPin);
  out.analogRmsSamples   = (uint16_t)_prefs.getUShort("a_rms", out.analogRmsSamples);
  out.audioResponseMode  = (uint8_t)_prefs.getUChar("a_resp", out.audioResponseMode);
  out.emaAlpha           = _prefs.getFloat("a_ema", out.emaAlpha);
  out.peakHoldMs         = _prefs.getUInt("a_peak", out.peakHoldMs);
  out.analogBaseOffsetDb = _prefs.getFloat("a_base", out.analogBaseOffsetDb);
  out.analogExtraOffsetDb= _prefs.getFloat("a_extra", out.analogExtraOffsetDb);
  out.calibrationCaptureMs = _prefs.getUInt("cal_capms", out.calibrationCaptureMs);

  out.mqttEnabled = (uint8_t)_prefs.getUChar("mq_en", out.mqttEnabled);
  _prefs.getString("mq_host", out.mqttHost, sizeof(out.mqttHost));
  out.mqttPort = (uint16_t)_prefs.getUShort("mq_pt", out.mqttPort);
  _prefs.getString("mq_usr", out.mqttUsername, sizeof(out.mqttUsername));
  _prefs.getString("mq_pwd", out.mqttPassword, sizeof(out.mqttPassword));
  _prefs.getString("mq_cid", out.mqttClientId, sizeof(out.mqttClientId));
  _prefs.getString("mq_base", out.mqttBaseTopic, sizeof(out.mqttBaseTopic));
  out.mqttPublishPeriodMs = (uint16_t)_prefs.getUShort("mq_pubms", out.mqttPublishPeriodMs);
  out.mqttRetain = (uint8_t)_prefs.getUChar("mq_ret", out.mqttRetain);

  for (uint8_t i = 0; i < 3; i++) {
    char keyRef[8];
    char keyRaw[8];
    char keyVal[8];
    snprintf(keyRef, sizeof(keyRef), "c%dr", i + 1);
    snprintf(keyRaw, sizeof(keyRaw), "c%dx", i + 1);
    snprintf(keyVal, sizeof(keyVal), "c%dv", i + 1);

    out.calPointRefDb[i]     = _prefs.getFloat(keyRef, out.calPointRefDb[i]);
    out.calPointRawLogRms[i] = _prefs.getFloat(keyRaw, out.calPointRawLogRms[i]);
    out.calPointValid[i]     = (uint8_t)_prefs.getUChar(keyVal, out.calPointValid[i]);
  }

  sanitize(out);
}

void SettingsStore::save(const SettingsV1 &s) {
  _prefs.putUInt("magic", SETTINGS_MAGIC);
  _prefs.putUShort("ver", SETTINGS_VERSION);

  _prefs.putUChar("ui_bl", s.backlight);
  _prefs.putUChar("th_g", s.th.greenMax);
  _prefs.putUChar("th_o", s.th.orangeMax);
  _prefs.putUChar("hist_m", s.historyMinutes);
  _prefs.putUInt("ui_ow_ms", s.orangeAlertHoldMs);
  _prefs.putUInt("ui_rw_ms", s.redAlertHoldMs);

  _prefs.putString("tz", s.tz);
  _prefs.putString("ntp", s.ntpServer);
  _prefs.putUInt("ntp_ms", s.ntpSyncIntervalMs);
  _prefs.putString("hn", s.hostname);

  _prefs.putUChar("ota_en", s.otaEnabled);
  _prefs.putUShort("ota_pt", s.otaPort);
  _prefs.putString("ota_hn", s.otaHostname);
  _prefs.putString("ota_pw", s.otaPassword);

  _prefs.putUChar("a_src", s.audioSource);
  _prefs.putUChar("a_pin", s.analogPin);
  _prefs.putUShort("a_rms", s.analogRmsSamples);
  _prefs.putUChar("a_resp", s.audioResponseMode);
  _prefs.putFloat("a_ema", s.emaAlpha);
  _prefs.putUInt("a_peak", s.peakHoldMs);
  _prefs.putFloat("a_base", s.analogBaseOffsetDb);
  _prefs.putFloat("a_extra", s.analogExtraOffsetDb);
  _prefs.putUInt("cal_capms", s.calibrationCaptureMs);

  _prefs.putUChar("mq_en", s.mqttEnabled);
  _prefs.putString("mq_host", s.mqttHost);
  _prefs.putUShort("mq_pt", s.mqttPort);
  _prefs.putString("mq_usr", s.mqttUsername);
  _prefs.putString("mq_pwd", s.mqttPassword);
  _prefs.putString("mq_cid", s.mqttClientId);
  _prefs.putString("mq_base", s.mqttBaseTopic);
  _prefs.putUShort("mq_pubms", s.mqttPublishPeriodMs);
  _prefs.putUChar("mq_ret", s.mqttRetain);

  for (uint8_t i = 0; i < 3; i++) {
    char keyRef[8];
    char keyRaw[8];
    char keyVal[8];
    snprintf(keyRef, sizeof(keyRef), "c%dr", i + 1);
    snprintf(keyRaw, sizeof(keyRaw), "c%dx", i + 1);
    snprintf(keyVal, sizeof(keyVal), "c%dv", i + 1);

    _prefs.putFloat(keyRef, s.calPointRefDb[i]);
    _prefs.putFloat(keyRaw, s.calPointRawLogRms[i]);
    _prefs.putUChar(keyVal, s.calPointValid[i]);
  }
}

void SettingsStore::factoryReset() {
  _prefs.clear();
}

void SettingsStore::sanitize(SettingsV1& s) {
  if (s.backlight > 100) s.backlight = 100;
  if (s.th.greenMax > 100) s.th.greenMax = 100;
  if (s.th.orangeMax > 100) s.th.orangeMax = 100;
  if (s.th.orangeMax < s.th.greenMax) s.th.orangeMax = s.th.greenMax;

  if (s.historyMinutes < 1) s.historyMinutes = 1;
  if (s.historyMinutes > 60) s.historyMinutes = 60;

  if (s.orangeAlertHoldMs > 60000UL) s.orangeAlertHoldMs = 60000UL;
  if (s.redAlertHoldMs > 60000UL) s.redAlertHoldMs = 60000UL;

  if (s.audioSource > 1) s.audioSource = 1;
  if (s.analogRmsSamples < 32) s.analogRmsSamples = 32;
  if (s.analogRmsSamples > 1024) s.analogRmsSamples = 1024;
  if (s.audioResponseMode > 1) s.audioResponseMode = 0;
  if (s.emaAlpha < 0.01f) s.emaAlpha = 0.01f;
  if (s.emaAlpha > 0.95f) s.emaAlpha = 0.95f;
  if (s.peakHoldMs < 500) s.peakHoldMs = 500;
  if (s.peakHoldMs > 30000UL) s.peakHoldMs = 30000UL;
  if (s.calibrationCaptureMs < 1000UL) s.calibrationCaptureMs = 1000UL;
  if (s.calibrationCaptureMs > 30000UL) s.calibrationCaptureMs = 30000UL;

  if (s.ntpSyncIntervalMs < 60000UL) s.ntpSyncIntervalMs = 60000UL;
  if (s.ntpSyncIntervalMs > 86400000UL) s.ntpSyncIntervalMs = 86400000UL;

  if (s.otaPort == 0) s.otaPort = 3232;
  if (s.mqttPort == 0) s.mqttPort = 1883;
  if (s.mqttPublishPeriodMs < 250) s.mqttPublishPeriodMs = 250;
  if (s.mqttPublishPeriodMs > 60000) s.mqttPublishPeriodMs = 60000;
  s.otaEnabled = s.otaEnabled ? 1 : 0;
  s.mqttEnabled = s.mqttEnabled ? 1 : 0;
  s.mqttRetain = s.mqttRetain ? 1 : 0;

  for (int i = 0; i < 3; i++) s.calPointValid[i] = s.calPointValid[i] ? 1 : 0;
}

String SettingsStore::exportJson(const SettingsV1& s) const {
  String json;
  json.reserve(2048);
  json += "{";
  json += "\"type\":\"soundpanel7-config\",";
  json += "\"version\":"; json += String(SETTINGS_VERSION); json += ",";
  json += "\"backlight\":"; json += String(s.backlight); json += ",";
  json += "\"greenMax\":"; json += String(s.th.greenMax); json += ",";
  json += "\"orangeMax\":"; json += String(s.th.orangeMax); json += ",";
  json += "\"historyMinutes\":"; json += String(s.historyMinutes); json += ",";
  json += "\"warningHoldSec\":"; json += String(s.orangeAlertHoldMs / 1000UL); json += ",";
  json += "\"criticalHoldSec\":"; json += String(s.redAlertHoldMs / 1000UL); json += ",";
  json += "\"tz\":\""; json += jsonEscapeLocal(s.tz); json += "\",";
  json += "\"ntpServer\":\""; json += jsonEscapeLocal(s.ntpServer); json += "\",";
  json += "\"ntpSyncMinutes\":"; json += String(s.ntpSyncIntervalMs / 60000UL); json += ",";
  json += "\"hostname\":\""; json += jsonEscapeLocal(s.hostname); json += "\",";
  json += "\"audioSource\":"; json += String(s.audioSource); json += ",";
  json += "\"analogPin\":"; json += String(s.analogPin); json += ",";
  json += "\"analogRmsSamples\":"; json += String(s.analogRmsSamples); json += ",";
  json += "\"audioResponseMode\":"; json += String(s.audioResponseMode); json += ",";
  json += "\"emaAlpha\":"; json += String(s.emaAlpha, 4); json += ",";
  json += "\"peakHoldMs\":"; json += String(s.peakHoldMs); json += ",";
  json += "\"calibrationCaptureSec\":"; json += String(s.calibrationCaptureMs / 1000UL); json += ",";
  json += "\"analogBaseOffsetDb\":"; json += String(s.analogBaseOffsetDb, 4); json += ",";
  json += "\"analogExtraOffsetDb\":"; json += String(s.analogExtraOffsetDb, 4); json += ",";
  json += "\"calPointRefDb\":["; json += String(s.calPointRefDb[0], 2); json += ","; json += String(s.calPointRefDb[1], 2); json += ","; json += String(s.calPointRefDb[2], 2); json += "],";
  json += "\"calPointRawLogRms\":["; json += String(s.calPointRawLogRms[0], 4); json += ","; json += String(s.calPointRawLogRms[1], 4); json += ","; json += String(s.calPointRawLogRms[2], 4); json += "],";
  json += "\"calPointValid\":["; json += String(s.calPointValid[0]); json += ","; json += String(s.calPointValid[1]); json += ","; json += String(s.calPointValid[2]); json += "],";
  json += "\"otaEnabled\":"; json += (s.otaEnabled ? "true" : "false"); json += ",";
  json += "\"otaPort\":"; json += String(s.otaPort); json += ",";
  json += "\"otaHostname\":\""; json += jsonEscapeLocal(s.otaHostname); json += "\",";
  json += "\"otaPassword\":\""; json += jsonEscapeLocal(s.otaPassword); json += "\",";
  json += "\"mqttEnabled\":"; json += (s.mqttEnabled ? "true" : "false"); json += ",";
  json += "\"mqttHost\":\""; json += jsonEscapeLocal(s.mqttHost); json += "\",";
  json += "\"mqttPort\":"; json += String(s.mqttPort); json += ",";
  json += "\"mqttUsername\":\""; json += jsonEscapeLocal(s.mqttUsername); json += "\",";
  json += "\"mqttPassword\":\""; json += jsonEscapeLocal(s.mqttPassword); json += "\",";
  json += "\"mqttClientId\":\""; json += jsonEscapeLocal(s.mqttClientId); json += "\",";
  json += "\"mqttBaseTopic\":\""; json += jsonEscapeLocal(s.mqttBaseTopic); json += "\",";
  json += "\"mqttPublishPeriodMs\":"; json += String(s.mqttPublishPeriodMs); json += ",";
  json += "\"mqttRetain\":"; json += (s.mqttRetain ? "true" : "false");
  json += "}";
  return json;
}

bool SettingsStore::importJson(SettingsV1& s, const String& json, String* err) {
  SettingsV1 next = s;

  next.backlight = (uint8_t)jsonIntLocal(json, "backlight", next.backlight);
  next.th.greenMax = (uint8_t)jsonIntLocal(json, "greenMax", next.th.greenMax);
  next.th.orangeMax = (uint8_t)jsonIntLocal(json, "orangeMax", next.th.orangeMax);
  next.historyMinutes = (uint8_t)jsonIntLocal(json, "historyMinutes", next.historyMinutes);
  next.orangeAlertHoldMs = (uint32_t)jsonIntLocal(json, "warningHoldSec", (int)(next.orangeAlertHoldMs / 1000UL)) * 1000UL;
  next.redAlertHoldMs = (uint32_t)jsonIntLocal(json, "criticalHoldSec", (int)(next.redAlertHoldMs / 1000UL)) * 1000UL;

  String tz = jsonStrLocal(json, "tz", String(next.tz));
  String ntp = jsonStrLocal(json, "ntpServer", String(next.ntpServer));
  String hostname = jsonStrLocal(json, "hostname", String(next.hostname));
  if (!safeCopyLocal(next.tz, sizeof(next.tz), tz)) {
    if (err) *err = "tz too long";
    return false;
  }
  if (!safeCopyLocal(next.ntpServer, sizeof(next.ntpServer), ntp)) {
    if (err) *err = "ntpServer too long";
    return false;
  }
  if (!safeCopyLocal(next.hostname, sizeof(next.hostname), hostname)) {
    if (err) *err = "hostname too long";
    return false;
  }
  next.ntpSyncIntervalMs = (uint32_t)jsonIntLocal(json, "ntpSyncMinutes", (int)(next.ntpSyncIntervalMs / 60000UL)) * 60000UL;

  next.audioSource = (uint8_t)jsonIntLocal(json, "audioSource", next.audioSource);
  next.analogPin = (uint8_t)jsonIntLocal(json, "analogPin", next.analogPin);
  next.analogRmsSamples = (uint16_t)jsonIntLocal(json, "analogRmsSamples", next.analogRmsSamples);
  next.audioResponseMode = (uint8_t)jsonIntLocal(json, "audioResponseMode", next.audioResponseMode);
  next.emaAlpha = jsonFloatLocal(json, "emaAlpha", next.emaAlpha);
  next.peakHoldMs = (uint32_t)jsonIntLocal(json, "peakHoldMs", next.peakHoldMs);
  next.calibrationCaptureMs = (uint32_t)jsonIntLocal(json, "calibrationCaptureSec", (int)(next.calibrationCaptureMs / 1000UL)) * 1000UL;
  next.analogBaseOffsetDb = jsonFloatLocal(json, "analogBaseOffsetDb", next.analogBaseOffsetDb);
  next.analogExtraOffsetDb = jsonFloatLocal(json, "analogExtraOffsetDb", next.analogExtraOffsetDb);

  float calRef[3];
  if (jsonFloatArray3Local(json, "calPointRefDb", calRef)) {
    for (int i = 0; i < 3; i++) next.calPointRefDb[i] = calRef[i];
  }
  float calRaw[3];
  if (jsonFloatArray3Local(json, "calPointRawLogRms", calRaw)) {
    for (int i = 0; i < 3; i++) next.calPointRawLogRms[i] = calRaw[i];
  }
  uint8_t calValid[3];
  if (jsonU8Array3Local(json, "calPointValid", calValid)) {
    for (int i = 0; i < 3; i++) next.calPointValid[i] = calValid[i];
  }

  next.otaEnabled = jsonBoolLocal(json, "otaEnabled", next.otaEnabled != 0) ? 1 : 0;
  next.otaPort = (uint16_t)jsonIntLocal(json, "otaPort", next.otaPort);
  String otaHostname = jsonStrLocal(json, "otaHostname", String(next.otaHostname));
  String otaPassword = jsonStrLocal(json, "otaPassword", String(next.otaPassword));
  if (!safeCopyLocal(next.otaHostname, sizeof(next.otaHostname), otaHostname)) {
    if (err) *err = "otaHostname too long";
    return false;
  }
  if (!safeCopyLocal(next.otaPassword, sizeof(next.otaPassword), otaPassword)) {
    if (err) *err = "otaPassword too long";
    return false;
  }

  next.mqttEnabled = jsonBoolLocal(json, "mqttEnabled", next.mqttEnabled != 0) ? 1 : 0;
  next.mqttPort = (uint16_t)jsonIntLocal(json, "mqttPort", next.mqttPort);
  next.mqttPublishPeriodMs = (uint16_t)jsonIntLocal(json, "mqttPublishPeriodMs", next.mqttPublishPeriodMs);
  next.mqttRetain = jsonBoolLocal(json, "mqttRetain", next.mqttRetain != 0) ? 1 : 0;

  String mqttHost = jsonStrLocal(json, "mqttHost", String(next.mqttHost));
  String mqttUsername = jsonStrLocal(json, "mqttUsername", String(next.mqttUsername));
  String mqttPassword = jsonStrLocal(json, "mqttPassword", String(next.mqttPassword));
  String mqttClientId = jsonStrLocal(json, "mqttClientId", String(next.mqttClientId));
  String mqttBaseTopic = jsonStrLocal(json, "mqttBaseTopic", String(next.mqttBaseTopic));

  if (!safeCopyLocal(next.mqttHost, sizeof(next.mqttHost), mqttHost)) {
    if (err) *err = "mqttHost too long";
    return false;
  }
  if (!safeCopyLocal(next.mqttUsername, sizeof(next.mqttUsername), mqttUsername)) {
    if (err) *err = "mqttUsername too long";
    return false;
  }
  if (!safeCopyLocal(next.mqttPassword, sizeof(next.mqttPassword), mqttPassword)) {
    if (err) *err = "mqttPassword too long";
    return false;
  }
  if (!safeCopyLocal(next.mqttClientId, sizeof(next.mqttClientId), mqttClientId)) {
    if (err) *err = "mqttClientId too long";
    return false;
  }
  if (!safeCopyLocal(next.mqttBaseTopic, sizeof(next.mqttBaseTopic), mqttBaseTopic)) {
    if (err) *err = "mqttBaseTopic too long";
    return false;
  }

  sanitize(next);
  s = next;
  return true;
}

bool SettingsStore::saveBackup(const SettingsV1& s) {
  Preferences backupPrefs;
  String backupNs = _ns + "_bak";
  if (!backupPrefs.begin(backupNs.c_str(), false)) return false;
  backupPrefs.putString("cfg", exportJson(s));
  time_t now = time(nullptr);
  backupPrefs.putUInt("ts", now > 946684800 ? (uint32_t)now : 0U);
  backupPrefs.end();
  return true;
}

uint32_t SettingsStore::backupTimestamp() const {
  Preferences backupPrefs;
  String backupNs = _ns + "_bak";
  if (!backupPrefs.begin(backupNs.c_str(), true)) return 0;
  uint32_t ts = backupPrefs.getUInt("ts", 0);
  backupPrefs.end();
  return ts;
}

bool SettingsStore::restoreBackup(SettingsV1& out, String* err) {
  Preferences backupPrefs;
  String backupNs = _ns + "_bak";
  if (!backupPrefs.begin(backupNs.c_str(), true)) {
    if (err) *err = "backup unavailable";
    return false;
  }
  String json = backupPrefs.getString("cfg", "");
  backupPrefs.end();

  if (!json.length()) {
    if (err) *err = "backup empty";
    return false;
  }

  return importJson(out, json, err);
}

bool SettingsStore::resetSection(SettingsV1& s, const String& scope, String* err) {
  SettingsV1 def;

  if (scope == "ui") {
    s.backlight = def.backlight;
    s.th = def.th;
    s.historyMinutes = def.historyMinutes;
    s.orangeAlertHoldMs = def.orangeAlertHoldMs;
    s.redAlertHoldMs = def.redAlertHoldMs;
    s.audioResponseMode = def.audioResponseMode;
  } else if (scope == "time") {
    memcpy(s.tz, def.tz, sizeof(s.tz));
    memcpy(s.ntpServer, def.ntpServer, sizeof(s.ntpServer));
    s.ntpSyncIntervalMs = def.ntpSyncIntervalMs;
    memcpy(s.hostname, def.hostname, sizeof(s.hostname));
  } else if (scope == "audio") {
    s.audioSource = def.audioSource;
    s.analogPin = def.analogPin;
    s.analogRmsSamples = def.analogRmsSamples;
    s.audioResponseMode = def.audioResponseMode;
    s.emaAlpha = def.emaAlpha;
    s.peakHoldMs = def.peakHoldMs;
    s.analogBaseOffsetDb = def.analogBaseOffsetDb;
    s.analogExtraOffsetDb = def.analogExtraOffsetDb;
  } else if (scope == "calibration") {
    s.calibrationCaptureMs = def.calibrationCaptureMs;
    memcpy(s.calPointRefDb, def.calPointRefDb, sizeof(s.calPointRefDb));
    memcpy(s.calPointRawLogRms, def.calPointRawLogRms, sizeof(s.calPointRawLogRms));
    memcpy(s.calPointValid, def.calPointValid, sizeof(s.calPointValid));
  } else if (scope == "ota") {
    s.otaEnabled = def.otaEnabled;
    s.otaPort = def.otaPort;
    memcpy(s.otaHostname, def.otaHostname, sizeof(s.otaHostname));
    memcpy(s.otaPassword, def.otaPassword, sizeof(s.otaPassword));
  } else if (scope == "mqtt") {
    s.mqttEnabled = def.mqttEnabled;
    memcpy(s.mqttHost, def.mqttHost, sizeof(s.mqttHost));
    s.mqttPort = def.mqttPort;
    memcpy(s.mqttUsername, def.mqttUsername, sizeof(s.mqttUsername));
    memcpy(s.mqttPassword, def.mqttPassword, sizeof(s.mqttPassword));
    memcpy(s.mqttClientId, def.mqttClientId, sizeof(s.mqttClientId));
    memcpy(s.mqttBaseTopic, def.mqttBaseTopic, sizeof(s.mqttBaseTopic));
    s.mqttPublishPeriodMs = def.mqttPublishPeriodMs;
    s.mqttRetain = def.mqttRetain;
  } else {
    if (err) *err = "unknown scope";
    return false;
  }

  sanitize(s);
  return true;
}
