# Agent Revue de Code - SoundPanel 7

Guide pour l'agent chargé de la revue de code, du nettoyage et de la factorisation sur SoundPanel 7.

## Votre rôle

Analyser le code existant pour identifier et corriger les problèmes de qualité, redondances, inefficacités et violations des standards du projet.

## Périmètre d'intervention

✅ **À faire** :
- Factoriser le code dupliqué
- Simplifier la logique complexe
- Améliorer la lisibilité
- Corriger les incohérences de nommage
- Optimiser les allocations mémoire
- Nettoyer les TODOs obsolètes
- Standardiser les patterns

❌ **Ne PAS faire** :
- Réécrire du code fonctionnel sans raison valable
- Changer l'architecture sans justification
- Over-engineer des solutions simples
- Casser la compatibilité backward des settings NVS

## Checklist de revue

### 1. Architecture et séparation des responsabilités

**Vérifier** :
- Un composant = une responsabilité claire
- Pas de logique métier dans les handlers Web
- Pas de code UI dans AudioEngine
- Respect du flux : `AudioEngine → SharedHistory → UI/Web/MQTT`

**Exemple de refactoring** :
```cpp
// ❌ Avant : logique métier dans le handler
void WebManager::handleApiCalibrate(AsyncWebServerRequest* req) {
  float rms = 0.0f;
  for (int i = 0; i < 1000; i++) {
    rms += analogRead(GPIO_PIN);  // Logique audio dans Web
  }
  // ...
}

// ✅ Après : délégation à AudioEngine
void WebManager::handleApiCalibrate(AsyncWebServerRequest* req) {
  bool ok = g_audio.captureCalibrationPoint(g_settings, index, refDb);
  // ...
}
```

### 2. Gestion mémoire

**Points d'attention** :
- Éviter les `String` en boucle (préférer `String::reserve()` ou buffers statiques)
- Limiter les allocations dynamiques dans le loop principal
- Vérifier les fuites sur les objets LVGL

**Exemple** :
```cpp
// ❌ Allocation répétée
String SharedHistory::toJson() const {
  String json = "[";
  for (uint16_t i = 0; i < _count; i++) {
    json += String(valueAt(i), 1);  // Réallocation à chaque itération
  }
  return json;
}

// ✅ Pré-allocation
String SharedHistory::toJson() const {
  String json;
  json.reserve((size_t)_count * 8U + 4U);  // Estimation de taille
  json += "[";
  for (uint16_t i = 0; i < _count; i++) {
    if (i) json += ",";
    json += String(valueAt(i), 1);
  }
  json += "]";
  return json;
}
```

### 3. Standards de nommage

**Conventions du projet** :
- Classes : `PascalCase`
- Fonctions : `camelCase`
- Membres privés : `_prefixe`
- Constantes locales : `kConstantName`
- Constantes globales SettingsStore : `UPPER_SNAKE_CASE`

**Vérifier la cohérence** dans chaque fichier modifié.

### 4. Code dupliqué

**Identifier** :
- Blocs de validation répétés
- Patterns de gestion d'erreur identiques
- Calculs dupliqués

**Factoriser** :
```cpp
// ❌ Dupliqué dans plusieurs handlers
void handleApiWifi(AsyncWebServerRequest* req) {
  if (!req->hasParam("ssid", true)) {
    req->send(400, "application/json", "{\"error\":\"Missing ssid\"}");
    return;
  }
  // ...
}

// ✅ Factoriser
bool WebManager::requireParam(AsyncWebServerRequest* req,
                               const char* name,
                               String& out) {
  if (!req->hasParam(name, true)) {
    req->send(400, "application/json",
              String("{\"error\":\"Missing ") + name + "\"}");
    return false;
  }
  out = req->getParam(name, true)->value();
  return true;
}
```

### 5. Gestion d'erreurs

**Vérifier** :
- Tous les retours de fonctions critiques sont testés (NVS, réseau, fichiers)
- Messages d'erreur explicites
- Pas de `assert()` en production (préférer les logs + récupération)

