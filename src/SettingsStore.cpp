#include "SettingsStore.h"

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

  if (out.backlight > 100) out.backlight = 100;
  if (out.th.greenMax > 100) out.th.greenMax = 100;
  if (out.th.orangeMax > 100) out.th.orangeMax = 100;
  if (out.th.orangeMax < out.th.greenMax) out.th.orangeMax = out.th.greenMax;

  if (out.historyMinutes < 1) out.historyMinutes = 1;
  if (out.historyMinutes > 60) out.historyMinutes = 60;

  if (out.orangeAlertHoldMs > 60000UL) out.orangeAlertHoldMs = 60000UL;
  if (out.redAlertHoldMs > 60000UL) out.redAlertHoldMs = 60000UL;

  if (out.analogRmsSamples < 32) out.analogRmsSamples = 32;
  if (out.analogRmsSamples > 1024) out.analogRmsSamples = 1024;
  if (out.audioResponseMode > 1) out.audioResponseMode = 0;

  if (out.emaAlpha < 0.01f) out.emaAlpha = 0.01f;
  if (out.emaAlpha > 0.95f) out.emaAlpha = 0.95f;

  if (out.peakHoldMs < 500) out.peakHoldMs = 500;
  if (out.peakHoldMs > 30000) out.peakHoldMs = 30000;

  if (out.ntpSyncIntervalMs < 60000UL) out.ntpSyncIntervalMs = 60000UL;
  if (out.ntpSyncIntervalMs > 86400000UL) out.ntpSyncIntervalMs = 86400000UL;
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
