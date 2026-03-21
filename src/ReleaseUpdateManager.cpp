#include "ReleaseUpdateManager.h"

#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <ctime>
#include <mbedtls/sha256.h>

#include "AppConfig.h"
#include "DebugLog.h"
#include "JsonHelpers.h"
#include "NetManager.h"
#include "TrustedCerts.h"
#include "ui/UiManager.h"

#define Serial0 DebugSerial0

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

String stripLeadingVersionPrefix(const String& tag) {
  if (tag.startsWith("v") || tag.startsWith("V")) {
    return tag.substring(1);
  }
  return tag;
}

int findAssetStart(const String& payload, const char* assetName) {
  if (!assetName || !assetName[0]) return -1;

  String withSpace = String("\"name\": \"") + assetName + "\"";
  int pos = payload.indexOf(withSpace);
  if (pos >= 0) return pos;

  String withoutSpace = String("\"name\":\"") + assetName + "\"";
  return payload.indexOf(withoutSpace);
}

String extractReleaseAssetField(const String& payload, const char* assetName, const char* fieldName, const String& def = "") {
  const int assetPos = findAssetStart(payload, assetName);
  if (assetPos < 0) return def;

  int nextAssetPos = payload.indexOf("\"name\": \"", assetPos + 1);
  if (nextAssetPos < 0) nextAssetPos = payload.indexOf("\"name\":\"", assetPos + 1);
  if (nextAssetPos < 0) nextAssetPos = payload.indexOf("]", assetPos + 1);
  if (nextAssetPos < 0) nextAssetPos = payload.length();

  const String scope = payload.substring(assetPos, nextAssetPos);
  return sp7json::parseString(scope, fieldName, def, false);
}

const char* preferredReleaseFirmwareAssetName() {
#if defined(BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7B)
  return "soundpanel7b_ota-firmware.bin";
#elif SOUNDPANEL7_HAS_SCREEN
  return "soundpanel7_ota-firmware.bin";
#else
  return "soundpanel7_headless_ota-firmware.bin";
#endif
}

bool selectManifestOtaForCurrentProfile(const String& payload, String& otaUrl, String& otaSha256) {
#if defined(BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7B)
  otaUrl = sp7json::parseString(payload, "ota7bUrl", "", false);
  otaSha256 = sp7json::parseString(payload, "ota7bSha256", "", false);
  if (!otaUrl.isEmpty() && otaSha256.length() == 64) return true;

  otaUrl = sp7json::parseString(payload, "ota_7b.url", "", false);
  otaSha256 = sp7json::parseString(payload, "ota_7b.sha256", "", false);
#elif SOUNDPANEL7_HAS_SCREEN
  otaUrl = sp7json::parseString(payload, "otaScreenUrl", "", false);
  otaSha256 = sp7json::parseString(payload, "otaScreenSha256", "", false);
#else
  otaUrl = sp7json::parseString(payload, "otaHeadlessUrl", "", false);
  otaSha256 = sp7json::parseString(payload, "otaHeadlessSha256", "", false);
#endif
  if (!otaUrl.isEmpty() && otaSha256.length() == 64) return true;

  otaUrl = sp7json::parseString(payload, "url", "", false);
  otaSha256 = sp7json::parseString(payload, "sha256", "", false);
  return !otaUrl.isEmpty() && otaSha256.length() == 64;
}
}

