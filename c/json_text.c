/* Unescape a JSON string interior, check UTF-8, and encode one row.
   A general JSON parser is not this file. body.muro classifies the
   post body. These bytes are the host's row and the text it stores. */

#include "json_text.h"

#include <stdint.h>
#include <string.h>

static int hexv(uint8_t c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
  if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
  return -1;
}

static int emit_cp(uint8_t *dst, size_t cap, size_t *o, unsigned cp) {
  if (cp < 0x80u) {
    if (*o >= cap) return -1;
    dst[(*o)++] = (uint8_t)cp;
    return 0;
  }
  if (cp < 0x800u) {
    if (*o + 2 > cap) return -1;
    dst[(*o)++] = (uint8_t)(0xC0u | (cp >> 6));
    dst[(*o)++] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 0;
  }
  if (*o + 3 > cap) return -1;
  dst[(*o)++] = (uint8_t)(0xE0u | (cp >> 12));
  dst[(*o)++] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
  dst[(*o)++] = (uint8_t)(0x80u | (cp & 0x3Fu));
  return 0;
}

int json_unescape(const uint8_t *src, size_t n, uint8_t *dst, size_t cap, size_t *out_n) {
  size_t i = 0;
  size_t o = 0;
  if (!out_n) return -1;
  if (n && !src) return -1;
  while (i < n) {
    uint8_t c = src[i];
    if (c == '\\') {
      uint8_t e;
      if (i + 1 >= n) return -1;
      e = src[i + 1];
      if (e == '"' || e == '\\' || e == '/') {
        if (o >= cap) return -1;
        dst[o++] = e;
        i += 2;
      } else if (e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') {
        if (o >= cap) return -1;
        dst[o++] = (uint8_t)(e == 'b' ? 0x08 : e == 'f' ? 0x0c : e == 'n' ? '\n' : e == 'r' ? '\r' : '\t');
        i += 2;
      } else if (e == 'u') {
        unsigned cp = 0;
        int k;
        if (i + 5 >= n) return -1;
        for (k = 0; k < 4; k++) {
          int h = hexv(src[i + 2 + (size_t)k]);
          if (h < 0) return -1;
          cp = (cp << 4) | (unsigned)h;
        }
        if (cp >= 0xD800u && cp <= 0xDFFFu) return -1;
        if (emit_cp(dst, cap, &o, cp) < 0) return -1;
        i += 6;
      } else {
        return -1;
      }
    } else {
      if (c < 0x20) return -1;
      if (o >= cap) return -1;
      dst[o++] = c;
      i += 1;
    }
  }
  *out_n = o;
  return 0;
}

int json_utf8(const uint8_t *s, size_t n) {
  size_t i = 0;
  if (n && !s) return 0;
  while (i < n) {
    uint8_t c = s[i];
    size_t need;
    size_t k;
    uint32_t cp;
    if (c <= 0x7Fu) {
      i += 1;
      continue;
    }
    if (c < 0xC2u || c > 0xF4u) return 0;
    if (c <= 0xDFu) {
      need = 2;
      cp = (uint32_t)c & 0x1Fu;
    } else if (c <= 0xEFu) {
      need = 3;
      cp = (uint32_t)c & 0x0Fu;
    } else {
      need = 4;
      cp = (uint32_t)c & 0x07u;
    }
    if (i + need > n) return 0;
    for (k = 1; k < need; k++) {
      if ((s[i + k] & 0xC0u) != 0x80u) return 0;
      cp = (cp << 6) | (uint32_t)(s[i + k] & 0x3Fu);
    }
    if (need == 2 && cp < 0x80u) return 0;
    if (need == 3 && (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu))) return 0;
    if (need == 4 && (cp < 0x10000u || cp > 0x10FFFFu)) return 0;
    if (c == 0xE0u && s[i + 1] < 0xA0u) return 0;
    if (c == 0xEDu && s[i + 1] > 0x9Fu) return 0;
    if (c == 0xF0u && s[i + 1] < 0x90u) return 0;
    if (c == 0xF4u && s[i + 1] > 0x8Fu) return 0;
    i += need;
  }
  return 1;
}

int json_append_raw(uint8_t *dst, size_t cap, size_t *n, const void *src, size_t sn) {
  if (!n || *n > cap || sn > cap - *n) return -1;
  if (sn && !dst) return -1;
  if (sn && src) memcpy(dst + *n, src, sn);
  *n += sn;
  return 0;
}

int json_append_i64(uint8_t *dst, size_t cap, size_t *n, int64_t v) {
  char digs[20];
  char tmp[24];
  size_t nd = 0;
  size_t p = 0;
  uint64_t u;
  int neg = v < 0;
  if (neg) u = (uint64_t)(-(v + 1)) + 1u;
  else u = (uint64_t)v;
  if (u == 0) digs[nd++] = '0';
  while (u) {
    digs[nd++] = (char)('0' + (u % 10u));
    u /= 10u;
  }
  if (neg) tmp[p++] = '-';
  while (nd) tmp[p++] = digs[--nd];
  return json_append_raw(dst, cap, n, tmp, p);
}

int json_append_str(uint8_t *dst, size_t cap, size_t *n, const uint8_t *src, size_t sn) {
  static const char hex[] = "0123456789abcdef";
  size_t i;
  size_t saved;
  if (!n) return -1;
  saved = *n;
  if (json_append_raw(dst, cap, n, "\"", 1) < 0) return -1;
  for (i = 0; i < sn; i++) {
    uint8_t c = src ? src[i] : 0;
    char esc[6];
    if (c == '"' || c == '\\') {
      esc[0] = '\\';
      esc[1] = (char)c;
      if (json_append_raw(dst, cap, n, esc, 2) < 0) {
        *n = saved;
        return -1;
      }
    } else if (c < 0x20) {
      esc[0] = '\\';
      esc[1] = 'u';
      esc[2] = '0';
      esc[3] = '0';
      esc[4] = hex[c >> 4];
      esc[5] = hex[c & 0x0F];
      if (json_append_raw(dst, cap, n, esc, 6) < 0) {
        *n = saved;
        return -1;
      }
    } else if (json_append_raw(dst, cap, n, &c, 1) < 0) {
      *n = saved;
      return -1;
    }
  }
  if (json_append_raw(dst, cap, n, "\"", 1) < 0) {
    *n = saved;
    return -1;
  }
  return 0;
}

int json_row_post(uint8_t *dst, size_t cap, size_t *n, int64_t id, const uint8_t *text, size_t text_n) {
  size_t saved;
  if (!n) return -1;
  saved = *n;
  if (json_append_raw(dst, cap, n, "{\"id\":", 6) < 0 || json_append_i64(dst, cap, n, id) < 0 ||
      json_append_raw(dst, cap, n, ",\"text\":", 8) < 0 ||
      json_append_str(dst, cap, n, text, text_n) < 0 || json_append_raw(dst, cap, n, "}", 1) < 0) {
    *n = saved;
    return -1;
  }
  return 0;
}

int json_row_like(uint8_t *dst, size_t cap, size_t *n, int64_t id, int64_t likes) {
  size_t saved;
  if (!n) return -1;
  saved = *n;
  if (json_append_raw(dst, cap, n, "{\"id\":", 6) < 0 || json_append_i64(dst, cap, n, id) < 0 ||
      json_append_raw(dst, cap, n, ",\"likes\":", 9) < 0 || json_append_i64(dst, cap, n, likes) < 0 ||
      json_append_raw(dst, cap, n, "}", 1) < 0) {
    *n = saved;
    return -1;
  }
  return 0;
}
