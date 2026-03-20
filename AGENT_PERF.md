# Agent Performance - SoundPanel 7

Guide pour l'agent chargé d'analyser et optimiser les performances du firmware embarqué ESP32-S3.

## Votre rôle

Identifier et éliminer les goulots d'étranglement, optimiser l'usage mémoire et CPU, garantir la réactivité du système en toutes circonstances.

## Métriques cibles

### Cibles de performance

**En production (mesure continue)** :
- Heap interne libre : **> 50KB** minimum
- PSRAM libre : **> 500KB** minimum
- CPU idle : **> 60%** (dual-core cumulé)
- LVGL idle : **> 50%** (charge UI acceptable)

**Temps d'exécution critiques** :
- Loop LVGL (`lv_timer_handler`) : **< 30ms** (seuil spike logged)
- Audio `update()` : **< 20ms** (période 80ms)
- SSE broadcast : **< 50ms** (éviter lag interface web)
- MQTT publish : **< 100ms** (non-bloquant)

**Latences réseau** :
- Réponse API `/api/status` : **< 200ms**
- OTA upload : pas de timeout (stable à 115200 bauds)

### Comment mesurer

**API `/api/status`** :
```bash
curl -s http://soundpanel7.local/api/status | jq '{
  heap: .heap,
  psram: .psram,
  cpuLoad: .cpuLoad,
  lvglLoad: .lvglLoad,
  uiWorkMax: .uiWorkMaxUs,
  lvHandlerMax: .lvHandlerMaxUs
}'
```

**Logs série** :
```
[LVGL][SPIKE] page=Principal load=53% ui=0.9ms max=2.5ms handler=75.2ms max=405.8ms obj=82 heap=105k
```

**Monitoring continu** :
```bash
# Script de surveillance (5s interval)
while true; do
  curl -s http://soundpanel7.local/api/status | \
    jq '{t: now, heap: .heap.free, cpu: .cpuLoad}' | \
    tee -a perf_monitor.jsonl
  sleep 5
done
```

## Architecture de performance

### Isolation dual-core

```
┌─────────────────────────────────────────────────┐
│ ESP32-S3 Dual Core - Performance Isolation      │
├─────────────────────┬───────────────────────────┤
│ CORE 0 (loop)       │ CORE 1 (audioTask)        │
├─────────────────────┼───────────────────────────┤
│ • WiFi/NetManager   │ • AudioEngine.update()    │
│ • MQTT client       │ • I2S/ADC acquisition     │
│ • Web server/SSE    │ • dB/RMS/Peak calc        │
│ • UI LVGL (screen)  │ • SharedHistory.update()  │
│ • OTA manager       │                           │
│                     │ Priority: 5 (HIGH)        │
│ Priority: 1         │ Stack: 8KB                │
│ Can block: WiFi/Web │ NEVER blocks             │
└─────────────────────┴───────────────────────────┘
```

**Principe** : Audio = temps réel garanti, tout le reste = best effort.

### Allocations mémoire

**Heap interne (DRAM)** :
- Variables globales statiques
- Stacks FreeRTOS tasks
- WiFi/LWIP buffers
- LVGL objets UI

**PSRAM (SPI RAM)** :
- Framebuffers LCD (2 × ~250KB)
- Gros buffers temporaires
- Cache LVGL

**NVS (Flash)** :
- Settings persistés (SettingsV1)
- WiFi credentials
- Calibration

## Profiling et diagnostics

### 1. Task watermark (stack usage)

**Identifier la consommation stack** :
```cpp
void checkTaskWatermark() {
  UBaseType_t audioStack = uxTaskGetStackHighWaterMark(g_audioTaskHandle);
  UBaseType_t loopStack = uxTaskGetStackHighWaterMark(nullptr);

  Serial0.printf("[TASK] audio_stack_free=%u loop_stack_free=%u\n",
                 audioStack, loopStack);

  if (audioStack < 512) {
    Serial0.println("[TASK][WARN] Audio task stack near overflow!");
  }
}
```

**À appeler périodiquement** (ex: toutes les 60s).

### 2. Heap fragmentation

**Vérifier la fragmentation** :
```cpp
void checkHeapFragmentation() {
  size_t free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

  if (largest < (free / 2)) {
    Serial0.printf("[HEAP][WARN] Fragmentation detected: free=%zu largest=%zu\n",
                   free, largest);
  }
}
```

**Causes fréquentes** :
- `String` en boucle sans `reserve()`
- Allocations/deallocations fréquentes de tailles variables
- Objets LVGL créés/détruits dynamiquement

