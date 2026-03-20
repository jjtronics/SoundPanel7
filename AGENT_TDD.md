# Agent TDD - SoundPanel 7

Guide pour l'agent chargé des tests et de la validation de non-régression sur SoundPanel 7.

## Votre rôle

Assurer la qualité et la stabilité du firmware en créant des tests, validant les fonctionnalités et détectant les régressions.

## Workflow TDD (Test-Driven Development)

Le développement piloté par les tests suit un cycle en 3 phases : **RED → GREEN → REFACTOR**

### Les Trois Étapes du Cycle TDD

#### Phase 1 - RED (Échouer)
1. Écris **le plus petit test qui échoue**
2. S'assurer que le test s'exécute et échoue effectivement
3. Le test définit le comportement attendu **avant** l'implémentation

```cpp
// test/test_audio_engine/test_db_conversion.cpp
void test_rms_to_db_zero_returns_minus_infinity() {
    AudioEngine engine;
    float db = engine.rmsToDb(0.0f);
    TEST_ASSERT_TRUE(isinf(db) && db < 0);
}
// ❌ FAIL - rmsToDb() n'existe pas encore
```

#### Phase 2 - GREEN (Réussir)
1. Écris **le code minimal** pour faire passer le test
2. Ne pas chercher la perfection, juste la fonction
3. Accepter une solution imparfaite ("quick and dirty" OK temporairement)

```cpp
// src/AudioEngine.cpp
float AudioEngine::rmsToDb(float rms) {
    if (rms <= 0.0f) return -INFINITY;
    return 20.0f * log10f(rms);
}
// ✅ PASS - Test passe, même si l'implémentation est basique
```

#### Phase 3 - REFACTOR (Améliorer)
1. Améliorer la structure du code
2. Supprimer les redondances
3. Optimiser sans casser les tests
4. **Les tests doivent rester verts**

```cpp
// src/AudioEngine.cpp - Refactorisé
static constexpr float RMS_TO_DB_MULTIPLIER = 20.0f;
static constexpr float RMS_EPSILON = 1e-10f;

float AudioEngine::rmsToDb(float rms) {
    if (rms <= RMS_EPSILON) return -INFINITY;
    return RMS_TO_DB_MULTIPLIER * log10f(rms);
}
// ✅ PASS - Tests toujours verts, code amélioré
```

### Conventions de Nommage des Tests

Format : `test_should_[résultat attendu]_when_[condition]`

**Exemples** :
```cpp
void test_should_return_empty_array_when_no_items()
void test_should_throw_error_when_invalid_input()
void test_should_update_revision_when_buffer_wraps()
void test_should_interpolate_linearly_when_between_calibration_points()
```

### Pattern AAA (Arrange-Act-Assert)

Structurer chaque test en 3 sections claires :

```cpp
void test_history_circular_buffer_wraps_correctly() {
    // ARRANGE - Préparer les données de test
    SettingsV1 settings;
    settings.historyMinutes = 5;
    SharedHistory history;
    history.begin(&settings);

    // ACT - Exécuter le comportement à tester
    for (uint16_t i = 0; i < SharedHistory::POINT_COUNT + 10; i++) {
        history.update(float(i), i * 1000);
    }

    // ASSERT - Vérifier les résultats
    TEST_ASSERT_EQUAL(SharedHistory::POINT_COUNT, history.count());
    TEST_ASSERT_EQUAL_FLOAT(10.0f, history.valueAt(0));
}
```

### Quand Utiliser TDD

**✅ TDD est approprié pour** :
- Logique métier complexe (calibration, conversions dB)
- Algorithmes avec edge cases (buffer circulaire, interpolation)
- Fonctions de validation (input sanitization, bounds checking)
- Calculs mathématiques (RMS, Leq, Peak)
- State machines (WiFi reconnection, OTA states)

**❌ TDD moins adapté pour** :
- Design d'interface utilisateur (LVGL widgets)
- Configuration matérielle (GPIO, I2S, ADC)
- Intégration système (WiFi stack, ESP-IDF)
- Prototypes exploratoires (proof of concept)
- Migrations de bases de données (NVS schema changes)

### Erreurs Courantes à Éviter

1. **Tests trop volumineux** : Un test = un comportement
2. **Code de production excessif** : Phase GREEN = code minimal
3. **Refactorisation négligée** : Ne pas sauter la phase REFACTOR
4. **Vérification d'implémentation** : Tester le comportement, pas le code interne
5. **Ignorer les tests défaillants** : Toujours garder la suite verte