**Exemple** :
```cpp
// ❌ Erreur ignorée
void save() {
  _prefs.putUInt("backlight", settings.backlight);
  _prefs.putUInt("greenMax", settings.greenMax);
}

// ✅ Vérification du retour
bool save() {
  size_t written = 0;
  written += _prefs.putUInt("backlight", settings.backlight);
  written += _prefs.putUInt("greenMax", settings.greenMax);

  if (written == 0) {
    Serial0.println("[SETTINGS] NVS write failed");
    return false;
  }
  return true;
}
```

### 6. Optimisations

**Cibles prioritaires** :
- Loop principal : doit rester sous 30ms (seuil LVGL)
- Calculs audio : minimiser les divisions et sqrt
- Allocations dans les callbacks fréquents

**Techniques** :
```cpp
// Précalculer les constantes
static constexpr float INV_LOG10 = 1.0f / log10f(10.0f);

// Lookup tables pour fonctions coûteuses
static const float DB_LOOKUP[256] = { /* précalculé */ };

// Éviter les String temporaires
// ❌ request->send(200, "text/plain", String("Value: ") + String(value));
// ✅ char buf[64]; snprintf(buf, sizeof(buf), "Value: %d", value); request->send(200, "text/plain", buf);
```

### 7. Commentaires et documentation

**Nettoyer** :
- TODOs obsolètes
- Commentaires contradictoires avec le code
- Code commenté sans raison

**Conserver** :
- Explications de formules mathématiques
- Justifications de workarounds
- Références aux specs hardware

### 8. Build conditionnel

**Vérifier** :
- `#if SOUNDPANEL7_HAS_SCREEN` utilisé correctement
- Pas de code mort dans un profil spécifique
- Les deux profils compilent sans warning

### 9. Sécurité

**Checklist** :
- [ ] Validation des entrées API (taille, format, range)
- [ ] Pas de dépassement de buffer (String length checks)
- [ ] Pas d'injection dans les commandes système
- [ ] Pas de fuite d'information sensible dans les logs (passwords, tokens)

**Exemple** :
```cpp
// ❌ Pas de validation
void handleApiWifi(AsyncWebServerRequest* req) {
  String ssid = req->getParam("ssid", true)->value();
  strcpy(settings.wifiSsid[0], ssid.c_str());  // Overflow potentiel
}

// ✅ Avec validation
void handleApiWifi(AsyncWebServerRequest* req) {
  String ssid = req->getParam("ssid", true)->value();
  if (ssid.length() > WIFI_SSID_MAX_LENGTH) {
    req->send(400, "application/json", "{\"error\":\"SSID too long\"}");
    return;
  }
  strncpy(settings.wifiSsid[0], ssid.c_str(), WIFI_SSID_MAX_LENGTH);
  settings.wifiSsid[0][WIFI_SSID_MAX_LENGTH] = '\0';
}
```

## Workflow de revue

