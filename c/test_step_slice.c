/* Same StepIn for c/step.c and the emitted slice.
   The host stays muro_http_step. http_step_stepFact is the book
   in c/generated/step_slice.c. This file compares the fields the
   slice decides.

   A finished request is a handler span in the book (phase 3) and a
   rendered response in the host. Those bytes must lie in the caller
   send buffer. The status text is not compared field for field.

   Clocks are the adapter's. idle_ms is the value passed to bind.
   Accept arms now+idle and ignores a carried deadline. A read or an
   idle park keeps a carried deadline, and arms now+idle when it is 0.
   A write parks with wake 0. NOW from read or idle closes. The book
   does not compare the two clocks.

   cover 1 is outside the slice. The driver still runs the host and
   requires it to return 0. It does not compare fields on that path.
   ACCEPTED is not part of the sweep: the host resets, and the book
   does not.

   A wrote with left 0 and owed 0 finishes in the book. That input
   is not a host fact. The owed-0 check is book-only. */

#include "step.h"
#include "host_stream.h"
#include "step_slice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IN_CAP = 65536, OUT_CAP = 69632 };
enum { DEC_MORE = 1, DEC_REPLY, DEC_STATUS, DEC_OWE, DEC_QUIET, DEC_OUT, DEC_STILL, DEC_IDLE };

static const char DATE[] = "Sun, 06 Nov 1994 08:49:37 GMT";

static const char *LINE = "GET / HT";
static const char *HDR = "GET / HTTP/1.1\r\nHost: h\r\n";
static const char *GET = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
static const char *POST_SHORT = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\n";
static const char *POST_BODY = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\nabcd";
static const char *TOO_BIG = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 100\r\n\r\n";
static const char *EXPECT = "POST / HTTP/1.1\r\nHost: h\r\nExpect: foo\r\nContent-Length: 4\r\n\r\n";
static const char *EXPECT_DONE =
    "POST / HTTP/1.1\r\nHost: h\r\nExpect: foo\r\nContent-Length: 0\r\n\r\n";
static const char *TRACE = "TRACE / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\n";
static const char *TRACE_DONE = "TRACE / HTTP/1.1\r\nHost: h\r\n\r\n";
static const char *GZIP = "GET / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n\r\n";
static const char *BAD = "GET/feed HTTP/1.1\r\n\r\n";
static const char *NO_HOST = "GET / HTTP/1.1\r\n\r\n";
static const char *CHUNK = "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
static const char *CONT =
    "POST / HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n";
static const char *HEAD = "HEAD / HTTP/1.1\r\nHost: h\r\n\r\n";
static const char *CLOSE = "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";
static const char *HTTP10 = "GET / HTTP/1.0\r\n\r\n";
static const char *TWO = "GET / HTTP/1.1\r\nHost: h\r\n\r\nGET / HTTP/1.1\r\nHost: h\r\n\r\n";
static const char *PIPE = "GET / HTTP/1.1\r\nHost: h\r\n\r\nGET / HT";
static const char *STREAM = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 16385\r\n\r\n";

static int after_spans;
static int scripted;
static int sweep_seq;
static int sweep_steps;
static int sweep_covered;
static int count_shape = 1;

static int on_req(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  (void)req;
  (void)user;
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
  resp->body = (const uint8_t *)"ok";
  resp->body_len = 2;
  return 0;
}

typedef struct Box {
  uint8_t *inb;
  uint8_t *outb;
  HttpStep st;
  struct HostIn hin;
  int rd;
  int wr;
  StepIn in;
} Box;

static int fail(const char *what) {
  fprintf(stderr, "step slice: %s\n", what);
  return -1;
}

static uint64_t bo_phase(http_step_Out o) { return o.muro__as.out.muro__f_0; }
static uint64_t bo_code(http_step_Out o) { return o.muro__as.out.muro__f_1; }
static uint64_t bo_close(http_step_Out o) { return o.muro__as.out.muro__f_2; }
static uint64_t bo_status(http_step_Out o) { return o.muro__as.out.muro__f_3; }
static uint64_t bo_read(http_step_Out o) { return o.muro__as.out.muro__f_4; }
static uint64_t bo_write(http_step_Out o) { return o.muro__as.out.muro__f_5; }
static uint64_t bo_span(http_step_Out o) { return o.muro__as.out.muro__f_6; }
static uint64_t bo_cover(http_step_Out o) { return o.muro__as.out.muro__f_7; }
static uint64_t bo_left(http_step_Out o) { return o.muro__as.out.muro__f_8; }
static uint64_t bo_wake(http_step_Out o) { return o.muro__as.out.muro__f_9; }
static uint64_t bo_keep(http_step_Out o) { return o.muro__as.out.muro__f_10; }

static muro__slice sl_of(const char *s) {
  muro__slice sl;
  sl.p = (const uint8_t *)(s ? s : "");
  sl.n = s ? strlen(s) : 0;
  return sl;
}

