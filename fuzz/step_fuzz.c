/* Clang libFuzzer harness for muro_http_step. One input is one
   connection: bind, ACCEPTED, then a script of facts. The bytes are
   not a socket and this file does not poll.

   Structured input (first byte 1..8, or 0xA5 then a config byte):
     0                 stop
     1 flags u16le n   payload     MURO_STEP_BYTES
     2 flags u16le n   payload     MURO_STEP_PEER_CLOSED
                       (n is trailing bytes; n 0 closes with what is
                       already buffered)
     3 mode            MURO_STEP_WROTE
     4                 MURO_STEP_WRITE_FAILED
     5 mode            MURO_STEP_NOW
     6 mode rounds     drain outstanding spans as WROTE
     7 flags index     BYTES of a built-in request
     8 sub u64le       set the harness clock or the deadline to pass
                       on later facts; does not call the step
   Any other leading byte is one raw request: BYTES with
   empty_before 1, then a full-span drain.

   flags bit0  empty_before, used when bit1 is clear
         bit1  honest empty_before: 1 iff the recv buffer was empty
               before this append
         bit2  pass the last armed wake_at as deadline
         bit3  pass deadline 0
   Otherwise the deadline last set (initially 0) is passed through.

   WROTE mode 0 full span, 1 one byte, 2 half, 3 zero, 4 span+1,
   5 span-1, 6 SIZE_MAX, 16+k means k bytes (k <= 239).

   NOW mode 0 jump so now >= last wake and pass that deadline,
   1 now += 1000000, 2 now -= 1000000 (saturate at 0),
   3 deadline 0, 4 now += 1, 5 now -= 1,
   6 now += 600000 and deadline 0, 7 deadline = now,
   8 now = 0 and deadline 0, 9 now near UINT64_MAX and deadline = now.

   Config (the byte after 0xA5), else the mem.c sizes:
     bits 0..2  max body  8, 10, 100, 1024, 16384, 65536, 1MB, 8MB
     bits 3..4  idle ms   0, 1, 1000, 60000
     bits 5..7  buffers   pairs of recv/send caps, including the
                65536/69632 pair the socket adapter uses

   The harness aborts on a span outside the send buffer, the
   100-continue area, or the spare/close area; on a length past
   those buffers; on a due MURO_STEP_NOW that makes no progress
   (the step's per-call spins fuse is 100000; this bound is 8);
   and on a fact after close that moves the image, the session,
   the recv length, or produces another span. Leak checks run on
   every 32nd input; ASan leak detection stays enabled for the rest. */

#include "step.h"
#include "host_stream.h"

#include <sanitizer/lsan_interface.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CALL_MAX = 4000, NOW_SPIN_MAX = 8, NOW_FOLLOW_MAX = 32, BLOB_N = 200 };

static const char DATE[] = "Sun, 06 Nov 1994 08:49:37 GMT";

_Static_assert(sizeof DATE - 1 == 29, "IMF-fixdate is 29 bytes");

/* A streamed body read calls these. No server is running, and a
   negative fd must not enter poll. */
int host_io_should_stop(void) { return 0; }

int host_io_once(int watch_fd) {
  (void)watch_fd;
  return -1;
}

void host_io_progress(int fd) { (void)fd; }

typedef struct FuzzUser {
  size_t blob_left;
} FuzzUser;

static int pull_blob(uint8_t *dst, size_t cap, void *user) {
  size_t *left = user;
  size_t n;
  if (!left || !dst) return -1;
  n = cap;
  if (n > *left) n = *left;
  if (n == 0) return 0;
  memset(dst, 'x', n);
  *left -= n;
  return (int)n;
}

static int pull_req(uint8_t *dst, size_t cap, void *user) {
  if (!user || !dst) return 0;
  return muro_http_body_read(user, dst, cap);
}

static int path_is(const MuroHttpRequest *req, const char *lit) {
  size_t n = strlen(lit);
  return req->path.n == n && req->path.p && memcmp(req->path.p, lit, n) == 0;
}

