/* A backend that is not a socket. It reports the same facts the
   poll adapter reports: a connection was accepted, bytes are in the
   recv buffer, a write of n bytes finished, the peer closed, and
   the clock. It performs the actions the step returns. The bytes
   to write are the step's pointer; this file does not copy them
   before recording them.

   The checks pin short writes, a hard write error, 100 Continue
   (including a short write then a hard error), MURO_STEP_NOW and
   wake_at, status_now for 413, 417, and 501, 414 and 431 for a
   header block past 16384, pipelining, empty_before 0, a
   one-byte window on a large body, and that a closed 413, 417,
   or 501 does not answer a later fact.

   Build: make c/test_step
   The reference server is still c/host.c. This file is the pattern,
   not a second production loop. */

#include "step.h"
#include "host_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IN_CAP = 65536, OUT_CAP = 69632, GOT_CAP = 131072, DRIVE_MAX = 250000 };
enum { BLOB_N = 100001 };

static const char DATE[] = "Sun, 06 Nov 1994 08:49:37 GMT";
static size_t blob_left;

static int pull_blob(uint8_t *dst, size_t cap, void *user) {
  size_t *left = user;
  size_t n;
  if (!left) return -1;
  n = cap;
  if (n > *left) n = *left;
  if (n == 0) return 0;
  memset(dst, 'x', n);
  *left -= n;
  return (int)n;
}

static int pull_req(uint8_t *dst, size_t cap, void *user) {
  if (!user) return 0;
  return muro_http_body_read(user, dst, cap);
}

static int path_is(const MuroHttpRequest *req, const char *lit) {
  size_t n = strlen(lit);
  return req->path.n == n && req->path.p && memcmp(req->path.p, lit, n) == 0;
}

static int on_req(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  static const char type[] = "Content-Type: text/plain\r\n";
  (void)user;
  resp->headers = type;
  resp->headers_len = sizeof type - 1;
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
  if (req->method.n == 3 && req->method.p && memcmp(req->method.p, "GET", 3) == 0) {
    if (path_is(req, "/blob")) {
      resp->pull = pull_blob;
      resp->pull_user = &blob_left;
      resp->has_length = 1;
      resp->content_length = blob_left;
      return 0;
    }
    resp->body = (const uint8_t *)"hello\n";
    resp->body_len = 6;
    return 0;
  }
  if (req->method.n == 4 && req->method.p && memcmp(req->method.p, "POST", 4) == 0 &&
      path_is(req, "/echo")) {
    if (req->body.p && req->body.n) {
      resp->body = req->body.p;
      resp->body_len = req->body.n;
      return 0;
    }
    resp->pull = pull_req;
    resp->pull_user = req->body_in;
    return 0;
  }
  resp->status = 405;
  resp->reason = "Method Not Allowed";
  resp->reason_len = 18;
  resp->body = (const uint8_t *)"no\n";
  resp->body_len = 3;
  resp->close_after = 1;
  return 0;
}

typedef struct Box {
  uint8_t *inb;
  uint8_t *outb;
  uint8_t *got;
  size_t got_n;
  size_t got_cap;
  HttpStep st;
  struct HostIn hin;
  int rd;
  int wr;
  int closed;
  StepIn in;
  const char *err;
} Box;

static int fail(const char *what) {
  fprintf(stderr, "mem adapter: %s\n", what);
  return 1;
}

static void set_fact(Box *b, int kind, size_t n) {
  uint64_t now = b->in.now_ms ? b->in.now_ms : 1000;
  uint64_t dl = b->in.deadline;
  memset(&b->in, 0, sizeof b->in);
  b->in.kind = kind;
  b->in.n = n;
  b->in.now_ms = now;
  b->in.deadline = dl;
  b->in.date = (const uint8_t *)DATE;
  b->in.date_n = 29;
}

