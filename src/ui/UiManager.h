#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include <esp_display_panel.hpp>

#include "../SettingsStore.h"
#include "../NetManager.h"

class UiManager {
public:
  void begin(esp_panel::board::Board* board,
             SettingsV1* settings,
             SettingsStore* store,
             NetManager* net);

  void showDashboard();
  void showManagement();

  void tick();
  void setDb(float dbInstant, float leq, float peak);

private:
  static constexpr uint16_t HISTORY_BAR_COUNT = 96;

  esp_panel::board::Board* _board = nullptr;
  SettingsV1* _s = nullptr;
  SettingsStore* _store = nullptr;
  NetManager* _net = nullptr;

  lv_obj_t* _scrDash = nullptr;
  lv_obj_t* _scrMgmt = nullptr;

  // Dashboard
  lv_obj_t* _arc = nullptr;
  lv_obj_t* _dot = nullptr;
  lv_obj_t* _lblDb = nullptr;
  lv_obj_t* _lblLeq = nullptr;
  lv_obj_t* _lblPeak = nullptr;
  lv_obj_t* _lblWifi = nullptr;
  lv_obj_t* _lblNtp = nullptr;
  lv_obj_t* _lblTime = nullptr;      // non utilisé sur le dashboard final
  lv_obj_t* _lblClockDate = nullptr;
  lv_obj_t* _lblClockMain = nullptr;
  lv_obj_t* _lblClockSec = nullptr;

  lv_obj_t* _histWrap = nullptr;
  lv_obj_t* _lblHist = nullptr;
  lv_obj_t* _lblHistYLow = nullptr;
  lv_obj_t* _lblHistYHigh = nullptr;
  lv_obj_t* _lblHistTLeft = nullptr;
  lv_obj_t* _lblHistTMid = nullptr;
  lv_obj_t* _lblHistTRight = nullptr;
  lv_obj_t* _histBars[HISTORY_BAR_COUNT] = {nullptr};

  // Popup confirmation reset
  lv_obj_t* _msgboxConfirm = nullptr;

  // Management
  lv_obj_t* _slBacklight = nullptr;
  lv_obj_t* _slGreen = nullptr;
  lv_obj_t* _slOrange = nullptr;
  lv_obj_t* _slHistory = nullptr;
  lv_obj_t* _lblNetInfo = nullptr;

  // History
  float _history[HISTORY_BAR_COUNT] = {0.0f};
  uint16_t _historyCount = 0;
  uint16_t _historyHead = 0;
  uint32_t _lastHistoryPushMs = 0;

  uint32_t _lastTickMs = 0;
  float _lastDb = 0.0f;
  float _lastLeq = 0.0f;
  float _lastPeak = 0.0f;

  void buildDashboard();
  void buildManagement();

  void applyBacklight(uint8_t percent);
  lv_color_t zoneColorForDb(float db);
  void powerOffNow();

  uint32_t historySamplePeriodMs() const;
  void pushHistory(float db);
  void redrawHistoryBars();

  static void onGear(lv_event_t* e);
  static void onBack(lv_event_t* e);
  static void onSliderBacklight(lv_event_t* e);
  static void onSliderThresholds(lv_event_t* e);
  static void onSliderHistory(lv_event_t* e);
  static void onReboot(lv_event_t* e);
  static void onFactoryReset(lv_event_t* e);
  static void onConfirmResetYes(lv_event_t* e);
  static void onConfirmResetNo(lv_event_t* e);
  static void onPowerOff(lv_event_t* e);
};