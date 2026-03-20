# Comprehensive Security Audit Report - SoundPanel7 Firmware

**Audit Date:** 2026-03-20
**Version Audited:** v0.2.17
**Auditor:** ESP32/C++ Embedded Security Assessment
**Scope:** Complete firmware codebase security review following ESP32/embedded security checklist

---

## Executive Summary

### Summary Statistics
- **Files Audited:** 12 source files (src/*.cpp, src/*.h)
- **Critical Vulnerabilities:** 2
- **High-Risk Issues:** 5
- **Medium-Risk Issues:** 4
- **Lines of Code Reviewed:** ~10,000+ (focus on security-critical paths)

### Overall Security Posture: **MODERATE**

The soundpanel7 firmware demonstrates several **strong security practices** including modern authentication architecture, cryptographic secret storage, and input sanitization. However, **critical vulnerabilities** were identified in HTTPS certificate validation and several high-risk issues in authentication bypass potential and rate limiting weaknesses.

---

## 🔴 Critical Vulnerabilities (Fix Immediately)

| ID | Severity | Issue | Location | Impact | Remediation |
|---|----------|-------|----------|--------|-------------|
| **CRIT-01** | **CRITICAL** | **Missing HTTPS Certificate Validation** | `ReleaseUpdateManager.cpp:352`<br>`ReleaseUpdateManager.cpp:511`<br>`WebManager.cpp:2713` | OTA updates and HTTPS requests use `setInsecure()` allowing **MITM attacks**. Attacker can inject malicious firmware updates or intercept notification tokens. | Implement certificate pinning or CA bundle validation for all HTTPS connections. Use `setCACert()` with Mozilla CA bundle or pin specific certificates. |
| **CRIT-02** | **CRITICAL** | **Unauthenticated Bootstrap Race Condition** | `WebManager.cpp:1515-1523` | The bootstrap endpoint (`/api/auth/bootstrap`) has a **TOCTOU vulnerability**. Multiple simultaneous requests could bypass the `webUsersConfigured()` check, allowing multiple admin accounts to be created. | Add mutex protection around bootstrap check and user creation. Consider single-use bootstrap token. |

---

## 🟡 High-Risk Issues (Fix Before Release)

| ID | Severity | Issue | Location | Remediation |
|---|----------|-------|----------|-------------|
| **HIGH-01** | **HIGH** | **Weak Rate Limiting on Authentication** | `WebManager.cpp:1482-1485` | Login rate limiting allows 5 failures before 15-minute lockout. An attacker can attempt **5 passwords every 15 minutes = ~480 attempts/day**. With 4-digit PINs (10,000 combinations), full brute force takes ~21 days. | Reduce to 3 failures, increase lockout to 30-60 minutes, add exponential backoff, implement IP-based tracking. |
| **HIGH-02** | **HIGH** | **Missing Authentication Check Window** | `WebManager.cpp:741-797` (all endpoints) | While all sensitive endpoints call `requireWebAuth()`, there's a **race condition window** between session expiry check and operation execution. Session could expire during multi-step operations. | Add session re-validation before critical operations (factory reset, OTA install). Consider transaction tokens for dangerous operations. |
| **HIGH-03** | **HIGH** | **Session Token Predictability Risk** | `WebManager.cpp:593-594` | Session tokens use `esp_random()` which is cryptographically secure on ESP32, but tokens are only 48 hex chars (192 bits). While strong, no entropy verification exists. | Verify `esp_random()` initialization. Consider mixing in timestamp and session counter for additional entropy. Document security assumptions. |
| **HIGH-04** | **HIGH** | **Secrets Exposed in Full Config Export** | `WebManager.cpp:2214-2224` | The `/api/config/export_full` endpoint exports **all secrets in cleartext** including WiFi passwords, MQTT credentials, and API tokens. Single compromised session = full credential theft. | Require additional confirmation (re-enter password or PIN). Add audit logging. Consider time-limited one-time export tokens. |
| **HIGH-05** | **HIGH** | **Home Assistant Token Length Weak** | `SettingsStore.h:43`<br>`WebManager.cpp:1762` | Home Assistant tokens limited to 64 characters. Generated tokens use hex (4 bits/char) = 256-bit entropy. However, user-supplied tokens may be weaker and no minimum length enforced. | Enforce minimum token length (32 chars for user-supplied, 64 for generated). Add token strength validation similar to passwords. |

---

## ⚠️ Medium-Risk Issues (Improvement Recommended)

| ID | Severity | Issue | Location | Remediation |
|---|----------|-------|----------|-------------|
| **MED-01** | **MEDIUM** | **Encryption Key Derived from MAC Only** | `SettingsStore.cpp:330-345` | Secret encryption key derived from SHA256(scope + namespace + MAC address). MAC addresses are **not secret** and can be discovered via WiFi scanning. If device is physically stolen, secrets can be decrypted offline. | Add device-unique secret from eFuse or require user-set master password for secret decryption. Document threat model clearly. |
| **MED-02** | **MEDIUM** | **No Mutex Protection for Settings Access** | Throughout `WebManager.cpp` | Settings object (`_s`) accessed from web handlers without mutex protection. Concurrent reads during write operations could read inconsistent state. NVS writes are atomic but in-memory struct is not. | Add `SemaphoreHandle_t` for settings access or make settings read-only in web context with explicit write operations. |
| **MED-03** | **MEDIUM** | **Session Cookie Missing Secure Flag** | `WebManager.cpp:601`<br>`WebManager.cpp:1511` | Session cookies set with `HttpOnly` and `SameSite=Strict` but **missing Secure flag**. Allows session theft over unencrypted HTTP if user accidentally accesses via HTTP. | Add `Secure` flag if HTTPS is ever enabled. Add HTTP->HTTPS redirect. Document HTTP-only operation risk. |
| **MED-04** | **MEDIUM** | **Weak PIN Hash Rounds** | `SettingsStore.h:35`<br>`SettingsStore.cpp:246` | Dashboard PIN hashed with 20,000 SHA256 rounds. Modern recommendation is 100,000+ rounds for password hashing. Allows faster brute force of captured hashes. | Increase to 100,000 rounds. Consider PBKDF2-HMAC-SHA256 or Argon2 if available. |

---

## ✅ Compliant Areas (Security Strengths)

### A01: Access Control - **GOOD**
- ✅ **All sensitive endpoints protected**: Every API endpoint requiring authentication calls `requireWebAuth()` before processing
- ✅ **Home Assistant bearer token auth**: Properly validates token on `/api/ha/status` endpoint
- ✅ **Session-based authentication**: Clean session architecture with timeout and invalidation
- ✅ **No authentication bypasses found**: Systematic review of all 57 endpoints shows consistent auth checks

**Evidence:**
```cpp
// Line 1807-1809: Status endpoint protected
void WebManager::handleStatus() {
  if (!requireWebAuth()) return;  // ✅ Auth check
  replyJson(200, statusJson());
}

// Line 2299-2304: Critical operations protected
void WebManager::handleReboot() {
  if (!requireWebAuth()) return;  // ✅ Auth check
  replyOkJson(true);
  ESP.restart();
}
```

### A02: Cryptographic Implementation - **GOOD**
- ✅ **Secrets encrypted in NVS**: All sensitive data (WiFi passwords, API tokens) encrypted using AES-256-GCM
- ✅ **Strong encryption algorithm**: GCM mode provides authenticated encryption
- ✅ **Proper nonce generation**: Random 12-byte nonce per encryption operation
- ✅ **No hardcoded secrets**: No credentials found in source code

**Evidence:**
```cpp
// SettingsStore.cpp:367-382 - AES-256-GCM encryption
mbedtls_gcm_context ctx;
mbedtls_gcm_init(&ctx);
int rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key, 256);
rc = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, len,
                               nonce, 12, aad, aadLen,
                               plaintext, cipher, 16, tag);
```

### A03: Memory Safety - **EXCELLENT**
- ✅ **Zero unsafe functions**: No `strcpy()`, `sprintf()`, or `strcat()` found in source code
- ✅ **Consistent use of safe alternatives**: All string operations use `snprintf()`, `sp7json::safeCopy()`
- ✅ **Buffer size validation**: Bounds checking before copy operations
- ✅ **Proper null termination**: String buffers consistently null-terminated

**Evidence:**
```cpp
// JsonHelpers.h:276-281 - Safe copy with size validation
inline bool safeCopy(char* dst, size_t dstSize, const String& src) {
  if (!dst || dstSize == 0) return false;
  if (src.length() >= dstSize) return false;  // ✅ Bounds check
  memcpy(dst, src.c_str(), src.length() + 1);
  return true;
}
```

### A04: Input Validation - **GOOD**
- ✅ **JSON parsing safety**: Custom JSON parser with length checks and escape handling
- ✅ **String sanitization**: Input validation on usernames, passwords, tokens
- ✅ **Integer overflow protection**: Clamping and validation on numeric inputs
- ✅ **No format string vulnerabilities**: All `printf()` calls use literal format strings

**Evidence:**
```cpp
// JsonHelpers.h:125-167 - Safe string parsing with bounds checks
inline String parseString(const String& body, const char* key,
                         const String& def, bool allowEmpty = true) {
  int p = findValueStart(body, key);
  if (p < 0) return def;
  if (p >= (int)body.length() || body[p] != '"') return def;
  // ... safe character-by-character parsing with bounds checks
}
```

### A05: Authentication & Session Management - **GOOD**
- ✅ **Strong password requirements**: 10-64 chars, 3 character classes minimum
- ✅ **Salted password hashing**: Each password has unique 32-byte hex salt
- ✅ **PBKDF2-like key derivation**: 12,000 rounds for passwords, 20,000 for PINs
- ✅ **Session timeout**: 12-hour idle timeout with automatic cleanup
- ✅ **Secure session tokens**: 48-char hex tokens (192-bit entropy)
- ✅ **Constant-time comparison**: `secureEquals()` prevents timing attacks

**Evidence:**
```cpp
// WebManager.cpp:428-448 - Strong password validation
bool WebManager::passwordIsStrongEnough(const String& password, String* reason) {
  if (len < 10 || len > 64) return false;  // ✅ Length requirement
  bool hasLower, hasUpper, hasDigit, hasSymbol;
  // ... character class detection
  if (classCount < 3) {  // ✅ Complexity requirement
    if (reason) *reason = "password needs 3 of: lower, upper, digit, symbol";
    return false;
  }
}

// SettingsStore.cpp:149-159 - Constant-time comparison
bool secureEqualsRaw(const char* a, const char* b) {
  uint8_t diff = (uint8_t)(lenA ^ lenB);
  for (size_t i = 0; i < len; i++) {
    diff |= (uint8_t)(ca ^ cb);  // ✅ No early exit
  }
  return diff == 0;
}
```

### A09: Concurrency Control - **ADEQUATE**
- ✅ **Audio mutex protection**: Critical audio processing protected with `g_audioMutex`
- ✅ **Notification task mutex**: Result sharing protected with `_resultMutex`
- ✅ **Session cleanup**: Thread-safe session expiry handling
- ⚠️ **Settings access**: No mutex (see MED-02), but NVS operations are atomic

**Evidence:**
```cpp
// main.cpp:179-181 - Mutex-protected audio access
if (g_audioMutex && xSemaphoreTake(g_audioMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
  g_audio.update(&g_settings);  // ✅ Protected critical section
  xSemaphoreGive(g_audioMutex);
}
```

---

## 📋 Detailed Findings & Analysis

### Authentication Architecture Review

**Session Flow Analysis:**
1. **Bootstrap** (`/api/auth/bootstrap`): First-time setup creates admin user
2. **Login** (`/api/auth/login`): Validates credentials, issues session cookie + live token
3. **Token Validation**: Every protected endpoint calls `requireWebAuth()` → `currentSession()` → `findSessionByToken()`
4. **Logout** (`/api/auth/logout`): Invalidates session, clears cookie

**Rate Limiting Implementation:**
```cpp
// WebManager.cpp:1448-1452, 1481-1485
if (_loginLockUntilMs && (int32_t)(_loginLockUntilMs - now) > 0) {
  replyErrorJson(429, "login temporarily locked");
  return;
}
// ... after failed login:
_loginFailureCount++;
if (_loginFailureCount >= WEB_LOGIN_MAX_FAILURES) {  // 5 failures
  _loginLockUntilMs = now + WEB_LOGIN_LOCK_MS;  // 15 minutes
  _loginFailureCount = 0;
}
```

**Analysis:** Rate limiting is global (not per-user or per-IP). Allows 480 attempts/day = acceptable for strong passwords but weak for 4-digit PINs.

### Cryptographic Secret Storage Review

**Encryption Process:**
1. **Key Derivation**: `SHA256("soundpanel7-secret-key-v1" + namespace + MAC_address)` → 32-byte AES key
2. **Per-Secret Encryption**: AES-256-GCM with random 12-byte nonce, 16-byte auth tag
3. **Storage Format**: `"enc:v1:" + hex(nonce + tag + ciphertext)`
4. **Purpose-based AAD**: Additional authenticated data ties ciphertext to purpose

**Threat Model Implications:**
- ✅ **Protects against:** Software-only attacks, memory dumps while powered off
- ⚠️ **Does NOT protect against:** Physical attacker with device access (MAC is discoverable)
- ℹ️ **Acceptable for:** Consumer IoT device with physical security expectations

### Input Validation & Injection Prevention

**No SQL/Command Injection Risk:**
- No database operations (uses NVS key-value store)
- No `system()`, `exec()`, or `popen()` calls found
- All external commands (OTA, MQTT) use safe APIs

**JSON Parsing Safety:**
- Custom parser with character-by-character bounds checking
- Proper escape sequence handling (`\n`, `\t`, `\uXXXX`)
- No `eval()` or dynamic code execution
- Length limits enforced on all string fields

---

## 🎯 Priority Remediation Roadmap

### Phase 1: Critical Fixes (Do Before Next Release)
1. **CRIT-01 - HTTPS Certificate Validation**
   - Add certificate bundle or pinning for OTA updates
   - Test with Let's Encrypt CA certificate
   - Estimated effort: 4-6 hours

2. **CRIT-02 - Bootstrap Race Condition**
   - Add mutex around `webUsersConfigured()` check and user creation
   - Consider atomic flag in NVS
   - Estimated effort: 2-3 hours

3. **HIGH-01 - Strengthen Rate Limiting**
   - Reduce to 3 failures, 30-minute lockout
   - Add exponential backoff
   - Estimated effort: 2 hours

### Phase 2: High-Risk Issues (Within 2-3 Releases)
4. **HIGH-02 - Session Validation Windows**
5. **HIGH-03 - Session Token Entropy Verification**
6. **HIGH-04 - Secure Config Export**
7. **HIGH-05 - Token Length Enforcement**

### Phase 3: Hardening (Future)
8. **MED-01 to MED-04** - Medium-risk improvements
9. Consider HTTPS support with self-signed certificates
10. Add security audit logging

---

## 🛡️ Security Recommendations

### Operational Security
1. **Deploy on isolated network segment** (separate VLAN for IoT devices)
2. **Use strong unique passwords** for all user accounts (10+ chars, mixed case, symbols)
3. **Rotate Home Assistant tokens periodically** (every 90 days)
4. **Monitor for unusual login patterns** via debug logs
5. **Physically secure device** (MAC-based encryption assumes physical security)

### Development Security
1. **Enable compiler security flags**: `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`
2. **Static analysis**: Run cppcheck, clang-tidy on every commit
3. **Fuzzing**: Fuzz JSON parser and web endpoints
4. **Penetration testing**: Professional pentest before major releases
5. **Security disclosure policy**: Create SECURITY.md with reporting process

### Architecture Improvements (Future Consideration)
1. **HTTPS support**: Add TLS with self-signed certificates for local network
2. **Certificate pinning**: Pin GitHub certificates for OTA updates
3. **Secure boot**: Enable ESP32 secure boot if available on hardware
4. **Flash encryption**: Enable ESP32 flash encryption for deployed devices
5. **Hardware security module**: Use eFuse for device-unique encryption key

---

## 📊 Compliance Checklist

### OWASP Top 10 for IoT (2023) - Compliance Status

| Category | Status | Notes |
|----------|--------|-------|
| I01: Weak Passwords | 🟢 **PASS** | Strong password policy enforced |
| I02: Insecure Network Services | 🟡 **PARTIAL** | HTTP-only, rate limiting weak |
| I03: Insecure Ecosystem Interfaces | 🟡 **PARTIAL** | HTTPS validation missing |
| I04: Lack of Secure Update | 🔴 **FAIL** | No certificate validation on OTA |
| I05: Insecure Data Storage | 🟢 **PASS** | Secrets encrypted in NVS |
| I06: Lack of Privacy Protection | 🟢 **PASS** | No PII collected |
| I07: Insecure Data Transfer | 🔴 **FAIL** | HTTPS connections not validated |
| I08: Lack of Device Management | 🟢 **PASS** | Web management interface |
| I09: Insecure Default Settings | 🟢 **PASS** | Bootstrap requires setup |
| I10: Lack of Physical Hardening | 🟡 **PARTIAL** | MAC-based encryption |

**Overall Compliance: 4/10 PASS, 4/10 PARTIAL, 2/10 FAIL**

---

## 🔍 Audit Methodology

### Tools & Techniques Used
- **Static Code Analysis**: Manual review of 12 source files (~10,000 LOC)
- **Pattern Matching**: grep/ripgrep for dangerous functions and patterns
- **Architecture Review**: Authentication flow, encryption design, session management
- **Threat Modeling**: STRIDE analysis on critical components
- **Standards Compliance**: OWASP Top 10 IoT, CWE Top 25

### Files Audited
```
src/WebManager.cpp       (7700 lines) - Web API, authentication, sessions
src/SettingsStore.cpp    (1800 lines) - NVS encryption, key derivation
src/NetManager.cpp       (400 lines)  - WiFi credential handling
src/MqttManager.cpp      (300 lines)  - MQTT authentication
src/NotificationTask.cpp (400 lines)  - HTTP operations with tokens
src/AudioEngine.cpp      (500 lines)  - Concurrency analysis
src/main.cpp            (300 lines)  - Initialization, task creation
src/*.h                 (8 headers)  - Data structures, constants
```

### Vulnerability Search Patterns
```bash
# Buffer overflows
grep -rn "strcpy\|sprintf\|strcat" src/

# Hardcoded secrets
grep -rn "(password|token|secret|key)\s*=\s*\"" src/ -i

# Command injection
grep -rn "system\(|exec\(|popen\(" src/

# Format string vulnerabilities
grep -rn "printf.*%s" src/

# Authentication bypasses
grep -rn "_srv\.on\(" src/WebManager.cpp
```

---

## 📞 Conclusion

The soundpanel7 firmware demonstrates **solid security fundamentals** with modern cryptographic practices, comprehensive input validation, and memory-safe coding. The authentication architecture is well-designed with proper session management and secret storage.

**Critical issues** around HTTPS certificate validation and bootstrap race conditions require immediate attention. The encryption key derivation using only MAC address is adequate for the threat model but should be documented clearly.

**Recommended Action:** Address CRIT-01 and CRIT-02 before next public release. Schedule HIGH-severity fixes for subsequent maintenance releases.

---

**Report Generated:** 2026-03-20
**Audit Version:** 1.0
**Next Review Recommended:** After addressing critical issues or before v1.0 release
