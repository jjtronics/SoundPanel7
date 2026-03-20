# Agent Sécurité - SoundPanel 7

Guide pour l'agent chargé d'auditer et renforcer la sécurité du firmware embarqué ESP32-S3.

## Votre rôle

Identifier et corriger les vulnérabilités de sécurité, protéger les données sensibles, auditer les points d'entrée (API, MQTT, OTA) et garantir la conformité aux bonnes pratiques de sécurité embarquée.

## Contexte sécurité

SoundPanel 7 est un dispositif IoT connecté exposant :
- **Interface Web** (port 80) : API REST + SSE live stream (port 81)
- **MQTT** : Commandes entrantes + publications sortantes
- **OTA** : Mise à jour firmware (réseau port 3232 + GitHub)
- **WiFi** : Multi-AP avec credentials stockés en NVS
- **mDNS** : Découverte réseau (`soundpanel7.local`)

**Modèle de menace** :
- Attaque locale (réseau WiFi compromis)
- Injection via API Web
- Commandes MQTT malveillantes
- OTA firmware malicieux
- Fuite de credentials (WiFi, MQTT)

**Hors scope** :
- Sécurité physique (accès matériel → compromission totale assumée)
- DoS hardware (reset permanent)
- Attaques cryptographiques avancées (pas de HSM sur ESP32-S3)

## Checklist OWASP Top 10 (IoT)

### 1. Mot de passe faible / Hardcodé

**Risque** : Credentials par défaut ou hardcodés permettent accès non autorisé.

**Audit** :
```bash
# Recherche de passwords hardcodés
grep -ri "password.*=" src/ include/
grep -ri "token.*=" src/ include/
```

**État actuel** :
- ✅ Pas de password par défaut dans le code
- ✅ WiFi credentials stockés en NVS (chiffré matériel)
- ✅ MQTT credentials configurables via interface
- ⚠️ OTA password hardcodé : `SoundPanel7` (acceptable pour réseau local)
- ✅ Web auth multi-utilisateurs avec credentials en NVS

**Recommandations** :
- [ ] Forcer changement password à la première connexion web
- [ ] Complexité minimum : 8 caractères (actuellement non vérifié)
- [ ] Rate limiting sur tentatives de connexion web

### 2. Services réseau non sécurisés

**Risque** : Communication en clair interceptable (MITM).

**État actuel** :
- ❌ Web HTTP (pas HTTPS)
- ❌ MQTT sans TLS (optionnel selon broker)
- ❌ OTA réseau non chiffré (espota)
- ✅ OTA GitHub avec vérification SHA-256

**Acceptable pour** :
- Usage réseau local privé
- Environnement domotique domestique

**Recommandations production** :
```cpp
// Activer HTTPS (si certificats disponibles)
#ifdef SOUNDPANEL7_ENABLE_HTTPS
  AsyncWebServer _server(443);
  _server.onSslFileRequest(...);
#endif

// MQTT avec TLS
if (settings.mqttUseTls) {
  WiFiClientSecure client;
  client.setCACert(settings.mqttCaCert);
}
```

### 3. Interfaces réseau non sécurisées

**Risque** : API exploitable sans authentification.

**État actuel** :
✅ **Authentification requise** sur endpoints sensibles :
```cpp
void WebManager::handleApiReboot(AsyncWebServerRequest* request) {
  if (!checkAuth(request)) return;  // Vérifie Basic Auth
  // ...
}
```

**Endpoints publics (intentionnel)** :
- `GET /` : Landing page (OK)
- `GET /api/status` : État public (pas de secret)

**Endpoints protégés** :
- `POST /api/reboot` ✅
- `POST /api/factory_reset` ✅
- `POST /api/config/import` ✅
- `POST /api/wifi` ✅
- `POST /api/mqtt` ✅

**Audit endpoints** :
```bash
# Lister tous les endpoints
grep -r "\.on(" src/WebManager.cpp | awk -F'"' '{print $2}' | sort
```

**Vérifier** : Chaque endpoint modifiant l'état doit appeler `checkAuth()`.

### 4. Manque de mécanisme de mise à jour sécurisée

**Risque** : Firmware malveillant installé via OTA.

**État actuel** :
- ✅ OTA GitHub avec SHA-256 checksum vérifié
- ✅ Rollback automatique si boot échoue (ESP32 dual-OTA)
- ⚠️ OTA réseau (espota) sans signature

**Code de vérification SHA-256** :
```cpp
// ReleaseUpdateManager.cpp
bool ReleaseUpdateManager::verifyDownload() {
  const String expectedSha = _latestRelease.sha256;
  const String computedSha = computeSHA256(_downloadedFile);

  if (expectedSha != computedSha) {
    Serial0.println("[OTA] SHA-256 mismatch!");
    return false;
  }
  return true;
}
```

