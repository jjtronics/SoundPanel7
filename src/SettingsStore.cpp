#include "SettingsStore.h"
#include "JsonHelpers.h"

#include <ctime>
#include <cstring>
#include <math.h>
#include <stdlib.h>

#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

namespace {
static constexpr const char* kEncryptedSecretPrefix = "enc:v1:";
static constexpr const char* kPinHashPrefix = "h1:";
static constexpr size_t kEncryptedSecretNonceLength = 12;
static constexpr size_t kEncryptedSecretTagLength = 16;
static constexpr size_t kSecretKeyLength = 32;

void applyAudioBoardProfileDefaults(SettingsV1& s) {
  s.analogPin = SOUNDPANEL7_DEFAULT_ANALOG_PIN;
  s.pdmClkPin = SOUNDPANEL7_DEFAULT_PDM_CLK_PIN;
  s.pdmDataPin = SOUNDPANEL7_DEFAULT_PDM_DATA_PIN;
  s.inmp441BclkPin = SOUNDPANEL7_DEFAULT_INMP441_BCLK_PIN;
  s.inmp441WsPin = SOUNDPANEL7_DEFAULT_INMP441_WS_PIN;
  s.inmp441DataPin = SOUNDPANEL7_DEFAULT_INMP441_DATA_PIN;
}

bool secureEqualsRaw(const char* a, const char* b) {
  const size_t lenA = a ? strlen(a) : 0;
  const size_t lenB = b ? strlen(b) : 0;
  const size_t len = lenA > lenB ? lenA : lenB;
  uint8_t diff = (uint8_t)(lenA ^ lenB);
  for (size_t i = 0; i < len; i++) {
    const uint8_t ca = (a && i < lenA) ? (uint8_t)a[i] : 0U;
    const uint8_t cb = (b && i < lenB) ? (uint8_t)b[i] : 0U;
    diff |= (uint8_t)(ca ^ cb);
  }
  return diff == 0;
}

bool isHexChar(char c) {
  return (c >= '0' && c <= '9')
    || (c >= 'a' && c <= 'f')
    || (c >= 'A' && c <= 'F');
}

uint8_t hexValue(char c) {
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t)(10 + (c - 'a'));
  if (c >= 'A' && c <= 'F') return (uint8_t)(10 + (c - 'A'));
  return 0;
}

void appendHex(String& out, const uint8_t* data, size_t len) {
  static const char kHexChars[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    out += kHexChars[(data[i] >> 4) & 0x0F];
    out += kHexChars[data[i] & 0x0F];
  }
}

bool decodeHex(const char* input, uint8_t* out, size_t outLen) {
  if (!input || !out) return false;
  const size_t inputLen = strlen(input);
  if (inputLen != outLen * 2U) return false;
  for (size_t i = 0; i < outLen; i++) {
    const char hi = input[i * 2U];
    const char lo = input[(i * 2U) + 1U];
    if (!isHexChar(hi) || !isHexChar(lo)) return false;
    out[i] = (uint8_t)((hexValue(hi) << 4) | hexValue(lo));
  }
  return true;
}

String randomHex(size_t hexChars) {
  static const char kHexChars[] = "0123456789abcdef";
  String out;
  out.reserve(hexChars);
  while (out.length() < hexChars) {
    const uint32_t value = esp_random();
    for (int shift = 28; shift >= 0 && out.length() < hexChars; shift -= 4) {
      out += kHexChars[(value >> shift) & 0x0F];
    }
  }
  return out;
}

String hashPinValue(const char* pin, const char* saltHex) {
  uint8_t digest[32] = {0};
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);

  auto runRound = [&](bool firstRound) {
    mbedtls_sha256_starts(&ctx, 0);
    if (!firstRound) mbedtls_sha256_update(&ctx, digest, sizeof(digest));
    static const char kPinScope[] = "soundpanel7-pin";
    mbedtls_sha256_update(&ctx, (const uint8_t*)kPinScope, strlen(kPinScope));
    if (saltHex) mbedtls_sha256_update(&ctx, (const uint8_t*)saltHex, strlen(saltHex));
    if (pin) mbedtls_sha256_update(&ctx, (const uint8_t*)pin, strlen(pin));
    mbedtls_sha256_finish(&ctx, digest);
  };

  runRound(true);
  for (uint16_t i = 1; i < PIN_HASH_ROUNDS; i++) runRound(false);
  mbedtls_sha256_free(&ctx);

  String out;
  out.reserve(PIN_HASH_LENGTH);
  appendHex(out, digest, sizeof(digest));
  return out;
}

