/* Same StepIn bytes for c/step.c and the emitted slice.
   The host stays muro_http_step. http_step_stepFact is the book
   in c/generated/step_slice.c. This file compares the fields the
   slice decides.

   A finished request is framed in the book and a handler response
   in the host. Those bytes must lie in the caller send buffer.
   The status text is not compared field for field.

   After a 413, 417, or 501 status_now close, both the book and
   the handwritten step must stay quiet. c/step.c now marks the
   connection shut, so a later fact returns close with no span. */

#include "step.h"
#include "host_stream.h"
#include "step_slice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IN_CAP = 65536, OUT_CAP = 69632 };
enum { DEC_MORE = 1, DEC_FRAMED, DEC_STATUS, DEC_OWE, DEC_QUIET, DEC_OUT };

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

static int after_spans;

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

static muro__slice sl_of(const char *s) {
  muro__slice sl;
  sl.p = (const uint8_t *)(s ? s : "");
  sl.n = s ? strlen(s) : 0;
  return sl;
}

static void set_fact(Box *b, int kind, size_t n) {
  memset(&b->in, 0, sizeof b->in);
  b->in.kind = kind;
  b->in.n = n;
  b->in.now_ms = 1000;
  b->in.date = (const uint8_t *)DATE;
  b->in.date_n = 29;
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
  return 0;
}

static int book_is(http_step_Out o, uint64_t phase, uint64_t code, uint64_t close, uint64_t status,
                   uint64_t rd, uint64_t wr, uint64_t span, uint64_t cover, const char *name) {
  if (bo_phase(o) == phase && bo_code(o) == code && bo_close(o) == close && bo_status(o) == status &&
      bo_read(o) == rd && bo_write(o) == wr && bo_span(o) == span && bo_cover(o) == cover)
    return 0;
  fprintf(stderr,
          "%s: book phase=%llu code=%llu close=%llu status=%llu read=%llu write=%llu span=%llu "
          "cover=%llu\n",
          name, (unsigned long long)bo_phase(o), (unsigned long long)bo_code(o),
          (unsigned long long)bo_close(o), (unsigned long long)bo_status(o),
          (unsigned long long)bo_read(o), (unsigned long long)bo_write(o),
          (unsigned long long)bo_span(o), (unsigned long long)bo_cover(o));
  return -1;
}

