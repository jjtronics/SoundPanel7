#include "UiManager.h"

#include <cstdio>
#include <ctime>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include "../AudioEngine.h"

using namespace esp_panel::board;

LV_FONT_DECLARE(sp7_font_clock_170);

extern AudioEngine g_audio;

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

static lv_obj_t* mgmtCard(lv_obj_t* parent, const char* title) {
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_width(c, lv_pct(94));
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

void UiManager::begin(Board* board, SettingsV1* settings, SettingsStore* store, NetManager* net) {
  _board = board;
  _s = settings;
  _store = store;
  _net = net;

  buildDashboard();
  buildManagement();

  if (_s) applyBacklight(_s->backlight);
  showDashboard();
}

uint32_t UiManager::historySamplePeriodMs() const {
  uint8_t minutes = (_s ? _s->historyMinutes : 5);
  if (minutes < 1) minutes = 1;
  if (minutes > 60) minutes = 60;

  uint32_t totalMs = (uint32_t)minutes * 60UL * 1000UL;
  uint32_t p = totalMs / HISTORY_BAR_COUNT;
  if (p < 250) p = 250;
  return p;
}

void UiManager::pushHistory(float db) {
  _history[_historyHead] = db;
  _historyHead = (_historyHead + 1) % HISTORY_BAR_COUNT;
  if (_historyCount < HISTORY_BAR_COUNT) _historyCount++;
  redrawHistoryBars();
}

void UiManager::redrawHistoryBars() {
  if (!_histWrap && !_histWrapFocus) return;

  uint16_t start = (_historyCount < HISTORY_BAR_COUNT) ? 0 : _historyHead;

  for (uint16_t i = 0; i < HISTORY_BAR_COUNT; i++) {
    float v = 0.0f;
    if (i < _historyCount) {
      uint16_t idx = (start + i) % HISTORY_BAR_COUNT;
      v = _history[idx];
    }

    const float histDbMin = 35.0f;   // plancher visuel
    const float histDbMax = 100.0f;  // plafond visuel

    if (v < histDbMin) v = histDbMin;
    if (v > histDbMax) v = histDbMax;

    const int barMin = 4;
    const int barMax = 62;

    float norm = (v - histDbMin) / (histDbMax - histDbMin);
    int h = barMin + (int)(norm * (float)(barMax - barMin));

    if (h < barMin) h = barMin;
    if (h > barMax) h = barMax;

    lv_obj_t* bar = _histBars[i];
    if (bar) {
      lv_obj_set_height(bar, h);
      lv_obj_set_style_bg_color(bar, zoneColorForDb(v), 0);
    }

    lv_obj_t* barFocus = _histBarsFocus[i];
    if (barFocus) {
      lv_obj_set_height(barFocus, h);
      lv_obj_set_style_bg_color(barFocus, zoneColorForDb(v), 0);
    }
  }

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
  lv_obj_t* top = lv_obj_create(_scrDash);
  lv_obj_set_size(top, lv_pct(100), 96);
  lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(top, lv_color_hex(0x111824), 0);
  lv_obj_set_style_border_width(top, 0, 0);
  lv_obj_set_style_pad_all(top, 12, 0);
  lv_obj_set_style_pad_row(top, 10, 0);
  lv_obj_set_style_radius(top, 0, 0);
  lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(top, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_flex_flow(top, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

  lv_obj_t* topRow = lv_obj_create(top);
  lv_obj_set_size(topRow, lv_pct(100), 36);
  lv_obj_set_style_bg_opa(topRow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(topRow, 0, 0);
  lv_obj_set_style_pad_all(topRow, 0, 0);
  lv_obj_clear_flag(topRow, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(topRow, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* title = lv_label_create(topRow);
  lv_label_set_text(title, "SoundPanel 7");
  lv_obj_set_style_text_color(title, lv_color_hex(0xDFE7EF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t* gear = lv_btn_create(topRow);
  lv_obj_set_size(gear, 52, 40);
  lv_obj_align(gear, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_radius(gear, 14, 0);
  lv_obj_add_event_cb(gear, UiManager::onGear, LV_EVENT_CLICKED, this);

  lv_obj_t* gearLbl = lv_label_create(gear);
  lv_label_set_text(gearLbl, LV_SYMBOL_SETTINGS);
  lv_obj_center(gearLbl);

  lv_obj_t* tabs = lv_obj_create(top);
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

  static const char* kTabTitles[DASH_PAGE_COUNT] = {"Principal", "Horloge", "Sonometre", "Calibration"};
  for (uint8_t i = 0; i < DASH_PAGE_COUNT; i++) {
    _dashTabs[i] = lv_btn_create(tabs);
    lv_obj_set_size(_dashTabs[i], 118, 30);
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
  lv_obj_set_size(_dashContent, lv_pct(100), 340);
  lv_obj_align(_dashContent, LV_ALIGN_TOP_MID, 0, 98);
  lv_obj_set_style_bg_opa(_dashContent, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(_dashContent, 0, 0);
  lv_obj_set_style_pad_all(_dashContent, 0, 0);
  lv_obj_clear_flag(_dashContent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_dashContent, LV_SCROLLBAR_MODE_OFF);

  for (uint8_t i = 0; i < DASH_PAGE_COUNT; i++) {
    _dashPages[i] = makeDashboardPage(_dashContent);
    if (i != DASH_PAGE_OVERVIEW) lv_obj_add_flag(_dashPages[i], LV_OBJ_FLAG_HIDDEN);
  }

  buildDashboardOverviewPage(_dashPages[DASH_PAGE_OVERVIEW]);
  buildDashboardClockPage(_dashPages[DASH_PAGE_CLOCK]);
  buildDashboardSoundPage(_dashPages[DASH_PAGE_SOUND]);
  buildDashboardCalibrationPage(_dashPages[DASH_PAGE_CALIBRATION]);

  // ===== Footer =====
  lv_obj_t* bottom = lv_obj_create(_scrDash);
  lv_obj_set_size(bottom, lv_pct(100), 40);
  lv_obj_align(bottom, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(bottom, lv_color_hex(0x0B0F14), 0);
  lv_obj_set_style_border_width(bottom, 0, 0);
  lv_obj_set_style_radius(bottom, 0, 0);
  lv_obj_set_style_pad_all(bottom, 10, 0);
  lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(bottom, LV_SCROLLBAR_MODE_OFF);

  _lblWifi = lv_label_create(bottom);
  lv_label_set_text(_lblWifi, "WiFi: --");
  lv_obj_set_style_text_font(_lblWifi, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblWifi, lv_color_hex(0x6F8192), 0);

  _lblNtp = lv_label_create(bottom);
  lv_label_set_text(_lblNtp, "NTP: --");
  lv_obj_set_style_text_font(_lblNtp, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblNtp, lv_color_hex(0x6F8192), 0);

  _lblTime = nullptr;

  setDashboardPage(DASH_PAGE_OVERVIEW);
  refreshCalibrationView();
  redrawHistoryBars();
}

void UiManager::buildDashboardOverviewPage(lv_obj_t* parent) {
  lv_obj_t* upper = lv_obj_create(parent);
  lv_obj_set_size(upper, lv_pct(100), 224);
  lv_obj_align(upper, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_opa(upper, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(upper, 0, 0);
  lv_obj_set_style_pad_all(upper, 0, 0);
  lv_obj_clear_flag(upper, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(upper, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* clockCard = makeCard(upper, 170, 204);
  lv_obj_align(clockCard, LV_ALIGN_LEFT_MID, 24, 0);
  lv_obj_set_style_bg_color(clockCard, lv_color_hex(0x101A28), 0);
  lv_obj_set_style_radius(clockCard, 22, 0);
  lv_obj_set_style_pad_all(clockCard, 16, 0);

  _lblClockDate = lv_label_create(clockCard);
  lv_label_set_text(_lblClockDate, "--/--/----");
  lv_obj_set_style_text_font(_lblClockDate, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblClockDate, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(_lblClockDate, LV_ALIGN_TOP_MID, 0, 0);

  _lblClockMain = lv_label_create(clockCard);
  lv_label_set_text(_lblClockMain, "--:--");
  lv_obj_set_style_text_font(_lblClockMain, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(_lblClockMain, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblClockMain, LV_ALIGN_CENTER, 0, -8);

  lv_obj_t* secBadge = lv_obj_create(clockCard);
  lv_obj_set_size(secBadge, 78, 34);
  lv_obj_align(secBadge, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
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

  _arc = lv_arc_create(upper);
  lv_obj_set_size(_arc, 206, 206);
  lv_obj_align(_arc, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_range(_arc, 0, 100);
  lv_arc_set_value(_arc, 0);
  lv_arc_set_rotation(_arc, 135);
  lv_arc_set_bg_angles(_arc, 0, 270);
  lv_obj_remove_style(_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(_arc, 20, LV_PART_MAIN);
  lv_obj_set_style_arc_width(_arc, 20, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(_arc, lv_color_hex(0x1A2332), LV_PART_MAIN);
  lv_obj_set_style_arc_color(_arc, lv_color_hex(0x23C552), LV_PART_INDICATOR);

  _lblDb = lv_label_create(upper);
  lv_label_set_text(_lblDb, "--.-");
  lv_obj_set_style_text_font(_lblDb, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(_lblDb, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align_to(_lblDb, _arc, LV_ALIGN_CENTER, 0, -10);

  lv_obj_t* unit = lv_label_create(upper);
  lv_label_set_text(unit, "dB");
  lv_obj_set_style_text_font(unit, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(unit, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align_to(unit, _lblDb, LV_ALIGN_OUT_BOTTOM_MID, 0, -2);

  _dot = lv_obj_create(upper);
  lv_obj_set_size(_dot, 14, 14);
  lv_obj_set_style_radius(_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(_dot, lv_color_hex(0x23C552), 0);
  lv_obj_set_style_border_width(_dot, 0, 0);
  lv_obj_align_to(_dot, unit, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
  lv_obj_clear_flag(_dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_dot, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* rightCol = lv_obj_create(upper);
  lv_obj_set_size(rightCol, 170, 204);
  lv_obj_align(rightCol, LV_ALIGN_RIGHT_MID, -24, 0);
  lv_obj_set_style_bg_opa(rightCol, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(rightCol, 0, 0);
  lv_obj_set_style_pad_all(rightCol, 0, 0);
  lv_obj_set_style_pad_row(rightCol, 14, 0);
  lv_obj_set_flex_flow(rightCol, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(rightCol, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_clear_flag(rightCol, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(rightCol, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* c1 = makeCard(rightCol, 170, 95);
  lv_obj_t* c2 = makeCard(rightCol, 170, 95);

  lv_obj_t* t1 = lv_label_create(c1);
  lv_label_set_text(t1, "Leq");
  lv_obj_set_style_text_font(t1, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(t1, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 0);

  _lblLeq = lv_label_create(c1);
  lv_label_set_text(_lblLeq, "--.-");
  lv_obj_set_style_text_font(_lblLeq, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(_lblLeq, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblLeq, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  lv_obj_t* t2 = lv_label_create(c2);
  lv_label_set_text(t2, "Peak");
  lv_obj_set_style_text_font(t2, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(t2, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(t2, LV_ALIGN_TOP_MID, 0, 0);

  _lblPeak = lv_label_create(c2);
  lv_label_set_text(_lblPeak, "--.-");
  lv_obj_set_style_text_font(_lblPeak, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(_lblPeak, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblPeak, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  buildHistoryCard(parent, 0, 108, &_histWrap, &_lblHist, &_lblHistTLeft, &_lblHistTMid, &_lblHistTRight, _histBars);
  lv_obj_set_width(_histWrap, lv_pct(92));
  lv_obj_align(_histWrap, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void UiManager::buildDashboardClockPage(lv_obj_t* parent) {
  lv_obj_t* card = makeCard(parent, 752, 330);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x101A28), 0);
  lv_obj_set_style_radius(card, 26, 0);
  lv_obj_set_style_pad_all(card, 28, 0);

  _lblClockStatusFocus = nullptr;

  lv_obj_t* topRule = lv_obj_create(card);
  lv_obj_set_size(topRule, lv_pct(100), 2);
  lv_obj_align(topRule, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_set_style_bg_color(topRule, lv_color_hex(0x243244), 0);
  lv_obj_set_style_bg_opa(topRule, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(topRule, 0, 0);
  lv_obj_set_style_radius(topRule, 0, 0);
  lv_obj_clear_flag(topRule, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(topRule, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* clockWrap = lv_obj_create(card);
  lv_obj_set_size(clockWrap, 720, 224);
  lv_obj_align(clockWrap, LV_ALIGN_CENTER, 0, 18);
  lv_obj_set_style_bg_opa(clockWrap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(clockWrap, 0, 0);
  lv_obj_set_style_pad_all(clockWrap, 0, 0);
  lv_obj_clear_flag(clockWrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(clockWrap, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_flag(clockWrap, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

  _lblClockMainFocus = lv_label_create(clockWrap);
  lv_label_set_text(_lblClockMainFocus, "--:--");
  lv_obj_set_style_text_font(_lblClockMainFocus, &sp7_font_clock_170, 0);
  lv_obj_set_style_text_color(_lblClockMainFocus, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblClockMainFocus, LV_ALIGN_CENTER, 0, 0);

  _clockSecBadgeFocus = lv_obj_create(clockWrap);
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

  _lblClockDateFocus = lv_label_create(card);
  lv_label_set_text(_lblClockDateFocus, "--/--/----");
  lv_obj_set_style_text_font(_lblClockDateFocus, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(_lblClockDateFocus, lv_color_hex(0xB9C7D6), 0);
  lv_obj_align(_lblClockDateFocus, LV_ALIGN_TOP_RIGHT, 0, 0);

  layoutClockFocus();
}

void UiManager::buildDashboardSoundPage(lv_obj_t* parent) {
  const lv_coord_t cardGap = 12;

  lv_obj_t* mainCard = makeCard(parent, 752, 212);
  lv_obj_align(mainCard, LV_ALIGN_TOP_MID, 0, 10);
  lv_obj_set_style_bg_color(mainCard, lv_color_hex(0x111824), 0);
  lv_obj_set_style_radius(mainCard, 26, 0);
  lv_obj_set_style_pad_all(mainCard, 20, 0);

  _arcFocus = lv_arc_create(mainCard);
  lv_obj_set_size(_arcFocus, 196, 196);
  lv_obj_align(_arcFocus, LV_ALIGN_LEFT_MID, 14, 8);
  lv_arc_set_range(_arcFocus, 0, 100);
  lv_arc_set_value(_arcFocus, 0);
  lv_arc_set_rotation(_arcFocus, 135);
  lv_arc_set_bg_angles(_arcFocus, 0, 270);
  lv_obj_remove_style(_arcFocus, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(_arcFocus, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(_arcFocus, 22, LV_PART_MAIN);
  lv_obj_set_style_arc_width(_arcFocus, 22, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(_arcFocus, lv_color_hex(0x1A2332), LV_PART_MAIN);
  lv_obj_set_style_arc_color(_arcFocus, lv_color_hex(0x23C552), LV_PART_INDICATOR);

  _lblDbFocus = lv_label_create(mainCard);
  lv_label_set_text(_lblDbFocus, "--.-");
  lv_obj_set_style_text_font(_lblDbFocus, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(_lblDbFocus, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align_to(_lblDbFocus, _arcFocus, LV_ALIGN_CENTER, 0, -8);

  lv_obj_t* unit = lv_label_create(mainCard);
  lv_label_set_text(unit, "dB");
  lv_obj_set_style_text_font(unit, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(unit, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align_to(unit, _lblDbFocus, LV_ALIGN_OUT_BOTTOM_MID, 0, -2);

  _dotFocus = lv_obj_create(mainCard);
  lv_obj_set_size(_dotFocus, 16, 16);
  lv_obj_set_style_radius(_dotFocus, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(_dotFocus, lv_color_hex(0x23C552), 0);
  lv_obj_set_style_border_width(_dotFocus, 0, 0);
  lv_obj_align_to(_dotFocus, unit, LV_ALIGN_OUT_BOTTOM_MID, 0, 12);
  lv_obj_clear_flag(_dotFocus, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(_dotFocus, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* metricsWrap = lv_obj_create(mainCard);
  lv_obj_set_size(metricsWrap, 318, 160);
  lv_obj_align(metricsWrap, LV_ALIGN_RIGHT_MID, 0, 10);
  lv_obj_set_style_bg_opa(metricsWrap, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(metricsWrap, 0, 0);
  lv_obj_set_style_pad_all(metricsWrap, 0, 0);
  lv_obj_set_style_pad_column(metricsWrap, 14, 0);
  lv_obj_set_layout(metricsWrap, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(metricsWrap, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(metricsWrap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(metricsWrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(metricsWrap, LV_SCROLLBAR_MODE_OFF);

  lv_obj_t* leqCard = makeCard(metricsWrap, 152, 160);
  lv_obj_set_style_bg_color(leqCard, lv_color_hex(0x0E141C), 0);
  lv_obj_set_style_radius(leqCard, 22, 0);
  lv_obj_set_style_pad_all(leqCard, 18, 0);

  lv_obj_t* leqTitle = lv_label_create(leqCard);
  lv_label_set_text(leqTitle, "Leq");
  lv_obj_set_style_text_font(leqTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(leqTitle, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(leqTitle, LV_ALIGN_TOP_LEFT, 0, 0);

  _lblLeqFocus = lv_label_create(leqCard);
  lv_label_set_text(_lblLeqFocus, "--.-");
  lv_obj_set_style_text_font(_lblLeqFocus, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(_lblLeqFocus, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblLeqFocus, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  lv_obj_t* peakCard = makeCard(metricsWrap, 152, 160);
  lv_obj_set_style_bg_color(peakCard, lv_color_hex(0x0E141C), 0);
  lv_obj_set_style_radius(peakCard, 22, 0);
  lv_obj_set_style_pad_all(peakCard, 18, 0);

  lv_obj_t* peakTitle = lv_label_create(peakCard);
  lv_label_set_text(peakTitle, "Peak");
  lv_obj_set_style_text_font(peakTitle, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(peakTitle, lv_color_hex(0x8EA1B3), 0);
  lv_obj_align(peakTitle, LV_ALIGN_TOP_LEFT, 0, 0);

  _lblPeakFocus = lv_label_create(peakCard);
  lv_label_set_text(_lblPeakFocus, "--.-");
  lv_obj_set_style_text_font(_lblPeakFocus, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(_lblPeakFocus, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(_lblPeakFocus, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  buildHistoryCard(parent, 752, 112, &_histWrapFocus, &_lblHistFocus, &_lblHistTLeftFocus,
                   &_lblHistTMidFocus, &_lblHistTRightFocus, _histBarsFocus);
  lv_obj_align(_histWrapFocus, LV_ALIGN_TOP_MID, 0, 10 + 212 + cardGap);
}

void UiManager::buildHistoryCard(lv_obj_t* parent,
                                 int width,
                                 int height,
                                 lv_obj_t** wrapOut,
                                 lv_obj_t** titleOut,
                                 lv_obj_t** leftOut,
                                 lv_obj_t** midOut,
                                 lv_obj_t** rightOut,
                                 lv_obj_t* barsOut[HISTORY_BAR_COUNT]) {
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
  lv_obj_set_style_pad_column(histArea, 1, 0);
  lv_obj_set_layout(histArea, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(histArea, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(histArea, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_clear_flag(histArea, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(histArea, LV_SCROLLBAR_MODE_OFF);

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

  for (uint16_t i = 0; i < HISTORY_BAR_COUNT; i++) {
    barsOut[i] = lv_obj_create(histArea);
    lv_obj_set_width(barsOut[i], 5);
    lv_obj_set_height(barsOut[i], 4);
    lv_obj_set_style_radius(barsOut[i], 2, 0);
    lv_obj_set_style_bg_color(barsOut[i], lv_color_hex(0x23C552), 0);
    lv_obj_set_style_border_width(barsOut[i], 0, 0);
    lv_obj_clear_flag(barsOut[i], LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(barsOut[i], LV_SCROLLBAR_MODE_OFF);
  }

  *wrapOut = wrap;
  *titleOut = title;
  *leftOut = left;
  *midOut = mid;
  *rightOut = right;
}

void UiManager::buildDashboardCalibrationPage(lv_obj_t* parent) {
  const lv_coord_t cardTopOffset = 10;

  lv_obj_t* card = makeCard(parent, 752, 330);
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

  lv_obj_t* pointsWrap = lv_obj_create(card);
  lv_obj_set_size(pointsWrap, lv_pct(100), 194);
  lv_obj_align(pointsWrap, LV_ALIGN_TOP_MID, 0, 52);
  lv_obj_set_style_bg_color(pointsWrap, lv_color_hex(0x0E141C), 0);
  lv_obj_set_style_border_width(pointsWrap, 0, 0);
  lv_obj_set_style_radius(pointsWrap, 22, 0);
  lv_obj_set_style_pad_all(pointsWrap, 18, 0);
  lv_obj_set_style_pad_row(pointsWrap, 8, 0);
  lv_obj_set_flex_flow(pointsWrap, LV_FLEX_FLOW_COLUMN);
  lv_obj_clear_flag(pointsWrap, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(pointsWrap, LV_SCROLLBAR_MODE_OFF);

  for (uint8_t i = 0; i < 3; i++) {
    lv_obj_t* row = lv_obj_create(pointsWrap);
    lv_obj_set_size(row, lv_pct(100), 42);
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
    lv_obj_set_size(_btnCalRefMinus[i], 34, 34);
    lv_obj_align(_btnCalRefMinus[i], LV_ALIGN_RIGHT_MID, -260, 0);
    lv_obj_set_style_radius(_btnCalRefMinus[i], 12, 0);
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
    lv_obj_set_size(refBadge, 74, 34);
    lv_obj_align(refBadge, LV_ALIGN_RIGHT_MID, -182, 0);
    lv_obj_set_style_radius(refBadge, 12, 0);
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
    lv_obj_set_size(_btnCalRefPlus[i], 34, 34);
    lv_obj_align(_btnCalRefPlus[i], LV_ALIGN_RIGHT_MID, -144, 0);
    lv_obj_set_style_radius(_btnCalRefPlus[i], 12, 0);
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
    lv_obj_set_size(_btnCalCapture[i], 126, 36);
    lv_obj_align(_btnCalCapture[i], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(_btnCalCapture[i], 14, 0);
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
  lv_obj_set_size(clearBtn, 190, 36);
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
}

void UiManager::refreshCalibrationView() {
  if (!_s) return;

  uint8_t validCount = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (_s->calPointValid[i]) validCount++;
    if (_lblCalRef[i]) {
      char refBuf[16];
      snprintf(refBuf, sizeof(refBuf), "%0.0f", _s->calPointRefDb[i]);
      lv_label_set_text(_lblCalRef[i], refBuf);
    }
    if (_lblCalPoint[i]) {
      char buf[96];
      if (_s->calPointValid[i]) {
        snprintf(buf, sizeof(buf), "Point %u  capture %0.3f",
                 (unsigned)(i + 1),
                 _s->calPointRawLogRms[i]);
      } else {
        snprintf(buf, sizeof(buf), "Point %u  non capture", (unsigned)(i + 1));
      }
      lv_label_set_text(_lblCalPoint[i], buf);
    }
  }

  if (_lblCalStatus) {
    char buf[48];
    snprintf(buf, sizeof(buf), "%u / 3 points valides", (unsigned)validCount);
    lv_label_set_text(_lblCalStatus, buf);
  }

}

void UiManager::onCalibrationCapture(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s || !self->_store) return;

  lv_obj_t* btn = lv_event_get_target(e);
  uint8_t index = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
  if (index >= 3) return;

  if (g_audio.captureCalibrationPoint(*self->_s, index, self->_s->calPointRefDb[index])) {
    self->_store->save(*self->_s);
    self->refreshCalibrationView();
    return;
  }

  self->refreshCalibrationView();
}

void UiManager::onCalibrationClear(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s || !self->_store) return;

  g_audio.clearCalibration(*self->_s);
  self->_store->save(*self->_s);
  self->refreshCalibrationView();
}

void UiManager::onCalibrationRefChanged(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  if (!self || !self->_s || !self->_store) return;

  lv_obj_t* btn = lv_event_get_target(e);
  uint8_t encoded = (uint8_t)(uintptr_t)lv_obj_get_user_data(btn);
  uint8_t index = encoded / 2;
  bool increment = (encoded % 2) == 1;
  if (index >= 3) return;

  float next = self->_s->calPointRefDb[index] + (increment ? 5.0f : -5.0f);
  if (next < 35.0f) next = 35.0f;
  if (next > 100.0f) next = 100.0f;
  self->_s->calPointRefDb[index] = next;
  self->_store->save(*self->_s);
  self->refreshCalibrationView();
}

void UiManager::updateClockDisplay(lv_obj_t* lblDate, lv_obj_t* lblMain, lv_obj_t* lblSec,
                                   const char* dateText, const char* mainText, const char* secText) {
  if (lblDate) lv_label_set_text(lblDate, dateText);
  if (lblMain) lv_label_set_text(lblMain, mainText);
  if (lblSec) lv_label_set_text(lblSec, secText);
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

void UiManager::buildManagement() {
  _scrMgmt = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(_scrMgmt, lv_color_hex(0x0B0F14), 0);
  lv_obj_set_style_bg_opa(_scrMgmt, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(_scrMgmt, 0, 0);

  lv_obj_t* top = lv_obj_create(_scrMgmt);
  lv_obj_set_size(top, lv_pct(100), 70);
  lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(top, lv_color_hex(0x111824), 0);
  lv_obj_set_style_border_width(top, 0, 0);
  lv_obj_set_style_pad_all(top, 14, 0);
  lv_obj_set_style_radius(top, 0, 0);

  lv_obj_t* back = lv_btn_create(top);
  lv_obj_set_size(back, 72, 40);
  lv_obj_align(back, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_set_style_radius(back, 14, 0);
  lv_obj_add_event_cb(back, UiManager::onBack, LV_EVENT_CLICKED, this);

  lv_obj_t* backLbl = lv_label_create(back);
  lv_label_set_text(backLbl, LV_SYMBOL_LEFT);
  lv_obj_center(backLbl);

  lv_obj_t* title = lv_label_create(top);
  lv_label_set_text(title, "Management");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xDFE7EF), 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* scroll = lv_obj_create(_scrMgmt);
  lv_obj_set_size(scroll, lv_pct(100), 408);
  lv_obj_align(scroll, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_opa(scroll, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(scroll, 0, 0);
  lv_obj_set_style_pad_all(scroll, 10, 0);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(scroll, 12, 0);

  lv_obj_t* cUi = mgmtCard(scroll, "UI");

  lv_obj_t* blLbl = lv_label_create(cUi);
  lv_label_set_text(blLbl, "Backlight");
  lv_obj_set_style_text_color(blLbl, lv_color_hex(0xB9C7D6), 0);

  _slBacklight = lv_slider_create(cUi);
  lv_slider_set_range(_slBacklight, 0, 100);
  lv_slider_set_value(_slBacklight, _s ? _s->backlight : 80, LV_ANIM_OFF);
  lv_obj_add_event_cb(_slBacklight, UiManager::onSliderBacklight, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_t* cTh = mgmtCard(scroll, "Seuils dB");

  lv_obj_t* g = lv_label_create(cTh);
  lv_label_set_text(g, "Vert <= ");
  lv_obj_set_style_text_color(g, lv_color_hex(0xB9C7D6), 0);

  _slGreen = lv_slider_create(cTh);
  lv_slider_set_range(_slGreen, 0, 100);
  lv_slider_set_value(_slGreen, _s ? _s->th.greenMax : 55, LV_ANIM_OFF);
  lv_obj_add_event_cb(_slGreen, UiManager::onSliderThresholds, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_t* o = lv_label_create(cTh);
  lv_label_set_text(o, "Orange <= ");
  lv_obj_set_style_text_color(o, lv_color_hex(0xB9C7D6), 0);

  _slOrange = lv_slider_create(cTh);
  lv_slider_set_range(_slOrange, 0, 100);
  lv_slider_set_value(_slOrange, _s ? _s->th.orangeMax : 70, LV_ANIM_OFF);
  lv_obj_add_event_cb(_slOrange, UiManager::onSliderThresholds, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_t* cHist = mgmtCard(scroll, "Historique");

  lv_obj_t* hLbl = lv_label_create(cHist);
  lv_label_set_text(hLbl, "Minutes");
  lv_obj_set_style_text_color(hLbl, lv_color_hex(0xB9C7D6), 0);

  _slHistory = lv_slider_create(cHist);
  lv_slider_set_range(_slHistory, 1, 60);
  lv_slider_set_value(_slHistory, _s ? _s->historyMinutes : 5, LV_ANIM_OFF);
  lv_obj_add_event_cb(_slHistory, UiManager::onSliderHistory, LV_EVENT_VALUE_CHANGED, this);

  lv_obj_t* cNet = mgmtCard(scroll, "Reseau & Heure");
  _lblNetInfo = lv_label_create(cNet);
  lv_label_set_text(_lblNetInfo, "-");
  lv_obj_set_style_text_font(_lblNetInfo, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(_lblNetInfo, lv_color_hex(0xB9C7D6), 0);

  lv_obj_t* cMaint = mgmtCard(scroll, "Maintenance");

  lv_obj_t* reboot = lv_btn_create(cMaint);
  lv_obj_set_width(reboot, lv_pct(100));
  lv_obj_add_event_cb(reboot, UiManager::onReboot, LV_EVENT_CLICKED, this);
  lv_obj_t* rebootLbl = lv_label_create(reboot);
  lv_label_set_text(rebootLbl, "Reboot");
  lv_obj_center(rebootLbl);

  lv_obj_t* poweroff = lv_btn_create(cMaint);
  lv_obj_set_width(poweroff, lv_pct(100));
  lv_obj_add_event_cb(poweroff, UiManager::onPowerOff, LV_EVENT_CLICKED, this);
  lv_obj_t* poweroffLbl = lv_label_create(poweroff);
  lv_label_set_text(poweroffLbl, "Eteindre");
  lv_obj_center(poweroffLbl);

  lv_obj_t* reset = lv_btn_create(cMaint);
  lv_obj_set_width(reset, lv_pct(100));
  lv_obj_add_event_cb(reset, UiManager::onFactoryReset, LV_EVENT_CLICKED, this);
  lv_obj_t* resetLbl = lv_label_create(reset);
  lv_label_set_text(resetLbl, "Factory reset");
  lv_obj_center(resetLbl);
}

void UiManager::showDashboard() {
  lv_scr_load(_scrDash);
}

void UiManager::showManagement() {
  lv_scr_load(_scrMgmt);
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
  delay(250);

  applyBacklight(0);
  delay(80);

  gpio_hold_dis(GPIO_NUM_0);
  rtc_gpio_pullup_dis(GPIO_NUM_0);
  rtc_gpio_pulldown_en(GPIO_NUM_0);

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);

  Serial0.println("[PWR] Deep sleep - wake on BOOT(GPIO0)");
  Serial0.flush();
  delay(50);

  esp_deep_sleep_start();
}

void UiManager::setDb(float dbInstant, float leq, float peak) {
  _lastDb = dbInstant;
  _lastLeq = leq;
  _lastPeak = peak;

  char buf[32];

  snprintf(buf, sizeof(buf), "%.1f", dbInstant);
  if (_lblDb) lv_label_set_text(_lblDb, buf);
  if (_lblDbFocus) lv_label_set_text(_lblDbFocus, buf);

  snprintf(buf, sizeof(buf), "%.1f", leq);
  if (_lblLeq) lv_label_set_text(_lblLeq, buf);
  if (_lblLeqFocus) lv_label_set_text(_lblLeqFocus, buf);

  snprintf(buf, sizeof(buf), "%.1f", peak);
  if (_lblPeak) lv_label_set_text(_lblPeak, buf);
  if (_lblPeakFocus) lv_label_set_text(_lblPeakFocus, buf);

  int v = (int)dbInstant;
  if (v < 0) v = 0;
  if (v > 100) v = 100;

  if (_arc) lv_arc_set_value(_arc, v);
  if (_arcFocus) lv_arc_set_value(_arcFocus, v);

  lv_color_t c = zoneColorForDb(dbInstant);
  if (_arc) lv_obj_set_style_arc_color(_arc, c, LV_PART_INDICATOR);
  if (_arcFocus) lv_obj_set_style_arc_color(_arcFocus, c, LV_PART_INDICATOR);
  if (_dot) lv_obj_set_style_bg_color(_dot, c, 0);
  if (_dotFocus) lv_obj_set_style_bg_color(_dotFocus, c, 0);

  uint32_t now = millis();
  if (now - _lastHistoryPushMs >= historySamplePeriodMs()) {
    _lastHistoryPushMs = now;
    pushHistory(dbInstant);
  }
}

void UiManager::tick() {
  uint32_t now = millis();
  if (now - _lastTickMs < 250) return;
  _lastTickMs = now;

  if (_lblWifi && _net) {
    if (_net->isWifiConnected()) {
      String s = "WiFi: OK  " + _net->ipString() + "  (RSSI " + String(_net->rssi()) + " dBm)";
      lv_label_set_text(_lblWifi, s.c_str());
    } else {
      lv_label_set_text(_lblWifi, "WiFi: OFF");
    }
  }

  struct tm ti;
  bool hasTime = _net && _net->localTime(&ti);
  bool timeLocked = _net && _net->timeIsValid();

  if (_lblClockDate && _lblClockMain && _lblClockSec) {
    if (hasTime) {
      char dateBuf[32];
      char mainBuf[8];
      char secBuf[8];

      strftime(dateBuf, sizeof(dateBuf), "%d/%m/%Y", &ti);
      strftime(mainBuf, sizeof(mainBuf), "%H:%M", &ti);
      strftime(secBuf, sizeof(secBuf), ":%S", &ti);

      updateClockDisplay(_lblClockDate, _lblClockMain, _lblClockSec, dateBuf, mainBuf, secBuf);
      updateClockDisplay(_lblClockDateFocus, _lblClockMainFocus, nullptr, dateBuf, mainBuf, secBuf);
      if (_lblClockSecFocus) lv_label_set_text(_lblClockSecFocus, secBuf + 1);
      layoutClockFocus();
    } else {
      updateClockDisplay(_lblClockDate, _lblClockMain, _lblClockSec, "--/--/----", "--:--", ":--");
      updateClockDisplay(_lblClockDateFocus, _lblClockMainFocus, nullptr, "--/--/----", "--:--", ":--");
      if (_lblClockSecFocus) lv_label_set_text(_lblClockSecFocus, "--");
      layoutClockFocus();
    }
  }

  if (_lblTime) {
    if (hasTime) {
      char tbuf[16];
      strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &ti);
      lv_label_set_text(_lblTime, tbuf);
    } else {
      lv_label_set_text(_lblTime, "--:--:--");
    }
  }

  if (_lblNtp) {
    if (timeLocked) {
      lv_label_set_text(_lblNtp, "NTP: LOCK");
      if (_lblClockStatusFocus) lv_label_set_text(_lblClockStatusFocus, "SYNC LOCK");
    } else {
      lv_label_set_text(_lblNtp, "NTP: WAIT");
      if (_lblClockStatusFocus) lv_label_set_text(_lblClockStatusFocus, "SYNC WAIT");
    }
  }

  if (_lblNetInfo && _net && _s) {
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

    lv_label_set_text(_lblNetInfo, s.c_str());
  }

  refreshCalibrationView();
}

void UiManager::onGear(lv_event_t* e) {
  selfFromEvent(e)->showManagement();
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

void UiManager::onBack(lv_event_t* e) {
  selfFromEvent(e)->showDashboard();
}

void UiManager::onSliderBacklight(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  int v = lv_slider_get_value(self->_slBacklight);

  if (self->_s) self->_s->backlight = (uint8_t)v;
  self->applyBacklight((uint8_t)v);

  if (self->_store && self->_s) self->_store->save(*self->_s);
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

  self->redrawHistoryBars();

  if (self->_store && self->_s) self->_store->save(*self->_s);
}

void UiManager::onSliderHistory(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  int v = lv_slider_get_value(self->_slHistory);

  if (self->_s) self->_s->historyMinutes = (uint8_t)v;
  self->redrawHistoryBars();
  self->_lastHistoryPushMs = 0;
  self->refreshCalibrationView();

  if (self->_store && self->_s) self->_store->save(*self->_s);
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
  delay(200);
  ESP.restart();
}

void UiManager::onPowerOff(lv_event_t* e) {
  UiManager* self = selfFromEvent(e);
  self->powerOffNow();
}
