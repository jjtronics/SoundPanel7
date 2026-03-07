#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum class AppEventType : uint8_t {
  SETTINGS_CHANGED = 1,
  WIFI_CHANGED,
  MQTT_CHANGED,
  AUDIO_CHANGED,
  REBOOT_REQUEST,
  FACTORY_RESET_REQUEST,
  MQTT_TEST_PUBLISH,
  WIFI_START_PORTAL
};

struct AppEvent {
  AppEventType type;
};

class EventBus {
public:
  EventBus();
  bool publish(AppEventType t);
  bool poll(AppEvent* out, uint32_t timeout_ms);

private:
  QueueHandle_t _q;
};