static int span_ok(const Box *b, const StepOut *o) {
  if (o->cont) {
    const uint8_t *base = b->st.cont;
    if (!o->bytes || o->bytes < base || o->bytes + o->len > base + sizeof b->st.cont) return -1;
  } else if (o->bytes && o->len) {
    if (o->bytes < b->outb || o->bytes + o->len > b->outb + OUT_CAP) return -1;
  }
  if (o->status && o->status_len) {
    const uint8_t *spare = b->st.spare;
    int in_spare = o->status >= spare && o->status + o->status_len <= spare + sizeof b->st.spare;
    int in_out = o->status >= b->outb && o->status + o->status_len <= b->outb + OUT_CAP;
    if (!in_spare && !in_out) return -1;
  }
  return 0;
}

static int call(Box *b, StepOut *o) {
  if (muro_http_step(&b->st, &b->in, o) < 0) {
    b->err = "step";
    return -1;
  }
  if (span_ok(b, o) < 0) {
    b->err = "pointer";
    return -1;
  }
  return 0;
}

static int take(Box *b, const StepOut *o, size_t n) {
  if (!o->bytes || n == 0 || n > o->len || b->got_n + n > b->got_cap) {
    b->err = "take";
    return -1;
  }
  memcpy(b->got + b->got_n, o->bytes, n);
  b->got_n += n;
  set_fact(b, MURO_STEP_WROTE, n);
  return 0;
}

static int feed(Box *b, const char *p, int empty, uint64_t now, uint64_t deadline) {
  size_t n = strlen(p);
  size_t cur = muro_http_step_in_len(&b->st);
  if (cur + n > IN_CAP) {
    b->err = "feed";
    return -1;
  }
  memcpy(b->inb + cur, p, n);
  muro_http_step_set_in_len(&b->st, cur + n);
  memset(&b->in, 0, sizeof b->in);
  b->in.kind = MURO_STEP_BYTES;
  b->in.empty_before = empty;
  b->in.now_ms = now;
  b->in.deadline = deadline;
  b->in.date = (const uint8_t *)DATE;
  b->in.date_n = 29;
  return 0;
}

/* 0 when the step is waiting for bytes or the connection closed.
   A returned span is accepted `limit` bytes at a time. limit 0
   accepts the whole span, the way a socket that takes the buffer
   does. */
static int drive(Box *b, size_t limit) {
  int steps = 0;
  while (steps < DRIVE_MAX) {
    StepOut o;
    size_t n;
    steps += 1;
    if (call(b, &o) < 0) return -1;
    if (o.close_conn) {
      if (o.status_now && o.status && o.status_len) {
        if (b->got_n + o.status_len > b->got_cap) {
          b->err = "status cap";
          return -1;
        }
        memcpy(b->got + b->got_n, o.status, o.status_len);
        b->got_n += o.status_len;
      }
      b->closed = 1;
      return 0;
    }
    if (o.bytes && o.len) {
      n = o.len;
      if (limit && n > limit) n = limit;
      if (take(b, &o, n) < 0) return -1;
      continue;
    }
    if (!o.read_more) {
      b->err = "stuck";
      return -1;
    }
    return 0;
  }
  b->err = "drive fuel";
  return -1;
}

static int saw(const Box *b, const char *lit) {
  size_t n = strlen(lit);
  return b->got_n >= n && memmem(b->got, b->got_n, lit, n) != NULL;
}

static int count_lit(const uint8_t *p, size_t n, const char *lit) {
  size_t ln = strlen(lit);
  size_t i;
  int c = 0;
  if (ln == 0 || n < ln) return 0;
  for (i = 0; i + ln <= n; i++) {
    if (memcmp(p + i, lit, ln) == 0) c += 1;
  }
  return c;
}

static void box_close(Box *b) {
  muro_http_step_shutdown(&b->st);
  host_in_close(&b->hin);
  free(b->inb);
  free(b->outb);
  free(b->got);
  b->inb = NULL;
  b->outb = NULL;
  b->got = NULL;
}

