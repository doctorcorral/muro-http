/* Emitted bodyClass, textOff, and textLen.
   bc-empty and bc-long are refl in body.muro. The scanner cases are
   here because a refl that walks jbScan does not finish. */

#include "shim.h"
#include "body.h"
#include "decision_tags.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void bad(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s: %s\n", name, what);
  failures += 1;
}

static void expect(const char *name, const uint8_t *p, size_t n, uint64_t cls, uint64_t off,
                   uint64_t len) {
  muro__slice s = muro__slice_of(p, n);
  unsigned long a0;
  unsigned long a1;
  uint64_t got;
  uint64_t goff;
  uint64_t glen;
  a0 = muro_http_allocs();
  got = bodyClass(s);
  a1 = muro_http_allocs();
  goff = textOff(s);
  glen = textLen(s);
  muro_http_reset();
  if (got != cls) {
    fprintf(stderr, "FAIL %s: class %llu\n", name, (unsigned long long)got);
    failures += 1;
  }
  if (goff != off || glen != len) {
    fprintf(stderr, "FAIL %s: off %llu len %llu\n", name, (unsigned long long)goff,
            (unsigned long long)glen);
    failures += 1;
  }
  if (a1 != a0) bad(name, "allocated");
}

static void expect_str(const char *name, const char *lit, uint64_t cls, uint64_t off, uint64_t len) {
  expect(name, (const uint8_t *)lit, strlen(lit), cls, off, len);
}

int main(void) {
  uint8_t cap[1100];
  size_t n;
  expect_str("empty", "", BODY_NONE, 0, 0);
  expect_str("trunc", "{", BODY_MALFORMED, 0, 0);
  expect_str("array", "[1]", BODY_MALFORMED, 0, 0);
  expect_str("obj", "{}", BODY_REQUIRED, 0, 0);
  expect_str("other", "{\"a\":1}", BODY_REQUIRED, 0, 0);
  expect_str("empty-text", "{\"text\":\"\"}", BODY_REQUIRED, 0, 0);
  expect_str("num", "{\"text\":1}", BODY_REQUIRED, 0, 0);
  expect_str("true", "{\"text\":true}", BODY_REQUIRED, 0, 0);
  expect_str("ok", "{\"text\":\"hi\"}", BODY_OK, 9, 2);
  expect_str("space", " {\"text\" : \"hi\"} ", BODY_OK, 12, 2);
  expect_str("extra", "{\"text\":\"hi\",\"a\":1}", BODY_OK, 9, 2);
  expect_str("trail", "{\"text\":\"hi\"}x", BODY_MALFORMED, 0, 0);
  expect_str("comma", "{\"text\":\"hi\",}", BODY_MALFORMED, 0, 0);
  expect_str("dup", "{\"text\":\"a\",\"text\":\"b\"}", BODY_MALFORMED, 0, 0);
  expect_str("sur", "{\"text\":\"\\uD800\"}", BODY_MALFORMED, 0, 0);
  expect_str("esc", "{\"text\":\"\\u0041\"}", BODY_OK, 9, 6);
  expect_str("nested", "{\"a\":{\"text\":\"hi\"}}", BODY_REQUIRED, 0, 0);
  expect_str("key-esc", "{\"\\u0074ext\":\"hi\"}", BODY_REQUIRED, 0, 0);
  expect_str("control", "{\"text\":\"\n\"}", BODY_MALFORMED, 0, 0);
  expect_str("ws", " \n\t", BODY_MALFORMED, 0, 0);
  expect_str("lead-zero", "{\"text\":\"a\",\"n\":01}", BODY_MALFORMED, 0, 0);
  expect_str("truee", "{\"text\":truee}", BODY_MALFORMED, 0, 0);
  expect("utf8-byte", (const uint8_t *)"{\"text\":\"\xff\"}", 12, BODY_OK, 9, 1);
  memset(cap, 'x', sizeof cap);
  memcpy(cap, "{\"text\":\"", 9);
  n = 9 + 1013;
  memcpy(cap + n, "\"}", 2);
  n += 2;
  if (n != 1024) bad("cap-1024", "length");
  expect("cap-1024", cap, n, BODY_OK, 9, 1013);
  n = 9 + 1014;
  memcpy(cap + 9 + 1014, "\"}", 2);
  expect("cap-1025", cap, n + 2, BODY_TOO_LONG, 0, 0);
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
