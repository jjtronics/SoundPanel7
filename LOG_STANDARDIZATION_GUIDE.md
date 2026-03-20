# Guide de Standardisation des Logs - SoundPanel 7

**Date** : 2025-03-19
**Objectif** : Uniformiser le format des logs pour améliorer la debuggabilité
**Conforme à** : CODE_REVIEW_REPORT.md - Priorité Moyenne #6

---

## Format Standard

### Pattern Principal

```cpp
Serial0.printf("[COMPONENT] action: details key=value\n", ...);
```

**Composants** :
- `[COMPONENT]` : Tag du module en MAJUSCULES (ex: `[MQTT]`, `[Net]`, `[WEB]`, `[OTA]`, `[AUDIO]`)
- `action:` : Verbe ou nom décrivant l'action (lowercase avec underscores si nécessaire)
- `details` : Informations contextuelles au format `key=value` séparés par des espaces

---

## Tags de Composants

| Component | Usage |
|-----------|-------|
| `[MQTT]` | MqttManager, publication, discovery |
| `[Net]` | NetManager, WiFi, mDNS, portail config |
| `[WEB]` | WebManager, API REST, authentification |
| `[AUDIO]` | AudioEngine, calibration, acquisition |
| `[OTA]` | OtaManager, updates réseau |
| `[RELEASE]` | ReleaseUpdateManager, GitHub OTA |
| `[SETTINGS]` | SettingsStore, NVS, import/export |
| `[UI]` | TouchManager, LVGL (si écran présent) |
| `[PWR]` | Power management, shutdown, sleep |
| `[TIME]` | NTP, horloge système |

---

## Actions Standard

### Statut
```cpp
Serial0.println("[MQTT] status: disabled");
Serial0.println("[OTA] status: enabled");
```

### Connexion
```cpp
Serial0.println("[MQTT] connection: established");
Serial0.println("[Net] connection: failed reason=timeout");
```

### Configuration
```cpp
Serial0.printf("[MQTT] config: host=%s port=%u\n", host, port);
Serial0.printf("[Net] credential: stored ssid=%s\n", ssid.c_str());
```

### Démarrage/Arrêt
```cpp
Serial0.printf("[Net] portal: starting ssid='%s'\n", setupApName.c_str());
Serial0.println("[MQTT] service: stopped");
```

### Migration/Sauvegarde
```cpp
Serial0.printf("[Net] migration: legacy_wifi ssid=%s\n", ssid.c_str());
Serial0.println("[SETTINGS] backup: saved");
```

### Erreurs
```cpp
Serial0.printf("[AUDIO] error: calibration_failed index=%d\n", index);
Serial0.println("[Net] credential: rejected reason=too_long");
```

### Opérations Réussies
```cpp
Serial0.printf("[WEB] CAL point %d/%d saved @ %.1f dB\n", index + 1, count, refDb);
Serial0.println("[RELEASE] install: started");
```

---

## Exemples de Refactoring

### Avant / Après

#### Exemple 1 : Status simple
```cpp
// ❌ Avant
Serial0.println("[MQTT] disabled");

// ✅ Après
Serial0.println("[MQTT] status: disabled");
```

#### Exemple 2 : Connexion
```cpp
// ❌ Avant
Serial0.println("[MQTT] connected");

// ✅ Après
Serial0.println("[MQTT] connection: established");
```

#### Exemple 3 : Démarrage avec paramètres
```cpp
// ❌ Avant
Serial0.printf("[Net] Starting WiFi portal '%s'\n", setupApName.c_str());

// ✅ Après
Serial0.printf("[Net] portal: starting ssid='%s'\n", setupApName.c_str());
```

#### Exemple 4 : État complexe
```cpp
// ❌ Avant
Serial0.println("[Net] WiFi portal running with STA retries enabled");

// ✅ Après
Serial0.println("[Net] portal: running mode=AP_STA sta_retries=enabled");
```

#### Exemple 5 : Migration
```cpp
// ❌ Avant
Serial0.printf("[Net] Migrating legacy WiFi credential for SSID=%s\n", ssid.c_str());

// ✅ Après
Serial0.printf("[Net] migration: legacy_wifi ssid=%s\n", ssid.c_str());
```

#### Exemple 6 : Rejet/Erreur
```cpp
// ❌ Avant
Serial0.println("[Net] WiFi credential skipped: too long");

// ✅ Après
Serial0.println("[Net] credential: rejected reason=too_long");
```

---

## Exemples Déjà Conformes

Ces logs suivent déjà le format standard (ne pas modifier) :

```cpp
// ✅ Bon format
Serial0.printf("[MQTT] configured host=%s port=%u clientId=%s base=%s\n", ...);
Serial0.printf("[WEB] UI saved: backlight=%d touch=%d tardis=%d ...\n", ...);
Serial0.printf("[WEB][AUTH] bootstrap attempt user='%s' body=%uB password=%uB\n", ...);
```

---

## Conventions Supplémentaires

### Format des Valeurs

**Strings** : Utiliser des guillemets simples si la valeur peut contenir des espaces
```cpp
ssid='My WiFi Network'
hostname='soundpanel7.local'
```

**Booleans** : Utiliser `true`/`false` ou `enabled`/`disabled`
```cpp
touch=enabled
mqtt=false
```

