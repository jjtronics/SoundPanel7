#pragma once

#include <Arduino.h>
#include <functional>
#include <stdint.h>

class LiveEventServer {
public:
  LiveEventServer(uint16_t port, const char* path);
  ~LiveEventServer();

  void begin(std::function<bool(const String&)> authorizeToken,
             std::function<String()> initialPayloadFactory,
             std::function<String()> initialSystemPayloadFactory = nullptr);
  void close();
  size_t clientCount() const;
  void sendEvent(const char* eventName, const String& payload, uint32_t eventId);
  void sendMetrics(const String& payload, uint32_t eventId);

private:
  class Impl;
  Impl* _impl = nullptr;
};
