#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_display_panel.hpp>

#include "AppConfig.h"

#if SOUNDPANEL7_HAS_SCREEN
#include <lvgl.h>
#endif

#include "AppRuntimeStats.h"
#include "DebugLog.h"
#include "lvgl_v8_port.h"
#include "SettingsStore.h"
#include "NetManager.h"
#include "ui/UiManager.h"
#include "WebManager.h"
#include "AudioEngine.h"
#include "SharedHistory.h"

#include "OtaManager.h"

#include "MqttManager.h"
#include "ReleaseUpdateManager.h"

#define Serial0 DebugSerial0

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

// Audio task isolation on Core 1
static SemaphoreHandle_t g_audioMutex = nullptr;
static AudioMetrics g_audioMetricsShared = {};
static TaskHandle_t g_audioTaskHandle = nullptr;
static constexpr uint8_t kAudioTaskPriority = 20;  // HIGHEST priority - audio measurement must NEVER be blocked
static constexpr uint32_t kAudioTaskStackSize = 8192;

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

#if SOUNDPANEL7_HAS_SCREEN
static lv_obj_tree_walk_res_t countLvglObjectsCb(lv_obj_t* obj, void* user_data) {
  (void)obj;
  uint32_t* count = static_cast<uint32_t*>(user_data);
  if (count) (*count)++;
  return LV_OBJ_TREE_WALK_NEXT;
}
#endif

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

#if SOUNDPANEL7_HAS_SCREEN
  g_runtimeStats.lvglIdlePct = lv_timer_get_idle();
  g_runtimeStats.lvglLoadPct = 100U - g_runtimeStats.lvglIdlePct;
#else
  g_runtimeStats.lvglIdlePct = 100;
  g_runtimeStats.lvglLoadPct = 0;
#endif
  g_runtimeStats.heapInternalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_runtimeStats.heapInternalTotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_runtimeStats.heapInternalMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  g_runtimeStats.heapPsramFree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  g_runtimeStats.heapPsramTotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  g_runtimeStats.heapPsramMin = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

#if SOUNDPANEL7_HAS_SCREEN
  uint32_t count = 0;
  lv_obj_t* active = lv_scr_act();
  if (active) lv_obj_tree_walk(active, countLvglObjectsCb, &count);
  lv_obj_t* top = lv_layer_top();
  if (top) lv_obj_tree_walk(top, countLvglObjectsCb, &count);
  lv_obj_t* sys = lv_layer_sys();
  if (sys) lv_obj_tree_walk(sys, countLvglObjectsCb, &count);
  g_runtimeStats.lvObjCount = count;
#else
  g_runtimeStats.lvObjCount = 0;
#endif
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

