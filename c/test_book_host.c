/* Prove the book host (muro_http_step in step_book.c) persists
   HttpStep.hend across steps via load_http / store_http. Linked
   with -DMURO_STEP_BOOK (c/step_book_host.o). */

#include "step.h"
#include "host_stream.h"
#include "step_slice.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IN_CAP = 4096, OUT_CAP = 8192 };

static const char DATE[] = "Sun, 06 Nov 1994 08:49:37 GMT";
static const char GET[] = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
static const char GET2[] = "GET /x HTTP/1.1\r\nHost: h\r\n\r\n";
static const char TWO_DIFF[] =
    "GET / HTTP/1.1\r\nHost: h\r\n\r\nGET /x HTTP/1.1\r\nHost: h\r\n\r\n";
static const char CLOSE_THEN[] =
    "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\nGET /x HTTP/1.1\r\nHost: h\r\n\r\n";
static const char POST_SHORT[] = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\n";
static const char POST_BODY[] = "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\nabcd";

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

static void set_in(StepIn *in, int kind, size_t n, int empty_before) {
  memset(in, 0, sizeof *in);
  in->kind = kind;
  in->n = n;
  in->now_ms = 1000;
  in->date = (const uint8_t *)DATE;
  in->date_n = 29;
  in->empty_before = empty_before;
}

static int fail(const char *what) {
  fprintf(stderr, "book host hend: %s\n", what);
  return 1;
}

static uint64_t slice_end(const char *buf) {
  muro__slice sl = muro__slice_of((const uint8_t *)buf, strlen(buf));
  uint64_t h = http_step_headerEnd(sl);
  if (h == 0 || h == sl.n + 1) return 0;
  return h;
}

static void done(HttpStep *st, struct HostIn *hin, uint8_t *inb, uint8_t *outb) {
  muro_http_step_shutdown(st);
  host_in_close(hin);
  free(inb);
  free(outb);
}

static int bind_fresh(HttpStep *st, struct HostIn *hin, uint8_t **inb, uint8_t **outb, int *rd,
                      int *wr, StepIn *in, StepOut *out) {
  *rd = 0;
  *wr = 0;
  *inb = calloc(1, IN_CAP);
  *outb = calloc(1, OUT_CAP);
  if (!*inb || !*outb) {
    free(*inb);
    free(*outb);
    *inb = *outb = NULL;
    return fail("alloc");
  }
  host_in_init(hin, -1, 10);
  muro_http_step_bind(st, *inb, IN_CAP, *outb, OUT_CAP, hin, rd, wr, on_req, NULL, 10, 60000);
  if (st->hend != 0) {
    done(st, hin, *inb, *outb);
    return fail("bind hend");
  }
  set_in(in, MURO_STEP_ACCEPTED, 0, 0);
  memset(out, 0, sizeof *out);
  if (muro_http_step(st, in, out) < 0 || out->close_conn || st->hend != 0) {
    done(st, hin, *inb, *outb);
    return fail("accept");
  }
  return 0;
}

/* One complete write of the parked response. A keep-alive pipeline
   parks the next reply in this same call, so looping would also
   send that second reply and hide the rebind. */
static int write_once(HttpStep *st, StepIn *in, StepOut *out, const char *what) {
  size_t n;
  if (!out->write_more || out->len == 0) return fail(what);
  n = out->len;
  set_in(in, MURO_STEP_WROTE, n, 0);
  memset(out, 0, sizeof *out);
  if (muro_http_step(st, in, out) < 0) return fail(what);
  return 0;
}

static int feed(HttpStep *st, uint8_t *inb, StepIn *in, StepOut *out, const char *buf, int empty,
                const char *what) {
  size_t n = strlen(buf);
  if (n > IN_CAP) return fail(what);
  memcpy(inb, buf, n);
  muro_http_step_set_in_len(st, n);
  set_in(in, MURO_STEP_BYTES, 0, empty);
  memset(out, 0, sizeof *out);
  if (muro_http_step(st, in, out) < 0) return fail(what);
  return 0;
}

