#include "UiManager.h"

#if SOUNDPANEL7_HAS_SCREEN

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <sys/time.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "../AudioEngine.h"
#include "../lvgl_v8_port.h"
#include "AppRuntimeStats.h"

using namespace esp_panel::board;

LV_FONT_DECLARE(sp7_font_clock_170);
LV_FONT_DECLARE(sp7_font_gauge_56);
LV_FONT_DECLARE(sp7_font_live_260);

extern AudioEngine g_audio;

static constexpr lv_coord_t DASHBOARD_FULLSCREEN_MARGIN = 10;

static const char* dashPageLabel(uint8_t page) {
  switch (page) {
    case 0: return "Principal";
    case 1: return "Horloge";
    case 2: return "LIVE";
    case 3: return "Sonometre";
    case 4: return "Calibration";
    case 5: return "Parametres";
    default: return "Inconnu";
  }
}

static bool setLabelTextIfChanged(lv_obj_t* label, const char* text) {
  if (!label || !text) return false;
  const char* current = lv_label_get_text(label);
  if (current && strcmp(current, text) == 0) return false;
  lv_label_set_text(label, text);
  return true;
}

static void setHiddenIfChanged(lv_obj_t* obj, bool hidden) {
  if (!obj) return;
  const bool isHidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN);
  if (hidden == isHidden) return;
  if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static UiManager* selfFromEvent(lv_event_t* e) {
  return (UiManager*)lv_event_get_user_data(e);
}

static lv_obj_t* makeCard(lv_obj_t* parent, int w, int h) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_size(c, w, h);

  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);

  lv_obj_set_style_bg_color(c, lv_color_hex(0x111824), 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 18, 0);
  lv_obj_set_style_pad_all(c, 14, 0);
  lv_obj_set_style_shadow_width(c, 0, 0);

  return c;
}

static bool isOrangeZone(const SettingsV1* s, float db) {
  if (!s) return false;
  return db > s->th.greenMax;
}

static bool isRedZone(const SettingsV1* s, float db) {
  if (!s) return false;
  return db > s->th.orangeMax;
}

static uint64_t currentClockUnixMs() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0 || tv.tv_sec <= 946684800) return 0;
  return ((uint64_t)tv.tv_sec * 1000ULL) + (uint64_t)(tv.tv_usec / 1000ULL);
}

static lv_obj_t* mgmtCard(lv_obj_t* parent, const char* title) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_width(c, lv_pct(100));
  lv_obj_set_height(c, LV_SIZE_CONTENT);

  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);

  lv_obj_set_style_bg_color(c, lv_color_hex(0x111824), 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 16, 0);
  lv_obj_set_style_pad_all(c, 14, 0);
  lv_obj_set_style_pad_row(c, 10, 0);
  lv_obj_set_style_shadow_width(c, 0, 0);
  lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);

  lv_obj_t* t = lv_label_create(c);
  lv_label_set_text(t, title);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(t, lv_color_hex(0xDFE7EF), 0);

  return c;
}