**Recommandations** :
- [ ] Ajouter signature GPG/RSA (si espace flash disponible)
- [ ] Désactiver OTA réseau en production (build flag)
- [ ] Rate limiting sur tentatives OTA échouées

### 5. Données sensibles non protégées

**Risque** : Fuite de credentials en logs ou stockage.

**Audit** :
```bash
# Vérifier que passwords ne sont pas loggés
grep -r "Serial.*password" src/
grep -r "printf.*password" src/
```

**État actuel** :
- ✅ WiFi passwords stockés en NVS chiffré (Flash Encryption ESP32)
- ✅ Pas de password en logs série
- ✅ Export config masque les passwords (remplacés par `***`)

**Code de masquage** :
```cpp
// SettingsStore.cpp
String SettingsStore::toJsonFull(const SettingsV1* s, bool maskSecrets) {
  // ...
  if (maskSecrets) {
    json += "\"wifiPassword\":\"***\",";
    json += "\"mqttPassword\":\"***\",";
  } else {
    json += "\"wifiPassword\":\"" + String(s->wifiPassword) + "\",";
  }
}
```

**Vérifier** : Export API utilise `maskSecrets=true` par défaut.

### 6. Manque de protection du stockage

**Risque** : Lecture NVS en dump flash.

**État actuel** :
- ✅ NVS chiffré matériellement (si Flash Encryption activé)
- ⚠️ Flash Encryption non activé par défaut (nécessite config eFuse)

**Activer Flash Encryption** (one-time, irréversible) :
```bash
# Via esptool (ATTENTION: irréversible)
esptool.py --port /dev/ttyUSB0 burn_efuse FLASH_CRYPT_CNT
```

**Recommandations production** :
- [ ] Documenter procédure Flash Encryption pour déploiement sensible
- [ ] Ajouter flag build `SOUNDPANEL7_REQUIRE_ENCRYPTION`

### 7. Validation d'entrées insuffisante

**Risque** : Injection, buffer overflow, crash.

**Points d'entrée** :
1. **API Web** : Paramètres POST/GET
2. **MQTT** : Payload commands
3. **Import config** : JSON parsing

**Audit validation** :

#### API Web
```cpp
// ❌ Pas de validation
void handleApiWifi(AsyncWebServerRequest* req) {
  String ssid = req->getParam("ssid", true)->value();
  strcpy(settings.wifiSsid, ssid.c_str());  // Buffer overflow possible!
}

// ✅ Avec validation
void handleApiWifi(AsyncWebServerRequest* req) {
  if (!req->hasParam("ssid", true)) {
    req->send(400, "application/json", "{\"error\":\"Missing ssid\"}");
    return;
  }

  String ssid = req->getParam("ssid", true)->value();

  if (ssid.length() > WIFI_SSID_MAX_LENGTH) {
    req->send(400, "application/json", "{\"error\":\"SSID too long\"}");
    return;
  }

  strncpy(settings.wifiSsid, ssid.c_str(), WIFI_SSID_MAX_LENGTH);
  settings.wifiSsid[WIFI_SSID_MAX_LENGTH] = '\0';  // Null-terminate
}
```

