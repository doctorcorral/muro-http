/* Drive the generated connection step. The shim only builds slices
   and counts muro__ALLOC. Parsing and the HTTP image are muro.
   Protocol moves go through c/proto.c, which applies the emitted
   admit choice and calls the stepper before the image runs. A refused move
   does not run the image. proto_begin and proto_shut return NULL
   on allocation or init failure. That NULL is a failed test, and
   the image is not stepped. */

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
  default: return 0;
  }
}

static int same(Conn a, Conn b) {
  int i;
  if (a.muro__field_tag != b.muro__field_tag) return 0;
  for (i = 0; i < 13; i++) {
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

void step_memo_reset(void);
void view_memo_reset(void);

static int restart(Conn *st, Proto **proto) {
  proto_free(*proto);
  *proto = NULL;
  step_memo_reset();
  *st = begin();
  *proto = proto_begin();
  if (!*proto) {
    bad("restart", "session init");
    return 0;
  }
  return 1;
}

static Proto *open_proto(const char *name) {
  Proto *p;
  /* A repeated pure step does not classify again. Start each scenario
     cold so the first bytes event still shows that one allocation. */
  step_memo_reset();
  p = proto_begin();
  if (!p) bad(name, "session init");
  return p;
}

/* Probe, then reset. classify runs here only so proto_admit can see
   the status tag. The tag is a plain integer copied out before
   muro_http_reset, which drops the probe. step classifies again, and
   that second call is what the printed count measures. A control step
   stays 0, a classify stays 1, and a handler reply stays 0. */
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

  if (!proto) {
    bad(name, "no session");
    return 0;
  }

  if (proto_live(proto) && !proto_done(proto) && (phase == PH_READ || phase == PH_IDLE) &&
      (tag == 0 || (tag == 1 && buf.n > 0))) {
    Classified *c = classify(buf);
    status = c->muro__as.classified.muro__f_0->muro__field_tag;
  }
  /* Tag 6 is the handler reply. n1 is 1 for HTTP 400..599, admitted
     as reply-bad. Any other status is reply-ok. reply -1 would offer
     no reply at all. */
  if (proto_live(proto) && !proto_done(proto) && phase == PH_DECIDE && tag == 6) {
    uint64_t bad_reply = ev.muro__as.ev.muro__f_3;
    reply = bad_reply == 0 ? 0 : 1;
    observe = 0;
  }
  if (proto_admit(proto, phase, tag, buf.n, status, reply, observe, close_after) != 1) {
    proto_where(proto, why, sizeof why);
    fprintf(stderr, "FAIL %s: protocol refused, at %s\n", name, why);
    failures += 1;
    return 0;
  }

  /* Build the event, then count only the step. Event builders are unboxed.
     The probe above may have remembered this buffer. Drop that memory
     so this step still classifies once. */
  muro_http_reset();
  (void)ev;
  built = muro_http_allocs();
  if (built != 0) bad(name, "event allocated");
  muro_http_reset();
  view_memo_reset();
  *st = step(*st, ev);
  n = muro_http_allocs();
  printf("%s %lu\n", name, n);
  return n;
}

static const char *health = "GET /health HTTP/1.1\r\nHost: t\r\n\r\n";
static const char *feed = "GET /feed HTTP/1.1\r\nHost: t\r\n\r\n";
static const char *partial = "GET /health HTTP/1.1\r\n";
static const char *close_req = "GET /health HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
static const char *http10 = "GET /health HTTP/1.0\r\n\r\n";
static const char *bad_req = "GET/feed HTTP/1.1\r\n\r\n";
static const char *piped =
    "GET /health HTTP/1.1\r\nHost: t\r\n\r\nGET /feed HTTP/1.1\r\nHost: t\r\n\r\n";

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
  want("begin", st, 4, ACT_READ);
  want("begin", st, 5, 1);
  muro_http_reset();
  proto = proto_begin();
  n = muro_http_allocs();
  printf("session-init-allocs %lu\n", n);
  if (n != 0) bad("session-init", "allocs");
  if (!proto) {
    bad("session-init", "init");
    return;
  }
  if (!proto_refuses(proto, PROTO_SEND, PROTO_DATA, 0, "reply-ok"))
    bad("session-init", "reply accepted before a piece");
  if (!proto_refuses(proto, PROTO_RECV, PROTO_PICK, 0, 0))
    bad("session-init", "recv pick accepted at the first choice");
  if (!proto_refuses(proto, PROTO_SEND, PROTO_PICK, 2, 0))
    bad("session-init", "pick 2 accepted");
  if (!proto_refuses(proto, PROTO_SEND, PROTO_PICK, 256, 0))
    bad("session-init", "pick 256 accepted");
  proto_free(proto);
}