bool isPinHashRecordRaw(const char* value) {
  if (!value) return false;
  const size_t prefixLen = strlen(kPinHashPrefix);
  const size_t expectedLen = prefixLen + PIN_HASH_SALT_LENGTH + 1U + PIN_HASH_LENGTH;
  if (strncmp(value, kPinHashPrefix, prefixLen) != 0) return false;
  if (strlen(value) != expectedLen) return false;
  if (value[prefixLen + PIN_HASH_SALT_LENGTH] != ':') return false;
  for (size_t i = prefixLen; i < prefixLen + PIN_HASH_SALT_LENGTH; i++) {
    if (!isHexChar(value[i])) return false;
  }
  for (size_t i = prefixLen + PIN_HASH_SALT_LENGTH + 1U; i < expectedLen; i++) {
    if (!isHexChar(value[i])) return false;
  }
  return true;
}
}

bool pinCodeIsConfigured(const char* pin) {
  return pinCodeIsValid(pin) || isPinHashRecordRaw(pin);
}

bool pinCodeMatches(const char* storedPin, const char* candidate) {
  if (!storedPin || !storedPin[0] || !candidate || !candidate[0]) return false;
  if (!pinCodeIsValid(candidate)) return false;
  if (isPinHashRecordRaw(storedPin)) {
    const size_t prefixLen = strlen(kPinHashPrefix);
    char salt[PIN_HASH_SALT_LENGTH + 1] = {0};
    char expectedHash[PIN_HASH_LENGTH + 1] = {0};
    memcpy(salt, storedPin + prefixLen, PIN_HASH_SALT_LENGTH);
    memcpy(expectedHash, storedPin + prefixLen + PIN_HASH_SALT_LENGTH + 1U, PIN_HASH_LENGTH);
    const String computed = hashPinValue(candidate, salt);
    return secureEqualsRaw(expectedHash, computed.c_str());
  }
  return secureEqualsRaw(storedPin, candidate);
}

bool encodePinCode(const char* pin, char* out, size_t outSize) {
  if (!out || outSize == 0) return false;
  out[0] = '\0';
  if (!pin || !pin[0]) return true;
  if (isPinHashRecordRaw(pin)) {
    const size_t len = strlen(pin);
    if (len >= outSize) return false;
    memcpy(out, pin, len + 1U);
    return true;
  }
  if (!pinCodeIsValid(pin)) return false;

  const String salt = randomHex(PIN_HASH_SALT_LENGTH);
  const String hash = hashPinValue(pin, salt.c_str());
  const String encoded = String(kPinHashPrefix) + salt + ":" + hash;
  if (encoded.length() >= outSize) return false;
  memcpy(out, encoded.c_str(), encoded.length() + 1U);
  return true;
}

bool SettingsStore::begin(const char* nvsNamespace) {
  _ns = nvsNamespace ? nvsNamespace : "sp7";
  return _prefs.begin(_ns.c_str(), false);
}

bool SettingsStore::deriveSecretKey(uint8_t (&outKey)[32]) const {
  uint8_t baseMac[6] = {0};
  if (esp_efuse_mac_get_default(baseMac) != ESP_OK) return false;

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  static const char kKeyScope[] = "soundpanel7-secret-key-v1";
  mbedtls_sha256_update(&ctx, (const uint8_t*)kKeyScope, strlen(kKeyScope));
  if (_ns.length()) {
    mbedtls_sha256_update(&ctx, (const uint8_t*)_ns.c_str(), _ns.length());
  }
  mbedtls_sha256_update(&ctx, baseMac, sizeof(baseMac));
  mbedtls_sha256_finish(&ctx, outKey);
  mbedtls_sha256_free(&ctx);
  return true;
}

bool SettingsStore::encryptSecret(const char* purpose, const char* plaintext, String& out) const {
  out = "";
  if (!plaintext || !plaintext[0]) return true;

  const size_t len = strlen(plaintext);
  uint8_t key[kSecretKeyLength] = {0};
  if (!deriveSecretKey(key)) return false;

  uint8_t nonce[kEncryptedSecretNonceLength] = {0};
  for (size_t offset = 0; offset < sizeof(nonce); offset += sizeof(uint32_t)) {
    const uint32_t value = esp_random();
    const size_t chunk = min(sizeof(uint32_t), sizeof(nonce) - offset);
    memcpy(nonce + offset, &value, chunk);
  }

  uint8_t tag[kEncryptedSecretTagLength] = {0};
  uint8_t* cipher = (uint8_t*)malloc(len ? len : 1U);
  if (!cipher) return false;

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, sizeof(key) * 8U);
  if (rc == 0) {
    rc = mbedtls_gcm_crypt_and_tag(&ctx,
                                   MBEDTLS_GCM_ENCRYPT,
                                   len,
                                   nonce,
                                   sizeof(nonce),
                                   (const uint8_t*)(purpose ? purpose : ""),
                                   purpose ? strlen(purpose) : 0U,
                                   (const uint8_t*)plaintext,
                                   cipher,
                                   sizeof(tag),
                                   tag);
  }
  mbedtls_gcm_free(&ctx);
  if (rc != 0) {
    free(cipher);
    return false;
  }

  out.reserve(strlen(kEncryptedSecretPrefix) + ((sizeof(nonce) + sizeof(tag) + len) * 2U));
  out += kEncryptedSecretPrefix;
  appendHex(out, nonce, sizeof(nonce));
  appendHex(out, tag, sizeof(tag));
  appendHex(out, cipher, len);
  free(cipher);
  return true;
}