static int box_open(Box *b, uint64_t max_body) {
  StepOut o;
  memset(b, 0, sizeof *b);
  b->inb = malloc(IN_CAP);
  b->outb = malloc(OUT_CAP);
  b->got = malloc(GOT_CAP);
  b->got_cap = GOT_CAP;
  if (!b->inb || !b->outb || !b->got) {
    box_close(b);
    return -1;
  }
  host_in_init(&b->hin, -1, max_body);
  muro_http_step_bind(&b->st, b->inb, IN_CAP, b->outb, OUT_CAP, &b->hin, &b->rd, &b->wr, on_req,
                      NULL, max_body, 60000);
  memset(&b->in, 0, sizeof b->in);
  b->in.kind = MURO_STEP_ACCEPTED;
  b->in.now_ms = 1000;
  b->in.date = (const uint8_t *)DATE;
  b->in.date_n = 29;
  if (call(b, &o) < 0 || o.close_conn || !b->rd) {
    b->err = b->err ? b->err : "accept";
    return -1;
  }
  return 0;
}

static int check_basic(void) {
  Box b;
  const char *req = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
  const char *bad = "GET / HTTP/1.1\r\nHost: localhost\r\n";
  if (box_open(&b, 8388608) < 0) return fail(b.err ? b.err : "alloc");
  if (feed(&b, req, 1, 1000, 0) < 0 || drive(&b, 0) < 0 || b.closed) {
    fail(b.err ? b.err : "keepalive");
    box_close(&b);
    return 1;
  }
  if (!saw(&b, "HTTP/1.1 200") || !saw(&b, "hello\n") || !saw(&b, DATE) ||
      !saw(&b, "Content-Length: 6")) {
    fail("keepalive bytes");
    box_close(&b);
    return 1;
  }
  if (muro_http_step_in_len(&b.st) != 0) {
    fail("keepalive leftover");
    box_close(&b);
    return 1;
  }
  b.got_n = 0;
  if (feed(&b, req, 1, 1100, 0) < 0 || drive(&b, 0) < 0 || b.closed || !saw(&b, "hello\n")) {
    fail(b.err ? b.err : "second");
    box_close(&b);
    return 1;
  }
  b.got_n = 0;
  b.closed = 0;
  if (feed(&b, bad, 1, 1200, 0) < 0) {
    fail("peer feed");
    box_close(&b);
    return 1;
  }
  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  b.in.empty_before = 1;
  b.in.now_ms = 1200;
  if (drive(&b, 0) < 0 || !b.closed || !saw(&b, "400") || !saw(&b, "malformed")) {
    fail(b.err ? b.err : "peer");
    box_close(&b);
    return 1;
  }
  box_close(&b);
  return 0;
}

/* A hard error on a 100 closes in the adapter. The step itself, if
   told WRITE_FAILED, parks the same span again. That is the spin
   the socket adapter must not enter. */
static int check_cont(void) {
  Box b;
  StepOut o;
  const char *hdr =
      "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n";
  const uint8_t *p0;
  size_t n0;
  if (box_open(&b, 8388608) < 0) return fail(b.err ? b.err : "cont alloc");
  if (feed(&b, hdr, 1, 1000, 0) < 0 || call(&b, &o) < 0) {
    fail(b.err ? b.err : "cont");
    box_close(&b);
    return 1;
  }
  if (!o.cont || !o.interim || o.close_conn || o.len < 16 || o.wake_at != 61000) {
    fail("cont first");
    box_close(&b);
    return 1;
  }
  p0 = o.bytes;
  n0 = o.len;
  if (take(&b, &o, 8) < 0 || call(&b, &o) < 0) {
    fail(b.err ? b.err : "cont short");
    box_close(&b);
    return 1;
  }
  if (!o.cont || o.interim || o.close_conn || o.bytes != p0 + 8 || o.len != n0 - 8 ||
      o.wake_at != 61000) {
    fail("cont durable");
    box_close(&b);
    return 1;
  }
  /* The adapter closes here. It does not report WRITE_FAILED. */
  if (!o.cont) {
    fail("cont policy");
    box_close(&b);
    return 1;
  }
  set_fact(&b, MURO_STEP_WRITE_FAILED, 0);
  if (call(&b, &o) < 0) {
    fail(b.err ? b.err : "cont fail fact");
    box_close(&b);
    return 1;
  }
  if (o.close_conn || !o.cont || o.interim || o.bytes != p0 + 8 || o.len != n0 - 8) {
    fail("cont fail repark");
    box_close(&b);
    return 1;
  }
  box_close(&b);
  return 0;
}

