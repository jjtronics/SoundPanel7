#include "SettingsStore.h"
#include "JsonHelpers.h"

#include <ctime>
#include <math.h>

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
  out.liveEnabled = (uint8_t)_prefs.getUChar("ui_live", out.liveEnabled);
  out.touchEnabled = (uint8_t)_prefs.getUChar("ui_touch", out.touchEnabled);
  out.dashboardPage = (uint8_t)_prefs.getUChar("ui_page", out.dashboardPage);
  out.dashboardFullscreenMask = (uint8_t)_prefs.getUChar("ui_fsm", out.dashboardFullscreenMask);
  _prefs.getString("ui_pin", out.dashboardPin, sizeof(out.dashboardPin));
  _prefs.getString("ha_tok", out.homeAssistantToken, sizeof(out.homeAssistantToken));

  _prefs.getString("tz", out.tz, sizeof(out.tz));
  _prefs.getString("ntp", out.ntpServer, sizeof(out.ntpServer));
  out.ntpSyncIntervalMs = _prefs.getUInt("ntp_ms", out.ntpSyncIntervalMs);
  _prefs.getString("hn", out.hostname, sizeof(out.hostname));
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    char keySsid[8];
    char keyPass[8];
    snprintf(keySsid, sizeof(keySsid), "wf%us", (unsigned)(i + 1));
    snprintf(keyPass, sizeof(keyPass), "wf%up", (unsigned)(i + 1));
    _prefs.getString(keySsid, out.wifiCredentials[i].ssid, sizeof(out.wifiCredentials[i].ssid));
    _prefs.getString(keyPass, out.wifiCredentials[i].password, sizeof(out.wifiCredentials[i].password));
  }

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
  out.calibrationPointCount = (uint8_t)_prefs.getUChar("cal_cnt", out.calibrationPointCount);
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

  for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
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
  _prefs.putUChar("ui_live", s.liveEnabled);
  _prefs.putUChar("ui_touch", s.touchEnabled);
  _prefs.putUChar("ui_page", s.dashboardPage);
  _prefs.putUChar("ui_fsm", s.dashboardFullscreenMask);
  _prefs.putString("ui_pin", s.dashboardPin);
  _prefs.putString("ha_tok", s.homeAssistantToken);

  _prefs.putString("tz", s.tz);
  _prefs.putString("ntp", s.ntpServer);
  _prefs.putUInt("ntp_ms", s.ntpSyncIntervalMs);
  _prefs.putString("hn", s.hostname);
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    char keySsid[8];
    char keyPass[8];
    snprintf(keySsid, sizeof(keySsid), "wf%us", (unsigned)(i + 1));
    snprintf(keyPass, sizeof(keyPass), "wf%up", (unsigned)(i + 1));
    _prefs.putString(keySsid, s.wifiCredentials[i].ssid);
    _prefs.putString(keyPass, s.wifiCredentials[i].password);
  }

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
  _prefs.putUChar("cal_cnt", s.calibrationPointCount);
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

  for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
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
  s.liveEnabled = s.liveEnabled ? LIVE_ENABLED : LIVE_DISABLED;
  s.touchEnabled = s.touchEnabled ? 1 : 0;
  s.dashboardPage = normalizedDashboardPage(s.dashboardPage);
  s.dashboardFullscreenMask = normalizedDashboardFullscreenMask(s.dashboardFullscreenMask);

  if (s.orangeAlertHoldMs > MAX_ALERT_HOLD_MS) s.orangeAlertHoldMs = MAX_ALERT_HOLD_MS;
  if (s.redAlertHoldMs > MAX_ALERT_HOLD_MS) s.redAlertHoldMs = MAX_ALERT_HOLD_MS;
  if (s.dashboardPin[0] && !pinCodeIsValid(s.dashboardPin)) {
    s.dashboardPin[0] = '\0';
  }
  s.homeAssistantToken[sizeof(s.homeAssistantToken) - 1] = '\0';

  if (s.audioSource > 1) s.audioSource = 1;
  if (s.analogRmsSamples < 32) s.analogRmsSamples = 32;
  if (s.analogRmsSamples > 1024) s.analogRmsSamples = 1024;
  if (s.audioResponseMode > 1) s.audioResponseMode = 0;
  if (s.emaAlpha < 0.01f) s.emaAlpha = 0.01f;
  if (s.emaAlpha > 0.95f) s.emaAlpha = 0.95f;
  if (s.peakHoldMs < 500) s.peakHoldMs = 500;
  if (s.peakHoldMs > MAX_PEAK_HOLD_MS) s.peakHoldMs = MAX_PEAK_HOLD_MS;
  s.calibrationPointCount = normalizedCalibrationPointCount(s.calibrationPointCount);
  if (s.calibrationCaptureMs < MIN_CALIBRATION_CAPTURE_MS) s.calibrationCaptureMs = MIN_CALIBRATION_CAPTURE_MS;
  if (s.calibrationCaptureMs > MAX_CALIBRATION_CAPTURE_MS) s.calibrationCaptureMs = MAX_CALIBRATION_CAPTURE_MS;

  if (s.ntpSyncIntervalMs < MIN_NTP_SYNC_INTERVAL_MS) s.ntpSyncIntervalMs = MIN_NTP_SYNC_INTERVAL_MS;
  if (s.ntpSyncIntervalMs > MAX_NTP_SYNC_INTERVAL_MS) s.ntpSyncIntervalMs = MAX_NTP_SYNC_INTERVAL_MS;
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    sanitizeWifiCredential(s.wifiCredentials[i]);
  }

  if (s.otaPort == 0) s.otaPort = 3232;
  if (s.mqttPort == 0) s.mqttPort = 1883;
  if (s.mqttPublishPeriodMs < MIN_MQTT_PUBLISH_PERIOD_MS) s.mqttPublishPeriodMs = MIN_MQTT_PUBLISH_PERIOD_MS;
  if (s.mqttPublishPeriodMs > MAX_MQTT_PUBLISH_PERIOD_MS) s.mqttPublishPeriodMs = MAX_MQTT_PUBLISH_PERIOD_MS;
  s.otaEnabled = s.otaEnabled ? 1 : 0;
  s.mqttEnabled = s.mqttEnabled ? 1 : 0;
  s.mqttRetain = s.mqttRetain ? 1 : 0;

  const float* recommended = (s.calibrationPointCount == CALIBRATION_POINT_MAX) ? RECOMMENDED_CALIBRATION_5 : RECOMMENDED_CALIBRATION_3;
  for (int i = 0; i < CALIBRATION_POINT_MAX; i++) {
    s.calPointValid[i] = s.calPointValid[i] ? 1 : 0;
    if (!isfinite(s.calPointRefDb[i])) s.calPointRefDb[i] = recommended[i];
    if (s.calPointRefDb[i] < 35.0f) s.calPointRefDb[i] = 35.0f;
    if (s.calPointRefDb[i] > 110.0f) s.calPointRefDb[i] = 110.0f;
    if (!isfinite(s.calPointRawLogRms[i])) s.calPointRawLogRms[i] = 0.0f;
  }
}

