#include "NotificationTask.h"
#include "WebManager.h"
#include "DebugLog.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

#define Serial0 DebugSerial0

QueueHandle_t NotificationTask::_queue = nullptr;
TaskHandle_t NotificationTask::_taskHandle = nullptr;
NotificationResult NotificationTask::_lastResult = {};
SemaphoreHandle_t NotificationTask::_resultMutex = nullptr;

static bool postJsonToUrlStatic(const String& url,
                               const String& payload,
                               const String& authorization,
                               int& statusCodeOut,
                               String& responseOut) {
  if (!WiFi.isConnected()) {
    responseOut = "WiFi not connected";
    statusCodeOut = -1;
    return false;
  }

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  if (authorization.length()) {
    http.addHeader("Authorization", authorization);
  }
  http.setTimeout(8000);

  const int httpCode = http.POST(payload);
  statusCodeOut = httpCode;

  if (httpCode > 0) {
    responseOut = http.getString();
  } else {
    responseOut = http.errorToString(httpCode);
  }

  http.end();
  return (httpCode >= 200 && httpCode < 300);
}

static bool sendSlackNotificationStatic(const NotificationRequest& req,
                                       uint8_t alertState,
                                       bool isTest,
                                       const String& message,
                                       String& summary) {
  if (!req.slackWebhookUrl[0]) {
    summary = "Slack: webhook URL manquante";
    return false;
  }

  String color = "#28a745";
  if (alertState == 1) color = "#ffc107";
  if (alertState == 2) color = "#dc3545";
  if (isTest) color = "#6c757d";

  String payload;
  payload.reserve(512);
  payload += "{";
  if (req.slackChannel[0]) {
    payload += "\"channel\":\"";
    payload += req.slackChannel;
    payload += "\",";
  }
  payload += "\"attachments\":[{";
  payload += "\"color\":\"";
  payload += color;
  payload += "\",\"text\":\"";

  for (size_t i = 0; i < message.length(); i++) {
    char c = message[i];
    if (c == '"') payload += "\\\"";
    else if (c == '\\') payload += "\\\\";
    else if (c == '\n') payload += "\\n";
    else payload += c;
  }

  payload += "\"}]}";

  int statusCode = 0;
  String response;
  const bool ok = postJsonToUrlStatic(req.slackWebhookUrl, payload, "", statusCode, response);
  summary = String("Slack: ") + (ok ? "OK" : String("Erreur ") + String(statusCode));
  return ok;
}

static bool sendTelegramNotificationStatic(const NotificationRequest& req,
                                          const String& message,
                                          String& summary) {
  if (!req.telegramBotToken[0] || !req.telegramChatId[0]) {
    summary = "Telegram: config incomplete";
    return false;
  }

  String url;
  url.reserve(128);
  url += "https://api.telegram.org/bot";
  url += req.telegramBotToken;
  url += "/sendMessage";

  String payload;
  payload.reserve(512);
  payload += "{\"chat_id\":\"";
  payload += req.telegramChatId;
  payload += "\",\"text\":\"";

  for (size_t i = 0; i < message.length(); i++) {
    char c = message[i];
    if (c == '"') payload += "\\\"";
    else if (c == '\\') payload += "\\\\";
    else if (c == '\n') payload += "\\n";
    else payload += c;
  }

  payload += "\"}";

  int statusCode = 0;
  String response;
  const bool ok = postJsonToUrlStatic(url, payload, "", statusCode, response);
  summary = String("Telegram: ") + (ok ? "OK" : String("Erreur ") + String(statusCode));
  return ok;
}