### 3. Profilage temps d'exécution

**Macro de benchmark** :
```cpp
#define PROFILE_START(name) \
  uint32_t __profile_##name##_start = micros();

#define PROFILE_END(name) \
  uint32_t __profile_##name##_us = micros() - __profile_##name##_start; \
  if (__profile_##name##_us > 5000) { \
    Serial0.printf("[PROFILE] %s: %lu us\n", #name, __profile_##name##_us); \
  }

// Usage
PROFILE_START(mqtt_publish);
g_mqtt.publishState();
PROFILE_END(mqtt_publish);
```

### 4. Détection de fuites mémoire

**Surveillance heap avant/après** :
```cpp
// Avant opération suspecte
size_t heap_before = esp_get_free_heap_size();

// Opération répétée N fois
for (int i = 0; i < 100; i++) {
  suspectedFunction();
}

// Après
size_t heap_after = esp_get_free_heap_size();
size_t leaked = heap_before - heap_after;

if (leaked > 1024) {
  Serial0.printf("[LEAK] Suspected memory leak: %zu bytes\n", leaked);
}
```

## Optimisations prioritaires

### 1. Loop principal (Core 0)

**Objectif** : Garder `loop()` < 30ms

**Points d'attention** :
- `g_wm.process()` peut bloquer → déjà mitigé (non-bloquant)
- `g_mqtt.loop()` appelle `client.loop()` → vérifie timeout interne PubSubClient
- SSE broadcast : limiter fréquence (actuellement throttle à `mqttPublishPeriodMs`)

**Optimisation** :
```cpp
// ❌ Bloquer dans loop
void loop() {
  sendLiveSse();  // peut prendre 50ms si 10 clients
  delay(1);
}

// ✅ Throttle avec période
void loop() {
  static uint32_t lastSse = 0;
  if (millis() - lastSse > 100) {  // Max 10Hz
    sendLiveSse();
    lastSse = millis();
  }
  delay(5);
}
```

### 2. Allocations String

**Problème** : `String` réalloue à chaque concaténation

**Solution** : `reserve()` avant boucle

**Exemple dans SharedHistory** :
```cpp
// ✅ Optimisé (déjà implémenté)
String SharedHistory::toJson() const {
  String json;
  json.reserve((size_t)_count * 8U + 4U);  // Estimation
  json += "[";
  for (uint16_t i = 0; i < _count; i++) {
    if (i) json += ",";
    json += String(valueAt(i), 1);
  }
  json += "]";
  return json;
}
```

**Appliquer systématiquement** dans WebManager (JSON responses).

### 3. LVGL optimisations

**Réduire charge UI** :
- Limiter animations lourdes (fade, slide)
- Cache images dans PSRAM
- `lv_obj_set_style_bg_opa(obj, LV_OPA_COVER)` pour objets opaques
- Éviter `lv_obj_clean()` en boucle (crée/détruit objets)

**Détecter objets orphelins** :
```cpp
// Compter objets LVGL
uint32_t count = 0;
lv_obj_tree_walk(lv_scr_act(), countLvglObjectsCb, &count);

if (count > 150) {
  Serial0.printf("[LVGL][WARN] Object count high: %lu\n", count);
}
```

### 4. Calculs audio

**Points coûteux** :
- `sqrt()` dans RMS
- `log10f()` dans dB conversion
- Boucles ADC (120µs × 1024 samples = 122ms max)

**Optimisations** :
```cpp
// ❌ Lent
float rms = 0.0f;
for (int i = 0; i < count; i++) {
  rms += samples[i] * samples[i];
}
rms = sqrt(rms / count);

// ✅ Précalculer constantes
static constexpr float INV_COUNT = 1.0f / (float)SAMPLE_COUNT;
float rms = sqrtf(sum2 * INV_COUNT);
```

**Limiter samples ADC** :
- Waveshare : 256 samples × 120µs = **30ms** (OK)
- Ne jamais dépasser 512 samples (61ms)

### 5. Réseau et MQTT

**WiFi scan freeze analog ADC** :
- ⚠️ Limitation hardware ESP32-S3
- Mitigation : arrêt scan après 3 échecs en mode AP
- **Solution définitive** : utiliser micro digital (PDM/I2S)

**MQTT reconnexion** :
- Timeout actuel : 5s → OK
- Intervalle retry : 15s (après fix récent) → OK

**Web SSE** :
- Broadcast limité à `mqttPublishPeriodMs` (min 1000ms)
- Max 10 clients simultanés (acceptable)

## Checklist d'optimisation

### Avant de commit une feature

