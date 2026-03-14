#include "ReleaseUpdateManager.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctime>
#include <mbedtls/sha256.h>

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
  processInstall();
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
  cleanupInstallTransport();
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

void ReleaseUpdateManager::cleanupInstallTransport() {
  if (_installShaActive) {
    mbedtls_sha256_free(&_installSha);
    _installShaActive = false;
  }
  if (_installHttp) {
    _installHttp->end();
    delete _installHttp;
    _installHttp = nullptr;
  }
  if (_installClient) {
    delete _installClient;
    _installClient = nullptr;
  }
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
  return true;
}

void ReleaseUpdateManager::processInstall() {
  if (!_installInProgress) return;
  if (!WiFi.isConnected()) {
    cleanupInstallTransport();
    Update.abort();
    finishInstall(false, "wifi disconnected");
    return;
  }

  if (!_installHttp) {
    setInstallStatus("downloading");
    WiFi.setSleep(false);

    _installClient = new WiFiClientSecure();
    _installHttp = new HTTPClient();
    if (!_installClient || !_installHttp) {
      cleanupInstallTransport();
      finishInstall(false, "not enough memory for ota");
      return;
    }

    _installClient->setInsecure();
    _installClient->setTimeout(kHttpTimeoutMs);

    _installHttp->setConnectTimeout(kHttpConnectTimeoutMs);
    _installHttp->setTimeout(kHttpTimeoutMs);
    _installHttp->setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    _installHttp->setReuse(false);
    _installHttp->setUserAgent(String("SoundPanel7/") + SOUNDPANEL7_VERSION);
    if (!_installHttp->begin(*_installClient, _otaUrl)) {
      cleanupInstallTransport();
      finishInstall(false, "ota request init failed");
      return;
    }

    const int code = _installHttp->GET();
    if (code != HTTP_CODE_OK) {
      String error = "ota http ";
      error += String(code);
      if (code < 0) {
        error += " ";
        error += _installHttp->errorToString(code);
      }
      cleanupInstallTransport();
      finishInstall(false, error);
      return;
    }

    const int totalSize = _installHttp->getSize();
    if (totalSize <= 0) {
      cleanupInstallTransport();
      finishInstall(false, "ota size missing");
      return;
    }

    _installTotalBytes = (uint32_t)totalSize;
    _installWrittenBytes = 0;
    _installProgressPct = 0;

    if (!Update.begin((size_t)totalSize, U_FLASH)) {
      cleanupInstallTransport();
      finishInstall(false, String("update begin failed: ") + Update.errorString());
      return;
    }

    mbedtls_sha256_init(&_installSha);
    mbedtls_sha256_starts(&_installSha, 0);
    _installShaActive = true;
    return;
  }

  const size_t remaining = (size_t)_installTotalBytes - (size_t)_installWrittenBytes;
  if (remaining == 0) {
    uint8_t digest[32] = {0};
    setInstallStatus("verifying");
    mbedtls_sha256_finish(&_installSha, digest);
    cleanupInstallTransport();

    const String actualSha = sha256Hex(digest);
    if (!actualSha.equalsIgnoreCase(String(_otaSha256))) {
      Update.abort();
      finishInstall(false, "sha256 mismatch");
      return;
    }

    if (!Update.end()) {
      finishInstall(false, String("update finalize failed: ") + Update.errorString());
      return;
    }

    _installWrittenBytes = _installTotalBytes;
    _installProgressPct = 100;
    setInstallStatus("rebooting");
    finishInstall(true);
    return;
  }

  NetworkClient* stream = _installHttp->getStreamPtr();
  if (!stream) {
    cleanupInstallTransport();
    Update.abort();
    finishInstall(false, "ota stream unavailable");
    return;
  }

  const int available = stream->available();
  if (available <= 0) {
    if (!_installHttp->connected()) {
      cleanupInstallTransport();
      Update.abort();
      finishInstall(false, "ota stream interrupted");
    }
    return;
  }

  const size_t targetRead = min(kInstallChunkSize, min(remaining, (size_t)available));
  const size_t readLen = stream->readBytes(_installBuffer, targetRead);
  if (readLen == 0) {
    if (!_installHttp->connected()) {
      cleanupInstallTransport();
      Update.abort();
      finishInstall(false, "ota stream interrupted");
    }
    return;
  }

  mbedtls_sha256_update(&_installSha, _installBuffer, readLen);
  const size_t written = Update.write(_installBuffer, readLen);
  if (written != readLen) {
    cleanupInstallTransport();
    Update.abort();
    finishInstall(false, String("update write failed: ") + Update.errorString());
    return;
  }

  _installWrittenBytes += (uint32_t)written;
  _installProgressPct = (uint8_t)(((uint64_t)_installWrittenBytes * 100ULL) / (uint64_t)_installTotalBytes);
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
