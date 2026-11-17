/* Drive the generated connection step. The shim only builds slices
   and counts muro__ALLOC. Parsing and the HTTP image are muro.
   Protocol moves go through the emitted session stepper in proto.c
   before the image runs. A refused move does not run the image. */

#include "shim.h"
#include "conn.h"
#include "proto.h"

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

static void restart(Conn *st, Proto **proto) {
  proto_free(*proto);
  *st = begin();
  *proto = proto_begin();
}

/* Classify or decide outside the alloc window, admit the protocol
   moves, then count only the image step. */
static unsigned long take(const char *name, Conn *st, Proto *proto, Ev ev) {
  unsigned long built;
  unsigned long n;
  int phase = (int)field(*st, 0);
  int half = (int)field(*st, 1);
  int keep = (int)field(*st, 2);
  int tag = (int)ev.muro__as.ev.muro__f_0;
  muro__slice buf = ev.muro__as.ev.muro__f_1;
  int status = -1;
  int reply = -1;
  int observe = 0;
  int close_after = half != 0 || keep != 0;
  char why[64];

  if (proto_live(proto) && !proto_done(proto) && (phase == PH_READ || phase == PH_IDLE) &&
      (tag == 0 || (tag == 1 && buf.n > 0))) {
    Classified *c = classify(buf);
    status = c->muro__as.classified.muro__f_0->muro__field_tag;
  }
  if (proto_live(proto) && !proto_done(proto) && phase == PH_DECIDE && tag == 6) {
    Decision *d = decide(verbFrom(field(*st, 3)), pathFrom(field(*st, 4)),
                        idFrom(ev.muro__as.ev.muro__f_6), authFrom(ev.muro__as.ev.muro__f_2),
                        bodyFrom(ev.muro__as.ev.muro__f_3), likeFrom(ev.muro__as.ev.muro__f_4),
                        dbFrom(ev.muro__as.ev.muro__f_5));
    int act = d->muro__as.decided.muro__f_2->muro__field_tag;
    int code = d->muro__as.decided.muro__f_0->muro__field_tag;
    if (act == 7) observe = 1;
    else reply = code <= 1 ? 0 : 1;
  }
  if (!proto_admit(proto, phase, tag, buf.n, status, reply, observe, close_after)) {
    proto_where(proto, why, sizeof why);
    fprintf(stderr, "FAIL %s: protocol refused, at %s\n", name, why);
    failures += 1;
    return 0;
  }

  /* Build the event, then count only the step. Event builders are unboxed. */
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
  Proto *proto;
  muro_http_reset();
  st = begin();
  n = muro_http_allocs();
  printf("begin-allocs %lu\n", n);
  if (n != 0) bad("begin", "allocs");
  want("begin", st, 0, PH_READ);
  want("begin", st, 6, ACT_READ);
  want("begin", st, 7, 1);
  muro_http_reset();
  proto = proto_begin();
  n = muro_http_allocs();
  printf("session-init-allocs %lu\n", n);
  if (n != 0) bad("session-init", "allocs");
  if (!proto_refuses(proto, PROTO_SEND, PROTO_DATA, 0, "reply-ok"))
    bad("session-init", "reply accepted before a piece");
  if (!proto_refuses(proto, PROTO_RECV, PROTO_PICK, 0, 0))
    bad("session-init", "recv pick accepted at the first choice");
  proto_free(proto);
}

static void check_need(void) {
  Proto *proto = proto_begin();
  Conn st = begin();
  unsigned long n = take("need-allocs", &st, proto, evBytes(of(partial)));
  if (n != 1) bad("need", "allocs");
  want("need", st, 0, PH_READ);
  want("need", st, 6, ACT_READ);
  if (proto_done(proto)) bad("need", "session ended on a chunk");
  n = take("need-rest-allocs", &st, proto, evBytes(of(health)));
  if (n != 1) bad("need-rest", "allocs");
  want("need-rest", st, 0, PH_DECIDE);
  want("need-rest", st, 6, ACT_DECIDE);
  if (proto_done(proto)) bad("need-rest", "session ended before the reply");
  proto_free(proto);
}

