#include <Arduino.h>
#include <esp_display_panel.hpp>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <lvgl.h>

#include "AppRuntimeStats.h"
#include "lvgl_v8_port.h"
#include "SettingsStore.h"
#include "NetManager.h"
#include "ui/UiManager.h"
#include "WebManager.h"
#include "AudioEngine.h"
#include "AppConfig.h"
#include "SharedHistory.h"

#include "OtaManager.h"

#include "MqttManager.h"
#include "ReleaseUpdateManager.h"

using namespace esp_panel::board;

static Board *g_board = nullptr;

static SettingsStore g_store;
static SettingsV1 g_settings;
static NetManager g_net;
static UiManager g_ui;
static WebManager g_web;
static SharedHistory g_history;
AudioEngine g_audio;
RuntimeStats g_runtimeStats;

static OtaManager g_ota;
static ReleaseUpdateManager g_releaseUpdate;

static MqttManager g_mqtt;
static constexpr bool kAudioDebugLogEnabled = false;
static constexpr uint32_t kAudioUpdatePeriodMs = 80;
static constexpr uint32_t kLvglSpikeThresholdUs = 30000;
static constexpr uint32_t kLvglSpikeLogIntervalMs = 1500;

static const char* otaStateLabel(esp_ota_img_states_t state) {
  switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    case ESP_OTA_IMG_UNDEFINED: return "undefined";
    default: return "unknown";
  }
}

static void confirmRunningOtaImage() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* boot = esp_ota_get_boot_partition();

  esp_ota_img_states_t otaState = ESP_OTA_IMG_UNDEFINED;
  esp_err_t stateErr = ESP_ERR_NOT_FOUND;
  if (running) {
    stateErr = esp_ota_get_state_partition(running, &otaState);
  }

  Serial0.printf("[BOOT] version=%s running=%s boot=%s state=%s err=%s\n",
                 SOUNDPANEL7_VERSION,
                 running ? running->label : "?",
                 boot ? boot->label : "?",
                 stateErr == ESP_OK ? otaStateLabel(otaState) : "unavailable",
                 esp_err_to_name(stateErr));

  if (stateErr == ESP_OK && otaState == ESP_OTA_IMG_PENDING_VERIFY) {
    const esp_err_t confirmErr = esp_ota_mark_app_valid_cancel_rollback();
    Serial0.printf("[BOOT] confirm running OTA image -> %s\n", esp_err_to_name(confirmErr));
  }
}

static lv_obj_tree_walk_res_t countLvglObjectsCb(lv_obj_t* obj, void* user_data) {
  (void)obj;
  uint32_t* count = static_cast<uint32_t*>(user_data);
  if (count) (*count)++;
  return LV_OBJ_TREE_WALK_NEXT;
}

static void sampleRuntimeStats() {
  static bool runtimeStatsInitialized = false;
  static configRUN_TIME_COUNTER_TYPE prevRunTimeCounter = 0;
  static configRUN_TIME_COUNTER_TYPE prevIdleCounters[configNUMBER_OF_CORES] = {0};
  const configRUN_TIME_COUNTER_TYPE runTimeCounter = portGET_RUN_TIME_COUNTER_VALUE();
  configRUN_TIME_COUNTER_TYPE idleDeltaSum = 0;

  for (BaseType_t core = 0; core < configNUMBER_OF_CORES; ++core) {
    const configRUN_TIME_COUNTER_TYPE idleCounter = ulTaskGetIdleRunTimeCounterForCore(core);
    if (runtimeStatsInitialized) {
      idleDeltaSum += (idleCounter - prevIdleCounters[core]);
    }
    prevIdleCounters[core] = idleCounter;
  }

  const configRUN_TIME_COUNTER_TYPE runTimeDelta = runTimeCounter - prevRunTimeCounter;
  prevRunTimeCounter = runTimeCounter;
  if (!runtimeStatsInitialized) {
    runtimeStatsInitialized = true;
  } else if (runTimeDelta > 0) {
    const uint64_t totalWindow = (uint64_t)runTimeDelta * (uint64_t)configNUMBER_OF_CORES;
    const uint64_t idlePct = totalWindow > 0
      ? ((uint64_t)idleDeltaSum * 100ULL) / totalWindow
      : 100ULL;
    g_runtimeStats.cpuIdlePct = (uint8_t)min<uint64_t>(idlePct, 100ULL);
    g_runtimeStats.cpuLoadPct = 100U - g_runtimeStats.cpuIdlePct;
  }

  g_runtimeStats.lvglIdlePct = lv_timer_get_idle();
  g_runtimeStats.lvglLoadPct = 100U - g_runtimeStats.lvglIdlePct;
  g_runtimeStats.heapInternalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_runtimeStats.heapInternalTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_runtimeStats.heapInternalMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_runtimeStats.heapPsramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  g_runtimeStats.heapPsramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  g_runtimeStats.heapPsramMin = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

  uint32_t count = 0;
  lv_obj_t* active = lv_scr_act();
  if (active) lv_obj_tree_walk(active, countLvglObjectsCb, &count);
  lv_obj_t* top = lv_layer_top();
  if (top) lv_obj_tree_walk(top, countLvglObjectsCb, &count);
  lv_obj_t* sys = lv_layer_sys();
  if (sys) lv_obj_tree_walk(sys, countLvglObjectsCb, &count);
  g_runtimeStats.lvObjCount = count;
}