bool SettingsStore::decryptSecret(const char* purpose, const char* stored, char* out, size_t outSize) const {
  if (!out || outSize == 0) return false;
  out[0] = '\0';
  if (!stored || !stored[0]) return true;
  if (!isEncryptedSecretRecord(stored)) {
    return sp7json::safeCopy(out, outSize, String(stored));
  }

  const char* hex = stored + strlen(kEncryptedSecretPrefix);
  const size_t hexLen = strlen(hex);
  if ((hexLen & 1U) != 0) return false;
  const size_t blobLen = hexLen / 2U;
  if (blobLen < (kEncryptedSecretNonceLength + kEncryptedSecretTagLength)) return false;

  uint8_t* blob = (uint8_t*)malloc(blobLen ? blobLen : 1U);
  uint8_t* plain = (uint8_t*)malloc((blobLen - kEncryptedSecretNonceLength - kEncryptedSecretTagLength) + 1U);
  if (!blob || !plain) {
    if (blob) free(blob);
    if (plain) free(plain);
    return false;
  }
  if (!decodeHex(hex, blob, blobLen)) {
    free(blob);
    free(plain);
    return false;
  }

  const size_t cipherLen = blobLen - kEncryptedSecretNonceLength - kEncryptedSecretTagLength;
  if ((cipherLen + 1U) > outSize) {
    free(blob);
    free(plain);
    return false;
  }

  const uint8_t* nonce = blob;
  const uint8_t* tag = blob + kEncryptedSecretNonceLength;
  const uint8_t* cipher = blob + kEncryptedSecretNonceLength + kEncryptedSecretTagLength;
  uint8_t key[kSecretKeyLength] = {0};
  if (!deriveSecretKey(key)) {
    free(blob);
    free(plain);
    return false;
  }

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);
  int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, sizeof(key) * 8U);
  if (rc == 0) {
    rc = mbedtls_gcm_auth_decrypt(&ctx,
                                  cipherLen,
                                  nonce,
                                  kEncryptedSecretNonceLength,
                                  (const uint8_t*)(purpose ? purpose : ""),
                                  purpose ? strlen(purpose) : 0U,
                                  tag,
                                  kEncryptedSecretTagLength,
                                  cipher,
                                  plain);
  }
  mbedtls_gcm_free(&ctx);
  if (rc != 0) {
    free(blob);
    free(plain);
    return false;
  }

  plain[cipherLen] = '\0';
  memcpy(out, plain, cipherLen + 1U);
  free(blob);
  free(plain);
  return true;
}

bool SettingsStore::loadSecret(const char* key, const char* purpose, char* out, size_t outSize, bool* migrated) {
  if (migrated) *migrated = false;
  String stored = _prefs.getString(key, "");
  if (!stored.length()) {
    if (outSize) out[0] = '\0';
    return true;
  }
  if (isEncryptedSecretRecord(stored.c_str())) {
    return decryptSecret(purpose, stored.c_str(), out, outSize);
  }
  if (!sp7json::safeCopy(out, outSize, stored)) return false;
  if (migrated) *migrated = true;
  return true;
}

void SettingsStore::saveSecret(const char* key, const char* purpose, const char* value) {
  String encrypted;
  if (!encryptSecret(purpose, value, encrypted)) {
    Serial0.printf("[SET] Failed to encrypt secret '%s'\n", key ? key : "?");
    return;
  }
  _prefs.putString(key, encrypted);
}

bool SettingsStore::isEncryptedSecretRecord(const char* value) {
  return value && strncmp(value, kEncryptedSecretPrefix, strlen(kEncryptedSecretPrefix)) == 0;
}

bool SettingsStore::isPinHashRecord(const char* value) {
  return isPinHashRecordRaw(value);
}