static muro__slice sl_mem(const uint8_t *p, size_t n) {
  muro__slice sl;
  sl.p = p ? p : (const uint8_t *)"";
  sl.n = p ? n : 0;
  return sl;
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

static void set_clock(Box *b, uint64_t now, uint64_t dl) {
  b->in.now_ms = now;
  b->in.deadline = dl;
}

static uint64_t armed(uint64_t now, uint64_t dl, uint64_t idle) {
  return dl == 0 ? now + idle : dl;
}

static int has_lit(const uint8_t *p, size_t n, const char *lit) {
  size_t ln;
  if (!lit) return 1;
  ln = strlen(lit);
  if (!p || n < ln) return 0;
  return memmem(p, n, lit, ln) != NULL;
}

static int has_code(const uint8_t *p, size_t n, uint64_t code) {
  char buf[32];
  if (code == 0) return 1;
  snprintf(buf, sizeof buf, "%llu", (unsigned long long)code);
  return has_lit(p, n, buf);
}

static int in_range(const uint8_t *p, size_t n, const uint8_t *base, size_t cap) {
  if (n == 0) return 1;
  if (!p || !base) return 0;
  if (p < base) return 0;
  if (n > cap) return 0;
  return (size_t)(p - base) <= cap - n;
}

static void box_free(Box *b) {
  if (!b->inb && !b->outb) return;
  muro_http_step_shutdown(&b->st);
  host_in_close(&b->hin);
  free(b->inb);
  free(b->outb);
  memset(b, 0, sizeof *b);
}

static int box_bind(Box *b, uint64_t max_body) {
  memset(b, 0, sizeof *b);
  b->inb = calloc(1, IN_CAP);
  b->outb = calloc(1, OUT_CAP);
  if (!b->inb || !b->outb) {
    free(b->inb);
    free(b->outb);
    memset(b, 0, sizeof *b);
    return fail("alloc");
  }
  host_in_init(&b->hin, -1, max_body);
  muro_http_step_bind(&b->st, b->inb, IN_CAP, b->outb, OUT_CAP, &b->hin, &b->rd, &b->wr, on_req,
                      NULL, max_body, 60000);
  set_clock(b, 1000, 0);
  return 0;
}

static void dump_book(const char *name, http_step_Out o) {
  fprintf(stderr,
          "%s: book phase=%llu code=%llu close=%llu status=%llu read=%llu write=%llu span=%llu "
          "cover=%llu left=%llu wake=%llu keep=%llu\n",
          name, (unsigned long long)bo_phase(o), (unsigned long long)bo_code(o),
          (unsigned long long)bo_close(o), (unsigned long long)bo_status(o),
          (unsigned long long)bo_read(o), (unsigned long long)bo_write(o),
          (unsigned long long)bo_span(o), (unsigned long long)bo_cover(o),
          (unsigned long long)bo_left(o), (unsigned long long)bo_wake(o),
          (unsigned long long)bo_keep(o));
}

static int book_is(http_step_Out o, uint64_t phase, uint64_t code, uint64_t close, uint64_t status,
                   uint64_t rd, uint64_t wr, uint64_t span, uint64_t cover, uint64_t left,
                   uint64_t wake, uint64_t keep, const char *name) {
  if (bo_phase(o) == phase && bo_code(o) == code && bo_close(o) == close && bo_status(o) == status &&
      bo_read(o) == rd && bo_write(o) == wr && bo_span(o) == span && bo_cover(o) == cover &&
      bo_left(o) == left && bo_wake(o) == wake && bo_keep(o) == keep)
    return 0;
  dump_book(name, o);
  return -1;
}

static int host_flags(const StepOut *o, int close, int status, int rd, int wr, const char *name) {
  if (o->close_conn == close && o->status_now == status && o->read_more == rd && o->write_more == wr &&
      !o->interim && !o->cont)
    return 0;
  fprintf(stderr, "%s: host close=%d status_now=%d read=%d write=%d interim=%d cont=%d wake=%llu\n",
          name, o->close_conn, o->status_now, o->read_more, o->write_more, o->interim, o->cont,
          (unsigned long long)o->wake_at);
  return -1;
}

static int wake_is(const StepOut *o, uint64_t wake, const char *name) {
  if (o->wake_at == wake) return 0;
  fprintf(stderr, "%s: host wake %llu want %llu\n", name, (unsigned long long)o->wake_at,
          (unsigned long long)wake);
  return -1;
}

static int text_ok(const StepOut *o, uint64_t code, const char *word, const char *name) {
  const uint8_t *p = NULL;
  size_t n = 0;
  if (o->status_now && o->status && o->status_len) {
    p = o->status;
    n = o->status_len;
  } else if (o->bytes && o->len) {
    p = o->bytes;
    n = o->len;
  }
  if (!has_code(p, n, code) || !has_lit(p, n, word)) {
    fprintf(stderr, "%s: host text missing %llu %s\n", name, (unsigned long long)code,
            word ? word : "");
    return -1;
  }
  return 0;
}

static int check_shape(int kind, const StepOut *o, http_step_Out bk, const Box *b, uint64_t code,
                       const char *word, uint64_t left, uint64_t wake, uint64_t keep,
                       const char *name) {
  if (kind == DEC_OUT) {
    if (book_is(bk, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, name) < 0) return -1;
    if (count_shape) scripted += 1;
    return 0;
  }
  if (wake_is(o, wake, name) < 0) return -1;
  if (kind == DEC_MORE) {
    if (book_is(bk, 0, 0, 0, 0, 1, 0, 0, 0, 0, wake, 0, name) < 0) return -1;
    if (host_flags(o, 0, 0, 1, 0, name) < 0) return -1;
    if ((o->bytes && o->len) || (o->status && o->status_len)) return fail(name);
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_QUIET) {
    if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, 0, 0, 0, name) < 0) return -1;
    if (host_flags(o, 1, 0, 0, 0, name) < 0) return -1;
    if ((o->bytes && o->len) || (o->status && o->status_len) || o->status_now) return fail(name);
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_REPLY) {
    if (book_is(bk, 3, 0, 0, 0, 0, 1, 2, 0, left, 0, keep, name) < 0) return -1;
    if (host_flags(o, 0, 0, 0, 1, name) < 0) return -1;
    if (!o->bytes || !o->len) return fail(name);
    if (!in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
    if (!has_lit(o->bytes, o->len, "200")) return fail(name);
    if (left != 0 && o->len != left) return fail(name);
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_IDLE) {
    if (book_is(bk, 4, 0, 0, 0, 1, 0, 0, 0, 0, wake, 0, name) < 0) return -1;
    if (host_flags(o, 0, 0, 1, 0, name) < 0) return -1;
    if ((o->bytes && o->len) || (o->status && o->status_len)) return fail(name);
    if (muro_http_step_phase(&b->st) != MURO_STEP_PH_IDLE) return fail(name);
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_STATUS) {
    if (book_is(bk, 2, code, 1, 1, 0, 0, 1, 0, 0, 0, 0, name) < 0) return -1;
    if (host_flags(o, 1, 1, 0, 0, name) < 0) return -1;
    if (o->bytes && o->len) return fail(name);
    if (!o->status || !o->status_len) return fail(name);
    if (!in_range(o->status, o->status_len, b->st.spare, sizeof b->st.spare)) return fail(name);
    if (text_ok(o, code, word, name) < 0) return -1;
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_OWE) {
    if (book_is(bk, 1, code, 0, 0, 0, 1, 2, 0, 0, 0, 1, name) < 0) return -1;
    if (host_flags(o, 0, 0, 0, 1, name) < 0) return -1;
    if (!o->bytes || !o->len) return fail(name);
    if (!in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
    if (text_ok(o, code, word, name) < 0) return -1;
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_STILL) {
    if (book_is(bk, bo_phase(bk), code, 0, 0, 0, 1, 2, 0, left, 0, keep, name) < 0) return -1;
    if (bo_phase(bk) != 1 && bo_phase(bk) != 3) return fail(name);
    if (host_flags(o, 0, 0, 0, 1, name) < 0) return -1;
    if (!o->bytes || o->len != left) return fail(name);
    if (!in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
    if (count_shape) scripted += 1;
    return 0;
  }
  return fail(name);
}

static http_step_Out book_at(http_step_St st, int fact, muro__slice sl, uint64_t max, uint64_t n,
                             uint64_t owed, uint64_t now, uint64_t dl, int idle_ms) {
  return http_step_stepFact(st, (uint64_t)fact, sl, max, n, owed, now, dl, (uint64_t)idle_ms);
}

/* fact ACCEPTED is the first call. Any other fact is preceded by
   ACCEPTED on the host only. The book starts from readSt. */
static int decide(int kind, int fact, const char *buf, uint64_t max_body, uint64_t code,
                  const char *word, uint64_t left, uint64_t wake, uint64_t keep, const char *name,
                  Box *keepb, http_step_Out *kept, StepOut *seen) {
  Box b;
  StepOut o;
  http_step_Out bk;
  if (box_bind(&b, max_body) < 0) return -1;
  memset(&o, 0, sizeof o);
  if (fact != MURO_STEP_ACCEPTED) {
    set_fact(&b, MURO_STEP_ACCEPTED, 0);
    if (muro_http_step(&b.st, &b.in, &o) < 0 || o.close_conn || !b.rd) {
      box_free(&b);
      return fail(name);
    }
  }
  if (fact == MURO_STEP_BYTES || fact == MURO_STEP_PEER_CLOSED) {
    size_t n = buf ? strlen(buf) : 0;
    if (n > IN_CAP) {
      box_free(&b);
      return fail(name);
    }
    if (n) memcpy(b.inb, buf, n);
    muro_http_step_set_in_len(&b.st, n);
    set_fact(&b, fact, 0);
    b.in.empty_before = 1;
  } else {
    set_fact(&b, fact, 0);
  }
  memset(&o, 0, sizeof o);
  bk = book_at(http_step_readSt(), fact, sl_of(buf), max_body, 0, 0, b.in.now_ms, b.in.deadline,
               b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    fprintf(stderr, "%s: host step failed\n", name);
    return -1;
  }
  if (check_shape(kind, &o, bk, &b, code, word, left, wake, keep, name) < 0) {
    box_free(&b);
    return -1;
  }
  if (seen) *seen = o;
  if (keepb) {
    *keepb = b;
    keepb->st.hin = &keepb->hin;
    keepb->st.read_more = &keepb->rd;
    keepb->st.write_more = &keepb->wr;
    keepb->hin.pub.in = &keepb->hin;
    *kept = bk;
    return 0;
  }
  box_free(&b);
  return 0;
}

static int later_fact(const char *req, uint64_t max_body, uint64_t code, int fact, const char *next,
                      const char *name) {
  Box b;
  StepOut o;
  http_step_Out first;
  http_step_Out bk;
  http_step_St closed;
  int rc;
  int span;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, req, max_body, code, NULL, 0, 0, 0, name, &b, &first,
             NULL) < 0)
    return -1;
  closed = http_step_take(first);
  if (fact == MURO_STEP_BYTES && next) {
    size_t n = strlen(next);
    memcpy(b.inb, next, n);
    muro_http_step_set_in_len(&b.st, n);
  }
  set_fact(&b, fact, 0);
  b.in.empty_before = 0;
  memset(&o, 0, sizeof o);
  bk = book_at(closed, fact, sl_of(fact == MURO_STEP_BYTES ? next : req), max_body, 0, 0,
               b.in.now_ms, b.in.deadline, b.st.idle_ms);
  rc = muro_http_step(&b.st, &b.in, &o);
  if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, 0, 0, 0, name) < 0) {
    box_free(&b);
    return -1;
  }
  if (rc < 0) {
    fprintf(stderr, "%s: handwritten-after-close step failed\n", name);
    box_free(&b);
    return -1;
  }
  span = ((o.bytes && o.len) || (o.status_now && o.status && o.status_len)) ? 1 : 0;
  if (span) after_spans += 1;
  printf("handwritten-after-close %s span=%d close=%d read=%d\n", name, span, o.close_conn,
         o.read_more);
  if (span || !o.close_conn || o.read_more || o.write_more || o.wake_at != 0) {
    fprintf(stderr, "%s: handwritten after close must stay quiet\n", name);
    box_free(&b);
    return -1;
  }
  scripted += 1;
  box_free(&b);
  return 0;
}

