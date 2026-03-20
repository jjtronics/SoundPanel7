# Agent Développement - SoundPanel 7

Guide pour l'agent chargé du développement de nouvelles fonctionnalités sur SoundPanel 7.

## Votre rôle

Développer des nouvelles features en respectant l'architecture existante, avec un code professionnel, lisible, commenté et maintenable.

## Avant de commencer

1. **Lire AI.md** pour comprendre l'architecture globale
2. **Identifier les composants concernés** par la feature
3. **Vérifier les flags de compilation** : `SOUNDPANEL7_HAS_SCREEN` et `SOUNDPANEL7_AUDIO_BOARD_PROFILE`

## Workflow de développement

### 1. Analyse de l'existant

Avant d'écrire du code :
- Lire les fichiers concernés pour comprendre les patterns
- Identifier les structures de données utilisées (ex: `SettingsV1` pour la persistance)
- Vérifier si une fonctionnalité similaire existe déjà

### 2. Conception

Respecter le flux de données :
```
AudioEngine → SharedHistory → UI/Web/MQTT
```

**Persistance** : toute nouvelle configuration doit être ajoutée à `SettingsV1` dans `SettingsStore.h` et gérée par NVS.

**API Web** : suivre le pattern existant dans `WebManager.cpp` :
- Endpoints REST cohérents (`GET /api/section`, `POST /api/section`)
- Validation des entrées
- Réponses JSON structurées
- Gestion d'erreurs explicites

### 3. Implémentation

#### Standards de code

**Nommage** :
- Classes : `PascalCase` (ex: `AudioEngine`)
- Fonctions/méthodes : `camelCase` (ex: `computeAnalogRms`)
- Variables membres privées : `_prefixe` (ex: `_lastSampleMs`)
- Constantes globales : `kConstantName` (ex: `kAudioUpdatePeriodMs`)
- Constantes de SettingsStore : `UPPER_SNAKE_CASE` (ex: `DEFAULT_HISTORY_MINUTES`)

**Commentaires** :
- Documenter le **pourquoi**, pas le **quoi** (le code doit être self-explanatory)
- Commenter les calculs complexes (ex: calibration audio, formules mathématiques)
- Ajouter des TODOs explicites si un workaround temporaire est nécessaire

**Exemple de code attendu** :
```cpp
// AudioEngine.cpp
float AudioEngine::computeCalibratedDb(float rms, const SettingsV1& s) const {
  // Conversion RMS vers pseudo-dB (échelle logarithmique)
  float pseudoDb = 20.0f * log10f(rms + 1e-6f);

  // Application de la courbe de calibration
  uint8_t profIdx = calibrationProfileIndexForAudioSource(s.audioSource);
  if (profIdx == 0xFF || s.calibrationPointCount < 3) {
    // Pas de calibration : offset par défaut
    return pseudoDb + DEFAULT_DB_OFFSET;
  }

  // Interpolation linéaire entre les points de calibration
  // ...
}
```

#### Gestion des builds conditionnels

```cpp
#if SOUNDPANEL7_HAS_SCREEN
  // Code spécifique à l'écran
  g_ui.updateDisplay(metrics);
#endif
```

Pour les fonctionnalités audio/web/MQTT : toujours supporter les deux profils.

### 4. Intégration

#### Nouvelle configuration persistante

1. Ajouter les champs dans `struct SettingsV1` (`SettingsStore.h`)
2. Incrémenter `SETTINGS_VERSION`
3. Ajouter les constantes de validation (min/max)
4. Implémenter le load/save dans `SettingsStore.cpp`
5. Ajouter dans le JSON d'export/import (`toJsonFull()`, `fromJson()`)

#### Nouvel endpoint API

1. Déclarer le handler dans `WebManager.h`
2. Implémenter dans `WebManager.cpp` :
   ```cpp
   void WebManager::handleApiNewFeature(AsyncWebServerRequest* request) {
     if (!checkAuth(request)) return;

     // Validation des paramètres
     if (!request->hasParam("param", true)) {
       request->send(400, "application/json", "{\"error\":\"Missing parameter\"}");
       return;
     }

     // Traitement
     // ...

     request->send(200, "application/json", "{\"success\":true}");
   }
   ```
3. Enregistrer la route dans `begin()` :
   ```cpp
   _server.on("/api/newfeature", HTTP_POST,
              std::bind(&WebManager::handleApiNewFeature, this, std::placeholders::_1));
   ```

#### Publication MQTT

Si la feature doit être publiée via MQTT :
1. Ajouter le topic dans `MqttManager.h`
2. Implémenter la publication dans `MqttManager.cpp`
3. Ajouter l'entité dans MQTT Discovery si applicable

### 5. Test sur appareil

```bash
# Build et flash USB
pio run -e soundpanel7_usb -t upload
pio device monitor -b 115200

# Vérifier :
# - Logs de boot sans erreur
# - Feature accessible via l'interface
# - Persistance après reboot
# - Pas de régression sur les fonctionnalités existantes
```

## Checklist avant commit

- [ ] Code compilé sans warning sur les 4 environnements
- [ ] Testé sur appareil réel (au moins USB)
- [ ] Pas de valeurs hardcodées (utiliser les constantes de SettingsStore)
- [ ] Gestion d'erreurs explicite (pas de crash sur entrée invalide)
- [ ] Persistance NVS fonctionnelle si applicable
- [ ] Endpoint API documenté dans AI.md si nouveau
- [ ] Code commenté (pourquoi, pas quoi)
- [ ] Respect des patterns existants
- [ ] Pas de fuite mémoire (attention aux `String` et allocations dynamiques)
- [ ] Compatible avec les deux profils hardware si applicable

## Anti-patterns à éviter

❌ **Créer des fichiers inutiles** : préférer éditer les composants existants
❌ **Dupliquer du code** : factoriser dans des fonctions réutilisables
❌ **Ignorer les erreurs** : toujours vérifier les retours (NVS, réseau, etc.)
❌ **Bloquer le loop principal** : utiliser des états asynchrones pour les opérations longues
❌ **Hardcoder des valeurs** : utiliser les constantes et la configuration

## Exemples de référence

- **Persistance NVS** : `SettingsStore.cpp::save()`, `load()`
- **Calibration multi-profils** : `AudioEngine.cpp::computeCalibratedDb()`
- **Endpoint avec validation** : `WebManager.cpp::handleApiWifi()`
- **Build conditionnel** : `main.cpp` (gestion de l'UI selon `SOUNDPANEL7_HAS_SCREEN`)
- **Circular buffer** : `SharedHistory.h`

## Ressources

- Architecture globale : `AI.md`
- Release workflow : `RELEASING.md`
- Pin mapping : `README.md` (sections câblage)