static void check_need(void) {
  Proto *proto = open_proto("need");
  Conn st = begin();
  unsigned long n;
  if (!proto) return;
  n = take("need-allocs", &st, proto, evBytes(of(partial)));
  if (n != 1) bad("need", "allocs");
  want("need", st, 0, PH_READ);
  want("need", st, 4, ACT_READ);
  if (proto_done(proto)) bad("need", "session ended on a chunk");
  n = take("need-rest-allocs", &st, proto, evBytes(of(health)));
  if (n != 1) bad("need-rest", "allocs");
  want("need-rest", st, 0, PH_DECIDE);
  want("need-rest", st, 4, ACT_DECIDE);
  if (proto_done(proto)) bad("need-rest", "session ended before the reply");
  proto_free(proto);
}

static void check_keepalive(void) {
  Proto *proto = open_proto("health");
  Conn st = begin();
  unsigned long n;
  if (!proto) return;
  n = take("health-allocs", &st, proto, evBytes(of(health)));
  if (n != 1) bad("health", "allocs");
  want("health", st, 0, PH_DECIDE);
  want("health", st, 1, 0);
  want("health", st, 2, 0);
  want("health", st, 4, ACT_DECIDE);
  want("health", st, 11, 33);
  want("health", st, 12, 0);
  /* The same bytes and the same fields skip the book. The result matches. */
  {
    Conn hit;
    muro_http_reset();
    hit = step(begin(), evBytes(of(health)));
    if (muro_http_allocs() != 0) bad("health", "repeat allocs");
    if (!same(hit, st)) bad("health", "repeat result");
  }
  {
    muro__slice s = of(health);
    Classified *a;
    Classified *b;
    muro__slice body;
    muro__slice body2;
    uint64_t code;
    unsigned long n0;
    int status;
    int keep;
    view_memo_reset();
    muro_http_reset();
    a = classify(s);
    status = a->muro__as.classified.muro__f_0->muro__field_tag;
    keep = a->muro__as.classified.muro__f_1->muro__field_tag;
    muro_http_reset();
    n0 = muro_http_allocs();
    b = classify(s);
    if (muro_http_allocs() != n0) bad("health", "classify hit allocs");
    if (b->muro__as.classified.muro__f_0->muro__field_tag != status) bad("health", "classify status");
    if (b->muro__as.classified.muro__f_1->muro__field_tag != keep) bad("health", "classify keep");
    code = methodCode(s);
    muro_http_reset();
    if (methodCode(s) != code) bad("health", "method hit");
    body = bodyView(s);
    muro_http_reset();
    body2 = bodyView(s);
    if (body2.n != body.n) bad("health", "body hit");
    if (body.n > 0 && (body2.p != s.p + (size_t)(body.p - s.p) ||
                       memcmp(body2.p, body.p, body.n) != 0))
      bad("health", "body hit");
  }
  n = take("reply-allocs", &st, proto, evReply(0));
  if (n != 0) bad("reply", "allocs");
  want("db", st, 0, PH_WRITE);
  want("db", st, 4, ACT_WRITE);
  want("db", st, 6, 0);
  want("db", st, 7, 0);
  want("db", st, 8, 0);
  want("db", st, 9, 0);
  want("db", st, 10, 16);
  n = take("progress-allocs", &st, proto, evProgress(8));
  if (n != 0) bad("progress", "allocs");
  want("progress", st, 0, PH_WRITE);
  want("progress", st, 9, 8);
  n = take("progress-full-allocs", &st, proto, evProgress(8));
  if (n != 0) bad("progress-full", "allocs");
  want("progress-full", st, 9, 16);
  n = take("idle-allocs", &st, proto, evDone());
  if (n != 0) bad("idle", "allocs");
  want("idle", st, 0, PH_IDLE);
  want("idle", st, 4, ACT_READ);
  want("idle", st, 5, 1);
  n = take("second-allocs", &st, proto, evBytes(of(feed)));
  if (n != 1) bad("second", "allocs");
  want("second", st, 0, PH_DECIDE);
  n = take("second-reply-allocs", &st, proto, evReply(0));
  if (n != 0) bad("second-reply", "allocs");
  want("second-reply", st, 8, 0);
  n = take("second-idle-allocs", &st, proto, evDone());
  if (n != 0) bad("second-idle", "allocs");
  want("second-idle", st, 0, PH_IDLE);
  n = take("idle-tick-allocs", &st, proto, evTick());
  if (n != 0) bad("idle-tick", "allocs");
  want("idle-tick", st, 0, PH_CLOSED);
  want("idle-tick", st, 4, ACT_CLOSE);
  if (!proto_done(proto)) bad("idle-tick", "session open");
  if (!proto_refuses(proto, PROTO_RECV, PROTO_DATA, 0, "piece-req"))
    bad("idle-tick", "piece accepted after end");
  proto_free(proto);
}