// Thread-safe read of audio metrics
static AudioMetrics getAudioMetrics() {
  AudioMetrics m;
  if (g_audioMutex && xSemaphoreTake(g_audioMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    m = g_audioMetricsShared;
    xSemaphoreGive(g_audioMutex);
  }
  return m;
}

// Audio task running on Core 1 with high priority
static void audioTask(void* parameter) {
  (void)parameter;
  Serial0.printf("[AUDIO] task started on core %d priority %d\n",
                 xPortGetCoreID(),
                 uxTaskPriorityGet(nullptr));

  uint32_t audioTickAnchorMs = 0;
  uint32_t lastDbg = 0;

  while (true) {
    uint32_t now = millis();

    if (dueOnFixedPeriod(audioTickAnchorMs, kAudioUpdatePeriodMs, now)) {
      // Audio acquisition - isolated from WiFi/MQTT/Web
      g_audio.update(&g_settings);
      const AudioMetrics& m = g_audio.metrics();
      g_history.update(m.dbInstant, now);

      // Thread-safe copy for main loop
      if (g_audioMutex && xSemaphoreTake(g_audioMutex, portMAX_DELAY) == pdTRUE) {
        g_audioMetricsShared = m;
        xSemaphoreGive(g_audioMutex);
      }

      // Debug log
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
    }

    // Small delay to prevent task starvation (10ms)
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void setup() {
  Serial0.begin(115200);
  delay(200);
  Serial0.println();
  Serial0.println("BOOT OK (UART0)");
  confirmRunningOtaImage();
  Serial0.printf("[MEM] psramFound=%d INT=%luKB PSRAM=%luKB\n",
                 psramFound() ? 1 : 0,
                 (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024UL),
                 (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024UL));

  const bool storeReady = g_store.begin();
  Serial0.printf("[SET] preferences namespace=sp7 ready=%d\n", storeReady ? 1 : 0);

  // TEMPORAIRE : à laisser une seule fois si besoin, puis supprimer
  // g_store.factoryReset();

  g_store.load(g_settings);
  uint8_t wifiCredentialCount = 0;
  for (const WifiCredentialRecord& credential : g_settings.wifiCredentials) {
    if (credential.ssid[0]) wifiCredentialCount++;
  }
  Serial0.printf("[SET] ui backlight=%u touch=%u page=%u pin=%u wifiSlots=%u ota=%u slack=%u\n",
                 (unsigned)g_settings.backlight,
                 (unsigned)g_settings.touchEnabled,
                 (unsigned)g_settings.dashboardPage,
                 pinCodeIsConfigured(g_settings.dashboardPin) ? 1U : 0U,
                 (unsigned)wifiCredentialCount,
                 (unsigned)g_settings.otaEnabled,
                 (unsigned)g_settings.slackEnabled);
  g_history.begin(&g_settings);

#if SOUNDPANEL7_HAS_SCREEN
  g_board = new Board();
  Serial0.println("[BOOT] board init start");
  g_board->init();
  Serial0.println("[BOOT] board init done");

#if defined(BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_7B)
  if (auto* lcd = g_board->getLCD()) {
    Serial0.println("[BOOT] 7B forcing RGB frame buffer count=2");
    lcd->configFrameBufferNumber(2);
  }
#endif

  Serial0.println("[BOOT] board begin start");
  if (!g_board->begin()) {
    Serial0.println("board->begin() failed");
    while (true) delay(1000);
  }
  Serial0.println("[BOOT] board begin ok");

  lvgl_port_init(g_board->getLCD(), g_board->getTouch());

  lvgl_port_lock(-1);
  g_ui.begin(g_board, &g_settings, &g_store, &g_net, &g_history);
  lvgl_port_unlock();
#else
  g_ui.begin(nullptr, &g_settings, &g_store, &g_net, &g_history);
#endif

  g_net.begin(&g_settings, &g_store);
  g_ota.begin(&g_settings, &g_ui);
  g_releaseUpdate.begin(&g_net, &g_ui);
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

  // Create mutex for thread-safe audio metrics
  g_audioMutex = xSemaphoreCreateMutex();
  if (!g_audioMutex) {
    Serial0.println("[AUDIO] FATAL: mutex creation failed");
    while (true) delay(1000);
  }

  // Start audio task on Core 1 with high priority
  BaseType_t ret = xTaskCreatePinnedToCore(
    audioTask,
    "audio",
    kAudioTaskStackSize,
    nullptr,
    kAudioTaskPriority,
    &g_audioTaskHandle,
    1  // Core 1
  );

  if (ret != pdPASS || !g_audioTaskHandle) {
    Serial0.println("[AUDIO] FATAL: task creation failed");
    while (true) delay(1000);
  }

  Serial0.println("READY");
}

void loop() {
  static uint32_t lastLvglSpikeLogMs = 0;

  // Check for OTA/install in progress early
  const bool otaInProgress = g_ota.inProgress();
  const bool installInProgress = g_releaseUpdate.installInProgress();

  // Audio acquisition now runs in dedicated task on Core 1
  // Read metrics in a thread-safe way
  const AudioMetrics m = getAudioMetrics();
  uint32_t now = millis();

  // PRIORITY 1: Process touch input and UI FIRST (before any blocking operations)
#if SOUNDPANEL7_HAS_SCREEN
  uint32_t lvHandlerStartUs = micros();
  lv_timer_handler();
  g_runtimeStats.lvHandlerLastUs = micros() - lvHandlerStartUs;
  if (g_runtimeStats.lvHandlerLastUs > g_runtimeStats.lvHandlerMaxUs) {
    g_runtimeStats.lvHandlerMaxUs = g_runtimeStats.lvHandlerLastUs;
  }
#else
  g_runtimeStats.lvHandlerLastUs = 0;
#endif

  // PRIORITY 2: Update UI state
  uint32_t uiStartUs = micros();
#if SOUNDPANEL7_HAS_SCREEN
  lvgl_port_lock(-1);
  g_ui.tick();
  g_ui.setDb(m.dbInstant, m.leq, m.peak);
  sampleRuntimeStats();
  lvgl_port_unlock();
#else
  g_ui.tick();
  g_ui.setDb(m.dbInstant, m.leq, m.peak);
  sampleRuntimeStats();
#endif
  g_runtimeStats.uiWorkLastUs = micros() - uiStartUs;
  if (g_runtimeStats.uiWorkLastUs > g_runtimeStats.uiWorkMaxUs) {
    g_runtimeStats.uiWorkMaxUs = g_runtimeStats.uiWorkLastUs;
  }

#if SOUNDPANEL7_HAS_SCREEN
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
#endif

  // PRIORITY 3: Network operations (can block, but after UI is responsive)
  g_ota.loop();
  if (otaInProgress) {
    // OTA is timing-sensitive on the ESP32. Keep the loop as quiet as possible
    // so the upload stream and flash writes are not disturbed by UI, audio, MQTT,
    // or web activity. Let WebManager run once so it can drop live SSE clients.
    g_web.loop();
    delay(1);
    return;
  }

  g_net.loop();
  g_releaseUpdate.loop();
  if (installInProgress) {
    g_web.loop();
    delay(1);
    return;
  }

  g_mqtt.loop();
  g_web.loop();

  g_web.updateMetrics(m.dbInstant, m.leq, m.peak);
  g_mqtt.updateMetrics(m.dbInstant, m.leq, m.peak);

  delay(1);
}
