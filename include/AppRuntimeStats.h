#pragma once

#include <Arduino.h>

struct RuntimeStats {
  uint8_t cpuIdlePct = 100;
  uint8_t cpuLoadPct = 0;
  uint8_t lvglIdlePct = 100;
  uint8_t lvglLoadPct = 0;
  uint32_t uiWorkLastUs = 0;
  uint32_t uiWorkMaxUs = 0;
  uint32_t lvHandlerLastUs = 0;
  uint32_t lvHandlerMaxUs = 0;
  uint32_t lvObjCount = 0;
  uint32_t heapInternalFree = 0;
  uint32_t heapInternalTotal = 0;
  uint32_t heapInternalMin = 0;
  uint32_t heapPsramFree = 0;
  uint32_t heapPsramTotal = 0;
  uint32_t heapPsramMin = 0;
  char activePage[24] = "Principal";
};

extern RuntimeStats g_runtimeStats;
