/* Drive the generated connection step. The shim only builds slices
   and counts muro__ALLOC. Parsing and the state machine are muro. */

#include "shim.h"
#include "conn.h"

#include <stdio.h>
#include <string.h>

enum {
  PH_READ = 0,
  PH_DECIDE = 1,
  PH_WRITE = 2,
  PH_IDLE = 3,
  PH_CLOSED = 4,
  ACT_READ = 1,
  ACT_WRITE = 2,
  ACT_CLOSE = 3,
  ACT_DECIDE = 4
};

static int failures;

static void bad(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s: %s\n", name, what);
  failures += 1;
}

static muro__slice of(const char *lit) {
  return muro__slice_of((const uint8_t *)lit, strlen(lit));
}

static uint64_t field(Conn st, int i) {
  switch (i) {
  case 0: return st.muro__as.conn.muro__f_0;
  case 1: return st.muro__as.conn.muro__f_1;
  case 2: return st.muro__as.conn.muro__f_2;
  case 3: return st.muro__as.conn.muro__f_3;
  case 4: return st.muro__as.conn.muro__f_4;
  case 5: return st.muro__as.conn.muro__f_5;
  case 6: return st.muro__as.conn.muro__f_6;
  case 7: return st.muro__as.conn.muro__f_7;
  case 8: return st.muro__as.conn.muro__f_8;
  case 9: return st.muro__as.conn.muro__f_9;
  case 10: return st.muro__as.conn.muro__f_10;
  case 11: return st.muro__as.conn.muro__f_11;
  case 12: return st.muro__as.conn.muro__f_12;
  case 13: return st.muro__as.conn.muro__f_13;
  case 14: return st.muro__as.conn.muro__f_14;
  default: return 0;
  }
}

static int same(Conn a, Conn b) {
  int i;
  if (a.muro__field_tag != b.muro__field_tag) return 0;
  for (i = 0; i < 15; i++) {
    if (field(a, i) != field(b, i)) return 0;
  }
  return 1;
}

static void want(const char *name, Conn st, int i, uint64_t expect) {
  if (field(st, i) != expect) {
    fprintf(stderr, "FAIL %s: field %d got %llu want %llu\n", name, i,
            (unsigned long long)field(st, i), (unsigned long long)expect);
    failures += 1;
  }
}

/* Build the event, then count only the step. Event builders are unboxed. */
static unsigned long take(const char *name, Conn *st, Ev ev) {
  unsigned long built;
  unsigned long n;
  muro_http_reset();
  (void)ev;
  built = muro_http_allocs();
  if (built != 0) bad(name, "event allocated");
  muro_http_reset();
  *st = step(*st, ev);
  n = muro_http_allocs();
  printf("%s %lu\n", name, n);
  return n;
}

static const char *health = "GET /health HTTP/1.1\r\n\r\n";
static const char *feed = "GET /feed HTTP/1.1\r\n\r\n";
static const char *partial = "GET /health HTTP/1.1\r\n";
static const char *close_req = "GET /health HTTP/1.1\r\nConnection: close\r\n\r\n";
static const char *http10 = "GET /health HTTP/1.0\r\n\r\n";
static const char *bad_req = "GET/feed HTTP/1.1\r\n\r\n";
static const char *piped = "GET /health HTTP/1.1\r\n\r\nGET /feed HTTP/1.1\r\n\r\n";

static void check_begin(void) {
  unsigned long n;
  Conn st;
  muro_http_reset();
  st = begin();
  n = muro_http_allocs();
  printf("begin-allocs %lu\n", n);
  if (n != 0) bad("begin", "allocs");
  want("begin", st, 0, PH_READ);
  want("begin", st, 6, ACT_READ);
  want("begin", st, 7, 1);
}

static void check_need(void) {
  Conn st = begin();
  unsigned long n = take("need-allocs", &st, evBytes(of(partial)));
  if (n != 1) bad("need", "allocs");
  want("need", st, 0, PH_READ);
  want("need", st, 6, ACT_READ);
  n = take("need-rest-allocs", &st, evBytes(of(health)));
  if (n != 1) bad("need-rest", "allocs");
  want("need-rest", st, 0, PH_DECIDE);
  want("need-rest", st, 6, ACT_DECIDE);
}

