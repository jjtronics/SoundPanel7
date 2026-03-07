\
#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct MeterMetrics {
  float db_inst = 0.0f;
  float db_smooth = 0.0f;
  float leq = 0.0f;
  float peak = 0.0f;
};

struct StatusFlags {
  bool wifi_connected = false;
  bool mqtt_connected = false;
  bool ntp_ok = false;
};

struct AppState {
  MeterMetrics metrics;
  StatusFlags status;
  time_t now = 0;
  int wifi_rssi = 0;
};

class AppStateStore {
public:
  AppStateStore();
  void setMetrics(const MeterMetrics& m);
  MeterMetrics getMetrics();
  void setStatus(const StatusFlags& s);
  StatusFlags getStatus();
  void setTime(time_t t);
  time_t getTime();
  void setWifiRssi(int rssi);
  int getWifiRssi();

private:
  AppState _state{};
  SemaphoreHandle_t _mtx;
};
