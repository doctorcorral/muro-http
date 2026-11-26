/* Same StepIn for c/step.c and the emitted slice.
   The host stays muro_http_step. http_step_stepFact is the book
   in c/generated/step_slice.c. This file compares the fields the
   slice decides.

   A finished request is a handler span in the book (book phase 3)
   and a rendered response in the host. Those bytes must lie in the
   caller send buffer, or in the interim buffer when the span is
   the 100. The status text is not compared field for field. Book
   phases are not image phases: book 0 read, 1 protocol write, 2
   closed, 3 handler write, 4 idle, 6 interim, 7 read after the
   interim, 8 interim after a peer close; image 0 read, 1 handle,
   2 write, 3 idle, 4 closed.

   Clocks are the adapter's. idle_ms is the value passed to bind.
   Accept arms now+idle and ignores a carried deadline. A read or an
   idle park keeps a carried deadline, and arms now+idle when it is 0.
   A write parks with wake 0. NOW from read or idle closes. The book
   does not compare the two clocks.

   cover 1 is an unknown fact. The host returns -1 for that fact.
   A streamed body whose Content-Length is within max while the
   peer is still open is a handler reply at the request keep. The
   handler then forces Connection: close when that body stays open
   and the response does not pull it. Accept during a write and
   accept while idle are a fresh read. Chunked bodies are in the
   slice: a short tail reads again, a finished body is a handler
   span, a bad size is 400, and a size past max is 413. 100 Continue
   is in the slice. Book phase 6 owes the interim (span 6). Phase 8
   is that interim after the peer has closed. Phase 7 is the read
   after the interim was sent. Bytes during
   a write park the same span. A wrote or a failure while the book
   is reading or idle stays there. A finished request on a peer
   close is in the slice: a handler span with keep 1, and that fresh
   reply says Connection: close. A within-max streamed body on a
   peer close is that same reply. The sweep includes those bodies,
   and it includes ACCEPTED so a write and an idle step can reset.
   Every sweep step marked cover 1 fails. Bytes during a write, and
   a wrote or a failure while reading or idle, are scripted covered
   cases.

   A wrote with left 0 and owed 0 during a write finishes in the
   book. That input is not a host fact. The owed-0 check is
   book-only.

   finishWrite sees the unconsumed slice, the host buffer as it
   stands. It drops one framed prefix. Re-passing the original
   two-GET buffer after that reply is outside the contract. */

#include "step.h"
#include "host_stream.h"
#include "step_slice.h"
#include "step_book.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IN_CAP = 65536, OUT_CAP = 69632 };
enum { DEC_MORE = 1, DEC_REPLY, DEC_STATUS, DEC_OWE, DEC_QUIET, DEC_OUT, DEC_STILL, DEC_IDLE };

static const char DATE[] = "Sun, 06 Nov 1994 08:49:37 GMT";

static const char *LINE = "GET / HT";
static const char *HDR = "GET / HTTP/1.1\r\nHost: h\r\n";
static const char *GET = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
static const char *GET2 = "GET /x HTTP/1.1\r\nHost: h\r\n\r\n";
static const char *TWO_DIFF =
    "GET / HTTP/1.1\r\nHost: h\r\n\r\nGET /x HTTP/1.1\r\nHost: h\r\n\r\n";
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
static const char *CHUNK_ONE =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n";
static const char *CHUNK_EXT =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=1\r\nwiki\r\n0\r\n\r\n";
static const char *CHUNK_TRAILER =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\nX-A: b\r\n\r\n";
static const char *CHUNK_BAD =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n";
static const char *CHUNK_SPACE =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n4 \r\nwiki\r\n0\r\n\r\n";
static const char *CHUNK_EXT_LF =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n4;bad\n\r\nwiki\r\n0\r\n\r\n";
static const char *CHUNK_OVERFLOW =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n";
static const char *CHUNK_TOO =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n1000000\r\n";
static const char *CHUNK_PREM =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhi";
static const char *CHUNK_CLOSE =
    "POST / HTTP/1.1\r\nHost: h\r\nConnection: close\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
static const char *CHUNK_PIPE =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nGET / HTTP/1.1\r\nHost: "
    "h\r\n\r\n";
static const char *CHUNK_MULTI =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n1\r\nb\r\n0\r\n\r\n";
static const char *CHUNK_EXPECT =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nExpect: foo\r\n\r\n";
static const char *CHUNK_EXPECT_DONE =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nExpect: foo\r\n\r\n0\r\n\r\n";
static const char *CHUNK_TRACE = "TRACE / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
static const char *CHUNK_TRACE_DONE =
    "TRACE / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n";
static const char *CHUNK_BARE =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nNope\r\n\r\n";
static const char *CONT =
    "POST / HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n";
static const char *CONT_FULL =
    "POST / HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\nping";
static const char *CONT_10 =
    "POST / HTTP/1.0\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\n";
static const char *CONT_HEAD =
    "HEAD / HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 4\r\n\r\n";
static const char *CONT_BIG =
    "POST / HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\nContent-Length: 100\r\n\r\n";
static const char *CONT_CHUNK =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n";
static const char *CONT_CHUNK_DONE =
    "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n0\r\n\r\n";
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
static int book_cmp;
static int book_skip;
/* Quiet closes the host answers from PEND_SHUT with its image still
   READ (after status_now). c/step.c keeps PEND_SHUT private; 3 is its
   value there. */
#define TEST_PEND_SHUT 3
static int quiet_read_shut;
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
  uint8_t *bin;
  uint8_t *bout;
  HttpStep st;
  struct HostIn hin;
  BookStep bk;
  struct HostIn bhin;
  int rd;
  int wr;
  int brd;
  int bwr;
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
static uint64_t bo_hend(http_step_Out o) { return o.muro__as.out.muro__f_11; }
static uint64_t bo_te(http_step_Out o) { return o.muro__as.out.muro__f_12; }
static uint64_t bo_sawCL(http_step_Out o) { return o.muro__as.out.muro__f_13; }
static uint64_t bo_cl(http_step_Out o) { return o.muro__as.out.muro__f_14; }

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

/* 0 when the header block is unfinished (headerEnd is the incomplete
   sentinel sliceLen+1). A complete block's end is never 0. */
static uint64_t slice_hend(const char *buf) {
  muro__slice sl = sl_of(buf);
  uint64_t h = http_step_headerEnd(sl);
  if (h == 0 || h == sl.n + 1) return 0;
  return h;
}

static void slice_fr(const char *buf, uint64_t *te, uint64_t *sawCL, uint64_t *cl) {
  muro__slice sl = sl_of(buf);
  uint64_t hend = slice_hend(buf);
  http_step_HSt hs;
  if (hend == 0) {
    *te = 0;
    *sawCL = 0;
    *cl = 0;
    return;
  }
  muro_http_reset();
  hs = http_step_scan(sl);
  *te = http_step_teWhen(hend, hs, sl);
  *sawCL = http_step_sawCLWhen(hend, hs, sl);
  *cl = http_step_clWhen(hend, hs, sl);
  muro_http_reset();
}

static uint64_t slice_te(const char *buf) {
  uint64_t te, sawCL, cl;
  slice_fr(buf, &te, &sawCL, &cl);
  return te;
}

static uint64_t slice_sawCL(const char *buf) {
  uint64_t te, sawCL, cl;
  slice_fr(buf, &te, &sawCL, &cl);
  return sawCL;
}

