# Analyse de la réactivité tactile

## Métriques actuelles
```
LVGL load / idle: 38% / 62%
UI work: 0.9 ms (max 84.8 ms)
Handler: 0.0 ms (max 464.3 ms) ⚠️
```

## Problèmes identifiés

### 🔴 Critique: Handler bloquant 464ms
**Impact:** Le touch handler est bloqué jusqu'à 464ms (presque une demi-seconde!)
**Cause:** Opérations synchrones bloquantes dans le loop principal:
- [WebManager.cpp:812](src/WebManager.cpp#L812) `processPendingNotification()`
  - Fait des requêtes HTTP synchrones vers Slack/Telegram/WhatsApp
  - Peut prendre 200-500ms par requête
  - **Bloque le thread principal** avant que le touch handler ne soit appelé
- [WebManager.cpp:815](src/WebManager.cpp#L815) `_srv.handleClient()`
  - Traite les requêtes HTTP entrantes de manière synchrone
- [NetManager](src/NetManager.cpp) `g_net.loop()`
  - Opérations WiFi potentiellement bloquantes
- [ReleaseUpdateManager](src/ReleaseUpdateManager.cpp) `g_releaseUpdate.loop()`
  - Vérifications de mise à jour HTTP

**Ordre d'exécution problématique:**
```cpp
void loop() {
  g_net.loop();              // 1. Peut bloquer (WiFi)
  g_releaseUpdate.loop();    // 2. Peut bloquer (HTTP)
  g_mqtt.loop();             // 3. Peut bloquer (MQTT)
  g_web.loop();              // 4. Peut bloquer (HTTP + notifications)
  // ... UI update ...
  lv_timer_handler();        // 5. ENFIN le touch est traité ici !
  delay(5);                  // 6. Delay additionnel
}
```

Si `g_web.loop()` envoie une notification Slack qui prend 300ms, le touch handler attend 300ms avant d'être appelé.

### 🟠 Important: Période de refresh trop lente
**Fichier:** [lv_conf.h:64-65](src/lv_conf.h#L64)
```c
#define LV_DISP_DEF_REFR_PERIOD 30    // 30ms = 33 FPS
#define LV_INDEV_DEF_READ_PERIOD 30   // Touch lu toutes les 30ms
```

**Comparaison:**
- **Actuel:** 30ms = 33 FPS
- **Smartphone 60Hz:** 16ms = 60 FPS
- **Smartphone 120Hz:** 8ms = 120 FPS

**Impact:** Latence perçue de 30-60ms entre le touch et la réponse visuelle

### 🟡 Modéré: Delay dans le loop
**Fichier:** [main.cpp:380](src/main.cpp#L380)
```cpp
delay(5);  // Ajoute 5ms de latence minimum
```

**Impact:** Ajoute 5ms de délai fixe à chaque itération du loop

### 🟡 Modéré: UI work max 84ms
**Impact:** Spike LVGL de 84ms indique des opérations UI trop lourdes
**Cause possible:**
- Trop d'objets LVGL à redessiner
- Animations complexes
- Buffer LVGL insuffisant

## Solutions proposées

### ✅ Solution 1: Réduire les périodes LVGL (facile, gain immédiat)

**Fichier:** [lv_conf.h](src/lv_conf.h#L64)
```c
// AVANT
#define LV_DISP_DEF_REFR_PERIOD 30
#define LV_INDEV_DEF_READ_PERIOD 30

// APRÈS (60 FPS)
#define LV_DISP_DEF_REFR_PERIOD 16
#define LV_INDEV_DEF_READ_PERIOD 10
```

**Gain estimé:**
- Latence touch: 30ms → 10ms (**3x plus rapide**)
- Fluidité visuelle: 33 FPS → 60 FPS

**Coût:**
- Charge CPU: +5-10% (de 11% à ~16-21%)
- ⚠️ Vérifier que le CPU peut tenir (actuellement 11% donc marge OK)

### ✅ Solution 2: Réduire delay loop (facile)

**Fichier:** [main.cpp:380](src/main.cpp#L380)
```cpp
// AVANT
delay(5);

// APRÈS
delay(1);  // Minimum pour yield au scheduler FreeRTOS
```

**Gain estimé:** Réduit latence loop de 5ms → 1ms

### ✅ Solution 3: Déplacer notifications en tâche asynchrone (moyen, gain majeur)

**Problème:** `processPendingNotification()` bloque le loop principal jusqu'à 464ms

**Solution:** Créer une tâche FreeRTOS dédiée pour les notifications

**Nouveau fichier:** `src/NotificationTask.cpp`
```cpp
static QueueHandle_t g_notificationQueue = nullptr;
static TaskHandle_t g_notificationTaskHandle = nullptr;

struct NotificationRequest {
  uint8_t alertState;
  bool isTest;
  float dbInstant;
  float leq;
  float peak;
  uint32_t durationMs;
};

void notificationTask(void* parameter) {
  NotificationRequest req;
  while (true) {
    if (xQueueReceive(g_notificationQueue, &req, portMAX_DELAY)) {
      // Ici on peut bloquer sans gêner le loop principal
      WebManager::dispatchNotificationBlocking(req);
    }
  }
}

void startNotificationTask() {
  g_notificationQueue = xQueueCreate(4, sizeof(NotificationRequest));
  xTaskCreatePinnedToCore(
    notificationTask,
    "notification",
    4096,
    nullptr,
    1,  // Priorité basse
    &g_notificationTaskHandle,
    0   // Core 0 (avec réseau)
  );
}

void queueNotification(NotificationRequest req) {
  xQueueSend(g_notificationQueue, &req, 0);  // Non-bloquant
}
```

**Fichier:** [WebManager.cpp:883](src/WebManager.cpp#L883)
```cpp
// AVANT
void WebManager::processPendingNotification() {
  if (!_notificationPending) return;
  // ... setup ...
  dispatchNotification(...);  // BLOQUE jusqu'à 464ms !
}

// APRÈS
void WebManager::processPendingNotification() {
  if (!_notificationPending) return;
  // ... setup ...
  NotificationRequest req = {
    .alertState = alertState,
    .isTest = isTest,
    // ... autres champs ...
  };
  queueNotification(req);  // Non-bloquant, retourne immédiatement
}
```

**Gain estimé:** Handler max 464ms → <10ms (**46x plus rapide !**)

### ✅ Solution 4: Optimiser ordre du loop (facile)

**Fichier:** [main.cpp:311](src/main.cpp#L311)
```cpp
// AVANT (touch traité en DERNIER)
void loop() {
  g_net.loop();              // Peut bloquer
  g_releaseUpdate.loop();    // Peut bloquer
  g_mqtt.loop();             // Peut bloquer
  g_web.loop();              // Peut bloquer (notifications)
  // ... UI update ...
  lv_timer_handler();        // Touch traité ICI
  delay(5);
}

// APRÈS (touch traité en PREMIER)
void loop() {
  // 1. PRIORITÉ: traiter le touch et l'UI
  #if SOUNDPANEL7_HAS_SCREEN
  uint32_t lvHandlerStartUs = micros();
  lv_timer_handler();  // Touch traité IMMÉDIATEMENT
  g_runtimeStats.lvHandlerLastUs = micros() - lvHandlerStartUs;
  // ... stats ...
  #endif

  // 2. UI update
  uint32_t uiStartUs = micros();
  #if SOUNDPANEL7_HAS_SCREEN
  lvgl_port_lock(-1);
  #endif
  g_ui.tick();
  g_ui.setDb(m.dbInstant, m.leq, m.peak);
  sampleRuntimeStats();
  #if SOUNDPANEL7_HAS_SCREEN
  lvgl_port_unlock();
  #endif
  g_runtimeStats.uiWorkLastUs = micros() - uiStartUs;
  // ... stats ...

  // 3. Réseau (peut bloquer mais après UI)
  g_net.loop();
  g_releaseUpdate.loop();
  g_mqtt.loop();
  g_web.loop();

  g_web.updateMetrics(m.dbInstant, m.leq, m.peak);
  g_mqtt.updateMetrics(m.dbInstant, m.leq, m.peak);

  delay(1);
}
```

**Gain estimé:** Touch traité sans attendre les opérations réseau

### ⚠️ Solution 5: Mode turbo conditionnel (avancé)

**Idée:** Détecter quand l'utilisateur touche l'écran et activer temporairement un mode "turbo"

```cpp
static bool g_touchActive = false;
static uint32_t g_lastTouchMs = 0;

void loop() {
  // Détection touch actif
  if (/* touch détecté */) {
    g_touchActive = true;
    g_lastTouchMs = millis();
  }

  // Désactiver turbo 500ms après dernier touch
  if (g_touchActive && (millis() - g_lastTouchMs > 500)) {
    g_touchActive = false;
  }

  // En mode turbo: skip les opérations lourdes
  if (!g_touchActive) {
    g_net.loop();
    g_releaseUpdate.loop();
    g_web.loop();  // Notifications seulement si pas de touch
  }

  // Toujours traiter
  g_mqtt.loop();  // Léger
  // ... UI ...
  lv_timer_handler();

  delay(g_touchActive ? 1 : 5);  // Delay réduit pendant touch
}
```

**Gain estimé:** Réactivité maximale pendant interaction, économie CPU au repos

## Plan d'implémentation recommandé

### Phase 1: Quick wins (gains immédiats, 10 min)
1. ✅ Réduire `LV_DISP_DEF_REFR_PERIOD` de 30 à 16
2. ✅ Réduire `LV_INDEV_DEF_READ_PERIOD` de 30 à 10
3. ✅ Réduire `delay(5)` à `delay(1)` dans loop

**Gain attendu:** Latence touch 30-60ms → 10-20ms

### Phase 2: Réorganiser loop (gain moyen, 20 min)
4. ✅ Inverser ordre: traiter touch/UI en premier, réseau après

**Gain attendu:** Touch prioritaire, pas d'attente réseau

### Phase 3: Tâche notification async (gain majeur, 1-2h)
5. ⚙️ Créer tâche FreeRTOS pour notifications
6. ⚙️ Queue non-bloquante pour dispatch
7. ⚙️ Modifier `processPendingNotification()` pour utiliser queue

**Gain attendu:** Handler max 464ms → <10ms

### Phase 4: Mode turbo (optionnel, gain situationnel, 1h)
8. 🔮 Implémenter détection touch actif
9. 🔮 Skip opérations lourdes pendant touch

## Métriques cibles

### Avant optimisation
- Touch polling: **30ms** (33 Hz)
- Display refresh: **30ms** (33 FPS)
- Handler max: **464ms** ⚠️
- Loop delay: **5ms**
- Latence perçue: **50-500ms** (inacceptable)

### Après Phase 1 (quick wins)
- Touch polling: **10ms** (100 Hz) ✅
- Display refresh: **16ms** (60 FPS) ✅
- Handler max: 464ms (pas changé)
- Loop delay: **1ms** ✅
- Latence perçue: **20-470ms** (mieux mais handler still bloque)

### Après Phase 2 (réorganisation)
- Touch polling: 10ms ✅
- Display refresh: 16ms ✅
- Handler max: 464ms (mais traité avant réseau) ✅
- Loop delay: 1ms ✅
- Latence perçue: **15-30ms** (acceptable pour UI touch basique)

### Après Phase 3 (async notifications)
- Touch polling: 10ms ✅
- Display refresh: 16ms ✅
- Handler max: **<10ms** ✅✅✅
- Loop delay: 1ms ✅
- Latence perçue: **10-20ms** (fluidité smartphone) 🎯

## Comparaison objectifs

| Métrique | Actuel | Cible smartphone | Après Phase 3 |
|----------|--------|------------------|---------------|
| Touch latency | 30ms | 8-16ms | **10ms** ✅ |
| Display refresh | 33 FPS | 60-120 FPS | **60 FPS** ✅ |
| Handler blocking | 464ms | <16ms | **<10ms** ✅ |
| Latence perçue | 50-500ms | 10-30ms | **10-20ms** ✅ |

## Risques et validations

### Risque: Augmentation charge CPU
**Phase 1:** Refresh plus rapide (30ms → 16ms)
- **Avant:** LVGL load 38%, CPU 11%
- **Estimé:** LVGL load 50-55%, CPU 16-21%
- **Validation:** Monitorer charge CPU et température MCU

### Risque: LVGL buffer overflow
**Phase 1:** Refresh plus rapide peut causer tearing ou buffer overflow
- **Validation:** Tester sur toutes les pages (Overview, Clock, Live, Sound)
- **Solution si problème:** Augmenter taille buffer LVGL dans lv_conf.h

### Risque: Notifications perdues
**Phase 3:** Queue saturée si trop de notifications
- **Mitigation:** Queue de 4 éléments (suffisant pour burst)
- **Validation:** Tester burst de notifications

### Risque: Race condition
**Phase 3:** Accès concurrent aux settings
- **Mitigation:** Copier settings dans NotificationRequest (pas de pointeur)
- **Validation:** Test stress avec changements settings pendant notif

## Conclusion

**Problème principal:** Handler bloqué jusqu'à 464ms par notifications HTTP synchrones

**Solution rapide (10 min):** Phase 1 réduit latence perçue de 30ms → 10-20ms

**Solution complète (2-3h):** Phase 1+2+3 atteint fluidité smartphone (<20ms latency)

**Recommandation:** Commencer par Phase 1 (gains immédiats sans risque), puis Phase 2, puis Phase 3 si nécessaire.
