#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

class WebManager;

// Structure pour transmettre les données de notification de manière thread-safe
struct NotificationRequest {
  uint8_t alertState;
  bool isTest;
  float dbInstant;
  float leq;
  float peak;
  uint32_t durationMs;
  bool updateAlertTracking;

  // Copie des settings nécessaires pour éviter accès concurrent
  bool slackEnabled;
  char slackWebhookUrl[193];
  char slackChannel[81];

  bool telegramEnabled;
  char telegramBotToken[97];
  char telegramChatId[49];

  bool whatsappEnabled;
  char whatsappAccessToken[193];
  char whatsappPhoneNumberId[33];
  char whatsappRecipient[33];
  char whatsappApiVersion[13];

  char hostname[32];
  uint8_t greenMax;
  uint8_t orangeMax;
};

// Structure pour retourner le résultat (optionnel, pour logs)
struct NotificationResult {
  bool ok;
  uint32_t timestampSec;
  char event[16];
  char result[128];
};

class NotificationTask {
public:
  static bool begin();
  static void queueNotification(const NotificationRequest& req);
  static bool getLastResult(NotificationResult& out);

private:
  static void taskFunc(void* parameter);
  static bool processNotification(const NotificationRequest& req, NotificationResult& result);

  static QueueHandle_t _queue;
  static TaskHandle_t _taskHandle;
  static NotificationResult _lastResult;
  static SemaphoreHandle_t _resultMutex;
};