void SettingsStore::sanitizeWebUser(WebUserRecord& user) {
  user.active = user.active ? 1 : 0;
  if (!user.active) {
    user.username[0] = '\0';
    user.passwordSalt[0] = '\0';
    user.passwordHash[0] = '\0';
    return;
  }

  user.username[sizeof(user.username) - 1] = '\0';
  user.passwordSalt[sizeof(user.passwordSalt) - 1] = '\0';
  user.passwordHash[sizeof(user.passwordHash) - 1] = '\0';

  if (!strlen(user.username) || !strlen(user.passwordSalt) || !strlen(user.passwordHash)) {
    user.active = 0;
    user.username[0] = '\0';
    user.passwordSalt[0] = '\0';
    user.passwordHash[0] = '\0';
  }
}

void SettingsStore::sanitizeWifiCredential(WifiCredentialRecord& credential) {
  credential.ssid[sizeof(credential.ssid) - 1] = '\0';
  credential.password[sizeof(credential.password) - 1] = '\0';

  if (!strlen(credential.ssid)) {
    credential.password[0] = '\0';
  }
}

void SettingsStore::loadWebUsers(WebUserRecord (&out)[WEB_USER_MAX_COUNT]) {
  for (uint8_t i = 0; i < WEB_USER_MAX_COUNT; i++) {
    out[i] = WebUserRecord{};

    char keyActive[8];
    char keyUser[8];
    char keySalt[8];
    char keyHash[8];
    snprintf(keyActive, sizeof(keyActive), "wu%da", i + 1);
    snprintf(keyUser, sizeof(keyUser), "wu%du", i + 1);
    snprintf(keySalt, sizeof(keySalt), "wu%ds", i + 1);
    snprintf(keyHash, sizeof(keyHash), "wu%dh", i + 1);

    out[i].active = (uint8_t)_prefs.getUChar(keyActive, 0);
    _prefs.getString(keyUser, out[i].username, sizeof(out[i].username));
    _prefs.getString(keySalt, out[i].passwordSalt, sizeof(out[i].passwordSalt));
    _prefs.getString(keyHash, out[i].passwordHash, sizeof(out[i].passwordHash));
    sanitizeWebUser(out[i]);
  }
}

