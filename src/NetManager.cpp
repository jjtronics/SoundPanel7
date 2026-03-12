// src/NetManager.cpp
#include "NetManager.h"

#include <WiFiManager.h>   // tzapu WiFiManager (AutoConnect portal)
#include <WiFiMulti.h>
#include <ESPmDNS.h>
#include <esp_sntp.h>
#include <time.h>
#include <sys/time.h>

#include "AppConfig.h"

static constexpr const char* kDefaultHostname = "soundpanel7";
static constexpr const char* kSetupApName = "SoundPanel7-Setup";
static constexpr uint32_t kWifiRetryPeriodMs = 15000UL;
static constexpr uint32_t kWifiConnectTimeoutMs = 8000UL;
static constexpr uint8_t kWifiFailuresBeforePortal = 3;
static WiFiManager g_wm;
static WiFiMulti g_wifiMulti;
static wl_status_t g_lastWifiStatus = WL_IDLE_STATUS;
static String g_lastWifiIp;

bool NetManager::begin(SettingsV1* settings, SettingsStore* store) {
  _s = settings;
  _store = store;
  _started = true;
  _mdnsStarted = false;
  _lastWifiAttemptMs = 0;
  _wifiAttemptFailures = 0;
  _legacyCredentialTried = false;

  configureHostname();

  // WiFiManager behavior
  g_wm.setDebugOutput(false);
  g_wm.setConfigPortalBlocking(false); // IMPORTANT: non-bloquant, on fera g_wm.process() dans loop()
  g_wm.setEnableConfigPortal(false);   // on ouvre le portail explicitement apres les tentatives multi-AP
  g_wm.setSaveConfigCallback([this]() { onPortalWifiSaved(); });

  // Optionnel mais pratique pour éviter des waits interminables
  g_wm.setConnectTimeout(20);          // sec
  g_wm.setConfigPortalTimeout(0);      // 0 = jamais timeout du portal

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);

  _ntpConfigured = false;
  migrateLegacyCredentialIfNeeded();
  rebuildWifiMulti();
  ensureWifiConnection(true);

  return true;
}

void NetManager::configureHostname() {
  const char* hostname = (_s && _s->hostname[0] != '\0') ? _s->hostname : kDefaultHostname;
  WiFi.setHostname(hostname);
  g_wm.setHostname(hostname);
}

uint8_t NetManager::wifiCredentialCount() const {
  uint8_t count = 0;
  if (!_s) return count;
  for (const WifiCredentialRecord& credential : _s->wifiCredentials) {
    if (credential.ssid[0]) count++;
  }
  return count;
}

void NetManager::rebuildWifiMulti() {
  g_wifiMulti.APlistClean();
  if (!_s) return;

  for (const WifiCredentialRecord& credential : _s->wifiCredentials) {
    if (!credential.ssid[0]) continue;
    g_wifiMulti.addAP(credential.ssid, credential.password[0] ? credential.password : nullptr);
  }
}

void NetManager::startConfigPortal() {
  if (g_wm.getConfigPortalActive()) return;
  Serial0.printf("[Net] Starting WiFi portal '%s'\n", kSetupApName);
  g_wm.startConfigPortal(kSetupApName);
}

void NetManager::reloadWifiConfig() {
  configureHostname();
  migrateLegacyCredentialIfNeeded();
  rebuildWifiMulti();
  _ntpConfigured = false;
  _mdnsStarted = false;
  _legacyCredentialTried = false;
  _wifiAttemptFailures = 0;
  _lastWifiAttemptMs = 0;
  g_lastWifiStatus = WL_IDLE_STATUS;
  g_lastWifiIp = "";

  if (g_wm.getConfigPortalActive()) {
    g_wm.stopConfigPortal();
  }

  if (WiFi.getMode() != WIFI_STA) {
    WiFi.mode(WIFI_STA);
  }

  bool currentConnectionManaged = false;
  if (isWifiConnected() && _s) {
    const String ssid = WiFi.SSID();
    for (const WifiCredentialRecord& credential : _s->wifiCredentials) {
      if (credential.ssid[0] && ssid == credential.ssid) {
        currentConnectionManaged = true;
        break;
      }
    }
  }
  if (isWifiConnected() && wifiCredentialCount() > 0 && !currentConnectionManaged) {
    WiFi.disconnect(false, false);
  }

  ensureWifiConnection(true);
}

void NetManager::migrateLegacyCredentialIfNeeded() {
  if (!_s || !_store) return;
  if (wifiCredentialCount() > 0) return;

  const String ssid = g_wm.getWiFiSSID(true);
  if (!ssid.length()) return;

  const String password = g_wm.getWiFiPass(true);
  Serial0.printf("[Net] Migrating legacy WiFi credential for SSID=%s\n", ssid.c_str());
  rememberWifiCredential(ssid, password);
}

