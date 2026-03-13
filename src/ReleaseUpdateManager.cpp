#include "ReleaseUpdateManager.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctime>
#include <mbedtls/sha256.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "AppConfig.h"
#include "JsonHelpers.h"
#include "NetManager.h"

namespace {
String sha256Hex(const uint8_t digest[32]) {
  static const char kHexChars[] = "0123456789abcdef";
  char out[65];
  for (size_t i = 0; i < 32; i++) {
    out[i * 2] = kHexChars[(digest[i] >> 4) & 0x0F];
    out[(i * 2) + 1] = kHexChars[digest[i] & 0x0F];
  }
  out[64] = '\0';
  return String(out);
}
}

bool ReleaseUpdateManager::begin(NetManager* net) {
  _net = net;
  clearResult();
  clearInstallState();
  _busy = false;
  _hasChecked = false;
  _prefsReady = _prefs.begin("releaseupd", false);
  if (_prefsReady) {
    loadPersistedState();
  }
  return true;
}

void ReleaseUpdateManager::loop() {
  if (_installRebootPending && (uint32_t)(millis() - _installRebootAtMs) >= kInstallRebootDelayMs) {
    Serial0.println("[REL] install complete, rebooting");
    delay(60);
    ESP.restart();
  }
}

const char* ReleaseUpdateManager::manifestUrl() const {
  return SOUNDPANEL7_RELEASE_MANIFEST_URL;
}

const char* ReleaseUpdateManager::currentVersion() const {
  return SOUNDPANEL7_VERSION;
}

void ReleaseUpdateManager::clearResult() {
  _lastCheckOk = false;
  _updateAvailable = false;
  _lastHttpCode = 0;
  _lastCheckUnix = 0;
  _latestVersion[0] = '\0';
  _publishedAt[0] = '\0';
  _releaseUrl[0] = '\0';
  _otaUrl[0] = '\0';
  _otaSha256[0] = '\0';
  _lastError[0] = '\0';
}

void ReleaseUpdateManager::setError(const String& error) {
  _lastCheckOk = false;
  _updateAvailable = false;
  _latestVersion[0] = '\0';
  _publishedAt[0] = '\0';
  _releaseUrl[0] = '\0';
  _otaUrl[0] = '\0';
  _otaSha256[0] = '\0';
  sp7json::safeCopy(_lastError, sizeof(_lastError), error);
}

void ReleaseUpdateManager::clearInstallState() {
  _installInProgress = false;
  _installFinished = false;
  _installSucceeded = false;
  _installStartedUnix = 0;
  _installFinishedUnix = 0;
  _installTotalBytes = 0;
  _installWrittenBytes = 0;
  _installProgressPct = 0;
  _installStatus[0] = '\0';
  memcpy(_installStatus, "idle", 5);
  _installError[0] = '\0';
  _installRebootPending = false;
  _installRebootAtMs = 0;
}

void ReleaseUpdateManager::setInstallStatus(const char* status) {
  if (!status) status = "idle";
  const String value(status);
  if (!sp7json::safeCopy(_installStatus, sizeof(_installStatus), value)) {
    memcpy(_installStatus, "state-too-long", 15);
    _installStatus[15] = '\0';
  }
}

void ReleaseUpdateManager::setInstallError(const String& error) {
  sp7json::safeCopy(_installError, sizeof(_installError), error);
}

void ReleaseUpdateManager::finishInstall(bool ok, const String& error) {
  _installInProgress = false;
  _installFinished = true;
  _installSucceeded = ok;
  _installFinishedUnix = currentUnixTimestamp();
  if (ok) {
    _installError[0] = '\0';
    setInstallStatus("success");
    _installProgressPct = 100;
    _installRebootPending = true;
    _installRebootAtMs = millis();
  } else {
    setInstallStatus("failed");
    setInstallError(error);
    _installRebootPending = false;
  }
}

void ReleaseUpdateManager::loadPersistedState() {
  if (!_prefsReady) return;

  _hasChecked = _prefs.getBool("checked", false);
  if (!_hasChecked) return;

  _lastCheckOk = _prefs.getBool("ok", false);
  _lastHttpCode = _prefs.getInt("http", 0);
  _lastCheckUnix = _prefs.getUInt("ts", 0U);
  _prefs.getString("latest", _latestVersion, sizeof(_latestVersion));
  _prefs.getString("pub", _publishedAt, sizeof(_publishedAt));
  _prefs.getString("rel", _releaseUrl, sizeof(_releaseUrl));
  _prefs.getString("ota", _otaUrl, sizeof(_otaUrl));
  _prefs.getString("sha", _otaSha256, sizeof(_otaSha256));
  _prefs.getString("err", _lastError, sizeof(_lastError));

  if (_lastCheckOk && _latestVersion[0] != '\0') {
    _updateAvailable = compareVersions(_latestVersion, SOUNDPANEL7_VERSION) > 0;
  } else {
    _updateAvailable = false;
  }
}

