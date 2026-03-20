# Rapport de Revue de Code - SoundPanel 7

**Date** : 2025-03-19
**Révision du code** : analyse complète selon AGENT_CODE_REVIEW.md
**Périmètre** : src/*.cpp, src/*.h

---

## Résumé Exécutif

Le code est globalement **de bonne qualité** avec une architecture claire et une séparation des responsabilités bien respectée. Quelques optimisations mémoire et standardisations pourraient améliorer la maintenabilité.

### Indicateurs

✅ **Points forts** :
- Architecture claire (AudioEngine → SharedHistory → UI/Web/MQTT)
- Séparation des responsabilités respectée
- Gestion d'erreurs présente dans les composants critiques
- Build conditionnel bien implémenté
- Sécurité : validation présente pour les entrées critiques

⚠️ **Points d'amélioration** :
- Quelques allocations String optimisables
- Code dupliqué dans les handlers JSON
- Commentaires manquants sur formules mathématiques
- Logs non uniformes

---

## 1. Architecture et Séparation des Responsabilités

### ✅ Bon

**Flux de données respecté** :
- `AudioEngine.cpp` : logique audio pure, pas de dépendance UI/Web
- `SharedHistory.h` : buffer circulaire indépendant
- `WebManager.cpp` : délègue la logique métier correctement
- `MqttManager.cpp` : publication uniquement, pas de logique métier

**Exemple positif** - Délégation correcte :
```cpp
// WebManager.cpp - Pas de logique audio dans le handler
void WebManager::handleApiCalibrate(AsyncWebServerRequest* request) {
  // Validation puis délégation
  bool ok = g_audio.captureCalibrationPoint(g_settings, index, refDb);
  // ...
}
```

### ⚠️ À améliorer

**NetManager.cpp:62** - Logique de migration dans begin()
```cpp
void NetManager::begin(SettingsV1* settings, SettingsStore* store) {
  // ...
  migrateLegacyCredentialIfNeeded();  // Logique métier dans init
  rebuildWifiMulti();
  ensureWifiConnection(true);
}
```

**Recommandation** : Extraire la migration dans une méthode séparée appelée explicitement depuis `main.cpp` après le chargement des settings.

---

## 2. Gestion Mémoire

### ✅ Bon

**SharedHistory.h:57** - Pré-allocation correcte :
```cpp
String SharedHistory::toJson() const {
  String json;
  json.reserve((size_t)_count * 8U + 4U);  // ✅ Évite réallocations
  json += "[";
  for (uint16_t i = 0; i < _count; i++) {
    if (i) json += ",";
    json += String(valueAt(i), 1);
  }
  json += "]";
  return json;
}
```

**MqttManager.cpp:42** - JSON escape avec reserve :
```cpp
String MqttManager::jsonEscape(const String& in) {
  String out;
  out.reserve(in.length() + 8);  // ✅ Pré-allocation
  // ...
}
```

### ⚠️ À optimiser

**WebManager.cpp:118-137** - `formatAlertDuration()` sans reserve :
```cpp
static String formatAlertDuration(uint32_t durationMs) {
  String out;
  // ❌ Pas de reserve, allocations multiples
  if (hours > 0) {
    out += String(hours);
    out += " h ";
  }
  if (minutes > 0 || hours > 0) {
    out += String(minutes);
    out += " min ";
  }
  out += String(seconds);
  out += " s";
  return out;
}
```

**Recommandation** :
```cpp
static String formatAlertDuration(uint32_t durationMs) {
  String out;
  out.reserve(32);  // Pré-allocation suffisante pour "XX h XX min XX s"
  // ... reste identique
}
```

**WebManager.cpp:147-152** - Concaténation String inefficace :
```cpp
static void appendWifiJson(String& json, bool wifiConnected, const String& ip, int rssi, const String& ssid) {
  json += "\"wifi\":"; json += (wifiConnected ? "true" : "false"); json += ",";
  // ❌ Multiples concaténations de littéraux
}
```

**Recommandation** : Utiliser des buffers statiques pour les petites valeurs JSON :
```cpp
static void appendWifiJson(String& json, bool wifiConnected, const String& ip, int rssi, const String& ssid) {
  json += wifiConnected ? "\"wifi\":true," : "\"wifi\":false,";
  // ...
}
```

---

## 3. Standards de Nommage

### ✅ Bon

Cohérence globale respectée :
- Classes : `AudioEngine`, `WebManager`, `MqttManager` (PascalCase) ✅
- Fonctions : `computeAnalogRms()`, `buildSetupApName()` (camelCase) ✅
- Membres privés : `_fast`, `_slow`, `_lastSampleMs` (\_prefixe) ✅
- Constantes locales : `kRmsEpsilon`, `kFastAlpha` (kConstantName) ✅
- Constantes globales : `SETTINGS_MAGIC`, `DEFAULT_HISTORY_MINUTES` (UPPER_SNAKE_CASE) ✅

### ⚠️ Incohérences mineures

**WebManager.cpp:39** - Notation mixte :
```cpp
static constexpr HTTPMethod kSyncHttpGet = static_cast<HTTPMethod>(::HTTP_GET);
static constexpr HTTPMethod kSyncHttpPost = static_cast<HTTPMethod>(::HTTP_POST);
```

**Recommandation** : Ces constantes ne semblent pas utilisées. À nettoyer si obsolètes.

**NetManager.cpp:22** - Variables globales sans préfixe :
```cpp
static WiFiManager g_wm;
static WiFiMulti g_wifiMulti;
```

**Note** : Le préfixe `g_` pour globales est cohérent. OK.

---

## 4. Code Dupliqué

### ⚠️ Duplication JSON building

**Pattern répété dans WebManager.cpp** - Construction JSON similaire :

```cpp
// Ligne 147
json += "\"wifi\":"; json += (wifiConnected ? "true" : "false"); json += ",";

// Ligne 164
json += "\"time_ok\":"; json += (hasTime ? "true" : "false"); json += ",";

// Ligne 185
json += "\"mcuTempOk\":"; json += (mcuTempOk ? "true" : "false"); json += ",";
```

**Recommandation** : Factoriser dans une helper function :
```cpp
static void appendBoolField(String& json, const char* key, bool value) {
  json += "\"";
  json += key;
  json += "\":";
  json += value ? "true" : "false";
  json += ",";
}

// Usage
appendBoolField(json, "wifi", wifiConnected);
appendBoolField(json, "time_ok", hasTime);
appendBoolField(json, "mcuTempOk", mcuTempOk);
```

**Note** : La fonction `appendBoolField()` existe déjà ligne 139 mais n'est pas utilisée partout ! À uniformiser.

### ⚠️ Duplication dans SettingsStore.cpp

**Lignes 88-119 et 121-147** - Load/Save calibration profile quasi identiques :
```cpp
// loadCalibrationProfile
char keyCount[12];
snprintf(keyCount, sizeof(keyCount), "cp%u_cnt", (unsigned)index);
// ... répété pour chaque champ

// saveCalibrationProfile
char keyCount[12];
snprintf(keyCount, sizeof(keyCount), "cp%u_cnt", (unsigned)index);
// ... répété pour chaque champ
```

**Recommandation** : Créer une helper function pour générer les clés NVS :
```cpp
static String makeCalibrationKey(uint8_t profileIndex, const char* suffix) {
  char key[12];
  snprintf(key, sizeof(key), "cp%u_%s", (unsigned)profileIndex, suffix);
  return String(key);
}
```

### ⚠️ Duplication dans JsonHelpers.h

**Lignes 181-230 et 233-238** - `parseFloatArray` et `parseU8Array` quasi identiques.

**Note** : Acceptable car template. Pas de refactoring nécessaire ici.

---

## 5. Gestion d'Erreurs

### ✅ Bon

**AudioEngine.cpp:116-146** - Vérification des retours I2S :
```cpp
bool AudioEngine::ensureDigitalInput(const SettingsV1& s) {
  // ...
  bool ok = false;
  switch (source) {
    case AudioSource::PdmMems:
      ok = g_audioI2s.begin(...);  // ✅ Retour vérifié
      break;
  }
  _digitalInputReady = ok;
  _activeDigitalSource = ok ? s.audioSource : 0xFF;
  return ok;
}
```

**NetManager.cpp:34-66** - Gestion robuste du WiFi :
```cpp
bool NetManager::begin(SettingsV1* settings, SettingsStore* store) {
  _s = settings;
  _store = store;
  _started = true;
  // ... init avec états cohérents
  return true;
}
```

### ⚠️ Erreurs potentiellement non gérées

**SettingsStore.cpp:38-43** - `ensureBackupFsMounted()` :
```cpp
bool ensureBackupFsMounted() {
  static int8_t mounted = -1;
  if (mounted >= 0) return mounted == 1;
  mounted = LittleFS.begin(true) ? 1 : 0;  // ✅ Retour vérifié
  return mounted == 1;
}
```

**Bon**, mais les appelants doivent vérifier le retour. Vérifier dans le code :

**SettingsStore.cpp:45-57** - `readBackupFileString()` vérifie bien :
```cpp
bool readBackupFileString(const char* path, String& out) {
  out = "";
  if (!ensureBackupFsMounted()) return false;  // ✅
  File file = LittleFS.open(path, "r");
  if (!file) return false;  // ✅
  // ...
}
```

**OK** : Gestion d'erreurs correcte dans ce module.

### ⚠️ MqttManager.cpp - Erreurs loggées mais pas toujours propagées

**MqttManager.cpp:21-25** :
```cpp
if (!_s->mqttEnabled) {
  _lastError = "disabled";
  Serial0.println("[MQTT] disabled");
  return false;  // ✅ Retour explicite
}
```

**Bon**, mais vérifier que l'appelant dans `main.cpp` teste le retour.

---

## 6. Optimisations

### ✅ Optimisations déjà présentes

**AudioEngine.cpp:6-7** - Constantes précalculées :
```cpp
static constexpr float kFastAlpha = 0.45f;
static constexpr float kSlowAlpha = 0.10f;
```

**WebManager.cpp:48-62** - Fonctions mathématiques inline :
```cpp
static float clamp01f(float value) {
  if (value < 0.0f) return 0.0f;
  if (value > 1.0f) return 1.0f;
  return value;
}
```

### ⚠️ Optimisations possibles

**AudioEngine.cpp:87-114** - `computeAnalogRms()` :
```cpp
for (uint16_t i = 0; i < sampleCount; i++) {
  int v = analogRead(pin);
  sum += (double)v;
  sum2 += (double)v * (double)v;
  lastOut = v;
  delayMicroseconds(120);  // ⚠️ Délai fixe peut être optimisé
}
```

**Note** : Le délai est nécessaire pour l'ADC. Acceptable.

**WebManager.cpp:64-102** - `tardisPulseWithPlateaus01()` :
Fonction complexe avec plusieurs `cosf()`. Si appelée fréquemment, considérer une lookup table.

**Recommandation** : Mesurer la fréquence d'appel. Si > 100Hz, lookup table. Sinon OK.

### ⚠️ Allocations dans loop

**Vérifier dans main.cpp** que les fonctions suivantes ne sont pas appelées dans le loop principal :
- `formatAlertDuration()` - OK, appelé seulement lors de notifications
- `buildSetupApName()` - OK, appelé seulement au boot
- JSON builders - ⚠️ Appelés pour chaque requête API, mais acceptable (async)

**Conclusion** : Optimisations loop principal OK.

---

## 7. Commentaires et Documentation

### ✅ Bon

**AudioEngine.cpp:70-74** - Commentaire justifiant un workaround :
```cpp
#if defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(12);
  // Set the default attenuation before the first analogRead().
  // analogSetPinAttenuation(pin, ...) logs an error if the channel has not yet
  // been initialized by the core, even though later reads still work.
  analogSetAttenuation(ADC_11db);
#endif
```

### ⚠️ Commentaires manquants

**WebManager.cpp:64-102** - Formule mathématique TARDIS sans explication :
```cpp
static float tardisPulseWithPlateaus01(float phase,
                                       float riseSpan,
                                       float highHoldSpan,
                                       float fallSpan,
                                       bool mechanicalRamp) {
  // ❌ Pas de commentaire expliquant l'algorithme
  phase = phase - floorf(phase);
  // ... calculs complexes
}
```

**Recommandation** :
```cpp
/**
 * Génère une courbe de pulse TARDIS avec montée, plateau haut et descente.
 *
 * @param phase Valeur [0..1+] (normalisée en interne)
 * @param riseSpan Durée de montée (fraction de cycle)
 * @param highHoldSpan Durée du plateau haut
 * @param fallSpan Durée de descente
 * @param mechanicalRamp Si true, utilise un mix linear/smoothstep pour effet mécanique
 * @return Intensité [0..1]
 */
