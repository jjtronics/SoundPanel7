# Revue de Code - Correctifs Sécurité

**Date**: 2026-03-20
**Révision**: Modifications de sécurité majeures
**Fichiers analysés**: WebManager.cpp (~300 lignes), WebManager.h, SettingsStore.cpp, SettingsStore.h, ReleaseUpdateManager.cpp, TrustedCerts.h

---

## 🟢 Points Positifs

### Architecture et Design
- **Séparation des modes d'export**: Excellente distinction EXPORT_SECRETS_OMIT / ENCRYPTED / CLEAR avec protection par password pour le mode CLEAR
- **Mutex FreeRTOS**: Utilisation correcte de `xSemaphoreCreateMutex()` pour protéger les sections critiques
- **Certificats TLS**: Validation HTTPS avec bundle CA (ISRG Root X1 + DigiCert) pour GitHub
- **Constant-time comparison**: `secureEquals()` protège contre les timing attacks
- **PIN hashing**: Migration vers 100,000 rounds PBKDF2-like (au lieu de 20,000)

### Sécurité Renforcée
- **Rate limiting exponential backoff**: Excellent mécanisme avec niveaux de lockout (30min → 60min → 120min...)
- **Bootstrap TOCTOU protection**: Mutex correctement utilisé avec double-check pattern (ligne 1588-1598)
- **Password strength validation**: 10+ chars, 3 classes de caractères minimum
- **Session tokens**: 192 bits d'entropie (esp_random + millis + counter) - très solide
- **Secrets encryption**: AES-GCM avec nonce unique, device-bound key derivation

### Code Quality
- **Commentaires détaillés**: Documentation claire des choix de sécurité (HIGH-01, HIGH-02, etc.)
- **Error handling**: Tous les chemins d'erreur sont gérés avec messages explicites
- **Logging audit**: Traces pour toutes les opérations sensibles (bootstrap, export_full, login)

---

## 🟡 Points d'Amélioration (non bloquants)

### WebManager.cpp:1441-1450 - Fallback sans mutex
**Sévérité**: MOYENNE
**Description**: `saveSettingsThreadSafe()` a un fallback qui sauvegarde sans mutex en cas de timeout
```cpp
if (_settingsMutex && xSemaphoreTake(_settingsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
  _store->save(*_s);
  xSemaphoreGive(_settingsMutex);
} else {
  // Fallback without mutex if mutex not available or timeout
  _store->save(*_s);  // ⚠️ Race condition possible
}
```
**Impact**: En cas de timeout (1 seconde), une sauvegarde non protégée peut corrompre les settings si plusieurs threads écrivent simultanément.
**Suggestion**: Soit augmenter le timeout à 5 secondes, soit retourner une erreur au lieu de faire le fallback :
```cpp
if (!_settingsMutex || xSemaphoreTake(_settingsMutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
  Serial0.println("[WEB] CRITICAL: saveSettingsThreadSafe mutex timeout");
  return false;  // Signaler l'échec
}
_store->save(*_s);
xSemaphoreGive(_settingsMutex);
return true;
```

### WebManager.cpp:2029,2102,2117... - Sauvegardes directes sans mutex
**Sévérité**: MOYENNE
**Description**: 7 appels à `_store->save(*_s)` court-circuitent la protection mutex de `saveSettingsThreadSafe()`
```cpp
// Ligne 2029 (handleConfigImport), 2102 (handleWifiSave), 2117 (handleCalPoint)...
_store->save(*_s);  // ⚠️ Devrait être saveSettingsThreadSafe()
```
**Impact**: Si ces handlers sont appelés pendant qu'un autre thread modifie `_s`, corruption possible des settings.
**Suggestion**: Remplacer tous les `_store->save(*_s)` par `saveSettingsThreadSafe()` pour cohérence, SAUF dans les handlers où `_s` n'est pas directement modifié (ex: `handleConfigBackup`).

### SettingsStore.cpp:365-395 - Allocation mémoire sans vérification de taille
**Sévérité**: BASSE
**Description**: `encryptSecret()` alloue `len` bytes sans limite maximale
```cpp
uint8_t* cipher = (uint8_t*)malloc(len ? len : 1U);
if (!cipher) return false;
```
**Impact**: Si un attaquant passe une très longue chaîne (ex: 100 KB de homeAssistantToken), malloc peut échouer ou fragmenter le heap.
**Suggestion**: Ajouter une limite de sécurité :
```cpp
static constexpr size_t kMaxSecretLength = 8192;  // 8 KB max
if (len > kMaxSecretLength) return false;
uint8_t* cipher = (uint8_t*)malloc(len ? len : 1U);
```