static int check_now(void) {
  Box b;
  StepOut o;
  const char *hdr =
      "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n";
  if (box_open(&b, 8388608) < 0) return fail(b.err ? b.err : "now alloc");
  if (feed(&b, hdr, 1, 1000, 0) < 0 || call(&b, &o) < 0 || !o.cont || o.wake_at != 61000) {
    fail(b.err ? b.err : "now park");
    box_close(&b);
    return 1;
  }
  if (take(&b, &o, 8) < 0 || call(&b, &o) < 0 || !o.cont || o.interim) {
    fail(b.err ? b.err : "now partial");
    box_close(&b);
    return 1;
  }
  set_fact(&b, MURO_STEP_NOW, 0);
  b.in.now_ms = 61000;
  b.in.deadline = 61000;
  if (call(&b, &o) < 0) {
    fail(b.err ? b.err : "now step");
    box_close(&b);
    return 1;
  }
  if (!o.close_conn || o.wake_at != 0 || (o.bytes && o.len) || o.cont) {
    fail("now close");
    box_close(&b);
    return 1;
  }
  box_close(&b);
  return 0;
}

static int check_empty_before(void) {
  Box b;
  StepOut o;
  const char *part = "GET / HTTP/1.1\r\nHos";
  const char *rest = "t: localhost\r\n\r\n";
  const char *hdr =
      "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n";
  const uint8_t *p0;
  size_t n0;
  if (box_open(&b, 8388608) < 0) return fail(b.err ? b.err : "empty alloc");
  if (feed(&b, part, 1, 1000, 0) < 0 || call(&b, &o) < 0) {
    fail(b.err ? b.err : "empty partial");
    box_close(&b);
    return 1;
  }
  if (o.bytes || o.close_conn || !o.read_more) {
    fail("empty wait");
    box_close(&b);
    return 1;
  }
  if (feed(&b, rest, 0, 1000, 0) < 0 || drive(&b, 0) < 0 || b.closed || !saw(&b, "hello\n")) {
    fail(b.err ? b.err : "empty rest");
    box_close(&b);
    return 1;
  }
  box_close(&b);

  if (box_open(&b, 8388608) < 0) return fail("empty cont alloc");
  if (feed(&b, hdr, 1, 1000, 0) < 0 || call(&b, &o) < 0 || !o.cont || !o.interim) {
    fail(b.err ? b.err : "empty cont");
    box_close(&b);
    return 1;
  }
  p0 = o.bytes;
  n0 = o.len;
  if (take(&b, &o, 8) < 0 || call(&b, &o) < 0 || !o.cont || o.interim || o.bytes != p0 + 8 ||
      o.len != n0 - 8) {
    fail(b.err ? b.err : "empty cont short");
    box_close(&b);
    return 1;
  }
  /* The recv buffer was not empty. A fresh arrival must not rebuild
     the 100 from the first byte. */
  if (feed(&b, "abcd", 0, 1100, 0) < 0 || call(&b, &o) < 0) {
    fail(b.err ? b.err : "empty body");
    box_close(&b);
    return 1;
  }
  if (!o.cont || o.interim || o.bytes != p0 + 8 || o.len != n0 - 8) {
    fail("empty kept cont");
    box_close(&b);
    return 1;
  }
  if (take(&b, &o, o.len) < 0 || drive(&b, 0) < 0 || b.closed) {
    fail(b.err ? b.err : "empty echo");
    box_close(&b);
    return 1;
  }
  if (count_lit(b.got, b.got_n, "100 Continue") != 1 || !saw(&b, "abcd") || !saw(&b, "200")) {
    fail("empty echo bytes");
    box_close(&b);
    return 1;
  }
  box_close(&b);
  return 0;
}