bool ReleaseUpdateManager::begin(NetManager* net, UiManager* ui) {
  _net = net;
  _ui = ui;
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
  _lastInstallUiProgressPct = 255;
  _lastInstallLoggedProgressPct = 255;
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

void ReleaseUpdateManager::updateInstallUi(bool force) {
  if (!_ui) return;

  uint8_t uiPct = _installProgressPct;
  if (uiPct < 100) {
    uiPct = (uint8_t)((uiPct / 5U) * 5U);
  }

  const bool progressChanged = (uiPct != _lastInstallUiProgressPct);
  if (!force && !progressChanged) return;
  _lastInstallUiProgressPct = uiPct;

  const char* title = "Mise a jour en cours";
  const char* detail = "Preparation de la mise a jour GitHub...";
  bool success = false;

  if (strcmp(_installStatus, "starting") == 0) {
    detail = "Preparation de la mise a jour GitHub...";
  } else if (strcmp(_installStatus, "downloading") == 0) {
    detail = "Telechargement du firmware depuis GitHub...";
  } else if (strcmp(_installStatus, "verifying") == 0) {
    detail = "Verification du firmware...";
  } else if (strcmp(_installStatus, "rebooting") == 0 || strcmp(_installStatus, "success") == 0) {
    title = "Mise a jour terminee";
    detail = "Verification terminee. Redemarrage en cours...";
    success = true;
    uiPct = 100;
    _lastInstallUiProgressPct = 100;
  } else if (strcmp(_installStatus, "failed") == 0) {
    _ui->hideOtaStatusScreen();
    return;
  }

  _ui->showOtaStatusScreen(title, detail, uiPct, success);
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
    updateInstallUi(true);
  } else {
    setInstallStatus("failed");
    setInstallError(error);
    _installRebootPending = false;
    if (_ui) {
      _ui->hideOtaStatusScreen();
    }
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

bool ReleaseUpdateManager::startInstall(bool force) {
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
  if (!_updateAvailable && !force) {
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
  _lastInstallUiProgressPct = 255;
  _lastInstallLoggedProgressPct = 255;
  setInstallStatus("starting");
  updateInstallUi(true);
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
    updateInstallUi(true);
    WiFi.setSleep(false);

    _installClient = new WiFiClientSecure();
    _installHttp = new HTTPClient();
    if (!_installClient || !_installHttp) {
      cleanupInstallTransport();
      finishInstall(false, "not enough memory for ota");
      return;
    }

    // SECURITY: Validate HTTPS certificates against trusted CA bundle
    configureSecureClient(*_installClient);
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
    updateInstallUi(true);
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
  if (_installProgressPct != _lastInstallLoggedProgressPct) {
    _lastInstallLoggedProgressPct = _installProgressPct;
    Serial0.printf("[REL] Install progress: %u%%\n", (unsigned)_installProgressPct);
  }
  updateInstallUi();
}

bool ReleaseUpdateManager::fetchManifest(String& payload) {
  String releasePayload;
  String apiError;
  if (fetchUrl(String(SOUNDPANEL7_RELEASE_LATEST_API_URL), releasePayload, _lastHttpCode, apiError)) {
    if (parseLatestReleaseApiPayload(releasePayload)) {
      payload = "";
      return true;
    }
    apiError = _lastError[0] ? String(_lastError) : String("latest release parse failed");
  } else if (apiError.length()) {
    apiError = "release api " + apiError;
  }

  String error;
  if (fetchUrl(String(manifestUrl()), payload, _lastHttpCode, error)) {
    return true;
  }

  if (apiError.length()) {
    if (error.length()) {
      setError(apiError + " / fallback manifest " + error);
    } else {
      setError(apiError);
    }
  } else if (error.length()) {
    setError("manifest " + error);
  } else {
    setError("release check failed");
  }
  return false;
}

bool ReleaseUpdateManager::fetchUrl(const String& url, String& payload, int& httpCode, String& error) {
  WiFi.setSleep(false);
  WiFiClientSecure client;
  // SECURITY: Validate HTTPS certificates against trusted CA bundle
  configureSecureClient(client);

  HTTPClient http;
  http.setConnectTimeout(kHttpConnectTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setReuse(false);
  http.setUserAgent(String("SoundPanel7/") + SOUNDPANEL7_VERSION);

  if (!http.begin(client, url)) {
    httpCode = 0;
    error = "request init failed";
    return false;
  }

  if (url.startsWith("https://api.github.com/")) {
    http.addHeader("Accept", "application/vnd.github+json");
    http.addHeader("X-GitHub-Api-Version", "2022-11-28");
  }

  const int code = http.GET();
  httpCode = code;
  if (code != HTTP_CODE_OK) {
    error = "http ";
    error += String(code);
    if (code < 0) {
      error += " ";
      error += http.errorToString(code);
    }
    Serial0.printf("[REL] HTTPS fail url=%s code=%d wifi=%d ip=%s rssi=%ld heap=%lu min=%lu\n",
                   url.c_str(),
                   code,
                   (int)WiFi.status(),
                   WiFi.localIP().toString().c_str(),
                   (long)WiFi.RSSI(),
                   (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                   (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    http.end();
    setError(error);
    return false;
  }

  payload = http.getString();
  http.end();

  if (payload.length() == 0) {
    error = "empty response";
    return false;
  }

  return true;
}

bool ReleaseUpdateManager::parseLatestReleaseApiPayload(const String& payload) {
  const String tag = sp7json::parseString(payload, "tag_name", "", false);
  const String version = stripLeadingVersionPrefix(tag);
  const String publishedAt = sp7json::parseString(payload, "published_at", "", false);
  const String releaseUrl = sp7json::parseString(payload, "html_url", "", false);
  const char* preferredAssetName = preferredReleaseFirmwareAssetName();
  String otaUrl = extractReleaseAssetField(payload, preferredAssetName, "browser_download_url", "");
  String otaSha256 = extractReleaseAssetField(payload, preferredAssetName, "digest", "");

  if (otaUrl.isEmpty() || otaSha256.isEmpty()) {
    otaUrl = extractReleaseAssetField(payload, "firmware.bin", "browser_download_url", "");
    otaSha256 = extractReleaseAssetField(payload, "firmware.bin", "digest", "");
  }

  if (version.isEmpty()) {
    setError("latest release missing tag_name");
    return false;
  }
  if (releaseUrl.isEmpty()) {
    setError("latest release missing html_url");
    return false;
  }
  if (otaUrl.isEmpty()) {
    setError("latest release missing firmware asset url");
    return false;
  }
  if (otaSha256.startsWith("sha256:")) {
    otaSha256.remove(0, 7);
  }
  if (otaSha256.length() != 64) {
    setError("latest release missing firmware digest");
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

bool ReleaseUpdateManager::fetchManifestViaLatestReleaseApi(String& payload, int& httpCode, String& error) {
  String releasePayload;
  if (!fetchUrl(String(SOUNDPANEL7_RELEASE_LATEST_API_URL), releasePayload, httpCode, error)) {
    return false;
  }

  const String tag = sp7json::parseString(releasePayload, "tag_name", "", false);
  if (tag.isEmpty()) {
    error = "latest release missing tag_name";
    httpCode = 0;
    return false;
  }

  const String fallbackManifestUrl =
      String("https://github.com/jjtronics/SoundPanel7/releases/download/") + tag + "/release-manifest.json";
  return fetchUrl(fallbackManifestUrl, payload, httpCode, error);
}

bool ReleaseUpdateManager::parseManifest(const String& payload) {
  String version = sp7json::parseString(payload, "version", "", false);
  String publishedAt = sp7json::parseString(payload, "published_at", "", false);
  String releaseUrl = sp7json::parseString(payload, "release_url", "", false);
  String otaUrl;
  String otaSha256;
  selectManifestOtaForCurrentProfile(payload, otaUrl, otaSha256);

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
  if (fetched && payload.length() > 0) {
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
