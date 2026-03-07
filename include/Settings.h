\
#pragma once
#include <stdint.h>
#include <Arduino.h>

struct SettingsV1 {
  // Versioning
  uint16_t schema_version = 1;

  // UI
  uint8_t backlight_percent = 80;
  float threshold_green = 55.0f;
  float threshold_orange = 70.0f;
  float db_min = 30.0f;
  float db_max = 100.0f;

  // Audio
  bool audio_enabled = true;
  bool audio_mock = true;
  float cal_offset_db = 0.0f;
  float cal_gain = 1.0f;         // multiplicative factor on RMS->dB mapping
  float ema_alpha = 0.18f;       // 0..1
  uint16_t leq_window_s = 5;     // 1/5/10/60
  uint16_t peak_window_s = 10;   // seconds

  // WiFi / NTP
  char wifi_ssid[33] = {0};
  char wifi_pass[65] = {0};
  char hostname[33]  = "soundpanel7";
  char ntp_server[65]= "fr.pool.ntp.org";

  // MQTT
  bool mqtt_enabled = false;
  char mqtt_host[65] = "192.168.1.10";
  uint16_t mqtt_port = 1883;
  char mqtt_user[33] = {0};
  char mqtt_pass[65] = {0};
  char mqtt_base_topic[65] = "soundpanel7";
  uint16_t mqtt_interval_s = 5;
  uint8_t mqtt_qos = 0;

  uint32_t crc32 = 0; // optional future use
};