static int check_status(uint64_t max_body, const char *req, const char *code, const char *body) {
  Box b;
  StepOut o;
  if (box_open(&b, max_body) < 0) return fail(b.err ? b.err : "status alloc");
  if (feed(&b, req, 1, 1000, 0) < 0 || call(&b, &o) < 0) {
    fail(b.err ? b.err : "status step");
    box_close(&b);
    return 1;
  }
  if (!o.close_conn || !o.status_now || !o.status || !o.status_len) {
    fail(code);
    box_close(&b);
    return 1;
  }
  if (o.status < b.st.spare || o.status + o.status_len > b.st.spare + sizeof b.st.spare) {
    fail("status spare");
    box_close(&b);
    return 1;
  }
  if (!memmem(o.status, o.status_len, code, strlen(code)) ||
      !memmem(o.status, o.status_len, body, strlen(body))) {
    fail(body);
    box_close(&b);
    return 1;
  }
  box_close(&b);
  return 0;
}

/* fuzz/crashes/trace-then-peer.bin: TRACE with a body is 501 and
   close_conn, then an empty PEER_CLOSED used to classify that buffer
   again and emit a second response. 413 and 417 share hand_status. */
static int quiet_after(Box *b, const Conn *snap, int phase, size_t in_len, int live, int done,
                       const char *where, const char *what) {
  StepOut o;
  char now[160];
  if (call(b, &o) < 0) {
    fail(b->err ? b->err : what);
    return -1;
  }
  if (o.bytes || o.len || o.status || o.status_len || o.status_now || !o.close_conn) {
    fail(what);
    return -1;
  }
  proto_where(b->st.proto, now, sizeof now);
  if (memcmp(&b->st.st, snap, sizeof *snap) != 0 || muro_http_step_phase(&b->st) != phase ||
      muro_http_step_in_len(&b->st) != in_len || proto_live(b->st.proto) != live ||
      proto_done(b->st.proto) != done || strcmp(now, where) != 0) {
    fail(what);
    return -1;
  }
  return 0;
}

static int check_closed_quiet(uint64_t max_body, const char *req, const char *code) {
  Box b;
  StepOut o;
  Conn snap;
  char where[160];
  int phase;
  int live;
  int done;
  size_t in_len;
  size_t n;
  const char *again = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
  if (box_open(&b, max_body) < 0) return fail(b.err ? b.err : "closed alloc");
  if (feed(&b, req, 1, 1000, 0) < 0 || call(&b, &o) < 0) {
    fail(b.err ? b.err : "closed step");
    box_close(&b);
    return 1;
  }
  if (!o.close_conn || !o.status || !o.status_len ||
      !memmem(o.status, o.status_len, code, strlen(code))) {
    fail(code);
    box_close(&b);
    return 1;
  }
  snap = b.st.st;
  phase = muro_http_step_phase(&b.st);
  in_len = muro_http_step_in_len(&b.st);
  live = proto_live(b.st.proto);
  done = proto_done(b.st.proto);
  proto_where(b.st.proto, where, sizeof where);

  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  b.in.empty_before = 1;
  if (quiet_after(&b, &snap, phase, in_len, live, done, where, "peer after close") < 0) {
    box_close(&b);
    return 1;
  }

  n = strlen(again);
  if (in_len + n > IN_CAP) {
    fail("closed feed");
    box_close(&b);
    return 1;
  }
  memcpy(b.inb + in_len, again, n);
  muro_http_step_set_in_len(&b.st, in_len + n);
  in_len += n;
  memset(&b.in, 0, sizeof b.in);
  b.in.kind = MURO_STEP_BYTES;
  b.in.empty_before = 0;
  b.in.now_ms = 1000;
  b.in.date = (const uint8_t *)DATE;
  b.in.date_n = 29;
  if (quiet_after(&b, &snap, phase, in_len, live, done, where, "bytes after close") < 0) {
    box_close(&b);
    return 1;
  }

  set_fact(&b, MURO_STEP_WROTE, 1);
  if (quiet_after(&b, &snap, phase, in_len, live, done, where, "wrote after close") < 0) {
    box_close(&b);
    return 1;
  }
  b.in.now_ms = 70000;
  b.in.deadline = 0;
  set_fact(&b, MURO_STEP_NOW, 0);
  if (quiet_after(&b, &snap, phase, in_len, live, done, where, "now after close") < 0) {
    box_close(&b);
    return 1;
  }
  set_fact(&b, MURO_STEP_WRITE_FAILED, 0);
  if (quiet_after(&b, &snap, phase, in_len, live, done, where, "fail after close") < 0) {
    box_close(&b);
    return 1;
  }
  box_close(&b);
  return 0;
}