static bool dueOnFixedPeriod(uint32_t& anchorMs, uint32_t periodMs, uint32_t nowMs) {
  if (anchorMs == 0) {
    anchorMs = nowMs;
    return true;
  }
  if ((uint32_t)(nowMs - anchorMs) < periodMs) return false;

  do {
    anchorMs += periodMs;
  } while ((uint32_t)(nowMs - anchorMs) >= periodMs);
  return true;
}

void setup() {
  Serial0.begin(115200);
  delay(200);
  Serial0.println();
  Serial0.println("BOOT OK (UART0)");
  confirmRunningOtaImage();

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

  g_net.begin(&g_settings, &g_store);
  g_ota.begin(&g_settings);
  g_releaseUpdate.begin(&g_net);
  g_mqtt.begin(&g_store, &g_settings);
  g_web.begin(&g_store, &g_settings, &g_net, g_board, &g_history, &g_ota, &g_releaseUpdate, &g_mqtt, &g_ui);

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
  static uint32_t lastLvglSpikeLogMs = 0;
  g_ota.loop();
  if (g_ota.inProgress()) {
    // OTA is timing-sensitive on the ESP32. Keep the loop as quiet as possible
    // so the upload stream and flash writes are not disturbed by UI, audio, MQTT,
    // or web activity. Let WebManager run once so it can drop live SSE clients.
    g_web.loop();
    delay(1);
    return;
  }
  g_net.loop();
  g_releaseUpdate.loop();
  if (g_releaseUpdate.installInProgress()) {
    g_web.loop();
    delay(1);
    return;
  }
  g_mqtt.loop();
  g_web.loop();

  static uint32_t audioTickAnchorMs = 0;
  uint32_t now = millis();

  if (dueOnFixedPeriod(audioTickAnchorMs, kAudioUpdatePeriodMs, now)) {
    g_audio.update(&g_settings);
    const AudioMetrics& m = g_audio.metrics();
    g_history.update(m.dbInstant, now);

    static uint32_t lastDbg = 0;
    if (kAudioDebugLogEnabled && (now - lastDbg >= 1000)) {
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

    uint32_t uiStartUs = micros();
    lvgl_port_lock(-1);
    g_ui.tick();
    g_ui.setDb(m.dbInstant, m.leq, m.peak);
    sampleRuntimeStats();
    lvgl_port_unlock();
    g_runtimeStats.uiWorkLastUs = micros() - uiStartUs;
    if (g_runtimeStats.uiWorkLastUs > g_runtimeStats.uiWorkMaxUs) {
      g_runtimeStats.uiWorkMaxUs = g_runtimeStats.uiWorkLastUs;
    }

    g_web.updateMetrics(m.dbInstant, m.leq, m.peak);
    g_mqtt.updateMetrics(m.dbInstant, m.leq, m.peak);
  } else {
    uint32_t uiStartUs = micros();
    lvgl_port_lock(-1);
    g_ui.tick();
    sampleRuntimeStats();
    lvgl_port_unlock();
    g_runtimeStats.uiWorkLastUs = micros() - uiStartUs;
    if (g_runtimeStats.uiWorkLastUs > g_runtimeStats.uiWorkMaxUs) {
      g_runtimeStats.uiWorkMaxUs = g_runtimeStats.uiWorkLastUs;
    }
  }

  uint32_t lvHandlerStartUs = micros();
  lv_timer_handler();
  g_runtimeStats.lvHandlerLastUs = micros() - lvHandlerStartUs;
  if (g_runtimeStats.lvHandlerLastUs > g_runtimeStats.lvHandlerMaxUs) {
    g_runtimeStats.lvHandlerMaxUs = g_runtimeStats.lvHandlerLastUs;
  }
  if ((g_runtimeStats.uiWorkLastUs >= kLvglSpikeThresholdUs || g_runtimeStats.lvHandlerLastUs >= kLvglSpikeThresholdUs) &&
      (now - lastLvglSpikeLogMs >= kLvglSpikeLogIntervalMs)) {
    lastLvglSpikeLogMs = now;
    Serial0.printf("[LVGL][SPIKE] page=%s load=%u%% ui=%.1fms max=%.1fms handler=%.1fms max=%.1fms obj=%lu heap=%luk\n",
                   g_runtimeStats.activePage,
                   (unsigned)g_runtimeStats.lvglLoadPct,
                   g_runtimeStats.uiWorkLastUs / 1000.0f,
                   g_runtimeStats.uiWorkMaxUs / 1000.0f,
                   g_runtimeStats.lvHandlerLastUs / 1000.0f,
                   g_runtimeStats.lvHandlerMaxUs / 1000.0f,
                   (unsigned long)g_runtimeStats.lvObjCount,
                   (unsigned long)(g_runtimeStats.heapInternalFree / 1024));
  }
  delay(5);
}