static lv_obj_t* createSettingHeader(lv_obj_t* parent, const char* title, lv_obj_t** valueOut) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_width(row, lv_pct(100));
  lv_obj_set_height(row, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* lbl = lv_label_create(row);
  lv_label_set_text(lbl, title);
  lv_obj_set_style_text_color(lbl, lv_color_hex(0xB9C7D6), 0);
  lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* value = lv_label_create(row);
  lv_label_set_text(value, "--");
  lv_obj_set_style_text_color(value, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(value, LV_ALIGN_RIGHT_MID, 0, 0);

  if (valueOut) *valueOut = value;
  return row;
}

static lv_obj_t* makeDashboardPage(lv_obj_t* parent) {
  lv_obj_t* page = lv_obj_create(parent);
  lv_obj_set_size(page, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(page, 0, 0);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
  return page;
}

lv_coord_t UiManager::screenWidth() const {
  lv_disp_t* disp = lv_disp_get_default();
  return disp ? lv_disp_get_hor_res(disp) : 800;
}

lv_coord_t UiManager::screenHeight() const {
  lv_disp_t* disp = lv_disp_get_default();
  return disp ? lv_disp_get_ver_res(disp) : 480;
}

lv_coord_t UiManager::scaleX(lv_coord_t value) const {
  return (value * screenWidth()) / 800;
}

lv_coord_t UiManager::scaleY(lv_coord_t value) const {
  return (value * screenHeight()) / 480;
}

lv_coord_t UiManager::dashboardTopHeight() const {
  return scaleY(96);
}

lv_coord_t UiManager::dashboardContentTop() const {
  return scaleY(98);
}

lv_coord_t UiManager::dashboardContentHeight() const {
  return screenHeight() - dashboardContentTop();
}

lv_coord_t UiManager::dashboardFullscreenCardHeight() const {
  return screenHeight() - (dashboardFullscreenMargin() * 2);
}

lv_coord_t UiManager::dashboardMainCardWidth() const {
  return scaleX(780);
}

lv_coord_t UiManager::dashboardSideCardWidth() const {
  return scaleX(245);
}

lv_coord_t UiManager::dashboardClockWrapWidth() const {
  return scaleX(720);
}

lv_coord_t UiManager::dashboardSoundMetricsWidth() const {
  return scaleX(318);
}

lv_coord_t UiManager::dashboardHistoryWidth() const {
  return scaleX(780);
}

lv_coord_t UiManager::dashboardFullscreenMargin() const {
  return scaleY(DASHBOARD_FULLSCREEN_MARGIN);
}

bool UiManager::hasPinConfigured() const {
  return _s && pinCodeIsConfigured(_s->dashboardPin);
}

bool UiManager::isProtectedPage(uint8_t page) const {
  return page == DASH_PAGE_CALIBRATION || page == DASH_PAGE_SETTINGS;
}

void UiManager::begin(Board* board, SettingsV1* settings, SettingsStore* store, NetManager* net,
                      SharedHistory* history) {
  _board = board;
  _s = settings;
  _store = store;
  _net = net;
  _history = history;

  buildDashboard();
  buildOtaStatusScreen();

  lvgl_port_set_touch_enabled(_s && _s->touchEnabled);
  if (_s) applyBacklight(_s->backlight);
  showDashboard();
}

void UiManager::requestDashboardPage(uint8_t page, bool persistSelection) {
  const uint8_t normalizedPage = normalizedDashboardPage(page);
  if (_s && persistSelection) _s->dashboardPage = normalizedPage;
  _requestedDashPage = normalizedPage;
}

void UiManager::redrawHistoryBars() {
  if (!_histWrap && !_histWrapFocus) return;
  _historyRevision = _history ? _history->revision() : 0;
  if (_histPlot) lv_obj_invalidate(_histPlot);
  if (_histPlotFocus) lv_obj_invalidate(_histPlotFocus);

  if (_lblHist && _s) {
    char buf[64];
    snprintf(buf, sizeof(buf), "Historique %u min", (unsigned)_s->historyMinutes);
    lv_label_set_text(_lblHist, buf);
    if (_lblHistFocus) lv_label_set_text(_lblHistFocus, buf);
  }

  if (_s) {
    char buf[16];

    if (_lblHistTLeft) {
      snprintf(buf, sizeof(buf), "-%um", (unsigned)_s->historyMinutes);
      lv_label_set_text(_lblHistTLeft, buf);
      if (_lblHistTLeftFocus) lv_label_set_text(_lblHistTLeftFocus, buf);
    }

    if (_lblHistTMid) {
      unsigned mid = (unsigned)_s->historyMinutes / 2;
      if (mid < 1) mid = 1;
      snprintf(buf, sizeof(buf), "-%um", mid);
      lv_label_set_text(_lblHistTMid, buf);
      if (_lblHistTMidFocus) lv_label_set_text(_lblHistTMidFocus, buf);
    }

    if (_lblHistTRight) {
      lv_label_set_text(_lblHistTRight, "0");
      if (_lblHistTRightFocus) lv_label_set_text(_lblHistTRightFocus, "0");
    }

    if (_lblHistYLow) {
      lv_label_set_text(_lblHistYLow, "40");
    }

    if (_lblHistYHigh) {
      lv_label_set_text(_lblHistYHigh, "80");
    }
  }
}

void UiManager::buildDashboard() {
  _scrDash = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(_scrDash, lv_color_hex(0x0B0F14), 0);
  lv_obj_set_style_bg_opa(_scrDash, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_scrDash, 0, 0);
  lv_obj_clear_flag(_scrDash, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_scrDash, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(_scrDash, UiManager::onDashGesture, LV_EVENT_GESTURE, this);

  // ===== Top bar =====
  _dashTop = lv_obj_create(_scrDash);
  lv_obj_set_size(_dashTop, lv_pct(100), dashboardTopHeight());
  lv_obj_align(_dashTop, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(_dashTop, lv_color_hex(0x111824), 0);
  lv_obj_set_style_border_width(_dashTop, 0, 0);
  lv_obj_set_style_pad_all(_dashTop, 12, 0);
  lv_obj_set_style_pad_row(_dashTop, 10, 0);
  lv_obj_set_style_radius(_dashTop, 0, 0);
  lv_obj_clear_flag(_dashTop, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_dashTop, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(_dashTop, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(_dashTop, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  lv_obj_t* topRow = lv_obj_create(_dashTop);
  lv_obj_set_size(topRow, lv_pct(100), 36);
  lv_obj_set_style_bg_opa(topRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(topRow, 0, 0);
  lv_obj_set_style_pad_all(topRow, 0, 0);
  lv_obj_clear_flag(topRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(topRow, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_opa(topRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(topRow, 0, 0);

  lv_obj_t* title = lv_label_create(topRow);
  lv_label_set_text(title, "SoundPanel 7");
  lv_obj_set_style_text_color(title, lv_color_hex(0xDFE7EF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* tabs = lv_obj_create(_dashTop);
  lv_obj_set_size(tabs, lv_pct(100), 28);
  lv_obj_set_style_bg_opa(tabs, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(tabs, 0, 0);
  lv_obj_set_style_pad_all(tabs, 0, 0);
  lv_obj_set_style_pad_column(tabs, 8, 0);
  lv_obj_set_layout(tabs, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(tabs, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(tabs, LV_SCROLLBAR_MODE_OFF);

  static const char* kTabTitles[DASH_PAGE_COUNT] = {
    "Principal", "Horloge", "LIVE", "Sonometre", "Calibration", "Parametres"
  };
  for (uint8_t i = 0; i < DASH_PAGE_COUNT; i++) {
    _dashTabs[i] = lv_btn_create(tabs);
    lv_obj_set_size(_dashTabs[i], 94, 30);
    lv_obj_set_style_radius(_dashTabs[i], 12, 0);
    lv_obj_set_style_bg_color(_dashTabs[i], lv_color_hex(0x0E141C), 0);
    lv_obj_set_style_bg_opa(_dashTabs[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(_dashTabs[i], 0, 0);
    lv_obj_add_event_cb(_dashTabs[i], UiManager::onDashTab, LV_EVENT_CLICKED, this);
    lv_obj_set_user_data(_dashTabs[i], (void*)(uintptr_t)i);

    lv_obj_t* lbl = lv_label_create(_dashTabs[i]);
    lv_label_set_text(lbl, kTabTitles[i]);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
  }

  _dashContent = lv_obj_create(_scrDash);
  lv_obj_set_size(_dashContent, lv_pct(100), dashboardContentHeight());
  lv_obj_align(_dashContent, LV_ALIGN_TOP_MID, 0, dashboardContentTop());
  lv_obj_set_style_bg_opa(_dashContent, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(_dashContent, 0, 0);
  lv_obj_set_style_pad_all(_dashContent, 0, 0);
  lv_obj_clear_flag(_dashContent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_dashContent, LV_SCROLLBAR_MODE_OFF);

  for (uint8_t i = 0; i < DASH_PAGE_COUNT; i++) {
    _dashPages[i] = makeDashboardPage(_dashContent);
    if (i != DASH_PAGE_OVERVIEW) lv_obj_add_flag(_dashPages[i], LV_OBJ_FLAG_HIDDEN);
  }

  buildPinOverlay();

  ensureDashboardPageBuilt(DASH_PAGE_OVERVIEW);

  _lblTime = nullptr;

  const uint8_t initialPage = normalizedDashboardPage(_s ? _s->dashboardPage : DEFAULT_DASHBOARD_PAGE);
  setDashboardPage(initialPage);
  refreshCalibrationView();
  redrawHistoryBars();
}

bool UiManager::pageUsesFullscreen(uint8_t page) const {
  if (!dashboardPageSupportsFullscreen(page)) return false;
  const uint8_t mask = _s ? _s->dashboardFullscreenMask : DEFAULT_DASHBOARD_FULLSCREEN_MASK;
  return dashboardPageFullscreenEnabled(mask, page);
}

void UiManager::applyDashboardChrome(uint8_t page) {
  const bool fullscreen = pageUsesFullscreen(page);

  setHiddenIfChanged(_dashTop, fullscreen);

  if (!_dashContent) return;

  if (fullscreen) {
    lv_obj_set_size(_dashContent, lv_pct(100), lv_pct(100));
    lv_obj_align(_dashContent, LV_ALIGN_TOP_MID, 0, 0);
  } else {
    lv_obj_set_size(_dashContent, lv_pct(100), dashboardContentHeight());
    lv_obj_align(_dashContent, LV_ALIGN_TOP_MID, 0, dashboardContentTop());
  }
}

void UiManager::layoutDashboardPage(uint8_t page) {
  const bool fullscreen = pageUsesFullscreen(page);
  switch ((DashPage)page) {
    case DASH_PAGE_OVERVIEW:
      if (_overviewUpper) {
        lv_obj_set_size(_overviewUpper, dashboardMainCardWidth(), fullscreen ? scaleY(300) : scaleY(244));
        lv_obj_align(_overviewUpper, LV_ALIGN_TOP_MID, 0, fullscreen ? dashboardFullscreenMargin() : scaleY(10));
      }
      if (_overviewClockCard) {
        lv_obj_set_size(_overviewClockCard, dashboardSideCardWidth(), fullscreen ? scaleY(300) : scaleY(244));
        lv_obj_align(_overviewClockCard, LV_ALIGN_LEFT_MID, 0, 0);
      }
      if (_dbCard) {
        lv_obj_set_size(_dbCard, scaleX(270), fullscreen ? scaleY(300) : scaleY(244));
        lv_obj_align(_dbCard, LV_ALIGN_CENTER, 0, 0);
      }
      if (_overviewMetricsCol) {
        lv_obj_set_size(_overviewMetricsCol, dashboardSideCardWidth(), fullscreen ? scaleY(300) : scaleY(244));
        lv_obj_align(_overviewMetricsCol, LV_ALIGN_RIGHT_MID, 0, 0);
      }
      if (_overviewLeqCard) lv_obj_set_size(_overviewLeqCard, dashboardSideCardWidth(), fullscreen ? scaleY(145) : scaleY(117));
      if (_overviewPeakCard) lv_obj_set_size(_overviewPeakCard, dashboardSideCardWidth(), fullscreen ? scaleY(145) : scaleY(117));
      if (_histWrap) {
        lv_obj_set_size(_histWrap, dashboardHistoryWidth(), fullscreen ? scaleY(140) : scaleY(108));
        lv_obj_align(_histWrap, LV_ALIGN_BOTTOM_MID, 0, fullscreen ? -dashboardFullscreenMargin() : -scaleY(10));
      }
      if (_histPlot) lv_obj_set_size(_histPlot, lv_pct(100), fullscreen ? scaleY(92) : scaleY(68));
      break;

    case DASH_PAGE_CLOCK:
      if (_clockCardFocus) {
        lv_obj_set_size(_clockCardFocus, dashboardMainCardWidth(), fullscreen ? dashboardFullscreenCardHeight() : scaleY(362));
        lv_obj_align(_clockCardFocus, LV_ALIGN_TOP_MID, 0, fullscreen ? dashboardFullscreenMargin() : scaleY(10));
      }
      if (_clockWrapFocus) {
        lv_obj_set_size(_clockWrapFocus, dashboardClockWrapWidth(), fullscreen ? scaleY(320) : scaleY(266));
        lv_obj_align(_clockWrapFocus, LV_ALIGN_CENTER, 0, fullscreen ? scaleY(36) : scaleY(30));
      }
      layoutClockFocus();
      break;

    case DASH_PAGE_LIVE:
      if (_liveBadge) {
        lv_obj_set_size(_liveBadge, dashboardMainCardWidth(), fullscreen ? dashboardFullscreenCardHeight() : scaleY(362));
        lv_obj_align(_liveBadge, LV_ALIGN_TOP_MID, 0, fullscreen ? dashboardFullscreenMargin() : scaleY(10));
      }
      break;

    case DASH_PAGE_SOUND:
      if (_dbCardFocus) {
        lv_obj_set_size(_dbCardFocus, dashboardMainCardWidth(), fullscreen ? scaleY(300) : scaleY(240));
        lv_obj_align(_dbCardFocus, LV_ALIGN_TOP_MID, 0, fullscreen ? dashboardFullscreenMargin() : scaleY(10));
      }
      if (_soundMetricsWrap) {
        lv_obj_set_size(_soundMetricsWrap, dashboardSoundMetricsWidth(), fullscreen ? scaleY(220) : scaleY(188));
        lv_obj_align(_soundMetricsWrap, LV_ALIGN_RIGHT_MID, 0, 8);
      }
      if (_soundLeqCard) lv_obj_set_size(_soundLeqCard, scaleX(152), fullscreen ? scaleY(220) : scaleY(188));
      if (_soundPeakCard) lv_obj_set_size(_soundPeakCard, scaleX(152), fullscreen ? scaleY(220) : scaleY(188));
      if (_histWrapFocus) {
        lv_obj_set_size(_histWrapFocus, dashboardHistoryWidth(), fullscreen ? scaleY(140) : scaleY(112));
        lv_obj_align(_histWrapFocus, LV_ALIGN_TOP_MID, 0, fullscreen ? scaleY(320) : scaleY(260));
      }
      if (_histPlotFocus) lv_obj_set_size(_histPlotFocus, lv_pct(100), fullscreen ? scaleY(92) : scaleY(68));
      break;

    default:
      break;
  }
}

void UiManager::refreshDashboardLayout() {
  applyDashboardChrome(_currentDashPage);
  layoutDashboardPage(_currentDashPage);
}

void UiManager::ensureDashboardPageBuilt(uint8_t page) {
  if (page >= DASH_PAGE_COUNT || !_dashPages[page] || _dashPageBuilt[page]) return;

  switch ((DashPage)page) {
    case DASH_PAGE_OVERVIEW:
      buildDashboardOverviewPage(_dashPages[page]);
      break;
    case DASH_PAGE_CLOCK:
      buildDashboardClockPage(_dashPages[page]);
      break;
    case DASH_PAGE_LIVE:
      buildDashboardLivePage(_dashPages[page]);
      break;
    case DASH_PAGE_SOUND:
      buildDashboardSoundPage(_dashPages[page]);
      break;
    case DASH_PAGE_CALIBRATION:
      buildDashboardCalibrationPage(_dashPages[page]);
      break;
    case DASH_PAGE_SETTINGS:
      buildDashboardSettingsPage(_dashPages[page]);
      break;
    default:
      break;
  }

  _dashPageBuilt[page] = true;
}

void UiManager::buildDashboardSettingsPage(lv_obj_t* parent) {
  lv_obj_t* scroll = lv_obj_create(parent);
  lv_obj_set_size(scroll, lv_pct(100), lv_pct(100));
  lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(scroll, 0, 0);
  lv_obj_set_style_pad_all(scroll, 10, 0);
  lv_obj_set_style_pad_row(scroll, 12, 0);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);

  lv_obj_t* cNet = mgmtCard(scroll, "Reseau & Heure");

  _lblWifiStatus = lv_label_create(cNet);
  lv_label_set_text(_lblWifiStatus, "WiFi: --");
  lv_obj_set_style_text_font(_lblWifiStatus, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblWifiStatus, lv_color_hex(0xDFE7EF), 0);

  _lblNtpStatus = lv_label_create(cNet);
  lv_label_set_text(_lblNtpStatus, "NTP: --");
  lv_obj_set_style_text_font(_lblNtpStatus, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblNtpStatus, lv_color_hex(0xDFE7EF), 0);

  _lblNetInfo = lv_label_create(cNet);
  lv_label_set_text(_lblNetInfo, "-");
  lv_obj_set_style_text_font(_lblNetInfo, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblNetInfo, lv_color_hex(0xB9C7D6), 0);

  lv_obj_t* cUi = mgmtCard(scroll, "UI");
  createSettingHeader(cUi, "Backlight", &_lblBacklightValue);

  _btnBacklight = lv_btn_create(cUi);
  lv_obj_set_size(_btnBacklight, lv_pct(100), 44);
  lv_obj_set_style_radius(_btnBacklight, 14, 0);
  lv_obj_add_event_cb(_btnBacklight, UiManager::onToggleBacklight, LV_EVENT_CLICKED, this);
  _lblBacklightToggle = lv_label_create(_btnBacklight);
  lv_label_set_text(_lblBacklightToggle, "ON");
  lv_obj_center(_lblBacklightToggle);

  lv_obj_t* cTh = mgmtCard(scroll, "Seuils dB");
  createSettingHeader(cTh, "Vert <=", &_lblGreenValue);

  _slGreen = lv_slider_create(cTh);
  lv_obj_set_width(_slGreen, lv_pct(100));
  lv_slider_set_range(_slGreen, 0, 100);
  lv_slider_set_value(_slGreen, _s ? _s->th.greenMax : DEFAULT_GREEN_MAX, LV_ANIM_OFF);
  lv_obj_add_event_cb(_slGreen, UiManager::onSliderThresholds, LV_EVENT_VALUE_CHANGED, this);
  createSettingHeader(cTh, "Orange <=", &_lblOrangeValue);

  _slOrange = lv_slider_create(cTh);
  lv_obj_set_width(_slOrange, lv_pct(100));
  lv_slider_set_range(_slOrange, 0, 100);
  lv_slider_set_value(_slOrange, _s ? _s->th.orangeMax : DEFAULT_ORANGE_MAX, LV_ANIM_OFF);
  lv_obj_add_event_cb(_slOrange, UiManager::onSliderThresholds, LV_EVENT_VALUE_CHANGED, this);

  createSettingHeader(cTh, "Reponse", &_lblResponseValue);

  lv_obj_t* responseRow = lv_obj_create(cTh);
  lv_obj_set_width(responseRow, lv_pct(100));
  lv_obj_set_height(responseRow, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(responseRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(responseRow, 0, 0);
  lv_obj_set_style_pad_all(responseRow, 0, 0);
  lv_obj_set_style_pad_column(responseRow, 10, 0);
  lv_obj_set_layout(responseRow, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(responseRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(responseRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(responseRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(responseRow, LV_SCROLLBAR_MODE_OFF);

  _btnResponseFast = lv_btn_create(responseRow);
  lv_obj_set_size(_btnResponseFast, 96, 40);
  lv_obj_set_style_radius(_btnResponseFast, 14, 0);
  lv_obj_set_user_data(_btnResponseFast, (void*)(uintptr_t)0);
  lv_obj_add_event_cb(_btnResponseFast, UiManager::onResponseMode, LV_EVENT_CLICKED, this);
  lv_obj_t* fastLbl = lv_label_create(_btnResponseFast);
  lv_label_set_text(fastLbl, "Fast");
  lv_obj_center(fastLbl);

  _btnResponseSlow = lv_btn_create(responseRow);
  lv_obj_set_size(_btnResponseSlow, 96, 40);
  lv_obj_set_style_radius(_btnResponseSlow, 14, 0);
  lv_obj_set_user_data(_btnResponseSlow, (void*)(uintptr_t)1);
  lv_obj_add_event_cb(_btnResponseSlow, UiManager::onResponseMode, LV_EVENT_CLICKED, this);
  lv_obj_t* slowLbl = lv_label_create(_btnResponseSlow);
  lv_label_set_text(slowLbl, "Slow");
  lv_obj_center(slowLbl);

  lv_obj_t* cHist = mgmtCard(scroll, "Historique");
  createSettingHeader(cHist, "Minutes", &_lblHistoryValue);

  _slHistory = lv_slider_create(cHist);
  lv_obj_set_width(_slHistory, lv_pct(100));
  lv_slider_set_range(_slHistory, 1, 60);
  lv_slider_set_value(_slHistory, _s ? _s->historyMinutes : DEFAULT_HISTORY_MINUTES, LV_ANIM_OFF);
  lv_obj_add_event_cb(_slHistory, UiManager::onSliderHistory, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_t* cPin = mgmtCard(scroll, "Protection");
  createSettingHeader(cPin, "Code PIN", &_lblPinState);
  lv_obj_set_style_text_font(_lblPinState, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(_lblPinState, lv_color_hex(0x8EA1B3), 0);

  _btnPinConfigure = lv_btn_create(cPin);
  lv_obj_set_size(_btnPinConfigure, lv_pct(100), 44);
  lv_obj_set_style_radius(_btnPinConfigure, 14, 0);
  lv_obj_set_style_bg_color(_btnPinConfigure, lv_color_hex(0x7A1E2C), 0);
  lv_obj_set_style_border_width(_btnPinConfigure, 0, 0);
  lv_obj_add_event_cb(_btnPinConfigure, UiManager::onPinConfigure, LV_EVENT_CLICKED, this);
  lv_obj_t* pinConfigureLbl = lv_label_create(_btnPinConfigure);
  lv_label_set_text(pinConfigureLbl, "Configurer PIN");
  lv_obj_center(pinConfigureLbl);

  _btnPinDisable = lv_btn_create(cPin);
  lv_obj_set_size(_btnPinDisable, lv_pct(100), 42);
  lv_obj_set_style_radius(_btnPinDisable, 14, 0);
  lv_obj_set_style_bg_color(_btnPinDisable, lv_color_hex(0x16202E), 0);
  lv_obj_set_style_border_width(_btnPinDisable, 0, 0);
  lv_obj_add_event_cb(_btnPinDisable, UiManager::onPinDisable, LV_EVENT_CLICKED, this);
  lv_obj_t* pinDisableLbl = lv_label_create(_btnPinDisable);
  lv_label_set_text(pinDisableLbl, "Desactiver PIN");
  lv_obj_center(pinDisableLbl);

  lv_obj_t* cMaint = mgmtCard(scroll, "Maintenance");
  lv_obj_t* actions = lv_obj_create(cMaint);
  lv_obj_set_width(actions, lv_pct(100));
  lv_obj_set_height(actions, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(actions, 0, 0);
  lv_obj_set_style_pad_all(actions, 0, 0);
  lv_obj_set_style_pad_column(actions, 10, 0);
  lv_obj_set_style_pad_row(actions, 10, 0);
  lv_obj_set_layout(actions, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(actions, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(actions, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(actions, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* reboot = lv_btn_create(actions);
  lv_obj_set_height(reboot, 42);
  lv_obj_add_event_cb(reboot, UiManager::onReboot, LV_EVENT_CLICKED, this);
  lv_obj_t* rebootLbl = lv_label_create(reboot);
  lv_label_set_text(rebootLbl, "Reboot");
  lv_obj_center(rebootLbl);
  lv_obj_update_layout(reboot);
  lv_obj_set_width(reboot, lv_obj_get_width(reboot) + 28);

  lv_obj_t* poweroff = lv_btn_create(actions);
  lv_obj_set_height(poweroff, 42);
  lv_obj_add_event_cb(poweroff, UiManager::onPowerOff, LV_EVENT_CLICKED, this);
  lv_obj_t* poweroffLbl = lv_label_create(poweroff);
  lv_label_set_text(poweroffLbl, "Eteindre");
  lv_obj_center(poweroffLbl);
  lv_obj_update_layout(poweroff);
  lv_obj_set_width(poweroff, lv_obj_get_width(poweroff) + 28);

  lv_obj_t* reset = lv_btn_create(actions);
  lv_obj_set_height(reset, 42);
  lv_obj_add_event_cb(reset, UiManager::onFactoryReset, LV_EVENT_CLICKED, this);
  lv_obj_t* resetLbl = lv_label_create(reset);
  lv_label_set_text(resetLbl, "Factory reset");
  lv_obj_center(resetLbl);
  lv_obj_update_layout(reset);
  lv_obj_set_width(reset, lv_obj_get_width(reset) + 28);

  refreshSettingsControls();
}

void UiManager::buildPinOverlay() {
  _pinOverlay = lv_obj_create(_scrDash);
  lv_obj_remove_style_all(_pinOverlay);
  lv_obj_set_size(_pinOverlay, lv_pct(100), lv_pct(100));
  lv_obj_center(_pinOverlay);
  lv_obj_set_style_bg_color(_pinOverlay, lv_color_hex(0x0B0F14), 0);
  lv_obj_set_style_bg_opa(_pinOverlay, LV_OPA_COVER, 0);
  lv_obj_add_flag(_pinOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(_pinOverlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(_pinOverlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* cancelBtn = lv_btn_create(_pinOverlay);
  lv_obj_set_size(cancelBtn, 140, 48);
  lv_obj_align(cancelBtn, LV_ALIGN_TOP_LEFT, 18, 18);
  lv_obj_set_style_radius(cancelBtn, 18, 0);
  lv_obj_set_style_bg_color(cancelBtn, lv_color_hex(0x16202E), 0);
  lv_obj_set_style_border_width(cancelBtn, 0, 0);
  lv_obj_add_event_cb(cancelBtn, UiManager::onPinCancel, LV_EVENT_CLICKED, this);
  lv_obj_t* cancelLbl = lv_label_create(cancelBtn);
  lv_label_set_text(cancelLbl, "Annuler");
  lv_obj_center(cancelLbl);

  _lblPinOverlayTitle = lv_label_create(_pinOverlay);
  lv_label_set_text(_lblPinOverlayTitle, "Code PIN");
  lv_obj_set_style_text_font(_lblPinOverlayTitle, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(_lblPinOverlayTitle, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblPinOverlayTitle, LV_ALIGN_TOP_MID, 0, 18);

  _lblPinOverlayHint = lv_label_create(_pinOverlay);
  lv_label_set_text(_lblPinOverlayHint, "Saisis le code pour continuer.");
  lv_obj_set_style_text_font(_lblPinOverlayHint, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblPinOverlayHint, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(_lblPinOverlayHint, LV_ALIGN_TOP_MID, 0, 58);

  _lblPinOverlayValue = lv_label_create(_pinOverlay);
  lv_label_set_text(_lblPinOverlayValue, "_ _ _ _ _ _ _ _");
  lv_obj_set_style_text_font(_lblPinOverlayValue, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_letter_space(_lblPinOverlayValue, 2, 0);
  lv_obj_set_style_text_color(_lblPinOverlayValue, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(_lblPinOverlayValue, LV_ALIGN_TOP_MID, 0, 98);

  _lblPinOverlayStatus = lv_label_create(_pinOverlay);
  lv_label_set_text(_lblPinOverlayStatus, "");
  lv_obj_set_style_text_font(_lblPinOverlayStatus, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblPinOverlayStatus, lv_color_hex(0xF0A202), 0);
  lv_obj_align(_lblPinOverlayStatus, LV_ALIGN_TOP_MID, 0, 136);

  struct PinKeyDef {
    const char* label;
    int32_t code;
    lv_color_t bg;
  };

  static const PinKeyDef keys[12] = {
    {"1", '1', lv_color_hex(0x111824)}, {"2", '2', lv_color_hex(0x111824)}, {"3", '3', lv_color_hex(0x111824)},
    {"4", '4', lv_color_hex(0x111824)}, {"5", '5', lv_color_hex(0x111824)}, {"6", '6', lv_color_hex(0x111824)},
    {"7", '7', lv_color_hex(0x111824)}, {"8", '8', lv_color_hex(0x111824)}, {"9", '9', lv_color_hex(0x111824)},
    {"Eff", -1, lv_color_hex(0x16202E)}, {"0", '0', lv_color_hex(0x111824)}, {"OK", -2, lv_color_hex(0x7A1E2C)}
  };

  const lv_coord_t keyW = 148;
  const lv_coord_t keyH = 50;
  const lv_coord_t gapX = 14;
  const lv_coord_t gapY = 12;
  const lv_coord_t keyboardW = (keyW * 3) + (gapX * 2);
  const lv_coord_t startX = (screenWidth() - keyboardW) / 2;
  const lv_coord_t startY = scaleY(182);

  for (uint8_t i = 0; i < 12; i++) {
    const lv_coord_t x = startX + (i % 3) * (keyW + gapX);
    const lv_coord_t y = startY + (i / 3) * (keyH + gapY);
    lv_obj_t* btn = lv_btn_create(_pinOverlay);
    lv_obj_set_size(btn, keyW, keyH);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_bg_color(btn, keys[i].bg, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_user_data(btn, (void*)(intptr_t)keys[i].code);
    if (keys[i].code == -1) {
      lv_obj_add_event_cb(btn, UiManager::onPinBackspace, LV_EVENT_CLICKED, this);
    } else if (keys[i].code == -2) {
      lv_obj_add_event_cb(btn, UiManager::onPinSubmit, LV_EVENT_CLICKED, this);
    } else {
      lv_obj_add_event_cb(btn, UiManager::onPinDigit, LV_EVENT_CLICKED, this);
    }
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, keys[i].label);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl);
  }
}

void UiManager::buildOtaStatusScreen() {
  _scrOta = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(_scrOta, lv_color_hex(0x07111A), 0);
  lv_obj_set_style_bg_opa(_scrOta, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_scrOta, 0, 0);
  lv_obj_clear_flag(_scrOta, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_scrOta, LV_SCROLLBAR_MODE_OFF);

  _otaStatusCard = lv_obj_create(_scrOta);
  lv_obj_set_size(_otaStatusCard, 560, 280);
  lv_obj_center(_otaStatusCard);
  lv_obj_set_style_radius(_otaStatusCard, 28, 0);
  lv_obj_set_style_border_width(_otaStatusCard, 0, 0);
  lv_obj_set_style_bg_color(_otaStatusCard, lv_color_hex(0x102030), 0);
  lv_obj_set_style_pad_all(_otaStatusCard, 28, 0);
  lv_obj_set_style_pad_row(_otaStatusCard, 16, 0);
  lv_obj_clear_flag(_otaStatusCard, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_otaStatusCard, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(_otaStatusCard, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(_otaStatusCard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  _lblOtaEyebrow = lv_label_create(_otaStatusCard);
  lv_label_set_text(_lblOtaEyebrow, "OTA");
  lv_obj_set_style_text_font(_lblOtaEyebrow, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblOtaEyebrow, lv_color_hex(0x7CC6FE), 0);

  _lblOtaTitle = lv_label_create(_otaStatusCard);
  lv_label_set_text(_lblOtaTitle, "Mise a jour en cours");
  lv_obj_set_style_text_font(_lblOtaTitle, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(_lblOtaTitle, lv_color_hex(0xF4F7FA), 0);

  _lblOtaDetail = lv_label_create(_otaStatusCard);
  lv_label_set_text(_lblOtaDetail, "Reception du firmware en cours...");
  lv_label_set_long_mode(_lblOtaDetail, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(_lblOtaDetail, lv_pct(100));
  lv_obj_set_style_text_font(_lblOtaDetail, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblOtaDetail, lv_color_hex(0xB7C7D6), 0);

  _barOtaProgress = lv_bar_create(_otaStatusCard);
  lv_obj_set_size(_barOtaProgress, lv_pct(100), 18);
  lv_bar_set_range(_barOtaProgress, 0, 100);
  lv_bar_set_value(_barOtaProgress, 0, LV_ANIM_OFF);
  lv_obj_set_style_radius(_barOtaProgress, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(_barOtaProgress, lv_color_hex(0x1D3145), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(_barOtaProgress, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(_barOtaProgress, 0, 0);
  lv_obj_set_style_bg_color(_barOtaProgress, lv_color_hex(0x39A0ED), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(_barOtaProgress, LV_OPA_COVER, LV_PART_INDICATOR);

  _lblOtaProgress = lv_label_create(_otaStatusCard);
  lv_label_set_text(_lblOtaProgress, "0%");
  lv_obj_set_style_text_font(_lblOtaProgress, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(_lblOtaProgress, lv_color_hex(0xDCE7F2), 0);
}

void UiManager::setPinOverlayStatus(const char* text) {
  if (_lblPinOverlayStatus) {
    lv_label_set_text(_lblPinOverlayStatus, text ? text : "");
  }
}

void UiManager::clearPinEntry(bool clearDraft) {
  _pinEntryLen = 0;
  _pinEntry[0] = '\0';
  if (clearDraft) _pinDraft[0] = '\0';
  setPinOverlayStatus("");
  updatePinOverlay();
}

void UiManager::updatePinOverlay() {
  char mask[(PIN_CODE_MAX_LENGTH * 2) + 1];
  size_t pos = 0;
  for (uint8_t i = 0; i < PIN_CODE_MAX_LENGTH; i++) {
    mask[pos++] = (i < _pinEntryLen) ? '*' : '_';
    if (i + 1 < PIN_CODE_MAX_LENGTH) mask[pos++] = ' ';
  }
  mask[pos] = '\0';
  setLabelTextIfChanged(_lblPinOverlayValue, mask);

  const char* title = "Code PIN";
  const char* hint = "Saisis le code pour continuer.";
  if (_pinOverlayMode == PIN_OVERLAY_UNLOCK) {
    title = "Code PIN";
    hint = (_pinPendingPage == DASH_PAGE_CALIBRATION)
      ? "Saisis le code pour ouvrir Calibration."
      : "Saisis le code pour ouvrir Parametres.";
  } else if (_pinOverlayMode == PIN_OVERLAY_SET) {
    title = "Nouveau PIN";
    hint = "Entre un code de 4 a 8 chiffres.";
  } else if (_pinOverlayMode == PIN_OVERLAY_CONFIRM) {
    title = "Confirmation PIN";
    hint = "Ressaisis le meme code pour confirmer.";
  }

  setLabelTextIfChanged(_lblPinOverlayTitle, title);
  setLabelTextIfChanged(_lblPinOverlayHint, hint);
}

void UiManager::openPinOverlayForUnlock(uint8_t targetPage) {
  if (!hasPinConfigured()) {
    _touchPinUnlocked = true;
    setDashboardPage(targetPage);
    return;
  }

  _pinPendingPage = targetPage;
  _pinOverlayMode = PIN_OVERLAY_UNLOCK;
  clearPinEntry(true);
  updatePinOverlay();
  lv_obj_clear_flag(_pinOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_pinOverlay);
}

void UiManager::openPinOverlayForSet() {
  _pinPendingPage = 255;
  _pinOverlayMode = PIN_OVERLAY_SET;
  clearPinEntry(true);
  updatePinOverlay();
  lv_obj_clear_flag(_pinOverlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(_pinOverlay);
}

void UiManager::closePinOverlay() {
  if (_pinOverlay) lv_obj_add_flag(_pinOverlay, LV_OBJ_FLAG_HIDDEN);
  _pinOverlayMode = PIN_OVERLAY_HIDDEN;
  _pinPendingPage = 255;
  _pinEntryLen = 0;
  _pinEntry[0] = '\0';
  _pinDraft[0] = '\0';
  setPinOverlayStatus("");
  updatePinOverlay();
}

void UiManager::appendPinDigit(char digit) {
  if (_pinOverlayMode == PIN_OVERLAY_HIDDEN) return;
  if (digit < '0' || digit > '9') return;
  if (_pinEntryLen >= PIN_CODE_MAX_LENGTH) return;
  _pinEntry[_pinEntryLen++] = digit;
  _pinEntry[_pinEntryLen] = '\0';
  setPinOverlayStatus("");
  updatePinOverlay();
}

void UiManager::backspacePinDigit() {
  if (_pinOverlayMode == PIN_OVERLAY_HIDDEN || _pinEntryLen == 0) return;
  _pinEntryLen--;
  _pinEntry[_pinEntryLen] = '\0';
  setPinOverlayStatus("");
  updatePinOverlay();
}

void UiManager::submitPinEntry() {
  if (!_s || !_store) return;

  if (_pinOverlayMode == PIN_OVERLAY_UNLOCK) {
    if (!pinCodeIsValid(_pinEntry)) {
      setPinOverlayStatus("PIN invalide");
      return;
    }
    if (!pinCodeMatches(_s->dashboardPin, _pinEntry)) {
      clearPinEntry(false);
      setPinOverlayStatus("PIN incorrect");
      return;
    }

    _touchPinUnlocked = true;
    uint8_t nextPage = _pinPendingPage;
    closePinOverlay();
    if (nextPage < DASH_PAGE_COUNT) setDashboardPage(nextPage);
    return;
  }

  if (_pinOverlayMode == PIN_OVERLAY_SET) {
    if (!pinCodeIsValid(_pinEntry)) {
      setPinOverlayStatus("PIN: 4 a 8 chiffres");
      return;
    }
    strncpy(_pinDraft, _pinEntry, sizeof(_pinDraft) - 1);
    _pinDraft[sizeof(_pinDraft) - 1] = '\0';
    _pinOverlayMode = PIN_OVERLAY_CONFIRM;
    _pinEntryLen = 0;
    _pinEntry[0] = '\0';
    setPinOverlayStatus("");
    updatePinOverlay();
    return;
  }

  if (_pinOverlayMode == PIN_OVERLAY_CONFIRM) {
    if (strcmp(_pinEntry, _pinDraft) != 0) {
      _pinOverlayMode = PIN_OVERLAY_SET;
      clearPinEntry(true);
      setPinOverlayStatus("Confirmation differente");
      return;
    }

    if (!encodePinCode(_pinDraft, _s->dashboardPin, sizeof(_s->dashboardPin))) {
      _pinOverlayMode = PIN_OVERLAY_SET;
      clearPinEntry(true);
      setPinOverlayStatus("Erreur de securite");
      return;
    }
    _store->saveDashboardPin(_s->dashboardPin);
    _touchPinUnlocked = true;
    refreshSettingsControls();
    closePinOverlay();
  }
}

void UiManager::buildDashboardOverviewPage(lv_obj_t* parent) {
  _overviewUpper = lv_obj_create(parent);
  lv_obj_set_size(_overviewUpper, dashboardMainCardWidth(), scaleY(300));
  lv_obj_align(_overviewUpper, LV_ALIGN_TOP_MID, 0, dashboardFullscreenMargin());
  lv_obj_set_style_bg_opa(_overviewUpper, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(_overviewUpper, 0, 0);
  lv_obj_set_style_pad_all(_overviewUpper, 0, 0);
  lv_obj_clear_flag(_overviewUpper, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_overviewUpper, LV_SCROLLBAR_MODE_OFF);

  _overviewClockCard = makeCard(_overviewUpper, dashboardSideCardWidth(), scaleY(300));
  lv_obj_align(_overviewClockCard, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_bg_color(_overviewClockCard, lv_color_hex(0x101A28), 0);
  lv_obj_set_style_radius(_overviewClockCard, 22, 0);
  lv_obj_set_style_pad_all(_overviewClockCard, 16, 0);
  lv_obj_add_flag(_overviewClockCard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(_overviewClockCard, (void*)(uintptr_t)DASH_PAGE_CLOCK);
  lv_obj_add_event_cb(_overviewClockCard, UiManager::onOverviewCard, LV_EVENT_CLICKED, this);

  _lblClockDate = lv_label_create(_overviewClockCard);
  lv_label_set_text(_lblClockDate, "--/--/----");
  lv_obj_set_style_text_font(_lblClockDate, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblClockDate, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(_lblClockDate, LV_ALIGN_TOP_MID, 0, 0);

  _lblClockMain = lv_label_create(_overviewClockCard);
  lv_label_set_text(_lblClockMain, "--:--");
  lv_obj_set_style_text_font(_lblClockMain, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(_lblClockMain, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblClockMain, LV_ALIGN_CENTER, 0, -8);

  lv_obj_t* secBadge = lv_obj_create(_overviewClockCard);
  lv_obj_set_size(secBadge, 78, 34);
  lv_obj_align(secBadge, LV_ALIGN_BOTTOM_RIGHT, 0, -50);
  lv_obj_set_style_bg_color(secBadge, lv_color_hex(0x7A1E2C), 0);
  lv_obj_set_style_border_width(secBadge, 0, 0);
  lv_obj_set_style_radius(secBadge, 12, 0);
  lv_obj_set_style_pad_all(secBadge, 0, 0);
  lv_obj_clear_flag(secBadge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(secBadge, LV_SCROLLBAR_MODE_OFF);

  _lblClockSec = lv_label_create(secBadge);
  lv_label_set_text(_lblClockSec, ":--");
  lv_obj_set_style_text_font(_lblClockSec, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(_lblClockSec, lv_color_hex(0xDFE7EF), 0);
  lv_obj_center(_lblClockSec);

  _dbCard = makeCard(_overviewUpper, scaleX(270), scaleY(300));
  lv_obj_align(_dbCard, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(_dbCard, lv_color_hex(0x111824), 0);
  lv_obj_set_style_radius(_dbCard, 24, 0);
  lv_obj_set_style_pad_all(_dbCard, 0, 0);
  lv_obj_add_flag(_dbCard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(_dbCard, (void*)(uintptr_t)DASH_PAGE_SOUND);
  lv_obj_add_event_cb(_dbCard, UiManager::onOverviewCard, LV_EVENT_CLICKED, this);

  _arc = lv_arc_create(_dbCard);
  lv_obj_set_size(_arc, 240, 240);
  lv_obj_align(_arc, LV_ALIGN_CENTER, 0, 15);
  lv_arc_set_range(_arc, 0, 100);
  lv_arc_set_value(_arc, 0);
  lv_arc_set_rotation(_arc, 135);
  lv_arc_set_bg_angles(_arc, 0, 270);
  lv_obj_remove_style(_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(_arc, 24, LV_PART_MAIN);
  lv_obj_set_style_arc_width(_arc, 24, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(_arc, lv_color_hex(0x1A2332), LV_PART_MAIN);
  lv_obj_set_style_arc_color(_arc, lv_color_hex(0x23C552), LV_PART_INDICATOR);

  _lblDb = lv_label_create(_dbCard);
  lv_label_set_text(_lblDb, "--.-");
  lv_obj_set_style_text_font(_lblDb, &sp7_font_gauge_56, 0);
  lv_obj_set_style_text_color(_lblDb, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align_to(_lblDb, _arc, LV_ALIGN_CENTER, -20, -10);

  lv_obj_t* unit = lv_label_create(_dbCard);
  lv_label_set_text(unit, "dB");
  lv_obj_set_style_text_font(unit, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(unit, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align_to(unit, _arc, LV_ALIGN_CENTER, 0, 54);

  _dot = lv_obj_create(_dbCard);
  lv_obj_set_size(_dot, 14, 14);
  lv_obj_set_style_radius(_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(_dot, lv_color_hex(0x23C552), 0);
  lv_obj_set_style_border_width(_dot, 0, 0);
  lv_obj_align_to(_dot, unit, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
  lv_obj_clear_flag(_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_dot, LV_SCROLLBAR_MODE_OFF);

  _overviewMetricsCol = lv_obj_create(_overviewUpper);
  lv_obj_set_size(_overviewMetricsCol, dashboardSideCardWidth(), scaleY(300));
  lv_obj_align(_overviewMetricsCol, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_opa(_overviewMetricsCol, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(_overviewMetricsCol, 0, 0);
  lv_obj_set_style_pad_all(_overviewMetricsCol, 0, 0);
  lv_obj_set_style_pad_row(_overviewMetricsCol, 10, 0);
  lv_obj_set_flex_flow(_overviewMetricsCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(_overviewMetricsCol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(_overviewMetricsCol, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_overviewMetricsCol, LV_SCROLLBAR_MODE_OFF);

  _overviewLeqCard = makeCard(_overviewMetricsCol, dashboardSideCardWidth(), scaleY(145));
  _overviewPeakCard = makeCard(_overviewMetricsCol, dashboardSideCardWidth(), scaleY(145));

  lv_obj_t* t1 = lv_label_create(_overviewLeqCard);
  lv_label_set_text(t1, "Leq");
  lv_obj_set_style_text_font(t1, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(t1, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 0);

  _lblLeq = lv_label_create(_overviewLeqCard);
  lv_label_set_text(_lblLeq, "--.-");
  lv_obj_set_style_text_font(_lblLeq, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(_lblLeq, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblLeq, LV_ALIGN_CENTER, 0, 16);

  lv_obj_t* t2 = lv_label_create(_overviewPeakCard);
  lv_label_set_text(t2, "Peak");
  lv_obj_set_style_text_font(t2, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(t2, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(t2, LV_ALIGN_TOP_MID, 0, 0);

  _lblPeak = lv_label_create(_overviewPeakCard);
  lv_label_set_text(_lblPeak, "--.-");
  lv_obj_set_style_text_font(_lblPeak, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(_lblPeak, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblPeak, LV_ALIGN_CENTER, 0, 16);

  buildHistoryCard(parent, dashboardHistoryWidth(), scaleY(140), &_histWrap, &_histPlot, &_lblHist, &_lblHistTLeft, &_lblHistTMid, &_lblHistTRight);
  lv_obj_set_width(_histWrap, dashboardHistoryWidth());
  lv_obj_align(_histWrap, LV_ALIGN_BOTTOM_MID, 0, -dashboardFullscreenMargin());
  lv_obj_set_size(_histPlot, lv_pct(100), scaleY(92));
}

void UiManager::buildDashboardClockPage(lv_obj_t* parent) {
  _clockCardFocus = makeCard(parent, dashboardMainCardWidth(), dashboardFullscreenCardHeight());
  lv_obj_align(_clockCardFocus, LV_ALIGN_TOP_MID, 0, dashboardFullscreenMargin());
  lv_obj_set_style_bg_color(_clockCardFocus, lv_color_hex(0x101A28), 0);
  lv_obj_set_style_radius(_clockCardFocus, 26, 0);
  lv_obj_set_style_pad_all(_clockCardFocus, 28, 0);

  _lblClockStatusFocus = nullptr;

  lv_obj_t* topRule = lv_obj_create(_clockCardFocus);
  lv_obj_set_size(topRule, lv_pct(100), 2);
  lv_obj_align(topRule, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_set_style_bg_color(topRule, lv_color_hex(0x243244), 0);
  lv_obj_set_style_bg_opa(topRule, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(topRule, 0, 0);
  lv_obj_set_style_radius(topRule, 0, 0);
  lv_obj_clear_flag(topRule, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(topRule, LV_SCROLLBAR_MODE_OFF);

  _clockWrapFocus = lv_obj_create(_clockCardFocus);
  lv_obj_set_size(_clockWrapFocus, dashboardClockWrapWidth(), scaleY(320));
  lv_obj_align(_clockWrapFocus, LV_ALIGN_CENTER, 0, 36);
  lv_obj_set_style_bg_opa(_clockWrapFocus, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(_clockWrapFocus, 0, 0);
  lv_obj_set_style_pad_all(_clockWrapFocus, 0, 0);
  lv_obj_clear_flag(_clockWrapFocus, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_clockWrapFocus, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(_clockWrapFocus, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

  _lblClockMainFocus = lv_label_create(_clockWrapFocus);
  lv_label_set_text(_lblClockMainFocus, "--:--");
  lv_obj_set_style_text_font(_lblClockMainFocus, &sp7_font_clock_170, 0);
  lv_obj_set_style_text_color(_lblClockMainFocus, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblClockMainFocus, LV_ALIGN_CENTER, 0, 0);

  _clockSecBadgeFocus = lv_obj_create(_clockWrapFocus);
  lv_obj_set_size(_clockSecBadgeFocus, 262, 156);
  lv_obj_set_style_bg_color(_clockSecBadgeFocus, lv_color_hex(0x7A1E2C), 0);
  lv_obj_set_style_bg_opa(_clockSecBadgeFocus, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_clockSecBadgeFocus, 0, 0);
  lv_obj_set_style_radius(_clockSecBadgeFocus, 24, 0);
  lv_obj_set_style_pad_all(_clockSecBadgeFocus, 0, 0);
  lv_obj_clear_flag(_clockSecBadgeFocus, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_clockSecBadgeFocus, LV_SCROLLBAR_MODE_OFF);

  _lblClockSecFocus = lv_label_create(_clockSecBadgeFocus);
  lv_label_set_text(_lblClockSecFocus, "--");
  lv_obj_set_style_text_font(_lblClockSecFocus, &sp7_font_clock_170, 0);
  lv_obj_set_style_text_color(_lblClockSecFocus, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(_lblClockSecFocus);

  _lblClockDateFocus = lv_label_create(_clockCardFocus);
  lv_label_set_text(_lblClockDateFocus, "--/--/----");
  lv_obj_set_style_text_font(_lblClockDateFocus, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(_lblClockDateFocus, lv_color_hex(0xB9C7D6), 0);
  lv_obj_align(_lblClockDateFocus, LV_ALIGN_TOP_RIGHT, 0, 0);

  layoutClockFocus();
}

void UiManager::buildDashboardLivePage(lv_obj_t* parent) {
  _liveBadge = lv_btn_create(parent);
  lv_obj_set_size(_liveBadge, dashboardMainCardWidth(), dashboardFullscreenCardHeight());
  lv_obj_align(_liveBadge, LV_ALIGN_TOP_MID, 0, dashboardFullscreenMargin());
  lv_obj_set_style_radius(_liveBadge, 40, 0);
  lv_obj_set_style_pad_all(_liveBadge, 0, 0);
  lv_obj_set_style_border_width(_liveBadge, 4, 0);
  lv_obj_set_style_border_color(_liveBadge, lv_color_hex(0x290306), 0);
  lv_obj_set_style_shadow_width(_liveBadge, 26, 0);
  lv_obj_set_style_shadow_opa(_liveBadge, LV_OPA_40, 0);
  lv_obj_add_event_cb(_liveBadge, UiManager::onLiveToggle, LV_EVENT_CLICKED, this);

  _lblLiveBadge = lv_label_create(_liveBadge);
  lv_label_set_text(_lblLiveBadge, "LIVE");
  lv_obj_set_style_text_font(_lblLiveBadge, &sp7_font_live_260, 0);
  lv_obj_set_style_text_color(_lblLiveBadge, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_letter_space(_lblLiveBadge, -3, 0);
  lv_obj_center(_lblLiveBadge);

  _lblLiveStatus = nullptr;
  _lblLiveHint = nullptr;

  refreshLiveControls();
}

void UiManager::buildDashboardSoundPage(lv_obj_t* parent) {
  _dbCardFocus = makeCard(parent, dashboardMainCardWidth(), scaleY(300));
  lv_obj_align(_dbCardFocus, LV_ALIGN_TOP_MID, 0, dashboardFullscreenMargin());
  lv_obj_set_style_bg_color(_dbCardFocus, lv_color_hex(0x111824), 0);
  lv_obj_set_style_radius(_dbCardFocus, 26, 0);
  lv_obj_set_style_pad_all(_dbCardFocus, 20, 0);

  _alertBadgeFocus = lv_obj_create(_dbCardFocus);
  lv_obj_set_size(_alertBadgeFocus, 112, 34);
  lv_obj_set_style_bg_color(_alertBadgeFocus, lv_color_hex(0xE53935), 0);
  lv_obj_set_style_bg_opa(_alertBadgeFocus, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_alertBadgeFocus, 0, 0);
  lv_obj_set_style_radius(_alertBadgeFocus, 17, 0);
  lv_obj_set_style_pad_all(_alertBadgeFocus, 0, 0);
  lv_obj_add_flag(_alertBadgeFocus, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(_alertBadgeFocus, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_alertBadgeFocus, LV_SCROLLBAR_MODE_OFF);

  _lblAlertBadgeFocus = lv_label_create(_alertBadgeFocus);
  lv_label_set_text(_lblAlertBadgeFocus, "ALERTE");
  lv_obj_set_style_text_font(_lblAlertBadgeFocus, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(_lblAlertBadgeFocus, lv_color_hex(0xFFFFFF), 0);
  lv_obj_center(_lblAlertBadgeFocus);

  _arcFocus = lv_arc_create(_dbCardFocus);
  lv_obj_set_size(_arcFocus, 240, 240);
  lv_obj_align(_arcFocus, LV_ALIGN_LEFT_MID, 12, 15);
  lv_arc_set_range(_arcFocus, 0, 100);
  lv_arc_set_value(_arcFocus, 0);
  lv_arc_set_rotation(_arcFocus, 135);
  lv_arc_set_bg_angles(_arcFocus, 0, 270);
  lv_obj_remove_style(_arcFocus, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(_arcFocus, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(_arcFocus, 24, LV_PART_MAIN);
  lv_obj_set_style_arc_width(_arcFocus, 24, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(_arcFocus, lv_color_hex(0x1A2332), LV_PART_MAIN);
  lv_obj_set_style_arc_color(_arcFocus, lv_color_hex(0x23C552), LV_PART_INDICATOR);
  lv_obj_align_to(_alertBadgeFocus, _arcFocus, LV_ALIGN_OUT_RIGHT_MID, 18, -46);
  lv_obj_move_foreground(_alertBadgeFocus);

  _lblDbFocus = lv_label_create(_dbCardFocus);
  lv_label_set_text(_lblDbFocus, "--.-");
  lv_obj_set_style_text_font(_lblDbFocus, &sp7_font_gauge_56, 0);
  lv_obj_set_style_text_color(_lblDbFocus, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align_to(_lblDbFocus, _arcFocus, LV_ALIGN_CENTER, -20, -8);

  lv_obj_t* unit = lv_label_create(_dbCardFocus);
  lv_label_set_text(unit, "dB");
  lv_obj_set_style_text_font(unit, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(unit, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align_to(unit, _arcFocus, LV_ALIGN_CENTER, 0, 54);

  _dotFocus = lv_obj_create(_dbCardFocus);
  lv_obj_set_size(_dotFocus, 16, 16);
  lv_obj_set_style_radius(_dotFocus, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(_dotFocus, lv_color_hex(0x23C552), 0);
  lv_obj_set_style_border_width(_dotFocus, 0, 0);
  lv_obj_align_to(_dotFocus, unit, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
  lv_obj_clear_flag(_dotFocus, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_dotFocus, LV_SCROLLBAR_MODE_OFF);

  _soundMetricsWrap = lv_obj_create(_dbCardFocus);
  lv_obj_set_size(_soundMetricsWrap, dashboardSoundMetricsWidth(), scaleY(220));
  lv_obj_align(_soundMetricsWrap, LV_ALIGN_RIGHT_MID, 0, 8);
  lv_obj_set_style_bg_opa(_soundMetricsWrap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(_soundMetricsWrap, 0, 0);
  lv_obj_set_style_pad_all(_soundMetricsWrap, 0, 0);
  lv_obj_set_style_pad_column(_soundMetricsWrap, 14, 0);
  lv_obj_set_layout(_soundMetricsWrap, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(_soundMetricsWrap, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(_soundMetricsWrap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(_soundMetricsWrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_soundMetricsWrap, LV_SCROLLBAR_MODE_OFF);

  _soundLeqCard = makeCard(_soundMetricsWrap, scaleX(152), scaleY(220));
  lv_obj_set_style_bg_color(_soundLeqCard, lv_color_hex(0x0E141C), 0);
  lv_obj_set_style_radius(_soundLeqCard, 22, 0);
  lv_obj_set_style_pad_all(_soundLeqCard, 18, 0);

  lv_obj_t* leqTitle = lv_label_create(_soundLeqCard);
  lv_label_set_text(leqTitle, "Leq");
  lv_obj_set_style_text_font(leqTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(leqTitle, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(leqTitle, LV_ALIGN_TOP_LEFT, 0, 0);

  _lblLeqFocus = lv_label_create(_soundLeqCard);
  lv_label_set_text(_lblLeqFocus, "--.-");
  lv_obj_set_style_text_font(_lblLeqFocus, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(_lblLeqFocus, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblLeqFocus, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  _soundPeakCard = makeCard(_soundMetricsWrap, scaleX(152), scaleY(220));
  lv_obj_set_style_bg_color(_soundPeakCard, lv_color_hex(0x0E141C), 0);
  lv_obj_set_style_radius(_soundPeakCard, 22, 0);
  lv_obj_set_style_pad_all(_soundPeakCard, 18, 0);

  lv_obj_t* peakTitle = lv_label_create(_soundPeakCard);
  lv_label_set_text(peakTitle, "Peak");
  lv_obj_set_style_text_font(peakTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(peakTitle, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(peakTitle, LV_ALIGN_TOP_LEFT, 0, 0);

  _lblPeakFocus = lv_label_create(_soundPeakCard);
  lv_label_set_text(_lblPeakFocus, "--.-");
  lv_obj_set_style_text_font(_lblPeakFocus, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(_lblPeakFocus, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblPeakFocus, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  buildHistoryCard(parent, dashboardHistoryWidth(), scaleY(140), &_histWrapFocus, &_histPlotFocus, &_lblHistFocus, &_lblHistTLeftFocus,
                   &_lblHistTMidFocus, &_lblHistTRightFocus);
  lv_obj_align(_histWrapFocus, LV_ALIGN_TOP_MID, 0, scaleY(320));
  lv_obj_set_size(_histPlotFocus, lv_pct(100), scaleY(92));
}

void UiManager::buildHistoryCard(lv_obj_t* parent,
                                 int width,
                                 int height,
                                 lv_obj_t** wrapOut,
                                 lv_obj_t** plotOut,
                                 lv_obj_t** titleOut,
                                 lv_obj_t** leftOut,
                                 lv_obj_t** midOut,
                                 lv_obj_t** rightOut) {
  lv_obj_t* wrap = lv_obj_create(parent);
  if (width > 0) lv_obj_set_size(wrap, width, height);
  else lv_obj_set_height(wrap, height);
  lv_obj_set_style_bg_color(wrap, lv_color_hex(0x111824), 0);
  lv_obj_set_style_border_width(wrap, 0, 0);
  lv_obj_set_style_radius(wrap, 20, 0);
  lv_obj_set_style_pad_all(wrap, 12, 0);
  lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(wrap, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* title = lv_label_create(wrap);
  lv_label_set_text(title, "Historique 5 min");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* alertTime = lv_label_create(wrap);
  lv_label_set_text(alertTime, "Rouge 0 s");
  lv_obj_set_style_text_font(alertTime, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(alertTime, lv_color_hex(0xE53935), 0);
  lv_obj_align(alertTime, LV_ALIGN_TOP_RIGHT, 0, 2);

  lv_obj_t* histArea = lv_obj_create(wrap);
  lv_obj_set_size(histArea, lv_pct(100), 68);
  lv_obj_align(histArea, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(histArea, lv_color_hex(0x0E141C), 0);
  lv_obj_set_style_border_width(histArea, 0, 0);
  lv_obj_set_style_radius(histArea, 14, 0);
  lv_obj_set_style_pad_left(histArea, 8, 0);
  lv_obj_set_style_pad_right(histArea, 8, 0);
  lv_obj_set_style_pad_top(histArea, 6, 0);
  lv_obj_set_style_pad_bottom(histArea, 6, 0);
  lv_obj_clear_flag(histArea, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(histArea, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(histArea, UiManager::onHistoryPlotDraw, LV_EVENT_DRAW_MAIN, this);

  lv_obj_t* left = lv_label_create(wrap);
  lv_label_set_text(left, "-5m");
  lv_obj_set_style_text_font(left, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(left, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(left, LV_ALIGN_BOTTOM_LEFT, 12, -4);

  lv_obj_t* mid = lv_label_create(wrap);
  lv_label_set_text(mid, "-2m");
  lv_obj_set_style_text_font(mid, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(mid, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(mid, LV_ALIGN_BOTTOM_MID, 0, -4);

  lv_obj_t* right = lv_label_create(wrap);
  lv_label_set_text(right, "0");
  lv_obj_set_style_text_font(right, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(right, lv_color_hex(0xFFFFFF), 0);
  lv_obj_align(right, LV_ALIGN_BOTTOM_RIGHT, -12, -4);

  *wrapOut = wrap;
  *plotOut = histArea;
  *titleOut = title;
  *leftOut = left;
  *midOut = mid;
  *rightOut = right;

  if (wrapOut == &_histWrap) {
    _lblAlertTime = alertTime;
  } else if (wrapOut == &_histWrapFocus) {
    _lblAlertTimeFocus = alertTime;
  }
}

void UiManager::onHistoryPlotDraw(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  lv_obj_t* obj = lv_event_get_target(e);
  lv_draw_ctx_t* drawCtx = lv_event_get_draw_ctx(e);
  if (!self || !obj || !drawCtx) return;

  lv_area_t coords;
  lv_obj_get_content_coords(obj, &coords);
  const lv_coord_t width = lv_area_get_width(&coords);
  const lv_coord_t height = lv_area_get_height(&coords);
  if (width <= 0 || height <= 0) return;

  const uint16_t count = self->_history ? self->_history->count() : 0;
  const uint16_t visibleCount = count > HISTORY_BAR_COUNT ? HISTORY_BAR_COUNT : count;
  const uint16_t emptyPrefix = HISTORY_BAR_COUNT - visibleCount;
  const float histDbMin = 35.0f;
  const float histDbMax = 100.0f;
  const lv_coord_t barMin = 4;
  const lv_coord_t radius = 2;

  lv_draw_rect_dsc_t rectDsc;
  lv_draw_rect_dsc_init(&rectDsc);
  rectDsc.bg_opa = LV_OPA_COVER;
  rectDsc.border_opa = LV_OPA_TRANSP;
  rectDsc.radius = radius;

  for (uint16_t i = 0; i < HISTORY_BAR_COUNT; i++) {
    float value = histDbMin;
    if (self->_history && i >= emptyPrefix) {
      value = self->_history->valueAt(i - emptyPrefix);
    }
    if (value < histDbMin) value = histDbMin;
    if (value > histDbMax) value = histDbMax;

    const float norm = (value - histDbMin) / (histDbMax - histDbMin);
    lv_coord_t barHeight = barMin + (lv_coord_t)(norm * (float)(height - barMin));
    if (barHeight < barMin) barHeight = barMin;
    if (barHeight > height) barHeight = height;

    const lv_coord_t x1 = coords.x1 + (lv_coord_t)((int32_t)i * width / HISTORY_BAR_COUNT);
    lv_coord_t x2 = coords.x1 + (lv_coord_t)(((int32_t)(i + 1) * width) / HISTORY_BAR_COUNT) - 2;
    if (x2 < x1) x2 = x1;
    lv_area_t barArea;
    barArea.x1 = x1;
    barArea.x2 = x2;
    barArea.y2 = coords.y2;
    barArea.y1 = coords.y2 - barHeight + 1;

    rectDsc.bg_color = self->zoneColorForDb(value);
    lv_draw_rect(drawCtx, &rectDsc, &barArea);
  }
}

void UiManager::buildDashboardCalibrationPage(lv_obj_t* parent) {
  const lv_coord_t cardTopOffset = scaleY(10);

  lv_obj_t* card = makeCard(parent, dashboardMainCardWidth(), scaleY(362));
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, cardTopOffset);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x111824), 0);
  lv_obj_set_style_radius(card, 26, 0);
  lv_obj_set_style_pad_all(card, 20, 0);

  lv_obj_t* title = lv_label_create(card);
  lv_label_set_text(title, "Calibration");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  _lblCalStatus = lv_label_create(card);
  lv_label_set_text(_lblCalStatus, "Aucune calibration");
  lv_obj_set_style_text_font(_lblCalStatus, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(_lblCalStatus, lv_color_hex(0xF0A202), 0);
  lv_obj_align(_lblCalStatus, LV_ALIGN_TOP_RIGHT, 0, 6);

  _lblCalLive = lv_label_create(card);
  lv_label_set_text(_lblCalLive, "Micro live: --");
  lv_obj_set_style_text_font(_lblCalLive, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(_lblCalLive, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(_lblCalLive, LV_ALIGN_TOP_LEFT, 0, 28);

  lv_obj_t* modeWrap = lv_obj_create(card);
  lv_obj_set_size(modeWrap, lv_pct(100), 32);
  lv_obj_align(modeWrap, LV_ALIGN_TOP_MID, 0, 52);
  lv_obj_set_style_bg_opa(modeWrap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(modeWrap, 0, 0);
  lv_obj_set_style_pad_all(modeWrap, 0, 0);
  lv_obj_clear_flag(modeWrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(modeWrap, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* modeLbl = lv_label_create(modeWrap);
  lv_label_set_text(modeLbl, "Mode calibration");
  lv_obj_set_style_text_font(modeLbl, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(modeLbl, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(modeLbl, LV_ALIGN_LEFT_MID, 0, 0);

  _btnCalMode3 = lv_btn_create(modeWrap);
  lv_obj_set_size(_btnCalMode3, 80, 28);
  lv_obj_align(_btnCalMode3, LV_ALIGN_RIGHT_MID, -96, 0);
  lv_obj_set_style_radius(_btnCalMode3, 12, 0);
  lv_obj_set_style_border_width(_btnCalMode3, 0, 0);
  lv_obj_set_user_data(_btnCalMode3, (void*)(uintptr_t)3);
  lv_obj_add_event_cb(_btnCalMode3, UiManager::onCalibrationMode, LV_EVENT_CLICKED, this);
  lv_obj_t* mode3Lbl = lv_label_create(_btnCalMode3);
  lv_label_set_text(mode3Lbl, "3 points");
  lv_obj_set_style_text_font(mode3Lbl, &lv_font_montserrat_14, 0);
  lv_obj_center(mode3Lbl);

  _btnCalMode5 = lv_btn_create(modeWrap);
  lv_obj_set_size(_btnCalMode5, 80, 28);
  lv_obj_align(_btnCalMode5, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_radius(_btnCalMode5, 12, 0);
  lv_obj_set_style_border_width(_btnCalMode5, 0, 0);
  lv_obj_set_user_data(_btnCalMode5, (void*)(uintptr_t)5);
  lv_obj_add_event_cb(_btnCalMode5, UiManager::onCalibrationMode, LV_EVENT_CLICKED, this);
  lv_obj_t* mode5Lbl = lv_label_create(_btnCalMode5);
  lv_label_set_text(mode5Lbl, "5 points");
  lv_obj_set_style_text_font(mode5Lbl, &lv_font_montserrat_14, 0);
  lv_obj_center(mode5Lbl);

  lv_obj_t* pointsWrap = lv_obj_create(card);
  lv_obj_set_size(pointsWrap, lv_pct(100), 182);
  lv_obj_align(pointsWrap, LV_ALIGN_TOP_MID, 0, 86);
  lv_obj_set_style_bg_color(pointsWrap, lv_color_hex(0x0E141C), 0);
  lv_obj_set_style_border_width(pointsWrap, 0, 0);
  lv_obj_set_style_radius(pointsWrap, 22, 0);
  lv_obj_set_style_pad_all(pointsWrap, 10, 0);
  lv_obj_set_style_pad_row(pointsWrap, 4, 0);
  lv_obj_set_flex_flow(pointsWrap, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(pointsWrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(pointsWrap, LV_SCROLLBAR_MODE_OFF);

  for (uint8_t i = 0; i < CALIBRATION_POINT_MAX; i++) {
    lv_obj_t* row = lv_obj_create(pointsWrap);
    _calRows[i] = row;
    lv_obj_set_size(row, lv_pct(100), 28);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);

    _lblCalPoint[i] = lv_label_create(row);
    lv_label_set_text(_lblCalPoint[i], "-");
    lv_obj_set_style_text_font(_lblCalPoint[i], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lblCalPoint[i], lv_color_hex(0xDFE7EF), 0);
    lv_obj_align(_lblCalPoint[i], LV_ALIGN_LEFT_MID, 0, 0);

    _btnCalRefMinus[i] = lv_btn_create(row);
    lv_obj_set_size(_btnCalRefMinus[i], 26, 26);
    lv_obj_align(_btnCalRefMinus[i], LV_ALIGN_RIGHT_MID, -218, 0);
    lv_obj_set_style_radius(_btnCalRefMinus[i], 10, 0);
    lv_obj_set_style_bg_color(_btnCalRefMinus[i], lv_color_hex(0x16202E), 0);
    lv_obj_set_style_border_width(_btnCalRefMinus[i], 0, 0);
    lv_obj_set_user_data(_btnCalRefMinus[i], (void*)(uintptr_t)(i * 2));
    lv_obj_add_event_cb(_btnCalRefMinus[i], UiManager::onCalibrationRefChanged, LV_EVENT_CLICKED, this);

    lv_obj_t* minusLbl = lv_label_create(_btnCalRefMinus[i]);
    lv_label_set_text(minusLbl, "-");
    lv_obj_set_style_text_font(minusLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(minusLbl, lv_color_hex(0xDFE7EF), 0);
    lv_obj_center(minusLbl);

    lv_obj_t* refBadge = lv_obj_create(row);
    lv_obj_set_size(refBadge, 62, 26);
    lv_obj_align(refBadge, LV_ALIGN_RIGHT_MID, -148, 0);
    lv_obj_set_style_radius(refBadge, 10, 0);
    lv_obj_set_style_bg_color(refBadge, lv_color_hex(0x16202E), 0);
    lv_obj_set_style_border_width(refBadge, 0, 0);
    lv_obj_set_style_pad_all(refBadge, 0, 0);
    lv_obj_clear_flag(refBadge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(refBadge, LV_SCROLLBAR_MODE_OFF);

    _lblCalRef[i] = lv_label_create(refBadge);
    lv_label_set_text(_lblCalRef[i], "--");
    lv_obj_set_style_text_font(_lblCalRef[i], &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(_lblCalRef[i], lv_color_hex(0xDFE7EF), 0);
    lv_obj_center(_lblCalRef[i]);

    _btnCalRefPlus[i] = lv_btn_create(row);
    lv_obj_set_size(_btnCalRefPlus[i], 26, 26);
    lv_obj_align(_btnCalRefPlus[i], LV_ALIGN_RIGHT_MID, -118, 0);
    lv_obj_set_style_radius(_btnCalRefPlus[i], 10, 0);
    lv_obj_set_style_bg_color(_btnCalRefPlus[i], lv_color_hex(0x16202E), 0);
    lv_obj_set_style_border_width(_btnCalRefPlus[i], 0, 0);
    lv_obj_set_user_data(_btnCalRefPlus[i], (void*)(uintptr_t)(i * 2 + 1));
    lv_obj_add_event_cb(_btnCalRefPlus[i], UiManager::onCalibrationRefChanged, LV_EVENT_CLICKED, this);

    lv_obj_t* plusLbl = lv_label_create(_btnCalRefPlus[i]);
    lv_label_set_text(plusLbl, "+");
    lv_obj_set_style_text_font(plusLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(plusLbl, lv_color_hex(0xDFE7EF), 0);
    lv_obj_center(plusLbl);

    _btnCalCapture[i] = lv_btn_create(row);
    lv_obj_set_size(_btnCalCapture[i], 104, 26);
    lv_obj_align(_btnCalCapture[i], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(_btnCalCapture[i], 10, 0);
    lv_obj_set_style_bg_color(_btnCalCapture[i], lv_color_hex(0x7A1E2C), 0);
    lv_obj_set_style_border_width(_btnCalCapture[i], 0, 0);
    lv_obj_set_user_data(_btnCalCapture[i], (void*)(uintptr_t)i);
    lv_obj_add_event_cb(_btnCalCapture[i], UiManager::onCalibrationCapture, LV_EVENT_CLICKED, this);

    lv_obj_t* btnLbl = lv_label_create(_btnCalCapture[i]);
    lv_label_set_text(btnLbl, "Capturer");
    lv_obj_set_style_text_font(btnLbl, &lv_font_montserrat_14, 0);
    lv_obj_center(btnLbl);
  }

  lv_obj_t* clearBtn = lv_btn_create(card);
  lv_obj_set_size(clearBtn, 182, 32);
  lv_obj_align(clearBtn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_radius(clearBtn, 16, 0);
  lv_obj_set_style_bg_color(clearBtn, lv_color_hex(0x4A1620), 0);
  lv_obj_set_style_border_width(clearBtn, 0, 0);
  lv_obj_add_event_cb(clearBtn, UiManager::onCalibrationClear, LV_EVENT_CLICKED, this);

  lv_obj_t* clearLbl = lv_label_create(clearBtn);
  lv_label_set_text(clearLbl, "Effacer calibration");
  lv_obj_set_style_text_font(clearLbl, &lv_font_montserrat_14, 0);
  lv_obj_center(clearLbl);

  _lblCalFallback = nullptr;
}

void UiManager::setDashboardPage(uint8_t page) {
  if (page >= DASH_PAGE_COUNT) return;
  if (hasPinConfigured() && isProtectedPage(page) && !_touchPinUnlocked) {
    openPinOverlayForUnlock(page);
    return;
  }
  bool pageChanged = (_currentDashPage != page);
  applyDashboardChrome(page);
  ensureDashboardPageBuilt(page);
  layoutDashboardPage(page);

  for (uint8_t i = 0; i < DASH_PAGE_COUNT; i++) {
    if (_dashPages[i]) {
      if (i == page) lv_obj_clear_flag(_dashPages[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(_dashPages[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (_dashTabs[i]) {
      lv_color_t bg = (i == page) ? lv_color_hex(0x7A1E2C) : lv_color_hex(0x0E141C);
      lv_color_t fg = (i == page) ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x8EA1B3);
      lv_obj_set_style_bg_color(_dashTabs[i], bg, 0);
      lv_obj_set_style_text_color(_dashTabs[i], fg, 0);
    }
  }

  _currentDashPage = page;
  if (!isProtectedPage(page)) _touchPinUnlocked = false;
  strncpy(g_runtimeStats.activePage, dashPageLabel(page), sizeof(g_runtimeStats.activePage) - 1);
  g_runtimeStats.activePage[sizeof(g_runtimeStats.activePage) - 1] = '\0';
  if (pageChanged) {
    g_runtimeStats.uiWorkMaxUs = 0;
    g_runtimeStats.lvHandlerMaxUs = 0;
    _lastSoundUiUpdateMs = 0;
    _lastClockUiUpdateMs = 0;
    _lastSettingsUiUpdateMs = 0;
    _lastCalibrationUiUpdateMs = 0;
    if (_history && _historyRevision != _history->revision() &&
        (page == DASH_PAGE_OVERVIEW || page == DASH_PAGE_SOUND)) {
      redrawHistoryBars();
    }
  }
}

void UiManager::refreshCalibrationView() {
  if (!_s) return;

  if (_lblCalLive) {
    const AudioMetrics& m = g_audio.metrics();
    char liveBuf[96];
    if (m.analogOk) {
      snprintf(liveBuf, sizeof(liveBuf), "Micro live: %0.1f dB | log=%0.4f",
               m.dbInstant,
               log10f(m.rawRms + 0.0001f));
    } else {
      snprintf(liveBuf, sizeof(liveBuf), "Micro live: indisponible");
    }
    setLabelTextIfChanged(_lblCalLive, liveBuf);
  }

  uint8_t validCount = 0;
  const uint8_t activeCount = normalizedCalibrationPointCount(_s->calibrationPointCount);
  for (uint8_t i = 0; i < activeCount; i++) {
    if (_s->calPointValid[i]) validCount++;
  }

  for (uint8_t row = 0; row < CALIBRATION_POINT_MAX; row++) {
    const bool visible = row < activeCount;
    setHiddenIfChanged(_calRows[row], !visible);
    setHiddenIfChanged(_btnCalCapture[row], !visible);
    setHiddenIfChanged(_btnCalRefMinus[row], !visible);
    setHiddenIfChanged(_btnCalRefPlus[row], !visible);
    if (_lblCalRef[row]) {
      char refBuf[16];
      snprintf(refBuf, sizeof(refBuf), visible ? "%0.0f" : "--", visible ? _s->calPointRefDb[row] : 0.0f);
      setLabelTextIfChanged(_lblCalRef[row], refBuf);
    }
    if (_lblCalPoint[row]) {
      char buf[96];
      if (!visible) {
        snprintf(buf, sizeof(buf), "-");
      } else if (_s->calPointValid[row]) {
        snprintf(buf, sizeof(buf), "Point %u  capture %0.3f",
                 (unsigned)(row + 1),
                 _s->calPointRawLogRms[row]);
      } else {
        snprintf(buf, sizeof(buf), "Point %u  non capture", (unsigned)(row + 1));
      }
      setLabelTextIfChanged(_lblCalPoint[row], buf);
    }
  }

  if (_lblCalStatus) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%u / %u points valides", (unsigned)validCount, (unsigned)activeCount);
    setLabelTextIfChanged(_lblCalStatus, buf);
  }

  if (_btnCalMode3 && _lastCalibrationActiveCount != activeCount) {
    const bool active = activeCount == 3;
    lv_obj_set_style_bg_color(_btnCalMode3, active ? lv_color_hex(0x7A1E2C) : lv_color_hex(0x16202E), 0);
    lv_obj_set_style_text_color(_btnCalMode3, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB9C7D6), 0);
  }
  if (_btnCalMode5 && _lastCalibrationActiveCount != activeCount) {
    const bool active = activeCount == CALIBRATION_POINT_MAX;
    lv_obj_set_style_bg_color(_btnCalMode5, active ? lv_color_hex(0x7A1E2C) : lv_color_hex(0x16202E), 0);
    lv_obj_set_style_text_color(_btnCalMode5, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB9C7D6), 0);
  }
  _lastCalibrationActiveCount = activeCount;
}

void UiManager::refreshLiveControls() {
  if (!_s || !_liveBadge) return;

  const uint8_t liveEnabled = _s->liveEnabled ? LIVE_ENABLED : LIVE_DISABLED;
  if (_lastLiveEnabled == liveEnabled) return;
  _lastLiveEnabled = liveEnabled;

  const bool active = liveEnabled == LIVE_ENABLED;
  lv_color_t bg = active ? lv_color_hex(0xC8141E) : lv_color_hex(0x6E747D);
  lv_color_t border = active ? lv_color_hex(0x290306) : lv_color_hex(0x24282D);
  lv_color_t shadow = active ? lv_color_hex(0xFF3B30) : lv_color_hex(0x555A62);

  lv_obj_set_style_bg_color(_liveBadge, bg, 0);
  lv_obj_set_style_bg_opa(_liveBadge, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(_liveBadge, border, 0);
  lv_obj_set_style_border_width(_liveBadge, active ? 5 : 4, 0);
  lv_obj_set_style_shadow_color(_liveBadge, shadow, 0);
  lv_obj_set_style_shadow_width(_liveBadge, active ? 44 : 18, 0);
  lv_obj_set_style_shadow_opa(_liveBadge, active ? LV_OPA_70 : LV_OPA_20, 0);
  lv_obj_set_style_shadow_spread(_liveBadge, active ? 2 : 0, 0);
}

void UiManager::refreshSettingsControls() {
  if (!_s) return;

  if (_lblBacklightValue) {
    lv_label_set_text(_lblBacklightValue, _s->backlight == 0 ? "OFF" : "ON");
  }

  if (_btnBacklight) {
    const bool active = _s->backlight > 0;
    lv_obj_set_style_bg_color(_btnBacklight, active ? lv_color_hex(0x7A1E2C) : lv_color_hex(0x16202E), 0);
    lv_obj_set_style_text_color(_btnBacklight, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB9C7D6), 0);
  }

  if (_lblBacklightToggle) {
    lv_label_set_text(_lblBacklightToggle, _s->backlight == 0 ? "OFF" : "ON");
  }

  if (_lblGreenValue) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u dB", (unsigned)_s->th.greenMax);
    lv_label_set_text(_lblGreenValue, buf);
  }

  if (_lblOrangeValue) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u dB", (unsigned)_s->th.orangeMax);
    lv_label_set_text(_lblOrangeValue, buf);
  }

  if (_lblHistoryValue) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u min", (unsigned)_s->historyMinutes);
    lv_label_set_text(_lblHistoryValue, buf);
  }

  if (_lblResponseValue) {
    lv_label_set_text(_lblResponseValue, AudioEngine::responseModeLabel(_s->audioResponseMode));
  }

  if (_btnResponseFast) {
    const bool active = _s->audioResponseMode == 0;
    lv_obj_set_style_bg_color(_btnResponseFast, active ? lv_color_hex(0x7A1E2C) : lv_color_hex(0x16202E), 0);
    lv_obj_set_style_text_color(_btnResponseFast, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB9C7D6), 0);
  }

  if (_btnResponseSlow) {
    const bool active = _s->audioResponseMode == 1;
    lv_obj_set_style_bg_color(_btnResponseSlow, active ? lv_color_hex(0x7A1E2C) : lv_color_hex(0x16202E), 0);
    lv_obj_set_style_text_color(_btnResponseSlow, active ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xB9C7D6), 0);
  }

  if (_lblPinState) {
    lv_label_set_text(_lblPinState, hasPinConfigured() ? "actif tactile" : "--");
    lv_obj_set_style_text_color(_lblPinState,
                                hasPinConfigured() ? lv_color_hex(0xB9C7D6) : lv_color_hex(0x6F8192),
                                0);
  }

  if (_btnPinConfigure) {
    lv_obj_t* lbl = lv_obj_get_child(_btnPinConfigure, 0);
    if (lbl) lv_label_set_text((lv_obj_t*)lbl, hasPinConfigured() ? "Modifier PIN" : "Configurer PIN");
  }

  if (_btnPinDisable) {
    setHiddenIfChanged(_btnPinDisable, !hasPinConfigured());
  }
}

void UiManager::onCalibrationCapture(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s || !self->_store) return;

  lv_obj_t* btn = lv_event_get_target(e);
  uint8_t row = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
  uint8_t index = row;
  if (row >= CALIBRATION_POINT_MAX || index >= self->_s->calibrationPointCount) return;

  if (g_audio.captureCalibrationPoint(*self->_s, index, self->_s->calPointRefDb[index])) {
    SettingsStore::syncActiveCalibrationProfile(*self->_s);
    self->_store->saveRuntimeSettings(*self->_s);
    self->refreshCalibrationView();
    return;
  }

  self->refreshCalibrationView();
}

void UiManager::onCalibrationClear(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s || !self->_store) return;

  g_audio.clearCalibration(*self->_s);
  SettingsStore::syncActiveCalibrationProfile(*self->_s);
  self->_store->saveRuntimeSettings(*self->_s);
  self->refreshCalibrationView();
}

void UiManager::onCalibrationRefChanged(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s || !self->_store) return;

  lv_obj_t* btn = lv_event_get_target(e);
  uint8_t encoded = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
  uint8_t row = encoded / 2;
  bool increment = (encoded % 2) == 1;
  uint8_t index = row;
  if (row >= CALIBRATION_POINT_MAX || index >= self->_s->calibrationPointCount) return;

  float next = self->_s->calPointRefDb[index] + (increment ? 5.0f : -5.0f);
  if (next < 35.0f) next = 35.0f;
  if (next > 110.0f) next = 110.0f;
  self->_s->calPointRefDb[index] = next;
  SettingsStore::syncActiveCalibrationProfile(*self->_s);
  self->_store->saveRuntimeSettings(*self->_s);
  self->refreshCalibrationView();
}

void UiManager::onCalibrationMode(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s || !self->_store) return;

  lv_obj_t* btn = lv_event_get_target(e);
  uint8_t pointCount = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
  pointCount = normalizedCalibrationPointCount((uint8_t)pointCount);
  if (pointCount == self->_s->calibrationPointCount) return;

  self->_s->calibrationPointCount = pointCount;
  g_audio.clearCalibration(*self->_s);
  SettingsStore::syncActiveCalibrationProfile(*self->_s);
  self->_store->saveRuntimeSettings(*self->_s);
  self->refreshCalibrationView();
}

bool UiManager::updateClockDisplay(lv_obj_t* lblDate, lv_obj_t* lblMain, lv_obj_t* lblSec,
                                   const char* dateText, const char* mainText, const char* secText) {
  bool changed = false;
  if (lblDate) changed |= setLabelTextIfChanged(lblDate, dateText);
  if (lblMain) changed |= setLabelTextIfChanged(lblMain, mainText);
  if (lblSec) changed |= setLabelTextIfChanged(lblSec, secText);
  return changed;
}

void UiManager::layoutClockFocus() {
  if (!_lblClockMainFocus || !_clockSecBadgeFocus) return;

  lv_obj_update_layout(_lblClockMainFocus);
  lv_coord_t mainW = lv_obj_get_width(_lblClockMainFocus);
  lv_coord_t badgeW = lv_obj_get_width(_clockSecBadgeFocus);
  const lv_coord_t gap = 12;
  const lv_coord_t y = 0;

  // Center the whole "HH:MM + seconds badge" group in the card.
  lv_coord_t mainX = -((badgeW + gap) / 2);
  lv_coord_t badgeX = (mainW / 2) + gap + (badgeW / 2) - ((badgeW + gap) / 2);

  lv_obj_align(_lblClockMainFocus, LV_ALIGN_CENTER, mainX, y);
  lv_obj_align(_clockSecBadgeFocus, LV_ALIGN_CENTER, badgeX, y);
}

void UiManager::showDashboard() {
  lv_scr_load(_scrDash);
}

void UiManager::showOtaStatusScreen(const char* title, const char* detail, uint8_t progressPct, bool success) {
  if (!_scrOta) return;
  if (progressPct > 100) progressPct = 100;
  if (!lvgl_port_lock(-1)) return;

  setLabelTextIfChanged(_lblOtaTitle, title ? title : "Mise a jour en cours");
  setLabelTextIfChanged(_lblOtaDetail, detail ? detail : "Reception du firmware en cours...");
  if (_barOtaProgress) {
    lv_obj_set_style_bg_color(_barOtaProgress,
                              success ? lv_color_hex(0x23C552) : lv_color_hex(0x39A0ED),
                              LV_PART_INDICATOR);
    lv_bar_set_value(_barOtaProgress, progressPct, LV_ANIM_OFF);
  }
  if (_lblOtaProgress) {
    char pctBuf[8];
    snprintf(pctBuf, sizeof(pctBuf), "%u%%", (unsigned)progressPct);
    setLabelTextIfChanged(_lblOtaProgress, pctBuf);
  }
  if (lv_scr_act() != _scrOta) {
    lv_scr_load(_scrOta);
  }
  lvgl_port_set_touch_enabled(false);
  lv_timer_handler();
  lvgl_port_unlock();
}

void UiManager::hideOtaStatusScreen() {
  if (!_scrOta) return;
  if (!lvgl_port_lock(-1)) return;
  if (_scrDash && lv_scr_act() == _scrOta) {
    lv_scr_load(_scrDash);
  }
  lvgl_port_set_touch_enabled(_s && _s->touchEnabled);
  lv_timer_handler();
  lvgl_port_unlock();
}

void UiManager::applyBacklight(uint8_t percent) {
  if (!_board) return;
  auto bl = _board->getBacklight();
  if (!bl) return;

  if (percent > 100) percent = 100;

  if (percent == 0) {
    bl->off();
    return;
  }

  bl->on();
  bl->setBrightness((int)percent);
}

lv_color_t UiManager::zoneColorForDb(float db) {
  if (!_s) return lv_color_hex(0x23C552);
  if (db <= _s->th.greenMax) return lv_color_hex(0x23C552);
  if (db <= _s->th.orangeMax) return lv_color_hex(0xF0A202);
  return lv_color_hex(0xE53935);
}

void UiManager::updateAlertState(uint32_t now) {
  if (!_s) return;

  const bool orange = isOrangeZone(_s, _lastDb);
  const bool red = isRedZone(_s, _lastDb);

  if (orange) {
    if (_orangeZoneSinceMs == 0) _orangeZoneSinceMs = now;
  } else {
    _orangeZoneSinceMs = 0;
  }

  if (red) {
    if (_redZoneSinceMs == 0) _redZoneSinceMs = now;
  } else {
    _redZoneSinceMs = 0;
  }
}

void UiManager::recordRedHistorySample(uint32_t now) {
  if (now - _lastRedHistorySampleMs < RED_HISTORY_SAMPLE_MS) return;
  _lastRedHistorySampleMs = now;

  const uint8_t sample = isRedZone(_s, _lastDb) ? 1 : 0;

  if (_redHistoryCount == RED_HISTORY_SAMPLE_COUNT) {
    _redHistorySum -= _redHistory[_redHistoryHead];
  } else {
    _redHistoryCount++;
  }

  _redHistory[_redHistoryHead] = sample;
  _redHistorySum += sample;
  _redHistoryHead = (_redHistoryHead + 1) % RED_HISTORY_SAMPLE_COUNT;
}

uint16_t UiManager::redSecondsWithinWindow() const {
  if (!_s || _redHistoryCount == 0) return 0;

  uint16_t wanted = (uint16_t)_s->historyMinutes * 60U;
  if (wanted > RED_HISTORY_SAMPLE_COUNT) wanted = RED_HISTORY_SAMPLE_COUNT;
  if (wanted == 0) return 0;

  uint16_t available = _redHistoryCount < wanted ? _redHistoryCount : wanted;
  uint16_t start = (_redHistoryHead + RED_HISTORY_SAMPLE_COUNT - available) % RED_HISTORY_SAMPLE_COUNT;
  uint16_t total = 0;
  for (uint16_t i = 0; i < available; i++) {
    total += _redHistory[(start + i) % RED_HISTORY_SAMPLE_COUNT];
  }
  return total;
}

void UiManager::applyAlertVisuals(uint32_t now) {
  const uint32_t orangeAlertHoldMs = _s ? _s->orangeAlertHoldMs : DEFAULT_WARNING_HOLD_MS;
  const uint32_t redAlertHoldMs = _s ? _s->redAlertHoldMs : DEFAULT_CRITICAL_HOLD_MS;
  const bool orangeAlert = _orangeZoneSinceMs != 0 && (now - _orangeZoneSinceMs) >= orangeAlertHoldMs;
  const bool redAlert = _redZoneSinceMs != 0 && (now - _redZoneSinceMs) >= redAlertHoldMs;
  const uint8_t alertState = redAlert ? 2 : (orangeAlert ? 1 : 0);
  uint8_t alertPhase = 0;
  if (redAlert) alertPhase = ((now / 120) % 10) < 5 ? 1 : 2;
  else if (orangeAlert) alertPhase = ((now / 180) % 10) < 5 ? 1 : 2;

  lv_color_t screenBase = lv_color_hex(0x0B0F14);
  lv_color_t screenRed = lv_color_hex(0x22080B);
  lv_color_t base = lv_color_hex(0x111824);
  lv_color_t orangeBase = lv_color_hex(0x3D2810);
  lv_color_t orangePulse = lv_color_hex(0x6C4311);
  lv_color_t redBase = lv_color_hex(0x4A161B);
  lv_color_t redPulse = lv_color_hex(0x8E1F28);
  lv_color_t borderBase = lv_color_hex(0x111824);
  lv_color_t orangeBorder = lv_color_hex(0xF0A202);
  lv_color_t redBorder = lv_color_hex(0xFF5A5F);

  lv_color_t dbCardColor = base;
  lv_color_t screenColor = screenBase;
  lv_color_t borderColor = borderBase;
  lv_opa_t borderOpa = LV_OPA_TRANSP;
  lv_coord_t borderWidth = 0;
  lv_coord_t shadowWidth = 0;
  if (redAlert) {
    dbCardColor = (alertPhase == 1) ? redPulse : redBase;
    screenColor = screenRed;
    borderColor = redBorder;
    borderOpa = LV_OPA_90;
    borderWidth = 3;
    shadowWidth = 26;
  } else if (orangeAlert) {
    dbCardColor = (alertPhase == 1) ? orangePulse : orangeBase;
    borderColor = orangeBorder;
    borderOpa = LV_OPA_70;
    borderWidth = 2;
    shadowWidth = 14;
  }

  if (_lastAlertVisualState != alertState || _lastAlertVisualPhase != alertPhase) {
    if (_scrDash) lv_obj_set_style_bg_color(_scrDash, screenColor, 0);
    if (_dbCard) lv_obj_set_style_bg_color(_dbCard, dbCardColor, 0);
    if (_dbCardFocus) lv_obj_set_style_bg_color(_dbCardFocus, dbCardColor, 0);
    if (_dbCard) {
      lv_obj_set_style_border_color(_dbCard, borderColor, 0);
      lv_obj_set_style_border_opa(_dbCard, borderOpa, 0);
      lv_obj_set_style_border_width(_dbCard, borderWidth, 0);
      lv_obj_set_style_shadow_color(_dbCard, borderColor, 0);
      lv_obj_set_style_shadow_width(_dbCard, shadowWidth, 0);
      lv_obj_set_style_shadow_opa(_dbCard, redAlert ? LV_OPA_50 : (orangeAlert ? LV_OPA_30 : LV_OPA_TRANSP), 0);
    }
    if (_dbCardFocus) {
      lv_obj_set_style_border_color(_dbCardFocus, borderColor, 0);
      lv_obj_set_style_border_opa(_dbCardFocus, borderOpa, 0);
      lv_obj_set_style_border_width(_dbCardFocus, borderWidth, 0);
      lv_obj_set_style_shadow_color(_dbCardFocus, borderColor, 0);
      lv_obj_set_style_shadow_width(_dbCardFocus, shadowWidth, 0);
      lv_obj_set_style_shadow_opa(_dbCardFocus, redAlert ? LV_OPA_50 : (orangeAlert ? LV_OPA_30 : LV_OPA_TRANSP), 0);
    }
    if (_alertBadgeFocus) {
      setHiddenIfChanged(_alertBadgeFocus, !redAlert);
      if (redAlert) lv_obj_move_foreground(_alertBadgeFocus);
    }
    _lastAlertVisualState = alertState;
    _lastAlertVisualPhase = alertPhase;
  }

  char buf[40];
  uint16_t redSeconds = redSecondsWithinWindow();
  if (redSeconds >= 60) {
    snprintf(buf, sizeof(buf), "Rouge %um%02us / %um",
             redSeconds / 60, redSeconds % 60, (unsigned)(_s ? _s->historyMinutes : 0));
  } else {
    snprintf(buf, sizeof(buf), "Rouge %us / %um",
             redSeconds, (unsigned)(_s ? _s->historyMinutes : 0));
  }

  setLabelTextIfChanged(_lblAlertTime, buf);
  setLabelTextIfChanged(_lblAlertTimeFocus, buf);
}

void UiManager::powerOffNow() {
  lv_obj_t* msg = lv_obj_create(lv_scr_act());
  lv_obj_set_size(msg, 260, 90);
  lv_obj_center(msg);
  lv_obj_set_style_radius(msg, 16, 0);
  lv_obj_set_style_border_width(msg, 0, 0);
  lv_obj_set_style_bg_color(msg, lv_color_hex(0x111824), 0);

  lv_obj_t* lbl = lv_label_create(msg);
  lv_label_set_text(lbl, "Extinction...\nReveil: bouton BOOT");
  lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(lbl);

  lv_timer_handler();
  delay(POWER_OFF_DIALOG_SETTLE_MS);

  applyBacklight(0);
  delay(POWER_OFF_BACKLIGHT_DELAY_MS);

  gpio_hold_dis(GPIO_NUM_0);
  rtc_gpio_pullup_dis(GPIO_NUM_0);
  rtc_gpio_pulldown_en(GPIO_NUM_0);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

  Serial0.println("[PWR] Deep sleep - wake on BOOT(GPIO0)");
  Serial0.flush();
  delay(POWER_OFF_FINAL_DELAY_MS);

  esp_deep_sleep_start();
}

void UiManager::setDb(float dbInstant, float leq, float peak) {
  _lastDb = dbInstant;
  _lastLeq = leq;
  _lastPeak = peak;

  const uint32_t now = millis();
  updateAlertState(now);
  applyAlertVisuals(now);
  if (_history && _historyRevision != _history->revision() &&
      (_currentDashPage == DASH_PAGE_OVERVIEW || _currentDashPage == DASH_PAGE_SOUND)) {
    redrawHistoryBars();
  }

  if (_currentDashPage != DASH_PAGE_OVERVIEW && _currentDashPage != DASH_PAGE_SOUND) {
    return;
  }
  if (_lastSoundUiUpdateMs != 0 && (now - _lastSoundUiUpdateMs) < SOUND_UI_UPDATE_MS) {
    return;
  }
  _lastSoundUiUpdateMs = now;

  char buf[32];

  snprintf(buf, sizeof(buf), "%.1f", dbInstant);
  if (_currentDashPage == DASH_PAGE_OVERVIEW) {
    setLabelTextIfChanged(_lblDb, buf);
  } else if (_currentDashPage == DASH_PAGE_SOUND) {
    setLabelTextIfChanged(_lblDbFocus, buf);
  }

  snprintf(buf, sizeof(buf), "%.1f", leq);
  if (_currentDashPage == DASH_PAGE_OVERVIEW) {
    setLabelTextIfChanged(_lblLeq, buf);
  } else if (_currentDashPage == DASH_PAGE_SOUND) {
    setLabelTextIfChanged(_lblLeqFocus, buf);
  }

  snprintf(buf, sizeof(buf), "%.1f", peak);
  if (_currentDashPage == DASH_PAGE_OVERVIEW) {
    setLabelTextIfChanged(_lblPeak, buf);
  } else if (_currentDashPage == DASH_PAGE_SOUND) {
    setLabelTextIfChanged(_lblPeakFocus, buf);
  }

  int v = (int)dbInstant;
  if (v < 0) v = 0;
  if (v > 100) v = 100;

  if (_currentDashPage == DASH_PAGE_OVERVIEW) {
    if (_arc) lv_arc_set_value(_arc, v);
  } else if (_currentDashPage == DASH_PAGE_SOUND) {
    if (_arcFocus) lv_arc_set_value(_arcFocus, v);
  }

  lv_color_t c = zoneColorForDb(dbInstant);
  if (_currentDashPage == DASH_PAGE_OVERVIEW) {
    if (_arc) lv_obj_set_style_arc_color(_arc, c, LV_PART_INDICATOR);
    if (_dot) lv_obj_set_style_bg_color(_dot, c, 0);
  } else if (_currentDashPage == DASH_PAGE_SOUND) {
    if (_arcFocus) lv_obj_set_style_arc_color(_arcFocus, c, LV_PART_INDICATOR);
    if (_dotFocus) lv_obj_set_style_bg_color(_dotFocus, c, 0);
  }
}

void UiManager::tick() {
  if (_requestedDashPage != UINT8_MAX) {
    const uint8_t requestedPage = _requestedDashPage;
    _requestedDashPage = UINT8_MAX;
    setDashboardPage(requestedPage);
  }

  uint32_t now = millis();
  struct tm ti;
  bool hasTime = false;
  bool timeLocked = _net && _net->timeIsValid();
  uint64_t clockUnixMs = 0;
  uint32_t clockDisplaySecond = UINT32_MAX;

  if ((_currentDashPage == DASH_PAGE_OVERVIEW || _currentDashPage == DASH_PAGE_CLOCK) &&
      (_lastClockUiUpdateMs == 0 || (now - _lastClockUiUpdateMs) >= CLOCK_UI_UPDATE_MS)) {
    _lastClockUiUpdateMs = now;
    bool clockChanged = false;
    clockUnixMs = currentClockUnixMs();
    hasTime = clockUnixMs > 0;
    if (!hasTime && _net) {
      hasTime = _net->localTime(&ti);
    }
    if (hasTime && clockUnixMs > 0) {
      const time_t displayEpoch = (time_t)((clockUnixMs + CLOCK_DISPLAY_PHASE_MS) / 1000ULL);
      localtime_r(&displayEpoch, &ti);
      clockDisplaySecond = (uint32_t)displayEpoch;
    } else if (hasTime) {
      clockDisplaySecond = (uint32_t)mktime(&ti);
    }

    if (hasTime && clockDisplaySecond != _lastClockDisplaySecond) {
      _lastClockDisplaySecond = clockDisplaySecond;
      char dateBuf[32];
      char mainBuf[8];
      char secBuf[8];

      strftime(dateBuf, sizeof(dateBuf), "%d/%m/%Y", &ti);
      strftime(mainBuf, sizeof(mainBuf), "%H:%M", &ti);
      strftime(secBuf, sizeof(secBuf), ":%S", &ti);

      if (_currentDashPage == DASH_PAGE_OVERVIEW) {
        clockChanged |= updateClockDisplay(_lblClockDate, _lblClockMain, _lblClockSec, dateBuf, mainBuf, secBuf);
      } else {
        clockChanged |= updateClockDisplay(_lblClockDateFocus, _lblClockMainFocus, nullptr, dateBuf, mainBuf, secBuf);
        clockChanged |= setLabelTextIfChanged(_lblClockSecFocus, secBuf + 1);
      }
    } else if (!hasTime) {
      _lastClockDisplaySecond = UINT32_MAX;
      if (_currentDashPage == DASH_PAGE_OVERVIEW) {
        clockChanged |= updateClockDisplay(_lblClockDate, _lblClockMain, _lblClockSec, "--/--/----", "--:--", ":--");
      } else {
        clockChanged |= updateClockDisplay(_lblClockDateFocus, _lblClockMainFocus, nullptr, "--/--/----", "--:--", ":--");
        clockChanged |= setLabelTextIfChanged(_lblClockSecFocus, "--");
      }
    }
    if (_currentDashPage == DASH_PAGE_CLOCK && clockChanged) {
      layoutClockFocus();
    }
  }

  if (_lblTime && _currentDashPage == DASH_PAGE_OVERVIEW) {
    if (hasTime) {
      char tbuf[16];
      strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &ti);
      setLabelTextIfChanged(_lblTime, tbuf);
    } else {
      setLabelTextIfChanged(_lblTime, "--:--:--");
    }
  }

  if (now - _lastTickMs < UI_TICK_PERIOD_MS) return;
  _lastTickMs = now;

  updateAlertState(now);
  recordRedHistorySample(now);
  applyAlertVisuals(now);

  if (_currentDashPage == DASH_PAGE_SETTINGS || _currentDashPage == DASH_PAGE_CLOCK) {
    if (timeLocked) {
      if (_currentDashPage == DASH_PAGE_SETTINGS) setLabelTextIfChanged(_lblNtpStatus, "NTP: LOCK");
      if (_currentDashPage == DASH_PAGE_CLOCK) setLabelTextIfChanged(_lblClockStatusFocus, "SYNC LOCK");
    } else {
      if (_currentDashPage == DASH_PAGE_SETTINGS) setLabelTextIfChanged(_lblNtpStatus, "NTP: WAIT");
      if (_currentDashPage == DASH_PAGE_CLOCK) setLabelTextIfChanged(_lblClockStatusFocus, "SYNC WAIT");
    }
  }

  if (_currentDashPage == DASH_PAGE_SETTINGS &&
      (_lastSettingsUiUpdateMs == 0 || (now - _lastSettingsUiUpdateMs) >= SETTINGS_UI_UPDATE_MS) &&
      _net && _s) {
    _lastSettingsUiUpdateMs = now;
    if (_lblWifiStatus) {
      if (_net->isWifiConnected()) {
        String s = "WiFi: OK  " + _net->ipString() + "  (RSSI " + String(_net->rssi()) + " dBm)";
        setLabelTextIfChanged(_lblWifiStatus, s.c_str());
      } else if (_net->isConfigPortalActive()) {
        setLabelTextIfChanged(_lblWifiStatus, "WiFi: AP");
      } else {
        setLabelTextIfChanged(_lblWifiStatus, "WiFi: OFF");
      }
    }
    String s;
    s.reserve(256);
    s += "NTP: ";
    s += _net->ntpServer();
    s += "\nTZ:  ";
    s += _net->tz();
    s += "\n";

    if (_net->isWifiConnected()) {
      s += "IP:   ";
      s += _net->ipString();
      s += "\nRSSI: ";
      s += String(_net->rssi());
      s += " dBm\n";
    } else {
      s += "WiFi: disconnected\n";
    }

    s += "Hist: ";
    s += String(_s->historyMinutes);
    s += " min";

    setLabelTextIfChanged(_lblNetInfo, s.c_str());
  }

  if (_currentDashPage == DASH_PAGE_CALIBRATION &&
      (_lastCalibrationUiUpdateMs == 0 || (now - _lastCalibrationUiUpdateMs) >= CALIBRATION_UI_UPDATE_MS)) {
    _lastCalibrationUiUpdateMs = now;
    refreshCalibrationView();
  }

  if (_dashPageBuilt[DASH_PAGE_LIVE]) refreshLiveControls();
}

void UiManager::onDashTab(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  lv_obj_t* target = lv_event_get_target(e);
  if (!self || !target) return;

  uintptr_t page = (uintptr_t)lv_obj_get_user_data(target);
  self->setDashboardPage((uint8_t)page);
}

void UiManager::onDashGesture(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self) return;

  lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT) {
    if (self->_currentDashPage + 1 < DASH_PAGE_COUNT) self->setDashboardPage(self->_currentDashPage + 1);
  } else if (dir == LV_DIR_RIGHT) {
    if (self->_currentDashPage > 0) self->setDashboardPage(self->_currentDashPage - 1);
  }
}

void UiManager::onOverviewCard(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  lv_obj_t* target = lv_event_get_target(e);
  if (!self || !target) return;

  uintptr_t page = (uintptr_t)lv_obj_get_user_data(target);
  self->setDashboardPage((uint8_t)page);
}

void UiManager::onLiveToggle(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s) return;

  self->_s->liveEnabled = self->_s->liveEnabled ? LIVE_DISABLED : LIVE_ENABLED;
  self->_lastLiveEnabled = 255;
  self->refreshLiveControls();

  if (self->_store) {
    self->_store->saveUiSettings(self->_s->backlight, self->_s->liveEnabled,
                                self->_s->touchEnabled, self->_s->dashboardPage,
                                self->_s->dashboardFullscreenMask);
  }
}

void UiManager::onToggleBacklight(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s) return;

  self->_s->backlight = self->_s->backlight == 0 ? 100 : 0;
  self->refreshSettingsControls();
  self->applyBacklight(self->_s->backlight);

  if (self->_store && self->_s) {
    self->_store->saveUiSettings(self->_s->backlight, self->_s->liveEnabled,
                                self->_s->touchEnabled, self->_s->dashboardPage,
                                self->_s->dashboardFullscreenMask);
  }
}

void UiManager::onSliderThresholds(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);

  int g = lv_slider_get_value(self->_slGreen);
  int o = lv_slider_get_value(self->_slOrange);

  if (o < g) {
    o = g;
    lv_slider_set_value(self->_slOrange, o, LV_ANIM_OFF);
  }

  if (self->_s) {
    self->_s->th.greenMax = (uint8_t)g;
    self->_s->th.orangeMax = (uint8_t)o;
  }
  self->refreshSettingsControls();

  self->redrawHistoryBars();
  self->_orangeZoneSinceMs = 0;
  self->_redZoneSinceMs = 0;
  self->_redHistoryHead = 0;
  self->_redHistoryCount = 0;
  self->_redHistorySum = 0;
  memset(self->_redHistory, 0, sizeof(self->_redHistory));
  self->applyAlertVisuals(millis());

  if (self->_store && self->_s) self->_store->saveThresholds(self->_s->th);
}

void UiManager::onSliderHistory(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  int v = lv_slider_get_value(self->_slHistory);

  if (self->_s) self->_s->historyMinutes = (uint8_t)v;
  self->refreshSettingsControls();
  self->redrawHistoryBars();
  if (self->_history) self->_history->settingsChanged();
  self->refreshCalibrationView();

  if (self->_store && self->_s) self->_store->saveRuntimeSettings(*self->_s);
}

void UiManager::onResponseMode(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  lv_obj_t* btn = lv_event_get_target(e);
  if (!self || !self->_s || !self->_store || !btn) return;

  uint8_t mode = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
  if (mode > 1) mode = 0;
  self->_s->audioResponseMode = mode;
  self->refreshSettingsControls();
  self->_store->saveAudioSettings(self->_s->audioSource, self->_s->analogRmsSamples,
                                  self->_s->audioResponseMode, self->_s->emaAlpha,
                                  self->_s->peakHoldMs, self->_s->analogBaseOffsetDb,
                                  self->_s->analogExtraOffsetDb);
}

void UiManager::onReboot(lv_event_t* e) {
  (void)e;
  ESP.restart();
}

void UiManager::onFactoryReset(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self) return;

  if (self->_msgboxConfirm) {
    lv_obj_del(self->_msgboxConfirm);
    self->_msgboxConfirm = nullptr;
  }

  lv_obj_t* overlay = lv_obj_create(lv_scr_act());
  self->_msgboxConfirm = overlay;

  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, lv_pct(100), lv_pct(100));
  lv_obj_center(overlay);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_50, 0);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

  lv_obj_t* box = lv_obj_create(overlay);
  lv_obj_set_size(box, 360, 190);
  lv_obj_center(box);
  lv_obj_set_style_radius(box, 18, 0);
  lv_obj_set_style_border_width(box, 0, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(0x111824), 0);
  lv_obj_set_style_pad_all(box, 16, 0);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* title = lv_label_create(box);
  lv_label_set_text(title, "Confirmation");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  lv_obj_t* txt = lv_label_create(box);
  lv_label_set_text(txt, "Factory reset ?\nTous les parametres seront effaces.");
  lv_obj_set_style_text_font(txt, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(txt, lv_color_hex(0xB9C7D6), 0);
  lv_obj_align(txt, LV_ALIGN_TOP_LEFT, 0, 42);

  lv_obj_t* btnNo = lv_btn_create(box);
  lv_obj_set_size(btnNo, 140, 44);
  lv_obj_align(btnNo, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_add_event_cb(btnNo, UiManager::onConfirmResetNo, LV_EVENT_CLICKED, self);

  lv_obj_t* lblNo = lv_label_create(btnNo);
  lv_label_set_text(lblNo, "Annuler");
  lv_obj_center(lblNo);

  lv_obj_t* btnYes = lv_btn_create(box);
  lv_obj_set_size(btnYes, 140, 44);
  lv_obj_align(btnYes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_color(btnYes, lv_color_hex(0x7A1E2C), 0);
  lv_obj_set_style_border_width(btnYes, 0, 0);
  lv_obj_add_event_cb(btnYes, UiManager::onConfirmResetYes, LV_EVENT_CLICKED, self);

  lv_obj_t* lblYes = lv_label_create(btnYes);
  lv_label_set_text(lblYes, "Confirmer");
  lv_obj_center(lblYes);
}

void UiManager::onConfirmResetNo(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self) return;

  if (self->_msgboxConfirm) {
    lv_obj_del(self->_msgboxConfirm);
    self->_msgboxConfirm = nullptr;
  }
}

void UiManager::onConfirmResetYes(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self) return;

  if (self->_msgboxConfirm) {
    lv_obj_del(self->_msgboxConfirm);
    self->_msgboxConfirm = nullptr;
  }

  if (self->_store) self->_store->factoryReset();
  delay(FACTORY_RESET_RESTART_DELAY_MS);
  ESP.restart();
}

void UiManager::onPowerOff(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  self->powerOffNow();
}

void UiManager::onPinDigit(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  lv_obj_t* btn = lv_event_get_target(e);
  if (!self || !btn) return;
  int32_t code = (int32_t)(intptr_t)lv_obj_get_user_data(btn);
  self->appendPinDigit((char)code);
}

void UiManager::onPinBackspace(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self) return;
  self->backspacePinDigit();
}

void UiManager::onPinCancel(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self) return;
  self->closePinOverlay();
}

void UiManager::onPinSubmit(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self) return;
  self->submitPinEntry();
}

void UiManager::onPinConfigure(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s || !self->_store) return;
  self->openPinOverlayForSet();
}

void UiManager::onPinDisable(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s || !self->_store) return;
  self->_s->dashboardPin[0] = '\0';
  self->_store->saveDashboardPin("");
  self->_touchPinUnlocked = false;
  self->refreshSettingsControls();
}

#else

void UiManager::begin(esp_panel::board::Board* board,
                      SettingsV1* settings,
                      SettingsStore* store,
                      NetManager* net,
                      SharedHistory* history) {
  (void)board;
  (void)store;
  (void)net;
  (void)history;
  _s = settings;
}

void UiManager::showDashboard() {}

void UiManager::showOtaStatusScreen(const char* title, const char* detail, uint8_t progressPct, bool success) {
  (void)title;
  (void)detail;
  (void)progressPct;
  (void)success;
}

void UiManager::hideOtaStatusScreen() {}

void UiManager::tick() {}

void UiManager::setDb(float dbInstant, float leq, float peak) {
  (void)dbInstant;
  (void)leq;
  (void)peak;
}

void UiManager::requestDashboardPage(uint8_t page, bool persistSelection) {
  (void)page;
  (void)persistSelection;
}

void UiManager::refreshDashboardLayout() {}

#endif