**Numériques** : Pas de guillemets
```cpp
port=1883
backlight=75
```

**Listes** : Séparer par des virgules sans espaces
```cpp
status=connected,online,publishing
```

### Sous-composants

Pour les sous-systèmes, utiliser `[COMPONENT][SUBSYSTEM]` :
```cpp
Serial0.println("[WEB][AUTH] login: success user='admin'");
Serial0.println("[AUDIO][CAL] point: captured index=2 refDb=65.0");
```

### Logs Multi-lignes

Si un log dépasse 120 caractères, couper en plusieurs lignes avec le même tag :
```cpp
Serial0.printf("[WEB] UI saved: backlight=%d touch=%d tardis=%d\n", ...);
Serial0.printf("[WEB] UI config: green=%d orange=%d hist=%d page=%d\n", ...);
```

---

## Checklist de Refactoring

### Fichiers Analysés (Exemples corrigés)

- [x] **MqttManager.cpp** (2 corrections)
  - Ligne 23 : `"[MQTT] disabled"` → `"[MQTT] status: disabled"`
  - Ligne 257 : `"[MQTT] connected"` → `"[MQTT] connection: established"`

- [x] **NetManager.cpp** (3 corrections)
  - Ligne 98 : `"Starting WiFi portal"` → `"portal: starting"`
  - Ligne 104 : `"WiFi portal running"` → `"portal: running mode=AP_STA"`
  - Ligne 157 : `"Migrating legacy"` → `"migration: legacy_wifi"`
  - Ligne 164 : `"skipped: too long"` → `"rejected reason=too_long"`

### Fichiers Restants à Auditer

- [ ] **WebManager.cpp** (~50 logs)
- [ ] **AudioEngine.cpp** (~15 logs)
- [ ] **OtaManager.cpp** (~10 logs)
- [ ] **ReleaseUpdateManager.cpp** (~20 logs)
- [ ] **SettingsStore.cpp** (~10 logs)
- [ ] **TouchManager.cpp** (~5 logs)
- [ ] **main.cpp** (~20 logs)

---

## Workflow de Refactoring

### Étape 1 : Identifier les Logs Non Conformes

```bash
# Rechercher les logs sans action:
grep -r "Serial0\\.print.*\\[" src/ | grep -v ": "

# Rechercher les logs avec phrases complètes
grep -r "Serial0\\.print.*\\[.*\\] [A-Z]" src/
```

### Étape 2 : Analyser le Contexte

Pour chaque log :
1. Lire le contexte du code environnant
2. Identifier l'action principale (connexion, config, erreur, etc.)
3. Extraire les détails pertinents au format `key=value`

### Étape 3 : Refactorer

```cpp
// Avant
Serial0.printf("[Component] Something happened with %s\n", detail);

// Après
Serial0.printf("[COMPONENT] action: happened detail=%s\n", detail);
```

### Étape 4 : Valider

1. Compiler tous les environnements : `pio run -e soundpanel7_usb` etc.
2. Tester sur appareil réel
3. Vérifier que les logs sont toujours lisibles
4. Confirmer que le parsing automatisé fonctionne

---

## Parsing Automatisé

Le format standardisé permet un parsing facile pour monitoring/alertes :

```python
import re

LOG_PATTERN = r'\[([A-Z]+)\] ([a-z_]+): (.+)'

def parse_log(line):
    match = re.match(LOG_PATTERN, line)
    if match:
        component, action, details = match.groups()
        # Parser les key=value pairs
        details_dict = {}
        for pair in details.split():
            if '=' in pair:
                key, value = pair.split('=', 1)
                details_dict[key] = value.strip("'\"")
        return {
            'component': component,
            'action': action,
            'details': details_dict
        }
    return None

# Exemple
log = "[MQTT] connection: established host=192.168.1.1 port=1883"
parsed = parse_log(log)
# {'component': 'MQTT', 'action': 'connection', 'details': {'host': '192.168.1.1', 'port': '1883'}}
```

---

## Bénéfices

### Avant Standardisation
```
[MQTT] disabled
[Net] Starting WiFi portal 'SoundPanel7-1A2B'
[MQTT] connected
[Net] WiFi credential skipped: too long
```

**Problèmes** :
- Inconsistant (verbes, noms, phrases complètes)
- Difficile à parser automatiquement
- Pas de structure claire action/détails

### Après Standardisation
```
[MQTT] status: disabled
[Net] portal: starting ssid='SoundPanel7-1A2B'
[MQTT] connection: established
[Net] credential: rejected reason=too_long
```

**Avantages** :
- Format uniforme et prévisible
- Parsing automatique trivial
- Filtrage par component/action facile
- Key=value permet extraction de métriques
- Plus concis et informatif

---

## Recommandation

**Approche Progressive** :
1. ✅ Appliquer le format aux nouveaux logs dès maintenant
2. ⏳ Refactorer les fichiers critiques (WebManager, MqttManager, NetManager) - FAIT partiellement
3. ⏳ Auditer et corriger les autres fichiers lors de modifications futures
4. 📊 Considérer un script de validation dans CI/CD pour forcer le format

**Priorité** : 🟡 Moyenne - Améliore debuggabilité mais pas critique

**Effort estimé** : 2-3 heures pour refactoring complet de tous les logs

---

**Prochaine étape** : Compiler et valider que les exemples corrigés fonctionnent correctement.