static uint64_t slice_cl(const char *buf) {
  uint64_t te, sawCL, cl;
  slice_fr(buf, &te, &sawCL, &cl);
  return cl;
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

/* A status token is the HTTP/1.1 status-line prefix, or a buffer
   that is only the decimal code. A later header must not satisfy it. */
static int status_token(const uint8_t *p, size_t n, uint64_t code) {
  char pref[40];
  char num[16];
  int pn;
  int nn;
  if (code == 0) return 1;
  pn = snprintf(pref, sizeof pref, "HTTP/1.1 %llu ", (unsigned long long)code);
  nn = snprintf(num, sizeof num, "%llu", (unsigned long long)code);
  if (pn < 0 || nn < 0 || (size_t)pn >= sizeof pref || (size_t)nn >= sizeof num) return 0;
  if (p && n >= (size_t)pn && memcmp(p, pref, (size_t)pn) == 0) return 1;
  if (p && n == (size_t)nn && memcmp(p, num, (size_t)nn) == 0) return 1;
  return 0;
}

static int has_code(const uint8_t *p, size_t n, uint64_t code) {
  return status_token(p, n, code);
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
  muro_book_shutdown(&b->bk);
  host_in_close(&b->hin);
  host_in_close(&b->bhin);
  free(b->inb);
  free(b->outb);
  free(b->bin);
  free(b->bout);
  memset(b, 0, sizeof *b);
}

static int box_bind(Box *b, uint64_t max_body) {
  memset(b, 0, sizeof *b);
  b->inb = calloc(1, IN_CAP);
  b->outb = calloc(1, OUT_CAP);
  b->bin = calloc(1, IN_CAP);
  b->bout = calloc(1, OUT_CAP);
  if (!b->inb || !b->outb || !b->bin || !b->bout) {
    free(b->inb);
    free(b->outb);
    free(b->bin);
    free(b->bout);
    memset(b, 0, sizeof *b);
    return fail("alloc");
  }
  host_in_init(&b->hin, -1, max_body);
  host_in_init(&b->bhin, -1, max_body);
  muro_http_step_bind(&b->st, b->inb, IN_CAP, b->outb, OUT_CAP, &b->hin, &b->rd, &b->wr, on_req,
                      NULL, max_body, 60000);
  muro_book_bind(&b->bk, b->bin, IN_CAP, b->bout, OUT_CAP, &b->bhin, &b->brd, &b->bwr, on_req, NULL,
                 max_body, 60000);
  set_clock(b, 1000, 0);
  return 0;
}

static void dump_book(const char *name, http_step_Out o) {
  fprintf(stderr,
          "%s: book phase=%llu code=%llu close=%llu status=%llu read=%llu write=%llu span=%llu "
          "cover=%llu left=%llu wake=%llu keep=%llu hend=%llu te=%llu sawCL=%llu cl=%llu\n",
          name, (unsigned long long)bo_phase(o), (unsigned long long)bo_code(o),
          (unsigned long long)bo_close(o), (unsigned long long)bo_status(o),
          (unsigned long long)bo_read(o), (unsigned long long)bo_write(o),
          (unsigned long long)bo_span(o), (unsigned long long)bo_cover(o),
          (unsigned long long)bo_left(o), (unsigned long long)bo_wake(o),
          (unsigned long long)bo_keep(o), (unsigned long long)bo_hend(o),
          (unsigned long long)bo_te(o), (unsigned long long)bo_sawCL(o),
          (unsigned long long)bo_cl(o));
}

static int book_is(http_step_Out o, uint64_t phase, uint64_t code, uint64_t close, uint64_t status,
                   uint64_t rd, uint64_t wr, uint64_t span, uint64_t cover, uint64_t left,
                   uint64_t wake, uint64_t keep, uint64_t hend, uint64_t te, uint64_t sawCL,
                   uint64_t cl, const char *name) {
  if (bo_phase(o) == phase && bo_code(o) == code && bo_close(o) == close && bo_status(o) == status &&
      bo_read(o) == rd && bo_write(o) == wr && bo_span(o) == span && bo_cover(o) == cover &&
      bo_left(o) == left && bo_wake(o) == wake && bo_keep(o) == keep && bo_hend(o) == hend &&
      bo_te(o) == te && bo_sawCL(o) == sawCL && bo_cl(o) == cl)
    return 0;
  dump_book(name, o);
  return -1;
}

static int book_hend(http_step_Out o, uint64_t hend, const char *name) {
  if (bo_hend(o) == hend) return 0;
  fprintf(stderr, "%s: book hend %llu want %llu\n", name, (unsigned long long)bo_hend(o),
          (unsigned long long)hend);
  return -1;
}

static int book_facts(http_step_Out o, uint64_t te, uint64_t sawCL, uint64_t cl, const char *name) {
  if (bo_te(o) == te && bo_sawCL(o) == sawCL && bo_cl(o) == cl) return 0;
  fprintf(stderr, "%s: book facts te=%llu sawCL=%llu cl=%llu want %llu %llu %llu\n", name,
          (unsigned long long)bo_te(o), (unsigned long long)bo_sawCL(o),
          (unsigned long long)bo_cl(o), (unsigned long long)te, (unsigned long long)sawCL,
          (unsigned long long)cl);
  return -1;
}

/* Host HttpStep.hend, adapter BookStep.bhend, and the book's Out.hend
   must agree. This is how a dropped host-bridge field is caught. */
static int hend_agree(const HttpStep *st, const BookStep *bk, http_step_Out o, uint64_t hend,
                      const char *name) {
  if (bo_hend(o) == hend && bk && bk->bhend == hend && (!st || st->hend == hend)) return 0;
  fprintf(stderr, "%s: hend out %llu host %llu book %llu want %llu\n", name,
          (unsigned long long)bo_hend(o), (unsigned long long)(st ? st->hend : 0),
          (unsigned long long)(bk ? bk->bhend : 0), (unsigned long long)hend);
  return -1;
}

static int facts_agree(const HttpStep *st, const BookStep *bk, http_step_Out o, uint64_t te,
                       uint64_t sawCL, uint64_t cl, const char *name) {
  int ok = bo_te(o) == te && bo_sawCL(o) == sawCL && bo_cl(o) == cl && bk && bk->bte == te &&
           bk->bsawcl == sawCL && bk->bcl == cl;
  if (ok && st && (st->te != te || st->sawcl != sawCL || st->cl != cl)) ok = 0;
  if (ok) return 0;
  fprintf(stderr,
          "%s: facts out %llu %llu %llu book %llu %llu %llu host %llu %llu %llu want %llu %llu "
          "%llu\n",
          name, (unsigned long long)bo_te(o), (unsigned long long)bo_sawCL(o),
          (unsigned long long)bo_cl(o), (unsigned long long)(bk ? bk->bte : 0),
          (unsigned long long)(bk ? bk->bsawcl : 0), (unsigned long long)(bk ? bk->bcl : 0),
          (unsigned long long)(st ? st->te : 0), (unsigned long long)(st ? st->sawcl : 0),
          (unsigned long long)(st ? st->cl : 0), (unsigned long long)te, (unsigned long long)sawCL,
          (unsigned long long)cl);
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

static int phase_is(const Box *b, int phase, const char *name) {
  int got = muro_http_step_phase(&b->st);
  if (got == phase) return 0;
  fprintf(stderr, "%s: image phase %d want %d\n", name, got, phase);
  return -1;
}

static int conn_is(const uint8_t *p, size_t n, uint64_t keep) {
  const char *lit = keep == 1 ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
  return has_lit(p, n, lit);
}

static int flag_true(http_step_Bool b) {
  return b == http_step_muro__tag_Bool_true_k;
}

static uint64_t st_phase(http_step_St st) { return st.muro__as.st.muro__f_0; }

static int check_shape(int kind, const StepOut *o, http_step_Out bk, const Box *b, uint64_t code,
                       const char *word, uint64_t left, uint64_t wake, uint64_t keep, uint64_t hend,
                       uint64_t te, uint64_t sawCL, uint64_t cl, const char *name) {
  if (wake_is(o, wake, name) < 0) return -1;
  if (kind == DEC_MORE) {
    if (book_is(bk, 0, 0, 0, 0, 1, 0, 0, 0, 0, wake, 0, hend, te, sawCL, cl, name) < 0) return -1;
    if (host_flags(o, 0, 0, 1, 0, name) < 0) return -1;
    if ((o->bytes && o->len) || (o->status && o->status_len)) return fail(name);
    if (phase_is(b, MURO_STEP_PH_READ, name) < 0) return -1;
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_QUIET) {
    if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, 0, 0, 0, hend, te, sawCL, cl, name) < 0) return -1;
    if (host_flags(o, 1, 0, 0, 0, name) < 0) return -1;
    if ((o->bytes && o->len) || (o->status && o->status_len) || o->status_now) return fail(name);
    if (phase_is(b, MURO_STEP_PH_CLOSED, name) < 0) return -1;
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_REPLY) {
    if (book_is(bk, 3, 0, 0, 0, 0, 1, 2, 0, left, 0, keep, hend, te, sawCL, cl, name) < 0) return -1;
    if (host_flags(o, 0, 0, 0, 1, name) < 0) return -1;
    if (!o->bytes || !o->len) return fail(name);
    if (!in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
    if (phase_is(b, MURO_STEP_PH_WRITE, name) < 0) return -1;
    if (left == 0) {
      if (o->bytes != b->outb) return fail(name);
      if (!status_token(o->bytes, o->len, 200)) return fail(name);
      if (!conn_is(o->bytes, o->len, keep)) return fail(name);
    } else if (o->len != left) {
      return fail(name);
    }
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_IDLE) {
    if (book_is(bk, 4, 0, 0, 0, 1, 0, 0, 0, 0, wake, 0, hend, te, sawCL, cl, name) < 0) return -1;
    if (host_flags(o, 0, 0, 1, 0, name) < 0) return -1;
    if ((o->bytes && o->len) || (o->status && o->status_len)) return fail(name);
    if (phase_is(b, MURO_STEP_PH_IDLE, name) < 0) return -1;
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_STATUS) {
    if (book_is(bk, 2, code, 1, 1, 0, 0, 1, 0, 0, 0, 0, hend, te, sawCL, cl, name) < 0) return -1;
    if (host_flags(o, 1, 1, 0, 0, name) < 0) return -1;
    if (o->bytes && o->len) return fail(name);
    if (!o->status || !o->status_len) return fail(name);
    if (!in_range(o->status, o->status_len, b->st.spare, sizeof b->st.spare)) return fail(name);
    if (text_ok(o, code, word, name) < 0) return -1;
    /* status_now closes from read. The image stays READ. */
    if (phase_is(b, MURO_STEP_PH_READ, name) < 0) return -1;
    if (count_shape) scripted += 1;
    return 0;
  }
  if (kind == DEC_OWE) {
    if (book_is(bk, 1, code, 0, 0, 0, 1, 2, 0, 0, 0, 1, hend, te, sawCL, cl, name) < 0) return -1;
    if (host_flags(o, 0, 0, 0, 1, name) < 0) return -1;
    if (!o->bytes || !o->len || o->bytes != b->outb) return fail(name);
    if (!in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
    if (text_ok(o, code, word, name) < 0) return -1;
    if (!conn_is(o->bytes, o->len, 1)) return fail(name);
    if (phase_is(b, MURO_STEP_PH_WRITE, name) < 0) return -1;
    if (count_shape) scripted += 1;
    return 0;
  }
  return fail(name);
}

/* phase is the book phase: 1 protocol write, 3 handler write. */
static int check_still(int phase, const StepOut *o, http_step_Out bk, const Box *b, uint64_t code,
                       uint64_t left, uint64_t keep, uint64_t hend, uint64_t te, uint64_t sawCL,
                       uint64_t cl, const char *name) {
  if (phase != 1 && phase != 3) return fail(name);
  if (book_is(bk, (uint64_t)phase, code, 0, 0, 0, 1, 2, 0, left, 0, keep, hend, te, sawCL, cl, name) < 0) return -1;
  if (host_flags(o, 0, 0, 0, 1, name) < 0) return -1;
  if (!o->bytes || o->len != left) return fail(name);
  if (!in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
  if (phase_is(b, MURO_STEP_PH_WRITE, name) < 0) return -1;
  /* A zero write leaves the pointer on the status line. A later
     short write points into the middle of the span. */
  if (o->bytes == b->outb) {
    uint64_t tok = code == 0 ? 200 : code;
    if (!status_token(o->bytes, o->len, tok)) return fail(name);
    if (!conn_is(o->bytes, o->len, keep)) return fail(name);
  }
  if (count_shape) scripted += 1;
  return 0;
}

static http_step_Out book_at(http_step_St st, int fact, muro__slice sl, uint64_t max, uint64_t n,
                             uint64_t owed, uint64_t now, uint64_t dl, int idle_ms) {
  /* Out is returned by value. Dropping the arena here keeps a long
     sweep, plus the cover predicate's classify, inside 64MB. */
  muro_http_reset();
  return http_step_stepFact(st, (uint64_t)fact, sl, max, n, owed, now, dl, (uint64_t)idle_ms);
}

/* The real host step. The macro below sends every later call through
   the book adapter as well. Pointers are into each side's own buffer.
   Compare the offset and the bytes, not the pointer identity. */
static int (*real_step)(HttpStep *, const StepIn *, StepOut *) = muro_http_step;

static int same_span(const StepOut *h, const uint8_t *hb, const StepOut *a, const uint8_t *ab) {
  size_t ho;
  size_t ao;
  int hlive = h->bytes && h->len;
  int alive = a->bytes && a->len;
  if (!hlive && !alive) return 1;
  if (!hlive || !alive || h->len != a->len || !hb || !ab) return 0;
  if (h->bytes < hb || a->bytes < ab) return 0;
  ho = (size_t)(h->bytes - hb);
  ao = (size_t)(a->bytes - ab);
  if (ho != ao) return 0;
  return memcmp(h->bytes, a->bytes, h->len) == 0;
}

/* A response span lives in the send buffer. The 100 lives in cont.
   Offsets are compared inside one of those regions, not across them. */
static int same_out_bytes(const StepOut *h, const Box *b, const StepOut *a) {
  int hlive = h->bytes && h->len;
  int alive = a->bytes && a->len;
  if (!hlive && !alive) return 1;
  if (in_range(h->bytes, h->len, b->outb, OUT_CAP) && in_range(a->bytes, a->len, b->bout, OUT_CAP) &&
      same_span(h, b->outb, a, b->bout))
    return 1;
  if (in_range(h->bytes, h->len, b->st.cont, sizeof b->st.cont) &&
      in_range(a->bytes, a->len, b->bk.cont, sizeof b->bk.cont) &&
      same_span(h, b->st.cont, a, b->bk.cont))
    return 1;
  return 0;
}

static int same_status(const StepOut *h, const StepOut *a) {
  int hlive = h->status && h->status_len;
  int alive = a->status && a->status_len;
  if (!hlive && !alive) return 1;
  if (!hlive || !alive || h->status_len != a->status_len) return 0;
  return memcmp(h->status, a->status, h->status_len) == 0;
}

/* cover 1, and a later fact on that shut step. The host is not the
   oracle here: the adapter's own shape is. want_note 0 means the
   shut path, which sets no note. */
static int adapter_shut(const StepOut *bo, const BookStep *bk, int brc, int want_note) {
  if (brc != 0) return 0;
  if (muro_book_phase(bk) != MURO_STEP_PH_CLOSED) return 0;
  if (bo->note != want_note) return 0;
  if (!bo->close_conn || bo->status_now || bo->wake_at) return 0;
  if (bo->read_more || bo->write_more || bo->interim || bo->cont) return 0;
  if ((bo->bytes && bo->len) || (bo->status && bo->status_len)) return 0;
  if (bk->read_more && *bk->read_more) return 0;
  if (bk->write_more && *bk->write_more) return 0;
  return 1;
}

static int protos_agree(const Proto *h, const Proto *a) {
  ProtoMove x, y;
  int n;
  int i;
  int hl = h && proto_live(h);
  int al = a && proto_live(a);
  int hd = h && proto_done(h);
  int ad = a && proto_done(a);
  if (hl != al || hd != ad) return 0;
  if (!h || !a) return h == a;
  n = proto_move_count(h);
  if (n != proto_move_count(a)) return 0;
  for (i = 0; i < n; i++) {
    if (!proto_move_at(h, i, &x) || !proto_move_at(a, i, &y)) return 0;
    if (x.dir != y.dir || x.kind != y.kind || x.bit != y.bit) return 0;
    if ((x.ctor == NULL) != (y.ctor == NULL)) return 0;
    if (x.ctor && strcmp(x.ctor, y.ctor) != 0) return 0;
  }
  return 1;
}

static int host_and_book(HttpStep *st, const StepIn *in, StepOut *out) {
  Box *b = (Box *)((unsigned char *)st - offsetof(Box, st));
  StepOut bo;
  int rc;
  int brc;
  int skip;
  size_t n;
  if (!b->bin || !in || !out) return real_step(st, in, out);
  n = st->in_len;
  if (n > b->bk.in_cap) n = b->bk.in_cap;
  if (n && st->in) memcpy(b->bin, st->in, n);
  muro_book_set_in_len(&b->bk, n);
  skip = b->bk.blind;
  rc = real_step(st, in, out);
  memset(&bo, 0, sizeof bo);
  brc = muro_book_step(&b->bk, in, &bo);
  if (b->bk.last_cover) {
    book_skip += 1;
    if (!adapter_shut(&bo, &b->bk, brc, MURO_BOOK_NOTE_COVER)) {
      fprintf(stderr, "book adapter cover fact %d note %d phase %d read %d write %d len %zu\n",
              in->kind, bo.note, muro_book_phase(&b->bk), bo.read_more, bo.write_more, bo.len);
      return -1;
    }
    return rc;
  }
  if (skip) {
    book_skip += 1;
    if (!adapter_shut(&bo, &b->bk, brc, 0)) {
      fprintf(stderr, "book adapter shut fact %d note %d phase %d len %zu\n", in->kind, bo.note,
              muro_book_phase(&b->bk), bo.len);
      return -1;
    }
    return rc;
  }
  book_cmp += 1;
  if (rc != brc || muro_http_step_phase(st) != muro_book_phase(&b->bk) ||
      muro_http_step_in_len(st) != muro_book_in_len(&b->bk) ||
      (muro_http_step_in_len(st) && memcmp(st->in, b->bin, st->in_len) != 0) ||
      out->read_more != bo.read_more || out->write_more != bo.write_more ||
      out->close_conn != bo.close_conn || out->status_now != bo.status_now ||
      out->interim != bo.interim || out->cont != bo.cont || out->wake_at != bo.wake_at ||
      out->note != bo.note || out->note2 != bo.note2 || !same_out_bytes(out, b, &bo) ||
      !same_status(out, &bo)) {
    fprintf(stderr,
            "book adapter fact %d phase host %d book %d in %zu/%zu close %d/%d read %d/%d "
            "write %d/%d wake %llu/%llu len %zu/%zu note %d/%d rc %d/%d\n",
            in->kind, muro_http_step_phase(st), muro_book_phase(&b->bk), muro_http_step_in_len(st),
            muro_book_in_len(&b->bk), out->close_conn, bo.close_conn, out->read_more, bo.read_more,
            out->write_more, bo.write_more, (unsigned long long)out->wake_at,
            (unsigned long long)bo.wake_at, out->len, bo.len, out->note, bo.note, rc, brc);
    return -1;
  }
  if (!protos_agree(st->proto, b->bk.proto)) {
    ProtoMove mv;
    int hn = st->proto ? proto_move_count(st->proto) : -1;
    int an = b->bk.proto ? proto_move_count(b->bk.proto) : -1;
    const char *hc = (hn > 0 && proto_move_at(st->proto, hn - 1, &mv) && mv.ctor) ? mv.ctor : "-";
    fprintf(stderr,
            "book admit fact %d live %d/%d done %d/%d moves %d/%d host-last %s\n", in->kind,
            st->proto && proto_live(st->proto), b->bk.proto && proto_live(b->bk.proto),
            st->proto && proto_done(st->proto), b->bk.proto && proto_done(b->bk.proto), hn, an, hc);
    return -1;
  }
  return rc;
}

#define muro_http_step(st, in, out) host_and_book((st), (in), (out))

/* fact ACCEPTED is the first call. Any other fact is preceded by
   ACCEPTED on the host only. The book starts from readSt. */
static int decide(int kind, int fact, const char *buf, uint64_t max_body, uint64_t code,
                  const char *word, uint64_t left, uint64_t wake, uint64_t keep, const char *name,
                  Box *keepb, http_step_Out *kept, StepOut *seen) {
  Box b;
  StepOut o;
  http_step_Out bk;
  uint64_t hend = 0;
  uint64_t te = 0, sawCL = 0, cl = 0;
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
  if ((kind == DEC_REPLY || kind == DEC_OWE || (kind == DEC_MORE && fact == MURO_STEP_BYTES)))
    hend = slice_hend(buf);
  if (hend) slice_fr(buf, &te, &sawCL, &cl);
  if (check_shape(kind, &o, bk, &b, code, word, left, wake, keep, hend, te, sawCL, cl, name) < 0) {
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
    keepb->bk.hin = &keepb->bhin;
    keepb->bk.read_more = &keepb->brd;
    keepb->bk.write_more = &keepb->bwr;
    keepb->bhin.pub.in = &keepb->bhin;
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
  if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, name) < 0) {
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
  if (check_shape(DEC_OWE, &o, bk, &b, code, word, 0, 0, 1, slice_hend(req), slice_te(req), slice_sawCL(req), slice_cl(req), tag) < 0) {
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
  if (check_shape(DEC_OWE, &o, bk, &b, code, word, 0, 0, 1, slice_hend(req), slice_te(req), slice_sawCL(req), slice_cl(req), tag) < 0) {
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
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, 0, 0, 0, 0, tag) < 0) {
    box_free(&b);
    return -1;
  }
  owing = http_step_take(bk);
  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  memset(&o, 0, sizeof o);
  bk = book_at(owing, MURO_STEP_PEER_CLOSED, sl_of(req), max_body, 0, 0, b.in.now_ms, b.in.deadline,
               b.st.idle_ms);
  rc = muro_http_step(&b.st, &b.in, &o);
  if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, name) < 0) {
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
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, 0, 0, 0, 0, name) < 0) {
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
  if (check_still(1, &o, bk, &b, code, (uint64_t)seen.len - 1, 1, slice_hend(req), slice_te(req), slice_sawCL(req), slice_cl(req), tag) < 0) {
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
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, 0, 0, 0, 0, tag) < 0) {
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
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, 0, 0, 0, 0, tag) < 0) {
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
  if (book_is(bk, 2, code, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, tag) < 0) {
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
  if (check_still(1, &o, bk, &b, code, (uint64_t)seen.len, 1, slice_hend(req), slice_te(req), slice_sawCL(req), slice_cl(req), tag) < 0) {
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
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, 0, 0, 0, 0, tag) < 0) {
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
  {
    muro__slice sl = sl_of(req);
    int done;
    uint64_t end;
    uint64_t cap;
    muro_http_reset();
    done = flag_true(http_step_headersDone(sl));
    end = http_step_headerEnd(sl);
    cap = http_step_capCode(sl);
    /* Both scripted caps close the header block, so headersDone is
       true and headerEnd is past 16384. capCode splits them: no LF in
       the first 16384 bytes is 414, a line that fit is 431. */
    if (!done || end <= 16384 || cap != code) {
      fprintf(stderr, "%s: headersDone %d headerEnd %llu cap %llu code %llu\n", name, done,
              (unsigned long long)end, (unsigned long long)cap, (unsigned long long)code);
      box_free(&b);
      free(req);
      return -1;
    }
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
  if (check_still(1, &o, bk, &b, code, (uint64_t)seen.len - 1, 1, slice_hend(req), slice_te(req), slice_sawCL(req), slice_cl(req), tag) < 0) {
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
  if (check_shape(DEC_QUIET, &o, bk, &b, code, NULL, 0, 0, 0, 0, 0, 0, 0, tag) < 0) {
    box_free(&b);
    free(req);
    return -1;
  }
  box_free(&b);
  free(req);
  return 0;
}

/* prepare_in drops a pending interim when the recv buffer was empty.
   step_both always sets that flag. The book has no such input, so
   this call rewinds phase 6, 7, or 8 to a fresh read before stepFact.
   A kept interim is a handwritten empty_before of 0. */
static http_step_St interim_cleared(http_step_St st) {
  uint64_t ph = st_phase(st);
  if (ph == 6 || ph == 7 || ph == 8) return http_step_readSt();
  return st;
}

/* Install buf when the fact carries bytes. Otherwise the book sees
   the host buffer as it stands before this call: the unconsumed
   slice. finishWrite drops one framed prefix from that slice.
   Passing the original full buffer after the host has consumed is
   outside that contract. The book would classify the first request
   again. Book runs first. */
static int step_both(Box *b, http_step_St *st, int fact, const char *buf, uint64_t wrote_n,
                     uint64_t owed, StepOut *o, http_step_Out *bk, const char *name) {
  muro__slice sl;
  http_step_St use = *st;
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
  if (fact == MURO_STEP_BYTES || fact == MURO_STEP_PEER_CLOSED) {
    b->in.empty_before = 1;
    use = interim_cleared(*st);
  }
  *bk = book_at(use, fact, sl, b->st.max_body, wrote_n, owed, b->in.now_ms, b->in.deadline,
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
  if (check_shape(DEC_MORE, &o, bk, b, 0, NULL, 0, wake, 0, 0, 0, 0, 0,  "accept") < 0) {
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
  if (check_shape(DEC_REPLY, &o, bk, b, 0, NULL, 0, 0, keep, slice_hend(buf), slice_te(buf), slice_sawCL(buf), slice_cl(buf), name) < 0) return -1;
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
  if (check_shape(kind, &o, bk, b, code, NULL, 0, wake, 0, 0, 0, 0, 0, name) < 0) return -1;
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
  {
    http_step_Out idle_bk =
        book_at(st, MURO_STEP_NOW, sl_mem(b.inb, muro_http_step_in_len(&b.st)), b.st.max_body, 0, 0,
                b.in.now_ms, b.in.deadline, b.st.idle_ms);
    if (book_hend(idle_bk, 0, "ka-idle") < 0 || book_facts(idle_bk, 0, 0, 0, "ka-idle") < 0) {
      box_free(&b);
      return -1;
    }
  }
  /* reply_bytes checks Out.hend against slice_hend(GET): the idle
     end was 0 and this GET rebinds it. */
  if (reply_bytes(&b, &st, GET, 0, &seen, "ka-second") < 0) {
    box_free(&b);
    return -1;
  }
  if (full_write(&b, &st, seen, DEC_IDLE, 0, wake, "ka-second-idle") < 0) {
    box_free(&b);
    return -1;
  }
  {
    http_step_Out idle_bk =
        book_at(st, MURO_STEP_NOW, sl_mem(b.inb, muro_http_step_in_len(&b.st)), b.st.max_body, 0, 0,
                b.in.now_ms, b.in.deadline, b.st.idle_ms);
    if (book_hend(idle_bk, 0, "ka-second-idle") < 0 || book_facts(idle_bk, 0, 0, 0, "ka-second-idle") < 0) {
      box_free(&b);
      return -1;
    }
  }
  if (step_both(&b, &st, MURO_STEP_PEER_CLOSED, "", 0, 0, &o, &bk, "ka-peer") < 0) {
    box_free(&b);
    return -1;
  }
  if (check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, 0, 0, 0, 0,  "ka-peer") < 0) {
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
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, 0, 0, 0, 0,  "now-before") < 0) {
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
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, 0, 0, 0, 0,  "now-after") < 0) {
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
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, 424242, 0, 0, 0, 0, 0,  "line-keep") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  set_clock(&b, 1000, 0);
  if (step_both(&b, &st, MURO_STEP_BYTES, LINE, 0, 0, &o, &bk, "line-arm") < 0 ||
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, armed(1000, 0, idle), 0, 0, 0, 0, 0,  "line-arm") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  set_clock(&b, 1000, 424242);
  if (step_both(&b, &st, MURO_STEP_BYTES, GET, 0, 0, &o, &bk, "reply-wake") < 0 ||
      check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "reply-wake") < 0) {
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
    if (check_still(3, &o, bk, &b, 0, owed - 1, keep, slice_hend(buf), slice_te(buf), slice_sawCL(buf), slice_cl(buf), tag) < 0) {
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
      check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "pipe-second") < 0) {
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
  /* The host buffer is now the unconsumed tail (one GET). The book
     on that tail goes idle. The book on the original two-GET buffer
     strips the first request again and parks another reply. Those
     answers differ. The host is not fed the full buffer. */
  {
    http_step_Out again =
        book_at(st, MURO_STEP_WROTE, sl_of(TWO), b.st.max_body, (uint64_t)seen.len, (uint64_t)seen.len,
                b.in.now_ms, b.in.deadline, b.st.idle_ms);
    http_step_Out tail =
        book_at(st, MURO_STEP_WROTE, sl_mem(b.inb, muro_http_step_in_len(&b.st)), b.st.max_body,
                (uint64_t)seen.len, (uint64_t)seen.len, b.in.now_ms, b.in.deadline, b.st.idle_ms);
    if (book_is(again, 3, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "pipe-full-again") < 0 ||
        book_is(tail, 4, 0, 0, 0, 1, 0, 0, 0, 0, armed(1000, 0, 60000), 0, 0, 0, 0, 0,  "pipe-tail-contract") <
            0) {
      box_free(&b);
      return -1;
    }
    if (bo_span(again) == bo_span(tail)) {
      box_free(&b);
      return fail("pipe-again-differs");
    }
    scripted += 1;
  }
  if (full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, 60000), "pipe-done") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != 0) {
    box_free(&b);
    return fail("pipe-done len");
  }
  {
    http_step_Out idle_bk =
        book_at(st, MURO_STEP_NOW, sl_mem(b.inb, muro_http_step_in_len(&b.st)), b.st.max_body, 0, 0,
                b.in.now_ms, b.in.deadline, b.st.idle_ms);
    if (book_hend(idle_bk, 0, "pipe-done") < 0 || book_facts(idle_bk, 0, 0, 0, "pipe-done") < 0) {
      box_free(&b);
      return -1;
    }
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
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, armed(1000, 0, 60000), 0, 0, 0, 0, 0,  "pipe-partial") < 0) {
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

  /* Distinct pipelined requests. GET is one byte shorter than GET2,
     so a stale first-request end would still fall inside the slid
     buffer and would not match headerEnd(GET2). */
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, TWO_DIFF, 0, &seen, "pipe-diff-one") < 0) {
    box_free(&b);
    return -1;
  }
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "pipe-diff-second") < 0 ||
      check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, slice_hend(GET2), slice_te(GET2), slice_sawCL(GET2), slice_cl(GET2),
                  "pipe-diff-second") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != strlen(GET2) || memcmp(b.inb, GET2, strlen(GET2)) != 0) {
    fprintf(stderr, "pipe-diff-second in_len %zu\n", muro_http_step_in_len(&b.st));
    box_free(&b);
    return -1;
  }
  if (slice_hend(GET) == slice_hend(GET2) || book_hend(bk, slice_hend(GET2), "pipe-diff-second") < 0 ||
        book_facts(bk, 0, 0, 0, "pipe-diff-second") < 0) {
    box_free(&b);
    return fail("pipe-diff-hend");
  }
  seen = o;
  st = http_step_take(bk);
  if (full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, 60000), "pipe-diff-done") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != 0) {
    box_free(&b);
    return fail("pipe-diff-done len");
  }
  {
    http_step_Out idle_bk =
        book_at(st, MURO_STEP_NOW, sl_mem(b.inb, muro_http_step_in_len(&b.st)), b.st.max_body, 0, 0,
                b.in.now_ms, b.in.deadline, b.st.idle_ms);
    if (book_hend(idle_bk, 0, "pipe-diff-done") < 0 || book_facts(idle_bk, 0, 0, 0, "pipe-diff-done") < 0) {
      box_free(&b);
      return -1;
    }
  }
  box_free(&b);
  return 0;
}