static bool sendWhatsappNotificationStatic(const NotificationRequest& req,
                                          const String& message,
                                          String& summary) {
  if (!req.whatsappAccessToken[0] || !req.whatsappPhoneNumberId[0] || !req.whatsappRecipient[0]) {
    summary = "WhatsApp: config incomplete";
    return false;
  }

  String url;
  url.reserve(128);
  url += "https://graph.facebook.com/";
  url += req.whatsappApiVersion;
  url += "/";
  url += req.whatsappPhoneNumberId;
  url += "/messages";

  String payload;
  payload.reserve(512);
  payload += "{\"messaging_product\":\"whatsapp\",\"to\":\"";
  payload += req.whatsappRecipient;
  payload += "\",\"type\":\"text\",\"text\":{\"body\":\"";

  for (size_t i = 0; i < message.length(); i++) {
    char c = message[i];
    if (c == '"') payload += "\\\"";
    else if (c == '\\') payload += "\\\\";
    else if (c == '\n') payload += "\\n";
    else payload += c;
  }

  payload += "\"}}";

  String auth;
  auth.reserve(256);
  auth += "Bearer ";
  auth += req.whatsappAccessToken;

  int statusCode = 0;
  String response;
  const bool ok = postJsonToUrlStatic(url, payload, auth, statusCode, response);
  summary = String("WhatsApp: ") + (ok ? "OK" : String("Erreur ") + String(statusCode));
  return ok;
}

static String buildNotificationMessageStatic(const NotificationRequest& req,
                                            uint8_t alertState,
                                            bool isTest,
                                            float dbInstant,
                                            float leq,
                                            float peak,
                                            uint32_t durationMs) {
  const char* hostname = req.hostname[0] ? req.hostname : "soundpanel7";
  const char* emoji = (alertState == 2) ? "🔴" : (alertState == 1) ? "🟠" : "🟢";
  const char* title = (alertState == 2) ? "ALERTE CRITIQUE" :
                      (alertState == 1) ? "ALERTE WARNING" :
                      isTest ? "TEST" : "RETOUR A LA NORMALE";
  const float triggerThreshold = (alertState == 2) ? req.orangeMax : req.greenMax;
  const char* triggerLabel = (alertState == 2) ? "Seuil rouge" : "Seuil warning";

  struct tm ti;
  char tbuf[32] = {0};
  const bool hasTime = getLocalTime(&ti, 0);
  if (hasTime) strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &ti);

  String message;
  message.reserve(512);
  message += emoji;
  message += " SoundPanel 7\n";
  message += "• ";
  message += title;
  message += " - ";
  message += String(dbInstant, 1);
  message += " dB";

  if (!isTest && alertState > 0) {
    message += "\n• ";
    message += triggerLabel;
    message += ": > ";
    message += String(triggerThreshold, 0);
    message += " dB";
  } else if (!isTest && alertState == 0 && durationMs > 0) {
    message += "\n• Duree alerte: ";
    uint32_t sec = durationMs / 1000;
    if (sec < 60) {
      message += String(sec);
      message += " sec";
    } else {
      message += String(sec / 60);
      message += " min ";
      message += String(sec % 60);
      message += " sec";
    }
  }

  message += "\n• Equipement: ";
  message += hostname;
  message += "\n\n📊 Mesures\n• dB instantane: ";
  message += String(dbInstant, 1);
  message += " dB\n• Leq: ";
  message += String(leq, 1);
  message += " dB\n• Peak: ";
  message += String(peak, 1);
  message += " dB";

  message += "\n\n🎯 Seuils\n• Vert <= ";
  message += String(req.greenMax);
  message += " dB\n• Orange <= ";
  message += String(req.orangeMax);
  message += " dB\n• Rouge > ";
  message += String(req.orangeMax);
  message += " dB";

  if (hasTime) {
    message += "\n\n🕐 ";
    message += tbuf;
  }

  return message;
}

