#pragma once
#include <Arduino.h>

class TimeSync {
public:
  // Start NTP sync. Call when Wi-Fi is connected.
  void begin(const String& ntpServer, const String& tz);

  // Call periodically (non blocking).
  void loop(bool wifiConnected);

  bool isSynced() const { return _synced; }
  String nowHHMM() const;          // "14:32"
  String nowFull() const;          // "2026-03-05 14:32:08"

private:
  String _ntp = "fr.pool.ntp.org";
  String _tz  = "CET-1CEST,M3.5.0/2,M10.5.0/3"; // Europe/Paris rules (POSIX TZ)

  bool _started = false;
  bool _synced = false;

  uint32_t _lastTryMs = 0;
  uint32_t _lastCheckMs = 0;

  void startNtp();
  void checkSync();
};