**Exemple d'erreur** :
```cpp
// ❌ WRONG - Test trop large, teste plusieurs comportements
void test_audio_engine_complete_workflow() {
    // Test initialization, calibration, measurement, et conversion
    // Si ça échoue, quelle partie est cassée ?
}

// ✅ RIGHT - Tests atomiques
void test_audio_engine_initializes_with_default_values()
void test_audio_engine_calibration_stores_reference_points()
void test_audio_engine_measures_rms_from_analog_input()
void test_audio_engine_converts_rms_to_calibrated_db()
```

### Cycle Complet TDD pour soundpanel7

**Exemple : Ajouter validation de bounds pour greenMax**

```cpp
// RED - Écrire le test qui échoue
void test_should_reject_greenMax_above_100() {
    SettingsV1 settings;
    settings.th.greenMax = 150;  // Invalide

    bool valid = SettingsStore::validateThresholds(settings.th);

    TEST_ASSERT_FALSE(valid);
}
// ❌ FAIL - validateThresholds() n'existe pas

// GREEN - Code minimal
bool SettingsStore::validateThresholds(const ThresholdsV1& th) {
    if (th.greenMax > 100) return false;
    return true;
}
// ✅ PASS

// REFACTOR - Améliorer
bool SettingsStore::validateThresholds(const ThresholdsV1& th) {
    if (th.greenMax < 0 || th.greenMax > 100) return false;
    if (th.orangeMax < th.greenMax || th.orangeMax > 100) return false;
    return true;
}
// ✅ PASS - Plus robuste, garde compatibilité
```

## Contexte du projet

SoundPanel 7 est un firmware ESP32-S3 embarqué. **Il n'y a pas encore de framework de tests unitaires en place**. Votre mission est de :

1. Établir une stratégie de test adaptée à l'embarqué
2. Créer des tests pour les composants critiques
3. Définir des procédures de validation manuelle
4. Automatiser ce qui peut l'être

## Architecture de test

### Tests unitaires (à mettre en place)

**Framework recommandé** : PlatformIO Test avec Unity

**Configuration** :
```ini
; Dans platformio.ini
test_framework = unity
test_build_src = yes
```

**Composants prioritaires pour les tests unitaires** :
1. `SharedHistory` : buffer circulaire, gestion de révision
2. `AudioEngine` : calculs dB, calibration, interpolation
3. `SettingsStore` : validation, serialization JSON
4. `JsonHelpers` : parsing et génération JSON

### Tests d'intégration (sur appareil)

**Workflow** :
1. Flash du firmware de test
2. Validation des logs série
3. Tests via API Web
4. Vérification de l'état persisté

## Plan de test par composant

### 1. SharedHistory (buffer circulaire)

**Tests unitaires** :
```cpp
// test/test_shared_history/test_main.cpp
#include <unity.h>
#include "SharedHistory.h"
#include "SettingsStore.h"

void test_history_initialization() {
    SettingsV1 settings;
    settings.historyMinutes = 5;

    SharedHistory history;
    history.begin(&settings);

    TEST_ASSERT_EQUAL(0, history.count());
    TEST_ASSERT_EQUAL(0, history.revision());
}

void test_history_circular_buffer() {
    SettingsV1 settings;
    settings.historyMinutes = 5;

    SharedHistory history;
    history.begin(&settings);

    // Remplir au-delà de la capacité
    for (uint16_t i = 0; i < SharedHistory::POINT_COUNT + 10; i++) {
        history.update(float(i), i * 1000);
    }

    // Vérifier que le buffer est circulaire
    TEST_ASSERT_EQUAL(SharedHistory::POINT_COUNT, history.count());
    TEST_ASSERT_EQUAL_FLOAT(10.0f, history.valueAt(0));  // Valeurs anciennes écrasées
}

void test_history_sample_period() {
    SettingsV1 settings;
    settings.historyMinutes = 10;

    SharedHistory history;
    history.begin(&settings);

    uint32_t period = history.samplePeriodMs();
    uint32_t expected = (10 * 60 * 1000) / SharedHistory::POINT_COUNT;

    TEST_ASSERT_EQUAL(expected, period);
}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_history_initialization);
    RUN_TEST(test_history_circular_buffer);
    RUN_TEST(test_history_sample_period);
    UNITY_END();
}

void loop() {}
```

### 2. AudioEngine (calibration)