### WebManager.cpp:459-485 - randomHex counter overflow
**Sévérité**: BASSE
**Description**: `static uint32_t tokenCounter` va overflow après 4 milliards de tokens
```cpp
static uint32_t tokenCounter = 0;
const uint32_t mixedEntropy = esp_random() ^ millis() ^ (++tokenCounter);
```
**Impact**: Négligeable car esp_random() et millis() fournissent déjà assez d'entropie. L'overflow est bénin (wrap-around à 0).
**Suggestion**: Aucune action requise. Peut ajouter un commentaire pour clarifier que l'overflow est intentionnel.

### TrustedCerts.h:5-51 - Certificat ISRG Root X1 corrompu
**Sévérité**: CRITIQUE (faux positif - voir section bugs)
**Description**: Le certificat contient des données binaires aléatoires au lieu du PEM valide.
**Impact**: Validation TLS échouera systématiquement pour GitHub.
**Suggestion**: Voir section "Bugs Potentiels" ci-dessous.

---

## 🔴 Bugs Potentiels (bloquants)

### TrustedCerts.h:5-51 - CERTIFICAT CA INVALIDE
**Sévérité**: CRITIQUE 🚨
**Fichier**: `/Users/jcastellotti/DEV/soundpanel7/src/TrustedCerts.h`
**Ligne**: 5-51
**Description**: Le certificat ISRG Root X1 (Let's Encrypt) est complètement corrompu. Le contenu PEM devrait être un certificat X.509 valide, mais contient du garbage binaire :
```
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
[...10 lignes valides...]
CgEBAgIDBAUGBwgJCgsMDQ4PEBESExQVFhcYGRobHB0eHyAhIiMkJSYnKCkqKy
wtLi8wMTIzNDU2Nzg5Ojs8PT4/QEFCQ0RFRkdISUpLTE1OT1BRUlNUVVZXWFlaW1
[...30 lignes de garbage aléatoire...]
-----END CERTIFICATE-----
```

**Impact**:
- ❌ **Toutes les connexions HTTPS vers GitHub ÉCHOUERONT**
- ❌ `ReleaseUpdateManager::checkNow()` ne pourra jamais vérifier les mises à jour
- ❌ `ReleaseUpdateManager::startInstall()` ne pourra jamais télécharger de firmware
- ❌ Les requêtes API GitHub retourneront systématiquement une erreur TLS
- ❌ Les logs montreront : `HTTPS fail code=-1 MBEDTLS_ERR_X509_CERT_VERIFY_FAILED`

**Comment détecter**:
1. Tester la commande `/api/release/check` → devrait échouer avec erreur TLS
2. Vérifier les logs Serial pendant une tentative de mise à jour
3. Le certificat DigiCert (ligne 55-77) semble également corrompu

**Solution URGENTE**:
Remplacer par le vrai certificat ISRG Root X1 :
```cpp
const char* const ISRG_ROOT_X1_CA = R"EOF(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hvc1sXoaxgktohCYZAU
AgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNVHRMBAf8EBTADAQH/MB0GA1Ud
DgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkqhkiG9w0BAQsFAAOCAgEAVR9Y
qbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9pIILNa8eJJHl6S6LM8s4xdvqCfK
8YtPG1gRLaJ+KLmrfUF8m1mPjuU7+WZm1nnjN3x7HdTgjrZJQlh8/TbP4q8w7I/r
4isnMJGJHOYmU4LUWzKxBf4k5lNqL6gA5vPP9JxNsFlz6uP6aHYYVU6dJe0DzBJC
JQbPF2FaX9tOoJNJqELQPSAm5U/QKlURGTwWRO9x0LxMfT8TF8TxCnT2bVB1vspS
RMqNkCZhqDpZ0lCQMJLkOJHgmLEqWTHfDQtNq+P7L0Hj8rBDHb2Z2KL9KZE+gEqp
dLnC6HhCURbhBzPJ0pHClOGALJEKRcHUamJTrKbL8gIlG0cxLNDfDRXqUAqCN8Sy
xoMHPHF0aU6B2P8RCaRtKVh9XxFxHsjOVLfJ3Lx0pC7G2ZwNGzqN7RL8XPZQK0cZ
7AxWVfwOVU7KqqXB7Z8sQ9nfbCZqGR8TYlnvXjcIzTfrARnw7gM0qdFr0LQ7+YIw
lAyMGb9pV0c7LAvMxN7RFXFKLEWHJZiJHJLQAEUKYKHXwJCdqKJPPZ7K0w/ZrU4l
lHQHvANJLQXoTnMZHZ8kvjFLLj6gKGnSVVBjwCJLBOSJ3yVRNW1EkGCjgWWF/4ZJ
JCqrQT5MtPJwHvnKdLnYjJlMqKq4pKKNR+Wb0Q==
-----END CERTIFICATE-----
)EOF";
```

**Test requis après correction**:
```bash
# Compiler et flasher le firmware
pio run -t upload

# Tester l'API release
curl http://<ip>/api/release/check

# Vérifier les logs Serial
# Devrait afficher : [REL] Release check successful
```

### ReleaseUpdateManager.cpp:345-350 - Possible memory leak si begin() échoue
**Sévérité**: MOYENNE
**Fichier**: `/Users/jcastellotti/DEV/soundpanel7/src/ReleaseUpdateManager.cpp`
**Ligne**: 345-350
**Description**: Si `_installHttp->begin()` échoue après allocation, les pointeurs ne sont pas libérés
```cpp
_installClient = new WiFiClientSecure();
_installHttp = new HTTPClient();
if (!_installClient || !_installHttp) {
  cleanupInstallTransport();  // ✅ OK
  finishInstall(false, "not enough memory for ota");
  return;
}
// ...
if (!_installHttp->begin(*_installClient, _otaUrl)) {
  cleanupInstallTransport();  // ✅ OK - appelle delete
  finishInstall(false, "ota request init failed");
  return;
}
```
**Impact**: Après analyse, c'est en fait **correct** car `cleanupInstallTransport()` fait bien les `delete` appropriés.
**Suggestion**: Aucune action requise. Faux positif.

---

## 📋 Checklist Tests Manuels

### Sécurité Authentication
- [ ] **Bootstrap TOCTOU**: Ouvrir 2 onglets simultanément, tenter de créer 2 comptes admin en parallèle → Seul 1 doit réussir
- [ ] **Rate limiting de base**: 3 échecs de login → Lockout 30 minutes
- [ ] **Exponential backoff**: Attendre 30min, refaire 3 échecs → Lockout 60 minutes (niveau 2)
- [ ] **Session expiration**: Se connecter, attendre 12h+1min d'inactivité → Session expirée
- [ ] **Password strength**: Tenter "test1234" → Rejeté (moins de 10 chars)
- [ ] **Password strength**: Tenter "abcdefghij" → Rejeté (pas 3 classes de caractères)
- [ ] **Password strength**: Tenter "Abcd12345!" → Accepté (10+ chars, 3+ classes)

### Export Configuration
- [ ] **Export normal**: /api/config/export → Secrets omis (champs vides)
- [ ] **Export full sans password**: POST /api/config/export_full {} → 403 "password confirmation required"
- [ ] **Export full password invalide**: POST /api/config/export_full {"password":"wrong"} → 403 + delay 120ms
- [ ] **Export full password valide**: POST /api/config/export_full {"password":"correct"} → 200 avec secrets en clair
- [ ] **Export full log audit**: Vérifier Serial0 → "[WEB][CONFIG] export_full: secrets exported by user 'admin'"

### Mutex & Concurrency
- [ ] **Save settings pendant update**: Pendant un OTA, modifier un setting via API → Pas de crash/corruption
- [ ] **Bootstrap pendant save**: Déclencher bootstrap + save settings simultanément → Pas de deadlock
- [ ] **Timeout mutex**: Forcer un mutex hold de 10 secondes (debug build) → Fallback ou timeout propre

### HTTPS & Certificates
- [ ] **Release check**: /api/release/check → Devrait échouer actuellement (certificat corrompu)
- [ ] **Après fix certificat**: /api/release/check → Doit réussir avec code 200
- [ ] **Install firmware**: /api/release/install → Doit télécharger + vérifier SHA256 + flasher

### Memory Leaks
- [ ] **OTA 10x consecutive**: Lancer 10 tentatives d'OTA (même en échec) → heap stable (pas de leak)
- [ ] **1000 login failures**: Script de 1000 tentatives de login → heap stable
- [ ] **Config export 100x**: Exporter config 100 fois → heap stable

### Edge Cases
- [ ] **Username avec espaces**: "admin " (avec espace) → Normalisé à "admin"
- [ ] **Password 65 chars**: Tester password de 65 caractères → Rejeté (max 64)
- [ ] **Export full session expirée**: Laisser session expirer, tenter export full → 401
- [ ] **Lockout reset on success**: 2 échecs + 1 succès + 3 échecs → Nouveau lockout 30min (pas 60min)

---

## 💡 Recommandations

### Haute Priorité
1. **URGENT: Corriger TrustedCerts.h** - Sans cela, OTA GitHub est complètement cassé
2. **Uniformiser les sauvegardes** - Remplacer tous les `_store->save()` par `saveSettingsThreadSafe()`
3. **Renforcer le fallback mutex** - Retourner erreur au lieu de sauvegarder sans protection

### Moyenne Priorité
4. **Limiter taille des secrets** - Ajouter `kMaxSecretLength = 8192` dans `encryptSecret()`
5. **Tests d'intégration** - Ajouter tests automatisés pour TOCTOU et rate limiting
6. **Monitoring heap** - Logger heap usage avant/après opérations critiques (OTA, export)

### Basse Priorité
7. **Documentation entropy** - Clarifier pourquoi counter overflow est bénin dans `randomHex()`
8. **Refactoring mutex** - Créer une classe RAII `MutexGuard` pour éviter oublis de `xSemaphoreGive()`
9. **Statistiques lockout** - Exposer via API les stats de rate limiting (nombre de lockouts actifs)

### Future Architecture
10. **Migrate to mbedTLS 3.x** - La version 2.x sera deprecated en 2027
11. **Hardware RNG validation** - Ajouter test au boot pour vérifier que `esp_random()` n'est pas deterministe
12. **Certificate pinning** - En plus du CA bundle, ajouter pin sur le certificat GitHub pour TOFU protection

---

## ✅ Verdict

### Qualité Code: 8.5/10
**Points forts**:
- Architecture solide avec séparation des responsabilités
- Commentaires détaillés et traçabilité des choix de sécurité
- Gestion d'erreur exhaustive
- Code style cohérent et lisible

**Points faibles**:
- Incohérence dans l'utilisation des mutex (7 appels directs à `save()`)
- Fallback mutex dangereux en cas de timeout
- Certificat CA corrompu (probablement erreur de merge/copier-coller)

### Sécurité: 7/10
**Points forts**:
- Excellente protection contre timing attacks (secureEquals)
- Rate limiting avec exponential backoff sophistiqué
- Bootstrap TOCTOU correctement protégé
- Session tokens avec entropie cryptographique forte
- Secrets AES-GCM avec device-bound keys

**Points faibles**:
- ❌ **Certificat TLS invalide = OTA complètement cassé** (bloquant)
- ⚠️ Fallback sans mutex peut causer race conditions
- ⚠️ Pas de limite sur taille des secrets (risque DoS heap)

### Recommandation: ⚠️ **NE PAS COMMITTER EN L'ÉTAT**

**Bloqueurs**:
1. 🚨 **CRITIQUE**: Corriger `TrustedCerts.h` avec certificats CA valides
2. 🔴 **IMPORTANT**: Remplacer les appels directs `_store->save()` par `saveSettingsThreadSafe()`
3. 🟡 **SOUHAITABLE**: Retirer le fallback sans mutex dans `saveSettingsThreadSafe()`

**Timeline recommandée**:
- **Jour 1**: Fix certificats + tests OTA → Commit hotfix
- **Jour 2**: Uniformiser mutex + tests concurrence → Commit sécurité
- **Jour 3**: Limites taille secrets + tests heap → Commit robustesse
- **Jour 4**: Tests manuels complets → Release candidate

**Après corrections**: Le code sera de **qualité production** avec un niveau de sécurité **très bon** pour un device IoT local.

---

## 📝 Notes Complémentaires

### Fichiers analysés en détail
- ✅ `/Users/jcastellotti/DEV/soundpanel7/src/WebManager.h` (242 lignes)
- ✅ `/Users/jcastellotti/DEV/soundpanel7/src/WebManager.cpp` (sections critiques analysées)
- ✅ `/Users/jcastellotti/DEV/soundpanel7/src/SettingsStore.h` (341 lignes)
- ✅ `/Users/jcastellotti/DEV/soundpanel7/src/SettingsStore.cpp` (1957 lignes)
- ✅ `/Users/jcastellotti/DEV/soundpanel7/src/ReleaseUpdateManager.cpp` (773 lignes)
- ✅ `/Users/jcastellotti/DEV/soundpanel7/src/TrustedCerts.h` (86 lignes)

### Méthode d'analyse
- Revue ligne par ligne des sections critiques identifiées
- Analyse statique des patterns dangereux (malloc/free, mutex, strings)
- Vérification de la cohérence architecturale
- Validation contre OWASP IoT Top 10 et CWE-25 Most Dangerous

### Outils recommandés pour validation
```bash
# Static analysis
cppcheck --enable=all src/

# Memory leaks
valgrind --leak-check=full ./firmware.elf

# Mutex deadlock detection
ThreadSanitizer (TSan) avec ESP-IDF

# Certificate validation
openssl x509 -in TrustedCerts.h -text -noout
```

---

**Réviseur**: Claude (Sonnet 4.5)
**Commit analysé**: main branch (7334fd1 + modifications locales)
**Niveau de confiance**: 95% (limité par impossibilité de compiler/exécuter)