static void check_keepalive(void) {
  Proto *proto = proto_begin();
  Conn st = begin();
  unsigned long n = take("health-allocs", &st, proto, evBytes(of(health)));
  if (n != 1) bad("health", "allocs");
  want("health", st, 0, PH_DECIDE);
  want("health", st, 1, 0);
  want("health", st, 2, 0);
  want("health", st, 3, 0);
  want("health", st, 4, 0);
  want("health", st, 6, ACT_DECIDE);
  want("health", st, 13, 24);
  want("health", st, 14, 0);
  n = take("ask-allocs", &st, proto, evDbAsk());
  if (n != 1) bad("ask", "allocs");
  want("ask", st, 0, PH_DECIDE);
  want("ask", st, 10, 7);
  n = take("db-allocs", &st, proto, evDbOk());
  if (n != 1) bad("db", "allocs");
  want("db", st, 0, PH_WRITE);
  want("db", st, 6, ACT_WRITE);
  want("db", st, 8, 0);
  want("db", st, 9, 0);
  want("db", st, 10, 0);
  want("db", st, 11, 0);
  want("db", st, 12, 16);
  n = take("progress-allocs", &st, proto, evProgress(8));
  if (n != 0) bad("progress", "allocs");
  want("progress", st, 0, PH_WRITE);
  want("progress", st, 11, 8);
  n = take("progress-full-allocs", &st, proto, evProgress(8));
  if (n != 0) bad("progress-full", "allocs");
  want("progress-full", st, 11, 16);
  n = take("idle-allocs", &st, proto, evDone());
  if (n != 0) bad("idle", "allocs");
  want("idle", st, 0, PH_IDLE);
  want("idle", st, 6, ACT_READ);
  want("idle", st, 7, 1);
  n = take("second-allocs", &st, proto, evBytes(of(feed)));
  if (n != 1) bad("second", "allocs");
  want("second", st, 0, PH_DECIDE);
  want("second", st, 4, 1);
  n = take("second-db-allocs", &st, proto, evDbOk());
  if (n != 1) bad("second-db", "allocs");
  want("second-db", st, 10, 1);
  n = take("second-idle-allocs", &st, proto, evDone());
  if (n != 0) bad("second-idle", "allocs");
  want("second-idle", st, 0, PH_IDLE);
  n = take("idle-tick-allocs", &st, proto, evTick());
  if (n != 0) bad("idle-tick", "allocs");
  want("idle-tick", st, 0, PH_CLOSED);
  want("idle-tick", st, 6, ACT_CLOSE);
  if (!proto_done(proto)) bad("idle-tick", "session open");
  if (!proto_refuses(proto, PROTO_RECV, PROTO_DATA, 0, "piece-req"))
    bad("idle-tick", "piece accepted after end");
  proto_free(proto);
}

static void check_close_header(const char *name, const char *raw) {
  Proto *proto = proto_begin();
  Conn st = begin();
  char label[64];
  unsigned long n;
  snprintf(label, sizeof label, "%s-bytes", name);
  n = take(label, &st, proto, evBytes(of(raw)));
  if (n != 1) bad(name, "bytes allocs");
  want(name, st, 2, 1);
  if (proto_done(proto)) bad(name, "session ended before the reply");
  snprintf(label, sizeof label, "%s-db", name);
  n = take(label, &st, proto, evDbOk());
  if (n != 1) bad(name, "db allocs");
  snprintf(label, sizeof label, "%s-done", name);
  n = take(label, &st, proto, evDone());
  if (n != 0) bad(name, "done allocs");
  want(name, st, 0, PH_CLOSED);
  want(name, st, 6, ACT_CLOSE);
  if (!proto_done(proto)) bad(name, "session open after close");
  proto_free(proto);
}

static void check_malformed(void) {
  Proto *proto = proto_begin();
  Conn st = begin();
  unsigned long n = take("malformed-allocs", &st, proto, evBytes(of(bad_req)));
  if (n != 1) bad("malformed", "allocs");
  want("malformed", st, 0, PH_WRITE);
  want("malformed", st, 2, 1);
  want("malformed", st, 6, ACT_WRITE);
  want("malformed", st, 8, 2);
  want("malformed", st, 9, 7);
  want("malformed", st, 10, 6);
  if (!proto_done(proto)) bad("malformed", "session open");
  n = take("malformed-done-allocs", &st, proto, evDone());
  if (n != 0) bad("malformed-done", "allocs");
  want("malformed-done", st, 0, PH_CLOSED);
  if (!proto_done(proto)) bad("malformed-done", "session open");
  if (!proto_refuses(proto, PROTO_SEND, PROTO_DATA, 0, "reply-ok"))
    bad("malformed-done", "reply accepted after end");
  proto_free(proto);
}

