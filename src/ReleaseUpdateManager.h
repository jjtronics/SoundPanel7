#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>
#include <mbedtls/sha256.h>

class NetManager;
class UiManager;

class ReleaseUpdateManager {
public:
  bool begin(NetManager* net, UiManager* ui);
  void loop();

  bool checkNow();
  bool startInstall();

  bool busy() const { return _busy; }
  bool hasChecked() const { return _hasChecked; }
  bool lastCheckOk() const { return _lastCheckOk; }
  bool updateAvailable() const { return _updateAvailable; }
  int lastHttpCode() const { return _lastHttpCode; }
  uint32_t lastCheckUnix() const { return _lastCheckUnix; }

  const char* manifestUrl() const;
  const char* currentVersion() const;
  const char* latestVersion() const { return _latestVersion; }
  const char* publishedAt() const { return _publishedAt; }
  const char* releaseUrl() const { return _releaseUrl; }
  const char* otaUrl() const { return _otaUrl; }
  const char* otaSha256() const { return _otaSha256; }
  const char* lastError() const { return _lastError; }
  bool installInProgress() const { return _installInProgress; }
  bool installFinished() const { return _installFinished; }
  bool installSucceeded() const { return _installSucceeded; }
  uint32_t installStartedUnix() const { return _installStartedUnix; }
  uint32_t installFinishedUnix() const { return _installFinishedUnix; }
  uint32_t installTotalBytes() const { return _installTotalBytes; }
  uint32_t installWrittenBytes() const { return _installWrittenBytes; }
  uint8_t installProgressPct() const { return _installProgressPct; }
  const char* installStatus() const { return _installStatus; }
  const char* installError() const { return _installError; }

private:
  static constexpr uint16_t kHttpConnectTimeoutMs = 5000;
  static constexpr uint16_t kHttpTimeoutMs = 12000;
  static constexpr uint32_t kInstallRebootDelayMs = 2200;
  static constexpr size_t kInstallChunkSize = 1024;

  NetManager* _net = nullptr;
  bool _busy = false;
  bool _hasChecked = false;
  bool _lastCheckOk = false;
  bool _updateAvailable = false;
  int _lastHttpCode = 0;
  uint32_t _lastCheckUnix = 0;
  char _latestVersion[24] = "";
  char _publishedAt[32] = "";
  char _releaseUrl[192] = "";
  char _otaUrl[256] = "";
  char _otaSha256[65] = "";
  char _lastError[128] = "";
  volatile bool _installInProgress = false;
  volatile bool _installFinished = false;
  volatile bool _installSucceeded = false;
  volatile uint32_t _installStartedUnix = 0;
  volatile uint32_t _installFinishedUnix = 0;
  volatile uint32_t _installTotalBytes = 0;
  volatile uint32_t _installWrittenBytes = 0;
  volatile uint8_t _installProgressPct = 0;
  char _installStatus[32] = "idle";
  char _installError[128] = "";
  bool _installRebootPending = false;
  uint32_t _installRebootAtMs = 0;

  bool fetchManifest(String& payload);
  bool fetchUrl(const String& url, String& payload, int& httpCode, String& error);
  bool parseLatestReleaseApiPayload(const String& payload);
  bool fetchManifestViaLatestReleaseApi(String& payload, int& httpCode, String& error);
  bool parseManifest(const String& payload);
  void clearResult();
  void setError(const String& error);
  void loadPersistedState();
  void savePersistedState();
  void clearInstallState();
  void setInstallStatus(const char* status);
  void setInstallError(const String& error);
  void finishInstall(bool ok, const String& error = "");
  void cleanupInstallTransport();
  void processInstall();
  void updateInstallUi(bool force = false);

  static uint32_t currentUnixTimestamp();
  static int compareVersions(const char* a, const char* b);
  static bool readNextVersionNumber(const char*& cursor, uint32_t& value, bool& hasValue);

  WiFiClientSecure* _installClient = nullptr;
  HTTPClient* _installHttp = nullptr;
  UiManager* _ui = nullptr;
  mbedtls_sha256_context _installSha = {};
  bool _installShaActive = false;
  uint8_t _installBuffer[kInstallChunkSize] = {};
  Preferences _prefs;
  bool _prefsReady = false;
  uint8_t _lastInstallUiProgressPct = 255;
  uint8_t _lastInstallLoggedProgressPct = 255;
};