static void check_keepalive(void) {
  Conn st = begin();
  unsigned long n = take("health-allocs", &st, evBytes(of(health)));
  if (n != 1) bad("health", "allocs");
  want("health", st, 0, PH_DECIDE);
  want("health", st, 1, 0);
  want("health", st, 2, 0);
  want("health", st, 3, 0);
  want("health", st, 4, 0);
  want("health", st, 6, ACT_DECIDE);
  want("health", st, 13, 24);
  want("health", st, 14, 0);
  n = take("ask-allocs", &st, evDbAsk());
  if (n != 1) bad("ask", "allocs");
  want("ask", st, 0, PH_DECIDE);
  want("ask", st, 10, 7);
  n = take("db-allocs", &st, evDbOk());
  if (n != 1) bad("db", "allocs");
  want("db", st, 0, PH_WRITE);
  want("db", st, 6, ACT_WRITE);
  want("db", st, 8, 0);
  want("db", st, 9, 0);
  want("db", st, 10, 0);
  want("db", st, 11, 0);
  want("db", st, 12, 16);
  n = take("progress-allocs", &st, evProgress(8));
  if (n != 0) bad("progress", "allocs");
  want("progress", st, 0, PH_WRITE);
  want("progress", st, 11, 8);
  n = take("progress-full-allocs", &st, evProgress(8));
  if (n != 0) bad("progress-full", "allocs");
  want("progress-full", st, 11, 16);
  n = take("idle-allocs", &st, evDone());
  if (n != 0) bad("idle", "allocs");
  want("idle", st, 0, PH_IDLE);
  want("idle", st, 6, ACT_READ);
  want("idle", st, 7, 1);
  n = take("second-allocs", &st, evBytes(of(feed)));
  if (n != 1) bad("second", "allocs");
  want("second", st, 0, PH_DECIDE);
  want("second", st, 4, 1);
  n = take("second-db-allocs", &st, evDbOk());
  if (n != 1) bad("second-db", "allocs");
  want("second-db", st, 10, 1);
  n = take("second-idle-allocs", &st, evDone());
  if (n != 0) bad("second-idle", "allocs");
  want("second-idle", st, 0, PH_IDLE);
  n = take("idle-tick-allocs", &st, evTick());
  if (n != 0) bad("idle-tick", "allocs");
  want("idle-tick", st, 0, PH_CLOSED);
  want("idle-tick", st, 6, ACT_CLOSE);
}

static void check_close_header(const char *name, const char *raw) {
  Conn st = begin();
  char label[64];
  unsigned long n;
  snprintf(label, sizeof label, "%s-bytes", name);
  n = take(label, &st, evBytes(of(raw)));
  if (n != 1) bad(name, "bytes allocs");
  want(name, st, 2, 1);
  snprintf(label, sizeof label, "%s-db", name);
  n = take(label, &st, evDbOk());
  if (n != 1) bad(name, "db allocs");
  snprintf(label, sizeof label, "%s-done", name);
  n = take(label, &st, evDone());
  if (n != 0) bad(name, "done allocs");
  want(name, st, 0, PH_CLOSED);
  want(name, st, 6, ACT_CLOSE);
}

static void check_malformed(void) {
  Conn st = begin();
  unsigned long n = take("malformed-allocs", &st, evBytes(of(bad_req)));
  if (n != 1) bad("malformed", "allocs");
  want("malformed", st, 0, PH_WRITE);
  want("malformed", st, 2, 1);
  want("malformed", st, 6, ACT_WRITE);
  want("malformed", st, 8, 2);
  want("malformed", st, 9, 7);
  want("malformed", st, 10, 6);
  n = take("malformed-done-allocs", &st, evDone());
  if (n != 0) bad("malformed-done", "allocs");
  want("malformed-done", st, 0, PH_CLOSED);
}

