/* SQLite rows. Not a status. */

#include "store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void unlink_db(const char *path) {
  char side[256];
  if (!path || !path[0]) return;
  unlink(path);
  snprintf(side, sizeof side, "%s-wal", path);
  unlink(side);
  snprintf(side, sizeof side, "%s-shm", path);
  unlink(side);
}

static void check_memory(void) {
  Store *st = store_open(":memory:");
  uint8_t buf[256];
  uint8_t tiny[4];
  uint8_t text[64];
  char journal[16];
  size_t n = 0;
  int64_t id = 0;
  int64_t likes = 0;
  int syncv = 0;
  int busy = 0;
  int i;
  uint8_t raw[] = {0x22, 0x00, 0x01};
  if (!st || !store_ping(st)) bad("open");
  if (store_durability(st, journal, sizeof journal, &syncv, &busy) != 0) bad("mem pragma");
  if (strcmp(journal, "memory") != 0) {
    fprintf(stderr, "FAIL mem journal: %s\n", journal);
    failures += 1;
  }
  if (syncv != 1) {
    fprintf(stderr, "FAIL mem sync: %d\n", syncv);
    failures += 1;
  }
  if (busy != 5000) {
    fprintf(stderr, "FAIL mem busy: %d\n", busy);
    failures += 1;
  }
  if (store_durability(NULL, journal, sizeof journal, &syncv, &busy) != -1) bad("null durability");
  if (store_ping(NULL) != 0) bad("null ping");
  if (store_feed(st, buf, sizeof buf, &n) != 0) bad("empty feed rc");
  eqmem("empty feed", buf, n, "[]");
  if (store_create(st, (const uint8_t *)"hi", 2, &id, buf, sizeof buf, &n) != 0 || id != 1)
    bad("create");
  eqmem("create row", buf, n, "{\"id\":1,\"text\":\"hi\"}");
  if (store_read(st, 1, text, sizeof text, &n) != 1) bad("read");
  eqmem("read hi", text, n, "hi");
  if (store_read(st, 2, text, sizeof text, &n) != 0) bad("missing");
  if (store_like(st, 1, (const uint8_t *)"ada", 3, &likes, buf, sizeof buf, &n) != 1 || likes != 1)
    bad("like");
  eqmem("like row", buf, n, "{\"id\":1,\"likes\":1}");
  if (store_like(st, 1, (const uint8_t *)"ada", 3, &likes, buf, sizeof buf, &n) != 2 || likes != 1)
    bad("again");
  if (store_like(st, 1, (const uint8_t *)"bob", 3, &likes, buf, sizeof buf, &n) != 1 || likes != 2)
    bad("bob");
  if (store_like(st, 9, (const uint8_t *)"ada", 3, &likes, buf, sizeof buf, &n) != 0) bad("no post");
  if (store_like(st, 1, (const uint8_t *)"cam", 3, &likes, tiny, sizeof tiny, &n) != -2) bad("like fit");
  if (store_like(st, 1, (const uint8_t *)"cam", 3, &likes, buf, sizeof buf, &n) != 1 || likes != 3)
    bad("like after fit");
  if (store_create(st, raw, 3, &id, buf, sizeof buf, &n) != 0 || id != 2) bad("nul create");
  if (store_read(st, 2, text, sizeof text, &n) != 1 || n != 3 || memcmp(text, raw, 3) != 0)
    bad("nul read");
  if (store_create(st, (const uint8_t *)"nope", 4, &id, tiny, sizeof tiny, &n) != -2) bad("create fit");
  for (i = 1; i <= 5; i++) {
    size_t tn = 0;
    int rr = store_read(st, i, text, sizeof text, &tn);
    if (rr == 1 && tn == 4 && memcmp(text, "nope", 4) == 0) bad("create fit committed");
  }
  if (store_feed(st, buf, sizeof buf, &n) != 0) bad("feed");
  if (store_feed(st, buf, 4, &n) != -2) bad("feed fit");
  if (store_open("/no/such/directory/muro-http.db") != NULL) bad("bad path");
  if (store_feed(NULL, buf, sizeof buf, &n) != -1) bad("null feed");
  if (store_create(NULL, (const uint8_t *)"a", 1, &id, buf, sizeof buf, &n) != -1) bad("null create");
  if (store_read(NULL, 1, text, sizeof text, &n) != -1) bad("null read");
  if (store_like(NULL, 1, (const uint8_t *)"a", 1, &likes, buf, sizeof buf, &n) != -1) bad("null like");
  store_close(st);
  store_close(NULL);
}

static void check_file(void) {
  char path[] = "/tmp/muro-http-store-XXXXXX";
  Store *st;
  Store *again;
  uint8_t buf[256];
  uint8_t text[64];
  char journal[16];
  size_t n = 0;
  int64_t id = 0;
  int syncv = -1;
  int busy = 0;
  int fd = mkstemp(path);
  if (fd < 0) {
    bad("mkstemp");
    return;
  }
  close(fd);
  st = store_open(path);
  if (!st) {
    bad("file open");
    unlink_db(path);
    return;
  }
  if (store_durability(st, journal, sizeof journal, &syncv, &busy) != 0) bad("file pragma");
  if (strcmp(journal, "wal") != 0) {
    fprintf(stderr, "FAIL file journal: %s\n", journal);
    failures += 1;
  }
  if (syncv != 1) {
    fprintf(stderr, "FAIL file sync: %d\n", syncv);
    failures += 1;
  }
  if (busy != 5000) {
    fprintf(stderr, "FAIL file busy: %d\n", busy);
    failures += 1;
  }
  if (store_create(st, (const uint8_t *)"dur", 3, &id, buf, sizeof buf, &n) != 0 || id != 1)
    bad("file create");
  eqmem("file row", buf, n, "{\"id\":1,\"text\":\"dur\"}");
  store_close(st);
  again = store_open(path);
  if (!again) {
    bad("reopen");
    unlink_db(path);
    return;
  }
  if (store_durability(again, journal, sizeof journal, &syncv, &busy) != 0 ||
      strcmp(journal, "wal") != 0 || syncv != 1)
    bad("reopen pragma");
  if (store_read(again, 1, text, sizeof text, &n) != 1) bad("reopen read");
  eqmem("reopen dur", text, n, "dur");
  store_close(again);
  unlink_db(path);
}

int main(void) {
  check_memory();
  check_file();
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