static int stays_quiet(Box *b, http_step_St st, int fact, const char *name);

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
  if (book_is(bk, 3, 0, 0, 0, 0, 1, 2, 0, 0, 0, 1, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "drop-peer") < 0 ||
      host_flags(&o, 0, 0, 0, 1, "drop-peer") < 0 || wake_is(&o, 0, "drop-peer") < 0 ||
      o.len != before || o.bytes != b.outb || !conn_is(o.bytes, o.len, 0)) {
    dump_book("drop-peer", bk);
    box_free(&b);
    return -1;
  }
  scripted += 1;
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)o.len, (uint64_t)o.len, &o, &bk,
                "drop-done") < 0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, 0, 0, 0, 0,  "drop-done") < 0) {
    box_free(&b);
    return -1;
  }
  /* finish with keep 1 shuts and does not consume. in_len stays the
     two-GET buffer, so the second GET is still sitting there. A later
     fact is quiet because the slot is shut, not because the bytes
     were cleared. in_len == 0 would be a different host. */
  if (muro_http_step_in_len(&b.st) != strlen(TWO) || memcmp(b.inb, TWO, strlen(TWO)) != 0) {
    fprintf(stderr, "drop-done in_len %zu\n", muro_http_step_in_len(&b.st));
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  if (stays_quiet(&b, st, MURO_STEP_BYTES, "drop-later") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

/* Peer during a short handler write forces keep and keeps the stored
   remainder. The finishing write then closes and drops the tail. */
static int peer_short(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  size_t full;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, TWO, 0, &seen, "peer-short-get") < 0) {
    box_free(&b);
    return -1;
  }
  full = seen.len;
  if (full < 2) {
    box_free(&b);
    return fail("peer-short-get");
  }
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, 1, (uint64_t)full, &o, &bk, "peer-short-wrote") <
          0 ||
      check_still(3, &o, bk, &b, 0, (uint64_t)full - 1, 0, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "peer-short-wrote") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_PEER_CLOSED, TWO, 0, 0, &o, &bk, "peer-short") < 0) {
    box_free(&b);
    return -1;
  }
  if (book_is(bk, 3, 0, 0, 0, 0, 1, 2, 0, (uint64_t)full - 1, 0, 1, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "peer-short") < 0 ||
      host_flags(&o, 0, 0, 0, 1, "peer-short") < 0 || wake_is(&o, 0, "peer-short") < 0 ||
      o.len != full - 1 || phase_is(&b, MURO_STEP_PH_WRITE, "peer-short") < 0) {
    dump_book("peer-short", bk);
    box_free(&b);
    return -1;
  }
  scripted += 1;
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)o.len, 0, &o, &bk, "peer-short-done") <
          0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, 0, 0, 0, 0,  "peer-short-done") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != strlen(TWO) || memcmp(b.inb, TWO, strlen(TWO)) != 0) {
    fprintf(stderr, "peer-short-done in_len %zu\n", muro_http_step_in_len(&b.st));
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  if (stays_quiet(&b, st, MURO_STEP_PEER_CLOSED, "peer-short-later") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

/* A stored end past the slice is ignored: headerEndAt folds again.
   Mirrors header-end-at-past in request.muro. A slide that left a
   previous request's end in place would be caught by pipe-diff and
   by the production-host tests in c/test_book_host.c. */
static int past_slice_hend(void) {
  muro__slice get = sl_of(GET);
  uint64_t end = http_step_headerEnd(get);
  uint64_t at;
  if (end == 0 || end == get.n + 1) return fail("past-slice end");
  at = http_step_headerEndAt(99, get);
  if (at != end) {
    fprintf(stderr, "past-slice At %llu want %llu\n", (unsigned long long)at,
            (unsigned long long)end);
    return -1;
  }
  if (http_step_headerEndAt(end, get) != end) return fail("past-slice stored");
  if (http_step_headerEndAt(0, get) != end) return fail("past-slice unknown");
  /* Wrong stored facts are ignored when the end is unknown or past the slice. */
  {
    uint64_t fe = http_step_framedEnd(get);
    uint64_t bad_te = 1, bad_saw = 1, bad_cl = 99;
    uint64_t at0, atp, atok;
    muro_http_reset();
    at0 = http_step_framedEndAt(0, bad_te, bad_saw, bad_cl, get);
    muro_http_reset();
    atp = http_step_framedEndAt(99, bad_te, bad_saw, bad_cl, get);
    muro_http_reset();
    atok = http_step_framedEndAt(end, 0, 0, 0, get);
    muro_http_reset();
    if (at0 != fe) return fail("past-slice framed unknown");
    if (atp != fe) return fail("past-slice framed past");
    if (atok != fe) return fail("past-slice framed stored");
  }
  return 0;
}

/* Append the rest of a GET onto the leftover "GET / HT" with
   empty_before 0. The book sees the whole slice and replies. */

/* Short POST headers, then the body bytes, with the header end
   carried on the book's Out.hend and BookStep.bhend. This file's
   muro_http_step is the handwritten host (c/step.c), which does not
   write HttpStep.hend; the production book host is covered in
   c/test_book_host.c. */
static int carry_header_end_book(void) {
  Box b;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  uint64_t hend;
  size_t have;
  const char *rest;
  size_t rn;
  muro__slice sl;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (step_both(&b, &st, MURO_STEP_BYTES, POST_SHORT, 0, 0, &o, &bk, "cl-short-carry") < 0) {
    box_free(&b);
    return -1;
  }
  sl = sl_of(POST_SHORT);
  hend = http_step_headerEnd(sl);
  if (hend == 0 || hend == sl.n + 1) {
    box_free(&b);
    return fail("cl-short-carry end");
  }
  if (check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, armed(1000, 0, 60000), 0, hend, 0, 1, 4,
                  "cl-short-carry") < 0) {
    box_free(&b);
    return -1;
  }
  if (book_hend(bk, hend, "cl-short-carry") < 0 ||
      book_facts(bk, 0, 1, 4, "cl-short-carry") < 0 ||
      hend_agree(NULL, &b.bk, bk, hend, "cl-short-carry") < 0 ||
      facts_agree(NULL, &b.bk, bk, 0, 1, 4, "cl-short-carry") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  have = strlen(POST_SHORT);
  rest = POST_BODY + have;
  rn = strlen(rest);
  if (have + rn > IN_CAP) {
    box_free(&b);
    return fail("cl-short-body");
  }
  memcpy(b.inb + have, rest, rn);
  muro_http_step_set_in_len(&b.st, have + rn);
  set_fact(&b, MURO_STEP_BYTES, 0);
  b.in.empty_before = 0;
  memset(&o, 0, sizeof o);
  bk = book_at(st, MURO_STEP_BYTES, sl_mem(b.inb, have + rn), b.st.max_body, 0, 0, b.in.now_ms,
               b.in.deadline, b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail("cl-short-body");
  }
  if (check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, hend, 0, 1, 4, "cl-short-body") < 0) {
    box_free(&b);
    return -1;
  }
  if (book_hend(bk, hend, "cl-short-body") < 0 ||
      book_facts(bk, 0, 1, 4, "cl-short-body") < 0 ||
      hend_agree(NULL, &b.bk, bk, hend, "cl-short-body") < 0 ||
      facts_agree(NULL, &b.bk, bk, 0, 1, 4, "cl-short-body") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int leftover_append(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  size_t have = strlen(LINE);
  const char *rest;
  size_t rn;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, PIPE, 0, &seen, "left-part") < 0) {
    box_free(&b);
    return -1;
  }
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "left-more") < 0 ||
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, armed(1000, 0, 60000), 0, 0, 0, 0, 0,  "left-more") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != have || memcmp(b.inb, LINE, have) != 0) {
    box_free(&b);
    return fail("left-more len");
  }
  st = http_step_take(bk);
  rest = GET + have;
  rn = strlen(rest);
  if (have + rn > IN_CAP) {
    box_free(&b);
    return fail("left-append");
  }
  memcpy(b.inb + have, rest, rn);
  muro_http_step_set_in_len(&b.st, have + rn);
  set_fact(&b, MURO_STEP_BYTES, 0);
  b.in.empty_before = 0;
  memset(&o, 0, sizeof o);
  bk = book_at(st, MURO_STEP_BYTES, sl_mem(b.inb, have + rn), b.st.max_body, 0, 0, b.in.now_ms,
               b.in.deadline, b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail("left-append");
  }
  if (check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "left-append") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