static int after_status(const char *req, uint64_t max_body, uint64_t code, const char *tag) {
  char name[64];
  const int facts[6] = {MURO_STEP_PEER_CLOSED, MURO_STEP_BYTES,     MURO_STEP_NOW,
                        MURO_STEP_WROTE,       MURO_STEP_WRITE_FAILED, MURO_STEP_ACCEPTED};
  const char *fn[6] = {"peer", "bytes", "now", "wrote", "failed", "accept"};
  int i;
  for (i = 0; i < 6; i++) {
    snprintf(name, sizeof name, "%s-%s", tag, fn[i]);
    if (later_fact(req, max_body, code, facts[i], GET, name) < 0) return -1;
  }
  return 0;
}

static int owe_then(const char *req, uint64_t max_body, uint64_t code, const char *word,
                    const char *name) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out first;
  http_step_Out bk;
  http_step_St owing;
  int rc;
  char tag[80];

  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, 0, 0, 1, name, &b, &first,
             &seen) < 0)
    return -1;
  owing = http_step_take(first);
  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  memset(&o, 0, sizeof o);
  bk = book_at(owing, MURO_STEP_PEER_CLOSED, sl_of(req), max_body, 0, 0, b.in.now_ms, b.in.deadline,
               b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    fprintf(stderr, "%s-peer: host step failed\n", name);
    return -1;
  }
  snprintf(tag, sizeof tag, "%s-peer", name);
  if (check_shape(DEC_OWE, &o, bk, &b, code, word, 0, 0, 1, tag) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, 0, 0, 1, name, &b, &first,
             &seen) < 0)
    return -1;
  owing = http_step_take(first);
  if (!seen.bytes || !seen.len) {
    box_free(&b);
    return fail(name);
  }
  set_fact(&b, MURO_STEP_NOW, 0);
  memset(&o, 0, sizeof o);
  bk = book_at(owing, MURO_STEP_NOW, sl_of(req), max_body, 0, 0, b.in.now_ms, b.in.deadline,
               b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail(name);
  }
  snprintf(tag, sizeof tag, "%s-now", name);
  if (check_shape(DEC_OWE, &o, bk, &b, code, word, 0, 0, 1, tag) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, 0, 0, 1, name, &b, &first,
             &seen) < 0)
    return -1;
  owing = http_step_take(first);
  if (!seen.bytes || !seen.len) {
    box_free(&b);
    return fail(name);
  }
  set_fact(&b, MURO_STEP_WROTE, seen.len);
  memset(&o, 0, sizeof o);
  bk = book_at(owing, MURO_STEP_WROTE, sl_of(req), max_body, (uint64_t)seen.len, (uint64_t)seen.len,
               b.in.now_ms, b.in.deadline, b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    fprintf(stderr, "%s-wrote: host step failed\n", name);
    return -1;
  }
  snprintf(tag, sizeof tag, "%s-wrote", name);
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, tag) < 0) {
    box_free(&b);
    return -1;
  }
  owing = http_step_take(bk);
  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  memset(&o, 0, sizeof o);
  bk = book_at(owing, MURO_STEP_PEER_CLOSED, sl_of(req), max_body, 0, 0, b.in.now_ms, b.in.deadline,
               b.st.idle_ms);
  rc = muro_http_step(&b.st, &b.in, &o);
  if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, 0, 0, 0, name) < 0) {
    box_free(&b);
    return -1;
  }
  if (rc < 0 || (o.bytes && o.len) || (o.status_now && o.status && o.status_len) || !o.close_conn ||
      o.read_more || o.write_more || o.wake_at != 0) {
    fprintf(stderr, "%s: host after the error write close=%d read=%d write=%d len=%zu\n", name,
            o.close_conn, o.read_more, o.write_more, o.len);
    box_free(&b);
    return -1;
  }
  scripted += 1;
  box_free(&b);
  return 0;
}

