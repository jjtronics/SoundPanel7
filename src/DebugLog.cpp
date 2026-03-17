#include "DebugLog.h"

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <stdlib.h>
#include <string.h>

namespace {

constexpr size_t kDebugLogMaxLines = 140;
constexpr size_t kDebugLogLineMax = 192;

struct DebugLogEntry {
  uint32_t seq = 0;
  uint32_t ms = 0;
  char text[kDebugLogLineMax] = {0};
};

DebugLogEntry* g_entries = nullptr;
size_t g_entryCount = 0;
size_t g_entryHead = 0;
uint32_t g_nextSeq = 1;
char* g_partial = nullptr;
portMUX_TYPE g_debugLogMux = portMUX_INITIALIZER_UNLOCKED;
DebugLogEntry g_entriesFallback[kDebugLogMaxLines];
char g_partialFallback[kDebugLogLineMax] = {0};

void ensureStorage() {
  if (g_entries && g_partial) return;

  DebugLogEntry* entries = static_cast<DebugLogEntry*>(
    heap_caps_calloc(kDebugLogMaxLines, sizeof(DebugLogEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  char* partial = static_cast<char*>(
    heap_caps_calloc(kDebugLogLineMax, sizeof(char), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (!entries || !partial) {
    if (entries) heap_caps_free(entries);
    if (partial) heap_caps_free(partial);
    entries = g_entriesFallback;
    partial = g_partialFallback;
    memset(entries, 0, sizeof(g_entriesFallback));
    memset(partial, 0, sizeof(g_partialFallback));
  }

  portENTER_CRITICAL(&g_debugLogMux);
  if (!g_entries || !g_partial) {
    g_entries = entries;
    g_partial = partial;
  } else {
    if (entries != g_entriesFallback) heap_caps_free(entries);
    if (partial != g_partialFallback) heap_caps_free(partial);
  }
  portEXIT_CRITICAL(&g_debugLogMux);
}

void commitPartialLocked() {
  if (!g_partial || !g_partial[0]) return;

  DebugLogEntry& entry = g_entries[g_entryHead];
  entry.seq = g_nextSeq++;
  entry.ms = millis();
  strlcpy(entry.text, g_partial, sizeof(entry.text));

  g_entryHead = (g_entryHead + 1) % kDebugLogMaxLines;
  if (g_entryCount < kDebugLogMaxLines) g_entryCount++;
  g_partial[0] = '\0';
}

void appendChunkLocked(const char* text) {
  if (!text || !g_partial) return;

  size_t partialLen = strlen(g_partial);
  for (const char* p = text; *p; ++p) {
    const char c = *p;
    if (c == '\r') continue;
    if (c == '\n') {
      commitPartialLocked();
      partialLen = 0;
      continue;
    }

    if (partialLen >= (kDebugLogLineMax - 1)) {
      commitPartialLocked();
      partialLen = 0;
    }
    g_partial[partialLen++] = c;
    g_partial[partialLen] = '\0';
  }
}

String formatEntry(const DebugLogEntry& entry) {
  char prefix[24];
  const uint32_t seconds = entry.ms / 1000UL;
  const uint32_t millisPart = entry.ms % 1000UL;
  snprintf(prefix, sizeof(prefix), "[%6lu.%03lu] ", (unsigned long)seconds, (unsigned long)millisPart);

  String line;
  line.reserve(strlen(prefix) + strlen(entry.text) + 1);
  line += prefix;
  line += entry.text;
  line += "\n";
  return line;
}

}  // namespace

namespace DebugLog {

void clear() {
  ensureStorage();
  portENTER_CRITICAL(&g_debugLogMux);
  g_entryCount = 0;
  g_entryHead = 0;
  g_nextSeq = 1;
  if (g_partial) g_partial[0] = '\0';
  for (size_t i = 0; i < kDebugLogMaxLines; ++i) {
    g_entries[i].seq = 0;
    g_entries[i].ms = 0;
    g_entries[i].text[0] = '\0';
  }
  portEXIT_CRITICAL(&g_debugLogMux);
}

size_t print(const char* text) {
  ensureStorage();
  if (!text) return 0;
  ::Serial0.print(text);
  portENTER_CRITICAL(&g_debugLogMux);
  appendChunkLocked(text);
  portEXIT_CRITICAL(&g_debugLogMux);
  return strlen(text);
}

size_t println(const char* text) {
  const size_t bytes = print(text ? text : "");
  ::Serial0.println();
  portENTER_CRITICAL(&g_debugLogMux);
  commitPartialLocked();
  portEXIT_CRITICAL(&g_debugLogMux);
  return bytes + 1;
}

size_t printf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const size_t written = vprintf(fmt, args);
  va_end(args);
  return written;
}

size_t vprintf(const char* fmt, va_list args) {
  if (!fmt) return 0;

  char buf[320];
  va_list argsCopy;
  va_copy(argsCopy, args);
  const int written = vsnprintf(buf, sizeof(buf), fmt, argsCopy);
  va_end(argsCopy);
  if (written <= 0) return 0;

  char* heapBuf = nullptr;
  const char* text = buf;
  if ((size_t)written >= sizeof(buf)) {
    heapBuf = static_cast<char*>(malloc((size_t)written + 1U));
    if (heapBuf) {
      va_list argsCopy2;
      va_copy(argsCopy2, args);
      vsnprintf(heapBuf, (size_t)written + 1U, fmt, argsCopy2);
      va_end(argsCopy2);
      text = heapBuf;
    }
  }

  ::Serial0.print(text);
  portENTER_CRITICAL(&g_debugLogMux);
  appendChunkLocked(text);
  portEXIT_CRITICAL(&g_debugLogMux);
  if (heapBuf) free(heapBuf);
  return (size_t)written;
}

String snapshotText() {
  ensureStorage();
  String out;
  DebugLogEntry* snapshot = nullptr;
  size_t count = 0;
  char partial[kDebugLogLineMax] = {0};

  snapshot = static_cast<DebugLogEntry*>(
    malloc(sizeof(DebugLogEntry) * kDebugLogMaxLines));
  if (!snapshot) return out;

  portENTER_CRITICAL(&g_debugLogMux);
  count = g_entryCount;
  const size_t start = (count == kDebugLogMaxLines) ? g_entryHead : 0;
  for (size_t i = 0; i < count; ++i) {
    snapshot[i] = g_entries[(start + i) % kDebugLogMaxLines];
  }
  if (g_partial) strlcpy(partial, g_partial, sizeof(partial));
  portEXIT_CRITICAL(&g_debugLogMux);

  out.reserve(count * 80U + (partial[0] ? strlen(partial) + 24U : 0U));
  for (size_t i = 0; i < count; ++i) {
    out += formatEntry(snapshot[i]);
  }
  if (partial[0]) {
    char prefix[24];
    const uint32_t nowMs = millis();
    snprintf(prefix, sizeof(prefix), "[%6lu.%03lu] ", (unsigned long)(nowMs / 1000UL), (unsigned long)(nowMs % 1000UL));
    out += prefix;
    out += partial;
  }

  free(snapshot);

  return out;
}

size_t lineCount() {
  ensureStorage();
  portENTER_CRITICAL(&g_debugLogMux);
  const size_t count = g_entryCount;
  portEXIT_CRITICAL(&g_debugLogMux);
  return count;
}

}  // namespace DebugLog

DebugSerialProxy DebugSerial0;

void DebugSerialProxy::begin(unsigned long baud) {
  ::Serial0.begin(baud);
}

void DebugSerialProxy::flush() {
  ::Serial0.flush();
}

size_t DebugSerialProxy::print(const char* text) {
  return DebugLog::print(text);
}

size_t DebugSerialProxy::println() {
  return DebugLog::println("");
}

size_t DebugSerialProxy::println(const char* text) {
  return DebugLog::println(text);
}

size_t DebugSerialProxy::printf(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  const size_t written = DebugLog::vprintf(fmt, args);
  va_end(args);
  return written;
}