static void check_close_header(const char *name, const char *raw) {
  Proto *proto = open_proto(name);
  Conn st = begin();
  char label[64];
  unsigned long n;
  if (!proto) return;
  snprintf(label, sizeof label, "%s-bytes", name);
  n = take(label, &st, proto, evBytes(of(raw)));
  if (n != 1) bad(name, "bytes allocs");
  want(name, st, 2, 1);
  if (proto_done(proto)) bad(name, "session ended before the reply");
  snprintf(label, sizeof label, "%s-db", name);
  n = take(label, &st, proto, evReply(0));
  if (n != 0) bad(name, "reply allocs");
  snprintf(label, sizeof label, "%s-done", name);
  n = take(label, &st, proto, evDone());
  if (n != 0) bad(name, "done allocs");
  want(name, st, 0, PH_CLOSED);
  want(name, st, 4, ACT_CLOSE);
  if (!proto_done(proto)) bad(name, "session open after close");
  proto_free(proto);
}

static void check_malformed(void) {
  Proto *proto = open_proto("malformed");
  Conn st = begin();
  unsigned long n;
  if (!proto) return;
  n = take("malformed-allocs", &st, proto, evBytes(of(bad_req)));
  if (n != 1) bad("malformed", "allocs");
  want("malformed", st, 0, PH_WRITE);
  want("malformed", st, 2, 1);
  want("malformed", st, 4, ACT_WRITE);
  want("malformed", st, 6, 1);
  want("malformed", st, 7, 0);
  want("malformed", st, 8, 0);
  if (!proto_done(proto)) bad("malformed", "session open");
  n = take("malformed-done-allocs", &st, proto, evDone());
  if (n != 0) bad("malformed-done", "allocs");
  want("malformed-done", st, 0, PH_CLOSED);
  if (!proto_done(proto)) bad("malformed-done", "session open");
  if (!proto_refuses(proto, PROTO_SEND, PROTO_DATA, 0, "reply-ok"))
    bad("malformed-done", "reply accepted after end");
  proto_free(proto);
}

static void check_unimpl(void) {
  Proto *proto = open_proto("unimpl");
  Conn st = begin();
  unsigned long n;
  const char *gzip = "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip\r\n\r\n";
  if (!proto) return;
  n = take("unimpl-allocs", &st, proto, evBytes(of(gzip)));
  if (n != 1) bad("unimpl", "allocs");
  want("unimpl", st, 0, PH_WRITE);
  want("unimpl", st, 2, 1);
  want("unimpl", st, 4, ACT_WRITE);
  want("unimpl", st, 6, 2);
  if (!proto_done(proto)) bad("unimpl", "session open");
  n = take("unimpl-done-allocs", &st, proto, evDone());
  if (n != 0) bad("unimpl-done", "allocs");
  want("unimpl-done", st, 0, PH_CLOSED);
  if (!proto_done(proto)) bad("unimpl-done", "session open");
  proto_free(proto);
}