/* Idle peer of a partial request is an owed 400. Idle accept is a
   fresh read. The carried deadline is dropped. */
static int idle_edges(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  size_t n;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "idle-peer-get") < 0 ||
      full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, 60000), "idle-peer-idle") < 0) {
    box_free(&b);
    return -1;
  }
  n = strlen(LINE);
  memcpy(b.inb, LINE, n);
  muro_http_step_set_in_len(&b.st, n);
  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  b.in.empty_before = 0;
  memset(&o, 0, sizeof o);
  bk = book_at(st, MURO_STEP_PEER_CLOSED, sl_of(LINE), b.st.max_body, 0, 0, b.in.now_ms,
               b.in.deadline, b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail("idle-peer-partial");
  }
  if (check_shape(DEC_OWE, &o, bk, &b, 400, "malformed", 0, 0, 1, 0, 0, 0, 0,  "idle-peer-partial") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "idle-accept-get") < 0 ||
      full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, 60000), "idle-accept-idle") < 0) {
    box_free(&b);
    return -1;
  }
  /* ACCEPTED on a live slot. take_fact frees the previous Proto.
     Both sides return to READ with wake now+idle. */
  if (step_both(&b, &st, MURO_STEP_ACCEPTED, NULL, 0, 0, &o, &bk, "idle-accept") < 0 ||
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, armed(1000, 0, 60000), 0, 0, 0, 0, 0,  "idle-accept") < 0) {
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
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, 0, 0, 0, 0,  "fail-reply") < 0) {
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
      check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "now-span") < 0 || o.len != full) {
    box_free(&b);
    return fail("now-span len");
  }
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, 1, (uint64_t)full, &o, &bk, "now-left") < 0 ||
      check_still(3, &o, bk, &b, 0, (uint64_t)full - 1, 0, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "now-left") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  full = o.len;
  if (step_both(&b, &st, MURO_STEP_NOW, NULL, 0, 0, &o, &bk, "now-keeps-left") < 0 ||
      check_still(3, &o, bk, &b, 0, (uint64_t)full, 0, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "now-keeps-left") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_WRITE_FAILED, NULL, 0, 0, &o, &bk, "fail-mid") < 0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, 0, 0, 0, 0,  "fail-mid") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

/* After a quiet close, a later fact must not read or write.
   Book phase 2 stays quiet. The host stays shut. */
static int stays_quiet(Box *b, http_step_St st, int fact, const char *name) {
  StepOut o;
  http_step_Out bk;
  size_t before;
  int rc;
  if (fact == MURO_STEP_BYTES || fact == MURO_STEP_PEER_CLOSED) {
    size_t n = strlen(GET);
    memcpy(b->inb, GET, n);
    muro_http_step_set_in_len(&b->st, n);
  }
  set_fact(b, fact, fact == MURO_STEP_WROTE ? 1 : 0);
  b->in.empty_before = 0;
  before = muro_http_step_in_len(&b->st);
  memset(&o, 0, sizeof o);
  bk = book_at(st, fact, sl_mem(b->inb, before), b->st.max_body, fact == MURO_STEP_WROTE ? 1 : 0, 0,
               b->in.now_ms, b->in.deadline, b->st.idle_ms);
  rc = muro_http_step(&b->st, &b->in, &o);
  if (book_is(bk, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, name) < 0) return -1;
  if (rc < 0) {
    fprintf(stderr, "%s: host step failed after close\n", name);
    return -1;
  }
  if ((o.bytes && o.len) || (o.status && o.status_len) || o.status_now || o.read_more || o.write_more ||
      !o.close_conn || o.wake_at != 0 || o.interim || o.cont || b->rd || b->wr ||
      muro_http_step_in_len(&b->st) != before ||
      muro_http_step_phase(&b->st) != MURO_STEP_PH_CLOSED) {
    fprintf(stderr, "%s: activity after close close=%d read=%d write=%d len=%zu phase=%d\n", name,
            o.close_conn, o.read_more, o.write_more, o.len, muro_http_step_phase(&b->st));
    return -1;
  }
  scripted += 1;
  return 0;
}

static int after_close(Box *b, http_step_St st, const char *tag) {
  const int facts[6] = {MURO_STEP_BYTES,     MURO_STEP_WROTE, MURO_STEP_NOW,
                        MURO_STEP_PEER_CLOSED, MURO_STEP_WRITE_FAILED, MURO_STEP_ACCEPTED};
  const char *fn[6] = {"bytes", "wrote", "now", "peer", "failed", "accept"};
  int i;
  for (i = 0; i < 6; i++) {
    char name[64];
    snprintf(name, sizeof name, "%s-%s", tag, fn[i]);
    if (stays_quiet(b, st, facts[i], name) < 0) return -1;
  }
  return 0;
}

