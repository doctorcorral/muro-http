#ifndef MURO_HTTP_H
#define MURO_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* A reusable HTTP/1.1 server. The books choose framing, admission,
   keep-alive, and close. The handler chooses the status, the extra
   headers, and the body. This process owns the sockets, the buffers,
   the connection table, the clock, and poll. It does not choose a
   status line. */

typedef struct MuroHttpSlice {
  const uint8_t *p;
  size_t n;
} MuroHttpSlice;

/* Request slices point at the connection receive buffer. That buffer
   stays live while the host frames the response, which is after the
   handler returns. A slice into the handler's own stack does not.
   buf is that same request span, including the body, so the header
   accessors can walk it during that copy. */
typedef struct MuroHttpRequest {
  MuroHttpSlice method;
  MuroHttpSlice target;
  MuroHttpSlice path;
  MuroHttpSlice query;
  MuroHttpSlice version;
  MuroHttpSlice body;
  const uint8_t *buf;
  size_t buf_n;
} MuroHttpRequest;

/* header_at and header_find return 1 when the header is present.
   An empty value is present and has n == 0. A missing header returns
   0. Names are compared without regard to ASCII case. */
size_t muro_http_header_count(const MuroHttpRequest *req);
int muro_http_header_at(const MuroHttpRequest *req, size_t index, MuroHttpSlice *name,
                        MuroHttpSlice *value);
int muro_http_header_find(const MuroHttpRequest *req, const char *name, MuroHttpSlice *value);

/* reason, headers, and body must stay valid until the host has framed
   the response, which is after the handler returns. A pointer into
   the handler's stack is then dangling. A string literal, a static
   buffer, or a slice of the request buffer is safe. headers is a
   block of extra header lines. A CR or LF in the reason, or a line
   break that is not CRLF, is rejected and the response is not sent.
   Connection, Content-Length, and Transfer-Encoding in that block
   are dropped. The book writes Connection and Content-Length.
   close_after 1 forces Connection: close. A status from 400 through
   599 also closes. owe_status 1 means a side effect has committed
   and a status must still leave the socket. */
typedef struct MuroHttpResponse {
  int status;
  const char *reason;
  size_t reason_len;
  const char *headers;
  size_t headers_len;
  const uint8_t *body;
  size_t body_len;
  int close_after;
  int owe_status;
} MuroHttpResponse;

/* Return 0 to send resp. Return -1 to close without that resp.
   If owe_status is set and no status byte has been written, the
   close still sends the book's 500. If owe_status is clear, return
   -1 sends nothing. */
typedef int (*MuroHttpHandler)(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user);

typedef struct MuroHttpConfig {
  const char *bind_host; /* NULL is 127.0.0.1. IPv4 only. */
  int port;
  int idle_ms;
  int max_conn; /* 1..1024. serve returns -1 outside that. */
  const char *name; /* log prefix. NULL is muro-http. */
} MuroHttpConfig;

void muro_http_config_init(MuroHttpConfig *cfg);
/* 0 ok, -1 a bad environment value. */
int muro_http_config_env(MuroHttpConfig *cfg);
/* 1 consumed a flag, 0 not one of ours, -1 bad value, 2 help. */
int muro_http_config_arg(MuroHttpConfig *cfg, int *index, int argc, char **argv);
/* Environment, then arguments. 0 ok, -1 error, >0 help. */
int muro_http_config_parse(MuroHttpConfig *cfg, int argc, char **argv);
void muro_http_config_usage(FILE *out, const MuroHttpConfig *cfg);

/* 0 after SIGINT or SIGTERM. -1 if listen or setup fails, or if
   max_conn is outside 1..1024. -2 if the bind host is empty or not
   an IPv4 address. Every successful listen prints
   "listening <host> <port>", including port 0. */
int muro_http_serve(const MuroHttpConfig *cfg, MuroHttpHandler handler, void *user);

#endif
