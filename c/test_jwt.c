/* HS256 unit tests. The key below is a test key. It is not a
   server default. Tokens are compact JSON, HS256, that key. */

#include "jwt.h"
#include "decision_tags.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void bad(const char *name) {
  fprintf(stderr, "FAIL %s\n", name);
  failures += 1;
}

static int hexeq(const uint8_t *got, const char *hex) {
  size_t i;
  for (i = 0; i < 32; i++) {
    unsigned v;
    if (sscanf(hex + i * 2, "%2x", &v) != 1) return 0;
    if (got[i] != (uint8_t)v) return 0;
  }
  return 1;
}

static const char *key = "muro-http-test-key";
static const int64_t now = 1700000000;

static const char *ada =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEifQ.3KE6Im9uhSMiXa5L1JtMwmzrpKhdX6M3BvI4x3brFts";
static const char *exp_ok =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEiLCJleHAiOjk5OTk5OTk5OTl9."
    "Epjenj7XS31B0sLSJP-s-TiWA-YPjZ5NC6IhmtQKqB0";
static const char *nbf_ok =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEiLCJuYmYiOjF9.BH2rRpEPRSrkyv8B4BUpQODlzBSGPofoKu-_vTsIIwE";
static const char *no_sub =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJleHAiOjF9.6O0vDw5xVpA0Vc5reohZXUgr71QOA86t3Yd4R7QHPyg";
static const char *empty_sub =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIifQ.3cHkEIQwrbEhQ0j2DFdg1fDQV24zE0me48Cvz9_oZSM";
static const char *sub_num =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOjF9.aQRXrUwRnMtL8wGhhFGC3iMJElpzIx6mkvG99u0Mxzw";
static const char *expired =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEiLCJleHAiOjF9.x6mPeg8JrI4FpgdQ5GgdiH0Ba__Ig0COpNp_LIZu0NU";
static const char *nbf_future =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEiLCJuYmYiOjk5OTk5OTk5OTl9."
    "uAiBGBuaNzU9ec4vg5IwpVrAF4Syhl4HmKZQp_SZATg";
static const char *alg_none =
    "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.eyJzdWIiOiJhZGEifQ.8NI1blacf6p2nNGEpD22a4xFD8e3zVHm1ClUuEbbzGs";
static const char *tamper =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEifQ.3KE6Im9uhSMiXa5L1JtMwmzrpKhdX6M3BvI4x3brFtA";

static void expect(const char *name, const char *token, int auth, const char *sub) {
  char hdr[600];
  JwtResult r;
  int n = snprintf(hdr, sizeof hdr, "Bearer %s", token);
  if (n < 0 || (size_t)n >= sizeof hdr) {
    bad(name);
    return;
  }
  jwt_classify((const uint8_t *)hdr, (size_t)n, (const uint8_t *)key, strlen(key), now, &r);
  if (r.auth != auth) {
    fprintf(stderr, "FAIL %s: auth %d\n", name, r.auth);
    failures += 1;
  }
  if (auth == AUTH_OK) {
    if (r.sub_n != strlen(sub) || memcmp(r.sub, sub, r.sub_n) != 0) bad(name);
  } else if (r.sub_n != 0) {
    bad(name);
  }
}

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64url_enc(const uint8_t *in, size_t n, char *dst, size_t cap) {
  size_t i = 0;
  size_t o = 0;
  while (i + 2 < n) {
    unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | (unsigned)in[i + 2];
    if (o + 4 >= cap) return -1;
    dst[o++] = b64tab[(v >> 18) & 63];
    dst[o++] = b64tab[(v >> 12) & 63];
    dst[o++] = b64tab[(v >> 6) & 63];
    dst[o++] = b64tab[v & 63];
    i += 3;
  }
  if (i < n) {
    unsigned v = (unsigned)in[i] << 16;
    int rem = (int)(n - i);
    if (rem == 2) v |= (unsigned)in[i + 1] << 8;
    if (o + (rem == 2 ? 3 : 2) >= cap) return -1;
    dst[o++] = b64tab[(v >> 18) & 63];
    dst[o++] = b64tab[(v >> 12) & 63];
    if (rem == 2) dst[o++] = b64tab[(v >> 6) & 63];
  }
  if (o >= cap) return -1;
  dst[o] = 0;
  return (int)o;
}

static int b64_val(char c) {
  const char *p = strchr(b64tab, c);
  if (!p) return -1;
  return (int)(p - b64tab);
}

static int sign_token(char *dst, size_t cap, const char *header, const char *payload) {
  char hb[256];
  char pb[800];
  char sb[64];
  char mac_in[1100];
  uint8_t mac[32];
  int hn;
  int pn;
  int sn;
  int n;
  if (!header) header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
  hn = b64url_enc((const uint8_t *)header, strlen(header), hb, sizeof hb);
  pn = b64url_enc((const uint8_t *)payload, strlen(payload), pb, sizeof pb);
  if (hn < 0 || pn < 0) return -1;
  n = snprintf(mac_in, sizeof mac_in, "%s.%s", hb, pb);
  if (n < 0 || (size_t)n >= sizeof mac_in) return -1;
  jwt_hmac_sha256((const uint8_t *)key, strlen(key), (const uint8_t *)mac_in, (size_t)n, mac);
  sn = b64url_enc(mac, 32, sb, sizeof sb);
  if (sn < 0) return -1;
  n = snprintf(dst, cap, "%s.%s", mac_in, sb);
  if (n < 0 || (size_t)n >= cap) return -1;
  return n;
}

