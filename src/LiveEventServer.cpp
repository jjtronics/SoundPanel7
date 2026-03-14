#include "LiveEventServer.h"

#include <ESPAsyncWebServer.h>

class LiveEventServer::Impl {
public:
  Impl(uint16_t port, const char* path)
      : server(port), events(path ? path : "/api/events") {}

  AsyncWebServer server;
  AsyncEventSource events;
};

LiveEventServer::LiveEventServer(uint16_t port, const char* path)
    : _impl(new Impl(port, path)) {}

LiveEventServer::~LiveEventServer() {
  delete _impl;
}

void LiveEventServer::begin(std::function<bool(const String&)> authorizeToken,
                            std::function<String()> initialPayloadFactory,
                            std::function<String()> initialSystemPayloadFactory) {
  if (!_impl) return;

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Cache-Control", "no-cache");

  _impl->events.authorizeConnect(
      [authorizeToken](AsyncWebServerRequest* request) -> bool {
        if (!request || !request->hasParam("t")) return false;
        const AsyncWebParameter* tokenParam = request->getParam("t");
        if (!tokenParam) return false;
        return authorizeToken ? authorizeToken(tokenParam->value()) : false;
      });

  _impl->events.onConnect(
      [initialPayloadFactory, initialSystemPayloadFactory](AsyncEventSourceClient* client) {
        if (!client || !initialPayloadFactory) return;
        const String payload = initialPayloadFactory();
        const uint32_t eventId = millis();
        client->send(payload.c_str(), "metrics", eventId, 500);
        if (initialSystemPayloadFactory) {
          const String systemPayload = initialSystemPayloadFactory();
          client->send(systemPayload.c_str(), "system", eventId + 1, 500);
        }
      });

  _impl->server.addHandler(&_impl->events);
  _impl->server.begin();
}

void LiveEventServer::close() {
  if (_impl) _impl->events.close();
}

size_t LiveEventServer::clientCount() const {
  return _impl ? _impl->events.count() : 0;
}

void LiveEventServer::sendEvent(const char* eventName, const String& payload, uint32_t eventId) {
  if (!_impl) return;
  _impl->events.send(payload.c_str(), eventName ? eventName : "message", eventId);
}

void LiveEventServer::sendMetrics(const String& payload, uint32_t eventId) {
  sendEvent("metrics", payload, eventId);
}