uint8_t SettingsStore::webUserCount() {
  WebUserRecord users[WEB_USER_MAX_COUNT];
  loadWebUsers(users);

  uint8_t count = 0;
  for (const WebUserRecord& user : users) {
    if (user.active) count++;
  }
  return count;
}

bool SettingsStore::upsertWebUser(const WebUserRecord& user, String* err) {
  WebUserRecord next = user;
  sanitizeWebUser(next);
  if (!next.active) {
    if (err) *err = "invalid user";
    return false;
  }

  WebUserRecord users[WEB_USER_MAX_COUNT];
  loadWebUsers(users);

  int existingIndex = -1;
  int freeIndex = -1;
  for (uint8_t i = 0; i < WEB_USER_MAX_COUNT; i++) {
    if (users[i].active && strcmp(users[i].username, next.username) == 0) {
      existingIndex = i;
      break;
    }
    if (!users[i].active && freeIndex < 0) freeIndex = i;
  }

  const int slot = existingIndex >= 0 ? existingIndex : freeIndex;
  if (slot < 0) {
    if (err) *err = "user limit reached";
    return false;
  }

  users[slot] = next;

  char keyActive[8];
  char keyUser[8];
  char keySalt[8];
  char keyHash[8];
  snprintf(keyActive, sizeof(keyActive), "wu%da", slot + 1);
  snprintf(keyUser, sizeof(keyUser), "wu%du", slot + 1);
  snprintf(keySalt, sizeof(keySalt), "wu%ds", slot + 1);
  snprintf(keyHash, sizeof(keyHash), "wu%dh", slot + 1);

  _prefs.putUChar(keyActive, users[slot].active);
  _prefs.putString(keyUser, users[slot].username);
  _prefs.putString(keySalt, users[slot].passwordSalt);
  _prefs.putString(keyHash, users[slot].passwordHash);
  return true;
}

bool SettingsStore::deleteWebUser(const char* username, String* err) {
  if (!username || !username[0]) {
    if (err) *err = "missing username";
    return false;
  }

  WebUserRecord users[WEB_USER_MAX_COUNT];
  loadWebUsers(users);

  int index = -1;
  uint8_t activeCount = 0;
  for (uint8_t i = 0; i < WEB_USER_MAX_COUNT; i++) {
    if (!users[i].active) continue;
    activeCount++;
    if (strcmp(users[i].username, username) == 0) index = i;
  }

  if (index < 0) {
    if (err) *err = "user not found";
    return false;
  }
  if (activeCount <= 1) {
    if (err) *err = "cannot delete last user";
    return false;
  }

  users[index] = WebUserRecord{};

  char keyActive[8];
  char keyUser[8];
  char keySalt[8];
  char keyHash[8];
  snprintf(keyActive, sizeof(keyActive), "wu%da", index + 1);
  snprintf(keyUser, sizeof(keyUser), "wu%du", index + 1);
  snprintf(keySalt, sizeof(keySalt), "wu%ds", index + 1);
  snprintf(keyHash, sizeof(keyHash), "wu%dh", index + 1);

  _prefs.putUChar(keyActive, 0);
  _prefs.putString(keyUser, "");
  _prefs.putString(keySalt, "");
  _prefs.putString(keyHash, "");
  return true;
}