static void check_half(void) {
  Proto *proto = proto_begin();
  Conn st = begin();
  Conn fin;
  unsigned long n = take("half-bytes-allocs", &st, proto, evBytes(of(health)));
  if (n != 1) bad("half-bytes", "allocs");
  if (proto_done(proto)) bad("half-bytes", "session ended before the reply");
  /* The FIN arrives while the reply is already in progress. That is the
     image's half flag. The session chose the open branch before the piece. */
  n = take("half-eof-allocs", &st, proto, evEof(of("")));
  if (n != 0) bad("half-eof", "allocs");
  want("half-eof", st, 0, PH_DECIDE);
  want("half-eof", st, 1, 1);
  want("half-eof", st, 2, 1);
  want("half-eof", st, 6, ACT_DECIDE);
  if (proto_done(proto)) bad("half-eof", "session ended on a host flag");
  n = take("half-db-allocs", &st, proto, evDbOk());
  if (n != 1) bad("half-db", "allocs");
  want("half-db", st, 0, PH_WRITE);
  want("half-db", st, 1, 1);
  n = take("half-progress-allocs", &st, proto, evProgress(8));
  if (n != 0) bad("half-progress", "allocs");
  want("half-progress", st, 1, 1);
  want("half-progress", st, 11, 8);
  n = take("half-done-allocs", &st, proto, evDone());
  if (n != 0) bad("half-done", "allocs");
  want("half-done", st, 0, PH_CLOSED);
  if (!proto_done(proto)) bad("half-done", "session open");
  restart(&fin, &proto);
  n = take("fin-req-allocs", &fin, proto, evEof(of(health)));
  if (n != 1) bad("fin-req", "allocs");
  want("fin-req", fin, 0, PH_DECIDE);
  want("fin-req", fin, 1, 1);
  if (proto_done(proto)) bad("fin-req", "session ended before the reply");
  n = take("trunc-allocs", &fin, proto, evEof(of(partial)));
  /* fin is already deciding; a second eof stays half and does not re-read. */
  if (n != 0) bad("trunc-ignored", "allocs");
  want("trunc-ignored", fin, 0, PH_DECIDE);
  if (proto_done(proto)) bad("trunc-ignored", "session ended");
  restart(&fin, &proto);
  n = take("trunc-allocs", &fin, proto, evEof(of(partial)));
  if (n != 1) bad("trunc", "allocs");
  want("trunc", fin, 0, PH_WRITE);
  want("trunc", fin, 1, 1);
  want("trunc", fin, 8, 2);
  if (!proto_done(proto)) bad("trunc", "session open");
  proto_free(proto);
}

static void check_pipe(void) {
  Proto *proto = proto_begin();
  Conn st = begin();
  unsigned long n = take("pipe-allocs", &st, proto, evBytes(of(piped)));
  if (n != 1) bad("pipe", "allocs");
  want("pipe", st, 0, PH_DECIDE);
  want("pipe", st, 4, 0);
  want("pipe", st, 13, 24);
  want("pipe", st, 14, 1);
  n = take("pipe-db-allocs", &st, proto, evDbOk());
  if (n != 1) bad("pipe-db", "allocs");
  want("pipe-db", st, 14, 1);
  n = take("pipe-idle-allocs", &st, proto, evDone());
  if (n != 0) bad("pipe-idle", "allocs");
  want("pipe-idle", st, 0, PH_IDLE);
  want("pipe-idle", st, 14, 1);
  if (proto_done(proto)) bad("pipe-idle", "session ended on keep");
  n = take("pipe-tail-allocs", &st, proto, evBytes(of(piped + 24)));
  if (n != 1) bad("pipe-tail", "allocs");
  want("pipe-tail", st, 0, PH_DECIDE);
  want("pipe-tail", st, 4, 1);
  want("pipe-tail", st, 14, 0);
  if (proto_done(proto)) bad("pipe-tail", "session ended before the reply");
  proto_free(proto);
}