**Tests unitaires** :
```cpp
// test/test_audio_engine/test_calibration.cpp
#include <unity.h>
#include "AudioEngine.h"

void test_calibration_3_points() {
    SettingsV1 settings;
    settings.audioSource = 1;  // Analog
    settings.calibrationPointCount = 3;
    settings.calibrationPoints[0][0].refDb = 45.0f;
    settings.calibrationPoints[0][0].logRms = -10.0f;
    settings.calibrationPoints[0][1].refDb = 65.0f;
    settings.calibrationPoints[0][1].logRms = -5.0f;
    settings.calibrationPoints[0][2].refDb = 85.0f;
    settings.calibrationPoints[0][2].logRms = 0.0f;

    AudioEngine engine;
    // Test interpolation linéaire
    float db = engine.computeCalibratedDb(-7.5f, settings);  // Milieu entre 45 et 65
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 55.0f, db);
}

void test_calibration_profile_selection() {
    TEST_ASSERT_EQUAL(0, calibrationProfileIndexForAudioSource(1));  // Analog
    TEST_ASSERT_EQUAL(1, calibrationProfileIndexForAudioSource(2));  // PDM
    TEST_ASSERT_EQUAL(2, calibrationProfileIndexForAudioSource(3));  // INMP441
    TEST_ASSERT_EQUAL(0xFF, calibrationProfileIndexForAudioSource(0));  // Demo
}
```

### 3. SettingsStore (persistance et validation)

**Tests unitaires** :
```cpp
// test/test_settings/test_validation.cpp
#include <unity.h>
#include "SettingsStore.h"

void test_settings_version_compatibility() {
    // Vérifier que la magic number est constante
    TEST_ASSERT_EQUAL_HEX32(0x53503730, SETTINGS_MAGIC);
}

void test_settings_json_roundtrip() {
    SettingsV1 original;
    original.backlight = 80;
    original.th.greenMax = 55;
    original.th.orangeMax = 70;
    strcpy(original.hostname, "soundpanel7");

    SettingsStore store;
    String json = store.toJsonFull(&original);

    SettingsV1 restored;
    bool success = store.fromJson(json.c_str(), &restored);

    TEST_ASSERT_TRUE(success);
    TEST_ASSERT_EQUAL(80, restored.backlight);
    TEST_ASSERT_EQUAL(55, restored.th.greenMax);
    TEST_ASSERT_EQUAL(70, restored.th.orangeMax);
    TEST_ASSERT_EQUAL_STRING("soundpanel7", restored.hostname);
}

void test_settings_bounds_validation() {
    SettingsV1 settings;

    // Test valeurs hors limites
    settings.backlight = 255;      // Max devrait être 100
    settings.th.greenMax = 200;    // Max devrait être ~100

    // La validation devrait clamp ou rejeter
    // TODO: implémenter validation dans SettingsStore
}
```

## Tests d'intégration manuels

### Checklist de validation complète

#### Boot et initialisation
- [ ] Le firmware boot sans erreur
- [ ] Version affichée correcte dans les logs
- [ ] Confirmation OTA image si boot après OTA
- [ ] Tous les composants s'initialisent dans l'ordre

**Commande** :
```bash
pio run -e soundpanel7_usb -t upload && pio device monitor -b 115200
```

**Logs attendus** :
```
[BOOT] version=0.2.17 running=ota_0 boot=ota_0 state=valid
[SETTINGS] loaded from NVS
[NET] connecting to WiFi...
[OTA] enabled port=3232
[WEB] started on port 80
[AUDIO] engine started source=Analog
```

#### Persistance NVS
- [ ] Modifier une config via web UI
- [ ] Redémarrer l'appareil
- [ ] Vérifier que la config est conservée

**Test via API** :
```bash
# Sauvegarder config actuelle
curl http://soundpanel7.local/api/config/export > backup.json

# Modifier une valeur
curl -X POST http://soundpanel7.local/api/ui \
  -d "backlight=75"

# Redémarrer
curl -X POST http://soundpanel7.local/api/reboot

# Attendre le reboot (30s)
sleep 30

# Vérifier la valeur
curl http://soundpanel7.local/api/status | jq '.backlight'
# Devrait afficher 75
```

#### Audio et calibration
- [ ] Passage de Demo à Analog : valeurs changent
- [ ] Calibration 3 points : capture réussie
- [ ] Calibration conservée après reboot
- [ ] dB, Leq, Peak mis à jour en temps réel

**Test** :
```bash
# Activer source Analog
curl -X POST http://soundpanel7.local/api/audio \
  -d "source=1"

# Lancer calibration point 1 (45 dB référence)
curl -X POST http://soundpanel7.local/api/calibrate \
  -d "index=0&refDb=45.0"

# Vérifier dans status
curl http://soundpanel7.local/api/status | jq '.calibration'
```