- [ ] Pas de `String` en boucle sans `reserve()`
- [ ] Pas d'allocation dynamique dans loop audio (Core 1)
- [ ] Vérifier heap libre après test (> 50KB)
- [ ] Pas de blocking call dans loop principal
- [ ] LVGL objets cleaned après usage

### Review de performance

- [ ] Profiler la fonction critique (> 5ms logged)
- [ ] Tester avec monitoring heap continu (1h minimum)
- [ ] Vérifier stack watermark tasks
- [ ] Pas de régression sur métriques cibles

### Tests de charge

**Scénarios** :
1. **10 clients web simultanés** : SSE + dashboard
2. **MQTT publish 1Hz pendant 1h** : pas de fuite mémoire
3. **WiFi déconnexion/reconnexion** : audio continue (digital mic)
4. **OTA pendant monitoring audio** : freeze acceptable (documenté)

## Outils recommandés

### ESP-IDF built-in

```cpp
#include <esp_heap_caps.h>
#include <esp_timer.h>

// Heap info détaillée
heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);

// Timer haute résolution
uint64_t start = esp_timer_get_time();
// ... operation ...
uint64_t elapsed = esp_timer_get_time() - start;
```

### PlatformIO monitor

```bash
# Monitor avec filtre logs
pio device monitor -b 115200 --filter colorize --filter time

# Monitor avec log vers fichier
pio device monitor -b 115200 | tee perf_session.log
```

### Scripts d'analyse

**Analyser spikes LVGL** :
```bash
# Extraire les spikes > 50ms
grep "LVGL.*SPIKE" perf_session.log | \
  awk -F'handler=' '{print $2}' | \
  awk '{print $1}' | \
  sort -n | tail -10
```

**Graphique heap over time** :
```python
import json
import matplotlib.pyplot as plt

# Parse logs JSON monitoring
data = [json.loads(line) for line in open('perf_monitor.jsonl')]
times = [d['t'] for d in data]
heap = [d['heap'] / 1024 for d in data]  # KB

plt.plot(times, heap)
plt.xlabel('Time (s)')
plt.ylabel('Free Heap (KB)')
plt.axhline(50, color='r', linestyle='--', label='Min threshold')
plt.legend()
plt.savefig('heap_monitoring.png')
```

## Anti-patterns à éviter

❌ **Optimisation prématurée** : profiler d'abord, optimiser ensuite
❌ **Micro-optimisations** : se concentrer sur les vrais goulots (> 5ms)
❌ **Casser la lisibilité** : préférer la clarté sauf cas critique
❌ **Ignorer les logs de spike** : chaque spike LVGL > 100ms doit être investigué
❌ **Tester seulement sur USB** : valider aussi en condition réelle (WiFi, MQTT actifs)

## Seuils d'alerte

**CRITICAL** (action immédiate) :
- Heap < 30KB
- PSRAM < 200KB
- CPU load > 90% pendant > 10s
- Loop time > 100ms

**WARNING** (à investiguer) :
- Heap < 50KB
- Loop time > 50ms
- Stack watermark < 512 bytes
- Heap largest block < 50% free

**INFO** (monitoring) :
- LVGL spike > 30ms (logged automatiquement)
- MQTT publish > 100ms
- SSE broadcast > 50ms

## Ressources

- ESP32 Performance : https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/performance/index.html
- LVGL Performance : https://docs.lvgl.io/master/overview/performance.html
- FreeRTOS Task monitoring : https://www.freertos.org/a00021.html
- ESP32 Heap tracing : https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/heap_debug.html

## Template de rapport de performance

```markdown
## Performance Report - YYYY-MM-DD

### Test conditions
- Firmware: v0.2.17
- Profile: soundpanel7_usb (Waveshare 7")
- Duration: 2h monitoring
- WiFi: connected, MQTT active

### Metrics
- Heap min: 68KB (target: > 50KB) ✅
- PSRAM min: 890KB (target: > 500KB) ✅
- CPU load avg: 42% (target: < 40%) ⚠️
- LVGL spike max: 105ms (threshold: 100ms) ⚠️

### Bottlenecks identified
1. SSE broadcast with 8 clients: 72ms avg
2. MQTT publish during network congestion: 180ms max
3. LVGL chart update: 18ms (acceptable)

### Optimizations applied
- [ ] Throttle SSE broadcast to 2Hz max
- [ ] Add timeout to MQTT publish (100ms)
- [ ] Profile LVGL chart rendering

### Recommendations
- Monitor heap during 24h test
- Test with 15+ web clients (stress test)
```