static int check_pipe_and_short(void) {
  Box b;
  const char *two = "GET / HTTP/1.1\r\nHost: h\r\n\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n";
  const char *one = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
  if (box_open(&b, 8388608) < 0) return fail("pipe alloc");
  if (feed(&b, two, 1, 1000, 0) < 0 || drive(&b, 0) < 0 || b.closed) {
    fail(b.err ? b.err : "pipe");
    box_close(&b);
    return 1;
  }
  if (count_lit(b.got, b.got_n, "hello\n") != 2 || muro_http_step_in_len(&b.st) != 0) {
    fail("pipe bytes");
    box_close(&b);
    return 1;
  }
  box_close(&b);
  if (box_open(&b, 8388608) < 0) return fail("short alloc");
  if (feed(&b, one, 1, 1000, 0) < 0 || drive(&b, 1) < 0 || b.closed || !saw(&b, "hello\n") ||
      !saw(&b, "HTTP/1.1 200")) {
    fail(b.err ? b.err : "short");
    box_close(&b);
    return 1;
  }
  box_close(&b);
  return 0;
}

static int check_deadline_and_fail(void) {
  Box b;
  StepOut o;
  const char *req = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
  if (box_open(&b, 8388608) < 0) return fail("deadline alloc");
  if (feed(&b, req, 1, 1000, 424242) < 0 || call(&b, &o) < 0) {
    fail(b.err ? b.err : "deadline");
    box_close(&b);
    return 1;
  }
  if (!o.bytes || o.len < 2 || o.cont || o.interim || o.wake_at != 0 || o.close_conn) {
    fail("deadline armed");
    box_close(&b);
    return 1;
  }
  b.in.deadline = 424242;
  if (take(&b, &o, 1) < 0) {
    fail("deadline take");
    box_close(&b);
    return 1;
  }
  b.in.deadline = 424242;
  if (call(&b, &o) < 0 || o.wake_at != 0 || o.cont || !o.bytes) {
    fail(b.err ? b.err : "deadline kept");
    box_close(&b);
    return 1;
  }
  set_fact(&b, MURO_STEP_WRITE_FAILED, 0);
  b.in.deadline = 424242;
  if (call(&b, &o) < 0 || !o.close_conn) {
    fail(b.err ? b.err : "write failed");
    box_close(&b);
    return 1;
  }
  box_close(&b);
  return 0;
}

