/* HS256 bearer check. SHA-256 and HMAC are the FIPS 180-4 and
   FIPS 198-1 constructions. The compare of the 32-byte MAC is
   constant-time. Header and payload JSON are read here because the
   body book classifies a post body. It does not extract a claim. */

#include "jwt.h"
#include "decision_tags.h"

#include <string.h>

static uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32u - n)); }

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

typedef struct Sha256 {
  uint32_t h[8];
  uint8_t block[64];
  size_t n;
  uint64_t total;
} Sha256;

static void sha_init(Sha256 *s) {
  s->h[0] = 0x6a09e667u;
  s->h[1] = 0xbb67ae85u;
  s->h[2] = 0x3c6ef372u;
  s->h[3] = 0xa54ff53au;
  s->h[4] = 0x510e527fu;
  s->h[5] = 0x9b05688cu;
  s->h[6] = 0x1f83d9abu;
  s->h[7] = 0x5be0cd19u;
  s->n = 0;
  s->total = 0;
}

static void sha_block(Sha256 *s, const uint8_t block[64]) {
  uint32_t w[64];
  uint32_t a, b, c, d, e, f, g, h;
  int i;
  for (i = 0; i < 16; i++) {
    w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
           ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
  }
  for (i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  a = s->h[0];
  b = s->h[1];
  c = s->h[2];
  d = s->h[3];
  e = s->h[4];
  f = s->h[5];
  g = s->h[6];
  h = s->h[7];
  for (i = 0; i < 64; i++) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t t1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t t2 = S0 + maj;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  s->h[0] += a;
  s->h[1] += b;
  s->h[2] += c;
  s->h[3] += d;
  s->h[4] += e;
  s->h[5] += f;
  s->h[6] += g;
  s->h[7] += h;
}

static void sha_update(Sha256 *s, const uint8_t *p, size_t n) {
  size_t i = 0;
  s->total += (uint64_t)n;
  while (i < n) {
    size_t room = 64 - s->n;
    size_t take = n - i;
    if (take > room) take = room;
    memcpy(s->block + s->n, p + i, take);
    s->n += take;
    i += take;
    if (s->n == 64) {
      sha_block(s, s->block);
      s->n = 0;
    }
  }
}

static void sha_final(Sha256 *s, uint8_t out[32]) {
  uint64_t bits = s->total * 8u;
  uint8_t one = 0x80;
  uint8_t lenb[8];
  int i;
  sha_update(s, &one, 1);
  one = 0;
  while (s->n != 56) sha_update(s, &one, 1);
  for (i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha_update(s, lenb, 8);
  for (i = 0; i < 8; i++) {
    out[i * 4] = (uint8_t)(s->h[i] >> 24);
    out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
    out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
    out[i * 4 + 3] = (uint8_t)s->h[i];
  }
}

void jwt_sha256(const uint8_t *msg, size_t n, uint8_t out[32]) {
  Sha256 s;
  sha_init(&s);
  sha_update(&s, msg, n);
  sha_final(&s, out);
}

void jwt_hmac_sha256(const uint8_t *key, size_t key_n, const uint8_t *msg, size_t msg_n,
                     uint8_t out[32]) {
  uint8_t k[64];
  uint8_t ipad[64];
  uint8_t opad[64];
  uint8_t inner[32];
  Sha256 s;
  size_t i;
  memset(k, 0, sizeof k);
  if (key_n > 64) jwt_sha256(key, key_n, k);
  else if (key && key_n) memcpy(k, key, key_n);
  for (i = 0; i < 64; i++) {
    ipad[i] = (uint8_t)(k[i] ^ 0x36u);
    opad[i] = (uint8_t)(k[i] ^ 0x5cu);
  }
  sha_init(&s);
  sha_update(&s, ipad, 64);
  sha_update(&s, msg, msg_n);
  sha_final(&s, inner);
  sha_init(&s);
  sha_update(&s, opad, 64);
  sha_update(&s, inner, 32);
  sha_final(&s, out);
}

static int ct_eq(const uint8_t *a, const uint8_t *b, size_t n) {
  volatile unsigned diff = 0;
  size_t i;
  for (i = 0; i < n; i++) diff |= (unsigned)(a[i] ^ b[i]);
  return diff == 0;
}

static int b64val(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return (int)(c - 'A');
  if (c >= 'a' && c <= 'z') return (int)(c - 'a') + 26;
  if (c >= '0' && c <= '9') return (int)(c - '0') + 52;
  if (c == '-') return 62;
  if (c == '_') return 63;
  return -1;
}

static int b64url_dec(const uint8_t *in, size_t n, uint8_t *dst, size_t cap, size_t *out_n) {
  size_t i = 0;
  size_t o = 0;
  int pad = 0;
  if (n == 0 || (n % 4u) == 1u) return -1;
  while (i < n) {
    int v[4];
    int k;
    int have = 0;
    unsigned acc;
    if (pad) return -1;
    for (k = 0; k < 4; k++) {
      uint8_t c;
      if (i >= n) {
        v[k] = -2;
        continue;
      }
      c = in[i++];
      if (c == '=') {
        v[k] = -2;
        pad = 1;
        continue;
      }
      if (pad) return -1;
      v[k] = b64val(c);
      if (v[k] < 0) return -1;
      have += 1;
    }
    if (have < 2) return -1;
    acc = ((unsigned)v[0] << 18) | ((unsigned)(v[1] < 0 ? 0 : v[1]) << 12);
    if (have > 2) acc |= (unsigned)v[2] << 6;
    if (have > 3) acc |= (unsigned)v[3];
    if (o >= cap) return -1;
    dst[o++] = (uint8_t)(acc >> 16);
    if (have > 2) {
      if (o >= cap) return -1;
      dst[o++] = (uint8_t)(acc >> 8);
    }
    if (have > 3) {
      if (o >= cap) return -1;
      dst[o++] = (uint8_t)acc;
    }
  }
  *out_n = o;
  return 0;
}

static void skip_ws(const uint8_t *p, size_t n, size_t *i) {
  while (*i < n) {
    uint8_t c = p[*i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
    *i += 1;
  }
}

static int hexv(uint8_t c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'a' && c <= 'f') return (int)(c - 'a') + 10;
  if (c >= 'A' && c <= 'F') return (int)(c - 'A') + 10;
  return -1;
}

static int take_string(const uint8_t *p, size_t n, size_t *i, uint8_t *dst, size_t cap, size_t *out_n,
                       int store) {
  size_t o = 0;
  if (*i >= n || p[*i] != '"') return -1;
  *i += 1;
  while (*i < n) {
    uint8_t c = p[*i];
    if (c == '"') {
      *i += 1;
      if (out_n) *out_n = o;
      return 0;
    }
    if (c == '\\') {
      uint8_t e;
      unsigned cp;
      int k;
      uint8_t tmp[3];
      size_t tn = 0;
      if (*i + 1 >= n) return -1;
      e = p[*i + 1];
      if (e == '"' || e == '\\' || e == '/') {
        tmp[0] = e;
        tn = 1;
        *i += 2;
      } else if (e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') {
        tmp[0] = (uint8_t)(e == 'b' ? 0x08 : e == 'f' ? 0x0c : e == 'n' ? '\n' : e == 'r' ? '\r' : '\t');
        tn = 1;
        *i += 2;
      } else if (e == 'u') {
        if (*i + 5 >= n) return -1;
        cp = 0;
        for (k = 0; k < 4; k++) {
          int h = hexv(p[*i + 2 + k]);
          if (h < 0) return -1;
          cp = (cp << 4) | (unsigned)h;
        }
        if (cp >= 0xD800u && cp <= 0xDFFFu) return -1;
        if (cp < 0x80u) {
          tmp[0] = (uint8_t)cp;
          tn = 1;
        } else if (cp < 0x800u) {
          tmp[0] = (uint8_t)(0xC0u | (cp >> 6));
          tmp[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
          tn = 2;
        } else {
          tmp[0] = (uint8_t)(0xE0u | (cp >> 12));
          tmp[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
          tmp[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
          tn = 3;
        }
        *i += 6;
      } else {
        return -1;
      }
      if (store) {
        size_t t;
        if (o + tn > cap) return -1;
        for (t = 0; t < tn; t++) dst[o + t] = tmp[t];
      }
      o += tn;
      continue;
    }
    if (c < 0x20) return -1;
    if (store) {
      if (o >= cap) return -1;
      dst[o] = c;
    }
    o += 1;
    *i += 1;
  }
  return -1;
}

static int skip_string(const uint8_t *p, size_t n, size_t *i) {
  return take_string(p, n, i, NULL, 0, NULL, 0);
}

static int take_i64(const uint8_t *p, size_t n, size_t *i, int64_t *out) {
  int neg = 0;
  uint64_t u = 0;
  int digits = 0;
  if (*i < n && p[*i] == '-') {
    neg = 1;
    *i += 1;
  }
  if (*i >= n || p[*i] < '0' || p[*i] > '9') return -1;
  if (p[*i] == '0') {
    *i += 1;
    digits = 1;
    if (*i < n && p[*i] >= '0' && p[*i] <= '9') return -1;
  } else {
    while (*i < n && p[*i] >= '0' && p[*i] <= '9') {
      uint64_t d = (uint64_t)(p[*i] - '0');
      if (u > (UINT64_MAX - d) / 10u) return -1;
      u = u * 10u + d;
      digits += 1;
      *i += 1;
    }
  }
  if (!digits) return -1;
  if (*i < n && (p[*i] == '.' || p[*i] == 'e' || p[*i] == 'E')) return -1;
  if (!neg) {
    if (u > (uint64_t)INT64_MAX) return -1;
    *out = (int64_t)u;
    return 0;
  }
  if (u > (uint64_t)INT64_MAX + 1u) return -1;
  if (u == (uint64_t)INT64_MAX + 1u) *out = INT64_MIN;
  else *out = -(int64_t)u;
  return 0;
}

static int skip_number(const uint8_t *p, size_t n, size_t *i) {
  int64_t v;
  return take_i64(p, n, i, &v);
}

static int skip_lit(const uint8_t *p, size_t n, size_t *i, const char *lit, size_t ln) {
  size_t k;
  if (*i + ln > n) return -1;
  for (k = 0; k < ln; k++) {
    if (p[*i + k] != (uint8_t)lit[k]) return -1;
  }
  *i += ln;
  return 0;
}

static int skip_value(const uint8_t *p, size_t n, size_t *i, int depth);

static int skip_container(const uint8_t *p, size_t n, size_t *i, int depth, uint8_t open, uint8_t close,
                          int pairs) {
  int first = 1;
  if (depth > 32) return -1;
  if (*i >= n || p[*i] != open) return -1;
  *i += 1;
  skip_ws(p, n, i);
  if (*i < n && p[*i] == close) {
    *i += 1;
    return 0;
  }
  for (;;) {
    if (!first) {
      skip_ws(p, n, i);
      if (*i >= n || p[*i] != ',') return -1;
      *i += 1;
    }
    first = 0;
    skip_ws(p, n, i);
    if (pairs) {
      if (skip_string(p, n, i) < 0) return -1;
      skip_ws(p, n, i);
      if (*i >= n || p[*i] != ':') return -1;
      *i += 1;
    }
    if (skip_value(p, n, i, depth + 1) < 0) return -1;
    skip_ws(p, n, i);
    if (*i < n && p[*i] == close) {
      *i += 1;
      return 0;
    }
  }
}

static int skip_value(const uint8_t *p, size_t n, size_t *i, int depth) {
  uint8_t c;
  skip_ws(p, n, i);
  if (*i >= n) return -1;
  c = p[*i];
  if (c == '"') return skip_string(p, n, i);
  if (c == '{') return skip_container(p, n, i, depth, '{', '}', 1);
  if (c == '[') return skip_container(p, n, i, depth, '[', ']', 0);
  if (c == 't') return skip_lit(p, n, i, "true", 4);
  if (c == 'f') return skip_lit(p, n, i, "false", 5);
  if (c == 'n') return skip_lit(p, n, i, "null", 4);
  if (c == '-' || (c >= '0' && c <= '9')) return skip_number(p, n, i);
  return -1;
}

static int key_is(const uint8_t *k, size_t kn, const char *lit) {
  size_t n = strlen(lit);
  return kn == n && memcmp(k, lit, n) == 0;
}

/* 0 ok. alg_hs is 1 only for the four bytes HS256. typ_bad is 1 when
   typ is present and is not JWT. */
static int parse_header(const uint8_t *p, size_t n, int *alg_hs, int *typ_bad) {
  size_t i = 0;
  int saw_alg = 0;
  int saw_typ = 0;
  int first = 1;
  *alg_hs = 0;
  *typ_bad = 0;
  skip_ws(p, n, &i);
  if (i >= n || p[i] != '{') return -1;
  i += 1;
  skip_ws(p, n, &i);
  if (i < n && p[i] == '}') return -1;
  for (;;) {
    uint8_t key[32];
    size_t kn = 0;
    if (!first) {
      skip_ws(p, n, &i);
      if (i >= n || p[i] != ',') return -1;
      i += 1;
    }
    first = 0;
    skip_ws(p, n, &i);
    if (take_string(p, n, &i, key, sizeof key, &kn, 1) < 0) return -1;
    skip_ws(p, n, &i);
    if (i >= n || p[i] != ':') return -1;
    i += 1;
    skip_ws(p, n, &i);
    if (key_is(key, kn, "alg")) {
      uint8_t val[16];
      size_t vn = 0;
      if (saw_alg) return -1;
      saw_alg = 1;
      if (take_string(p, n, &i, val, sizeof val, &vn, 1) < 0) return -1;
      *alg_hs = key_is(val, vn, "HS256");
    } else if (key_is(key, kn, "typ")) {
      uint8_t val[16];
      size_t vn = 0;
      if (saw_typ) return -1;
      saw_typ = 1;
      if (take_string(p, n, &i, val, sizeof val, &vn, 1) < 0) return -1;
      if (!key_is(val, vn, "JWT")) *typ_bad = 1;
    } else if (skip_value(p, n, &i, 0) < 0) {
      return -1;
    }
    skip_ws(p, n, &i);
    if (i < n && p[i] == '}') {
      i += 1;
      skip_ws(p, n, &i);
      if (i != n || !saw_alg) return -1;
      return 0;
    }
  }
}

static int parse_payload(const uint8_t *p, size_t n, int64_t now, JwtResult *out) {
  size_t i = 0;
  int saw_sub = 0;
  int saw_exp = 0;
  int saw_nbf = 0;
  int first = 1;
  int64_t exp = 0;
  int64_t nbf = 0;
  out->sub_n = 0;
  skip_ws(p, n, &i);
  if (i >= n || p[i] != '{') return -1;
  i += 1;
  skip_ws(p, n, &i);
  if (i < n && p[i] == '}') return -1;
  for (;;) {
    uint8_t key[32];
    size_t kn = 0;
    if (!first) {
      skip_ws(p, n, &i);
      if (i >= n || p[i] != ',') return -1;
      i += 1;
    }
    first = 0;
    skip_ws(p, n, &i);
    if (take_string(p, n, &i, key, sizeof key, &kn, 1) < 0) return -1;
    skip_ws(p, n, &i);
    if (i >= n || p[i] != ':') return -1;
    i += 1;
    skip_ws(p, n, &i);
    if (key_is(key, kn, "sub")) {
      if (saw_sub) return -1;
      saw_sub = 1;
      if (i >= n || p[i] != '"') return -1;
      if (take_string(p, n, &i, out->sub, sizeof out->sub, &out->sub_n, 1) < 0) return -1;
      if (out->sub_n == 0) return -1;
    } else if (key_is(key, kn, "exp")) {
      if (saw_exp) return -1;
      saw_exp = 1;
      if (take_i64(p, n, &i, &exp) < 0) return -1;
    } else if (key_is(key, kn, "nbf")) {
      if (saw_nbf) return -1;
      saw_nbf = 1;
      if (take_i64(p, n, &i, &nbf) < 0) return -1;
    } else if (skip_value(p, n, &i, 0) < 0) {
      return -1;
    }
    skip_ws(p, n, &i);
    if (i < n && p[i] == '}') {
      i += 1;
      skip_ws(p, n, &i);
      if (i != n || !saw_sub) return -1;
      if (saw_exp && (now < 0 || exp < now)) return -1;
      if (saw_nbf && (now < 0 || nbf > now)) return -1;
      return 0;
    }
  }
}

static void bad_token(JwtResult *out) {
  out->auth = AUTH_BAD_TOKEN;
  out->sub_n = 0;
}

static void bad_payload(JwtResult *out) {
  out->auth = AUTH_BAD_PAYLOAD;
  out->sub_n = 0;
}

void jwt_classify(const uint8_t *val, size_t n, const uint8_t *key, size_t key_n, int64_t now,
                  JwtResult *out) {
  const uint8_t *tok;
  size_t tn;
  size_t d1 = 0;
  size_t d2 = 0;
  size_t i;
  uint8_t mac[32];
  uint8_t sig[64];
  size_t sig_n = 0;
  uint8_t header[4096];
  uint8_t payload[4096];
  size_t header_n = 0;
  size_t payload_n = 0;
  int alg_hs = 0;
  int typ_bad = 0;
  int sig_ok;
  out->auth = AUTH_ABSENT;
  out->sub_n = 0;
  if (!val || n == 0) return;
  if (n < 7 || memcmp(val, "Bearer ", 7) != 0) {
    bad_token(out);
    return;
  }
  tok = val + 7;
  tn = n - 7;
  if (tn == 0 || !key || key_n == 0) {
    bad_token(out);
    return;
  }
  for (i = 0; i < tn; i++) {
    if (tok[i] == '.') {
      if (d1 == 0) d1 = i;
      else if (d2 == 0) d2 = i;
      else {
        bad_token(out);
        return;
      }
    }
  }
  if (d1 == 0 || d2 == 0 || d1 + 1 >= d2 || d2 + 1 >= tn) {
    bad_token(out);
    return;
  }
  jwt_hmac_sha256(key, key_n, tok, d2, mac);
  if (b64url_dec(tok + d2 + 1, tn - d2 - 1, sig, sizeof sig, &sig_n) < 0) {
    bad_token(out);
    return;
  }
  sig_ok = sig_n == 32 && ct_eq(mac, sig, 32);
  if (!sig_ok) {
    bad_token(out);
    return;
  }
  if (b64url_dec(tok, d1, header, sizeof header, &header_n) < 0 ||
      parse_header(header, header_n, &alg_hs, &typ_bad) < 0 || !alg_hs || typ_bad) {
    bad_token(out);
    return;
  }
  if (b64url_dec(tok + d1 + 1, d2 - d1 - 1, payload, sizeof payload, &payload_n) < 0 ||
      parse_payload(payload, payload_n, now, out) < 0) {
    bad_payload(out);
    return;
  }
  out->auth = AUTH_OK;
}
