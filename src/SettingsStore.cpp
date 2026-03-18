#include "SettingsStore.h"
#include "JsonHelpers.h"

#include <ctime>
#include <cstring>
#include <math.h>
#include <stdlib.h>

#include <esp_mac.h>
#include <esp_random.h>
#include <LittleFS.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

#include "DebugLog.h"

#define Serial0 DebugSerial0

namespace {
static constexpr const char* kEncryptedSecretPrefix = "enc:v1:";
static constexpr const char* kPlainSecretPrefix = "raw:v1:";
static constexpr const char* kPinHashPrefix = "h1:";
static constexpr size_t kEncryptedSecretNonceLength = 12;
static constexpr size_t kEncryptedSecretTagLength = 16;
static constexpr size_t kSecretKeyLength = 32;
static constexpr size_t kBackupChunkSize = 768;
static constexpr uint8_t kBackupChunkMaxCount = 16;
static constexpr const char* kBackupFilePath = "/sp7-backup.json";
static constexpr const char* kBackupTsFilePath = "/sp7-backup.ts";
static constexpr const char* kWebUsersFilePath = "/sp7-webusers.bin";
static constexpr const char* kWebUsersTmpFilePath = "/sp7-webusers.tmp";
static constexpr const char* kCalibrationProfileJsonNames[CALIBRATION_PROFILE_COUNT] = {
  "analog",
  "pdm",
  "inmp441",
};

bool ensureBackupFsMounted() {
  static int8_t mounted = -1;
  if (mounted >= 0) return mounted == 1;
  mounted = LittleFS.begin(true) ? 1 : 0;
  return mounted == 1;
}

bool readBackupFileString(const char* path, String& out) {
  out = "";
  if (!ensureBackupFsMounted()) return false;
  File file = LittleFS.open(path, "r");
  if (!file) return false;
  const size_t size = file.size();
  if (size) out.reserve(size + 1U);
  while (file.available()) {
    out += (char)file.read();
  }
  file.close();
  return out.length() > 0;
}

void applyAudioBoardProfileDefaults(SettingsV1& s) {
  s.analogPin = SOUNDPANEL7_DEFAULT_ANALOG_PIN;
  s.pdmClkPin = SOUNDPANEL7_DEFAULT_PDM_CLK_PIN;
  s.pdmDataPin = SOUNDPANEL7_DEFAULT_PDM_DATA_PIN;
  s.inmp441BclkPin = SOUNDPANEL7_DEFAULT_INMP441_BCLK_PIN;
  s.inmp441WsPin = SOUNDPANEL7_DEFAULT_INMP441_WS_PIN;
  s.inmp441DataPin = SOUNDPANEL7_DEFAULT_INMP441_DATA_PIN;
}

void copyActiveCalibrationToProfile(const SettingsV1& s, CalibrationProfile& profile) {
  profile.baseOffsetDb = s.analogBaseOffsetDb;
  profile.extraOffsetDb = s.analogExtraOffsetDb;
  profile.pointCount = s.calibrationPointCount;
  profile.captureMs = s.calibrationCaptureMs;
  memcpy(profile.pointRefDb, s.calPointRefDb, sizeof(profile.pointRefDb));
  memcpy(profile.pointRawLogRms, s.calPointRawLogRms, sizeof(profile.pointRawLogRms));
  memcpy(profile.pointValid, s.calPointValid, sizeof(profile.pointValid));
}

void applyProfileToActiveCalibration(SettingsV1& s, const CalibrationProfile& profile) {
  s.analogBaseOffsetDb = profile.baseOffsetDb;
  s.analogExtraOffsetDb = profile.extraOffsetDb;
  s.calibrationPointCount = profile.pointCount;
  s.calibrationCaptureMs = profile.captureMs;
  memcpy(s.calPointRefDb, profile.pointRefDb, sizeof(s.calPointRefDb));
  memcpy(s.calPointRawLogRms, profile.pointRawLogRms, sizeof(s.calPointRawLogRms));
  memcpy(s.calPointValid, profile.pointValid, sizeof(s.calPointValid));
}

bool loadCalibrationProfile(Preferences& prefs, uint8_t index, CalibrationProfile& out) {
  char keyCount[12];
  snprintf(keyCount, sizeof(keyCount), "cp%u_cnt", (unsigned)index);
  const uint8_t savedCount = (uint8_t)prefs.getUChar(keyCount, 0xFF);
  if (savedCount == 0xFF) return false;

  char keyCapture[12];
  char keyBase[12];
  char keyExtra[12];
  snprintf(keyCapture, sizeof(keyCapture), "cp%u_cap", (unsigned)index);
  snprintf(keyBase, sizeof(keyBase), "cp%u_bas", (unsigned)index);
  snprintf(keyExtra, sizeof(keyExtra), "cp%u_ext", (unsigned)index);

  out.pointCount = savedCount;
  out.captureMs = prefs.getUInt(keyCapture, out.captureMs);
  out.baseOffsetDb = prefs.getFloat(keyBase, out.baseOffsetDb);
  out.extraOffsetDb = prefs.getFloat(keyExtra, out.extraOffsetDb);

  for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
    char keyRef[12];
    char keyRaw[12];
    char keyVal[12];
    snprintf(keyRef, sizeof(keyRef), "cp%ur%u", (unsigned)index, (unsigned)(i + 1));
    snprintf(keyRaw, sizeof(keyRaw), "cp%ux%u", (unsigned)index, (unsigned)(i + 1));
    snprintf(keyVal, sizeof(keyVal), "cp%uv%u", (unsigned)index, (unsigned)(i + 1));
    out.pointRefDb[i] = prefs.getFloat(keyRef, out.pointRefDb[i]);
    out.pointRawLogRms[i] = prefs.getFloat(keyRaw, out.pointRawLogRms[i]);
    out.pointValid[i] = (uint8_t)prefs.getUChar(keyVal, out.pointValid[i]);
  }