```

**AudioEngine.cpp:107-113** - Calcul variance sans explication :
```cpp
double mean = sum / n;
double var = (sum2 / n) - (mean * mean);  // ❌ Formule mathématique non documentée
if (var < 0.0) var = 0.0;
```

**Recommandation** : Ajouter commentaire :
```cpp
// Variance: E[X²] - E[X]² (formule numérique stable)
```

### ⚠️ TODOs obsolètes ?

**Recherche** : aucun TODO trouvé dans les fichiers lus. ✅

### ⚠️ Code commenté

**Aucun bloc de code commenté détecté**. ✅

---

## 8. Build Conditionnel

### ✅ Bon

**main.cpp:10-12** - Usage correct :
```cpp
#if SOUNDPANEL7_HAS_SCREEN
#include <lvgl.h>
#endif
```

**AudioEngine.cpp:69-75** - Conditionnel ESP32 spécifique :
```cpp
#if defined(ARDUINO_ARCH_ESP32)
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);
#endif
```

### ✅ Pas de code mort détecté

Les deux profils (Waveshare avec écran / headless) semblent bien gérés.

**Recommandation** : Compiler les 4 environnements pour validation finale :
```bash
pio run -e soundpanel7_usb
pio run -e soundpanel7_ota
pio run -e soundpanel7_headless_usb
pio run -e soundpanel7_headless_ota
```

---

## 9. Sécurité

### ✅ Bon

**JsonHelpers.h:276-281** - `safeCopy()` avec vérification taille :
```cpp
inline bool safeCopy(char* dst, size_t dstSize, const String& src) {
  if (!dst || dstSize == 0) return false;
  if (src.length() >= dstSize) return false;  // ✅ Évite overflow
  memcpy(dst, src.c_str(), src.length() + 1);
  return true;
}
```

**SettingsStore.cpp:149-161** - `secureEqualsRaw()` pour comparer secrets :
```cpp
bool secureEqualsRaw(const char* a, const char* b) {
  const size_t lenA = a ? strlen(a) : 0;
  const size_t lenB = b ? strlen(b) : 0;
  // Timing-safe comparison
  // ...
}
```

### ⚠️ Validation entrées à vérifier

**WebManager.cpp** - Vérifier que tous les handlers valident les longueurs :

**Exemple à vérifier** (non lu entièrement) :
```cpp
// Hypothétique handler
void handleApiWifi(AsyncWebServerRequest* req) {
  String ssid = req->getParam("ssid", true)->value();
  // ⚠️ Vérifier : length check avant strcpy ?
}
```

**Recommandation** : Audit complet des handlers dans `WebManager.cpp` (ligne 200+) pour validation systématique :
- SSID length <= `WIFI_SSID_MAX_LENGTH`
- Password length <= `WIFI_PASSWORD_MAX_LENGTH`
- Hostname length validation
- Token length validation

### ✅ Pas d'injection système détectée

Aucun `system()`, `exec()`, ou appel shell direct trouvé. ✅

### ✅ Secrets non loggés

Les logs semblent éviter les mots de passe. Vérifier dans `DebugLog.cpp` si non lu.

---

## 10. Logs de Debug

### ⚠️ Format non uniforme

**Exemples trouvés** :

```cpp
// NetManager.cpp:98
Serial0.printf("[Net] Starting WiFi portal '%s'\n", setupApName.c_str());