static int host_flags(const StepOut *o, int close, int status, int rd, int wr, const char *name) {
  if (o->close_conn == close && o->status_now == status && o->read_more == rd && o->write_more == wr &&
      !o->interim && !o->cont)
    return 0;
  fprintf(stderr, "%s: host close=%d status_now=%d read=%d write=%d interim=%d cont=%d\n", name,
          o->close_conn, o->status_now, o->read_more, o->write_more, o->interim, o->cont);
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
                       const char *word, const char *name) {
  if (kind == DEC_OUT) {
    return book_is(bk, 0, 0, 0, 0, 0, 0, 0, 1, name);
  }
  if (kind == DEC_MORE) {
    if (book_is(bk, 0, 0, 0, 0, 1, 0, 0, 0, name) < 0) return -1;
    if (host_flags(o, 0, 0, 1, 0, name) < 0) return -1;
    if ((o->bytes && o->len) || (o->status && o->status_len)) return fail(name);
    return 0;
  }
  if (kind == DEC_QUIET) {
    if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, name) < 0) return -1;
    if (host_flags(o, 1, 0, 0, 0, name) < 0) return -1;
    if ((o->bytes && o->len) || (o->status && o->status_len) || o->status_now) return fail(name);
    return 0;
  }
  if (kind == DEC_FRAMED) {
    if (book_is(bk, 0, 0, 0, 0, 0, 0, 0, 0, name) < 0) return -1;
    if (host_flags(o, 0, 0, 0, 1, name) < 0) return -1;
    if (!o->bytes || !o->len) return fail(name);
    if (!in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
    if (!has_lit(o->bytes, o->len, "200")) return fail(name);
    return 0;
  }
  if (kind == DEC_STATUS) {
    if (book_is(bk, 2, code, 1, 1, 0, 0, 1, 0, name) < 0) return -1;
    if (host_flags(o, 1, 1, 0, 0, name) < 0) return -1;
    if (o->bytes && o->len) return fail(name);
    if (!o->status || !o->status_len) return fail(name);
    if (!in_range(o->status, o->status_len, b->st.spare, sizeof b->st.spare)) return fail(name);
    return text_ok(o, code, word, name);
  }
  if (kind == DEC_OWE) {
    if (book_is(bk, 1, code, 0, 0, 0, 1, 2, 0, name) < 0) return -1;
    if (host_flags(o, 0, 0, 0, 1, name) < 0) return -1;
    if (!o->bytes || !o->len) return fail(name);
    if (!in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
    return text_ok(o, code, word, name);
  }
  return fail(name);
}

/* fact ACCEPTED is the first call. Any other fact is preceded by
   ACCEPTED on the host only. The book starts from readSt, which is
   the same phase that accept leaves. */
static int decide(int kind, int fact, const char *buf, uint64_t max_body, uint64_t code,
                  const char *word, const char *name, Box *keep, http_step_Out *kept,
                  StepOut *seen) {
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
  if (kind != DEC_OUT && muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    fprintf(stderr, "%s: host step failed\n", name);
    return -1;
  }
  if (kind == DEC_OUT) {
    /* Still run the host. A crash fails the driver. The result is
       not a field of this slice. */
    muro_http_step(&b.st, &b.in, &o);
  }
  bk = http_step_stepFact(http_step_readSt(), (uint64_t)fact, sl_of(buf), max_body);
  if (check_shape(kind, &o, bk, &b, code, word, name) < 0) {
    box_free(&b);
    return -1;
  }
  if (seen) *seen = o;
  if (keep) {
    /* The step stores pointers into this box. A struct copy leaves
       those pointers on the donor, which is about to return. */
    *keep = b;
    keep->st.hin = &keep->hin;
    keep->st.read_more = &keep->rd;
    keep->st.write_more = &keep->wr;
    keep->hin.pub.in = &keep->hin;
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
  if (decide(DEC_STATUS, MURO_STEP_BYTES, req, max_body, code, NULL, name, &b, &first, NULL) < 0)
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
  rc = muro_http_step(&b.st, &b.in, &o);
  bk = http_step_stepFact(closed, (uint64_t)fact, sl_of(fact == MURO_STEP_BYTES ? next : req),
                          max_body);
  if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, name) < 0) {
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
  if (span || !o.close_conn || o.read_more || o.write_more) {
    fprintf(stderr, "%s: handwritten after close must stay quiet\n", name);
    box_free(&b);
    return -1;
  }
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

  /* Peer close while the error span is still outstanding. */
  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, name, &b, &first, &seen) < 0)
    return -1;
  owing = http_step_take(first);
  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  memset(&o, 0, sizeof o);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    fprintf(stderr, "%s-peer: host step failed\n", name);
    return -1;
  }
  bk = http_step_stepFact(owing, MURO_STEP_PEER_CLOSED, sl_of(req), max_body);
  if (check_shape(DEC_OWE, &o, bk, &b, code, word, name) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  /* A full write of that span closes. A later peer close stays quiet.
     The book ignores the byte count. The host is given the whole span. */
  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, name, &b, &first, &seen) < 0)
    return -1;
  owing = http_step_take(first);
  if (!seen.bytes || !seen.len) {
    box_free(&b);
    return fail(name);
  }
  set_fact(&b, MURO_STEP_WROTE, seen.len);
  memset(&o, 0, sizeof o);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    fprintf(stderr, "%s-wrote: host step failed\n", name);
    return -1;
  }
  bk = http_step_stepFact(owing, MURO_STEP_WROTE, sl_of(req), max_body);
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, name) < 0) {
    box_free(&b);
    return -1;
  }
  owing = http_step_take(bk);
  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  memset(&o, 0, sizeof o);
  rc = muro_http_step(&b.st, &b.in, &o);
  bk = http_step_stepFact(owing, MURO_STEP_PEER_CLOSED, sl_of(req), max_body);
  if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, name) < 0) {
    box_free(&b);
    return -1;
  }
  if (rc < 0) {
    printf("handwritten-after-wrote %s span=err close=err read=err\n", name);
    box_free(&b);
    return 0;
  }
  if ((o.bytes && o.len) || (o.status_now && o.status && o.status_len) || !o.close_conn ||
      o.read_more || o.write_more) {
    fprintf(stderr, "%s: host after the error write close=%d read=%d write=%d len=%zu\n", name,
            o.close_conn, o.read_more, o.write_more, o.len);
    box_free(&b);
    return -1;
  }
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
  int rc;
  if (decide(DEC_OWE, MURO_STEP_BYTES, req, max_body, code, word, name, &b, &first, NULL) < 0)
    return -1;
  owing = http_step_take(first);
  set_fact(&b, MURO_STEP_WRITE_FAILED, 0);
  memset(&o, 0, sizeof o);
  rc = muro_http_step(&b.st, &b.in, &o);
  bk = http_step_stepFact(owing, MURO_STEP_WRITE_FAILED, sl_of(req), max_body);
  if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, name) < 0) {
    box_free(&b);
    return -1;
  }
  if (rc < 0) {
    printf("handwritten-owe-failed %s=err\n", name);
    box_free(&b);
    return 0;
  }
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, name) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