static int closes(void) {
  Box b;
  StepOut seen;
  http_step_St st;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, CLOSE, 1, &seen, "close-reply") < 0 ||
      full_write(&b, &st, seen, DEC_QUIET, 0, 0, "close-done") < 0 ||
      after_close(&b, st, "close-later") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, HTTP10, 1, &seen, "http10-reply") < 0 ||
      full_write(&b, &st, seen, DEC_QUIET, 0, 0, "http10-done") < 0 ||
      after_close(&b, st, "http10-later") < 0) {
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

/* Request keep is 0, the body is streamed, and policy accepted it.
   The wire then says close. */
static int stream_forced_close(const Box *b, http_step_Out bk) {
  muro__slice sl;
  int stream;
  uint64_t pol;
  if (bo_phase(bk) != 3 || bo_code(bk) != 0 || bo_keep(bk) != 0) return 0;
  sl = sl_mem(b->inb, muro_http_step_in_len(&b->st));
  muro_http_reset();
  stream = flag_true(http_step_streamed(sl));
  pol = http_step_policy(sl, b->st.max_body);
  muro_http_reset();
  return stream && pol == 0;
}

static http_step_St st_of(uint64_t phase, uint64_t code, uint64_t left, uint64_t keep, uint64_t hend,
                         uint64_t te, uint64_t sawCL, uint64_t cl) {
  http_step_St st;
  muro_http_reset();
  st = http_step_take(http_step_still(phase, code, left, keep, hend, te, sawCL, cl));
  muro_http_reset();
  return st;
}

static int match_covered(const StepOut *o, http_step_Out bk, const Box *b, int fact, uint64_t wrote_n,
                          uint64_t owed, int prior_book, const char *name) {
  int img;
  uint64_t tok;
  if ((uint64_t)o->close_conn != bo_close(bk) || (uint64_t)o->status_now != bo_status(bk) ||
      (uint64_t)o->read_more != bo_read(bk) || (uint64_t)o->write_more != bo_write(bk) ||
      o->wake_at != bo_wake(bk)) {
    fprintf(stderr, "%s: host close=%d status=%d read=%d write=%d wake=%llu\n", name, o->close_conn,
            o->status_now, o->read_more, o->write_more, (unsigned long long)o->wake_at);
    dump_book(name, bk);
    return -1;
  }
  /* span 6 is the interim. The bytes live in cont, not the send
     buffer. left 0 is the full literal. A positive left is the tail. */
  if (bo_span(bk) == 6) {
    img = muro_http_step_phase(&b->st);
    if (bo_write(bk) != 1 || bo_read(bk) != 0 || bo_status(bk) != 0 || bo_close(bk) != 0)
      return fail(name);
    if (!o->cont || !o->bytes || !o->len) return fail(name);
    if (!in_range(o->bytes, o->len, b->st.cont, sizeof b->st.cont)) return fail(name);
    if (o->interim != (o->bytes == b->st.cont)) return fail(name);
    if (o->bytes == b->st.cont && !status_token(o->bytes, o->len, 100)) return fail(name);
    if (bo_left(bk) == 0) {
      if (o->bytes != b->st.cont) return fail(name);
    } else if (o->len != bo_left(bk)) {
      fprintf(stderr, "%s: interim len %zu book left %llu\n", name, o->len,
              (unsigned long long)bo_left(bk));
      return -1;
    }
    if (bo_phase(bk) == 6 && img != MURO_STEP_PH_READ) return fail(name);
    if (bo_phase(bk) == 8 && img != MURO_STEP_PH_WRITE) return fail(name);
    if (bo_phase(bk) != 6 && bo_phase(bk) != 8) return fail(name);
    return 0;
  }
  if (o->interim || o->cont) return fail(name);
  /* span 2 is the write, span 1 is status_now, span 0 is neither. */
  if (bo_span(bk) == 2 && bo_write(bk) != 1) return fail(name);
  if (bo_span(bk) == 1 && bo_status(bk) != 1) return fail(name);
  if (bo_span(bk) == 0 && (bo_write(bk) != 0 || bo_status(bk) != 0)) return fail(name);
  if (bo_span(bk) > 2) return fail(name);
  if (bo_write(bk) == 1) {
    if (!o->bytes || !o->len || !in_range(o->bytes, o->len, b->outb, OUT_CAP)) return fail(name);
    /* left 0 is the full span, parked at the start of the send buffer.
       A stored remainder is the host length, and may start mid-span. */
    if (bo_left(bk) == 0) {
      if (o->bytes != b->outb) {
        fprintf(stderr, "%s: first park not at out\n", name);
        return -1;
      }
    } else if (o->len != bo_left(bk)) {
      fprintf(stderr, "%s: host len %zu book left %llu\n", name, o->len,
              (unsigned long long)bo_left(bk));
      return -1;
    }
    if (o->bytes == b->outb) {
      int keep_ok;
      int finishing = fact == MURO_STEP_WROTE && owed != 0 && wrote_n >= owed;
      tok = bo_code(bk) == 0 ? 200 : bo_code(bk);
      keep_ok = conn_is(o->bytes, o->len, bo_keep(bk));
      /* A peer close during a handler write that is already parked
         (book phase 3 before this fact) sets book keep to 1 and does
         not rewrite the span. NOW and a short write keep that bit,
         so the parked bytes can still say keep-alive. A fresh keep-1
         reply, including a peer close of a finished request, is
         rendered Connection: close. A write that finishes the parked
         span and starts a new reply is fresh too. */
      if (!keep_ok && prior_book == 3 && bo_code(bk) == 0 && bo_keep(bk) == 1 &&
          conn_is(o->bytes, o->len, 0) && !finishing)
        keep_ok = 1;
      /* stepFact keeps the request keep. An open streamed body is
         then forced to Connection: close because the handler does
         not pull it. */
      if (!keep_ok && stream_forced_close(b, bk) && conn_is(o->bytes, o->len, 1)) keep_ok = 1;
      if (!status_token(o->bytes, o->len, tok) || !keep_ok) {
        fprintf(stderr, "%s: status/keep text code %llu keep %llu fact %d\n", name,
                (unsigned long long)bo_code(bk), (unsigned long long)bo_keep(bk), fact);
        return -1;
      }
    }
  } else if (o->bytes && o->len) {
    return fail(name);
  }
  if (bo_status(bk) == 1) {
    if (!o->status || !o->status_len ||
        !in_range(o->status, o->status_len, b->st.spare, sizeof b->st.spare))
      return fail(name);
    if (!status_token(o->status, o->status_len, bo_code(bk))) {
      fprintf(stderr, "%s: status token %llu\n", name, (unsigned long long)bo_code(bk));
      return -1;
    }
  }
  /* Book phases are not image phases. status_now (book phase 2, status
     1) closes while the image is still READ and marks the step
     PEND_SHUT. A later fact on that step is a quiet close (book phase
     2, status 0) that the host answers from PEND_SHUT, so its image is
     still READ. That is the only quiet close allowed in READ: it must
     carry PEND_SHUT, close, and leave no interest and no bytes. Any
     other quiet close (a tick, a peer close, Connection: close) must
     be CLOSED. */
  img = muro_http_step_phase(&b->st);
  if (bo_phase(bk) == 0 && img != MURO_STEP_PH_READ) return fail(name);
  if ((bo_phase(bk) == 1 || bo_phase(bk) == 3) && img != MURO_STEP_PH_WRITE) return fail(name);
  if (bo_phase(bk) == 4) {
    if (img != MURO_STEP_PH_IDLE) return fail(name);
    if (muro_http_step_in_len(&b->st) != 0) return fail(name);
  }
  if (bo_phase(bk) == 2 && bo_status(bk) == 0 && img != MURO_STEP_PH_CLOSED) {
    if (img != MURO_STEP_PH_READ || b->st.pend_kind != TEST_PEND_SHUT) return fail(name);
    if (!o->close_conn || o->read_more || o->write_more || o->status_now || o->bytes || o->len)
      return fail(name);
    quiet_read_shut += 1;
  }
  if (bo_phase(bk) == 2 && bo_status(bk) == 1 && img != MURO_STEP_PH_READ) return fail(name);
  if (bo_phase(bk) == 7 && img != MURO_STEP_PH_READ) return fail(name);
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
    int book_ph = (int)st_phase(st);
    sweep_steps += 1;
    if (step_both(&b, &st, fact, deliver, n, pass_owed, &o, &bk, "sweep") < 0) {
      box_free(&b);
      return -1;
    }
    if (bo_cover(bk) == 1) {
      fprintf(stderr, "sweep cover at step %d fact %d phase %d\n", i, fact, book_ph);
      dump_book("sweep", bk);
      box_free(&b);
      return -1;
    }
    if (match_covered(&o, bk, &b, fact, n, owed, book_ph, "sweep") < 0) {
      fprintf(stderr, "sweep step %d fact %d buflen %zu\n", i, fact, buf ? strlen(buf) : 0);
      box_free(&b);
      return -1;
    }
    /* hand() may raise keep when a streamed body is still open.
       The next fact has to see that keep, or the book would idle
       while the host closes. Any other disagreement is a bug. */
    if (b.bk.bphase != bo_phase(bk) || b.bk.bcode != bo_code(bk) || b.bk.bleft != bo_left(bk) ||
        b.bk.bkeep != bo_keep(bk) || b.bk.bhend != bo_hend(bk) || b.bk.bte != bo_te(bk) ||
            b.bk.bsawcl != bo_sawCL(bk) || b.bk.bcl != bo_cl(bk)) {
      if (!(stream_forced_close(&b, bk) && b.bk.bphase == 3 && b.bk.bcode == 0 && b.bk.bleft == 0 &&
            b.bk.bkeep == 1)) {
        fprintf(stderr, "sweep hand state phase %llu/%llu keep %llu/%llu hend %llu/%llu\n",
                (unsigned long long)bo_phase(bk), (unsigned long long)b.bk.bphase,
                (unsigned long long)bo_keep(bk), (unsigned long long)b.bk.bkeep,
                (unsigned long long)bo_hend(bk), (unsigned long long)b.bk.bhend);
        box_free(&b);
        return -1;
      }
      st = st_of(b.bk.bphase, b.bk.bcode, b.bk.bleft, b.bk.bkeep, b.bk.bhend, b.bk.bte, b.bk.bsawcl, b.bk.bcl);
    } else {
      st = http_step_take(bk);
    }
    owed = (o.write_more && o.len) ? (uint64_t)o.len : 0;
  }
  box_free(&b);
  return 0;
}

static int sweep(void) {
  static const int facts5[5] = {MURO_STEP_BYTES, MURO_STEP_PEER_CLOSED, MURO_STEP_WROTE,
                                MURO_STEP_WRITE_FAILED, MURO_STEP_NOW};
  /* Empty, a partial line, a GET, a malformed request, Connection:
     close, two pipelined GETs, a streamed body (Content-Length past
     16384, so at max 10 the shape is the owed 413), chunked headers,
     one finished chunk, a bad size, a size past max, and Expect:
     100-continue with the body still short. Depth 3 is 5^3 * 12.
     Then 256 sequences, depth 4, xorshift32 seed 0xC0FFEE.
     The streamed body is repeated at max 8388608, where it is a
     handler reply. Accept is a depth-3 grid on the empty buffer,
     the GET, and the malformed request, so a write and an idle
     step both reset. */
  const char *bufs[12];
  int a, c, d, e;
  int seq[4];
  uint32_t xs = 0xC0FFEEu;
  int n;
  bufs[0] = "";
  bufs[1] = LINE;
  bufs[2] = GET;
  bufs[3] = BAD;
  bufs[4] = CLOSE;
  bufs[5] = TWO;
  bufs[6] = STREAM;
  bufs[7] = CHUNK;
  bufs[8] = CHUNK_ONE;
  bufs[9] = CHUNK_BAD;
  bufs[10] = CHUNK_TOO;
  bufs[11] = CONT;
  for (e = 0; e < 12; e++) {
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
    if (run_seq(bufs[xs % 12], seq, 4, 10) < 0) return -1;
  }
  for (a = 0; a < 5; a++) {
    for (c = 0; c < 5; c++) {
      for (d = 0; d < 5; d++) {
        seq[0] = facts5[a];
        seq[1] = facts5[c];
        seq[2] = facts5[d];
        if (run_seq(STREAM, seq, 3, 8388608) < 0) return -1;
      }
    }
  }
  {
    static const int facts6[6] = {MURO_STEP_BYTES, MURO_STEP_PEER_CLOSED, MURO_STEP_WROTE,
                                  MURO_STEP_WRITE_FAILED, MURO_STEP_NOW, MURO_STEP_ACCEPTED};
    const char *acc[3];
    acc[0] = "";
    acc[1] = GET;
    acc[2] = BAD;
    for (e = 0; e < 3; e++) {
      for (a = 0; a < 6; a++) {
        for (c = 0; c < 6; c++) {
          for (d = 0; d < 6; d++) {
            seq[0] = facts6[a];
            seq[1] = facts6[c];
            seq[2] = facts6[d];
            if (run_seq(acc[e], seq, 3, 10) < 0) return -1;
          }
        }
      }
    }
  }
  return 0;
}

/* A finished request on a peer close is a handler span with keep 1.
   The completing write closes. */
static int fin_reply(const char *buf, uint64_t max_body, const char *name) {
  Box b;
  StepOut seen;
  http_step_Out first;
  http_step_St st;
  char done[80];
  if (decide(DEC_REPLY, MURO_STEP_PEER_CLOSED, buf, max_body, 0, NULL, 0, 0, 1, name, &b, &first,
             &seen) < 0)
    return -1;
  st = http_step_take(first);
  snprintf(done, sizeof done, "%s-done", name);
  if (full_write(&b, &st, seen, DEC_QUIET, 0, 0, done) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int idle_peer_get(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  size_t n;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "idle-fin-get") < 0 ||
      full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, 60000), "idle-fin-idle") < 0) {
    box_free(&b);
    return -1;
  }
  n = strlen(GET);
  memcpy(b.inb, GET, n);
  muro_http_step_set_in_len(&b.st, n);
  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  b.in.empty_before = 0;
  memset(&o, 0, sizeof o);
  bk = book_at(st, MURO_STEP_PEER_CLOSED, sl_of(GET), b.st.max_body, 0, 0, b.in.now_ms, b.in.deadline,
               b.st.idle_ms);
  if (muro_http_step(&b.st, &b.in, &o) < 0) {
    box_free(&b);
    return fail("idle-peer-get");
  }
  if (check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 1, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "idle-peer-get") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  seen = o;
  if (full_write(&b, &st, seen, DEC_QUIET, 0, 0, "idle-peer-get-done") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

/* Accept during a handler write, and during an owed 400, is a fresh
   read. The carried deadline is dropped. */
static int write_accept(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_Out first;
  http_step_St st;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "write-accept-get") < 0) {
    box_free(&b);
    return -1;
  }
  if (step_both(&b, &st, MURO_STEP_ACCEPTED, NULL, 0, 0, &o, &bk, "write-accept") < 0 ||
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, armed(1000, 0, 60000), 0, 0, 0, 0, 0,  "write-accept") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (decide(DEC_OWE, MURO_STEP_BYTES, BAD, 10, 400, "malformed", 0, 0, 1, "owe-accept-span", &b,
             &first, NULL) < 0)
    return -1;
  st = http_step_take(first);
  if (step_both(&b, &st, MURO_STEP_ACCEPTED, NULL, 0, 0, &o, &bk, "owe-accept") < 0 ||
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, armed(1000, 0, 60000), 0, 0, 0, 0, 0,  "owe-accept") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

/* The book replies at the request keep. The open body forces
   Connection: close, and the completing write closes. */
static int stream_open(void) {
  Box b;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  if (open_acc(&b, 8388608, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (step_both(&b, &st, MURO_STEP_BYTES, STREAM, 0, 0, &o, &bk, "stream-open") < 0) {
    box_free(&b);
    return -1;
  }
  if (book_is(bk, 3, 0, 0, 0, 0, 1, 2, 0, 0, 0, 0, slice_hend(STREAM), slice_te(STREAM), slice_sawCL(STREAM), slice_cl(STREAM), "stream-open") < 0 ||
      host_flags(&o, 0, 0, 0, 1, "stream-open") < 0 ||
      phase_is(&b, MURO_STEP_PH_WRITE, "stream-open") < 0 || !o.bytes || o.bytes != b.outb ||
      !status_token(o.bytes, o.len, 200) || !conn_is(o.bytes, o.len, 1) || o.wake_at != 0 ||
      b.bk.bkeep != 1 || b.bk.bphase != 3) {
    box_free(&b);
    return fail("stream-open");
  }
  scripted += 1;
  st = st_of(b.bk.bphase, b.bk.bcode, b.bk.bleft, b.bk.bkeep, b.bk.bhend, b.bk.bte, b.bk.bsawcl, b.bk.bcl);
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)o.len, (uint64_t)o.len, &o, &bk,
                "stream-open-done") < 0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, 0, 0, 0, 0,  "stream-open-done") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