**Checklist validation** :
- [ ] Length check sur tous les String inputs
- [ ] Range check sur valeurs numériques (brightness 0-100, etc.)
- [ ] Sanitization caractères spéciaux (pas d'injection)
- [ ] Null-termination buffers C-strings

#### MQTT Commands
```cpp
// MqttManager.cpp
void MqttManager::handleMessage(char* topic, uint8_t* payload, unsigned int length) {
  // Validation length
  if (length > 256) {
    Serial0.println("[MQTT] Payload too large, ignored");
    return;
  }

  String body;
  body.reserve(length);
  for (unsigned int i = 0; i < length; i++) {
    body += (char)payload[i];
  }
  body.trim();
  body.toUpperCase();

  // Validation valeur
  if (body != "ON" && body != "OFF" && body != "1" && body != "0") {
    Serial0.printf("[MQTT] Invalid payload: %s\n", body.c_str());
    return;
  }
}
```

**État actuel** : ✅ Validation présente dans MQTT, ⚠️ vérifier tous les handlers Web.

#### Import Config JSON
```cpp
// SettingsStore.cpp
bool SettingsStore::fromJson(const char* json, SettingsV1* out) {
  // Validation structure JSON
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, json);

  if (error) {
    Serial0.printf("[SETTINGS] JSON parse error: %s\n", error.c_str());
    return false;
  }

  // Validation valeurs
  if (doc["backlight"].as<uint8_t>() > 100) {
    Serial0.println("[SETTINGS] Invalid backlight value");
    return false;
  }

  // Validation strings
  const char* hostname = doc["hostname"] | "soundpanel7";
  if (strlen(hostname) >= sizeof(out->hostname)) {
    Serial0.println("[SETTINGS] Hostname too long");
    return false;
  }

  strncpy(out->hostname, hostname, sizeof(out->hostname) - 1);
  out->hostname[sizeof(out->hostname) - 1] = '\0';
}
```

**État actuel** : ✅ Parsing JSON sécurisé, ⚠️ vérifier validation complète des ranges.

### 8. Gestion de code obsolète

**Risque** : Bibliothèques avec CVE connus.

**Audit dépendances** :
```bash
# Vérifier versions libraries
pio pkg list

# Identifier CVE connus
# Consulter: https://cve.mitre.org/
```

**Dépendances critiques** :
- `ESPAsyncWebServer` : vérifier upstream régulièrement
- `PubSubClient` : stable, maintenu
- `WiFiManager` : stable
- `ArduinoJson` : bien maintenu, vérifier version

**Recommandations** :
- [ ] Update régulier des dépendances (mensuel)
- [ ] Pin versions exactes dans `platformio.ini`
- [ ] Tester après chaque update

### 9. Journalisation de sécurité insuffisante

**Risque** : Incidents non détectés, forensics impossible.

**État actuel** :
- ✅ Logs série pour debug (115200 bauds)
- ❌ Pas de logs syslog distant
- ❌ Pas de détection d'intrusion

**Logs de sécurité à ajouter** :
```cpp
// Tentatives de connexion échouées
Serial0.printf("[SEC][WARN] Failed auth attempt from IP=%s user=%s\n",
               request->client()->remoteIP().toString().c_str(),
               username.c_str());

// Commandes sensibles
Serial0.printf("[SEC][INFO] Factory reset initiated by IP=%s\n",
               request->client()->remoteIP().toString().c_str());

// Changement de credentials
Serial0.printf("[SEC][INFO] WiFi credentials changed\n");
```

**Recommandations** :
- [ ] Logger toutes les actions sensibles (reboot, reset, config change)
- [ ] Rate limiting + ban temporaire après N échecs auth
- [ ] Syslog distant optionnel (UDP vers serveur log)

### 10. Manque de durcissement physique

**Risque** : Accès UART/JTAG permet dump firmware.

**État actuel** :
- ⚠️ UART0 (Serial) actif pour debug (115200 bauds)
- ⚠️ JTAG non désactivé (accessible via GPIO)

**Durcissement production** :
```cpp
// Désactiver Serial en production
#ifndef SOUNDPANEL7_DEBUG
  #define Serial0 if(0) Serial
#endif

// Désactiver JTAG (eFuse, irréversible)
// Via menuconfig ESP-IDF: Security features → Disable JTAG
```

**Recommandations** :
- [ ] Build flag `SOUNDPANEL7_PRODUCTION` désactive Serial
- [ ] Documenter procédure désactivation JTAG pour déploiement critique

## Audit endpoints API

### Endpoints publics (pas d'auth)

| Endpoint | Méthode | Risque | Justification |
|----------|---------|--------|---------------|
| `/` | GET | Faible | Landing page |
| `/api/status` | GET | Faible | État public (pas de secret) |
| `/api/ha/status` | GET | Faible | Home Assistant Discovery |

### Endpoints protégés (auth requise)

| Endpoint | Méthode | Validation | Action |
|----------|---------|------------|--------|
| `/api/reboot` | POST | ✅ | Redémarrage |
| `/api/shutdown` | POST | ✅ | Extinction |
| `/api/factory_reset` | POST | ✅ | Réinitialisation |
| `/api/config/export` | GET | ✅ | Export settings |
| `/api/config/import` | POST | ✅ | Import settings |
| `/api/wifi` | POST | ⚠️ | Config WiFi (valider length) |
| `/api/mqtt` | POST | ⚠️ | Config MQTT (valider length) |
| `/api/audio` | POST | ⚠️ | Config audio (valider range) |
| `/api/calibrate` | POST | ✅ | Calibration audio |

**Action** : Audit complet validation dans WebManager.cpp.

## Checklist avant production

### Configuration

- [ ] Flash Encryption activé (si déploiement sensible)
- [ ] JTAG désactivé (eFuse)
- [ ] Serial disabled (build flag `SOUNDPANEL7_PRODUCTION`)
- [ ] WiFi password complexe (min 8 char, alphanum + special)
- [ ] MQTT username/password configurés (pas de anonymous)
- [ ] Web auth multi-utilisateurs avec passwords forts

### Code

- [ ] Tous les endpoints sensibles appellent `checkAuth()`
- [ ] Validation length sur tous les String inputs
- [ ] Validation range sur tous les int inputs
- [ ] Pas de password en logs
- [ ] Export config masque les secrets
- [ ] OTA GitHub vérifie SHA-256
- [ ] MQTT payload validé avant traitement

### Tests

- [ ] Fuzzing API avec payloads malformés
- [ ] Tentatives auth invalides (rate limiting)
- [ ] Import config avec JSON malveillant
- [ ] MQTT commands hors format
- [ ] Buffer overflow test (long strings)

## Outils de test sécurité

### Fuzzing API

```bash
# Test avec curl
for i in {1..100}; do
  # Payload trop long
  curl -X POST http://soundpanel7.local/api/wifi \
    -d "ssid=$(python3 -c 'print("A"*1000)')"

  # Caractères spéciaux
  curl -X POST http://soundpanel7.local/api/wifi \
    -d "ssid=<script>alert(1)</script>"

  # Injection SQL (N/A mais tester)
  curl -X POST http://soundpanel7.local/api/wifi \
    -d "ssid=' OR 1=1--"
done
```

### Rate limiting test

```bash
# Tentatives auth répétées
for i in {1..50}; do
  curl -u wrong:password http://soundpanel7.local/api/reboot
  echo "Attempt $i"
  sleep 0.1
done

# Vérifier: doit être bloqué après N tentatives
```

### MQTT injection

```bash
# Payload trop long
mosquitto_pub -h BROKER -t "soundpanel7/live/set" \
  -m "$(python3 -c 'print("A"*10000)')"

# Commande invalide
mosquitto_pub -h BROKER -t "soundpanel7/live/set" \
  -m "'; DROP TABLE users;--"
```

## Détection d'anomalies

### Métriques à surveiller

```cpp
// Compteurs d'anomalies
static uint16_t g_authFailures = 0;
static uint16_t g_malformedRequests = 0;
static uint32_t g_lastResetRequest = 0;

void detectAnomalies() {
  // Tentatives auth suspectes
  if (g_authFailures > 10) {
    Serial0.println("[SEC][ALERT] Brute force attack suspected");
    // Action: ban IP temporaire, alert admin
  }

  // Requêtes malformées
  if (g_malformedRequests > 50) {
    Serial0.println("[SEC][ALERT] Fuzzing attack suspected");
  }

  // Resets répétés
  if (millis() - g_lastResetRequest < 60000) {
    Serial0.println("[SEC][WARN] Multiple reset requests");
  }
}
```

## Recommandations par environnement

### Environnement domestique (usage typique)

✅ **Acceptable** :
- HTTP (pas HTTPS)
- MQTT sans TLS
- OTA réseau activé
- Serial debug activé
- WiFi AP ouvert temporairement

⚠️ **Recommandé** :
- Web auth activé
- MQTT avec username/password
- Réseau WiFi privé (pas de guest)

### Environnement professionnel

✅ **Requis** :
- HTTPS avec certificat
- MQTT avec TLS
- OTA réseau désactivé
- Flash Encryption activé
- JTAG désactivé
- Serial désactivé (production build)
- Rate limiting sur API
- Logs syslog distants

### Environnement critique (rare)

✅ **Requis** :
- Tout environnement professionnel +
- Signature firmware GPG/RSA
- HSM externe pour secrets
- Réseau isolé (VLAN dédié)
- Monitoring IDS/IPS
- Audit logs immutable

## Template de rapport de sécurité

```markdown
## Security Audit Report - YYYY-MM-DD

### Firmware
- Version: v0.2.17
- Build: soundpanel7_usb
- Environment: Domestic

### Vulnerabilities identified
- [MEDIUM] WiFi credentials length not validated (line 142)
- [LOW] Serial debug enabled in production
- [INFO] HTTPS not enabled (acceptable for domestic)

### Remediation
- [ ] Add length validation in handleApiWifi()
- [ ] Create production build flag to disable Serial
- [ ] Document HTTPS setup procedure

### Compliance
- OWASP IoT Top 10: 8/10 compliant
- Flash Encryption: Not enabled (not required for domestic)
- Rate limiting: Not implemented (to add)

### Risk assessment
- Overall risk: LOW (domestic environment)
- Network attack surface: MEDIUM (HTTP/MQTT exposed)
- Physical attack surface: HIGH (UART/JTAG accessible)
```

## Ressources

- OWASP IoT Top 10: https://owasp.org/www-project-internet-of-things/
- ESP32 Security: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/index.html
- Flash Encryption: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/flash-encryption.html
- Secure Boot: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/security/secure-boot-v2.html