bool NotificationTask::processNotification(const NotificationRequest& req, NotificationResult& result) {
  const bool slackActive = req.slackEnabled;
  const bool telegramActive = req.telegramEnabled;
  const bool whatsappActive = req.whatsappEnabled;

  if (!slackActive && !telegramActive && !whatsappActive) {
    result.ok = false;
    snprintf(result.event, sizeof(result.event), "%s", req.isTest ? "test" : (req.alertState == 2 ? "critical" : req.alertState == 1 ? "warning" : "recovery"));
    snprintf(result.result, sizeof(result.result), "Aucune cible active");
    result.timestampSec = time(nullptr);
    return false;
  }

  if (!WiFi.isConnected()) {
    result.ok = false;
    snprintf(result.event, sizeof(result.event), "%s", req.isTest ? "test" : (req.alertState == 2 ? "critical" : req.alertState == 1 ? "warning" : "recovery"));
    snprintf(result.result, sizeof(result.result), "WiFi indisponible");
    result.timestampSec = time(nullptr);
    return false;
  }

  const String message = buildNotificationMessageStatic(req, req.alertState, req.isTest,
                                                       req.dbInstant, req.leq, req.peak, req.durationMs);

  bool overallOk = true;
  bool attempted = false;
  String summary;

  if (slackActive) {
    String slackResult;
    attempted = true;
    if (!sendSlackNotificationStatic(req, req.alertState, req.isTest, message, slackResult)) overallOk = false;
    if (summary.length()) summary += " | ";
    summary += slackResult;
  }

  if (telegramActive) {
    String telegramResult;
    attempted = true;
    if (!sendTelegramNotificationStatic(req, message, telegramResult)) overallOk = false;
    if (summary.length()) summary += " | ";
    summary += telegramResult;
  }

  if (whatsappActive) {
    String whatsappResult;
    attempted = true;
    if (!sendWhatsappNotificationStatic(req, message, whatsappResult)) overallOk = false;
    if (summary.length()) summary += " | ";
    summary += whatsappResult;
  }

  result.ok = attempted && overallOk;
  snprintf(result.event, sizeof(result.event), "%s", req.isTest ? "test" : (req.alertState == 2 ? "critical" : req.alertState == 1 ? "warning" : "recovery"));
  snprintf(result.result, sizeof(result.result), "%s", summary.length() ? summary.c_str() : "Aucune cible active");
  result.timestampSec = time(nullptr);

  Serial0.printf("[NOTIFY] event=%s result=%s\n", result.event, result.result);
  return result.ok;
}

void NotificationTask::taskFunc(void* parameter) {
  Serial0.println("[NOTIFY] Task started on core 0");

  NotificationRequest req;
  while (true) {
    if (xQueueReceive(_queue, &req, portMAX_DELAY)) {
      Serial0.printf("[NOTIFY] Processing notification (alertState=%u isTest=%d)\n",
                    (unsigned)req.alertState, req.isTest);

      NotificationResult result;
      processNotification(req, result);

      // Store result (thread-safe)
      if (xSemaphoreTake(_resultMutex, pdMS_TO_TICKS(100))) {
        _lastResult = result;
        xSemaphoreGive(_resultMutex);
      }
    }
  }
}

bool NotificationTask::begin() {
  if (_queue) {
    Serial0.println("[NOTIFY] Task already started");
    return true;
  }

  _queue = xQueueCreate(4, sizeof(NotificationRequest));
  if (!_queue) {
    Serial0.println("[NOTIFY] Failed to create queue");
    return false;
  }

  _resultMutex = xSemaphoreCreateMutex();
  if (!_resultMutex) {
    Serial0.println("[NOTIFY] Failed to create mutex");
    vQueueDelete(_queue);
    _queue = nullptr;
    return false;
  }

  BaseType_t result = xTaskCreatePinnedToCore(
    taskFunc,
    "notification",
    8192,  // Stack size (increased for HTTP operations)
    nullptr,
    1,  // Low priority
    &_taskHandle,
    0   // Core 0 (network operations)
  );

  if (result != pdPASS) {
    Serial0.println("[NOTIFY] Failed to create task");
    vQueueDelete(_queue);
    vSemaphoreDelete(_resultMutex);
    _queue = nullptr;
    _resultMutex = nullptr;
    return false;
  }

  Serial0.println("[NOTIFY] Task created successfully");
  return true;
}

void NotificationTask::queueNotification(const NotificationRequest& req) {
  if (!_queue) {
    Serial0.println("[NOTIFY] Queue not initialized");
    return;
  }

  if (xQueueSend(_queue, &req, 0) != pdPASS) {
    Serial0.println("[NOTIFY] Queue full, notification dropped");
  }
}

bool NotificationTask::getLastResult(NotificationResult& out) {
  if (!_resultMutex) return false;

  if (xSemaphoreTake(_resultMutex, pdMS_TO_TICKS(10))) {
    out = _lastResult;
    xSemaphoreGive(_resultMutex);
    return true;
  }

  return false;
}