static int check_blob(void) {
  Box b;
  size_t xs = 0;
  size_t i;
  size_t body;
  const uint8_t *eoh;
  const char *req = "GET /blob HTTP/1.1\r\nHost: h\r\n\r\n";
  blob_left = BLOB_N;
  if (box_open(&b, 8388608) < 0) return fail("blob alloc");
  if (feed(&b, req, 1, 1000, 0) < 0 || drive(&b, 1) < 0 || b.closed) {
    fail(b.err ? b.err : "blob");
    box_close(&b);
    return 1;
  }
  eoh = memmem(b.got, b.got_n, "\r\n\r\n", 4);
  if (!eoh) {
    fail("blob headers");
    box_close(&b);
    return 1;
  }
  body = (size_t)((b.got + b.got_n) - (eoh + 4));
  if (!saw(&b, "Content-Length: 100001") || body != BLOB_N) {
    fail("blob bytes");
    box_close(&b);
    return 1;
  }
  for (i = 0; i < body; i++) {
    if (((const uint8_t *)eoh)[4 + i] != 'x') xs += 1;
  }
  if (xs != 0) {
    fail("blob bytes");
    box_close(&b);
    return 1;
  }
  box_close(&b);
  return 0;
}

/* Request line past the 16384 header cap is 414. A short request
   line with a header block past that cap is 431. Both close. */
static int check_caps(void) {
  Box b;
  char *req;
  const char *tail = " HTTP/1.1\r\nHost: h\r\n\r\n";
  const char *head = "GET / HTTP/1.1\r\nHost: h\r\nX-Pad: ";
  size_t tail_n = strlen(tail);
  size_t head_n = strlen(head);
  req = malloc(17000 + head_n + 64);
  if (!req) return fail("cap alloc");
  memcpy(req, "GET /", 5);
  memset(req + 5, 'a', 17000);
  memcpy(req + 5 + 17000, tail, tail_n + 1);
  if (box_open(&b, 8388608) < 0) {
    free(req);
    return fail(b.err ? b.err : "line cap box");
  }
  if (feed(&b, req, 1, 1000, 0) < 0 || drive(&b, 0) < 0 || !b.closed || !saw(&b, "HTTP/1.1 414") ||
      !saw(&b, "uri too long")) {
    fail(b.err ? b.err : "414");
    box_close(&b);
    free(req);
    return 1;
  }
  box_close(&b);
  memcpy(req, head, head_n);
  memset(req + head_n, 'b', 17000);
  memcpy(req + head_n + 17000, "\r\n\r\n", 5);
  if (box_open(&b, 8388608) < 0) {
    free(req);
    return fail(b.err ? b.err : "header cap box");
  }
  if (feed(&b, req, 1, 1000, 0) < 0 || drive(&b, 0) < 0 || !b.closed || !saw(&b, "HTTP/1.1 431") ||
      !saw(&b, "header fields")) {
    fail(b.err ? b.err : "431");
    box_close(&b);
    free(req);
    return 1;
  }
  box_close(&b);
  free(req);
  return 0;
}

int main(void) {
  if (check_basic()) return 1;
  if (check_cont()) return 1;
  if (check_now()) return 1;
  if (check_empty_before()) return 1;
  if (check_status(10, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 100\r\n\r\n", "413",
                   "too large"))
    return 1;
  if (check_status(8388608,
                   "POST / HTTP/1.1\r\nHost: h\r\nExpect: foo\r\nContent-Length: 4\r\n\r\n", "417",
                   "expectation"))
    return 1;
  if (check_status(8388608, "TRACE / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\n", "501",
                   "unimplemented"))
    return 1;
  if (check_closed_quiet(10, "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 100\r\n\r\n", "413"))
    return 1;
  if (check_closed_quiet(8388608,
                         "POST / HTTP/1.1\r\nHost: h\r\nExpect: foo\r\nContent-Length: 4\r\n\r\n",
                         "417"))
    return 1;
  if (check_closed_quiet(8388608, "TRACE / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\n", "501"))
    return 1;
  if (check_pipe_and_short()) return 1;
  if (check_deadline_and_fail()) return 1;
  if (check_blob()) return 1;
  if (check_caps()) return 1;
  printf("OK\n");
  return 0;
}
