#pragma once

#include <Arduino.h>

class NetManager;

class ReleaseUpdateManager {
public:
  bool begin(NetManager* net);
  void loop();

  bool checkNow();

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

private:
  static constexpr uint16_t kHttpConnectTimeoutMs = 5000;
  static constexpr uint16_t kHttpTimeoutMs = 12000;

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

  bool fetchManifest(String& payload);
  bool parseManifest(const String& payload);
  void clearResult();
  void setError(const String& error);

  static uint32_t currentUnixTimestamp();
  static int compareVersions(const char* a, const char* b);
  static bool readNextVersionNumber(const char*& cursor, uint32_t& value, bool& hasValue);
};