/* Bytes during a handler write, or a wrote or a failure while idle.
   The book stays on that phase. The name is the evidence name. */
static int holds(int fact, int from_idle, const char *name) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  int book_ph;
  const char *deliver = NULL;
  char setup[96];
  uint8_t *saved = NULL;
  size_t saved_n = 0;
  uint64_t owed;
  snprintf(setup, sizeof setup, "%s-get", name);
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, setup) < 0) {
    box_free(&b);
    return -1;
  }
  if (from_idle) {
    snprintf(setup, sizeof setup, "%s-idle", name);
    if (full_write(&b, &st, seen, DEC_IDLE, 0, armed(1000, 0, 60000), setup) < 0) {
      box_free(&b);
      return -1;
    }
  } else if (seen.bytes && seen.len && seen.bytes == b.outb) {
    saved_n = seen.len;
    saved = malloc(saved_n);
    if (!saved) {
      box_free(&b);
      return fail(name);
    }
    memcpy(saved, b.outb, saved_n);
  }
  book_ph = (int)st_phase(st);
  owed = from_idle ? 0 : (uint64_t)seen.len;
  if (fact == MURO_STEP_BYTES) deliver = GET;
  if (step_both(&b, &st, fact, deliver, 0, owed, &o, &bk, name) < 0) {
    free(saved);
    box_free(&b);
    return -1;
  }
  if (bo_cover(bk) != 0) {
    fprintf(stderr, "%s: expected the step in the slice\n", name);
    dump_book(name, bk);
    free(saved);
    box_free(&b);
    return -1;
  }
  if (match_covered(&o, bk, &b, fact, 0, owed, book_ph, name) < 0) {
    free(saved);
    box_free(&b);
    return -1;
  }
  if (fact == MURO_STEP_BYTES &&
      (!saved || o.bytes != b.outb || o.len != saved_n || memcmp(b.outb, saved, saved_n) != 0 ||
       !conn_is(o.bytes, o.len, 0))) {
    fprintf(stderr, "%s: parked span changed\n", name);
    free(saved);
    box_free(&b);
    return -1;
  }
  free(saved);
  scripted += 1;
  box_free(&b);
  return 0;
}

typedef struct PullBuf {
  const uint8_t *p;
  size_t n;
  size_t off;
} PullBuf;

static int pull_take(uint8_t *dst, size_t cap, void *user) {
  PullBuf *b = user;
  size_t left;
  size_t k;
  if (!b || b->off >= b->n) return 0;
  left = b->n - b->off;
  k = left < cap ? left : cap;
  if (k && dst) memcpy(dst, b->p + b->off, k);
  b->off += k;
  return (int)k;
}

static int on_close_after(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  on_req(req, resp, user);
  resp->close_after = 1;
  return 0;
}

static int on_bad(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  (void)req;
  (void)user;
  resp->status = 400;
  resp->reason = "Bad";
  resp->reason_len = 3;
  resp->body = (const uint8_t *)"no";
  resp->body_len = 2;
  return 0;
}

static int on_handler_500(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  (void)req;
  (void)user;
  resp->owe_status = 1;
  return -1;
}

static int on_refuse(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  (void)req;
  (void)resp;
  (void)user;
  return -1;
}

static int on_over(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  int code = user ? *(const int *)user : 400;
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
  resp->body = (const uint8_t *)"ok";
  resp->body_len = 2;
  if (req && req->body_in && req->body_in->in) req->body_in->in->err = code;
  return 0;
}

static int on_refuse_over(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  (void)resp;
  (void)user;
  if (req && req->body_in && req->body_in->in) req->body_in->in->err = 400;
  return -1;
}

static int on_too_big(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  (void)req;
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
  resp->body = user;
  resp->body_len = 70000;
  return 0;
}

static int on_too_big_owe(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  on_too_big(req, resp, user);
  resp->owe_status = 1;
  return 0;
}

static int on_pull_len(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  (void)req;
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
  resp->pull = pull_take;
  resp->pull_user = user;
  resp->has_length = 1;
  resp->content_length = 2;
  return 0;
}

static int on_pull_open(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  (void)req;
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
  resp->pull = pull_take;
  resp->pull_user = user;
  return 0;
}

static void use_handler(Box *b, MuroHttpHandler fn, void *host_user, void *book_user) {
  b->st.handler = fn;
  b->bk.handler = fn;
  b->st.handler_user = host_user;
  b->bk.handler_user = book_user;
}

static int put_req(Box *b, const char *buf, StepOut *o) {
  size_t n = strlen(buf);
  if (n > IN_CAP) return -1;
  memcpy(b->inb, buf, n);
  muro_http_step_set_in_len(&b->st, n);
  set_fact(b, MURO_STEP_BYTES, 0);
  b->in.empty_before = 1;
  memset(o, 0, sizeof *o);
  return muro_http_step(&b->st, &b->in, o);
}

static int wrote_span(Box *b, const StepOut *prev, StepOut *o) {
  set_fact(b, MURO_STEP_WROTE, prev->len);
  memset(o, 0, sizeof *o);
  return muro_http_step(&b->st, &b->in, o);
}

/* Handler results the book used to leave to the adapter. Each call
   goes through host_and_book, so the adapter and muro_http_step
   agree on the span, the phase, and the note. */
