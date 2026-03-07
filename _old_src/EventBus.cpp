\
#include "EventBus.h"

EventBus::EventBus() {
  _q = xQueueCreate(16, sizeof(AppEvent));
}

bool EventBus::publish(AppEventType t) {
  AppEvent e{t};
  return xQueueSend(_q, &e, 0) == pdTRUE;
}

bool EventBus::poll(AppEvent* out, uint32_t timeout_ms) {
  TickType_t to = timeout_ms == 0 ? 0 : pdMS_TO_TICKS(timeout_ms);
  return xQueueReceive(_q, out, to) == pdTRUE;
}
