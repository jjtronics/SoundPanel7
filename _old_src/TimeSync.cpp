#include "TimeSync.h"
#include <time.h>

void TimeSync::begin(const String& ntpServer, const String& tz) {
  if (ntpServer.length()) _ntp = ntpServer;
  if (tz.length()) _tz = tz;

  _started = false;
  _synced = false;
  _lastTryMs = 0;
  _lastCheckMs = 0;
}

void TimeSync::startNtp() {
  // Set timezone (POSIX TZ string)
  setenv("TZ", _tz.c_str(), 1);
  tzset();

  // configTime: NTP sync via LWIP SNTP
  configTime(0, 0, _ntp.c_str());
  _started = true;
  _synced = false;

  Serial0.printf("[NTP] configTime(ntp='%s', tz='%s')\n", _ntp.c_str(), _tz.c_str());
}

void TimeSync::checkSync() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    // Heuristic: consider synced if year >= 2023
    if (timeinfo.tm_year + 1900 >= 2023) {
      if (!_synced) {
        Serial0.printf("[NTP] Synced OK: %04d-%02d-%02d %02d:%02d:%02d\n",
                       timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                       timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
      }
      _synced = true;
      return;
    }
  }
  _synced = false;
}

void TimeSync::loop(bool wifiConnected) {
  const uint32_t now = millis();

  if (!wifiConnected) {
    // If Wi-Fi drops, keep last time but mark unsynced until rechecked
    _started = false;
    _synced = false;
    return;
  }

  // Start NTP once when Wi-Fi becomes available, then retry if not synced
  if (!_started) {
    startNtp();
    _lastTryMs = now;
  }

  // Check sync every ~1s
  if (now - _lastCheckMs > 1000) {
    _lastCheckMs = now;
    checkSync();
  }

  // If not synced, retry configTime every 30s (in case DNS/NTP blocked initially)
  if (!_synced && (now - _lastTryMs > 30000)) {
    Serial0.println("[NTP] Not synced yet -> retry");
    startNtp();
    _lastTryMs = now;
  }
}

String TimeSync::nowHHMM() const {
  struct tm t;
  if (!getLocalTime(&t, 0)) return "--:--";
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}

String TimeSync::nowFull() const {
  struct tm t;
  if (!getLocalTime(&t, 0)) return "---- -- --:--:--";
  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}