int main(void) {
  if (decide(DEC_MORE, MURO_STEP_ACCEPTED, "", 10, 0, NULL, "accept", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_MORE, MURO_STEP_BYTES, LINE, 10, 0, NULL, "line", NULL, NULL, NULL) < 0) return 1;
  if (decide(DEC_MORE, MURO_STEP_BYTES, HDR, 10, 0, NULL, "headers", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_MORE, MURO_STEP_BYTES, POST_SHORT, 10, 0, NULL, "cl-short", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_FRAMED, MURO_STEP_BYTES, GET, 10, 0, NULL, "get", NULL, NULL, NULL) < 0) return 1;
  if (decide(DEC_FRAMED, MURO_STEP_BYTES, POST_BODY, 10, 0, NULL, "cl-body", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, TOO_BIG, 10, 413, "too large", "close-413", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, EXPECT, 8388608, 417, "expectation", "close-417", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, TRACE, 8388608, 501, "unimplemented", "close-501", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, POST_BODY, 0, 413, "too large", "owe-413", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, EXPECT_DONE, 8388608, 417, "expectation", "owe-417", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, TRACE_DONE, 8388608, 501, "unimplemented", "owe-501", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, GZIP, 10, 501, "unimplemented", "owe-gzip", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, BAD, 10, 400, "malformed", "owe-400", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, NO_HOST, 10, 400, "malformed", "owe-400-host", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_QUIET, MURO_STEP_PEER_CLOSED, "", 10, 0, NULL, "fin-empty", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_PEER_CLOSED, LINE, 10, 400, "malformed", "fin-partial", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_OWE, MURO_STEP_PEER_CLOSED, TOO_BIG, 10, 400, "malformed", "fin-oversize", NULL,
             NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OUT, MURO_STEP_BYTES, CHUNK, 10, 0, NULL, "chunk", NULL, NULL, NULL) < 0) return 1;
  if (decide(DEC_OUT, MURO_STEP_BYTES, CONT, 10, 0, NULL, "continue", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_OUT, MURO_STEP_NOW, GET, 10, 0, NULL, "now", NULL, NULL, NULL) < 0) return 1;
  if (decide(DEC_OUT, MURO_STEP_WROTE, GET, 10, 0, NULL, "wrote-read", NULL, NULL, NULL) < 0)
    return 1;

  if (after_status(TOO_BIG, 10, 413, "413") < 0) return 1;
  if (after_status(EXPECT, 8388608, 417, "417") < 0) return 1;
  if (after_status(TRACE, 8388608, 501, "501") < 0) return 1;

  if (owe_then(BAD, 10, 400, "malformed", "400") < 0) return 1;
  if (owe_failed(BAD, 10, 400, "malformed", "400-failed") < 0) return 1;

  printf("handwritten-after-close-spans %d\n", after_spans);
  if (after_spans != 0) return 1;
  printf("OK\n");
  return 0;
}