void ReleaseUpdateManager::savePersistedState() {
  if (!_prefsReady) return;

  _prefs.putBool("checked", _hasChecked);
  _prefs.putBool("ok", _lastCheckOk);
  _prefs.putInt("http", _lastHttpCode);
  _prefs.putUInt("ts", _lastCheckUnix);
  _prefs.putString("latest", _latestVersion);
  _prefs.putString("pub", _publishedAt);
  _prefs.putString("rel", _releaseUrl);
  _prefs.putString("ota", _otaUrl);
  _prefs.putString("sha", _otaSha256);
  _prefs.putString("err", _lastError);
}

bool ReleaseUpdateManager::startInstall() {
  if (_busy || _installInProgress) {
    setInstallStatus("busy");
    setInstallError("install already running");
    return false;
  }
  if (!_lastCheckOk) {
    setInstallStatus("idle");
    setInstallError("check updates first");
    return false;
  }
  if (!_updateAvailable) {
    setInstallStatus("idle");
    setInstallError("no update available");
    return false;
  }
  if (!_otaUrl[0] || strlen(_otaSha256) != 64) {
    setInstallStatus("idle");
    setInstallError("release manifest incomplete");
    return false;
  }
  if (!WiFi.isConnected()) {
    setInstallStatus("idle");
    setInstallError("wifi not connected");
    return false;
  }

  _installInProgress = true;
  _installFinished = false;
  _installSucceeded = false;
  _installStartedUnix = currentUnixTimestamp();
  _installFinishedUnix = 0;
  _installTotalBytes = 0;
  _installWrittenBytes = 0;
  _installProgressPct = 0;
  _installError[0] = '\0';
  _installRebootPending = false;
  _installRebootAtMs = 0;
  setInstallStatus("starting");

  BaseType_t created = xTaskCreatePinnedToCore(
      &ReleaseUpdateManager::installTaskEntry,
      "sp7_release_ota",
      14336,
      this,
      1,
      &_installTask,
      1);
  if (created != pdPASS) {
    _installTask = nullptr;
    finishInstall(false, "failed to start install task");
    return false;
  }
  return true;
}

void ReleaseUpdateManager::installTaskEntry(void* self) {
  ReleaseUpdateManager* manager = static_cast<ReleaseUpdateManager*>(self);
  if (manager) {
    const bool ok = manager->runInstall();
    if (ok) {
      manager->finishInstall(true);
    } else if (manager->installInProgress()) {
      manager->finishInstall(false, manager->installError()[0] ? manager->installError() : "install failed");
    }
    manager->_installTask = nullptr;
  }
  vTaskDelete(nullptr);
}

bool ReleaseUpdateManager::runInstall() {
  if (!WiFi.isConnected()) {
    setInstallError("wifi disconnected");
    return false;
  }

  setInstallStatus("downloading");
  WiFi.setSleep(false);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(kHttpTimeoutMs);

  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(kHttpConnectTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setUserAgent(String("SoundPanel7/") + SOUNDPANEL7_VERSION);
  if (!http.begin(client, _otaUrl)) {
    setInstallError("ota request init failed");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    String error = "ota http ";
    error += String(code);
    if (code < 0) {
      error += " ";
      error += http.errorToString(code);
    }
    http.end();
    setInstallError(error);
    return false;
  }

  const int totalSize = http.getSize();
  if (totalSize <= 0) {
    http.end();
    setInstallError("ota size missing");
    return false;
  }

  _installTotalBytes = (uint32_t)totalSize;
  _installWrittenBytes = 0;
  _installProgressPct = 0;

  if (!Update.begin((size_t)totalSize, U_FLASH)) {
    http.end();
    setInstallError(String("update begin failed: ") + Update.errorString());
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);

  NetworkClient* stream = http.getStreamPtr();
  uint8_t buffer[4096];
  size_t writtenTotal = 0;
  bool streamOk = true;
  while (writtenTotal < (size_t)totalSize) {
    const size_t targetRead = min(sizeof(buffer), (size_t)totalSize - writtenTotal);
    const size_t readLen = stream->readBytes(buffer, targetRead);
    if (readLen == 0) {
      streamOk = false;
      break;
    }

    mbedtls_sha256_update(&sha, buffer, readLen);
    const size_t written = Update.write(buffer, readLen);
    if (written != readLen) {
      mbedtls_sha256_free(&sha);
      Update.abort();
      http.end();
      setInstallError(String("update write failed: ") + Update.errorString());
      return false;
    }

    writtenTotal += written;
    _installWrittenBytes = (uint32_t)writtenTotal;
    _installProgressPct = (uint8_t)((writtenTotal * 100U) / (size_t)totalSize);
    vTaskDelay(pdMS_TO_TICKS(1));
  }

  http.end();

  if (!streamOk || writtenTotal != (size_t)totalSize) {
    mbedtls_sha256_free(&sha);
    Update.abort();
    setInstallError("ota stream interrupted");
    return false;
  }

  uint8_t digest[32] = {0};
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);

  setInstallStatus("verifying");
  const String actualSha = sha256Hex(digest);
  if (!actualSha.equalsIgnoreCase(String(_otaSha256))) {
    Update.abort();
    setInstallError("sha256 mismatch");
    return false;
  }

  if (!Update.end()) {
    setInstallError(String("update finalize failed: ") + Update.errorString());
    return false;
  }

  _installWrittenBytes = _installTotalBytes;
  _installProgressPct = 100;
  setInstallStatus("rebooting");
  return true;
}

