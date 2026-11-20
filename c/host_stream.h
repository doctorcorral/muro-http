#ifndef MURO_HTTP_HOST_STREAM_H
#define MURO_HTTP_HOST_STREAM_H

/* Request-body side of one connection. The image decides when the
   request is complete enough to handle. This file decodes a finished
   chunked body, reads a body the 64 KiB window will not hold, and
   remembers bytes that arrived after a streamed chunked message.
   It does not choose a response. */

#include "muro_http.h"
#include "conn.h"

struct HostIn;

struct MuroHttpBody {
  struct HostIn *in;
};

struct HostIn {
  int fd;
  uint64_t max_body;
  uint8_t *hold;
  size_t hold_len;
  size_t hold_off;
  uint8_t *decoded;
  const uint8_t *mem;
  size_t mem_len;
  size_t mem_off;
  uint8_t *nextq;
  size_t nextq_len;
  uint8_t scratch[2048];
  size_t scratch_len;
  size_t scratch_off;
  Ck ck;
  int kind;
  int captured;
  int open;
  int err;
  int pull_owns;
  uint64_t need;
  uint64_t got;
  struct MuroHttpBody pub;
};

void host_in_init(struct HostIn *in, int fd, uint64_t max_body);
void host_in_reset(struct HostIn *in);
void host_in_close(struct HostIn *in);

/* Before step, when streamTake is 1. Copies the bytes after the
   header block so a later consume can drop that prefix. */
void host_in_capture(struct HostIn *in, const uint8_t *buf, size_t n);

/* 0 to continue. 413, 417, or 501 to send that status and stop.
   *send_100 is 1 when the host should write 100 Continue before
   reading the rest of an acceptable HTTP/1.1 body. */
int host_in_policy(const uint8_t *buf, size_t n, uint64_t max_body, int *send_100);

/* Fills req. A finished chunked body is decoded into a side buffer.
   A streamed body sets req->body_in and leaves req->body empty.
   Returns 0, or 400 or 413. */
int host_in_bind(struct HostIn *in, const uint8_t *buf, size_t n, MuroHttpRequest *req);

/* pull_owns 1 means the response pull will read body_in. */
void host_in_after(struct HostIn *in, int pull_owns, int *force_close, int *override);

/* Moves pipelined bytes out. The caller frees *p. Returns 1 if any. */
int host_in_take_next(struct HostIn *in, uint8_t **p, size_t *n);

/* 1 when a drain deadline has passed. The host loop defines it. */
int host_io_should_stop(void);

#endif