static void check_half(void) {
  Proto *proto = open_proto("half-bytes");
  Conn st = begin();
  Conn fin;
  unsigned long n;
  if (!proto) return;
  n = take("half-bytes-allocs", &st, proto, evBytes(of(health)));
  if (n != 1) bad("half-bytes", "allocs");
  if (proto_done(proto)) bad("half-bytes", "session ended before the reply");
  /* The FIN arrives while the reply is already in progress. That is the
     image's half flag. The session chose the open branch before the piece. */
  n = take("half-eof-allocs", &st, proto, evEof(of("")));
  if (n != 0) bad("half-eof", "allocs");
  want("half-eof", st, 0, PH_DECIDE);
  want("half-eof", st, 1, 1);
  want("half-eof", st, 2, 1);
  want("half-eof", st, 4, ACT_DECIDE);
  if (proto_done(proto)) bad("half-eof", "session ended on a host flag");
  n = take("half-reply-allocs", &st, proto, evReply(0));
  if (n != 0) bad("half-reply", "allocs");
  want("half-db", st, 0, PH_WRITE);
  want("half-db", st, 1, 1);
  n = take("half-progress-allocs", &st, proto, evProgress(8));
  if (n != 0) bad("half-progress", "allocs");
  want("half-progress", st, 1, 1);
  want("half-progress", st, 9, 8);
  n = take("half-done-allocs", &st, proto, evDone());
  if (n != 0) bad("half-done", "allocs");
  want("half-done", st, 0, PH_CLOSED);
  if (!proto_done(proto)) bad("half-done", "session open");
  if (!restart(&fin, &proto)) return;
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
  if (!restart(&fin, &proto)) return;
  n = take("trunc-allocs", &fin, proto, evEof(of(partial)));
  if (n != 1) bad("trunc", "allocs");
  want("trunc", fin, 0, PH_WRITE);
  want("trunc", fin, 1, 1);
  want("trunc", fin, 6, 1);
  if (!proto_done(proto)) bad("trunc", "session open");
  proto_free(proto);
}

static void check_pipe(void) {
  Proto *proto = open_proto("pipe");
  Conn st = begin();
  unsigned long n;
  if (!proto) return;
  n = take("pipe-allocs", &st, proto, evBytes(of(piped)));
  if (n != 1) bad("pipe", "allocs");
  want("pipe", st, 0, PH_DECIDE);
  want("pipe", st, 11, 33);
  want("pipe", st, 12, 1);
  n = take("pipe-reply-allocs", &st, proto, evReply(0));
  if (n != 0) bad("pipe-reply", "allocs");
  want("pipe-db", st, 12, 1);
  n = take("pipe-idle-allocs", &st, proto, evDone());
  if (n != 0) bad("pipe-idle", "allocs");
  want("pipe-idle", st, 0, PH_IDLE);
  want("pipe-idle", st, 12, 1);
  if (proto_done(proto)) bad("pipe-idle", "session ended on keep");
  n = take("pipe-tail-allocs", &st, proto, evBytes(of(piped + 33)));
  if (n != 1) bad("pipe-tail", "allocs");
  want("pipe-tail", st, 0, PH_DECIDE);
  want("pipe-tail", st, 12, 0);
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
  if (!proto) {
    bad("shut", "alloc");
    return;
  }
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
  n = take("closed-reply-allocs", &st, proto, evReply(0));
  if (n != 0 || !same(st, again)) bad("closed-db", "state");
  want("closed", st, 0, PH_CLOSED);
  want("closed", st, 4, ACT_CLOSE);
  proto_free(proto);
}

static void check_write_error(void) {
  Proto *proto = open_proto("werr");
  Conn st = begin();
  unsigned long n;
  if (!proto) return;
  (void)take("werr-bytes-allocs", &st, proto, evBytes(of(health)));
  (void)take("werr-reply-allocs", &st, proto, evReply(0));
  n = take("werr-allocs", &st, proto, evError());
  if (n != 0) bad("werr", "allocs");
  want("werr", st, 0, PH_CLOSED);
  if (!proto_done(proto)) bad("werr", "session open");
  proto_free(proto);
}

static void check_ignore_tick_while_writing(void) {
  Proto *proto = open_proto("wtick");
  Conn st = begin();
  unsigned long n;
  if (!proto) return;
  (void)take("wtick-bytes-allocs", &st, proto, evBytes(of(health)));
  (void)take("wtick-reply-allocs", &st, proto, evReply(0));
  n = take("wtick-allocs", &st, proto, evTick());
  if (n != 0) bad("wtick", "allocs");
  want("wtick", st, 0, PH_WRITE);
  want("wtick", st, 4, ACT_WRITE);
  if (proto_done(proto)) bad("wtick", "session ended on a host tick");
  proto_free(proto);
}

