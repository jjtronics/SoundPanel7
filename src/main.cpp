#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>

#include "lvgl_v8_port.h"
#include "SettingsStore.h"
#include "NetManager.h"
#include "ui/UiManager.h"
#include "WebManager.h"
#include "AudioEngine.h"
#include "SharedHistory.h"

#include "OtaManager.h"

#include "MqttManager.h"

using namespace esp_panel::board;

static Board *g_board = nullptr;

static SettingsStore g_store;
static SettingsV1 g_settings;
static NetManager g_net;
static UiManager g_ui;
static WebManager g_web;
static SharedHistory g_history;
AudioEngine g_audio;

static OtaManager g_ota;

static MqttManager g_mqtt;

void setup() {
  Serial0.begin(115200);
  delay(200);
  Serial0.println();
  Serial0.println("BOOT OK (UART0)");

  g_store.begin();

  // TEMPORAIRE : à laisser une seule fois si besoin, puis supprimer
  // g_store.factoryReset();

  g_store.load(g_settings);
  g_history.begin(&g_settings);

  g_board = new Board();
  g_board->init();

  if (!g_board->begin()) {
    Serial0.println("board->begin() failed");
    while (true) delay(1000);
  }

  lvgl_port_init(g_board->getLCD(), g_board->getTouch());

  lvgl_port_lock(-1);
  g_ui.begin(g_board, &g_settings, &g_store, &g_net, &g_history);
  lvgl_port_unlock();

  g_net.begin(&g_settings);
  g_ota.begin(&g_settings);
  g_mqtt.begin(&g_settings);
  g_web.begin(&g_store, &g_settings, &g_net, g_board, &g_history, &g_ota, &g_mqtt);

  g_audio.begin(&g_settings);

  Serial0.printf("[AUDIO] source=%s\n", AudioEngine::sourceLabel(g_settings.audioSource));
  Serial0.printf("[AUDIO] analogPin=%u rmsSamples=%u response=%s ema=%.2f peakHold=%lu ms\n",
                 g_settings.analogPin,
                 g_settings.analogRmsSamples,
                 AudioEngine::responseModeLabel(g_settings.audioResponseMode),
                 g_settings.emaAlpha,
                 (unsigned long)g_settings.peakHoldMs);

  Serial0.println("READY");
}

void loop() {
  g_net.loop();
  g_ota.loop();
  g_mqtt.loop();
  g_web.loop();

  static uint32_t lastDb = 0;
  uint32_t now = millis();

  if (now - lastDb >= 80) {
    lastDb = now;

    g_audio.update(&g_settings);
    const AudioMetrics& m = g_audio.metrics();
    g_history.update(m.dbInstant, now);

    static uint32_t lastDbg = 0;
    if (now - lastDbg >= 1000) {
      lastDbg = now;
      Serial0.printf("[AUDIO][DBG] ok=%d adcLast=%d adcMean=%u rms=%.2f pdb=%.1f db=%.1f leq=%.1f peak=%.1f\n",
                     m.analogOk ? 1 : 0,
                     m.rawAdcLast,
                     m.rawAdcMean,
                     m.rawRms,
                     m.rawPseudoDb,
                     m.dbInstant,
                     m.leq,
                     m.peak);
    }

    lvgl_port_lock(-1);
    g_ui.tick();
    g_ui.setDb(m.dbInstant, m.leq, m.peak);
    lvgl_port_unlock();

    g_web.updateMetrics(m.dbInstant, m.leq, m.peak);
    g_mqtt.updateMetrics(m.dbInstant, m.leq, m.peak);
  } else {
    lvgl_port_lock(-1);
    g_ui.tick();
    lvgl_port_unlock();
  }

  lv_timer_handler();
  delay(5);
}
