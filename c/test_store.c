/* SQLite rows. Not a status. */

#include "store.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void bad(const char *name) {
  fprintf(stderr, "FAIL %s\n", name);
  failures += 1;
}

static void eqmem(const char *name, const uint8_t *p, size_t n, const char *lit) {
  if (n != strlen(lit) || memcmp(p, lit, n) != 0) {
    fprintf(stderr, "FAIL %s: '%.*s'\n", name, (int)n, (const char *)p);
    failures += 1;
  }
}

int main(void) {
  Store *st = store_open(":memory:");
  uint8_t buf[256];
  uint8_t text[64];
  size_t n = 0;
  int64_t id = 0;
  int64_t likes = 0;
  uint8_t raw[] = {0x22, 0x00, 0x01};
  if (!st || !store_ping(st)) bad("open");
  if (store_ping(NULL) != 0) bad("null ping");
  if (store_feed(st, buf, sizeof buf, &n) != 0) bad("empty feed rc");
  eqmem("empty feed", buf, n, "[]");
  if (store_create(st, (const uint8_t *)"hi", 2, &id) != 0 || id != 1) bad("create");
  if (store_read(st, 1, text, sizeof text, &n) != 1) bad("read");
  eqmem("read hi", text, n, "hi");
  if (store_read(st, 2, text, sizeof text, &n) != 0) bad("missing");
  if (store_like(st, 1, (const uint8_t *)"ada", 3, &likes) != 1 || likes != 1) bad("like");
  if (store_like(st, 1, (const uint8_t *)"ada", 3, &likes) != 2 || likes != 1) bad("again");
  if (store_like(st, 1, (const uint8_t *)"bob", 3, &likes) != 1 || likes != 2) bad("bob");
  if (store_like(st, 9, (const uint8_t *)"ada", 3, &likes) != 0) bad("no post");
  if (store_create(st, raw, 3, &id) != 0 || id != 2) bad("nul create");
  if (store_read(st, 2, text, sizeof text, &n) != 1 || n != 3 || memcmp(text, raw, 3) != 0)
    bad("nul read");
  if (store_feed(st, buf, sizeof buf, &n) != 0) bad("feed");
  if (store_feed(st, buf, 4, &n) != -2) bad("feed fit");
  if (store_open("/no/such/directory/muro-http.db") != NULL) bad("bad path");
  if (store_feed(NULL, buf, sizeof buf, &n) != -1) bad("null feed");
  if (store_create(NULL, (const uint8_t *)"a", 1, &id) != -1) bad("null create");
  if (store_read(NULL, 1, text, sizeof text, &n) != -1) bad("null read");
  if (store_like(NULL, 1, (const uint8_t *)"a", 1, &likes) != -1) bad("null like");
  store_close(st);
  store_close(NULL);
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
