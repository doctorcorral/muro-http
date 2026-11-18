/* Unescape, UTF-8, and row bytes. Not the body class. */

#include "json_text.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void bad(const char *name) {
  fprintf(stderr, "FAIL %s\n", name);
  failures += 1;
}

static void eq_un(const char *name, const char *src, const char *want) {
  uint8_t dst[64];
  size_t n = 0;
  if (json_unescape((const uint8_t *)src, strlen(src), dst, sizeof dst, &n) != 0 || n != strlen(want) ||
      memcmp(dst, want, n) != 0 || !json_utf8(dst, n))
    bad(name);
}

int main(void) {
  uint8_t dst[128];
  size_t n = 0;
  uint8_t raw[] = {0x22, 0x00, 0x01};
  eq_un("plain", "hi", "hi");
  eq_un("quote", "a\\\"b", "a\"b");
  eq_un("slash", "a\\/b", "a/b");
  eq_un("nl", "a\\nb", "a\nb");
  eq_un("u", "\\u0041", "A");
  eq_un("copy", "\\u00A9", "\xC2\xA9");
  if (json_unescape((const uint8_t *)"\\uD800", 6, dst, sizeof dst, &n) == 0) bad("surrogate");
  if (json_unescape((const uint8_t *)"\\q", 2, dst, sizeof dst, &n) == 0) bad("bad escape");
  if (json_utf8((const uint8_t *)"\xC0\x80", 2)) bad("overlong");
  if (json_utf8((const uint8_t *)"\xED\xA0\x80", 3)) bad("utf8 surrogate");
  if (!json_utf8((const uint8_t *)"\xF0\x9F\x98\x80", 4)) bad("emoji");
  n = 0;
  if (json_row_post(dst, sizeof dst, &n, 1, (const uint8_t *)"hi", 2) != 0 ||
      n != strlen("{\"id\":1,\"text\":\"hi\"}") || memcmp(dst, "{\"id\":1,\"text\":\"hi\"}", n) != 0)
    bad("row post");
  n = 0;
  if (json_row_post(dst, sizeof dst, &n, 2, raw, 3) != 0) bad("row nul");
  {
    const char *want = "{\"id\":2,\"text\":\"\\\"\\u0000\\u0001\"}";
    if (n != strlen(want) || memcmp(dst, want, n) != 0) bad("row escapes controls");
  }
  n = 0;
  if (json_row_like(dst, sizeof dst, &n, 7, 1) != 0 ||
      memcmp(dst, "{\"id\":7,\"likes\":1}", n) != 0)
    bad("row like");
  n = 0;
  if (json_row_post(dst, 4, &n, 1, (const uint8_t *)"hi", 2) == 0 || n != 0) bad("overflow");
  if (failures) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