static int on_req(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  static const char type[] = "Content-Type: text/plain\r\n";
  FuzzUser *u = user;
  resp->headers = type;
  resp->headers_len = sizeof type - 1;
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
  if (req->method.n == 3 && req->method.p && memcmp(req->method.p, "GET", 3) == 0) {
    if (path_is(req, "/blob") && u) {
      u->blob_left = BLOB_N;
      resp->pull = pull_blob;
      resp->pull_user = &u->blob_left;
      resp->has_length = 1;
      resp->content_length = u->blob_left;
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

static const char *const DICT[] = {
    "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: h\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: localhost\r\n",
    "GET /health HTTP/1.1\r\nHost: t\r\n\r\n",
    "GET /health HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
    "GET /health HTTP/1.0\r\n\r\n",
    "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\nabcd",
    "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n",
    "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\nabcd",
    "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 100\r\n\r\n",
    "POST / HTTP/1.1\r\nHost: h\r\nExpect: foo\r\nContent-Length: 4\r\n\r\n",
    "TRACE / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\n",
    "GET /blob HTTP/1.1\r\nHost: h\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: h\r\n\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n",
    "GET /health HTTP/1.1\r\nHost: t\r\n\r\nGET /feed HTTP/1.1\r\nHost: t\r\n\r\n",
    "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n",
    "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n5;foo=1\r\nhello\r\n0\r\n\r\n",
    "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip\r\n\r\n",
    "HEAD / HTTP/1.1\r\nHost: h\r\n\r\n",
    "PUT /x HTTP/1.1\r\nHost: t\r\n\r\n",
    "GET /feed?x=1 HTTP/1.1\r\nHost: t\r\n\r\n",
    "GET /health HTTP/1.1\nHost: t\n\n",
    "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: +1\r\n\r\n",
    "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",
    "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n",
    "GET /health HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
    "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n",
    "GET / HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n\r\n",
    "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n0\r\n\r\n",
};

enum { NDICT = sizeof DICT / sizeof DICT[0] };

static size_t g_off;
static int g_trace;
static unsigned g_inputs;

static void die(const char *msg) {
  fprintf(stderr, "step_fuzz: %s at input offset %zu\n", msg, g_off);
  fflush(stderr);
  abort();
}

static uint64_t conn_field(Conn st, int i) {
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

static int image_eq(Conn a, Conn b) {
  int i;
  if (a.muro__field_tag != b.muro__field_tag) return 0;
  for (i = 0; i < 13; i++) {
    if (conn_field(a, i) != conn_field(b, i)) return 0;
  }
  return 1;
}

/* Half-open [base, base+cap). n == 0 allows a pointer at the end. */
static int in_region(const uint8_t *p, size_t n, const uint8_t *base, size_t cap) {
  uintptr_t off;
  if (!p || !base) return 0;
  if ((uintptr_t)p < (uintptr_t)base) return 0;
  off = (uintptr_t)p - (uintptr_t)base;
  if (n == 0) return off <= cap;
  if (off > cap || n > cap - off) return 0;
  return 1;
}

typedef struct Freeze {
  int on;
  Conn image;
  int live;
  int done;
  char where[96];
} Freeze;

typedef struct Prog {
  uint64_t f[13];
  uint8_t tag;
  size_t in_len;
  size_t out_off;
  size_t out_len;
  size_t cont_off;
  size_t cont_len;
  int pend;
  int sent_100;
  int phase;
  uint64_t wake;
  size_t span;
  int close_conn;
} Prog;

typedef struct Box {
  uint8_t *inb;
  uint8_t *outb;
  size_t in_cap;
  size_t out_cap;
  HttpStep st;
  struct HostIn hin;
  int rd;
  int wr;
  StepIn fact;
  StepOut last;
  size_t span;
  uint64_t now;
  uint64_t pass_dl;
  uint64_t armed;
  Freeze freeze;
  int calls;
  int stop;
  FuzzUser user;
} Box;

static int span_ok(const Box *b, const StepOut *o) {
  if (o->cont) {
    if (!in_region(o->bytes, o->len, b->st.cont, sizeof b->st.cont)) return 0;
  } else if (o->bytes && o->len) {
    if (!in_region(o->bytes, o->len, b->outb, b->out_cap)) return 0;
  }
  if (o->status && o->status_len) {
    int in_spare = in_region(o->status, o->status_len, b->st.spare, sizeof b->st.spare);
    int in_out = in_region(o->status, o->status_len, b->outb, b->out_cap);
    if (!in_spare && !in_out) return 0;
  }
  return 1;
}

static void check_lengths(const Box *b) {
  if (b->st.in_len > b->st.in_cap) die("recv length past cap");
  if (b->st.out_len > b->st.out_cap) die("send length past cap");
  if (b->st.cont_len > sizeof b->st.cont) die("continue length past area");
  if (b->st.spare_len > sizeof b->st.spare) die("spare length past area");
  if (b->st.cont_off > b->st.cont_len) die("continue offset past length");
  if (b->st.pend_kind == 2 && b->st.out_off > b->st.out_len) die("send offset past length");
}

static void snap_freeze(Box *b) {
  b->freeze.on = 1;
  b->freeze.image = b->st.st;
  b->freeze.live = proto_live(b->st.proto);
  b->freeze.done = proto_done(b->st.proto);
  proto_where(b->st.proto, b->freeze.where, sizeof b->freeze.where);
}

static void check_frozen(Box *b, size_t in_before, int rc, const StepOut *o) {
  char where[96];
  (void)rc;
  if ((o->bytes && o->len) || (o->status && o->status_len)) die("span after close");
  if (!image_eq(b->st.st, b->freeze.image)) die("image moved after close");
  if (muro_http_step_in_len(&b->st) != in_before) die("recv advanced after close");
  if (proto_live(b->st.proto) != b->freeze.live || proto_done(b->st.proto) != b->freeze.done)
    die("session moved after close");
  proto_where(b->st.proto, where, sizeof where);
  if (strcmp(where, b->freeze.where) != 0) die("session moved after close");
}

static Prog take_prog(const Box *b) {
  Prog p;
  int i;
  memset(&p, 0, sizeof p);
  p.tag = b->st.st.muro__field_tag;
  for (i = 0; i < 13; i++) p.f[i] = conn_field(b->st.st, i);
  p.in_len = b->st.in_len;
  p.out_off = b->st.out_off;
  p.out_len = b->st.out_len;
  p.cont_off = b->st.cont_off;
  p.cont_len = b->st.cont_len;
  p.pend = b->st.pend_kind;
  p.sent_100 = b->st.sent_100;
  p.phase = muro_http_step_phase(&b->st);
  p.wake = b->last.wake_at;
  p.span = b->span;
  p.close_conn = b->last.close_conn;
  return p;
}

static void apply(Box *b, int follow);

static void follow_now(Box *b) {
  int repeats = 0;
  int steps = 0;
  int have = 0;
  Prog prev;
  memset(&prev, 0, sizeof prev);
  while (steps < NOW_FOLLOW_MAX && !b->stop && !b->freeze.on) {
    Prog cur;
    if (b->last.close_conn || (b->last.bytes && b->last.len)) break;
    if (b->last.wake_at == 0 || b->last.wake_at > b->now) break;
    cur = take_prog(b);
    if (have && memcmp(&prev, &cur, sizeof cur) == 0) {
      repeats += 1;
      if (repeats >= NOW_SPIN_MAX) die("now livelock");
    } else {
      repeats = 0;
    }
    prev = cur;
    have = 1;
    b->pass_dl = b->last.wake_at;
    b->fact.kind = MURO_STEP_NOW;
    b->fact.n = 0;
    b->fact.empty_before = 0;
    apply(b, 0);
    steps += 1;
  }
}

static void apply(Box *b, int follow) {
  StepOut o;
  int rc;
  size_t in_before;
  int was;
  if (b->stop) return;
  if (b->calls >= CALL_MAX) {
    b->stop = 1;
    return;
  }
  b->calls += 1;
  b->fact.date = (const uint8_t *)DATE;
  b->fact.date_n = 29;
  b->fact.now_ms = b->now;
  b->fact.deadline = b->pass_dl;
  in_before = muro_http_step_in_len(&b->st);
  was = b->freeze.on;
  rc = muro_http_step(&b->st, &b->fact, &o);
  if (g_trace) {
    fprintf(stderr,
            "step kind=%d rc=%d phase=%d close=%d cont=%d len=%zu status=%zu wake=%llu now=%llu dl=%llu\n",
            b->fact.kind, rc, muro_http_step_phase(&b->st), o.close_conn, o.cont, o.len, o.status_len,
            (unsigned long long)o.wake_at, (unsigned long long)b->now, (unsigned long long)b->pass_dl);
  }
  check_lengths(b);
  if (!span_ok(b, &o)) die("span outside buffer");
  if (o.note == MURO_STEP_NOTE_FUEL || o.note2 == MURO_STEP_NOTE_FUEL) die("pump fuel");
  if (was) check_frozen(b, in_before, rc, &o);
  else if (rc < 0 || o.close_conn || muro_http_step_phase(&b->st) == MURO_STEP_PH_CLOSED)
    snap_freeze(b);
  b->last = o;
  b->armed = o.wake_at;
  if (o.bytes && o.len) b->span = o.len;
  else b->span = 0;
  if (follow) follow_now(b);
}

static void append(Box *b, const uint8_t *p, size_t n) {
  size_t cur = muro_http_step_in_len(&b->st);
  size_t room;
  if (cur > b->in_cap) die("recv length past cap");
  room = b->in_cap - cur;
  if (n > room) n = room;
  if (n && p) memcpy(b->inb + cur, p, n);
  muro_http_step_set_in_len(&b->st, cur + n);
}

static int empty_of(unsigned flags, size_t before) {
  if (flags & 2) return before == 0;
  return (flags & 1) ? 1 : 0;
}

static void note_deadline(Box *b, unsigned flags) {
  if (flags & 8) b->pass_dl = 0;
  else if (flags & 4) b->pass_dl = b->armed;
}

static void feed(Box *b, int kind, unsigned flags, const uint8_t *p, size_t n) {
  size_t before = muro_http_step_in_len(&b->st);
  note_deadline(b, flags);
  append(b, p, n);
  b->fact.kind = kind;
  b->fact.n = 0;
  b->fact.empty_before = empty_of(flags, before);
  apply(b, 1);
}

static size_t wrote_n(const Box *b, unsigned mode) {
  size_t span = b->span;
  if (mode >= 16) return (size_t)(mode - 16);
  switch (mode) {
  case 0: return span;
  case 1: return span ? 1 : 0;
  case 2: return span / 2;
  case 3: return 0;
  case 4: return span + 1;
  case 5: return span ? span - 1 : 0;
  case 6: return SIZE_MAX;
  default: return span ? 1 : 0;
  }
}

static void do_wrote(Box *b, unsigned mode) {
  b->fact.kind = MURO_STEP_WROTE;
  b->fact.n = wrote_n(b, mode);
  b->fact.empty_before = 0;
  apply(b, 1);
}

static void drain(Box *b, unsigned mode, unsigned rounds) {
  unsigned i;
  if (rounds == 0) rounds = 1;
  if (rounds > 64) rounds = 64;
  if (mode == 1 && rounds > 48) rounds = 48;
  for (i = 0; i < rounds && !b->stop && !b->freeze.on; i++) {
    if (b->span == 0 && mode != 3 && mode != 4 && mode != 6 && mode < 16) break;
    do_wrote(b, mode);
  }
}

static void adjust_now(Box *b, unsigned mode) {
  switch (mode) {
  case 0:
    if (b->armed > b->now) b->now = b->armed;
    b->pass_dl = b->armed;
    break;
  case 1:
    if (b->now <= UINT64_MAX - 1000000ull) b->now += 1000000ull;
    else b->now = UINT64_MAX;
    break;
  case 2:
    if (b->now >= 1000000ull) b->now -= 1000000ull;
    else b->now = 0;
    break;
  case 3:
    b->pass_dl = 0;
    break;
  case 4:
    if (b->now < UINT64_MAX) b->now += 1;
    break;
  case 5:
    if (b->now > 0) b->now -= 1;
    break;
  case 6:
    b->pass_dl = 0;
    if (b->now <= UINT64_MAX - 600000ull) b->now += 600000ull;
    else b->now = UINT64_MAX;
    break;
  case 7:
    b->pass_dl = b->now;
    break;
  case 8:
    b->now = 0;
    b->pass_dl = 0;
    break;
  case 9:
    b->now = UINT64_MAX - 50ull;
    b->pass_dl = b->now;
    break;
  default:
    b->pass_dl = 0;
    break;
  }
}

typedef struct Cur {
  const uint8_t *p;
  size_t n;
  size_t i;
} Cur;

static int take_u8(Cur *c, unsigned *o) {
  if (c->i >= c->n) return 0;
  *o = c->p[c->i++];
  return 1;
}

static int take_u16(Cur *c, unsigned *o) {
  unsigned a, bch;
  if (!take_u8(c, &a) || !take_u8(c, &bch)) return 0;
  *o = a | (bch << 8);
  return 1;
}

static int take_u64(Cur *c, uint64_t *o) {
  uint64_t v = 0;
  int k;
  for (k = 0; k < 8; k++) {
    unsigned bch;
    if (!take_u8(c, &bch)) return 0;
    v |= (uint64_t)bch << (8 * k);
  }
  *o = v;
  return 1;
}

static int take_span(Cur *c, const uint8_t **p, unsigned n) {
  if ((size_t)n > c->n - c->i) return 0;
  *p = c->p + c->i;
  c->i += n;
  return 1;
}

static void run_structured(Box *b, const uint8_t *data, size_t size) {
  Cur c;
  c.p = data;
  c.n = size;
  c.i = 0;
  while (!b->stop && c.i < c.n) {
    unsigned op, flags, n, mode, rounds, sub, idx;
    const uint8_t *p;
    uint64_t v;
    g_off = c.i;
    if (!take_u8(&c, &op)) return;
    switch (op) {
    case 0:
      return;
    case 1:
    case 2:
      if (!take_u8(&c, &flags) || !take_u16(&c, &n) || !take_span(&c, &p, n)) return;
      feed(b, op == 1 ? MURO_STEP_BYTES : MURO_STEP_PEER_CLOSED, flags, p, n);
      break;
    case 3:
      if (!take_u8(&c, &mode)) return;
      do_wrote(b, mode);
      break;
    case 4:
      b->fact.kind = MURO_STEP_WRITE_FAILED;
      b->fact.n = 0;
      b->fact.empty_before = 0;
      apply(b, 1);
      break;
    case 5:
      if (!take_u8(&c, &mode)) return;
      adjust_now(b, mode);
      b->fact.kind = MURO_STEP_NOW;
      b->fact.n = 0;
      b->fact.empty_before = 0;
      apply(b, 1);
      break;
    case 6:
      if (!take_u8(&c, &mode) || !take_u8(&c, &rounds)) return;
      drain(b, mode, rounds);
      break;
    case 7:
      if (!take_u8(&c, &flags) || !take_u8(&c, &idx)) return;
      p = (const uint8_t *)DICT[idx % NDICT];
      feed(b, MURO_STEP_BYTES, flags, p, strlen((const char *)p));
      break;
    case 8:
      if (!take_u8(&c, &sub) || !take_u64(&c, &v)) return;
      if (sub == 0) b->pass_dl = v;
      else if (sub == 1) b->now = v;
      else if (sub == 2) {
        b->now = v;
        b->pass_dl = v;
      } else if (sub == 3) b->armed = v;
      break;
    default:
      return;
    }
  }
}

static void caps_of(unsigned cfg, size_t *in_cap, size_t *out_cap, uint64_t *max_body, int *idle) {
  static const uint64_t bodies[] = {8, 10, 100, 1024, 16384, 65536, 1048576ull, 8388608ull};
  static const int idles[] = {0, 1, 1000, 60000};
  static const size_t ins[] = {256, 1024, 4096, 16384, 65536, 65536, 128, 65536};
  static const size_t outs[] = {128, 512, 4096, 8192, 69632, 256, 69632, 64};
  unsigned capi = (cfg >> 5) & 7;
  *max_body = bodies[cfg & 7];
  *idle = idles[(cfg >> 3) & 3];
  *in_cap = ins[capi];
  *out_cap = outs[capi];
}

static void box_shutdown(Box *b) {
  muro_http_step_shutdown(&b->st);
  host_in_close(&b->hin);
  free(b->inb);
  free(b->outb);
  b->inb = NULL;
  b->outb = NULL;
  muro_http_reset();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  Box b;
  size_t in_cap = 65536;
  size_t out_cap = 69632;
  uint64_t max_body = 8388608ull;
  int idle = 60000;
  int structured = 0;
  size_t begin = 0;
  static int ready = 0;

  if (!ready) {
    g_trace = getenv("STEP_FUZZ_TRACE") != NULL;
    ready = 1;
  }
  if (size && data[0] == 0xA5) {
    structured = 1;
    if (size >= 2) caps_of(data[1], &in_cap, &out_cap, &max_body, &idle);
    begin = size >= 2 ? 2 : 1;
  } else if (size && data[0] >= 1 && data[0] <= 8) {
    structured = 1;
  }

  memset(&b, 0, sizeof b);
  b.inb = malloc(in_cap);
  b.outb = malloc(out_cap);
  b.in_cap = in_cap;
  b.out_cap = out_cap;
  if (!b.inb || !b.outb) {
    free(b.inb);
    free(b.outb);
    return 0;
  }
  b.user.blob_left = BLOB_N;
  b.now = 1000;
  host_in_init(&b.hin, -1, max_body);
  muro_http_step_bind(&b.st, b.inb, in_cap, b.outb, out_cap, &b.hin, &b.rd, &b.wr, on_req, &b.user,
                      max_body, idle);
  b.fact.kind = MURO_STEP_ACCEPTED;
  b.fact.now_ms = b.now;
  apply(&b, 1);

  if (!b.stop && structured) run_structured(&b, data + begin, size - begin);
  else if (!b.stop && size) {
    feed(&b, MURO_STEP_BYTES, 2, data, size);
    if (!b.stop && !b.freeze.on) drain(&b, 0, 64);
  }

  box_shutdown(&b);
  /* A full leak scan is most of the per-input time. Sampling keeps
     detection on without turning the run into a leak-checker. */
  g_inputs += 1;
  if ((g_inputs & 31u) == 0 && __lsan_do_recoverable_leak_check()) die("leak across input");
  return 0;
}