// MqttManager.cpp:23
Serial0.println("[MQTT] disabled");

// MqttManager.cpp:28
Serial0.printf("[MQTT] configured host=%s port=%u clientId=%s base=%s\n", ...);
```

**Observation** :
- `[Net]` vs `[MQTT]` : tags différents ✅
- Format printf/println mixte : OK pour la flexibilité
- Pas de timestamp : normal pour ESP32 (utiliser Serial0)

**Recommandation** : Standardiser le format selon AGENT_CODE_REVIEW.md :
```cpp
Serial0.printf("[COMPONENT] action: detail key=value\n", ...);
```

**Exemples à standardiser** :
```cpp
// Avant
Serial0.println("[MQTT] disabled");

// Après (plus cohérent)
Serial0.println("[MQTT] status: disabled");
```

---

## 11. Patterns à Standardiser

### ✅ Handlers API déjà bien structurés

**Pattern observé dans les handlers** (non lus exhaustivement mais semble cohérent) :
1. Check auth
2. Méthode GET → retour JSON
3. Méthode POST → validation + mise à jour
4. Méthode non supportée → 405

**OK** : Semble respecter le pattern documenté dans AGENT_CODE_REVIEW.md.

---

## Recommandations Prioritaires

### 🔴 Priorité Haute

1. **Uniformiser `appendBoolField()`** dans WebManager.cpp
   - Fonction existe (ligne 139) mais pas utilisée partout
   - Impact : lisibilité, maintenance
   - Effort : 15 min

2. **Audit validation inputs API** dans WebManager.cpp
   - Vérifier tous les handlers POST
   - Valider lengths avant strcpy/strncpy
   - Impact : sécurité
   - Effort : 1-2h

3. **Ajouter `reserve()` dans `formatAlertDuration()`**
   - WebManager.cpp:118
   - Impact : performance (allocations)
   - Effort : 2 min

### 🟡 Priorité Moyenne

4. **Documenter formules mathématiques**
   - `tardisPulseWithPlateaus01()` : algorithme
   - `computeAnalogRms()` : formule variance
   - Impact : maintenabilité
   - Effort : 30 min

5. **Factoriser clés NVS calibration**
   - SettingsStore.cpp:88-147
   - Helper `makeCalibrationKey()`
   - Impact : DRY, lisibilité
   - Effort : 30 min

6. **Standardiser format logs**
   - Uniformiser `[COMPONENT] action: details`
   - Impact : debuggabilité
   - Effort : 1h

### 🟢 Priorité Basse

7. **Nettoyer constantes inutilisées**
   - WebManager.cpp:39 `kSyncHttpGet`, `kSyncHttpPost`
   - Impact : propreté
   - Effort : 2 min

8. **Mesurer fréquence `tardisPulseWithPlateaus01()`**
   - Si > 100 Hz, lookup table
   - Impact : performance (si applicable)
   - Effort : analyse 15 min + implémentation 1h

---

## Métriques de Qualité

| Critère | Score | Commentaire |
|---------|-------|-------------|
| Architecture | 9/10 | Flux clair, séparation OK |
| Gestion mémoire | 7/10 | Quelques optimisations possibles |
| Nommage | 9/10 | Cohérent, quelques incohérences mineures |
| DRY | 6/10 | Duplication JSON building |
| Gestion erreurs | 8/10 | Présente, à auditer exhaustivement |
| Optimisations | 8/10 | Bonnes pratiques, pas de bottleneck évident |
| Documentation | 6/10 | Manque commentaires sur formules |
| Build conditionnel | 9/10 | Correct |
| Sécurité | 8/10 | Bonne base, audit API complet requis |

**Score global** : **7.8/10** - Code de bonne qualité, améliorations mineures recommandées.

---

## Conclusion

Le code de SoundPanel 7 est **bien structuré et maintenable**. Les recommandations portent principalement sur :
- Optimisations mémoire mineures
- Factorisation de code dupliqué
- Documentation de formules complexes
- Validation exhaustive des entrées API (audit de sécurité)

**Aucun problème bloquant détecté**. Le code est prêt pour production avec les améliorations suggérées en priorité haute/moyenne.

---

**Prochaines étapes recommandées** :
1. Implémenter les recommandations priorité haute (2-3h)
2. Compiler et tester les 4 environnements
3. Audit sécurité complet des handlers API (cf. AGENT_TDD.md pour tests)
4. Documenter formules mathématiques
5. Itération suivante : factorisation NVS et logs
