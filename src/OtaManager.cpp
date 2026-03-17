#include "OtaManager.h"

#include <WiFi.h>
#include <ArduinoOTA.h>

#include "DebugLog.h"
#include "ui/UiManager.h"

#define Serial0 DebugSerial0

static constexpr uint32_t kOtaRetryIntervalMs = 5000;

bool OtaManager::begin(SettingsV1* settings, UiManager* ui) {
  _s = settings;
  _ui = ui;
  _started = false;
  _inProgress = false;
  _rebootPending = false;
  _lastStartAttemptMs = 0;
  _rebootAtMs = 0;
  _lastLoggedProgressPct = 255;
  _lastUiProgressPct = 255;

  if (!_s) {
    Serial0.println("[OTA] settings=null");
    return false;
  }

  if (!_s->otaEnabled) {
    Serial0.println("[OTA] disabled");
    return false;
  }

  if (!WiFi.isConnected()) {
    Serial0.println("[OTA] WiFi not connected yet, OTA pending");
    return true;
  }

  return tryStart();
}

bool OtaManager::tryStart() {
  if (!_s) {
    Serial0.println("[OTA] settings=null");
    return false;
  }

  if (_started) return true;

  if (!_s->otaEnabled) {
    return false;
  }

  if (!WiFi.isConnected()) {
    return false;
  }

  WiFi.setSleep(false);
  ArduinoOTA.setHostname(_s->otaHostname);
  ArduinoOTA.setTimeout(15000);
  ArduinoOTA.setRebootOnSuccess(false);

  if (_s->otaPort > 0) {
    ArduinoOTA.setPort(_s->otaPort);
  }

  if (strlen(_s->otaPassword) > 0) {
    ArduinoOTA.setPassword(_s->otaPassword);
  }

  ArduinoOTA
    .onStart([this]() {
      _inProgress = true;
      _rebootPending = false;
      _rebootAtMs = 0;
      _lastLoggedProgressPct = 255;
      _lastUiProgressPct = 255;
      if (_ui) {
        _ui->showOtaStatusScreen("Mise a jour en cours", "Reception du firmware via OTA locale...", 0, false);
      }
      Serial0.println("[OTA] Start");
    })
    .onEnd([this]() {
      if (_ui) {
        _ui->showOtaStatusScreen("Mise a jour terminee", "Verification terminee. Redemarrage en cours...", 100, true);
      }
      _rebootPending = true;
      _rebootAtMs = millis() + kRebootDelayAfterSuccessMs;
      Serial0.println("\n[OTA] End");
    })
    .onProgress([this](unsigned int progress, unsigned int total) {
      const uint8_t pct = (total == 0) ? 0 : (uint8_t)((progress * 100U) / total);
      if (pct == _lastLoggedProgressPct) return;
      _lastLoggedProgressPct = pct;
      const uint8_t uiPct = (pct >= 100) ? 100 : (uint8_t)((pct / 5U) * 5U);
      if (_ui && (uiPct != _lastUiProgressPct || pct == 100)) {
        _lastUiProgressPct = uiPct;
        _ui->showOtaStatusScreen("Mise a jour en cours", "Ecriture du firmware en flash...", pct, false);
      }
      Serial0.printf("[OTA] Progress: %u%%\n", (unsigned)pct);
    })
    .onError([this](ota_error_t error) {
      _inProgress = false;
      _rebootPending = false;
      _rebootAtMs = 0;
      if (_ui) {
        _ui->hideOtaStatusScreen();
      }
      Serial0.printf("[OTA] Error[%u]\n", (unsigned)error);
    });

  ArduinoOTA.begin();
  _started = true;
  _lastStartAttemptMs = millis();

  Serial0.printf("[OTA] ready on %s:%u hostname=%s\n",
                 WiFi.localIP().toString().c_str(),
                 (unsigned)_s->otaPort,
                 _s->otaHostname);
  return true;
}

void OtaManager::loop() {
  if (!_s || !_s->otaEnabled) return;

  if (_rebootPending) {
    if ((int32_t)(millis() - _rebootAtMs) >= 0) {
      Serial0.println("[OTA] Reboot");
      delay(20);
      ESP.restart();
    }
    return;
  }

  if (!_started) {
    uint32_t now = millis();
    if (_lastStartAttemptMs == 0 || (now - _lastStartAttemptMs) >= kOtaRetryIntervalMs) {
      _lastStartAttemptMs = now;
      if (WiFi.isConnected()) {
        tryStart();
      }
    }
    return;
  }

  ArduinoOTA.handle();
}
