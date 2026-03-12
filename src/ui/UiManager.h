#pragma once

#include <Arduino.h>
#include <lvgl.h>
#include <esp_display_panel.hpp>

#include "../SettingsStore.h"
#include "../NetManager.h"
#include "../SharedHistory.h"

class UiManager {
public:
  void begin(esp_panel::board::Board* board,
             SettingsV1* settings,
             SettingsStore* store,
             NetManager* net,
             SharedHistory* history);

  void showDashboard();

  void tick();
  void setDb(float dbInstant, float leq, float peak);
  void requestDashboardPage(uint8_t page, bool persistSelection = false);

private:
  static constexpr uint16_t HISTORY_BAR_COUNT = SharedHistory::POINT_COUNT;
  static constexpr uint8_t DASH_PAGE_COUNT = 6;
  static constexpr uint16_t RED_HISTORY_SAMPLE_COUNT = 3600;
  static constexpr uint32_t RED_HISTORY_SAMPLE_MS = 1000;
  static constexpr uint32_t SOUND_UI_UPDATE_MS = 160;
  static constexpr uint32_t UI_TICK_PERIOD_MS = 250;
  static constexpr uint32_t CLOCK_UI_UPDATE_MS = 1000;
  static constexpr uint32_t SETTINGS_UI_UPDATE_MS = 1000;
  static constexpr uint32_t CALIBRATION_UI_UPDATE_MS = 250;
  static constexpr uint32_t POWER_OFF_DIALOG_SETTLE_MS = 250;
  static constexpr uint32_t POWER_OFF_BACKLIGHT_DELAY_MS = 80;
  static constexpr uint32_t POWER_OFF_FINAL_DELAY_MS = 50;
  static constexpr uint32_t FACTORY_RESET_RESTART_DELAY_MS = 200;

  enum DashPage : uint8_t {
    DASH_PAGE_OVERVIEW = 0,
    DASH_PAGE_CLOCK = 1,
    DASH_PAGE_LIVE = 2,
    DASH_PAGE_SOUND = 3,
    DASH_PAGE_CALIBRATION = 4,
    DASH_PAGE_SETTINGS = 5,
  };

  esp_panel::board::Board* _board = nullptr;
  SettingsV1* _s = nullptr;
  SettingsStore* _store = nullptr;
  NetManager* _net = nullptr;

  lv_obj_t* _scrDash = nullptr;
  lv_obj_t* _dashContent = nullptr;
  lv_obj_t* _dashPages[DASH_PAGE_COUNT] = {nullptr};
  lv_obj_t* _dashTabs[DASH_PAGE_COUNT] = {nullptr};
  bool _dashPageBuilt[DASH_PAGE_COUNT] = {false};
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
  lv_obj_t* _liveBadge = nullptr;
  lv_obj_t* _lblLiveBadge = nullptr;
  lv_obj_t* _lblLiveStatus = nullptr;
  lv_obj_t* _lblLiveHint = nullptr;
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
  lv_obj_t* _histPlotFocus = nullptr;
  lv_obj_t* _lblCalStatus = nullptr;
  lv_obj_t* _lblCalLive = nullptr;
  lv_obj_t* _btnCalMode3 = nullptr;
  lv_obj_t* _btnCalMode5 = nullptr;
  lv_obj_t* _lblCalPoint[CALIBRATION_POINT_MAX] = {nullptr};
  lv_obj_t* _btnCalRefMinus[CALIBRATION_POINT_MAX] = {nullptr};
  lv_obj_t* _btnCalRefPlus[CALIBRATION_POINT_MAX] = {nullptr};
  lv_obj_t* _lblCalRef[CALIBRATION_POINT_MAX] = {nullptr};
  lv_obj_t* _btnCalCapture[CALIBRATION_POINT_MAX] = {nullptr};
  lv_obj_t* _calRows[CALIBRATION_POINT_MAX] = {nullptr};
  lv_obj_t* _lblCalFallback = nullptr;

  lv_obj_t* _histWrap = nullptr;
  lv_obj_t* _lblHist = nullptr;
  lv_obj_t* _lblAlertTime = nullptr;
  lv_obj_t* _lblHistYLow = nullptr;
  lv_obj_t* _lblHistYHigh = nullptr;
  lv_obj_t* _lblHistTLeft = nullptr;
  lv_obj_t* _lblHistTMid = nullptr;
  lv_obj_t* _lblHistTRight = nullptr;
  lv_obj_t* _histPlot = nullptr;

  // Popup confirmation reset
  lv_obj_t* _msgboxConfirm = nullptr;

  // Management
  lv_obj_t* _btnBacklight = nullptr;
  lv_obj_t* _slGreen = nullptr;
  lv_obj_t* _slOrange = nullptr;
  lv_obj_t* _slHistory = nullptr;
  lv_obj_t* _btnResponseFast = nullptr;
  lv_obj_t* _btnResponseSlow = nullptr;
  lv_obj_t* _lblBacklightToggle = nullptr;
  lv_obj_t* _lblBacklightValue = nullptr;
  lv_obj_t* _lblGreenValue = nullptr;
  lv_obj_t* _lblOrangeValue = nullptr;
  lv_obj_t* _lblHistoryValue = nullptr;
  lv_obj_t* _lblResponseValue = nullptr;
  lv_obj_t* _lblWifiStatus = nullptr;
  lv_obj_t* _lblNtpStatus = nullptr;
  lv_obj_t* _lblNetInfo = nullptr;
  lv_obj_t* _lblPinState = nullptr;
  lv_obj_t* _btnPinConfigure = nullptr;
  lv_obj_t* _btnPinDisable = nullptr;