void NetManager::rememberWifiCredential(const String& ssid, const String& password) {
  if (!_s || !_store || !ssid.length()) return;
  if (ssid.length() >= sizeof(_s->wifiCredentials[0].ssid) || password.length() >= sizeof(_s->wifiCredentials[0].password)) {
    Serial0.println("[Net] WiFi credential skipped: too long");
    return;
  }

  int existingIndex = -1;
  int freeIndex = -1;
  for (uint8_t i = 0; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
    if (_s->wifiCredentials[i].ssid[0] == '\0') {
      if (freeIndex < 0) freeIndex = i;
      continue;
    }
    if (ssid == _s->wifiCredentials[i].ssid) {
      existingIndex = i;
      break;
    }
  }

  WifiCredentialRecord credential{};
  memcpy(credential.ssid, ssid.c_str(), ssid.length() + 1);
  memcpy(credential.password, password.c_str(), password.length() + 1);

  if (existingIndex >= 0) {
    _s->wifiCredentials[existingIndex] = credential;
  } else if (freeIndex >= 0) {
    _s->wifiCredentials[freeIndex] = credential;
  } else {
    for (uint8_t i = 1; i < WIFI_CREDENTIAL_MAX_COUNT; i++) {
      _s->wifiCredentials[i - 1] = _s->wifiCredentials[i];
    }
    _s->wifiCredentials[WIFI_CREDENTIAL_MAX_COUNT - 1] = credential;
  }

  _store->save(*_s);
  rebuildWifiMulti();
  Serial0.printf("[Net] WiFi credential stored for SSID=%s\n", ssid.c_str());
}

void NetManager::onPortalWifiSaved() {
  const String ssid = g_wm.getWiFiSSID(true);
  const String password = g_wm.getWiFiPass(true);
  rememberWifiCredential(ssid, password);
  _legacyCredentialTried = true;
  _wifiAttemptFailures = 0;
}

void NetManager::ensureWifiConnection(bool force) {
  if (!_started || isWifiConnected() || g_wm.getConfigPortalActive()) return;

  const uint32_t now = millis();
  if (!force && (uint32_t)(now - _lastWifiAttemptMs) < kWifiRetryPeriodMs) return;
  _lastWifiAttemptMs = now;

  const uint8_t credentialCount = wifiCredentialCount();
  if (credentialCount > 0) {
    Serial0.printf("[Net] Trying %u saved WiFi AP(s)\n", (unsigned)credentialCount);
    const uint8_t status = g_wifiMulti.run(kWifiConnectTimeoutMs);
    if (status == WL_CONNECTED || isWifiConnected()) {
      _wifiAttemptFailures = 0;
      _legacyCredentialTried = true;
      return;
    }

    _wifiAttemptFailures++;
    Serial0.printf("[Net] WiFiMulti status=%d (attempt %u/%u)\n",
                   (int)status,
                   (unsigned)_wifiAttemptFailures,
                   (unsigned)kWifiFailuresBeforePortal);
    if (_wifiAttemptFailures >= kWifiFailuresBeforePortal) {
      startConfigPortal();
    }
    return;
  }

  if (!_legacyCredentialTried) {
    Serial0.println("[Net] Trying legacy WiFiManager credential");
    _legacyCredentialTried = true;
    if (g_wm.autoConnect(kSetupApName) || isWifiConnected()) {
      _wifiAttemptFailures = 0;
      return;
    }
    Serial0.println("[Net] No legacy WiFi credential available");
  }

  startConfigPortal();
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
  MDNS.addServiceTxt("soundpanel7", "tcp", "api_path", "/api/ha/status");
  MDNS.addServiceTxt("soundpanel7", "tcp", "auth", "bearer");
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
  ensureWifiConnection();

  wl_status_t wifiStatus = WiFi.status();
  String currentIp = isWifiConnected() ? WiFi.localIP().toString() : String("");
  if (wifiStatus != g_lastWifiStatus || currentIp != g_lastWifiIp) {
    g_lastWifiStatus = wifiStatus;
    g_lastWifiIp = currentIp;

    if (isWifiConnected()) {
      _wifiAttemptFailures = 0;
      Serial0.printf("[Net] WiFi OK IP=%s RSSI=%ld\n",
                     currentIp.c_str(),
                     (long)rssi());
      Serial0.printf("[Net] WiFi SSID=%s\n", currentSsid().c_str());
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

String NetManager::currentSsid() const {
  if (!isWifiConnected()) return String("");
  return WiFi.SSID();
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
