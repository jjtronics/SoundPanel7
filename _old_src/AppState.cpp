\
#include "AppState.h"

AppStateStore::AppStateStore() {
  _mtx = xSemaphoreCreateMutex();
}

void AppStateStore::setMetrics(const MeterMetrics& m) {
  xSemaphoreTake(_mtx, portMAX_DELAY);
  _state.metrics = m;
  xSemaphoreGive(_mtx);
}

MeterMetrics AppStateStore::getMetrics() {
  xSemaphoreTake(_mtx, portMAX_DELAY);
  auto m = _state.metrics;
  xSemaphoreGive(_mtx);
  return m;
}

void AppStateStore::setStatus(const StatusFlags& s) {
  xSemaphoreTake(_mtx, portMAX_DELAY);
  _state.status = s;
  xSemaphoreGive(_mtx);
}

StatusFlags AppStateStore::getStatus() {
  xSemaphoreTake(_mtx, portMAX_DELAY);
  auto s = _state.status;
  xSemaphoreGive(_mtx);
  return s;
}

void AppStateStore::setTime(time_t t) {
  xSemaphoreTake(_mtx, portMAX_DELAY);
  _state.now = t;
  xSemaphoreGive(_mtx);
}

time_t AppStateStore::getTime() {
  xSemaphoreTake(_mtx, portMAX_DELAY);
  auto t = _state.now;
  xSemaphoreGive(_mtx);
  return t;
}

void AppStateStore::setWifiRssi(int rssi) {
  xSemaphoreTake(_mtx, portMAX_DELAY);
  _state.wifi_rssi = rssi;
  xSemaphoreGive(_mtx);
}

int AppStateStore::getWifiRssi() {
  xSemaphoreTake(_mtx, portMAX_DELAY);
  auto r = _state.wifi_rssi;
  xSemaphoreGive(_mtx);
  return r;
}