int main(void) {
  uint8_t *inb = NULL;
  uint8_t *outb = NULL;
  HttpStep st;
  struct HostIn hin;
  StepIn in;
  StepOut out;
  int rd, wr;
  size_t have, rest;
  uint64_t hend, hend_get, hend_get2;
  int rc;

  /* Short POST headers, then the body: hend is stored and reused. */
  if (bind_fresh(&st, &hin, &inb, &outb, &rd, &wr, &in, &out) != 0) return 1;
  have = strlen(POST_SHORT);
  memcpy(inb, POST_SHORT, have);
  muro_http_step_set_in_len(&st, have);
  set_in(&in, MURO_STEP_BYTES, 0, 1);
  memset(&out, 0, sizeof out);
  rc = muro_http_step(&st, &in, &out);
  hend = slice_end(POST_SHORT);
  if (rc < 0 || out.close_conn || out.write_more || !out.read_more || hend == 0 ||
      st.hend != hend) {
    fprintf(stderr, "book host hend: short rc=%d hend host=%llu want=%llu read=%d write=%d\n", rc,
            (unsigned long long)st.hend, (unsigned long long)hend, out.read_more, out.write_more);
    done(&st, &hin, inb, outb);
    return 1;
  }
  rest = strlen(POST_BODY) - have;
  memcpy(inb + have, POST_BODY + have, rest);
  muro_http_step_set_in_len(&st, have + rest);
  set_in(&in, MURO_STEP_BYTES, 0, 0);
  memset(&out, 0, sizeof out);
  rc = muro_http_step(&st, &in, &out);
  if (rc < 0 || !out.write_more || !out.bytes || !out.len || st.hend != hend) {
    fprintf(stderr, "book host hend: body rc=%d hend host=%llu want=%llu write=%d len=%zu\n", rc,
            (unsigned long long)st.hend, (unsigned long long)hend, out.write_more, out.len);
    done(&st, &hin, inb, outb);
    return 1;
  }
  done(&st, &hin, inb, outb);

  /* Past-slice stored end is ignored (mirrors header-end-at-past). */
  {
    muro__slice get = muro__slice_of((const uint8_t *)GET, strlen(GET));
    uint64_t end = http_step_headerEnd(get);
    if (end == 0 || end == get.n + 1) return fail("past-slice end");
    if (http_step_headerEndAt(99, get) != end) return fail("past-slice At");
    if (http_step_headerEndAt(end, get) != end) return fail("past-slice stored");
    if (http_step_headerEndAt(0, get) != end) return fail("past-slice unknown");
  }

  hend_get = slice_end(GET);
  hend_get2 = slice_end(GET2);
  if (hend_get == 0 || hend_get2 == 0 || hend_get == hend_get2) return fail("distinct ends");

  /* Keep-alive: idle clears hend; the next GET rebinds it. */
  if (bind_fresh(&st, &hin, &inb, &outb, &rd, &wr, &in, &out) != 0) return 1;
  if (feed(&st, inb, &in, &out, GET, 1, "ka-get") != 0) {
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (!out.write_more || st.hend != hend_get) {
    fprintf(stderr, "book host hend: ka-get host=%llu want=%llu write=%d\n",
            (unsigned long long)st.hend, (unsigned long long)hend_get, out.write_more);
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (write_once(&st, &in, &out, "ka-idle") != 0) {
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (st.hend != 0 || muro_http_step_in_len(&st) != 0) {
    fprintf(stderr, "book host hend: ka-idle host=%llu in_len=%zu\n", (unsigned long long)st.hend,
            muro_http_step_in_len(&st));
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (feed(&st, inb, &in, &out, GET, 1, "ka-second") != 0) {
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (!out.write_more || st.hend != hend_get) {
    fprintf(stderr, "book host hend: ka-second host=%llu want=%llu\n", (unsigned long long)st.hend,
            (unsigned long long)hend_get);
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (write_once(&st, &in, &out, "ka-second-idle") != 0) {
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (st.hend != 0 || muro_http_step_in_len(&st) != 0) {
    fprintf(stderr, "book host hend: ka-second-idle host=%llu in_len=%zu\n",
            (unsigned long long)st.hend, muro_http_step_in_len(&st));
    done(&st, &hin, inb, outb);
    return 1;
  }
  done(&st, &hin, inb, outb);

  /* Pipelined GET then GET /x: consume must rebind hend to GET2.
     Leaving the first request's end in place would still fall
     inside GET2 (one byte longer) and fail this compare. */
  if (bind_fresh(&st, &hin, &inb, &outb, &rd, &wr, &in, &out) != 0) return 1;
  if (feed(&st, inb, &in, &out, TWO_DIFF, 1, "pipe-diff-one") != 0) {
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (!out.write_more || st.hend != hend_get) {
    fprintf(stderr, "book host hend: pipe-diff-one host=%llu want=%llu\n",
            (unsigned long long)st.hend, (unsigned long long)hend_get);
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (write_once(&st, &in, &out, "pipe-diff-second") != 0) {
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (muro_http_step_in_len(&st) != strlen(GET2) || memcmp(inb, GET2, strlen(GET2)) != 0 ||
      st.hend != hend_get2) {
    fprintf(stderr, "book host hend: pipe-diff-second host=%llu want=%llu in_len=%zu\n",
            (unsigned long long)st.hend, (unsigned long long)hend_get2,
            muro_http_step_in_len(&st));
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (write_once(&st, &in, &out, "pipe-diff-done") != 0) {
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (st.hend != 0 || muro_http_step_in_len(&st) != 0) {
    fprintf(stderr, "book host hend: pipe-diff-done host=%llu in_len=%zu\n",
            (unsigned long long)st.hend, muro_http_step_in_len(&st));
    done(&st, &hin, inb, outb);
    return 1;
  }
  done(&st, &hin, inb, outb);

  /* Connection: close finishes with keep 1 (keepHand / hand force
     keep when hclose). finishWrite therefore takes quiet (hend 0)
     and does not classify a tail. consume is skipped when
     img_half || img_keep || prev_keep != 0, so the unconsumed
     pipelined GET /x must still sit in the buffer with hend 0. */
  if (bind_fresh(&st, &hin, &inb, &outb, &rd, &wr, &in, &out) != 0) return 1;
  if (feed(&st, inb, &in, &out, CLOSE_THEN, 1, "close-skip") != 0) {
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (!out.write_more) {
    done(&st, &hin, inb, outb);
    return fail("close-skip reply");
  }
  if (write_once(&st, &in, &out, "close-skip-done") != 0) {
    done(&st, &hin, inb, outb);
    return 1;
  }
  if (st.hend != 0 || muro_http_step_in_len(&st) != strlen(CLOSE_THEN) ||
      memcmp(inb, CLOSE_THEN, strlen(CLOSE_THEN)) != 0) {
    fprintf(stderr, "book host hend: close-skip host=%llu in_len=%zu want_len=%zu\n",
            (unsigned long long)st.hend, muro_http_step_in_len(&st), strlen(CLOSE_THEN));
    done(&st, &hin, inb, outb);
    return 1;
  }
  done(&st, &hin, inb, outb);

  printf("OK\n");
  return 0;
}