  return true;
}

void saveCalibrationProfile(Preferences& prefs, uint8_t index, const CalibrationProfile& profile) {
  char keyCount[12];
  char keyCapture[12];
  char keyBase[12];
  char keyExtra[12];
  snprintf(keyCount, sizeof(keyCount), "cp%u_cnt", (unsigned)index);
  snprintf(keyCapture, sizeof(keyCapture), "cp%u_cap", (unsigned)index);
  snprintf(keyBase, sizeof(keyBase), "cp%u_bas", (unsigned)index);
  snprintf(keyExtra, sizeof(keyExtra), "cp%u_ext", (unsigned)index);

  prefs.putUChar(keyCount, profile.pointCount);
  prefs.putUInt(keyCapture, profile.captureMs);
  prefs.putFloat(keyBase, profile.baseOffsetDb);
  prefs.putFloat(keyExtra, profile.extraOffsetDb);

  for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
    char keyRef[12];
    char keyRaw[12];
    char keyVal[12];
    snprintf(keyRef, sizeof(keyRef), "cp%ur%u", (unsigned)index, (unsigned)(i + 1));
    snprintf(keyRaw, sizeof(keyRaw), "cp%ux%u", (unsigned)index, (unsigned)(i + 1));
    snprintf(keyVal, sizeof(keyVal), "cp%uv%u", (unsigned)index, (unsigned)(i + 1));
    prefs.putFloat(keyRef, profile.pointRefDb[i]);
    prefs.putFloat(keyRaw, profile.pointRawLogRms[i]);
    prefs.putUChar(keyVal, profile.pointValid[i]);
  }
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

void fillLegacyWebUserKeys(uint8_t index, char (&keyActive)[8], char (&keyUser)[8], char (&keySalt)[8], char (&keyHash)[8]) {
  snprintf(keyActive, sizeof(keyActive), "wu%da", index + 1);
  snprintf(keyUser, sizeof(keyUser), "wu%du", index + 1);
  snprintf(keySalt, sizeof(keySalt), "wu%ds", index + 1);
  snprintf(keyHash, sizeof(keyHash), "wu%dh", index + 1);
}

