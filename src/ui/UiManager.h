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
  static constexpr uint8_t DASH_PAGE_COUNT = 4;
  static constexpr uint16_t RED_HISTORY_SAMPLE_COUNT = 3600;
  static constexpr uint32_t RED_HISTORY_SAMPLE_MS = 1000;

  enum DashPage : uint8_t {
    DASH_PAGE_OVERVIEW = 0,
    DASH_PAGE_CLOCK = 1,
    DASH_PAGE_SOUND = 2,
    DASH_PAGE_CALIBRATION = 3,
  };

  esp_panel::board::Board* _board = nullptr;
  SettingsV1* _s = nullptr;
  SettingsStore* _store = nullptr;
  NetManager* _net = nullptr;

  lv_obj_t* _scrDash = nullptr;
  lv_obj_t* _scrMgmt = nullptr;
  lv_obj_t* _dashContent = nullptr;
  lv_obj_t* _dashPages[DASH_PAGE_COUNT] = {nullptr};
  lv_obj_t* _dashTabs[DASH_PAGE_COUNT] = {nullptr};
  uint8_t _currentDashPage = DASH_PAGE_OVERVIEW;

  // Dashboard
  lv_obj_t* _arc = nullptr;
  lv_obj_t* _dot = nullptr;
  lv_obj_t* _lblDb = nullptr;
  lv_obj_t* _dbCard = nullptr;
  lv_obj_t* _alertBadge = nullptr;
  lv_obj_t* _lblAlertBadge = nullptr;
  lv_obj_t* _lblLeq = nullptr;
  lv_obj_t* _lblPeak = nullptr;
  lv_obj_t* _lblWifi = nullptr;
  lv_obj_t* _lblNtp = nullptr;
  lv_obj_t* _lblTime = nullptr;      // non utilisé sur le dashboard final
  lv_obj_t* _lblClockDate = nullptr;
  lv_obj_t* _lblClockMain = nullptr;
  lv_obj_t* _lblClockSec = nullptr;
  lv_obj_t* _lblClockDateFocus = nullptr;
  lv_obj_t* _lblClockMainFocus = nullptr;
  lv_obj_t* _lblClockSecFocus = nullptr;
  lv_obj_t* _clockSecBadgeFocus = nullptr;
  lv_obj_t* _lblClockStatusFocus = nullptr;
  lv_obj_t* _arcFocus = nullptr;
  lv_obj_t* _dotFocus = nullptr;
  lv_obj_t* _lblDbFocus = nullptr;
  lv_obj_t* _dbCardFocus = nullptr;
  lv_obj_t* _alertBadgeFocus = nullptr;
  lv_obj_t* _lblAlertBadgeFocus = nullptr;
  lv_obj_t* _lblLeqFocus = nullptr;
  lv_obj_t* _lblPeakFocus = nullptr;
  lv_obj_t* _histWrapFocus = nullptr;
  lv_obj_t* _lblHistFocus = nullptr;
  lv_obj_t* _lblAlertTimeFocus = nullptr;
  lv_obj_t* _lblHistTLeftFocus = nullptr;
  lv_obj_t* _lblHistTMidFocus = nullptr;
  lv_obj_t* _lblHistTRightFocus = nullptr;
  lv_obj_t* _histBarsFocus[HISTORY_BAR_COUNT] = {nullptr};
  lv_obj_t* _lblCalStatus = nullptr;
  lv_obj_t* _lblCalPoint[3] = {nullptr};
  lv_obj_t* _btnCalRefMinus[3] = {nullptr};
  lv_obj_t* _btnCalRefPlus[3] = {nullptr};
  lv_obj_t* _lblCalRef[3] = {nullptr};
  lv_obj_t* _btnCalCapture[3] = {nullptr};
  lv_obj_t* _lblCalFallback = nullptr;

  lv_obj_t* _histWrap = nullptr;
  lv_obj_t* _lblHist = nullptr;
  lv_obj_t* _lblAlertTime = nullptr;
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
  uint32_t _orangeZoneSinceMs = 0;
  uint32_t _redZoneSinceMs = 0;
  uint32_t _lastRedHistorySampleMs = 0;
  uint8_t _redHistory[RED_HISTORY_SAMPLE_COUNT] = {0};
  uint16_t _redHistoryHead = 0;
  uint16_t _redHistoryCount = 0;
  uint16_t _redHistorySum = 0;

  void buildDashboard();
  void buildManagement();
  void buildDashboardOverviewPage(lv_obj_t* parent);
  void buildDashboardClockPage(lv_obj_t* parent);
  void buildDashboardSoundPage(lv_obj_t* parent);
  void buildDashboardCalibrationPage(lv_obj_t* parent);
  void buildHistoryCard(lv_obj_t* parent,
                        int width,
                        int height,
                        lv_obj_t** wrapOut,
                        lv_obj_t** titleOut,
                        lv_obj_t** leftOut,
                        lv_obj_t** midOut,
                        lv_obj_t** rightOut,
                        lv_obj_t* barsOut[HISTORY_BAR_COUNT]);
  void setDashboardPage(uint8_t page);
  void refreshCalibrationView();
  void updateClockDisplay(lv_obj_t* lblDate, lv_obj_t* lblMain, lv_obj_t* lblSec,
                          const char* dateText, const char* mainText, const char* secText);
  void layoutClockFocus();

  void applyBacklight(uint8_t percent);
  lv_color_t zoneColorForDb(float db);
  void updateAlertState(uint32_t now);
  void recordRedHistorySample(uint32_t now);
  uint16_t redSecondsWithinWindow() const;
  void applyAlertVisuals(uint32_t now);
  void powerOffNow();

  uint32_t historySamplePeriodMs() const;
  void pushHistory(float db);
  void redrawHistoryBars();

  static void onGear(lv_event_t* e);
  static void onDashTab(lv_event_t* e);
  static void onDashGesture(lv_event_t* e);
  static void onBack(lv_event_t* e);
  static void onSliderBacklight(lv_event_t* e);
  static void onSliderThresholds(lv_event_t* e);
  static void onSliderHistory(lv_event_t* e);
  static void onReboot(lv_event_t* e);
  static void onFactoryReset(lv_event_t* e);
  static void onConfirmResetYes(lv_event_t* e);
  static void onConfirmResetNo(lv_event_t* e);
  static void onPowerOff(lv_event_t* e);
  static void onCalibrationCapture(lv_event_t* e);
  static void onCalibrationClear(lv_event_t* e);
  static void onCalibrationRefChanged(lv_event_t* e);
};
