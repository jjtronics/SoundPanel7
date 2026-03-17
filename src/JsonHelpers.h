#pragma once

#include <Arduino.h>
#include <cstring>

namespace sp7json {

inline bool isJsonWhitespace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline bool appendUtf8Codepoint(String& out, uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    out += (char)codepoint;
    return true;
  }
  if (codepoint <= 0x7FF) {
    out += (char)(0xC0 | ((codepoint >> 6) & 0x1F));
    out += (char)(0x80 | (codepoint & 0x3F));
    return true;
  }
  if (codepoint <= 0xFFFF) {
    out += (char)(0xE0 | ((codepoint >> 12) & 0x0F));
    out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out += (char)(0x80 | (codepoint & 0x3F));
    return true;
  }
  if (codepoint <= 0x10FFFF) {
    out += (char)(0xF0 | ((codepoint >> 18) & 0x07));
    out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out += (char)(0x80 | (codepoint & 0x3F));
    return true;
  }
  return false;
}

inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
  if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
  return -1;
}

inline int findValueStart(const String& body, const char* key) {
  if (!key || !key[0]) return -1;

  const String quotedKey = String("\"") + key + "\"";
  int searchFrom = 0;
  while (searchFrom >= 0 && searchFrom < (int)body.length()) {
    const int keyPos = body.indexOf(quotedKey, searchFrom);
    if (keyPos < 0) return -1;

    int p = keyPos + quotedKey.length();
    while (p < (int)body.length() && isJsonWhitespace(body[p])) p++;
    if (p >= (int)body.length() || body[p] != ':') {
      searchFrom = keyPos + 1;
      continue;
    }

    p++;
    while (p < (int)body.length() && isJsonWhitespace(body[p])) p++;
    return p;
  }
  return -1;
}

inline int parseInt(const String& body, const char* key, int def) {
  int p = findValueStart(body, key);
  if (p < 0) return def;

  bool neg = false;
  if (p < (int)body.length() && body[p] == '-') {
    neg = true;
    p++;
  }

  long v = 0;
  bool ok = false;
  while (p < (int)body.length()) {
    char c = body[p];
    if (c < '0' || c > '9') break;
    ok = true;
    v = v * 10 + (c - '0');
    p++;
  }

  if (!ok) return def;
  return neg ? (int)-v : (int)v;
}

inline float parseFloat(const String& body, const char* key, float def) {
  int p = findValueStart(body, key);
  if (p < 0) return def;

  bool neg = false;
  if (p < (int)body.length() && body[p] == '-') {
    neg = true;
    p++;
  }

  String num;
  bool dotSeen = false;
  while (p < (int)body.length()) {
    char c = body[p];
    if (c >= '0' && c <= '9') {
      num += c;
      p++;
      continue;
    }
    if (c == '.' && !dotSeen) {
      dotSeen = true;
      num += c;
      p++;
      continue;
    }
    break;
  }

  if (num.isEmpty()) return def;
  float v = num.toFloat();
  return neg ? -v : v;
}

inline String parseString(const String& body, const char* key, const String& def, bool allowEmpty = true) {
  int p = findValueStart(body, key);
  if (p < 0) return def;
  if (p >= (int)body.length() || body[p] != '"') return def;
  p++;

  String out;
  while (p < (int)body.length()) {
    char c = body[p++];
    if (c == '\\' && p < (int)body.length()) {
      char n = body[p++];
      if (n == '"' || n == '\\' || n == '/') out += n;
      else if (n == 'n') out += '\n';
      else if (n == 'r') out += '\r';
      else if (n == 't') out += '\t';
      else if (n == 'b') out += '\b';
      else if (n == 'f') out += '\f';
      else if (n == 'u' && (p + 4) <= (int)body.length()) {
        uint32_t codepoint = 0;
        bool ok = true;
        for (int i = 0; i < 4; ++i) {
          const int nibble = hexNibble(body[p + i]);
          if (nibble < 0) {
            ok = false;
            break;
          }
          codepoint = (codepoint << 4) | (uint32_t)nibble;
        }
        if (ok) {
          appendUtf8Codepoint(out, codepoint);
          p += 4;
        } else {
          out += 'u';
        }
      } else out += n;
      continue;
    }
    if (c == '"') break;
    out += c;
  }

  if (!allowEmpty && out.isEmpty()) return def;
  return out;
}

inline bool parseBool(const String& body, const char* key, bool def) {
  int p = findValueStart(body, key);
  if (p < 0) return def;
  if (body.startsWith("true", p)) return true;
  if (body.startsWith("false", p)) return false;
  if (p < (int)body.length() && body[p] == '1') return true;
  if (p < (int)body.length() && body[p] == '0') return false;
  return def;
}

template <size_t N>
inline bool parseFloatArray(const String& body, const char* key, float (&out)[N]) {
  int p = findValueStart(body, key);
  if (p < 0) return false;
  if (p >= (int)body.length() || body[p] != '[') return false;
  p++;

  size_t count = 0;
  while (count < N) {
    while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p < (int)body.length() && body[p] == ']') break;

    bool neg = false;
    if (p < (int)body.length() && body[p] == '-') {
      neg = true;
      p++;
    }

    String num;
    bool dotSeen = false;
    while (p < (int)body.length()) {
      char c = body[p];
      if (c >= '0' && c <= '9') {
        num += c;
        p++;
        continue;
      }
      if (c == '.' && !dotSeen) {
        dotSeen = true;
        num += c;
        p++;
        continue;
      }
      break;
    }

    if (num.isEmpty()) return false;
    out[count] = neg ? -num.toFloat() : num.toFloat();
    count++;

    while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
    if (p < (int)body.length() && body[p] == ',') {
      p++;
      continue;
    }
    break;
  }

  while (p < (int)body.length() && (body[p] == ' ' || body[p] == '\t')) p++;
  return count > 0 && p < (int)body.length() && body[p] == ']';
}

template <size_t N>
inline bool parseU8Array(const String& body, const char* key, uint8_t (&out)[N]) {
  float vals[N] = {};
  if (!parseFloatArray(body, key, vals)) return false;
  for (size_t i = 0; i < N; i++) out[i] = vals[i] > 0.5f ? 1 : 0;
  return true;
}

inline String escape(const char* src) {
  String out;
  if (!src) return out;
  for (size_t i = 0; src[i]; i++) {
    const unsigned char c = (unsigned char)src[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[7];
          snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
          out += buf;
        } else {
          out += (char)c;
        }
        break;
    }
  }
  return out;
}

inline void appendEscapedField(String& json, const char* key, const char* value, bool trailingComma = true) {
  json += "\"";
  json += key;
  json += "\":\"";
  json += escape(value);
  json += "\"";
  if (trailingComma) json += ",";
}

inline bool safeCopy(char* dst, size_t dstSize, const String& src) {
  if (!dst || dstSize == 0) return false;
  if (src.length() >= dstSize) return false;
  memcpy(dst, src.c_str(), src.length() + 1);
  return true;
}

}  // namespace sp7json