void clearLegacyWebUserKeys(Preferences& prefs) {
  for (uint8_t i = 0; i < WEB_USER_MAX_COUNT; i++) {
    char keyActive[8];
    char keyUser[8];
    char keySalt[8];
    char keyHash[8];
    fillLegacyWebUserKeys(i, keyActive, keyUser, keySalt, keyHash);
    prefs.remove(keyActive);
    prefs.remove(keyUser);
    prefs.remove(keySalt);
    prefs.remove(keyHash);
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

void SettingsStore::syncActiveCalibrationProfile(SettingsV1& s) {
  const uint8_t profileIndex = calibrationProfileIndexForAudioSource(s.audioSource);
  if (profileIndex >= CALIBRATION_PROFILE_COUNT) return;
  copyActiveCalibrationToProfile(s, s.calibrationProfiles[profileIndex]);
}

void SettingsStore::switchCalibrationProfile(SettingsV1& s, uint8_t nextAudioSource) {
  syncActiveCalibrationProfile(s);
  s.audioSource = nextAudioSource;
  const uint8_t profileIndex = calibrationProfileIndexForAudioSource(nextAudioSource);
  if (profileIndex >= CALIBRATION_PROFILE_COUNT) return;
  applyProfileToActiveCalibration(s, s.calibrationProfiles[profileIndex]);
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
  if (strncmp(stored, kPlainSecretPrefix, strlen(kPlainSecretPrefix)) == 0) {
    return sp7json::safeCopy(out, outSize, String(stored + strlen(kPlainSecretPrefix)));
  }
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
  if (strncmp(stored.c_str(), kPlainSecretPrefix, strlen(kPlainSecretPrefix)) == 0) {
    return decryptSecret(purpose, stored.c_str(), out, outSize);
  }
  if (!sp7json::safeCopy(out, outSize, stored)) return false;
  if (migrated) *migrated = true;
  return true;
}

bool SettingsStore::saveSecret(const char* key, const char* purpose, const char* value) {
  String encrypted;
  if (!encryptSecret(purpose, value, encrypted)) {
    Serial0.printf("[SET] Failed to encrypt secret '%s'\n", key ? key : "?");
    encrypted = "";
  }

  if (encrypted.length()) {
    const size_t written = _prefs.putString(key, encrypted);
    if (written == encrypted.length()) return true;
    Serial0.printf("[SET] Failed to store encrypted secret '%s' (len=%u), trying plaintext fallback\n",
                   key ? key : "?",
                   (unsigned)encrypted.length());
  }

  const char* plaintext = value ? value : "";
  const bool hasPlaintext = plaintext[0] != '\0';
  const String fallbackStored = hasPlaintext
    ? String(kPlainSecretPrefix) + plaintext
    : String("");
  const size_t plainLen = fallbackStored.length();
  const size_t plainWritten = _prefs.putString(key, fallbackStored);
  if (plainWritten == plainLen) {
    if (hasPlaintext) {
      Serial0.printf("[SET] Secret '%s' stored in plaintext fallback mode\n", key ? key : "?");
    }
    return true;
  }

  Serial0.printf("[SET] Failed to store secret '%s' even in plaintext fallback mode\n", key ? key : "?");
  return false;
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
  const bool versionMismatch = (ver != SETTINGS_VERSION);

  if (magic != SETTINGS_MAGIC) {
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
  out.tardisModeEnabled = (uint8_t)_prefs.getUChar("td_en", out.tardisModeEnabled);
  out.tardisInteriorLedEnabled = (uint8_t)_prefs.getUChar("td_in", out.tardisInteriorLedEnabled);
  out.tardisExteriorLedEnabled = (uint8_t)_prefs.getUChar("td_ex", out.tardisExteriorLedEnabled);
  out.tardisInteriorRgbMode = (uint8_t)_prefs.getUChar("td_im", out.tardisInteriorRgbMode);
  out.tardisInteriorRgbColor = _prefs.getUInt("td_ic", out.tardisInteriorRgbColor);
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

  bool calibrationProfilesLoaded = false;
  for (uint8_t i = 0; i < CALIBRATION_PROFILE_COUNT; i++) {
    calibrationProfilesLoaded = loadCalibrationProfile(_prefs, i, out.calibrationProfiles[i]) || calibrationProfilesLoaded;
  }

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

  if (!calibrationProfilesLoaded) {
    const uint8_t currentProfileIndex = calibrationProfileIndexForAudioSource(out.audioSource);
    const uint8_t fallbackProfileIndex = currentProfileIndex < CALIBRATION_PROFILE_COUNT ? currentProfileIndex : 0;
    copyActiveCalibrationToProfile(out, out.calibrationProfiles[fallbackProfileIndex]);
  }

  sanitize(out);

  const uint8_t activeProfileIndex = calibrationProfileIndexForAudioSource(out.audioSource);
  if (activeProfileIndex < CALIBRATION_PROFILE_COUNT) {
    applyProfileToActiveCalibration(out, out.calibrationProfiles[activeProfileIndex]);
    sanitize(out);
    syncActiveCalibrationProfile(out);
  }

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
  if (versionMismatch && ver < SETTINGS_VERSION) save(out);
}

void SettingsStore::save(const SettingsV1 &s) {
  SettingsV1 persisted = s;
  syncActiveCalibrationProfile(persisted);

  _prefs.putUInt("magic", SETTINGS_MAGIC);
  _prefs.putUShort("ver", SETTINGS_VERSION);

  _prefs.putUChar("ui_bl", persisted.backlight);
  _prefs.putUChar("th_g", persisted.th.greenMax);
  _prefs.putUChar("th_o", persisted.th.orangeMax);
  _prefs.putUChar("hist_m", persisted.historyMinutes);
  _prefs.putUInt("ui_ow_ms", persisted.orangeAlertHoldMs);
  _prefs.putUInt("ui_rw_ms", persisted.redAlertHoldMs);
  _prefs.putUChar("ui_live", persisted.liveEnabled);
  _prefs.putUChar("ui_touch", persisted.touchEnabled);
  _prefs.putUChar("ui_page", persisted.dashboardPage);
  _prefs.putUChar("ui_fsm", persisted.dashboardFullscreenMask);
  _prefs.putUChar("td_en", persisted.tardisModeEnabled);
  _prefs.putUChar("td_in", persisted.tardisInteriorLedEnabled);
  _prefs.putUChar("td_ex", persisted.tardisExteriorLedEnabled);
  _prefs.putUChar("td_im", persisted.tardisInteriorRgbMode);
  _prefs.putUInt("td_ic", persisted.tardisInteriorRgbColor);
  char dashboardPin[sizeof(persisted.dashboardPin)] = {0};
  if (sp7json::safeCopy(dashboardPin, sizeof(dashboardPin), String(persisted.dashboardPin))
      && normalizePinStorage(dashboardPin, sizeof(dashboardPin))) {
    _prefs.putString("ui_pin", dashboardPin);
  } else {
    _prefs.putString("ui_pin", "");
  }
  saveSecret("ha_tok", "ha_token", persisted.homeAssistantToken);

  _prefs.putString("tz", persisted.tz);
  _prefs.putString("ntp", persisted.ntpServer);
  _prefs.putUInt("ntp_ms", persisted.ntpSyncIntervalMs);
  _prefs.putString("hn", persisted.hostname);
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    char keySsid[8];
    char keyPass[8];
    char purpose[8];
    snprintf(keySsid, sizeof(keySsid), "wf%us", (unsigned)(i + 1));
    snprintf(keyPass, sizeof(keyPass), "wf%up", (unsigned)(i + 1));
    snprintf(purpose, sizeof(purpose), "wifi%u", (unsigned)(i + 1));
    _prefs.putString(keySsid, persisted.wifiCredentials[i].ssid);
    saveSecret(keyPass, purpose, persisted.wifiCredentials[i].password);
  }

  _prefs.putUChar("ota_en", persisted.otaEnabled);
  _prefs.putUShort("ota_pt", persisted.otaPort);
  _prefs.putString("ota_hn", persisted.otaHostname);
  saveSecret("ota_pw", "ota_password", persisted.otaPassword);

  _prefs.putUChar("a_src", persisted.audioSource);
  _prefs.remove("a_pin");
  _prefs.remove("a_pdmck");
  _prefs.remove("a_pdmdt");
  _prefs.remove("a_i2sbk");
  _prefs.remove("a_i2sws");
  _prefs.remove("a_i2sdt");
  _prefs.putUShort("a_rms", persisted.analogRmsSamples);
  _prefs.putUChar("a_resp", persisted.audioResponseMode);
  _prefs.putFloat("a_ema", persisted.emaAlpha);
  _prefs.putUInt("a_peak", persisted.peakHoldMs);
  _prefs.putFloat("a_base", persisted.analogBaseOffsetDb);
  _prefs.putFloat("a_extra", persisted.analogExtraOffsetDb);
  _prefs.putUChar("cal_cnt", persisted.calibrationPointCount);
  _prefs.putUInt("cal_capms", persisted.calibrationCaptureMs);
  for (uint8_t i = 0; i < CALIBRATION_PROFILE_COUNT; i++) {
    saveCalibrationProfile(_prefs, i, persisted.calibrationProfiles[i]);
  }

  _prefs.putUChar("mq_en", persisted.mqttEnabled);
  _prefs.putString("mq_host", persisted.mqttHost);
  _prefs.putUShort("mq_pt", persisted.mqttPort);
  _prefs.putString("mq_usr", persisted.mqttUsername);
  saveSecret("mq_pwd", "mqtt_password", persisted.mqttPassword);
  _prefs.putString("mq_cid", persisted.mqttClientId);
  _prefs.putString("mq_base", persisted.mqttBaseTopic);
  _prefs.putUShort("mq_pubms", persisted.mqttPublishPeriodMs);
  _prefs.putUChar("mq_ret", persisted.mqttRetain);

  _prefs.putUChar("n_lvl", persisted.notifyOnWarning);
  _prefs.putUChar("n_rec", persisted.notifyOnRecovery);
  _prefs.putUChar("n_s_en", persisted.slackEnabled);
  saveSecret("n_s_url", "slack_webhook", persisted.slackWebhookUrl);
  _prefs.putString("n_s_ch", persisted.slackChannel);
  _prefs.putUChar("n_t_en", persisted.telegramEnabled);
  saveSecret("n_t_tok", "telegram_token", persisted.telegramBotToken);
  _prefs.putString("n_t_chat", persisted.telegramChatId);
  _prefs.putUChar("n_w_en", persisted.whatsappEnabled);
  saveSecret("n_w_tok", "whatsapp_token", persisted.whatsappAccessToken);
  _prefs.putString("n_w_pid", persisted.whatsappPhoneNumberId);
  _prefs.putString("n_w_to", persisted.whatsappRecipient);
  _prefs.putString("n_w_ver", persisted.whatsappApiVersion);

  for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
    char keyRef[8];
    char keyRaw[8];
    char keyVal[8];
    snprintf(keyRef, sizeof(keyRef), "c%dr", i + 1);
    snprintf(keyRaw, sizeof(keyRaw), "c%dx", i + 1);
    snprintf(keyVal, sizeof(keyVal), "c%dv", i + 1);

    _prefs.putFloat(keyRef, persisted.calPointRefDb[i]);
    _prefs.putFloat(keyRaw, persisted.calPointRawLogRms[i]);
    _prefs.putUChar(keyVal, persisted.calPointValid[i]);
  }
}

void SettingsStore::factoryReset() {
  _prefs.clear();
}

void SettingsStore::sanitize(SettingsV1& s) {
  auto sanitizeCalibrationProfile = [](CalibrationProfile& profile) {
    profile.pointCount = normalizedCalibrationPointCount(profile.pointCount);
    if (profile.captureMs < MIN_CALIBRATION_CAPTURE_MS) profile.captureMs = MIN_CALIBRATION_CAPTURE_MS;
    if (profile.captureMs > MAX_CALIBRATION_CAPTURE_MS) profile.captureMs = MAX_CALIBRATION_CAPTURE_MS;
    const float* recommended = (profile.pointCount == CALIBRATION_POINT_MAX) ? RECOMMENDED_CALIBRATION_5 : RECOMMENDED_CALIBRATION_3;
    for (int i = 0; i < CALIBRATION_POINT_MAX; i++) {
      profile.pointValid[i] = profile.pointValid[i] ? 1 : 0;
      if (!isfinite(profile.pointRefDb[i])) profile.pointRefDb[i] = recommended[i];
      if (profile.pointRefDb[i] < 35.0f) profile.pointRefDb[i] = 35.0f;
      if (profile.pointRefDb[i] > 110.0f) profile.pointRefDb[i] = 110.0f;
      if (!isfinite(profile.pointRawLogRms[i])) profile.pointRawLogRms[i] = 0.0f;
    }
  };

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
  s.tardisModeEnabled = s.tardisModeEnabled ? 1 : 0;
  s.tardisInteriorLedEnabled = s.tardisInteriorLedEnabled ? 1 : 0;
  s.tardisExteriorLedEnabled = s.tardisExteriorLedEnabled ? 1 : 0;
  if (s.tardisInteriorRgbMode > TARDIS_INTERIOR_RGB_MODE_MAX) {
    s.tardisInteriorRgbMode = TARDIS_INTERIOR_RGB_MODE_ALERT;
  }
  s.tardisInteriorRgbColor &= 0x00FFFFFFUL;

  if (s.orangeAlertHoldMs > MAX_ALERT_HOLD_MS) s.orangeAlertHoldMs = MAX_ALERT_HOLD_MS;
  if (s.redAlertHoldMs > MAX_ALERT_HOLD_MS) s.redAlertHoldMs = MAX_ALERT_HOLD_MS;
  if (s.dashboardPin[0] && !pinCodeIsConfigured(s.dashboardPin)) {
    s.dashboardPin[0] = '\0';
  }
  s.homeAssistantToken[sizeof(s.homeAssistantToken) - 1] = '\0';

  applyAudioBoardProfileDefaults(s);
  for (uint8_t i = 0; i < CALIBRATION_PROFILE_COUNT; i++) {
    sanitizeCalibrationProfile(s.calibrationProfiles[i]);
  }
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

  s.calibrationPointCount = normalizedCalibrationPointCount(s.calibrationPointCount);
  if (s.calibrationCaptureMs < MIN_CALIBRATION_CAPTURE_MS) s.calibrationCaptureMs = MIN_CALIBRATION_CAPTURE_MS;
  if (s.calibrationCaptureMs > MAX_CALIBRATION_CAPTURE_MS) s.calibrationCaptureMs = MAX_CALIBRATION_CAPTURE_MS;
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
  }

  if (ensureBackupFsMounted()) {
    File file = LittleFS.open(kWebUsersFilePath, "r");
    if (file) {
      const size_t expectedBytes = sizeof(out);
      if (file.size() == expectedBytes && file.read((uint8_t*)out, expectedBytes) == expectedBytes) {
        file.close();
        for (uint8_t i = 0; i < WEB_USER_MAX_COUNT; i++) {
          sanitizeWebUser(out[i]);
        }
        return;
      }
      file.close();
      LittleFS.remove(kWebUsersFilePath);
    }
  }

  for (uint8_t i = 0; i < WEB_USER_MAX_COUNT; i++) {
    char keyActive[8];
    char keyUser[8];
    char keySalt[8];
    char keyHash[8];
    fillLegacyWebUserKeys(i, keyActive, keyUser, keySalt, keyHash);

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

  if (!ensureBackupFsMounted()) {
    if (err) *err = "web users fs unavailable";
    return false;
  }

  File tmp = LittleFS.open(kWebUsersTmpFilePath, "w");
  if (!tmp) {
    if (err) *err = "web users temp file open failed";
    return false;
  }
  const size_t expectedBytes = sizeof(users);
  const size_t written = tmp.write((const uint8_t*)users, expectedBytes);
  tmp.close();
  if (written != expectedBytes) {
    LittleFS.remove(kWebUsersTmpFilePath);
    if (err) *err = "web users write failed";
    return false;
  }
  LittleFS.remove(kWebUsersFilePath);
  if (!LittleFS.rename(kWebUsersTmpFilePath, kWebUsersFilePath)) {
    LittleFS.remove(kWebUsersTmpFilePath);
    if (err) *err = "web users commit failed";
    return false;
  }

  WebUserRecord verify[WEB_USER_MAX_COUNT];
  loadWebUsers(verify);
  if (!verify[slot].active
      || strcmp(verify[slot].username, users[slot].username) != 0
      || strcmp(verify[slot].passwordSalt, users[slot].passwordSalt) != 0
      || strcmp(verify[slot].passwordHash, users[slot].passwordHash) != 0) {
    if (err) *err = "user verification failed";
    return false;
  }

  clearLegacyWebUserKeys(_prefs);
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

  if (!ensureBackupFsMounted()) {
    if (err) *err = "web users fs unavailable";
    return false;
  }

  File tmp = LittleFS.open(kWebUsersTmpFilePath, "w");
  if (!tmp) {
    if (err) *err = "user delete temp file open failed";
    return false;
  }
  const size_t expectedBytes = sizeof(users);
  const size_t written = tmp.write((const uint8_t*)users, expectedBytes);
  tmp.close();
  if (written != expectedBytes) {
    LittleFS.remove(kWebUsersTmpFilePath);
    if (err) *err = "user delete write failed";
    return false;
  }
  LittleFS.remove(kWebUsersFilePath);
  if (!LittleFS.rename(kWebUsersTmpFilePath, kWebUsersFilePath)) {
    LittleFS.remove(kWebUsersTmpFilePath);
    if (err) *err = "user delete commit failed";
    return false;
  }

  WebUserRecord verify[WEB_USER_MAX_COUNT];
  loadWebUsers(verify);
  if (verify[index].active) {
    if (err) *err = "user delete verification failed";
    return false;
  }
  clearLegacyWebUserKeys(_prefs);
  return true;
}

void SettingsStore::clearWebUsers() {
  if (ensureBackupFsMounted()) {
    LittleFS.remove(kWebUsersTmpFilePath);
    LittleFS.remove(kWebUsersFilePath);
  }
  clearLegacyWebUserKeys(_prefs);
}

String SettingsStore::exportJson(const SettingsV1& s, SecretExportMode secretMode, String* err) const {
  if (err) *err = "";
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
  json += "\"tardisModeEnabled\":"; json += (s.tardisModeEnabled ? "true" : "false"); json += ",";
  json += "\"tardisInteriorLedEnabled\":"; json += (s.tardisInteriorLedEnabled ? "true" : "false"); json += ",";
  json += "\"tardisExteriorLedEnabled\":"; json += (s.tardisExteriorLedEnabled ? "true" : "false"); json += ",";
  json += "\"tardisInteriorRgbMode\":"; json += String(s.tardisInteriorRgbMode); json += ",";
  json += "\"tardisInteriorRgbColor\":"; json += String(s.tardisInteriorRgbColor); json += ",";
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
      if (!encryptSecret("ha_token", s.homeAssistantToken, homeAssistantToken)) {
        if (err) *err = "home assistant token export failed";
        return String();
      }
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
        if (!encryptSecret(purpose, s.wifiCredentials[i].password, encrypted)) {
          if (err) *err = String("wifi") + String(i + 1) + " export failed";
          return String();
        }
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
  for (uint8_t profileIndex = 0; profileIndex < CALIBRATION_PROFILE_COUNT; profileIndex++) {
    const CalibrationProfile& profile = s.calibrationProfiles[profileIndex];
    const String prefix = String(kCalibrationProfileJsonNames[profileIndex]);
    json += "\""; json += prefix; json += "CalibrationPointCount\":"; json += String(profile.pointCount); json += ",";
    json += "\""; json += prefix; json += "CalibrationCaptureSec\":"; json += String(profile.captureMs / MS_PER_SECOND); json += ",";
    json += "\""; json += prefix; json += "BaseOffsetDb\":"; json += String(profile.baseOffsetDb, 4); json += ",";
    json += "\""; json += prefix; json += "ExtraOffsetDb\":"; json += String(profile.extraOffsetDb, 4); json += ",";
    json += "\""; json += prefix; json += "CalPointRefDb\":[";
    for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
      if (i) json += ",";
      json += String(profile.pointRefDb[i], 2);
    }
    json += "],";
    json += "\""; json += prefix; json += "CalPointRawLogRms\":[";
    for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
      if (i) json += ",";
      json += String(profile.pointRawLogRms[i], 4);
    }
    json += "],";
    json += "\""; json += prefix; json += "CalPointValid\":[";
    for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
      if (i) json += ",";
      json += String(profile.pointValid[i]);
    }
    json += "],";
  }
  json += "\"otaEnabled\":"; json += (s.otaEnabled ? "true" : "false"); json += ",";
  json += "\"otaPort\":"; json += String(s.otaPort); json += ",";
  json += "\"otaHostname\":\""; json += sp7json::escape(s.otaHostname); json += "\",";
  if (includeSecrets) {
    if (clearSecrets) {
      sp7json::appendEscapedField(json, "otaPassword", s.otaPassword);
    } else {
      String otaPassword;
      if (!encryptSecret("ota_password", s.otaPassword, otaPassword)) {
        if (err) *err = "ota password export failed";
        return String();
      }
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
      if (!encryptSecret("mqtt_password", s.mqttPassword, mqttPassword)) {
        if (err) *err = "mqtt password export failed";
        return String();
      }
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
      if (!encryptSecret("slack_webhook", s.slackWebhookUrl, slackWebhookUrl)) {
        if (err) *err = "slack webhook export failed";
        return String();
      }
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
      if (!encryptSecret("telegram_token", s.telegramBotToken, telegramBotToken)) {
        if (err) *err = "telegram token export failed";
        return String();
      }
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
      if (!encryptSecret("whatsapp_token", s.whatsappAccessToken, whatsappAccessToken)) {
        if (err) *err = "whatsapp token export failed";
        return String();
      }
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
  const String type = sp7json::parseString(json, "type", "", false);
  if (type != "soundpanel7-config") {
    if (err) *err = "invalid config type";
    return false;
  }
  const int version = sp7json::parseInt(json, "version", 0);
  if (version <= 0) {
    if (err) *err = "missing config version";
    return false;
  }
  if (version > SETTINGS_VERSION) {
    if (err) *err = "config created by newer firmware";
    return false;
  }

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
  next.tardisModeEnabled = sp7json::parseBool(json, "tardisModeEnabled", next.tardisModeEnabled != 0) ? 1 : 0;
  next.tardisInteriorLedEnabled = sp7json::parseBool(json, "tardisInteriorLedEnabled", next.tardisInteriorLedEnabled != 0) ? 1 : 0;
  next.tardisExteriorLedEnabled = sp7json::parseBool(json, "tardisExteriorLedEnabled", next.tardisExteriorLedEnabled != 0) ? 1 : 0;
  next.tardisInteriorRgbMode = (uint8_t)sp7json::parseInt(json, "tardisInteriorRgbMode", next.tardisInteriorRgbMode);
  next.tardisInteriorRgbColor = (uint32_t)sp7json::parseInt(json, "tardisInteriorRgbColor", (int)next.tardisInteriorRgbColor);
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
  bool importedCalibrationProfiles = false;
  for (uint8_t profileIndex = 0; profileIndex < CALIBRATION_PROFILE_COUNT; profileIndex++) {
    CalibrationProfile& profile = next.calibrationProfiles[profileIndex];
    const String prefix = String(kCalibrationProfileJsonNames[profileIndex]);
    const bool hasProfileCount = sp7json::findValueStart(json, (prefix + "CalibrationPointCount").c_str()) >= 0;
    const bool hasProfileCapture = sp7json::findValueStart(json, (prefix + "CalibrationCaptureSec").c_str()) >= 0;
    const bool hasProfileBase = sp7json::findValueStart(json, (prefix + "BaseOffsetDb").c_str()) >= 0;
    const bool hasProfileExtra = sp7json::findValueStart(json, (prefix + "ExtraOffsetDb").c_str()) >= 0;
    const int parsedPointCount = sp7json::parseInt(json, (prefix + "CalibrationPointCount").c_str(), profile.pointCount);
    profile.pointCount = (uint8_t)parsedPointCount;
    const int parsedCaptureSec = sp7json::parseInt(
      json,
      (prefix + "CalibrationCaptureSec").c_str(),
      (int)(profile.captureMs / MS_PER_SECOND)
    );
    profile.captureMs = (uint32_t)parsedCaptureSec * MS_PER_SECOND;
    const float parsedBaseOffset = sp7json::parseFloat(json, (prefix + "BaseOffsetDb").c_str(), profile.baseOffsetDb);
    const float parsedExtraOffset = sp7json::parseFloat(json, (prefix + "ExtraOffsetDb").c_str(), profile.extraOffsetDb);
    profile.baseOffsetDb = parsedBaseOffset;
    profile.extraOffsetDb = parsedExtraOffset;

    float profileRef[CALIBRATION_POINT_MAX];
    memcpy(profileRef, profile.pointRefDb, sizeof(profileRef));
    const bool hasProfileRef = sp7json::parseFloatArray(json, (prefix + "CalPointRefDb").c_str(), profileRef);
    if (hasProfileRef) {
      for (int i = 0; i < CALIBRATION_POINT_MAX; i++) profile.pointRefDb[i] = profileRef[i];
    }

    float profileRaw[CALIBRATION_POINT_MAX];
    memcpy(profileRaw, profile.pointRawLogRms, sizeof(profileRaw));
    const bool hasProfileRaw = sp7json::parseFloatArray(json, (prefix + "CalPointRawLogRms").c_str(), profileRaw);
    if (hasProfileRaw) {
      for (int i = 0; i < CALIBRATION_POINT_MAX; i++) profile.pointRawLogRms[i] = profileRaw[i];
    }

    uint8_t profileValid[CALIBRATION_POINT_MAX];
    memcpy(profileValid, profile.pointValid, sizeof(profileValid));
    const bool hasProfileValid = sp7json::parseU8Array(json, (prefix + "CalPointValid").c_str(), profileValid);
    if (hasProfileValid) {
      for (int i = 0; i < CALIBRATION_POINT_MAX; i++) profile.pointValid[i] = profileValid[i];
    }
    importedCalibrationProfiles = importedCalibrationProfiles
      || hasProfileCount
      || hasProfileCapture
      || hasProfileBase
      || hasProfileExtra
      || hasProfileRef
      || hasProfileRaw
      || hasProfileValid
      ;
  }
  if (!importedCalibrationProfiles) {
    const uint8_t currentProfileIndex = calibrationProfileIndexForAudioSource(next.audioSource);
    const uint8_t fallbackProfileIndex = currentProfileIndex < CALIBRATION_PROFILE_COUNT ? currentProfileIndex : 0;
    copyActiveCalibrationToProfile(next, next.calibrationProfiles[fallbackProfileIndex]);
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
  const uint8_t activeProfileIndex = calibrationProfileIndexForAudioSource(next.audioSource);
  if (activeProfileIndex < CALIBRATION_PROFILE_COUNT) {
    applyProfileToActiveCalibration(next, next.calibrationProfiles[activeProfileIndex]);
    sanitize(next);
    syncActiveCalibrationProfile(next);
  }
  s = next;
  return true;
}

bool SettingsStore::saveBackup(const SettingsV1& s) {
  SettingsV1 persisted = s;
  syncActiveCalibrationProfile(persisted);

  Preferences backupPrefs;
  String backupNs = _ns + "_bak";
  if (!backupPrefs.begin(backupNs.c_str(), false)) return false;
  String err;
  const String json = exportJson(persisted, EXPORT_SECRETS_ENCRYPTED, &err);
  if (!json.length()) {
    backupPrefs.end();
    return false;
  }
  if (ensureBackupFsMounted()) {
    File backupFile = LittleFS.open(kBackupFilePath, "w");
    if (backupFile) {
      const size_t written = backupFile.print(json);
      backupFile.close();
      if (written == json.length()) {
        time_t now = time(nullptr);
        const uint32_t ts = now > 946684800 ? (uint32_t)now : 0U;
        File tsFile = LittleFS.open(kBackupTsFilePath, "w");
        if (tsFile) {
          const size_t tsWritten = tsFile.print(String(ts));
          tsFile.close();
          if (tsWritten > 0) {
            backupPrefs.end();
            return true;
          }
        }
      }
      LittleFS.remove(kBackupFilePath);
      LittleFS.remove(kBackupTsFilePath);
    }
  }
  const size_t chunkCount = (json.length() + kBackupChunkSize - 1U) / kBackupChunkSize;
  if (chunkCount == 0 || chunkCount > kBackupChunkMaxCount) {
    backupPrefs.end();
    return false;
  }

  backupPrefs.remove("cfg");
  backupPrefs.remove("parts");
  backupPrefs.remove("len");
  for (uint8_t i = 0; i < kBackupChunkMaxCount; i++) {
    char key[8];
    snprintf(key, sizeof(key), "cfg%u", (unsigned)i);
    backupPrefs.remove(key);
  }

  bool chunksOk = true;
  for (uint8_t i = 0; i < chunkCount; i++) {
    const size_t start = (size_t)i * kBackupChunkSize;
    const String chunk = json.substring(start, min(json.length(), start + kBackupChunkSize));
    char key[8];
    snprintf(key, sizeof(key), "cfg%u", (unsigned)i);
    if (backupPrefs.putString(key, chunk) != chunk.length()) {
      chunksOk = false;
      break;
    }
  }
  time_t now = time(nullptr);
  const uint32_t ts = now > 946684800 ? (uint32_t)now : 0U;
  const size_t partsWritten = backupPrefs.putUChar("parts", (uint8_t)chunkCount);
  const size_t lenWritten = backupPrefs.putUInt("len", (uint32_t)json.length());
  const size_t tsWritten = backupPrefs.putUInt("ts", ts);
  const bool ok = chunksOk
               && partsWritten == sizeof(uint8_t)
               && lenWritten == sizeof(uint32_t)
               && tsWritten == sizeof(ts);
  backupPrefs.end();
  return ok;
}

uint32_t SettingsStore::backupTimestamp() const {
  String tsFileValue;
  if (readBackupFileString(kBackupTsFilePath, tsFileValue)) {
    tsFileValue.trim();
    const uint32_t ts = (uint32_t)strtoul(tsFileValue.c_str(), nullptr, 10);
    if (ts > 0) return ts;
  }

  Preferences backupPrefs;
  String backupNs = _ns + "_bak";
  if (!backupPrefs.begin(backupNs.c_str(), true)) return 0;
  uint32_t ts = backupPrefs.getUInt("ts", 0);
  backupPrefs.end();
  return ts;
}

bool SettingsStore::restoreBackup(SettingsV1& out, String* err) {
  String json;
  if (readBackupFileString(kBackupFilePath, json)) {
    return importJson(out, json, err);
  }

  Preferences backupPrefs;
  String backupNs = _ns + "_bak";
  if (!backupPrefs.begin(backupNs.c_str(), true)) {
    if (err) *err = "backup unavailable";
    return false;
  }
  const uint8_t partCount = backupPrefs.getUChar("parts", 0);
  if (partCount > 0 && partCount <= kBackupChunkMaxCount) {
    const uint32_t expectedLen = backupPrefs.getUInt("len", 0);
    json.reserve(expectedLen ? expectedLen : (uint32_t)partCount * kBackupChunkSize);
    for (uint8_t i = 0; i < partCount; i++) {
      char key[8];
      snprintf(key, sizeof(key), "cfg%u", (unsigned)i);
      const String chunk = backupPrefs.getString(key, "");
      if (!chunk.length()) {
        json = "";
        break;
      }
      json += chunk;
    }
    if (expectedLen && json.length() != expectedLen) {
      json = "";
    }
  }
  if (!json.length()) {
    json = backupPrefs.getString("cfg", "");
  }
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
    s.tardisModeEnabled = def.tardisModeEnabled;
    s.tardisInteriorLedEnabled = def.tardisInteriorLedEnabled;
    s.tardisExteriorLedEnabled = def.tardisExteriorLedEnabled;
    s.tardisInteriorRgbMode = def.tardisInteriorRgbMode;
    s.tardisInteriorRgbColor = def.tardisInteriorRgbColor;
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
    switchCalibrationProfile(s, def.audioSource);
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
