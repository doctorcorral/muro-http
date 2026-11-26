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

int main(void) {
  uint8_t *inb = calloc(1, IN_CAP);
  uint8_t *outb = calloc(1, OUT_CAP);
  HttpStep st;
  struct HostIn hin;
  StepIn in;
  StepOut out;
  int rd = 0, wr = 0;
  size_t have, rest;
  uint64_t hend;
  muro__slice sl;
  int rc;

  if (!inb || !outb) {
    free(inb);
    free(outb);
    return fail("alloc");
  }

  host_in_init(&hin, -1, 10);
  muro_http_step_bind(&st, inb, IN_CAP, outb, OUT_CAP, &hin, &rd, &wr, on_req, NULL, 10, 60000);
  if (st.hend != 0) {
    muro_http_step_shutdown(&st);
    host_in_close(&hin);
    free(inb);
    free(outb);
    return fail("bind hend");
  }

  set_in(&in, MURO_STEP_ACCEPTED, 0, 0);
  memset(&out, 0, sizeof out);
  if (muro_http_step(&st, &in, &out) < 0 || out.close_conn || st.hend != 0) {
    muro_http_step_shutdown(&st);
    host_in_close(&hin);
    free(inb);
    free(outb);
    return fail("accept");
  }

  have = strlen(POST_SHORT);
  memcpy(inb, POST_SHORT, have);
  muro_http_step_set_in_len(&st, have);
  set_in(&in, MURO_STEP_BYTES, 0, 1);
  memset(&out, 0, sizeof out);
  rc = muro_http_step(&st, &in, &out);
  sl = muro__slice_of((const uint8_t *)POST_SHORT, have);
  hend = http_step_headerEnd(sl);
  if (rc < 0 || out.close_conn || out.write_more || !out.read_more || hend == 0 ||
      hend == have + 1 || st.hend != hend) {
    fprintf(stderr, "book host hend: short rc=%d hend host=%llu want=%llu read=%d write=%d\n", rc,
            (unsigned long long)st.hend, (unsigned long long)hend, out.read_more, out.write_more);
    muro_http_step_shutdown(&st);
    host_in_close(&hin);
    free(inb);
    free(outb);
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
    muro_http_step_shutdown(&st);
    host_in_close(&hin);
    free(inb);
    free(outb);
    return 1;
  }

  muro_http_step_shutdown(&st);
  host_in_close(&hin);
  free(inb);
  free(outb);
  printf("OK\n");
  return 0;
}