void SettingsStore::clearWebUsers() {
  for (uint8_t i = 0; i < WEB_USER_MAX_COUNT; i++) {
    char keyActive[8];
    char keyUser[8];
    char keySalt[8];
    char keyHash[8];
    snprintf(keyActive, sizeof(keyActive), "wu%da", i + 1);
    snprintf(keyUser, sizeof(keyUser), "wu%du", i + 1);
    snprintf(keySalt, sizeof(keySalt), "wu%ds", i + 1);
    snprintf(keyHash, sizeof(keyHash), "wu%dh", i + 1);

    _prefs.putUChar(keyActive, 0);
    _prefs.putString(keyUser, "");
    _prefs.putString(keySalt, "");
    _prefs.putString(keyHash, "");
  }
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
  json += "\"warningHoldSec\":"; json += String(s.orangeAlertHoldMs / MS_PER_SECOND); json += ",";
  json += "\"criticalHoldSec\":"; json += String(s.redAlertHoldMs / MS_PER_SECOND); json += ",";
  json += "\"liveEnabled\":"; json += (s.liveEnabled ? "true" : "false"); json += ",";
  json += "\"touchEnabled\":"; json += (s.touchEnabled ? "true" : "false"); json += ",";
  json += "\"dashboardPage\":"; json += String(s.dashboardPage); json += ",";
  json += "\"dashboardFullscreenMask\":"; json += String(s.dashboardFullscreenMask); json += ",";
  json += "\"dashboardPin\":\""; json += sp7json::escape(s.dashboardPin); json += "\",";
  json += "\"homeAssistantToken\":\""; json += sp7json::escape(s.homeAssistantToken); json += "\",";
  json += "\"tz\":\""; json += sp7json::escape(s.tz); json += "\",";
  json += "\"ntpServer\":\""; json += sp7json::escape(s.ntpServer); json += "\",";
  json += "\"ntpSyncMinutes\":"; json += String(s.ntpSyncIntervalMs / MS_PER_MINUTE); json += ",";
  json += "\"hostname\":\""; json += sp7json::escape(s.hostname); json += "\",";
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    json += "\"wifi";
    json += String(i + 1);
    json += "Ssid\":\"";
    json += sp7json::escape(s.wifiCredentials[i].ssid);
    json += "\",";
    json += "\"wifi";
    json += String(i + 1);
    json += "Password\":\"";
    json += sp7json::escape(s.wifiCredentials[i].password);
    json += "\",";
  }
  json += "\"audioSource\":"; json += String(s.audioSource); json += ",";
  json += "\"analogPin\":"; json += String(s.analogPin); json += ",";
  json += "\"analogRmsSamples\":"; json += String(s.analogRmsSamples); json += ",";
  json += "\"audioResponseMode\":"; json += String(s.audioResponseMode); json += ",";
  json += "\"emaAlpha\":"; json += String(s.emaAlpha, 4); json += ",";
  json += "\"peakHoldMs\":"; json += String(s.peakHoldMs); json += ",";
  json += "\"calibrationPointCount\":"; json += String(s.calibrationPointCount); json += ",";
  json += "\"calibrationCaptureSec\":"; json += String(s.calibrationCaptureMs / MS_PER_SECOND); json += ",";
  json += "\"analogBaseOffsetDb\":"; json += String(s.analogBaseOffsetDb, 4); json += ",";
  json += "\"analogExtraOffsetDb\":"; json += String(s.analogExtraOffsetDb, 4); json += ",";
  json += "\"calPointRefDb\":[";
  for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
    if (i) json += ",";
    json += String(s.calPointRefDb[i], 2);
  }
  json += "],";
  json += "\"calPointRawLogRms\":[";
  for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
    if (i) json += ",";
    json += String(s.calPointRawLogRms[i], 4);
  }
  json += "],";
  json += "\"calPointValid\":[";
  for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
    if (i) json += ",";
    json += String(s.calPointValid[i]);
  }
  json += "],";
  json += "\"otaEnabled\":"; json += (s.otaEnabled ? "true" : "false"); json += ",";
  json += "\"otaPort\":"; json += String(s.otaPort); json += ",";
  json += "\"otaHostname\":\""; json += sp7json::escape(s.otaHostname); json += "\",";
  json += "\"otaPassword\":\""; json += sp7json::escape(s.otaPassword); json += "\",";
  json += "\"mqttEnabled\":"; json += (s.mqttEnabled ? "true" : "false"); json += ",";
  json += "\"mqttHost\":\""; json += sp7json::escape(s.mqttHost); json += "\",";
  json += "\"mqttPort\":"; json += String(s.mqttPort); json += ",";
  json += "\"mqttUsername\":\""; json += sp7json::escape(s.mqttUsername); json += "\",";
  json += "\"mqttPassword\":\""; json += sp7json::escape(s.mqttPassword); json += "\",";
  json += "\"mqttClientId\":\""; json += sp7json::escape(s.mqttClientId); json += "\",";
  json += "\"mqttBaseTopic\":\""; json += sp7json::escape(s.mqttBaseTopic); json += "\",";
  json += "\"mqttPublishPeriodMs\":"; json += String(s.mqttPublishPeriodMs); json += ",";
  json += "\"mqttRetain\":"; json += (s.mqttRetain ? "true" : "false");
  json += "}";
  return json;
}