static void check_closed(void) {
  Proto *proto = proto_shut();
  Conn st;
  Conn again;
  unsigned long n;
  muro_http_reset();
  st = shut();
  if (muro_http_allocs() != 0) bad("shut", "allocs");
  if (proto_live(proto)) bad("shut", "session live");
  n = take("closed-bytes-allocs", &st, proto, evBytes(of(health)));
  if (n != 0) bad("closed-bytes", "allocs");
  again = st;
  n = take("closed-eof-allocs", &st, proto, evEof(of(health)));
  if (n != 0 || !same(st, again)) bad("closed-eof", "state");
  n = take("closed-progress-allocs", &st, proto, evProgress(8));
  if (n != 0 || !same(st, again)) bad("closed-progress", "state");
  n = take("closed-done-allocs", &st, proto, evDone());
  if (n != 0 || !same(st, again)) bad("closed-done", "state");
  n = take("closed-error-allocs", &st, proto, evError());
  if (n != 0 || !same(st, again)) bad("closed-error", "state");
  n = take("closed-tick-allocs", &st, proto, evTick());
  if (n != 0 || !same(st, again)) bad("closed-tick", "state");
  n = take("closed-db-allocs", &st, proto, evDbOk());
  if (n != 0 || !same(st, again)) bad("closed-db", "state");
  want("closed", st, 0, PH_CLOSED);
  want("closed", st, 6, ACT_CLOSE);
  proto_free(proto);
}

static void check_write_error(void) {
  Proto *proto = proto_begin();
  Conn st = begin();
  unsigned long n;
  (void)take("werr-bytes-allocs", &st, proto, evBytes(of(health)));
  (void)take("werr-db-allocs", &st, proto, evDbOk());
  n = take("werr-allocs", &st, proto, evError());
  if (n != 0) bad("werr", "allocs");
  want("werr", st, 0, PH_CLOSED);
  if (!proto_done(proto)) bad("werr", "session open");
  proto_free(proto);
}

static void check_ignore_tick_while_writing(void) {
  Proto *proto = proto_begin();
  Conn st = begin();
  unsigned long n;
  (void)take("wtick-bytes-allocs", &st, proto, evBytes(of(health)));
  (void)take("wtick-db-allocs", &st, proto, evDbOk());
  n = take("wtick-allocs", &st, proto, evTick());
  if (n != 0) bad("wtick", "allocs");
  want("wtick", st, 0, PH_WRITE);
  want("wtick", st, 6, ACT_WRITE);
  if (proto_done(proto)) bad("wtick", "session ended on a host tick");
  proto_free(proto);
}

/* A refused admit leaves the session where it was and does not run the
   image. A later idle tick is still a legal protocol move. */
static void check_gate(void) {
  Proto *proto = proto_begin();
  Conn st = begin();
  Conn again = st;
  unsigned long n;
  if (proto_admit(proto, PH_READ, 0, 4, -1, -1, 0, 0))
    bad("gate", "unclassified bytes admitted");
  if (!same(st, again)) bad("gate", "image moved on a refusal");
  if (proto_done(proto)) bad("gate", "refusal ended the session");
  n = take("read-tick-allocs", &st, proto, evTick());
  if (n != 0) bad("read-tick", "allocs");
  want("read-tick", st, 0, PH_CLOSED);
  want("read-tick", st, 6, ACT_CLOSE);
  if (!proto_done(proto)) bad("read-tick", "session open");
  again = st;
  n = take("after-tick-bytes-allocs", &st, proto, evBytes(of(health)));
  if (n != 0 || !same(st, again)) bad("after-tick-bytes", "state");
  if (!proto_refuses(proto, PROTO_RECV, PROTO_DATA, 0, "piece-req"))
    bad("after-tick", "piece accepted after end");
  proto_free(proto);
}

static void check_fin(void) {
  Proto *proto = proto_begin();
  Conn st = begin();
  unsigned long n = take("bare-fin-allocs", &st, proto, evEof(of("")));
  if (n != 0) bad("bare-fin", "allocs");
  want("bare-fin", st, 0, PH_CLOSED);
  want("bare-fin", st, 6, ACT_CLOSE);
  if (!proto_done(proto)) bad("bare-fin", "session open");
  proto_free(proto);
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
  check_gate();
  check_fin();
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
