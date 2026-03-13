#include "ReleaseUpdateManager.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ctime>

#include "AppConfig.h"
#include "JsonHelpers.h"
#include "NetManager.h"

bool ReleaseUpdateManager::begin(NetManager* net) {
  _net = net;
  clearResult();
  _busy = false;
  _hasChecked = false;
  return true;
}

void ReleaseUpdateManager::loop() {
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
  if (_busy) {
    setError("check already running");
    _hasChecked = true;
    _lastCheckUnix = currentUnixTimestamp();
    return false;
  }

  _busy = true;
  clearResult();

  if (!_net || !WiFi.isConnected()) {
    setError("wifi not connected");
    _hasChecked = true;
    _lastCheckUnix = currentUnixTimestamp();
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