bool ReleaseUpdateManager::fetchManifest(String& payload) {
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(kHttpConnectTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setUserAgent(String("SoundPanel7/") + SOUNDPANEL7_VERSION);

  if (!http.begin(client, manifestUrl())) {
    setError("manifest request init failed");
    return false;
  }

  const int code = http.GET();
  _lastHttpCode = code;
  if (code != HTTP_CODE_OK) {
    String error = "manifest http ";
    error += String(code);
    if (code < 0) {
      error += " ";
      error += http.errorToString(code);
    }
    http.end();
    setError(error);
    return false;
  }

  payload = http.getString();
  http.end();

  if (payload.length() == 0) {
    setError("empty manifest");
    return false;
  }

  return true;
}

bool ReleaseUpdateManager::parseManifest(const String& payload) {
  String version = sp7json::parseString(payload, "version", "", false);
  String publishedAt = sp7json::parseString(payload, "published_at", "", false);
  String releaseUrl = sp7json::parseString(payload, "release_url", "", false);
  String otaUrl = sp7json::parseString(payload, "url", "", false);
  String otaSha256 = sp7json::parseString(payload, "sha256", "", false);

  if (version.isEmpty()) {
    setError("manifest missing version");
    return false;
  }
  if (releaseUrl.isEmpty()) {
    setError("manifest missing release_url");
    return false;
  }
  if (otaUrl.isEmpty()) {
    setError("manifest missing ota.url");
    return false;
  }
  if (otaSha256.length() != 64) {
    setError("manifest bad ota.sha256");
    return false;
  }

  if (!sp7json::safeCopy(_latestVersion, sizeof(_latestVersion), version)) {
    setError("version too long");
    return false;
  }
  if (!sp7json::safeCopy(_publishedAt, sizeof(_publishedAt), publishedAt)) {
    setError("published_at too long");
    return false;
  }
  if (!sp7json::safeCopy(_releaseUrl, sizeof(_releaseUrl), releaseUrl)) {
    setError("release_url too long");
    return false;
  }
  if (!sp7json::safeCopy(_otaUrl, sizeof(_otaUrl), otaUrl)) {
    setError("ota url too long");
    return false;
  }
  if (!sp7json::safeCopy(_otaSha256, sizeof(_otaSha256), otaSha256)) {
    setError("sha256 too long");
    return false;
  }

  _updateAvailable = compareVersions(_latestVersion, SOUNDPANEL7_VERSION) > 0;
  _lastCheckOk = true;
  _lastError[0] = '\0';
  return true;
}

bool ReleaseUpdateManager::checkNow() {
  if (_installInProgress) {
    setError("install in progress");
    return false;
  }
  if (_busy) {
    setError("check already running");
    _hasChecked = true;
    _lastCheckUnix = currentUnixTimestamp();
    savePersistedState();
    return false;
  }

  _busy = true;
  clearResult();

  if (!_net || !WiFi.isConnected()) {
    setError("wifi not connected");
    _hasChecked = true;
    _lastCheckUnix = currentUnixTimestamp();
    savePersistedState();
    _busy = false;
    return false;
  }

  String payload;
  const bool fetched = fetchManifest(payload);
  if (fetched) {
    parseManifest(payload);
  }

  _hasChecked = true;
  _lastCheckUnix = currentUnixTimestamp();
  savePersistedState();
  _busy = false;
  return _lastCheckOk;
}

uint32_t ReleaseUpdateManager::currentUnixTimestamp() {
  const time_t now = time(nullptr);
  return now > 946684800 ? (uint32_t)now : 0U;
}

bool ReleaseUpdateManager::readNextVersionNumber(const char*& cursor, uint32_t& value, bool& hasValue) {
  hasValue = false;
  value = 0;
  while (*cursor && ((*cursor < '0' || *cursor > '9'))) {
    cursor++;
  }
  while (*cursor >= '0' && *cursor <= '9') {
    hasValue = true;
    value = (value * 10U) + (uint32_t)(*cursor - '0');
    cursor++;
  }
  return *cursor != '\0';
}

int ReleaseUpdateManager::compareVersions(const char* a, const char* b) {
  if (!a) a = "";
  if (!b) b = "";

  const char* pa = a;
  const char* pb = b;
  while (*pa || *pb) {
    uint32_t va = 0;
    uint32_t vb = 0;
    bool hasA = false;
    bool hasB = false;
    readNextVersionNumber(pa, va, hasA);
    readNextVersionNumber(pb, vb, hasB);
    if (va != vb) return va > vb ? 1 : -1;
    if (!hasA && !hasB) break;
  }
  return 0;
}