#### MQTT
- [ ] Connexion au broker réussie
- [ ] Publication des topics `db`, `leq`, `peak`
- [ ] MQTT Discovery : entités créées dans HA
- [ ] Contrôle LIVE via `live/set` fonctionne

**Test avec mosquitto** :
```bash
# Subscribe aux topics
mosquitto_sub -h BROKER_IP -t "soundpanel7/#" -v

# Vérifier publication régulière :
# soundpanel7/db 65.3
# soundpanel7/leq 63.8
# soundpanel7/peak 72.1

# Tester commande LIVE
mosquitto_pub -h BROKER_IP -t "soundpanel7/live/set" -m "1"
```

#### OTA
- [ ] OTA réseau depuis PlatformIO fonctionne
- [ ] OTA GitHub : détection de nouvelle release
- [ ] OTA GitHub : vérification SHA-256
- [ ] OTA GitHub : installation et reboot automatique

**Test OTA réseau** :
```bash
pio run -e soundpanel7_ota -t upload --upload-port 192.168.X.X
# Doit se connecter, uploader et redémarrer automatiquement
```

#### Interface Web
- [ ] Toutes les pages chargent sans erreur 404
- [ ] SSE stream fonctionne (port 81)
- [ ] Authentification requise pour les pages sensibles
- [ ] Export/Import config fonctionne
- [ ] Backup/Restore fonctionne

#### Builds conditionnels
- [ ] `soundpanel7_usb` compile et fonctionne
- [ ] `soundpanel7_ota` compile et fonctionne
- [ ] `soundpanel7_headless_usb` compile et fonctionne
- [ ] `soundpanel7_headless_ota` compile et fonctionne

## Tests de non-régression

### Scénarios critiques à valider avant release

1. **Boot clean après flash USB**
2. **Boot après OTA (réseau et GitHub)**
3. **Persistance settings après reboot**
4. **Changement de source audio sans crash**
5. **Calibration 3 et 5 points**
6. **Export/Import config complet**
7. **Factory reset**
8. **MQTT reconnexion après perte réseau**
9. **Web UI avec plusieurs sessions simultanées**
10. **Charge CPU/RAM acceptable (monitoring continu 24h)**

### Métriques de performance

**À surveiller** :
- Loop time LVGL : < 30ms (seuil spike)
- Heap libre : > 50KB en continu
- PSRAM libre : > 500KB
- CPU idle : > 60%

**Commande de monitoring** :
```bash
# Via API status
while true; do
  curl -s http://soundpanel7.local/api/status | \
    jq '{heap: .heap, psram: .psram, lvgl_avg: .lvglAvg}'
  sleep 5
done
```

## Automatisation future

### Tests à automatiser en priorité

1. **JSON serialization/deserialization** (SettingsStore)
2. **Calibration math** (AudioEngine)
3. **Circular buffer logic** (SharedHistory)
4. **Input validation** (WebManager helpers)

### CI/CD pipeline suggestion

```yaml
# .github/workflows/test.yml
name: Test
on: [push, pull_request]
jobs:
  build-test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v3
      - uses: actions/setup-python@v4
      - name: Install PlatformIO
        run: pip install platformio
      - name: Run unit tests
        run: pio test -e native
      - name: Build all environments
        run: |
          pio run -e soundpanel7_usb
          pio run -e soundpanel7_ota
          pio run -e soundpanel7_headless_usb
          pio run -e soundpanel7_headless_ota
```

## Rapport de test

Après chaque cycle de test, documenter :

```markdown
## Test Report - YYYY-MM-DD

### Build Info
- Version: v0.2.17
- Commit: abc1234
- Environments tested: soundpanel7_usb, soundpanel7_ota

### Test Results
- [x] Boot sequence
- [x] NVS persistence
- [x] Audio calibration
- [ ] MQTT reconnection (FAILED - see issue #42)
- [x] OTA GitHub

### Regressions Detected
- MQTT ne reconnecte pas automatiquement après perte WiFi
- OTA GitHub timeout après 60s sur connexion lente

### Performance
- Heap min: 78KB (OK)
- LVGL avg: 12ms (OK)
- CPU idle: 72% (OK)

### Recommendations
- Implémenter retry MQTT avec backoff exponentiel
- Augmenter timeout OTA GitHub à 120s
```

## Ressources

- Unity framework: https://github.com/ThrowTheSwitch/Unity
- PlatformIO testing: https://docs.platformio.org/en/latest/advanced/unit-testing/index.html
- ESP32 test examples: https://github.com/espressif/esp-idf/tree/master/examples/system/unit_test