/* A refused admit leaves the session where it was and does not run the
   image. A later idle tick is still a legal protocol move. */
static void check_gate(void) {
  Proto *proto = open_proto("gate");
  Conn st = begin();
  Conn again = st;
  unsigned long n;
  if (!proto) return;
  if (proto_admit(proto, PH_READ, 0, 4, -1, -1, 0, 0))
    bad("gate", "unclassified bytes admitted");
  if (!same(st, again)) bad("gate", "image moved on a refusal");
  if (proto_done(proto)) bad("gate", "refusal ended the session");
  n = take("read-tick-allocs", &st, proto, evTick());
  if (n != 0) bad("read-tick", "allocs");
  want("read-tick", st, 0, PH_CLOSED);
  want("read-tick", st, 4, ACT_CLOSE);
  if (!proto_done(proto)) bad("read-tick", "session open");
  again = st;
  n = take("after-tick-bytes-allocs", &st, proto, evBytes(of(health)));
  if (n != 0 || !same(st, again)) bad("after-tick-bytes", "state");
  if (!proto_refuses(proto, PROTO_RECV, PROTO_DATA, 0, "piece-req"))
    bad("after-tick", "piece accepted after end");
  proto_free(proto);
}

static void check_fin(void) {
  Proto *proto = open_proto("bare-fin");
  Conn st = begin();
  unsigned long n;
  if (!proto) return;
  n = take("bare-fin-allocs", &st, proto, evEof(of("")));
  if (n != 0) bad("bare-fin", "allocs");
  want("bare-fin", st, 0, PH_CLOSED);
  want("bare-fin", st, 4, ACT_CLOSE);
  if (!proto_done(proto)) bad("bare-fin", "session open");
  proto_free(proto);
}

static int last_ctor(Proto *proto, const char *want) {
  ProtoMove mv;
  int n = proto_move_count(proto);
  if (n < 1 || !proto_move_at(proto, n - 1, &mv) || !mv.ctor) return 0;
  return strcmp(mv.ctor, want) == 0;
}

/* reply-bad is the Reply word. The session then chooses keep or
   close, so it is not done until the write ends with keep set. */
static void check_reply_bad(void) {
  Conn st;
  Proto *proto;
  unsigned long n;
  muro_http_reset();
  st = begin();
  proto = open_proto("reply-bad");
  if (!proto) return;
  n = take("reply-bad-req", &st, proto, evBytes(of(health)));
  if (n != 1) bad("reply-bad", "classify allocs");
  if (proto_done(proto)) bad("reply-bad", "done before reply");
  n = take("reply-bad-reply", &st, proto, evReplyOf(1, 1));
  if (n != 0) bad("reply-bad", "reply allocs");
  want("reply-bad", st, 0, PH_WRITE);
  want("reply-bad", st, 2, 1);
  if (!last_ctor(proto, "reply-bad")) bad("reply-bad", "reply-ok");
  if (proto_done(proto)) bad("reply-bad", "done before the close choice");
  n = take("reply-bad-done", &st, proto, evDone());
  if (n != 0) bad("reply-bad", "done allocs");
  want("reply-bad-done", st, 0, PH_CLOSED);
  if (!proto_done(proto)) bad("reply-bad", "session open after close");
  proto_free(proto);

  muro_http_reset();
  st = begin();
  proto = open_proto("reply-n1");
  if (!proto) return;
  take("reply-n1-req", &st, proto, evBytes(of(health)));
  take("reply-n1", &st, proto, evReplyOf(0, 1));
  want("reply-n1", st, 0, PH_WRITE);
  want("reply-n1", st, 2, 1);
  if (!last_ctor(proto, "reply-bad")) bad("reply-n1", "reply-ok");
  take("reply-n1-done", &st, proto, evDone());
  want("reply-n1-done", st, 0, PH_CLOSED);
  if (!proto_done(proto)) bad("reply-n1", "session open after close");
  proto_free(proto);
}

int main(void) {
  check_begin();
  check_need();
  check_keepalive();
  check_close_header("close", close_req);
  check_close_header("http10", http10);
  check_malformed();
  check_unimpl();
  check_half();
  check_pipe();
  check_closed();
  check_write_error();
  check_ignore_tick_while_writing();
  check_gate();
  check_fin();
  check_reply_bad();
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