static int owe_failed(const char *req, uint64_t max_body, uint64_t code, const char *word,
                      const char *name) {
  Box b;
  StepOut o;
  http_step_Out first;
  http_step_Out bk;
  http_step_St owing;
  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, 0, 0, 1, name, &b, &first,
             NULL) < 0)
    return -1;
  owing = http_step_take(first);
  set_fact(&b, MURO_STEP_WRITE_FAILED, 0);
  memset(&o, 0, sizeof o);
  bk = book_at(owing, MURO_STEP_WRITE_FAILED, sl_of(req), max_body, 0, 0, b.in.now_ms,
               b.in.deadline, b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    fprintf(stderr, "%s: handwritten-owe-failed step failed\n", name);
    return -1;
  }
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, name) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int owe_short(const char *req, uint64_t max_body, uint64_t code, const char *word,
                     const char *name) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out first;
  http_step_Out bk;
  http_step_St owing;
  size_t n;
  char tag[80];
  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, 0, 0, 1, name, &b, &first,
             &seen) < 0)
    return -1;
  owing = http_step_take(first);
  if (!seen.bytes || seen.len < 2) {
    box_free(&b);
    return fail(name);
  }
  n = 1;
  set_fact(&b, MURO_STEP_WROTE, n);
  memset(&o, 0, sizeof o);
  bk = book_at(owing, MURO_STEP_WROTE, sl_of(req), max_body, (uint64_t)n, (uint64_t)seen.len,
               b.in.now_ms, b.in.deadline, b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail(name);
  }
  snprintf(tag, sizeof tag, "%s-short", name);
  if (check_shape(DEC_STILL, &o, bk, &b, code, NULL, (uint64_t)seen.len - 1, 0, 1, tag) < 0) {
    box_free(&b);
    return -1;
  }
  owing = http_step_take(bk);
  set_fact(&b, MURO_STEP_WROTE, o.len);
  bk = book_at(owing, MURO_STEP_WROTE, sl_of(req), max_body, (uint64_t)o.len, 0, b.in.now_ms,
               b.in.deadline, b.st.idle_ms);
  memset(&o, 0, sizeof o);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail(name);
  }
  snprintf(tag, sizeof tag, "%s-rest", name);
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, tag) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

/* n > owed completes. owed 0 with left 0 finishes in the book only.
   n == 0 stores left = owed, and the next write of that left finishes
   even when the owed argument is 0. */