  lv_obj_t* _pinOverlay = nullptr;
  lv_obj_t* _lblPinOverlayTitle = nullptr;
  lv_obj_t* _lblPinOverlayHint = nullptr;
  lv_obj_t* _lblPinOverlayValue = nullptr;
  lv_obj_t* _lblPinOverlayStatus = nullptr;

  enum PinOverlayMode : uint8_t {
    PIN_OVERLAY_HIDDEN = 0,
    PIN_OVERLAY_UNLOCK = 1,
    PIN_OVERLAY_SET = 2,
    PIN_OVERLAY_CONFIRM = 3,
  };

  PinOverlayMode _pinOverlayMode = PIN_OVERLAY_HIDDEN;
  bool _touchPinUnlocked = false;
  uint8_t _pinPendingPage = 255;
  uint8_t _pinEntryLen = 0;
  char _pinEntry[PIN_CODE_MAX_LENGTH + 1] = {0};
  char _pinDraft[PIN_CODE_MAX_LENGTH + 1] = {0};

  SharedHistory* _history = nullptr;
  uint32_t _historyRevision = UINT32_MAX;

  uint32_t _lastTickMs = 0;
  uint32_t _lastSoundUiUpdateMs = 0;
  uint32_t _lastClockUiUpdateMs = 0;
  uint32_t _lastSettingsUiUpdateMs = 0;
  uint32_t _lastCalibrationUiUpdateMs = 0;
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
  uint8_t _lastAlertVisualState = 255;
  uint8_t _lastAlertVisualPhase = 255;
  uint8_t _lastCalibrationActiveCount = 0;
  uint8_t _lastLiveEnabled = 255;
  uint8_t _requestedDashPage = UINT8_MAX;

  void buildDashboard();
  void buildDashboardOverviewPage(lv_obj_t* parent);
  void buildDashboardClockPage(lv_obj_t* parent);
  void buildDashboardLivePage(lv_obj_t* parent);
  void buildDashboardSoundPage(lv_obj_t* parent);
  void buildDashboardCalibrationPage(lv_obj_t* parent);
  void buildDashboardSettingsPage(lv_obj_t* parent);
  void ensureDashboardPageBuilt(uint8_t page);
  void buildHistoryCard(lv_obj_t* parent,
                        int width,
                        int height,
                        lv_obj_t** wrapOut,
                        lv_obj_t** plotOut,
                        lv_obj_t** titleOut,
                        lv_obj_t** leftOut,
                        lv_obj_t** midOut,
                        lv_obj_t** rightOut);
  void setDashboardPage(uint8_t page);
  void refreshCalibrationView();
  void refreshLiveControls();
  void refreshSettingsControls();
  bool updateClockDisplay(lv_obj_t* lblDate, lv_obj_t* lblMain, lv_obj_t* lblSec,
                          const char* dateText, const char* mainText, const char* secText);
  void layoutClockFocus();
  bool hasPinConfigured() const;
  bool isProtectedPage(uint8_t page) const;
  void buildPinOverlay();
  void openPinOverlayForUnlock(uint8_t targetPage);
  void openPinOverlayForSet();
  void closePinOverlay();
  void setPinOverlayStatus(const char* text);
  void clearPinEntry(bool clearDraft = false);
  void appendPinDigit(char digit);
  void backspacePinDigit();
  void updatePinOverlay();
  void submitPinEntry();

  void applyBacklight(uint8_t percent);
  lv_color_t zoneColorForDb(float db);
  void updateAlertState(uint32_t now);
  void recordRedHistorySample(uint32_t now);
  uint16_t redSecondsWithinWindow() const;
  void applyAlertVisuals(uint32_t now);
  void powerOffNow();

  void redrawHistoryBars();

  static void onDashTab(lv_event_t* e);
  static void onDashGesture(lv_event_t* e);
  static void onOverviewCard(lv_event_t* e);
  static void onLiveToggle(lv_event_t* e);
  static void onToggleBacklight(lv_event_t* e);
  static void onSliderThresholds(lv_event_t* e);
  static void onSliderHistory(lv_event_t* e);
  static void onResponseMode(lv_event_t* e);
  static void onReboot(lv_event_t* e);
  static void onFactoryReset(lv_event_t* e);
  static void onConfirmResetYes(lv_event_t* e);
  static void onConfirmResetNo(lv_event_t* e);
  static void onPowerOff(lv_event_t* e);
  static void onCalibrationCapture(lv_event_t* e);
  static void onCalibrationClear(lv_event_t* e);
  static void onCalibrationRefChanged(lv_event_t* e);
  static void onCalibrationMode(lv_event_t* e);
  static void onHistoryPlotDraw(lv_event_t* e);
  static void onPinDigit(lv_event_t* e);
  static void onPinBackspace(lv_event_t* e);
  static void onPinCancel(lv_event_t* e);
  static void onPinSubmit(lv_event_t* e);
  static void onPinConfigure(lv_event_t* e);
  static void onPinDisable(lv_event_t* e);
};
