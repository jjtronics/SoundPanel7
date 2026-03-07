#include "OtaManager.h"

#include <WiFi.h>
#include <ArduinoOTA.h>

bool OtaManager::begin(SettingsV1* settings) {
  _s = settings;
  _started = false;

  if (!_s) {
    Serial0.println("[OTA] settings=null");
    return false;
  }

  if (!_s->otaEnabled) {
    Serial0.println("[OTA] disabled");
    return false;
  }

  if (!WiFi.isConnected()) {
    Serial0.println("[OTA] WiFi not connected, OTA not started");
    return false;
  }

  ArduinoOTA.setHostname(_s->otaHostname);

  if (_s->otaPort > 0) {
    ArduinoOTA.setPort(_s->otaPort);
  }

  if (strlen(_s->otaPassword) > 0) {
    ArduinoOTA.setPassword(_s->otaPassword);
  }

  ArduinoOTA
    .onStart([]() {
      Serial0.println("[OTA] Start");
    })
    .onEnd([]() {
      Serial0.println("\n[OTA] End");
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      unsigned pct = (total == 0) ? 0 : (progress * 100U) / total;
      Serial0.printf("[OTA] Progress: %u%%\r", pct);
    })
    .onError([](ota_error_t error) {
      Serial0.printf("[OTA] Error[%u]\n", (unsigned)error);
    });

  ArduinoOTA.begin();
  _started = true;

  Serial0.printf("[OTA] ready on %s:%u hostname=%s\n",
                 WiFi.localIP().toString().c_str(),
                 (unsigned)_s->otaPort,
                 _s->otaHostname);
  return true;
}

void OtaManager::loop() {
  if (_started) {
    ArduinoOTA.handle();
  }
}