static int owe_edges(const char *req, uint64_t max_body, uint64_t code, const char *word,
                     const char *name) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out first;
  http_step_Out bk;
  http_step_St owing;
  char tag[80];

  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, 0, 0, 1, name, &b, &first,
             &seen) < 0)
    return -1;
  owing = http_step_take(first);
  if (!seen.bytes || !seen.len) {
    box_free(&b);
    return fail(name);
  }
  snprintf(tag, sizeof tag, "%s-over", name);
  set_fact(&b, MURO_STEP_WROTE, seen.len + 8);
  memset(&o, 0, sizeof o);
  bk = book_at(owing, MURO_STEP_WROTE, sl_of(req), max_body, (uint64_t)seen.len + 8,
               (uint64_t)seen.len, b.in.now_ms, b.in.deadline, b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail(tag);
  }
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, tag) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  snprintf(tag, sizeof tag, "%s-owed0", name);
  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, 0, 0, 1, tag, &b, &first,
             &seen) < 0)
    return -1;
  owing = http_step_take(first);
  bk = book_at(owing, MURO_STEP_WROTE, sl_of(req), max_body, 0, 0, b.in.now_ms, b.in.deadline,
               b.st.idle_ms);
  if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, 0, 0, 0, tag) < 0) {
    box_free(&b);
    return -1;
  }
  scripted += 1;
  box_free(&b);

  snprintf(tag, sizeof tag, "%s-zero", name);
  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, 0, 0, 1, tag, &b, &first,
             &seen) < 0)
    return -1;
  owing = http_step_take(first);
  if (!seen.bytes || !seen.len) {
    box_free(&b);
    return fail(tag);
  }
  set_fact(&b, MURO_STEP_WROTE, 0);
  memset(&o, 0, sizeof o);
  bk = book_at(owing, MURO_STEP_WROTE, sl_of(req), max_body, 0, (uint64_t)seen.len, b.in.now_ms,
               b.in.deadline, b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail(tag);
  }
  if (check_shape(DEC_STILL, &o, bk, &b, code, NULL, (uint64_t)seen.len, 0, 1, tag) < 0) {
    box_free(&b);
    return -1;
  }
  owing = http_step_take(bk);
  set_fact(&b, MURO_STEP_WROTE, seen.len);
  snprintf(tag, sizeof tag, "%s-stale", name);
  bk = book_at(owing, MURO_STEP_WROTE, sl_of(req), max_body, (uint64_t)seen.len, 1, b.in.now_ms,
               b.in.deadline, b.st.idle_ms);
  memset(&o, 0, sizeof o);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail(tag);
  }
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, tag) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int cap_buf(int which, char **out, const char **word, uint64_t *code) {
  const char *tail = " HTTP/1.1\r\nHost: h\r\n\r\n";
  const char *head = "GET / HTTP/1.1\r\nHost: h\r\nX-Pad: ";
  size_t tail_n = strlen(tail);
  size_t head_n = strlen(head);
  char *req = malloc(17000 + head_n + 64);
  if (!req) return fail("cap alloc");
  if (which == 6) {
    memcpy(req, "GET /", 5);
    memset(req + 5, 'a', 17000);
    memcpy(req + 5 + 17000, tail, tail_n + 1);
    *word = "uri too long";
    *code = 414;
  } else {
    memcpy(req, head, head_n);
    memset(req + head_n, 'b', 17000);
    memcpy(req + head_n + 17000, "\r\n\r\n", 5);
    *word = "header fields";
    *code = 431;
  }
  *out = req;
  return 0;
}

static int cap_span(int which, const char *name) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out first;
  http_step_Out bk;
  http_step_St owing;
  char *req = NULL;
  const char *word = NULL;
  uint64_t code = 0;
  char tag[80];
  if (cap_buf(which, &req, &word, &code) < 0) return -1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, req, 8388608, code, word, 0, 0, 1, name, &b, &first,
             &seen) < 0) {
    free(req);
    return -1;
  }
  if (has_lit(seen.bytes, seen.len, "400") && code != 400) {
    fprintf(stderr, "%s: host text has 400\n", name);
    box_free(&b);
    free(req);
    return -1;
  }
  owing = http_step_take(first);
  if (!seen.bytes || seen.len < 2) {
    box_free(&b);
    free(req);
    return fail(name);
  }
  set_fact(&b, MURO_STEP_WROTE, 1);
  bk = book_at(owing, MURO_STEP_WROTE, sl_of(req), 8388608, 1, (uint64_t)seen.len, b.in.now_ms,
               b.in.deadline, b.st.idle_ms);
  memset(&o, 0, sizeof o);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    free(req);
    return fail(name);
  }
  snprintf(tag, sizeof tag, "%s-short", name);
  if (check_shape(DEC_STILL, &o, bk, &b, code, NULL, (uint64_t)seen.len - 1, 0, 1, tag) < 0) {
    box_free(&b);
    free(req);
    return -1;
  }
  owing = http_step_take(bk);
  set_fact(&b, MURO_STEP_WROTE, o.len);
  bk = book_at(owing, MURO_STEP_WROTE, sl_of(req), 8388608, (uint64_t)o.len, 0, b.in.now_ms,
               b.in.deadline, b.st.idle_ms);
  memset(&o, 0, sizeof o);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    free(req);
    return fail(name);
  }
  snprintf(tag, sizeof tag, "%s-done", name);
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, tag) < 0) {
    box_free(&b);
    free(req);
    return -1;
  }
  box_free(&b);
  free(req);
  return 0;
}

/* Install buf when the fact carries bytes. Otherwise the book sees
   the host buffer as it stands before this call. Book runs first. */
static int step_both(Box *b, http_step_St *st, int fact, const char *buf, uint64_t wrote_n,
                     uint64_t owed, StepOut *o, http_step_Out *bk, const char *name) {
  muro__slice sl;
  if (fact == MURO_STEP_BYTES || fact == MURO_STEP_PEER_CLOSED) {
    size_t n = buf ? strlen(buf) : 0;
    if (n > IN_CAP) return fail(name);
    if (n) memcpy(b->inb, buf, n);
    muro_http_step_set_in_len(&b->st, n);
    sl = sl_of(buf);
  } else {
    sl = sl_mem(b->inb, muro_http_step_in_len(&b->st));
  }
  set_fact(b, fact, (size_t)wrote_n);
  if (fact == MURO_STEP_BYTES || fact == MURO_STEP_PEER_CLOSED) b->in.empty_before = 1;
  *bk = book_at(*st, fact, sl, b->st.max_body, wrote_n, owed, b->in.now_ms, b->in.deadline,
                b->st.idle_ms);
  memset(o, 0, sizeof *o);
  if (muro_http_step(&b->st, &b->in, o) < 0) {
    fprintf(stderr, "%s: host step failed\n", name);
    return -1;
  }
  return 0;
}

static int open_acc(Box *b, uint64_t max_body, uint64_t now, uint64_t dl) {
  StepOut o;
  http_step_Out bk;
  uint64_t wake;
  if (box_bind(b, max_body) < 0) return -1;
  set_clock(b, now, dl);
  set_fact(b, MURO_STEP_ACCEPTED, 0);
  wake = now + (uint64_t)b->st.idle_ms;
  bk = book_at(http_step_readSt(), MURO_STEP_ACCEPTED, sl_of(""), max_body, 0, 0, now, dl,
               b->st.idle_ms);
  if (muro_http_step(&b->st, &b->in, &o) < 0) {
    box_free(b);
    return fail("accept");
  }
  if (check_shape(DEC_MORE, &o, bk, b, 0, NULL, 0, wake, 0, "accept") < 0) {
    box_free(b);
    return -1;
  }
  return 0;
}

static int reply_bytes(Box *b, http_step_St *st, const char *buf, uint64_t keep, StepOut *seen,
                       const char *name) {
  StepOut o;
  http_step_Out bk;
  if (step_both(b, st, MURO_STEP_BYTES, buf, 0, 0, &o, &bk, name) < 0) return -1;
  if (check_shape(DEC_REPLY, &o, bk, b, 0, NULL, 0, 0, keep, name) < 0) return -1;
  *st = http_step_take(bk);
  if (seen) *seen = o;
  return 0;
}