bool SettingsStore::normalizePinStorage(char* storage, size_t storageSize) {
  if (!storage || storageSize == 0) return false;
  storage[storageSize - 1] = '\0';
  if (!storage[0]) return true;
  if (isPinHashRecord(storage)) return true;
  if (!pinCodeIsValid(storage)) {
    storage[0] = '\0';
    return false;
  }

  char encoded[PIN_STORAGE_MAX_LENGTH + 1] = {0};
  if (!encodePinCode(storage, encoded, sizeof(encoded))) {
    storage[0] = '\0';
    return false;
  }
  if (strlen(encoded) >= storageSize) {
    storage[0] = '\0';
    return false;
  }
  memcpy(storage, encoded, strlen(encoded) + 1U);
  return true;
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
  const bool pinMigrated = out.dashboardPin[0]
    && !isPinHashRecord(out.dashboardPin)
    && normalizePinStorage(out.dashboardPin, sizeof(out.dashboardPin));
  bool haMigrated = false;
  if (!loadSecret("ha_tok", "ha_token", out.homeAssistantToken, sizeof(out.homeAssistantToken), &haMigrated)) {
    out.homeAssistantToken[0] = '\0';
  }

  _prefs.getString("tz", out.tz, sizeof(out.tz));
  _prefs.getString("ntp", out.ntpServer, sizeof(out.ntpServer));
  out.ntpSyncIntervalMs = _prefs.getUInt("ntp_ms", out.ntpSyncIntervalMs);
  _prefs.getString("hn", out.hostname, sizeof(out.hostname));
  bool wifiMigrated[WIFI_CREDENTIAL_MAX_COUNT] = {false, false, false, false};
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    char keySsid[8];
    char keyPass[8];
    char purpose[8];
    snprintf(keySsid, sizeof(keySsid), "wf%us", (unsigned)(i + 1));
    snprintf(keyPass, sizeof(keyPass), "wf%up", (unsigned)(i + 1));
    snprintf(purpose, sizeof(purpose), "wifi%u", (unsigned)(i + 1));
    _prefs.getString(keySsid, out.wifiCredentials[i].ssid, sizeof(out.wifiCredentials[i].ssid));
    if (!loadSecret(keyPass, purpose, out.wifiCredentials[i].password, sizeof(out.wifiCredentials[i].password), &wifiMigrated[i])) {
      out.wifiCredentials[i].password[0] = '\0';
    }
  }

  out.otaEnabled = (uint8_t)_prefs.getUChar("ota_en", out.otaEnabled);
  out.otaPort = (uint16_t)_prefs.getUShort("ota_pt", out.otaPort);
  _prefs.getString("ota_hn", out.otaHostname, sizeof(out.otaHostname));
  bool otaMigrated = false;
  if (!loadSecret("ota_pw", "ota_password", out.otaPassword, sizeof(out.otaPassword), &otaMigrated)) {
    out.otaPassword[0] = '\0';
  }

  out.audioSource        = (uint8_t)_prefs.getUChar("a_src", out.audioSource);
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
  bool mqttMigrated = false;
  if (!loadSecret("mq_pwd", "mqtt_password", out.mqttPassword, sizeof(out.mqttPassword), &mqttMigrated)) {
    out.mqttPassword[0] = '\0';
  }
  _prefs.getString("mq_cid", out.mqttClientId, sizeof(out.mqttClientId));
  _prefs.getString("mq_base", out.mqttBaseTopic, sizeof(out.mqttBaseTopic));
  out.mqttPublishPeriodMs = (uint16_t)_prefs.getUShort("mq_pubms", out.mqttPublishPeriodMs);
  out.mqttRetain = (uint8_t)_prefs.getUChar("mq_ret", out.mqttRetain);

  out.notifyOnWarning = (uint8_t)_prefs.getUChar("n_lvl", out.notifyOnWarning);
  out.notifyOnRecovery = (uint8_t)_prefs.getUChar("n_rec", out.notifyOnRecovery);
  out.slackEnabled = (uint8_t)_prefs.getUChar("n_s_en", out.slackEnabled);
  bool slackMigrated = false;
  if (!loadSecret("n_s_url", "slack_webhook", out.slackWebhookUrl, sizeof(out.slackWebhookUrl), &slackMigrated)) {
    out.slackWebhookUrl[0] = '\0';
  }
  _prefs.getString("n_s_ch", out.slackChannel, sizeof(out.slackChannel));
  out.telegramEnabled = (uint8_t)_prefs.getUChar("n_t_en", out.telegramEnabled);
  bool telegramMigrated = false;
  if (!loadSecret("n_t_tok", "telegram_token", out.telegramBotToken, sizeof(out.telegramBotToken), &telegramMigrated)) {
    out.telegramBotToken[0] = '\0';
  }
  _prefs.getString("n_t_chat", out.telegramChatId, sizeof(out.telegramChatId));
  out.whatsappEnabled = (uint8_t)_prefs.getUChar("n_w_en", out.whatsappEnabled);
  bool whatsappMigrated = false;
  if (!loadSecret("n_w_tok", "whatsapp_token", out.whatsappAccessToken, sizeof(out.whatsappAccessToken), &whatsappMigrated)) {
    out.whatsappAccessToken[0] = '\0';
  }
  _prefs.getString("n_w_pid", out.whatsappPhoneNumberId, sizeof(out.whatsappPhoneNumberId));
  _prefs.getString("n_w_to", out.whatsappRecipient, sizeof(out.whatsappRecipient));
  _prefs.getString("n_w_ver", out.whatsappApiVersion, sizeof(out.whatsappApiVersion));

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

  if (pinMigrated) _prefs.putString("ui_pin", out.dashboardPin);
  if (haMigrated) saveSecret("ha_tok", "ha_token", out.homeAssistantToken);
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    if (!wifiMigrated[i]) continue;
    char keyPass[8];
    char purpose[8];
    snprintf(keyPass, sizeof(keyPass), "wf%up", (unsigned)(i + 1));
    snprintf(purpose, sizeof(purpose), "wifi%u", (unsigned)(i + 1));
    saveSecret(keyPass, purpose, out.wifiCredentials[i].password);
  }
  if (otaMigrated) saveSecret("ota_pw", "ota_password", out.otaPassword);
  if (mqttMigrated) saveSecret("mq_pwd", "mqtt_password", out.mqttPassword);
  if (slackMigrated) saveSecret("n_s_url", "slack_webhook", out.slackWebhookUrl);
  if (telegramMigrated) saveSecret("n_t_tok", "telegram_token", out.telegramBotToken);
  if (whatsappMigrated) saveSecret("n_w_tok", "whatsapp_token", out.whatsappAccessToken);
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
  char dashboardPin[sizeof(s.dashboardPin)] = {0};
  if (sp7json::safeCopy(dashboardPin, sizeof(dashboardPin), String(s.dashboardPin))
      && normalizePinStorage(dashboardPin, sizeof(dashboardPin))) {
    _prefs.putString("ui_pin", dashboardPin);
  } else {
    _prefs.putString("ui_pin", "");
  }
  saveSecret("ha_tok", "ha_token", s.homeAssistantToken);

  _prefs.putString("tz", s.tz);
  _prefs.putString("ntp", s.ntpServer);
  _prefs.putUInt("ntp_ms", s.ntpSyncIntervalMs);
  _prefs.putString("hn", s.hostname);
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    char keySsid[8];
    char keyPass[8];
    char purpose[8];
    snprintf(keySsid, sizeof(keySsid), "wf%us", (unsigned)(i + 1));
    snprintf(keyPass, sizeof(keyPass), "wf%up", (unsigned)(i + 1));
    snprintf(purpose, sizeof(purpose), "wifi%u", (unsigned)(i + 1));
    _prefs.putString(keySsid, s.wifiCredentials[i].ssid);
    saveSecret(keyPass, purpose, s.wifiCredentials[i].password);
  }

  _prefs.putUChar("ota_en", s.otaEnabled);
  _prefs.putUShort("ota_pt", s.otaPort);
  _prefs.putString("ota_hn", s.otaHostname);
  saveSecret("ota_pw", "ota_password", s.otaPassword);

  _prefs.putUChar("a_src", s.audioSource);
  _prefs.remove("a_pin");
  _prefs.remove("a_pdmck");
  _prefs.remove("a_pdmdt");
  _prefs.remove("a_i2sbk");
  _prefs.remove("a_i2sws");
  _prefs.remove("a_i2sdt");
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
  saveSecret("mq_pwd", "mqtt_password", s.mqttPassword);
  _prefs.putString("mq_cid", s.mqttClientId);
  _prefs.putString("mq_base", s.mqttBaseTopic);
  _prefs.putUShort("mq_pubms", s.mqttPublishPeriodMs);
  _prefs.putUChar("mq_ret", s.mqttRetain);

  _prefs.putUChar("n_lvl", s.notifyOnWarning);
  _prefs.putUChar("n_rec", s.notifyOnRecovery);
  _prefs.putUChar("n_s_en", s.slackEnabled);
  saveSecret("n_s_url", "slack_webhook", s.slackWebhookUrl);
  _prefs.putString("n_s_ch", s.slackChannel);
  _prefs.putUChar("n_t_en", s.telegramEnabled);
  saveSecret("n_t_tok", "telegram_token", s.telegramBotToken);
  _prefs.putString("n_t_chat", s.telegramChatId);
  _prefs.putUChar("n_w_en", s.whatsappEnabled);
  saveSecret("n_w_tok", "whatsapp_token", s.whatsappAccessToken);
  _prefs.putString("n_w_pid", s.whatsappPhoneNumberId);
  _prefs.putString("n_w_to", s.whatsappRecipient);
  _prefs.putString("n_w_ver", s.whatsappApiVersion);

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
  if (s.dashboardPin[0] && !pinCodeIsConfigured(s.dashboardPin)) {
    s.dashboardPin[0] = '\0';
  }
  s.homeAssistantToken[sizeof(s.homeAssistantToken) - 1] = '\0';

  applyAudioBoardProfileDefaults(s);
  if (s.audioSource > 3) s.audioSource = 1;
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
  s.notifyOnWarning = s.notifyOnWarning == ALERT_NOTIFY_LEVEL_WARNING ? ALERT_NOTIFY_LEVEL_WARNING : ALERT_NOTIFY_LEVEL_CRITICAL;
  s.notifyOnRecovery = s.notifyOnRecovery ? 1 : 0;
  s.slackEnabled = s.slackEnabled ? 1 : 0;
  s.telegramEnabled = s.telegramEnabled ? 1 : 0;
  s.whatsappEnabled = s.whatsappEnabled ? 1 : 0;
  s.slackWebhookUrl[sizeof(s.slackWebhookUrl) - 1] = '\0';
  s.slackChannel[sizeof(s.slackChannel) - 1] = '\0';
  s.telegramBotToken[sizeof(s.telegramBotToken) - 1] = '\0';
  s.telegramChatId[sizeof(s.telegramChatId) - 1] = '\0';
  s.whatsappAccessToken[sizeof(s.whatsappAccessToken) - 1] = '\0';
  s.whatsappPhoneNumberId[sizeof(s.whatsappPhoneNumberId) - 1] = '\0';
  s.whatsappRecipient[sizeof(s.whatsappRecipient) - 1] = '\0';
  s.whatsappApiVersion[sizeof(s.whatsappApiVersion) - 1] = '\0';
  if (!strlen(s.whatsappApiVersion)) {
    strncpy(s.whatsappApiVersion, "v22.0", sizeof(s.whatsappApiVersion) - 1);
    s.whatsappApiVersion[sizeof(s.whatsappApiVersion) - 1] = '\0';
  }

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

