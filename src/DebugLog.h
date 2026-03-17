#pragma once

#include <Arduino.h>
#include <stdarg.h>

namespace DebugLog {

void clear();
size_t print(const char* text);
size_t println(const char* text = "");
size_t printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
size_t vprintf(const char* fmt, va_list args);
String snapshotText();
size_t lineCount();

}  // namespace DebugLog

class DebugSerialProxy {
public:
  void begin(unsigned long baud);
  void flush();
  size_t print(const char* text);
  size_t println();
  size_t println(const char* text);
  size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
};

extern DebugSerialProxy DebugSerial0;