static int full_write(Box *b, http_step_St *st, StepOut seen, int kind, uint64_t code,
                      uint64_t wake, const char *name) {
  StepOut o;
  http_step_Out bk;
  if (step_both(b, st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                name) < 0)
    return -1;
  if (check_shape(kind, &o, bk, b, code, NULL, 0, wake, 0, name) < 0) return -1;
  *st = http_step_take(bk);
  return 0;
}

static int keep_alive(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  uint64_t wake;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "ka-get") < 0) {
    box_free(&b);
    return -1;
  }
  wake = armed(1000, 0, (uint64_t)b.st.idle_ms);
  if (full_write(&b, &st, seen, DEC_IDLE, 0, wake, "ka-idle") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != 0) {
    box_free(&b);
    return fail("ka-idle len");
  }
  if (reply_bytes(&b, &st, GET, 0, &seen, "ka-second") < 0) {
    box_free(&b);
    return -1;
  }
  if (full_write(&b, &st, seen, DEC_IDLE, 0, wake, "ka-second-idle") < 0) {
    box_free(&b);
    return -1;
  }
  if (step_both(&b, &st, MURO_STEP_PEER_CLOSED, "", 0, 0, &o, &bk, "ka-peer") < 0) {
    box_free(&b);
    return -1;
  }
  if (check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, "ka-peer") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_phase(&b.st) != MURO_STEP_PH_CLOSED) {
    box_free(&b);
    return fail("ka-peer phase");
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "now-before-get") < 0 ||
      full_write(&b, &st, seen, DEC_IDLE, 0, wake, "now-before-idle") < 0) {
    box_free(&b);
    return -1;
  }
  set_clock(&b, 1000, 61000);
  if (step_both(&b, &st, MURO_STEP_NOW, NULL, 0, 0, &o, &bk, "now-before") < 0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, "now-before") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "now-after-get") < 0 ||
      full_write(&b, &st, seen, DEC_IDLE, 0, wake, "now-after-idle") < 0) {
    box_free(&b);
    return -1;
  }
  set_clock(&b, 70000, 61000);
  if (step_both(&b, &st, MURO_STEP_NOW, NULL, 0, 0, &o, &bk, "now-after") < 0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, "now-after") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int clocks(void) {
  Box b;
  StepOut o;
  StepOut seen;
  http_step_Out bk;
  http_step_St st;
  uint64_t idle;
  if (open_acc(&b, 10, 1000, 424242) < 0) return -1;
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  idle = (uint64_t)b.st.idle_ms;
  set_clock(&b, 1000, 424242);
  if (step_both(&b, &st, MURO_STEP_BYTES, LINE, 0, 0, &o, &bk, "line-keep") < 0 ||
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, 424242, 0, "line-keep") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  set_clock(&b, 1000, 0);
  if (step_both(&b, &st, MURO_STEP_BYTES, LINE, 0, 0, &o, &bk, "line-arm") < 0 ||
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, armed(1000, 0, idle), 0, "line-arm") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  set_clock(&b, 1000, 424242);
  if (step_both(&b, &st, MURO_STEP_BYTES, GET, 0, 0, &o, &bk, "reply-wake") < 0 ||
      check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, "reply-wake") < 0) {
    box_free(&b);
    return -1;
  }
  seen = o;
  st = http_step_take(bk);
  set_clock(&b, 1000, 424242);
  if (full_write(&b, &st, seen, DEC_IDLE, 0, 424242, "idle-keep") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  set_clock(&b, 1000, 0);
  if (reply_bytes(&b, &st, GET, 0, &seen, "idle-arm-get") < 0 ||
      full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, idle), "idle-arm") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int one_byte(const char *buf, uint64_t keep, int done_kind, const char *name) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  int guard = 0;
  char tag[80];
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, buf, keep, &seen, name) < 0) {
    box_free(&b);
    return -1;
  }
  while (seen.len > 1 && guard < 100000) {
    uint64_t owed = (uint64_t)seen.len;
    set_fact(&b, MURO_STEP_WROTE, 1);
    bk = book_at(st, MURO_STEP_WROTE, sl_mem(b.inb, muro_http_step_in_len(&b.st)), b.st.max_body, 1,
                 guard == 0 ? owed : 0, b.in.now_ms, b.in.deadline, b.st.idle_ms);
    memset(&o, 0, sizeof o);
    if (muro_http_step(&b.st, &b.in, &o) < 0) {
      box_free(&b);
      return fail(name);
    }
    snprintf(tag, sizeof tag, "%s-1", name);
    if (check_shape(DEC_STILL, &o, bk, &b, 0, NULL, owed - 1, 0, keep, tag) < 0) {
      box_free(&b);
      return -1;
    }
    st = http_step_take(bk);
    seen = o;
    guard += 1;
  }
  if (guard >= 100000) {
    box_free(&b);
    return fail(name);
  }
  if (full_write(&b, &st, seen, done_kind, 0, done_kind == DEC_IDLE ? armed(1000, 0, 60000) : 0,
                 name) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int pipeline(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  size_t one = strlen(GET);
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, TWO, 0, &seen, "pipe-one") < 0) {
    box_free(&b);
    return -1;
  }
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "pipe-second") < 0 ||
      check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, "pipe-second") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != one) {
    fprintf(stderr, "pipe-second in_len %zu\n", muro_http_step_in_len(&b.st));
    box_free(&b);
    return -1;
  }
  seen = o;
  st = http_step_take(bk);
  if (full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, 60000), "pipe-done") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != 0) {
    box_free(&b);
    return fail("pipe-done len");
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, PIPE, 0, &seen, "pipe-part") < 0) {
    box_free(&b);
    return -1;
  }
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "pipe-partial") < 0 ||
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, armed(1000, 0, 60000), 0, "pipe-partial") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != strlen(LINE)) {
    box_free(&b);
    return fail("pipe-partial len");
  }
  if (muro_http_step_phase(&b.st) != MURO_STEP_PH_READ) {
    box_free(&b);
    return fail("pipe-partial phase");
  }
  st = http_step_take(bk);
  if (reply_bytes(&b, &st, GET, 0, &seen, "pipe-finish") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

/* Peer during a fresh handler span sets keep and leaves left 0.
   The host span length stays the rendered length. A later full
   write closes and drops the pipelined tail. */
static int peer_drops(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  size_t before;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, TWO, 0, &seen, "drop-get") < 0) {
    box_free(&b);
    return -1;
  }
  before = seen.len;
  if (step_both(&b, &st, MURO_STEP_PEER_CLOSED, TWO, 0, 0, &o, &bk, "drop-peer") < 0) {
    box_free(&b);
    return -1;
  }
  if (book_is(bk, 3, 0, 0, 0, 0, 1, 2, 0, 0, 0, 1, "drop-peer") < 0 ||
      host_flags(&o, 0, 0, 0, 1, "drop-peer") < 0 || wake_is(&o, 0, "drop-peer") < 0 ||
      o.len != before) {
    dump_book("drop-peer", bk);
    box_free(&b);
    return -1;
  }
  scripted += 1;
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)o.len, (uint64_t)o.len, &o, &bk,
                "drop-done") < 0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, "drop-done") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int reply_fail_and_now(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  size_t full;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "fail-get") < 0) {
    box_free(&b);
    return -1;
  }
  if (step_both(&b, &st, MURO_STEP_WRITE_FAILED, NULL, 0, 0, &o, &bk, "fail-reply") < 0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, "fail-reply") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "now-span") < 0) {
    box_free(&b);
    return -1;
  }
  full = seen.len;
  if (step_both(&b, &st, MURO_STEP_NOW, NULL, 0, 0, &o, &bk, "now-span") < 0 ||
      check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, "now-span") < 0 || o.len != full) {
    box_free(&b);
    return fail("now-span len");
  }
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, 1, (uint64_t)full, &o, &bk, "now-left") < 0 ||
      check_shape(DEC_STILL, &o, bk, &b, 0, NULL, (uint64_t)full - 1, 0, 0, "now-left") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  full = o.len;
  if (step_both(&b, &st, MURO_STEP_NOW, NULL, 0, 0, &o, &bk, "now-keeps-left") < 0 ||
      check_shape(DEC_STILL, &o, bk, &b, 0, NULL, (uint64_t)full, 0, 0, "now-keeps-left") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_WRITE_FAILED, NULL, 0, 0, &o, &bk, "fail-mid") < 0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, "fail-mid") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int closes(void) {
  Box b;
  StepOut seen;
  http_step_St st;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, CLOSE, 1, &seen, "close-reply") < 0 ||
      full_write(&b, &st, seen, DEC_QUIET, 0, 0, "close-done") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, HTTP10, 1, &seen, "http10-reply") < 0 ||
      full_write(&b, &st, seen, DEC_QUIET, 0, 0, "http10-done") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, HEAD, 0, &seen, "head-reply") < 0 ||
      full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, 60000), "head-done") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, POST_BODY, 0, &seen, "post-reply") < 0 ||
      full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, 60000), "post-idle") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int match_covered(const StepOut *o, http_step_Out bk, const Box *b, const char *name) {
  if (bo_cover(bk) == 1) return 0;
  if (o->interim || o->cont) return fail(name);
  if ((uint64_t)o->close_conn != bo_close(bk) || (uint64_t)o->status_now != bo_status(bk) ||
      (uint64_t)o->read_more != bo_read(bk) || (uint64_t)o->write_more != bo_write(bk) ||
      o->wake_at != bo_wake(bk)) {
    fprintf(stderr, "%s: host close=%d status=%d read=%d write=%d wake=%llu\n", name, o->close_conn,
            o->status_now, o->read_more, o->write_more, (unsigned long long)o->wake_at);
    dump_book(name, bk);
    return -1;
  }
  if (bo_write(bk) == 1) {
    if (!o->bytes || !o->len || !in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
    if (bo_left(bk) != 0 && o->len != bo_left(bk)) {
      fprintf(stderr, "%s: host len %zu book left %llu\n", name, o->len,
              (unsigned long long)bo_left(bk));
      return -1;
    }
  } else if (o->bytes && o->len) {
    return fail(name);
  }
  if (bo_status(bk) == 1) {
    if (!o->status || !o->status_len ||
        !in_range(o->status, o->status_len, b->st.spare, sizeof b->st.spare))
      return fail(name);
  }
  if (bo_phase(bk) == 4) {
    if (muro_http_step_phase(&b->st) != MURO_STEP_PH_IDLE) return fail(name);
    if (muro_http_step_in_len(&b->st) != 0) return fail(name);
  }
  if (bo_phase(bk) == 2 && bo_status(bk) == 0) {
    if (muro_http_step_phase(&b->st) != MURO_STEP_PH_CLOSED) return fail(name);
  }
  return 0;
}

static uint64_t pick_n(int k, uint64_t owed) {
  switch (k % 5) {
  case 0:
    return 0;
  case 1:
    return 1;
  case 2:
    return owed / 2;
  default:
    return k % 5 == 3 ? owed : owed + 4;
  }
}

static int run_seq(const char *buf, const int *facts, int depth, uint64_t max_body) {
  Box b;
  http_step_St st;
  StepOut o;
  http_step_Out bk;
  uint64_t owed = 0;
  int i;
  sweep_seq += 1;
  count_shape = 0;
  if (open_acc(&b, max_body, 1000, 0) < 0) {
    count_shape = 1;
    return -1;
  }
  count_shape = 1;
  st = http_step_readSt();
  for (i = 0; i < depth; i++) {
    int fact = facts[i];
    uint64_t n = fact == MURO_STEP_WROTE ? pick_n(i, owed) : 0;
    uint64_t pass_owed = fact == MURO_STEP_WROTE ? owed : 0;
    const char *deliver = (fact == MURO_STEP_BYTES || fact == MURO_STEP_PEER_CLOSED) ? buf : NULL;
    sweep_steps += 1;
    if (step_both(&b, &st, fact, deliver, n, pass_owed, &o, &bk, "sweep") < 0) {
      box_free(&b);
      return -1;
    }
    if (bo_cover(bk) == 1) {
      sweep_covered += 1;
      box_free(&b);
      return 0;
    }
    if (match_covered(&o, bk, &b, "sweep") < 0) {
      fprintf(stderr, "sweep step %d fact %d buflen %zu\n", i, fact, buf ? strlen(buf) : 0);
      box_free(&b);
      return -1;
    }
    st = http_step_take(bk);
    owed = (o.write_more && o.len) ? (uint64_t)o.len : 0;
  }
  box_free(&b);
  return 0;
}

static int sweep(void) {
  static const int facts5[5] = {MURO_STEP_BYTES, MURO_STEP_PEER_CLOSED, MURO_STEP_WROTE,
                                MURO_STEP_WRITE_FAILED, MURO_STEP_NOW};
  static const char *bufs[4];
  int a, c, d, e;
  int seq[4];
  uint32_t xs = 0xC0FFEEu;
  int n;
  bufs[0] = "";
  bufs[1] = LINE;
  bufs[2] = GET;
  bufs[3] = BAD;
  for (e = 0; e < 4; e++) {
    for (a = 0; a < 5; a++) {
      for (c = 0; c < 5; c++) {
        for (d = 0; d < 5; d++) {
          seq[0] = facts5[a];
          seq[1] = facts5[c];
          seq[2] = facts5[d];
          if (run_seq(bufs[e], seq, 3, 10) < 0) return -1;
        }
      }
    }
  }
  for (n = 0; n < 256; n++) {
    int i;
    for (i = 0; i < 4; i++) {
      xs ^= xs << 13;
      xs ^= xs >> 17;
      xs ^= xs << 5;
      seq[i] = facts5[xs % 5];
    }
    xs ^= xs << 13;
    xs ^= xs >> 17;
    xs ^= xs << 5;
    if (run_seq(bufs[xs % 4], seq, 4, 10) < 0) return -1;
  }
  return 0;
}

int main(void) {
  uint64_t wake = 61000;
  if (decide(DEC_MORE, MURO_STEP_ACCEPTED, "", 10, 0, NULL, 0, wake, 0, "accept", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_MORE, MURO_STEP_BYTES, LINE, 10, 0, NULL, 0, wake, 0, "line", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_MORE, MURO_STEP_BYTES, HDR, 10, 0, NULL, 0, wake, 0, "headers", NULL, NULL, NULL) <
      0)
    return 1;
  if (decide(DEC_MORE, MURO_STEP_BYTES, POST_SHORT, 10, 0, NULL, 0, wake, 0, "cl-short", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_REPLY, MURO_STEP_BYTES, GET, 10, 0, NULL, 0, 0, 0, "get", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_REPLY, MURO_STEP_BYTES, POST_BODY, 10, 0, NULL, 0, 0, 0, "cl-body", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, TOO_BIG, 10, 413, "too large", 0, 0, 0, "close-413", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, EXPECT, 8388608, 417, "expectation", 0, 0, 0, "close-417",
             NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, TRACE, 8388608, 501, "unimplemented", 0, 0, 0, "close-501",
             NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, POST_BODY, 0, 413, "too large", 0, 0, 1, "owe-413", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, EXPECT_DONE, 8388608, 417, "expectation", 0, 0, 1, "owe-417",
             NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, TRACE_DONE, 8388608, 501, "unimplemented", 0, 0, 1, "owe-501",
             NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, GZIP, 10, 501, "unimplemented", 0, 0, 1, "owe-gzip", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, BAD, 10, 400, "malformed", 0, 0, 1, "owe-400", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, NO_HOST, 10, 400, "malformed", 0, 0, 1, "owe-400-host", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_QUIET, MURO_STEP_PEER_CLOSED, "", 10, 0, NULL, 0, 0, 0, "fin-empty", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_PEER_CLOSED, LINE, 10, 400, "malformed", 0, 0, 1, "fin-partial",
             NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_PEER_CLOSED, TOO_BIG, 10, 400, "malformed", 0, 0, 1, "fin-oversize",
             NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OUT, MURO_STEP_BYTES, CHUNK, 10, 0, NULL, 0, 0, 0, "chunk", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OUT, MURO_STEP_BYTES, CONT, 10, 0, NULL, 0, 0, 0, "continue", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_QUIET, MURO_STEP_NOW, GET, 10, 0, NULL, 0, 0, 0, "now", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OUT, MURO_STEP_WROTE, GET, 10, 0, NULL, 0, 0, 0, "wrote-read", NULL, NULL, NULL) <
      0)
    return 1;
  if (decide(DEC_OUT, MURO_STEP_PEER_CLOSED, GET, 10, 0, NULL, 0, 0, 0, "fin-get", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_OUT, MURO_STEP_PEER_CLOSED, POST_BODY, 10, 0, NULL, 0, 0, 0, "fin-body", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, STREAM, 10, 413, "too large", 0, 0, 1, "stream-413", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OUT, MURO_STEP_BYTES, STREAM, 8388608, 0, NULL, 0, 0, 0, "stream-outside", NULL,
             NULL, NULL) < 0)
    return 1;

  if (after_status(TOO_BIG, 10, 413, "413") < 0) return 1;
  if (after_status(EXPECT, 8388608, 417, "417") < 0) return 1;
  if (after_status(TRACE, 8388608, 501, "501") < 0) return 1;

  if (owe_then(BAD, 10, 400, "malformed", "400") < 0) return 1;
  if (owe_failed(BAD, 10, 400, "malformed", "400-failed") < 0) return 1;
  if (owe_short(BAD, 10, 400, "malformed", "400-short") < 0) return 1;
  if (owe_edges(BAD, 10, 400, "malformed", "400-edges") < 0) return 1;
  if (cap_span(6, "cap-414") < 0) return 1;
  if (cap_span(7, "cap-431") < 0) return 1;

  if (keep_alive() < 0) return 1;
  if (clocks() < 0) return 1;
  if (one_byte(GET, 0, DEC_IDLE, "get-bytes") < 0) return 1;
  if (one_byte(CLOSE, 1, DEC_QUIET, "close-bytes") < 0) return 1;
  if (pipeline() < 0) return 1;
  if (peer_drops() < 0) return 1;
  if (reply_fail_and_now() < 0) return 1;
  if (closes() < 0) return 1;
  if (sweep() < 0) return 1;

  printf("handwritten-after-close-spans %d\n", after_spans);
  printf("scripted %d\n", scripted);
  printf("sweep-seq %d sweep-steps %d sweep-covered %d\n", sweep_seq, sweep_steps, sweep_covered);
  if (after_spans != 0) return 1;
  printf("OK\n");
  return 0;
}
