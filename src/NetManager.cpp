// src/NetManager.cpp
#include "NetManager.h"

#include <WiFiManager.h>   // tzapu WiFiManager (AutoConnect portal)
#include <ESPmDNS.h>
#include <esp_sntp.h>
#include <time.h>
#include <sys/time.h>

#include "AppConfig.h"

static constexpr const char* kDefaultHostname = "soundpanel7";
static WiFiManager g_wm;
static wl_status_t g_lastWifiStatus = WL_IDLE_STATUS;
static String g_lastWifiIp;

bool NetManager::begin(SettingsV1* settings) {
  _s = settings;
  _started = true;
  _mdnsStarted = false;

  // Hostname (STA)
  const char* hostname = (_s && _s->hostname[0] != '\0') ? _s->hostname : kDefaultHostname;
  WiFi.setHostname(hostname);
  g_wm.setHostname(hostname);

  // WiFiManager behavior
  g_wm.setDebugOutput(false);
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

void NetManager::ensureMdns() {
  if (_mdnsStarted || !isWifiConnected()) return;

  const char* hostname = (_s && _s->hostname[0] != '\0') ? _s->hostname : kDefaultHostname;
  if (!MDNS.begin(hostname)) {
    Serial0.printf("[Net] mDNS start failed for %s\n", hostname);
    return;
  }

  const String mac = WiFi.macAddress();
  MDNS.setInstanceName(String("SoundPanel 7 ") + mac);
  MDNS.addService("soundpanel7", "tcp", 80);
  MDNS.addServiceTxt("soundpanel7", "tcp", "path", "/");
  MDNS.addServiceTxt("soundpanel7", "tcp", "api_path", "/api/status");
  MDNS.addServiceTxt("soundpanel7", "tcp", "name", hostname);
  MDNS.addServiceTxt("soundpanel7", "tcp", "model", "SoundPanel 7");
  MDNS.addServiceTxt("soundpanel7", "tcp", "manufacturer", "JJ");
  MDNS.addServiceTxt("soundpanel7", "tcp", "version", SOUNDPANEL7_VERSION);
  MDNS.addServiceTxt("soundpanel7", "tcp", "mac", mac);
  MDNS.addServiceTxt("soundpanel7", "tcp", "id", mac);
  _mdnsStarted = true;

  Serial0.printf("[Net] mDNS ready: http://%s.local/ (_soundpanel7._tcp)\n", hostname);
}

void NetManager::loop() {
  if (!_started) return;

  // fait tourner WiFiManager (important en mode non-bloquant)
  g_wm.process();

  wl_status_t wifiStatus = WiFi.status();
  String currentIp = isWifiConnected() ? WiFi.localIP().toString() : String("");
  if (wifiStatus != g_lastWifiStatus || currentIp != g_lastWifiIp) {
    g_lastWifiStatus = wifiStatus;
    g_lastWifiIp = currentIp;

    if (isWifiConnected()) {
      Serial0.printf("[Net] WiFi OK IP=%s RSSI=%ld\n",
                     currentIp.c_str(),
                     (long)rssi());
      if (_s) {
        Serial0.printf("[Net] NTP server: %s\n", _s->ntpServer);
        Serial0.printf("[Net] TZ: %s\n", _s->tz);
      }
    } else {
      Serial0.printf("[Net] WiFi status=%d\n", (int)wifiStatus);
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

  ensureMdns();
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

const char* NetManager::ntpServer() const {
  return _s ? _s->ntpServer : "";
}

const char* NetManager::tz() const {
  return _s ? _s->tz : "";
}