static void expect_raw(const char *name, const char *token, int auth, const char *sub) {
  expect(name, token, auth, sub);
}

static void expect_claims(const char *name, const char *header, const char *payload, int auth,
                          const char *sub) {
  char tok[1400];
  if (sign_token(tok, sizeof tok, header, payload) < 0) {
    bad(name);
    return;
  }
  expect_raw(name, tok, auth, sub);
}

/* Flip a leftover bit in the last character of a partial quartet.
   rem 1 leaves 4 bits. rem 2 leaves 2 bits. */
static int flip_tail(char *seg, int leftover_mask) {
  size_t n = strlen(seg);
  int v;
  int flipped;
  if (n == 0) return -1;
  v = b64_val(seg[n - 1]);
  if (v < 0) return -1;
  flipped = v | leftover_mask;
  if (flipped == v) flipped = v | (leftover_mask == 1 ? 2 : 1);
  if ((flipped & ~leftover_mask) != (v & ~leftover_mask)) return -1;
  seg[n - 1] = b64tab[flipped & 63];
  return flipped == v ? -1 : 0;
}

int main(void) {
  uint8_t dig[32];
  JwtResult r;
  jwt_sha256((const uint8_t *)"", 0, dig);
  if (!hexeq(dig, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"))
    bad("sha256 empty");
  jwt_hmac_sha256((const uint8_t *)"key", 3,
                  (const uint8_t *)"The quick brown fox jumps over the lazy dog", 43, dig);
  if (!hexeq(dig, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8"))
    bad("hmac fox");
  jwt_classify(NULL, 0, (const uint8_t *)key, strlen(key), now, &r);
  if (r.auth != AUTH_ABSENT) bad("absent");
  jwt_classify((const uint8_t *)"bearer x", 8, (const uint8_t *)key, strlen(key), now, &r);
  if (r.auth != AUTH_BAD_TOKEN) bad("lowercase bearer");
  jwt_classify((const uint8_t *)"Bearer ", 7, (const uint8_t *)key, strlen(key), now, &r);
  if (r.auth != AUTH_BAD_TOKEN) bad("empty bearer");
  {
    char hdr[600];
    int n = snprintf(hdr, sizeof hdr, "Bearer %s", ada);
    jwt_classify((const uint8_t *)hdr, (size_t)n, NULL, 0, now, &r);
    if (r.auth != AUTH_BAD_TOKEN) bad("no key");
  }
  expect("ada", ada, AUTH_OK, "ada");
  expect("exp", exp_ok, AUTH_OK, "ada");
  expect("nbf", nbf_ok, AUTH_OK, "ada");
  expect("no-sub", no_sub, AUTH_BAD_PAYLOAD, NULL);
  expect("empty-sub", empty_sub, AUTH_BAD_PAYLOAD, NULL);
  expect("sub-num", sub_num, AUTH_BAD_PAYLOAD, NULL);
  expect("expired", expired, AUTH_BAD_PAYLOAD, NULL);
  expect("nbf-future", nbf_future, AUTH_BAD_PAYLOAD, NULL);
  expect("alg-none", alg_none, AUTH_BAD_TOKEN, NULL);
  expect("tamper", tamper, AUTH_BAD_TOKEN, NULL);
  {
    char hdr[600];
    int n = snprintf(hdr, sizeof hdr, "Bearer %s", ada);
    jwt_classify((const uint8_t *)hdr, (size_t)n, (const uint8_t *)"other", 5, now, &r);
    if (r.auth != AUTH_BAD_TOKEN) bad("other key");
  }
  {
    char tok[800];
    if (sign_token(tok, sizeof tok, NULL, "{\"sub\":\"ada\"}") < 0 || strcmp(tok, ada) != 0)
      bad("sign ada");
  }
  {
    char payload[80];
    snprintf(payload, sizeof payload, "{\"sub\":\"ada\",\"exp\":%lld}", (long long)now);
    expect_claims("exp-eq", NULL, payload, AUTH_BAD_PAYLOAD, NULL);
    snprintf(payload, sizeof payload, "{\"sub\":\"ada\",\"exp\":%lld}", (long long)(now + 1));
    expect_claims("exp-next", NULL, payload, AUTH_OK, "ada");
    snprintf(payload, sizeof payload, "{\"sub\":\"ada\",\"nbf\":%lld}", (long long)now);
    expect_claims("nbf-eq", NULL, payload, AUTH_OK, "ada");
    snprintf(payload, sizeof payload, "{\"sub\":\"ada\",\"nbf\":%lld}", (long long)(now + 1));
    expect_claims("nbf-next", NULL, payload, AUTH_BAD_PAYLOAD, NULL);
    snprintf(payload, sizeof payload, "{\"sub\":\"ada\",\"nbf\":%lld,\"exp\":%lld}", (long long)now,
             (long long)(now + 1));
    expect_claims("window", NULL, payload, AUTH_OK, "ada");
    snprintf(payload, sizeof payload, "{\"sub\":\"ada\",\"nbf\":%lld,\"exp\":%lld}", (long long)now,
             (long long)now);
    expect_claims("window-empty", NULL, payload, AUTH_BAD_PAYLOAD, NULL);
  }
  expect_claims("float-iat", NULL, "{\"sub\":\"ada\",\"iat\":1.5}", AUTH_OK, "ada");
  expect_claims("float-neg", NULL, "{\"sub\":\"ada\",\"x\":-2.5e+1}", AUTH_OK, "ada");
  expect_claims("float-nest", NULL, "{\"sub\":\"ada\",\"aud\":[1.5,2e1],\"m\":{\"k\":-0.5}}", AUTH_OK,
                "ada");
  expect_claims("float-big", NULL, "{\"sub\":\"ada\",\"big\":123456789012345678901234567890}",
                AUTH_OK, "ada");
  expect_claims("float-exp", NULL, "{\"sub\":\"ada\",\"exp\":1.5}", AUTH_BAD_PAYLOAD, NULL);
  expect_claims("float-exp-e", NULL, "{\"sub\":\"ada\",\"exp\":1e2}", AUTH_BAD_PAYLOAD, NULL);
  expect_claims("float-nbf", NULL, "{\"sub\":\"ada\",\"nbf\":1.0}", AUTH_BAD_PAYLOAD, NULL);
  expect_claims("bad-num", NULL, "{\"sub\":\"ada\",\"x\":01}", AUTH_BAD_PAYLOAD, NULL);
  expect_claims("header-float", "{\"alg\":\"HS256\",\"typ\":\"JWT\",\"kid\":1.5}", "{\"sub\":\"ada\"}",
                AUTH_OK, "ada");
  expect_claims("typ-lower", "{\"alg\":\"HS256\",\"typ\":\"jwt\"}", "{\"sub\":\"ada\"}", AUTH_OK,
                "ada");
  expect_claims("typ-mixed", "{\"alg\":\"HS256\",\"typ\":\"Jwt\"}", "{\"sub\":\"ada\"}", AUTH_OK,
                "ada");
  expect_claims("typ-absent", "{\"alg\":\"HS256\"}", "{\"sub\":\"ada\"}", AUTH_OK, "ada");
  expect_claims("typ-jws", "{\"alg\":\"HS256\",\"typ\":\"JWS\"}", "{\"sub\":\"ada\"}", AUTH_BAD_TOKEN,
                NULL);
  {
    const char *hdr_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    const char *pay_json = "{\"sub\":\"ada\"}";
    char hb[128];
    char pb[128];
    char sb[64];
    char mac_in[300];
    char tok[400];
    uint8_t mac[32];
    int hn = b64url_enc((const uint8_t *)hdr_json, strlen(hdr_json), hb, sizeof hb);
    int pn = b64url_enc((const uint8_t *)pay_json, strlen(pay_json), pb, sizeof pb);
    int n;
    int sn;
    if (hn < 0 || pn < 0 || flip_tail(pb, 0x0f) < 0) bad("payload bits");
    else {
      n = snprintf(mac_in, sizeof mac_in, "%s.%s", hb, pb);
      jwt_hmac_sha256((const uint8_t *)key, strlen(key), (const uint8_t *)mac_in, (size_t)n, mac);
      sn = b64url_enc(mac, 32, sb, sizeof sb);
      if (sn < 0) bad("payload bits sig");
      else {
        snprintf(tok, sizeof tok, "%s.%s", mac_in, sb);
        expect_raw("payload-bits", tok, AUTH_BAD_PAYLOAD, NULL);
        if (flip_tail(sb, 0x03) < 0) bad("sig bits flip");
        else {
          snprintf(tok, sizeof tok, "%s.%s", mac_in, sb);
          expect_raw("sig-bits", tok, AUTH_BAD_TOKEN, NULL);
        }
      }
    }
    if (sign_token(tok, sizeof tok, NULL, "{\"sub\":\"ada\"}") < 0) bad("short sig sign");
    else {
      size_t tn = strlen(tok);
      if (tn > 0) tok[tn - 1] = 0;
      expect_raw("short-sig", tok, AUTH_BAD_TOKEN, NULL);
    }
    if (sign_token(tok, sizeof tok, NULL, "{\"sub\":\"ada\"}") < 0) bad("long sig sign");
    else {
      size_t tn = strlen(tok);
      if (tn + 1 >= sizeof tok) bad("long sig room");
      else {
        tok[tn] = 'A';
        tok[tn + 1] = 0;
        expect_raw("long-sig", tok, AUTH_BAD_TOKEN, NULL);
      }
    }
  }
  if (failures) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