static void check_half(void) {
  Conn st = begin();
  Conn fin;
  unsigned long n = take("half-bytes-allocs", &st, evBytes(of(health)));
  if (n != 1) bad("half-bytes", "allocs");
  n = take("half-eof-allocs", &st, evEof(of("")));
  if (n != 0) bad("half-eof", "allocs");
  want("half-eof", st, 0, PH_DECIDE);
  want("half-eof", st, 1, 1);
  want("half-eof", st, 2, 1);
  want("half-eof", st, 6, ACT_DECIDE);
  n = take("half-db-allocs", &st, evDbOk());
  if (n != 1) bad("half-db", "allocs");
  want("half-db", st, 0, PH_WRITE);
  want("half-db", st, 1, 1);
  n = take("half-progress-allocs", &st, evProgress(8));
  if (n != 0) bad("half-progress", "allocs");
  want("half-progress", st, 1, 1);
  want("half-progress", st, 11, 8);
  n = take("half-done-allocs", &st, evDone());
  if (n != 0) bad("half-done", "allocs");
  want("half-done", st, 0, PH_CLOSED);
  fin = begin();
  n = take("fin-req-allocs", &fin, evEof(of(health)));
  if (n != 1) bad("fin-req", "allocs");
  want("fin-req", fin, 0, PH_DECIDE);
  want("fin-req", fin, 1, 1);
  n = take("trunc-allocs", &fin, evEof(of(partial)));
  /* fin is already deciding; a second eof stays half and does not re-read. */
  if (n != 0) bad("trunc-ignored", "allocs");
  want("trunc-ignored", fin, 0, PH_DECIDE);
  fin = begin();
  n = take("trunc-allocs", &fin, evEof(of(partial)));
  if (n != 1) bad("trunc", "allocs");
  want("trunc", fin, 0, PH_WRITE);
  want("trunc", fin, 1, 1);
  want("trunc", fin, 8, 2);
}

static void check_pipe(void) {
  Conn st = begin();
  unsigned long n = take("pipe-allocs", &st, evBytes(of(piped)));
  if (n != 1) bad("pipe", "allocs");
  want("pipe", st, 0, PH_DECIDE);
  want("pipe", st, 4, 0);
  want("pipe", st, 13, 24);
  want("pipe", st, 14, 1);
  n = take("pipe-db-allocs", &st, evDbOk());
  if (n != 1) bad("pipe-db", "allocs");
  want("pipe-db", st, 14, 1);
  n = take("pipe-idle-allocs", &st, evDone());
  if (n != 0) bad("pipe-idle", "allocs");
  want("pipe-idle", st, 0, PH_IDLE);
  want("pipe-idle", st, 14, 1);
  n = take("pipe-tail-allocs", &st, evBytes(of(piped + 24)));
  if (n != 1) bad("pipe-tail", "allocs");
  want("pipe-tail", st, 0, PH_DECIDE);
  want("pipe-tail", st, 4, 1);
  want("pipe-tail", st, 14, 0);
}

static void check_closed(void) {
  Conn st;
  Conn again;
  unsigned long n;
  muro_http_reset();
  st = shut();
  if (muro_http_allocs() != 0) bad("shut", "allocs");
  n = take("closed-bytes-allocs", &st, evBytes(of(health)));
  if (n != 0) bad("closed-bytes", "allocs");
  again = st;
  n = take("closed-eof-allocs", &st, evEof(of(health)));
  if (n != 0 || !same(st, again)) bad("closed-eof", "state");
  n = take("closed-progress-allocs", &st, evProgress(8));
  if (n != 0 || !same(st, again)) bad("closed-progress", "state");
  n = take("closed-done-allocs", &st, evDone());
  if (n != 0 || !same(st, again)) bad("closed-done", "state");
  n = take("closed-error-allocs", &st, evError());
  if (n != 0 || !same(st, again)) bad("closed-error", "state");
  n = take("closed-tick-allocs", &st, evTick());
  if (n != 0 || !same(st, again)) bad("closed-tick", "state");
  n = take("closed-db-allocs", &st, evDbOk());
  if (n != 0 || !same(st, again)) bad("closed-db", "state");
  want("closed", st, 0, PH_CLOSED);
  want("closed", st, 6, ACT_CLOSE);
}

static void check_write_error(void) {
  Conn st = begin();
  unsigned long n;
  (void)take("werr-bytes-allocs", &st, evBytes(of(health)));
  (void)take("werr-db-allocs", &st, evDbOk());
  n = take("werr-allocs", &st, evError());
  if (n != 0) bad("werr", "allocs");
  want("werr", st, 0, PH_CLOSED);
}

static void check_ignore_tick_while_writing(void) {
  Conn st = begin();
  unsigned long n;
  (void)take("wtick-bytes-allocs", &st, evBytes(of(health)));
  (void)take("wtick-db-allocs", &st, evDbOk());
  n = take("wtick-allocs", &st, evTick());
  if (n != 0) bad("wtick", "allocs");
  want("wtick", st, 0, PH_WRITE);
  want("wtick", st, 6, ACT_WRITE);
}

int main(void) {
  check_begin();
  check_need();
  check_keepalive();
  check_close_header("close", close_req);
  check_close_header("http10", http10);
  check_malformed();
  check_half();
  check_pipe();
  check_closed();
  check_write_error();
  check_ignore_tick_while_writing();
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