bool SettingsStore::importJson(SettingsV1& s, const String& json, String* err) {
  SettingsV1 next = s;

  next.backlight = (uint8_t)sp7json::parseInt(json, "backlight", next.backlight);
  next.th.greenMax = (uint8_t)sp7json::parseInt(json, "greenMax", next.th.greenMax);
  next.th.orangeMax = (uint8_t)sp7json::parseInt(json, "orangeMax", next.th.orangeMax);
  next.historyMinutes = (uint8_t)sp7json::parseInt(json, "historyMinutes", next.historyMinutes);
  next.orangeAlertHoldMs = (uint32_t)sp7json::parseInt(json, "warningHoldSec", (int)(next.orangeAlertHoldMs / MS_PER_SECOND)) * MS_PER_SECOND;
  next.redAlertHoldMs = (uint32_t)sp7json::parseInt(json, "criticalHoldSec", (int)(next.redAlertHoldMs / MS_PER_SECOND)) * MS_PER_SECOND;
  next.liveEnabled = sp7json::parseBool(json, "liveEnabled", next.liveEnabled != 0) ? LIVE_ENABLED : LIVE_DISABLED;
  next.touchEnabled = sp7json::parseBool(json, "touchEnabled", next.touchEnabled != 0) ? 1 : 0;
  next.dashboardPage = normalizedDashboardPage((uint8_t)sp7json::parseInt(json, "dashboardPage", next.dashboardPage));
  next.dashboardFullscreenMask = normalizedDashboardFullscreenMask(
    (uint8_t)sp7json::parseInt(json, "dashboardFullscreenMask", next.dashboardFullscreenMask)
  );
  String dashboardPin = sp7json::parseString(json, "dashboardPin", String(next.dashboardPin));
  String homeAssistantToken = sp7json::parseString(json, "homeAssistantToken", String(next.homeAssistantToken));

  String tz = sp7json::parseString(json, "tz", String(next.tz));
  String ntp = sp7json::parseString(json, "ntpServer", String(next.ntpServer));
  String hostname = sp7json::parseString(json, "hostname", String(next.hostname));
  String wifiSsids[WIFI_CREDENTIAL_MAX_COUNT];
  String wifiPasswords[WIFI_CREDENTIAL_MAX_COUNT];
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    const String ssidKey = String("wifi") + String(i + 1) + "Ssid";
    const String passwordKey = String("wifi") + String(i + 1) + "Password";
    wifiSsids[i] = sp7json::parseString(json, ssidKey.c_str(), String(next.wifiCredentials[i].ssid));
    wifiPasswords[i] = sp7json::parseString(json, passwordKey.c_str(), String(next.wifiCredentials[i].password));
  }
  if (!sp7json::safeCopy(next.dashboardPin, sizeof(next.dashboardPin), dashboardPin)) {
    if (err) *err = "dashboardPin too long";
    return false;
  }
  if (!sp7json::safeCopy(next.homeAssistantToken, sizeof(next.homeAssistantToken), homeAssistantToken)) {
    if (err) *err = "homeAssistantToken too long";
    return false;
  }
  if (!sp7json::safeCopy(next.tz, sizeof(next.tz), tz)) {
    if (err) *err = "tz too long";
    return false;
  }
  if (!sp7json::safeCopy(next.ntpServer, sizeof(next.ntpServer), ntp)) {
    if (err) *err = "ntpServer too long";
    return false;
  }
  if (!sp7json::safeCopy(next.hostname, sizeof(next.hostname), hostname)) {
    if (err) *err = "hostname too long";
    return false;
  }
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    if (!sp7json::safeCopy(next.wifiCredentials[i].ssid, sizeof(next.wifiCredentials[i].ssid), wifiSsids[i])) {
      if (err) *err = String("wifi") + String(i + 1) + "Ssid too long";
      return false;
    }
    if (!sp7json::safeCopy(next.wifiCredentials[i].password, sizeof(next.wifiCredentials[i].password), wifiPasswords[i])) {
      if (err) *err = String("wifi") + String(i + 1) + "Password too long";
      return false;
    }
  }
  next.ntpSyncIntervalMs = (uint32_t)sp7json::parseInt(json, "ntpSyncMinutes", (int)(next.ntpSyncIntervalMs / MS_PER_MINUTE)) * MS_PER_MINUTE;

  next.audioSource = (uint8_t)sp7json::parseInt(json, "audioSource", next.audioSource);
  next.analogPin = (uint8_t)sp7json::parseInt(json, "analogPin", next.analogPin);
  next.analogRmsSamples = (uint16_t)sp7json::parseInt(json, "analogRmsSamples", next.analogRmsSamples);
  next.audioResponseMode = (uint8_t)sp7json::parseInt(json, "audioResponseMode", next.audioResponseMode);
  next.emaAlpha = sp7json::parseFloat(json, "emaAlpha", next.emaAlpha);
  next.peakHoldMs = (uint32_t)sp7json::parseInt(json, "peakHoldMs", next.peakHoldMs);
  next.calibrationPointCount = (uint8_t)sp7json::parseInt(json, "calibrationPointCount", next.calibrationPointCount);
  next.calibrationCaptureMs = (uint32_t)sp7json::parseInt(json, "calibrationCaptureSec", (int)(next.calibrationCaptureMs / MS_PER_SECOND)) * MS_PER_SECOND;
  next.analogBaseOffsetDb = sp7json::parseFloat(json, "analogBaseOffsetDb", next.analogBaseOffsetDb);
  next.analogExtraOffsetDb = sp7json::parseFloat(json, "analogExtraOffsetDb", next.analogExtraOffsetDb);

  float calRef[CALIBRATION_POINT_MAX];
  memcpy(calRef, next.calPointRefDb, sizeof(calRef));
  if (sp7json::parseFloatArray(json, "calPointRefDb", calRef)) {
    for (int i = 0; i < CALIBRATION_POINT_MAX; i++) next.calPointRefDb[i] = calRef[i];
  }
  float calRaw[CALIBRATION_POINT_MAX];
  memcpy(calRaw, next.calPointRawLogRms, sizeof(calRaw));
  if (sp7json::parseFloatArray(json, "calPointRawLogRms", calRaw)) {
    for (int i = 0; i < CALIBRATION_POINT_MAX; i++) next.calPointRawLogRms[i] = calRaw[i];
  }
  uint8_t calValid[CALIBRATION_POINT_MAX];
  memcpy(calValid, next.calPointValid, sizeof(calValid));
  if (sp7json::parseU8Array(json, "calPointValid", calValid)) {
    for (int i = 0; i < CALIBRATION_POINT_MAX; i++) next.calPointValid[i] = calValid[i];
  }

  next.otaEnabled = sp7json::parseBool(json, "otaEnabled", next.otaEnabled != 0) ? 1 : 0;
  next.otaPort = (uint16_t)sp7json::parseInt(json, "otaPort", next.otaPort);
  String otaHostname = sp7json::parseString(json, "otaHostname", String(next.otaHostname));
  String otaPassword = sp7json::parseString(json, "otaPassword", String(next.otaPassword));
  if (!sp7json::safeCopy(next.otaHostname, sizeof(next.otaHostname), otaHostname)) {
    if (err) *err = "otaHostname too long";
    return false;
  }
  if (!sp7json::safeCopy(next.otaPassword, sizeof(next.otaPassword), otaPassword)) {
    if (err) *err = "otaPassword too long";
    return false;
  }

  next.mqttEnabled = sp7json::parseBool(json, "mqttEnabled", next.mqttEnabled != 0) ? 1 : 0;
  next.mqttPort = (uint16_t)sp7json::parseInt(json, "mqttPort", next.mqttPort);
  next.mqttPublishPeriodMs = (uint16_t)sp7json::parseInt(json, "mqttPublishPeriodMs", next.mqttPublishPeriodMs);
  next.mqttRetain = sp7json::parseBool(json, "mqttRetain", next.mqttRetain != 0) ? 1 : 0;

  String mqttHost = sp7json::parseString(json, "mqttHost", String(next.mqttHost));
  String mqttUsername = sp7json::parseString(json, "mqttUsername", String(next.mqttUsername));
  String mqttPassword = sp7json::parseString(json, "mqttPassword", String(next.mqttPassword));
  String mqttClientId = sp7json::parseString(json, "mqttClientId", String(next.mqttClientId));
  String mqttBaseTopic = sp7json::parseString(json, "mqttBaseTopic", String(next.mqttBaseTopic));

  if (!sp7json::safeCopy(next.mqttHost, sizeof(next.mqttHost), mqttHost)) {
    if (err) *err = "mqttHost too long";
    return false;
  }
  if (!sp7json::safeCopy(next.mqttUsername, sizeof(next.mqttUsername), mqttUsername)) {
    if (err) *err = "mqttUsername too long";
    return false;
  }
  if (!sp7json::safeCopy(next.mqttPassword, sizeof(next.mqttPassword), mqttPassword)) {
    if (err) *err = "mqttPassword too long";
    return false;
  }
  if (!sp7json::safeCopy(next.mqttClientId, sizeof(next.mqttClientId), mqttClientId)) {
    if (err) *err = "mqttClientId too long";
    return false;
  }
  if (!sp7json::safeCopy(next.mqttBaseTopic, sizeof(next.mqttBaseTopic), mqttBaseTopic)) {
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
    s.liveEnabled = def.liveEnabled;
    s.touchEnabled = def.touchEnabled;
    s.dashboardPage = def.dashboardPage;
    s.dashboardFullscreenMask = def.dashboardFullscreenMask;
    s.audioResponseMode = def.audioResponseMode;
  } else if (scope == "security") {
    memcpy(s.dashboardPin, def.dashboardPin, sizeof(s.dashboardPin));
    memcpy(s.homeAssistantToken, def.homeAssistantToken, sizeof(s.homeAssistantToken));
  } else if (scope == "time") {
    memcpy(s.tz, def.tz, sizeof(s.tz));
    memcpy(s.ntpServer, def.ntpServer, sizeof(s.ntpServer));
    s.ntpSyncIntervalMs = def.ntpSyncIntervalMs;
    memcpy(s.hostname, def.hostname, sizeof(s.hostname));
  } else if (scope == "wifi") {
    memcpy(s.wifiCredentials, def.wifiCredentials, sizeof(s.wifiCredentials));
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
    s.calibrationPointCount = def.calibrationPointCount;
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