static int handler_paths(void) {
  Box b;
  StepOut o;
  StepOut nxt;
  PullBuf host_pull;
  PullBuf book_pull;
  uint8_t *big;
  const uint8_t *msg = NULL;
  size_t msg_n = 0;
  int log_500 = 0;
  static const uint8_t body[] = {'o', 'k'};

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  use_handler(&b, on_close_after, NULL, NULL);
  if (put_req(&b, GET, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-close") < 0 ||
      !o.bytes || !conn_is(o.bytes, o.len, 1) || !status_token(o.bytes, o.len, 200)) {
    box_free(&b);
    return fail("hand-close");
  }
  scripted += 1;
  if (wrote_span(&b, &o, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_CLOSED, "hand-close-done") < 0 ||
      !nxt.close_conn) {
    box_free(&b);
    return fail("hand-close-done");
  }
  scripted += 1;
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  use_handler(&b, on_bad, NULL, NULL);
  if (put_req(&b, GET, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-400") < 0 || !o.bytes ||
      !conn_is(o.bytes, o.len, 1) || !status_token(o.bytes, o.len, 400)) {
    box_free(&b);
    return fail("hand-400");
  }
  scripted += 1;
  if (wrote_span(&b, &o, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_CLOSED, "hand-400-done") < 0 ||
      !nxt.close_conn) {
    box_free(&b);
    return fail("hand-400-done");
  }
  scripted += 1;
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  use_handler(&b, on_handler_500, NULL, NULL);
  if (put_req(&b, GET, &o) < 0 || phase_is(&b, MURO_STEP_PH_DECIDE, "hand-500") < 0 ||
      o.note != MURO_STEP_NOTE_HANDLER_500 || !o.close_conn || (o.bytes && o.len) ||
      muro_book_phase(&b.bk) != MURO_STEP_PH_DECIDE) {
    box_free(&b);
    return fail("hand-500");
  }
  scripted += 1;
  muro_http_step_close_status(&b.st, (const uint8_t *)DATE, 29, &msg, &msg_n, &log_500);
  if (!msg || !status_token(msg, msg_n, 500)) {
    box_free(&b);
    return fail("hand-500-status");
  }
  scripted += 1;
  if (!b.bk.owe || !b.bk.rendered || !status_token(b.bout, b.bk.out_len, 500)) {
    box_free(&b);
    return fail("hand-500-book");
  }
  scripted += 1;
  set_fact(&b, MURO_STEP_NOW, 0);
  memset(&nxt, 0, sizeof nxt);
  if (muro_http_step(&b.st, &b.in, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_DECIDE, "hand-500-later") < 0 ||
      !nxt.close_conn || nxt.note || (nxt.bytes && nxt.len) ||
      muro_book_phase(&b.bk) != MURO_STEP_PH_DECIDE) {
    box_free(&b);
    return fail("hand-500-later");
  }
  scripted += 1;
  box_free(&b);

  /* hand-refuse: a refused handler without owe_status shuts in
     decide with no note and no 500. A later fact stays shut. */
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  use_handler(&b, on_refuse, NULL, NULL);
  if (put_req(&b, GET, &o) < 0 || phase_is(&b, MURO_STEP_PH_DECIDE, "hand-refuse") < 0 || o.note ||
      !o.close_conn || (o.bytes && o.len) || muro_book_phase(&b.bk) != MURO_STEP_PH_DECIDE ||
      b.bk.owe) {
    box_free(&b);
    return fail("hand-refuse");
  }
  scripted += 1;
  set_fact(&b, MURO_STEP_NOW, 0);
  memset(&nxt, 0, sizeof nxt);
  if (muro_http_step(&b.st, &b.in, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_DECIDE, "hand-refuse-later") < 0 ||
      !nxt.close_conn || nxt.note || (nxt.bytes && nxt.len) ||
      muro_book_phase(&b.bk) != MURO_STEP_PH_DECIDE) {
    box_free(&b);
    return fail("hand-refuse-later");
  }
  scripted += 1;
  box_free(&b);

  host_pull.p = body;
  host_pull.n = 2;
  host_pull.off = 0;
  book_pull = host_pull;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  use_handler(&b, on_pull_len, &host_pull, &book_pull);
  if (put_req(&b, GET, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-pull-len") < 0 || !o.bytes ||
      !has_lit(o.bytes, o.len, "Content-Length: 2\r\n") || !conn_is(o.bytes, o.len, 0) ||
      host_pull.off || book_pull.off) {
    box_free(&b);
    return fail("hand-pull-len");
  }
  scripted += 1;
  if (wrote_span(&b, &o, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-pull-body") < 0 ||
      !nxt.bytes || nxt.len != 2 || memcmp(nxt.bytes, "ok", 2) != 0) {
    box_free(&b);
    return fail("hand-pull-body");
  }
  scripted += 1;
  if (wrote_span(&b, &nxt, &o) < 0 || phase_is(&b, MURO_STEP_PH_IDLE, "hand-pull-idle") < 0 ||
      o.close_conn || (o.bytes && o.len)) {
    box_free(&b);
    return fail("hand-pull-idle");
  }
  scripted += 1;
  box_free(&b);

  host_pull.off = 0;
  book_pull.off = 0;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  use_handler(&b, on_pull_open, &host_pull, &book_pull);
  if (put_req(&b, GET, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-pull-chunk") < 0 ||
      !o.bytes || !has_lit(o.bytes, o.len, "Transfer-Encoding: chunked\r\n") ||
      !conn_is(o.bytes, o.len, 0)) {
    box_free(&b);
    return fail("hand-pull-chunk");
  }
  scripted += 1;
  if (wrote_span(&b, &o, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-pull-chunk-body") < 0 ||
      !nxt.bytes || !has_lit(nxt.bytes, nxt.len, "ok")) {
    box_free(&b);
    return fail("hand-pull-chunk-body");
  }
  scripted += 1;
  if (wrote_span(&b, &nxt, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-pull-last") < 0 ||
      !o.bytes || !has_lit(o.bytes, o.len, "0\r\n\r\n")) {
    box_free(&b);
    return fail("hand-pull-last");
  }
  scripted += 1;
  if (wrote_span(&b, &o, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_IDLE, "hand-pull-chunk-idle") < 0 ||
      nxt.close_conn) {
    box_free(&b);
    return fail("hand-pull-chunk-idle");
  }
  scripted += 1;
  box_free(&b);

  host_pull.off = 0;
  book_pull.off = 0;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  use_handler(&b, on_pull_open, &host_pull, &book_pull);
  if (put_req(&b, HEAD, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-pull-head") < 0 ||
      !o.bytes || !conn_is(o.bytes, o.len, 1) || host_pull.off || book_pull.off) {
    box_free(&b);
    return fail("hand-pull-head");
  }
  scripted += 1;
  if (wrote_span(&b, &o, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_CLOSED, "hand-pull-head-done") < 0 ||
      !nxt.close_conn || host_pull.off || book_pull.off) {
    box_free(&b);
    return fail("hand-pull-head-done");
  }
  scripted += 1;
  box_free(&b);

  host_pull.off = 0;
  book_pull.off = 0;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  use_handler(&b, on_pull_open, &host_pull, &book_pull);
  if (put_req(&b, HTTP10, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-pull-delim") < 0 ||
      !o.bytes || !conn_is(o.bytes, o.len, 1)) {
    box_free(&b);
    return fail("hand-pull-delim");
  }
  scripted += 1;
  if (wrote_span(&b, &o, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-pull-delim-body") < 0 ||
      !nxt.bytes || nxt.len != 2 || memcmp(nxt.bytes, "ok", 2) != 0) {
    box_free(&b);
    return fail("hand-pull-delim-body");
  }
  scripted += 1;
  if (wrote_span(&b, &nxt, &o) < 0 || phase_is(&b, MURO_STEP_PH_CLOSED, "hand-pull-delim-done") < 0 ||
      !o.close_conn) {
    box_free(&b);
    return fail("hand-pull-delim-done");
  }
  scripted += 1;
  box_free(&b);

  big = malloc(70000);
  if (!big) return fail("hand-big alloc");
  memset(big, 'x', 70000);
  if (open_acc(&b, 8388608, 1000, 0) < 0) {
    free(big);
    return -1;
  }
  use_handler(&b, on_too_big, big, big);
  if (put_req(&b, GET, &o) < 0 || phase_is(&b, MURO_STEP_PH_DECIDE, "hand-toobig") < 0 ||
      o.note != MURO_STEP_NOTE_TOO_BIG || !o.close_conn || (o.bytes && o.len)) {
    box_free(&b);
    free(big);
    return fail("hand-toobig");
  }
  scripted += 1;
  box_free(&b);

  if (open_acc(&b, 8388608, 1000, 0) < 0) {
    free(big);
    return -1;
  }
  use_handler(&b, on_too_big_owe, big, big);
  if (put_req(&b, GET, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "hand-toobig-owe") < 0 ||
      o.note != MURO_STEP_NOTE_TOO_BIG_COMMIT || !o.bytes || !status_token(o.bytes, o.len, 500) ||
      !conn_is(o.bytes, o.len, 1)) {
    box_free(&b);
    free(big);
    return fail("hand-toobig-owe");
  }
  scripted += 1;
  box_free(&b);
  free(big);

  /* The body reader sets 400 or 413 after a successful handler.
     overStatus owes that span. A refusal is checked first, so the
     same 400 does not replace the shut. */
  {
    int code = 400;
    if (open_acc(&b, 10, 1000, 0) < 0) return -1;
    use_handler(&b, on_over, &code, &code);
    if (put_req(&b, POST_BODY, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "over-400") < 0 ||
        b.bk.bphase != 1 || b.bk.bcode != 400 || !o.bytes || !status_token(o.bytes, o.len, 400) ||
        !has_lit(o.bytes, o.len, "malformed") || !conn_is(o.bytes, o.len, 1) || o.note) {
      box_free(&b);
      return fail("over-400");
    }
    scripted += 1;
    if (wrote_span(&b, &o, &nxt) < 0 || phase_is(&b, MURO_STEP_PH_CLOSED, "over-400-done") < 0 ||
        !nxt.close_conn) {
      box_free(&b);
      return fail("over-400-done");
    }
    scripted += 1;
    box_free(&b);

    code = 413;
    if (open_acc(&b, 10, 1000, 0) < 0) return -1;
    use_handler(&b, on_over, &code, &code);
    if (put_req(&b, POST_BODY, &o) < 0 || phase_is(&b, MURO_STEP_PH_WRITE, "over-413") < 0 ||
        b.bk.bphase != 1 || b.bk.bcode != 413 || !o.bytes || !status_token(o.bytes, o.len, 413) ||
        !has_lit(o.bytes, o.len, "too large") || !conn_is(o.bytes, o.len, 1) || o.note) {
      box_free(&b);
      return fail("over-413");
    }
    scripted += 1;
    box_free(&b);

    if (open_acc(&b, 10, 1000, 0) < 0) return -1;
    use_handler(&b, on_refuse_over, NULL, NULL);
    if (put_req(&b, POST_BODY, &o) < 0 || phase_is(&b, MURO_STEP_PH_DECIDE, "over-refuse") < 0 ||
        o.note || !o.close_conn || (o.bytes && o.len) || b.bk.bphase != 5) {
      box_free(&b);
      return fail("over-refuse");
    }
    scripted += 1;
    box_free(&b);
  }
  return 0;
}

/* cover 1 shuts the adapter. A later fact stays shut: close, image
   closed, no interests, no span, and no second cover note. */
static int cover_stays_shut(void) {
  BookStep b;
  struct HostIn hin;
  StepIn in;
  StepOut out;
  uint8_t ibuf[256];
  uint8_t obuf[512];
  int rd = 1;
  int wr = 1;
  size_t n;
  /* An unknown fact is outside the slice. The book fails closed.
     Chunked bodies and 100-continue are in the slice. */
  n = 0;
  memset(&b, 0, sizeof b);
  host_in_init(&hin, -1, 10);
  muro_book_bind(&b, ibuf, sizeof ibuf, obuf, sizeof obuf, &hin, &rd, &wr, on_req, NULL, 10, 60000);
  muro_book_set_in_len(&b, n);
  memset(&in, 0, sizeof in);
  in.kind = 0;
  in.now_ms = 1000;
  in.date = (const uint8_t *)DATE;
  in.date_n = 29;
  in.empty_before = 1;
  memset(&out, 0, sizeof out);
  if (muro_book_step(&b, &in, &out) != 0 || !adapter_shut(&out, &b, 0, MURO_BOOK_NOTE_COVER)) {
    host_in_close(&hin);
    return fail("cover-shut");
  }
  scripted += 1;
  in.kind = MURO_STEP_NOW;
  memset(&out, 0, sizeof out);
  rd = 1;
  wr = 1;
  if (muro_book_step(&b, &in, &out) != 0 || !adapter_shut(&out, &b, 0, 0) || b.last_cover) {
    host_in_close(&hin);
    return fail("cover-later");
  }
  scripted += 1;
  in.kind = MURO_STEP_BYTES;
  memset(&out, 0, sizeof out);
  if (muro_book_step(&b, &in, &out) != 0 || !adapter_shut(&out, &b, 0, 0)) {
    host_in_close(&hin);
    return fail("cover-bytes");
  }
  scripted += 1;
  host_in_close(&hin);
  muro_book_shutdown(&b);
  return 0;
}

/* An invariant failure shuts with MURO_BOOK_NOTE_STUCK, not the
   cover note. A covered GET with no handler cannot render, so the
   adapter closes with note 7. A later fact stays shut with note 0. */
static int stuck_note(void) {
  BookStep b;
  struct HostIn hin;
  StepIn in;
  StepOut out;
  uint8_t ibuf[256];
  uint8_t obuf[512];
  int rd = 1;
  int wr = 1;
  size_t n;
  n = strlen(GET);
  if (n >= sizeof ibuf) return fail("stuck-note");
  memcpy(ibuf, GET, n);
  memset(&b, 0, sizeof b);
  host_in_init(&hin, -1, 10);
  muro_book_bind(&b, ibuf, sizeof ibuf, obuf, sizeof obuf, &hin, &rd, &wr, NULL, NULL, 10, 60000);
  muro_book_set_in_len(&b, n);
  memset(&in, 0, sizeof in);
  in.kind = MURO_STEP_BYTES;
  in.now_ms = 1000;
  in.date = (const uint8_t *)DATE;
  in.date_n = 29;
  in.empty_before = 1;
  memset(&out, 0, sizeof out);
  if (muro_book_step(&b, &in, &out) != 0 || !adapter_shut(&out, &b, 0, MURO_BOOK_NOTE_STUCK) ||
      b.last_cover) {
    host_in_close(&hin);
    return fail("stuck-note");
  }
  scripted += 1;
  in.kind = MURO_STEP_NOW;
  memset(&out, 0, sizeof out);
  rd = 1;
  wr = 1;
  if (muro_book_step(&b, &in, &out) != 0 || !adapter_shut(&out, &b, 0, 0)) {
    host_in_close(&hin);
    return fail("stuck-later");
  }
  scripted += 1;
  host_in_close(&hin);
  muro_book_shutdown(&b);
  return 0;
}

/* streamTake is 1 only for a Content-Length past 16384. A normal GET
   captures nothing. The peer-close streamed body does, on both the
   host and the adapter, before the handler binds. */
static int capture_stream(void) {
  Box b;
  StepOut o;
  size_t n;
  if (open_acc(&b, 8388608, 1000, 0) < 0) return -1;
  n = strlen(GET);
  memcpy(b.inb, GET, n);
  muro_http_step_set_in_len(&b.st, n);
  set_fact(&b, MURO_STEP_BYTES, 0);
  b.in.empty_before = 1;
  memset(&o, 0, sizeof o);
  if (muro_http_step(&b.st, &b.in, &o) < 0 || b.hin.captured || b.bhin.captured) {
    box_free(&b);
    return fail("capture-get");
  }
  scripted += 1;
  box_free(&b);
  if (open_acc(&b, 8388608, 1000, 0) < 0) return -1;
  n = strlen(STREAM);
  memcpy(b.inb, STREAM, n);
  muro_http_step_set_in_len(&b.st, n);
  set_fact(&b, MURO_STEP_PEER_CLOSED, 0);
  b.in.empty_before = 1;
  memset(&o, 0, sizeof o);
  if (muro_http_step(&b.st, &b.in, &o) < 0 || !b.hin.captured || !b.bhin.captured) {
    fprintf(stderr, "capture-stream host %d book %d\n", b.hin.captured, b.bhin.captured);
    box_free(&b);
    return fail("capture-stream");
  }
  if (!o.bytes || !conn_is(o.bytes, o.len, 1)) {
    box_free(&b);
    return fail("capture-stream close");
  }
  scripted += 1;
  box_free(&b);
  return 0;
}

/* The last admit of a keep-alive GET is the reply. The last admit
   of the completing write is done. The session stays open. */
static int admit_agrees(void) {
  Box b;
  StepOut seen;
  http_step_St st;
  uint64_t wake;
  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  if (!b.st.proto || !b.bk.proto || !proto_live(b.st.proto) || proto_done(b.st.proto) ||
      proto_move_count(b.st.proto) != 0 || !protos_agree(b.st.proto, b.bk.proto)) {
    box_free(&b);
    return fail("admit-accept");
  }
  scripted += 1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, GET, 0, &seen, "admit-get") < 0) {
    box_free(&b);
    return -1;
  }
  if (!proto_live(b.st.proto) || proto_done(b.st.proto) || !protos_agree(b.st.proto, b.bk.proto)) {
    box_free(&b);
    return fail("admit-get");
  }
  scripted += 1;
  wake = armed(1000, 0, (uint64_t)b.st.idle_ms);
  if (full_write(&b, &st, seen, DEC_IDLE, 0, wake, "admit-idle") < 0) {
    box_free(&b);
    return -1;
  }
  if (!proto_live(b.st.proto) || proto_done(b.st.proto) || !protos_agree(b.st.proto, b.bk.proto)) {
    box_free(&b);
    return fail("admit-idle");
  }
  scripted += 1;
  box_free(&b);
  return 0;
}

/* Chunked bodies. The book decides; the adapter renders the same
   span the host does. A pipelined GET after a last-chunk is the
   next reply, then idle. Connection: close finishes quiet. */
static int chunk_cases(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  uint64_t wake = armed(1000, 0, 60000);
  if (decide(DEC_REPLY, MURO_STEP_BYTES, CHUNK_ONE, 10, 0, NULL, 0, 0, 0, "chunk-one", NULL, NULL,
             NULL) < 0)
    return -1;
  if (decide(DEC_REPLY, MURO_STEP_BYTES, CHUNK_EXT, 10, 0, NULL, 0, 0, 0, "chunk-ext", NULL, NULL,
             NULL) < 0)
    return -1;
  if (decide(DEC_REPLY, MURO_STEP_BYTES, CHUNK_TRAILER, 10, 0, NULL, 0, 0, 0, "chunk-trailer", NULL,
             NULL, NULL) < 0)
    return -1;
  if (decide(DEC_REPLY, MURO_STEP_BYTES, CHUNK_MULTI, 10, 0, NULL, 0, 0, 0, "chunk-multi", NULL,
             NULL, NULL) < 0)
    return -1;
  if (decide(DEC_REPLY, MURO_STEP_BYTES, CHUNK_CLOSE, 10, 0, NULL, 0, 0, 1, "chunk-close", NULL,
             NULL, NULL) < 0)
    return -1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, CHUNK_BAD, 10, 400, "malformed", 0, 0, 1, "chunk-bad", NULL,
             NULL, NULL) < 0)
    return -1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, CHUNK_SPACE, 10, 400, "malformed", 0, 0, 1, "chunk-space",
             NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, CHUNK_EXT_LF, 10, 400, "malformed", 0, 0, 1, "chunk-ext-lf",
             NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, CHUNK_OVERFLOW, 10, 400, "malformed", 0, 0, 1,
             "chunk-overflow", NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, CHUNK_BARE, 10, 400, "malformed", 0, 0, 1, "chunk-bare",
             NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, CHUNK_TOO, 8388608, 413, "too large", 0, 0, 0, "chunk-too",
             NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_OWE, MURO_STEP_PEER_CLOSED, CHUNK_PREM, 8388608, 400, "malformed", 0, 0, 1,
             "chunk-prem", NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, CHUNK_ONE, 1, 413, "too large", 0, 0, 1, "chunk-over-done",
             NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, CHUNK_EXPECT, 8388608, 417, "expectation", 0, 0, 0,
             "chunk-expect", NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, CHUNK_EXPECT_DONE, 8388608, 417, "expectation", 0, 0, 1,
             "chunk-expect-done", NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, CHUNK_TRACE, 8388608, 501, "unimplemented", 0, 0, 0,
             "chunk-trace", NULL, NULL, NULL) < 0)
    return -1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, CHUNK_TRACE_DONE, 8388608, 501, "unimplemented", 0, 0, 1,
             "chunk-trace-done", NULL, NULL, NULL) < 0)
    return -1;

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, CHUNK_ONE, 0, &seen, "chunk-one-reply") < 0 ||
      full_write(&b, &st, seen, DEC_IDLE, 0, wake, "chunk-one-idle") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, CHUNK_CLOSE, 1, &seen, "chunk-close-reply") < 0 ||
      full_write(&b, &st, seen, DEC_QUIET, 0, 0, "chunk-close-done") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (reply_bytes(&b, &st, CHUNK_PIPE, 0, &seen, "chunk-pipe") < 0) {
    box_free(&b);
    return -1;
  }
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "chunk-pipe-next") < 0 ||
      check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, slice_hend(GET), slice_te(GET), slice_sawCL(GET), slice_cl(GET), "chunk-pipe-next") < 0) {
    box_free(&b);
    return -1;
  }
  if (muro_http_step_in_len(&b.st) != strlen(GET)) {
    fprintf(stderr, "chunk-pipe-next in_len %zu\n", muro_http_step_in_len(&b.st));
    box_free(&b);
    return -1;
  }
  seen = o;
  st = http_step_take(bk);
  if (full_write(&b, &st, seen, DEC_IDLE, 0, wake, "chunk-pipe-idle") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int check_interim(const StepOut *o, http_step_Out bk, const Box *b, uint64_t phase,
                         uint64_t left, int interim, uint64_t wake, int image, uint64_t hend,
                         uint64_t te, uint64_t sawCL, uint64_t cl, const char *name) {
  if (book_is(bk, phase, 100, 0, 0, 0, 1, 6, 0, left, wake, 0, hend, te, sawCL, cl, name) < 0) return -1;
  if (o->close_conn || o->status_now || o->read_more || !o->write_more || !o->cont) return fail(name);
  if ((int)o->interim != interim) {
    fprintf(stderr, "%s: interim %d want %d len %zu\n", name, o->interim, interim, o->len);
    return -1;
  }
  if (!o->bytes || !o->len || !in_range(o->bytes, o->len, b->st.cont, sizeof b->st.cont))
    return fail(name);
  /* A short write points into the middle of the literal. The status
     token is only at the first byte. */
  if (o->bytes == b->st.cont && !status_token(o->bytes, o->len, 100)) return fail(name);
  if (left == 0) {
    if (o->bytes != b->st.cont) return fail(name);
  } else if (o->len != left) {
    fprintf(stderr, "%s: interim len %zu left %llu\n", name, o->len, (unsigned long long)left);
    return -1;
  }
  if (wake_is(o, wake, name) < 0) return -1;
  if (phase_is(b, image, name) < 0) return -1;
  if (count_shape) scripted += 1;
  return 0;
}

static int check_sent(const StepOut *o, http_step_Out bk, const Box *b, uint64_t wake, uint64_t hend,
                      uint64_t te, uint64_t sawCL, uint64_t cl, const char *name) {
  if (book_is(bk, 7, 0, 0, 0, 1, 0, 0, 0, 0, wake, 0, hend, te, sawCL, cl, name) < 0) return -1;
  if (host_flags(o, 0, 0, 1, 0, name) < 0) return -1;
  if ((o->bytes && o->len) || (o->status && o->status_len)) return fail(name);
  if (wake_is(o, wake, name) < 0) return -1;
  if (phase_is(b, MURO_STEP_PH_READ, name) < 0) return -1;
  if (count_shape) scripted += 1;
  return 0;
}

/* empty_before stays as given. step_both forces 1 and would drop a
   pending interim. The book state is the one from the previous step. */
static int feed_kept(Box *b, http_step_St *st, int fact, const char *buf, int empty, StepOut *o,
                     http_step_Out *bk, const char *name) {
  size_t n = buf ? strlen(buf) : 0;
  if (n > IN_CAP) return fail(name);
  if (n) memcpy(b->inb, buf, n);
  muro_http_step_set_in_len(&b->st, n);
  set_fact(b, fact, 0);
  b->in.empty_before = empty;
  *bk = book_at(*st, fact, sl_of(buf), b->st.max_body, 0, 0, b->in.now_ms, b->in.deadline,
                b->st.idle_ms);
  memset(o, 0, sizeof *o);
  if (muro_http_step(&b->st, &b->in, o) < 0) {
    fprintf(stderr, "%s: host step failed\n", name);
    return -1;
  }
  return 0;
}

static int cont_open(const char *buf, uint64_t max_body, const char *name) {
  Box b;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  uint64_t wake = armed(1000, 0, 60000);
  if (open_acc(&b, max_body, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (step_both(&b, &st, MURO_STEP_BYTES, buf, 0, 0, &o, &bk, name) < 0 ||
      check_interim(&o, bk, &b, 6, 0, 1, wake, MURO_STEP_PH_READ, slice_hend(buf), slice_te(buf), slice_sawCL(buf), slice_cl(buf), name) < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
  return 0;
}

static int park_cont_case(Box *b, http_step_St *st, const char *buf, StepOut *seen, const char *name) {
  StepOut o;
  http_step_Out bk;
  uint64_t wake = armed(1000, 0, 60000);
  if (step_both(b, st, MURO_STEP_BYTES, buf, 0, 0, &o, &bk, name) < 0 ||
      check_interim(&o, bk, b, 6, 0, 1, wake, MURO_STEP_PH_READ, slice_hend(buf), slice_te(buf), slice_sawCL(buf), slice_cl(buf), name) < 0)
    return -1;
  *seen = o;
  *st = http_step_take(bk);
  return 0;
}

/* 100 Continue. The interim is owed only while the body is short.
   A body already buffered, HTTP/1.0, HEAD, and a 413 do not owe it.
   The final response follows the interim. A tick drops it. */
static int cont_cases(void) {
  Box b;
  StepOut seen;
  StepOut o;
  http_step_Out bk;
  http_step_St st;
  uint64_t wake = armed(1000, 0, 60000);
  if (decide(DEC_REPLY, MURO_STEP_BYTES, CONT_FULL, 10, 0, NULL, 0, 0, 0, "cont-have", NULL, NULL,
             NULL) < 0)
    return -1;
  if (decide(DEC_MORE, MURO_STEP_BYTES, CONT_10, 10, 0, NULL, 0, wake, 0, "cont-10", NULL, NULL,
             NULL) < 0)
    return -1;
  if (decide(DEC_MORE, MURO_STEP_BYTES, CONT_HEAD, 10, 0, NULL, 0, wake, 0, "cont-head", NULL, NULL,
             NULL) < 0)
    return -1;
  if (decide(DEC_STATUS, MURO_STEP_BYTES, CONT_BIG, 10, 413, "too large", 0, 0, 0, "cont-413", NULL,
             NULL, NULL) < 0)
    return -1;
  if (decide(DEC_REPLY, MURO_STEP_BYTES, CONT_CHUNK_DONE, 10, 0, NULL, 0, 0, 0, "cont-chunk-done",
             NULL, NULL, NULL) < 0)
    return -1;
  if (cont_open(CONT_CHUNK, 10, "cont-chunk") < 0) return -1;

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (park_cont_case(&b, &st, CONT, &seen, "cont-owed") < 0) {
    box_free(&b);
    return -1;
  }
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "cont-wrote") < 0 ||
      check_sent(&o, bk, &b, wake, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-wrote") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  if (feed_kept(&b, &st, MURO_STEP_BYTES, CONT, 0, &o, &bk, "cont-hold") < 0 ||
      check_sent(&o, bk, &b, wake, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-hold") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (park_cont_case(&b, &st, CONT, &seen, "cont-again-owed") < 0 ||
      step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "cont-again-sent") < 0 ||
      check_sent(&o, bk, &b, wake, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-again-sent") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_BYTES, CONT, 0, 0, &o, &bk, "cont-again") < 0 ||
      check_interim(&o, bk, &b, 6, 0, 1, wake, MURO_STEP_PH_READ, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-again") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (park_cont_case(&b, &st, CONT, &seen, "cont-body-owed") < 0 ||
      step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "cont-body-sent") < 0 ||
      check_sent(&o, bk, &b, wake, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-body-sent") < 0) {
    box_free(&b);
    return -1;
  }
  st = http_step_take(bk);
  if (feed_kept(&b, &st, MURO_STEP_BYTES, CONT_FULL, 0, &o, &bk, "cont-body") < 0 ||
      check_shape(DEC_REPLY, &o, bk, &b, 0, NULL, 0, 0, 0, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-body") < 0) {
    box_free(&b);
    return -1;
  }
  seen = o;
  st = http_step_take(bk);
  if (full_write(&b, &st, seen, DEC_IDLE, 0, wake, "cont-body-idle") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (park_cont_case(&b, &st, CONT, &seen, "cont-short-owed") < 0 ||
      step_both(&b, &st, MURO_STEP_WROTE, NULL, 1, (uint64_t)seen.len, &o, &bk, "cont-short") < 0 ||
      check_interim(&o, bk, &b, 6, (uint64_t)seen.len - 1, 0, wake, MURO_STEP_PH_READ,
                    slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-short") < 0) {
    box_free(&b);
    return -1;
  }
  seen = o;
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "cont-short-done") < 0 ||
      check_sent(&o, bk, &b, wake, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-short-done") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (park_cont_case(&b, &st, CONT, &seen, "cont-fail-owed") < 0 ||
      step_both(&b, &st, MURO_STEP_WRITE_FAILED, NULL, 0, (uint64_t)seen.len, &o, &bk,
                "cont-fail") < 0 ||
      check_interim(&o, bk, &b, 6, 0, 1, wake, MURO_STEP_PH_READ, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-fail") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (park_cont_case(&b, &st, CONT, &seen, "cont-now-owed") < 0 ||
      step_both(&b, &st, MURO_STEP_NOW, NULL, 0, 0, &o, &bk, "cont-now") < 0 ||
      check_shape(DEC_QUIET, &o, bk, &b, 0, NULL, 0, 0, 0, 0, 0, 0, 0,  "cont-now") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (park_cont_case(&b, &st, CONT, &seen, "cont-clear-owed") < 0 ||
      step_both(&b, &st, MURO_STEP_PEER_CLOSED, CONT, 0, 0, &o, &bk, "cont-peer-clear") < 0 ||
      check_shape(DEC_OWE, &o, bk, &b, 400, "malformed", 0, 0, 1, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-peer-clear") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (park_cont_case(&b, &st, CONT, &seen, "cont-peer-owed") < 0 ||
      feed_kept(&b, &st, MURO_STEP_PEER_CLOSED, CONT, 0, &o, &bk, "cont-peer") < 0 ||
      check_interim(&o, bk, &b, 8, 0, 1, 0, MURO_STEP_PH_WRITE, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-peer") < 0) {
    box_free(&b);
    return -1;
  }
  seen = o;
  st = http_step_take(bk);
  if (step_both(&b, &st, MURO_STEP_WROTE, NULL, (uint64_t)seen.len, (uint64_t)seen.len, &o, &bk,
                "cont-peer-done") < 0 ||
      check_shape(DEC_OWE, &o, bk, &b, 400, "malformed", 0, 0, 1, slice_hend(CONT), slice_te(CONT), slice_sawCL(CONT), slice_cl(CONT), "cont-peer-done") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);

  if (open_acc(&b, 10, 1000, 0) < 0) return -1;
  st = http_step_readSt();
  if (park_cont_case(&b, &st, CONT, &seen, "cont-accept-owed") < 0 ||
      step_both(&b, &st, MURO_STEP_ACCEPTED, NULL, 0, 0, &o, &bk, "cont-accept") < 0 ||
      check_shape(DEC_MORE, &o, bk, &b, 0, NULL, 0, wake, 0, 0, 0, 0, 0,  "cont-accept") < 0) {
    box_free(&b);
    return -1;
  }
  box_free(&b);
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
  if (decide(DEC_MORE, MURO_STEP_BYTES, CHUNK, 10, 0, NULL, 0, wake, 0, "chunk-more", NULL, NULL,
             NULL) < 0)
    return 1;
  if (chunk_cases() < 0) return 1;
  if (cont_cases() < 0) return 1;
  if (decide(DEC_QUIET, MURO_STEP_NOW, GET, 10, 0, NULL, 0, 0, 0, "now", NULL, NULL, NULL) < 0)
    return 1;
  if (decide(DEC_MORE, MURO_STEP_WROTE, GET, 10, 0, NULL, 0, wake, 0, "wrote-read", NULL, NULL,
             NULL) < 0)
    return 1;
  if (decide(DEC_MORE, MURO_STEP_WRITE_FAILED, GET, 10, 0, NULL, 0, wake, 0, "read-failed-holds",
             NULL, NULL, NULL) < 0)
    return 1;
  if (fin_reply(GET, 10, "fin-get") < 0) return 1;
  if (fin_reply(POST_BODY, 10, "fin-body") < 0) return 1;
  if (fin_reply(STREAM, 8388608, "stream-peer") < 0) return 1;
  if (decide(DEC_OWE, MURO_STEP_BYTES, STREAM, 10, 413, "too large", 0, 0, 1, "stream-413", NULL,
             NULL, NULL) < 0)
    return 1;
  if (stream_open() < 0) return 1;

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
  if (admit_agrees() < 0) return 1;
  if (clocks() < 0) return 1;
  if (one_byte(GET, 0, DEC_IDLE, "get-bytes") < 0) return 1;
  if (one_byte(CLOSE, 1, DEC_QUIET, "close-bytes") < 0) return 1;
  if (pipeline() < 0) return 1;
  if (peer_drops() < 0) return 1;
  if (peer_short() < 0) return 1;
  if (past_slice_hend() < 0) return 1;
  if (carry_header_end_book() < 0) return 1;
  if (leftover_append() < 0) return 1;
  if (idle_edges() < 0) return 1;
  if (idle_peer_get() < 0) return 1;
  if (holds(MURO_STEP_BYTES, 0, "write-bytes-holds") < 0) return 1;
  if (write_accept() < 0) return 1;
  if (holds(MURO_STEP_WROTE, 1, "idle-wrote-holds") < 0) return 1;
  if (holds(MURO_STEP_WRITE_FAILED, 1, "idle-failed-holds") < 0) return 1;
  if (reply_fail_and_now() < 0) return 1;
  if (closes() < 0) return 1;
  if (handler_paths() < 0) return 1;
  if (cover_stays_shut() < 0) return 1;
  if (stuck_note() < 0) return 1;
  if (capture_stream() < 0) return 1;
  if (sweep() < 0) return 1;

  printf("handwritten-after-close-spans %d\n", after_spans);
  printf("scripted %d\n", scripted);
  printf("sweep-seq %d sweep-steps %d sweep-covered %d\n", sweep_seq, sweep_steps, sweep_covered);
  printf("book-cmp %d book-skip %d\n", book_cmp, book_skip);
  printf("quiet-read-shut %d\n", quiet_read_shut);
  if (quiet_read_shut == 0) return 1;
  if (after_spans != 0) return 1;
  printf("OK\n");
  return 0;
}
