/* A backend that is not a socket. It reports the same facts the
   poll adapter reports: a connection was accepted, bytes are in the
   recv buffer, a write of n bytes finished, the peer closed, and
   the clock. It performs the actions the step returns. The bytes
   to write are the step's pointer; this file does not copy them
   before recording them.

   Build: make c/test_step
   The reference server is still c/host.c. This file is the pattern,
   not a second production loop. */

#include "step.h"
#include "host_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { IN_CAP = 65536, OUT_CAP = 69632 };

static int on_req(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  static const char type[] = "Content-Type: text/plain\r\n";
  (void)user;
  resp->headers = type;
  resp->headers_len = sizeof type - 1;
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
  if (req->method.n == 3 && req->method.p && memcmp(req->method.p, "GET", 3) == 0) {
    resp->body = (const uint8_t *)"hello\n";
    resp->body_len = 6;
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

/* Drive until the step wants the adapter to wait or to close.
   A returned span is accepted in one report, the way a socket
   write that takes the whole buffer does. The pointer must sit
   in the connection's send buffer on the keep-alive path. */
static int turn(HttpStep *st, StepIn *in, uint8_t *out, size_t out_cap, uint8_t *got, size_t *got_n,
                size_t got_cap, int *closed) {
  for (;;) {
    StepOut o;
    if (muro_http_step(st, in, &o) < 0) return -1;
    if (o.close_conn) {
      if (o.status_now && o.status && o.status_len) {
        if (*got_n + o.status_len > got_cap) return -1;
        memcpy(got + *got_n, o.status, o.status_len);
        *got_n += o.status_len;
      }
      *closed = 1;
      return 0;
    }
    if (o.bytes && o.len) {
      if (o.bytes < out || o.bytes + o.len > out + out_cap) return -1;
      if (*got_n + o.len > got_cap) return -1;
      memcpy(got + *got_n, o.bytes, o.len);
      *got_n += o.len;
      in->kind = MURO_STEP_WROTE;
      in->n = o.len;
      continue;
    }
    if (!o.read_more && !o.close_conn) return -1;
    return 0;
  }
}

static int fail(const char *what) {
  fprintf(stderr, "mem adapter: %s\n", what);
  return 1;
}

int main(void) {
  static const char date[] = "Sun, 06 Nov 1994 08:49:37 GMT";
  static const char req[] =
      "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
  static const char bad[] = "GET / HTTP/1.1\r\nHost: localhost\r\n";
  uint8_t *inb;
  uint8_t *outb;
  uint8_t *got;
  HttpStep st;
  struct HostIn hin;
  int rd = 0;
  int wr = 0;
  size_t got_n = 0;
  int closed = 0;
  StepIn in;
  const size_t req_n = sizeof req - 1;
  const size_t bad_n = sizeof bad - 1;

  inb = malloc(IN_CAP);
  outb = malloc(OUT_CAP);
  got = malloc(OUT_CAP);
  if (!inb || !outb || !got) return fail("alloc");
  host_in_init(&hin, -1, 8388608);
  muro_http_step_bind(&st, inb, IN_CAP, outb, OUT_CAP, &hin, &rd, &wr, on_req, NULL, 8388608, 60000);
  memset(&in, 0, sizeof in);
  in.kind = MURO_STEP_ACCEPTED;
  in.now_ms = 1000;
  in.date = (const uint8_t *)date;
  in.date_n = 29;
  if (turn(&st, &in, outb, OUT_CAP, got, &got_n, OUT_CAP, &closed) < 0 || closed || !rd)
    return fail("accept");

  memcpy(inb, req, req_n);
  muro_http_step_set_in_len(&st, req_n);
  memset(&in, 0, sizeof in);
  in.kind = MURO_STEP_BYTES;
  in.empty_before = 1;
  in.now_ms = 1000;
  in.date = (const uint8_t *)date;
  in.date_n = 29;
  got_n = 0;
  if (turn(&st, &in, outb, OUT_CAP, got, &got_n, OUT_CAP, &closed) < 0 || closed)
    return fail("keepalive");
  if (!memmem(got, got_n, "HTTP/1.1 200", 12) || !memmem(got, got_n, "hello\n", 6) ||
      !memmem(got, got_n, date, 29) || !memmem(got, got_n, "Content-Length: 6", 17))
    return fail("keepalive bytes");
  if (muro_http_step_in_len(&st) != 0) return fail("keepalive leftover");

  memcpy(inb, req, req_n);
  muro_http_step_set_in_len(&st, req_n);
  memset(&in, 0, sizeof in);
  in.kind = MURO_STEP_BYTES;
  in.empty_before = 1;
  in.now_ms = 1100;
  in.date = (const uint8_t *)date;
  in.date_n = 29;
  got_n = 0;
  if (turn(&st, &in, outb, OUT_CAP, got, &got_n, OUT_CAP, &closed) < 0 || closed)
    return fail("second");
  if (!memmem(got, got_n, "hello\n", 6)) return fail("second bytes");

  memcpy(inb, bad, bad_n);
  muro_http_step_set_in_len(&st, bad_n);
  memset(&in, 0, sizeof in);
  in.kind = MURO_STEP_PEER_CLOSED;
  in.empty_before = 1;
  in.now_ms = 1200;
  in.date = (const uint8_t *)date;
  in.date_n = 29;
  got_n = 0;
  closed = 0;
  if (turn(&st, &in, outb, OUT_CAP, got, &got_n, OUT_CAP, &closed) < 0 || !closed)
    return fail("peer");
  if (!memmem(got, got_n, "400", 3) || !memmem(got, got_n, "malformed", 9)) return fail("peer bytes");

  muro_http_step_shutdown(&st);
  host_in_close(&hin);
  free(inb);
  free(outb);
  free(got);
  printf("OK\n");
  return 0;
}
