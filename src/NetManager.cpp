// src/NetManager.cpp
#include "NetManager.h"

#include <WiFiManager.h>   // tzapu WiFiManager (AutoConnect portal)
#include <esp_sntp.h>
#include <time.h>
#include <sys/time.h>

static WiFiManager g_wm;

bool NetManager::begin(SettingsV1* settings) {
  _s = settings;
  _started = true;

  // Hostname (STA)
  if (_s && _s->hostname[0] != '\0') {
    WiFi.setHostname(_s->hostname);
    g_wm.setHostname(_s->hostname);
  } else {
    WiFi.setHostname("soundpanel7");
    g_wm.setHostname("soundpanel7");
  }

  // WiFiManager behavior
  g_wm.setDebugOutput(true);
  g_wm.setConfigPortalBlocking(false); // IMPORTANT: non-bloquant, on fera g_wm.process() dans loop()

  // Optionnel mais pratique pour éviter des waits interminables
  g_wm.setConnectTimeout(20);          // sec
  g_wm.setConfigPortalTimeout(0);      // 0 = jamais timeout du portal

  // Démarrage connexion
  // autoConnect() -> tente SSID sauvegardé, sinon ouvre portal
  // Le SSID du portal, on le met clair
  const char* apName = "SoundPanel7-Setup";
  bool ok = g_wm.autoConnect(apName);

  // Si ok == false ici, ça veut dire "portal démarré" (ou échec immédiat),
  // mais comme on est en non-bloquant, on continue quand même.
  (void)ok;

  // On configure NTP dès qu'on a le WiFi (ou on le fera dans loop())
  _ntpConfigured = false;

  return true;
}

void NetManager::loop() {
  if (!_started) return;

  // fait tourner WiFiManager (important en mode non-bloquant)
  g_wm.process();

  // Print de status wifi toutes ~3s max (évite spam)
  uint32_t now = millis();
  if (now - _lastPrint >= 3000) {
    _lastPrint = now;

    if (isWifiConnected()) {
      Serial0.printf("[Net] WiFi OK IP=%s RSSI=%ld\n",
                     ipString().c_str(),
                     (long)rssi());
      if (_s) {
        Serial0.printf("[Net] NTP server: %s\n", _s->ntpServer);
        Serial0.printf("[Net] TZ: %s\n", _s->tz);
      }
    } else {
      Serial0.println("[Net] WiFi not connected (portal?)");
    }
  }

  // Configure TZ + NTP une seule fois dès que WiFi OK
  if (!_ntpConfigured && isWifiConnected() && _s) {
    // 1) Appliquer TZ au runtime (IMPORTANT pour localtime/getLocalTime)
    setenv("TZ", _s->tz, 1);
    tzset();

    // 2) Démarrer NTP AVEC timezone
    // configTzTime applique la TZ + configure les serveurs NTP
    configTzTime(_s->tz, _s->ntpServer);
    sntp_set_sync_interval(_s->ntpSyncIntervalMs);
    sntp_restart();

    Serial0.printf("[Net] NTP configured (interval=%lu ms)\n",
                   (unsigned long)sntp_get_sync_interval());
    _ntpConfigured = true;
  }
}

bool NetManager::isWifiConnected() const {
  return WiFi.status() == WL_CONNECTED;
}

String NetManager::ipString() const {
  if (!isWifiConnected()) return String("0.0.0.0");
  return WiFi.localIP().toString();
}

int32_t NetManager::rssi() const {
  if (!isWifiConnected()) return -127;
  return WiFi.RSSI();
}

bool NetManager::timeIsValid() const {
  // On considère "valide" si l'epoch > 2024-01-01 environ (1704067200)
  time_t t = time(nullptr);
  return (t > 1704067200);
}

bool NetManager::localTime(struct tm* out) const {
  if (!out) return false;

  if (timeIsValid()) {
    if (getLocalTime(out, 0)) {
      _lastValidEpoch = time(nullptr);
      return true;
    }
  }

  if (_lastValidEpoch <= 0) return false;
  localtime_r(&_lastValidEpoch, out);
  return true;
}

String NetManager::timeStringLocal() const {
  struct tm ti;
  if (!localTime(&ti)) return String("NTP...");
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
  return String(buf);
}

const char* NetManager::ntpServer() const {
  return _s ? _s->ntpServer : "";
}

const char* NetManager::tz() const {
  return _s ? _s->tz : "";
}