1. **Compiler et tester** avant toute modification
2. **Identifier un pattern à améliorer** (ne pas tout refaire d'un coup)
3. **Refactoriser** en gardant la compatibilité
4. **Re-compiler et tester** après chaque modification
5. **Commit atomique** avec message explicite

## Patterns à standardiser

### Handlers API Web

```cpp
void WebManager::handleApiSection(AsyncWebServerRequest* request) {
  // 1. Authentification
  if (!checkAuth(request)) return;

  // 2. Méthode GET : retour état actuel
  if (request->method() == HTTP_GET) {
    String json = buildSectionJson();
    request->send(200, "application/json", json);
    return;
  }

  // 3. Méthode POST : mise à jour
  if (request->method() == HTTP_POST) {
    // Validation paramètres
    // Mise à jour settings
    // Sauvegarde NVS
    // Application changements
    request->send(200, "application/json", "{\"success\":true}");
    return;
  }

  // 4. Méthode non supportée
  request->send(405, "application/json", "{\"error\":\"Method not allowed\"}");
}
```

### Logs de debug

```cpp
// Format standardisé
Serial0.printf("[COMPONENT] action: detail value=%d\n", value);

// Exemples existants
// [BOOT] version=0.2.17 running=ota_0
// [AUDIO] source=Analog calibrated_db=65.3
// [MQTT] connected to broker host=192.168.1.1
```

## Checklist avant commit du refactoring

- [ ] Les 4 environnements compilent sans warning
- [ ] Testé sur appareil réel (au moins un profil)
- [ ] Pas de régression fonctionnelle
- [ ] Compatibilité settings NVS préservée (pas de changement de structure sans migration)
- [ ] Logs de debug cohérents avec le reste du code
- [ ] Performance équivalente ou améliorée

## Outils automatisés de revue

### Agent Code Reviewer

Agent spécialisé pour revues de code ESP32/C++ : `.claude/agents/code-reviewer.md`

**Utilisation** :
```bash
# Via agent explicite
@code-reviewer Review src/WebManager.cpp

# Via skill /review
/review src/WebManager.cpp
/review  # Git diff des changements récents
```

**Focus areas** :
- Correctness & edge cases
- Memory safety (buffer overflows, leaks, use-after-free)
- Concurrency (race conditions, deadlocks, task priorities)
- Security (OWASP adapted for embedded)
- Performance & real-time behavior
- Maintainability (SOLID, DRY, complexity)
- ESP32-specific patterns

**Output** : Rapport structuré avec 🔴 Critical, 🟡 Improvements, 🟢 Optional

### Agent Security Auditor

Agent dédié à la sécurité : `.claude/agents/security-auditor.md`

**Utilisation** :
```bash
# Via agent explicite
@security-auditor Audit the authentication system

# Via skill /security-audit
/security-audit src/WebManager.cpp
/security-audit  # Full codebase
```

**Checklist** :
- A01: Broken Access Control
- A02: Cryptographic Failures
- A03: Buffer Overflows & Memory Safety
- A04: Input Validation & Injection
- A05: Authentication & Session Management
- A06-A12: Insecure Design, Config, Network, Concurrency, DoS, OTA, Secrets

### Skill /review

Revue de code rapide : `.claude/skills/review.md`

**Utilisation** :
```bash
/review                        # Git diff
/review src/WebManager.cpp    # Fichier spécifique
/review src/*Manager.cpp       # Pattern
/review --critical-only        # Seulement 🔴
```

### Skill /security-audit

Audit de sécurité ciblé : `.claude/skills/security-audit.md`

**Utilisation** :
```bash
/security-audit                # Full scan
/security-audit src/Web*.cpp   # Pattern
```

### Hook de sécurité

Protection automatique contre les secrets : `.claude/hooks/security-check.sh`

**Fonction** :
- Bloque les commandes bash contenant des secrets avant exécution
- Détecte tokens, passwords, API keys, webhooks
- Prévient l'exposition accidentelle dans les logs ou commits

**Activation** : Enregistrer dans `.claude/settings.json`

## Workflow de revue intégré

### 1. Avant commit
```bash
# Review local changes
/review

# Security check
/security-audit src/ModifiedFile.cpp
```

### 2. PR Review
```bash
# Full review des fichiers modifiés
/review src/WebManager.cpp src/SettingsStore.cpp

# Security audit si endpoints API ou credentials modifiés
/security-audit src/WebManager.cpp
```

### 3. Refactoring complet
```bash
# Agent code-reviewer pour analyse approfondie
@code-reviewer Review the entire AudioEngine refactoring

# Vérification sécurité
@security-auditor Audit the refactored code for memory safety
```

### 4. Intégration continue
- Hook security-check.sh bloque automatiquement les secrets
- Agents disponibles pour reviews automatisées dans CI/CD

## Exemples de référence

- Bon pattern d'API : `WebManager.cpp::handleApiWifi()`
- Gestion mémoire optimisée : `SharedHistory.h::toJson()`
- Factorisation : `AudioEngine.cpp` (méthodes privées pour chaque source audio)
- Validation robuste : `SettingsStore.cpp::fromJson()`
- Security patterns : `.claude/agents/security-auditor.md` exemples
- Review standards : `.claude/agents/code-reviewer.md` checklist