String SettingsStore::exportJson(const SettingsV1& s, SecretExportMode secretMode) const {
  const bool includeSecrets = secretMode != EXPORT_SECRETS_OMIT;
  const bool clearSecrets = secretMode == EXPORT_SECRETS_CLEAR;
  String json;
  json.reserve(includeSecrets ? 4096 : 3072);
  json += "{";
  json += "\"type\":\"soundpanel7-config\",";
  json += "\"version\":"; json += String(SETTINGS_VERSION); json += ",";
  json += "\"secretMode\":\"";
  json += clearSecrets ? "clear" : (includeSecrets ? "device-encrypted" : "omitted");
  json += "\",";
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
  if (includeSecrets) {
    char dashboardPin[sizeof(s.dashboardPin)] = {0};
    if (sp7json::safeCopy(dashboardPin, sizeof(dashboardPin), String(s.dashboardPin))) {
      normalizePinStorage(dashboardPin, sizeof(dashboardPin));
    }
    sp7json::appendEscapedField(json, "dashboardPin", dashboardPin);
    if (clearSecrets) {
      sp7json::appendEscapedField(json, "homeAssistantToken", s.homeAssistantToken);
    } else {
      String homeAssistantToken;
      encryptSecret("ha_token", s.homeAssistantToken, homeAssistantToken);
      sp7json::appendEscapedField(json, "homeAssistantToken", homeAssistantToken.c_str());
    }
  }
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
    if (includeSecrets) {
      json += "\"wifi";
      json += String(i + 1);
      json += "Password\":\"";
      if (clearSecrets) {
        json += sp7json::escape(s.wifiCredentials[i].password);
      } else {
        char purpose[8];
        snprintf(purpose, sizeof(purpose), "wifi%u", (unsigned)(i + 1));
        String encrypted;
        encryptSecret(purpose, s.wifiCredentials[i].password, encrypted);
        json += sp7json::escape(encrypted.c_str());
      }
      json += "\",";
    }
  }
  json += "\"audioSource\":"; json += String(s.audioSource); json += ",";
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
  if (includeSecrets) {
    if (clearSecrets) {
      sp7json::appendEscapedField(json, "otaPassword", s.otaPassword);
    } else {
      String otaPassword;
      encryptSecret("ota_password", s.otaPassword, otaPassword);
      sp7json::appendEscapedField(json, "otaPassword", otaPassword.c_str());
    }
  }
  json += "\"mqttEnabled\":"; json += (s.mqttEnabled ? "true" : "false"); json += ",";
  json += "\"mqttHost\":\""; json += sp7json::escape(s.mqttHost); json += "\",";
  json += "\"mqttPort\":"; json += String(s.mqttPort); json += ",";
  json += "\"mqttUsername\":\""; json += sp7json::escape(s.mqttUsername); json += "\",";
  if (includeSecrets) {
    if (clearSecrets) {
      sp7json::appendEscapedField(json, "mqttPassword", s.mqttPassword);
    } else {
      String mqttPassword;
      encryptSecret("mqtt_password", s.mqttPassword, mqttPassword);
      sp7json::appendEscapedField(json, "mqttPassword", mqttPassword.c_str());
    }
  }
  json += "\"mqttClientId\":\""; json += sp7json::escape(s.mqttClientId); json += "\",";
  json += "\"mqttBaseTopic\":\""; json += sp7json::escape(s.mqttBaseTopic); json += "\",";
  json += "\"mqttPublishPeriodMs\":"; json += String(s.mqttPublishPeriodMs); json += ",";
  json += "\"mqttRetain\":"; json += (s.mqttRetain ? "true" : "false"); json += ",";
  json += "\"notifyOnWarning\":"; json += (s.notifyOnWarning == ALERT_NOTIFY_LEVEL_WARNING ? "true" : "false"); json += ",";
  json += "\"notifyOnRecovery\":"; json += (s.notifyOnRecovery ? "true" : "false"); json += ",";
  json += "\"slackEnabled\":"; json += (s.slackEnabled ? "true" : "false"); json += ",";
  if (includeSecrets) {
    if (clearSecrets) {
      sp7json::appendEscapedField(json, "slackWebhookUrl", s.slackWebhookUrl);
    } else {
      String slackWebhookUrl;
      encryptSecret("slack_webhook", s.slackWebhookUrl, slackWebhookUrl);
      sp7json::appendEscapedField(json, "slackWebhookUrl", slackWebhookUrl.c_str());
    }
  }
  json += "\"slackChannel\":\""; json += sp7json::escape(s.slackChannel); json += "\",";
  json += "\"telegramEnabled\":"; json += (s.telegramEnabled ? "true" : "false"); json += ",";
  if (includeSecrets) {
    if (clearSecrets) {
      sp7json::appendEscapedField(json, "telegramBotToken", s.telegramBotToken);
    } else {
      String telegramBotToken;
      encryptSecret("telegram_token", s.telegramBotToken, telegramBotToken);
      sp7json::appendEscapedField(json, "telegramBotToken", telegramBotToken.c_str());
    }
  }
  json += "\"telegramChatId\":\""; json += sp7json::escape(s.telegramChatId); json += "\",";
  json += "\"whatsappEnabled\":"; json += (s.whatsappEnabled ? "true" : "false"); json += ",";
  if (includeSecrets) {
    if (clearSecrets) {
      sp7json::appendEscapedField(json, "whatsappAccessToken", s.whatsappAccessToken);
    } else {
      String whatsappAccessToken;
      encryptSecret("whatsapp_token", s.whatsappAccessToken, whatsappAccessToken);
      sp7json::appendEscapedField(json, "whatsappAccessToken", whatsappAccessToken.c_str());
    }
  }
  json += "\"whatsappPhoneNumberId\":\""; json += sp7json::escape(s.whatsappPhoneNumberId); json += "\",";
  json += "\"whatsappRecipient\":\""; json += sp7json::escape(s.whatsappRecipient); json += "\",";
  json += "\"whatsappApiVersion\":\""; json += sp7json::escape(s.whatsappApiVersion); json += "\"";
  json += "}";
  return json;
}

