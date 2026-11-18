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
  if (failures) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