bool SettingsStore::importJson(SettingsV1& s, const String& json, String* err) {
  SettingsV1 next = s;
  auto loadImportedSecret = [&](const String& raw,
                                const char* purpose,
                                char* out,
                                size_t outSize,
                                const String& fieldName) -> bool {
    if (isEncryptedSecretRecord(raw.c_str())) {
      if (!decryptSecret(purpose, raw.c_str(), out, outSize)) {
        if (err) *err = String(fieldName) + " unreadable on this device";
        return false;
      }
      return true;
    }
    if (!sp7json::safeCopy(out, outSize, raw)) {
      if (err) *err = String(fieldName) + " too long";
      return false;
    }
    return true;
  };

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
  dashboardPin.trim();
  if (dashboardPin.length()) {
    if (!sp7json::safeCopy(next.dashboardPin, sizeof(next.dashboardPin), dashboardPin)
        || !normalizePinStorage(next.dashboardPin, sizeof(next.dashboardPin))) {
      if (err) *err = "bad dashboardPin";
      return false;
    }
  } else {
    next.dashboardPin[0] = '\0';
  }
  if (!loadImportedSecret(homeAssistantToken, "ha_token", next.homeAssistantToken, sizeof(next.homeAssistantToken), "homeAssistantToken")) {
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
    char purpose[8];
    snprintf(purpose, sizeof(purpose), "wifi%u", (unsigned)(i + 1));
    if (!loadImportedSecret(wifiPasswords[i],
                            purpose,
                            next.wifiCredentials[i].password,
                            sizeof(next.wifiCredentials[i].password),
                            String("wifi") + String(i + 1) + "Password")) {
      return false;
    }
  }
  next.ntpSyncIntervalMs = (uint32_t)sp7json::parseInt(json, "ntpSyncMinutes", (int)(next.ntpSyncIntervalMs / MS_PER_MINUTE)) * MS_PER_MINUTE;

  next.audioSource = (uint8_t)sp7json::parseInt(json, "audioSource", next.audioSource);
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
  if (!loadImportedSecret(otaPassword, "ota_password", next.otaPassword, sizeof(next.otaPassword), "otaPassword")) {
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
  next.notifyOnWarning = sp7json::parseBool(json, "notifyOnWarning", next.notifyOnWarning == ALERT_NOTIFY_LEVEL_WARNING)
    ? ALERT_NOTIFY_LEVEL_WARNING
    : ALERT_NOTIFY_LEVEL_CRITICAL;
  next.notifyOnRecovery = sp7json::parseBool(json, "notifyOnRecovery", next.notifyOnRecovery != 0) ? 1 : 0;
  next.slackEnabled = sp7json::parseBool(json, "slackEnabled", next.slackEnabled != 0) ? 1 : 0;
  next.telegramEnabled = sp7json::parseBool(json, "telegramEnabled", next.telegramEnabled != 0) ? 1 : 0;
  next.whatsappEnabled = sp7json::parseBool(json, "whatsappEnabled", next.whatsappEnabled != 0) ? 1 : 0;

  String slackWebhookUrl = sp7json::parseString(json, "slackWebhookUrl", String(next.slackWebhookUrl));
  String slackChannel = sp7json::parseString(json, "slackChannel", String(next.slackChannel));
  String telegramBotToken = sp7json::parseString(json, "telegramBotToken", String(next.telegramBotToken));
  String telegramChatId = sp7json::parseString(json, "telegramChatId", String(next.telegramChatId));
  String whatsappAccessToken = sp7json::parseString(json, "whatsappAccessToken", String(next.whatsappAccessToken));
  String whatsappPhoneNumberId = sp7json::parseString(json, "whatsappPhoneNumberId", String(next.whatsappPhoneNumberId));
  String whatsappRecipient = sp7json::parseString(json, "whatsappRecipient", String(next.whatsappRecipient));
  String whatsappApiVersion = sp7json::parseString(json, "whatsappApiVersion", String(next.whatsappApiVersion));

  if (!sp7json::safeCopy(next.mqttHost, sizeof(next.mqttHost), mqttHost)) {
    if (err) *err = "mqttHost too long";
    return false;
  }
  if (!sp7json::safeCopy(next.mqttUsername, sizeof(next.mqttUsername), mqttUsername)) {
    if (err) *err = "mqttUsername too long";
    return false;
  }
  if (!loadImportedSecret(mqttPassword, "mqtt_password", next.mqttPassword, sizeof(next.mqttPassword), "mqttPassword")) {
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
  if (!loadImportedSecret(slackWebhookUrl, "slack_webhook", next.slackWebhookUrl, sizeof(next.slackWebhookUrl), "slackWebhookUrl")) {
    return false;
  }
  if (!sp7json::safeCopy(next.slackChannel, sizeof(next.slackChannel), slackChannel)) {
    if (err) *err = "slackChannel too long";
    return false;
  }
  if (!loadImportedSecret(telegramBotToken, "telegram_token", next.telegramBotToken, sizeof(next.telegramBotToken), "telegramBotToken")) {
    return false;
  }
  if (!sp7json::safeCopy(next.telegramChatId, sizeof(next.telegramChatId), telegramChatId)) {
    if (err) *err = "telegramChatId too long";
    return false;
  }
  if (!loadImportedSecret(whatsappAccessToken, "whatsapp_token", next.whatsappAccessToken, sizeof(next.whatsappAccessToken), "whatsappAccessToken")) {
    return false;
  }
  if (!sp7json::safeCopy(next.whatsappPhoneNumberId, sizeof(next.whatsappPhoneNumberId), whatsappPhoneNumberId)) {
    if (err) *err = "whatsappPhoneNumberId too long";
    return false;
  }
  if (!sp7json::safeCopy(next.whatsappRecipient, sizeof(next.whatsappRecipient), whatsappRecipient)) {
    if (err) *err = "whatsappRecipient too long";
    return false;
  }
  if (!sp7json::safeCopy(next.whatsappApiVersion, sizeof(next.whatsappApiVersion), whatsappApiVersion)) {
    if (err) *err = "whatsappApiVersion too long";
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
  backupPrefs.putString("cfg", exportJson(s, EXPORT_SECRETS_ENCRYPTED));
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
    s.pdmClkPin = def.pdmClkPin;
    s.pdmDataPin = def.pdmDataPin;
    s.inmp441BclkPin = def.inmp441BclkPin;
    s.inmp441WsPin = def.inmp441WsPin;
    s.inmp441DataPin = def.inmp441DataPin;
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
  } else if (scope == "notifications") {
    s.notifyOnWarning = def.notifyOnWarning;
    s.notifyOnRecovery = def.notifyOnRecovery;
    s.slackEnabled = def.slackEnabled;
    memcpy(s.slackWebhookUrl, def.slackWebhookUrl, sizeof(s.slackWebhookUrl));
    memcpy(s.slackChannel, def.slackChannel, sizeof(s.slackChannel));
    s.telegramEnabled = def.telegramEnabled;
    memcpy(s.telegramBotToken, def.telegramBotToken, sizeof(s.telegramBotToken));
    memcpy(s.telegramChatId, def.telegramChatId, sizeof(s.telegramChatId));
    s.whatsappEnabled = def.whatsappEnabled;
    memcpy(s.whatsappAccessToken, def.whatsappAccessToken, sizeof(s.whatsappAccessToken));
    memcpy(s.whatsappPhoneNumberId, def.whatsappPhoneNumberId, sizeof(s.whatsappPhoneNumberId));
    memcpy(s.whatsappRecipient, def.whatsappRecipient, sizeof(s.whatsappRecipient));
    memcpy(s.whatsappApiVersion, def.whatsappApiVersion, sizeof(s.whatsappApiVersion));
  } else {
    if (err) *err = "unknown scope";
    return false;
  }

  sanitize(s);
  return true;
}
