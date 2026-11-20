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

/* Slices point at the connection receive buffer. They are valid only
   until the handler returns. buf is that same request span, including
   the body, so the header accessors can walk it. */
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

/* headers is a block of extra header lines that already ends in CRLF.
   It must not include Connection or Content-Length. The book writes
   those. close_after 1 forces Connection: close. owe_status 1 means a
   side effect has committed and a status must still leave the socket. */
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

/* Return 0 to send resp. Return -1 to close. If owe_status is set,
   the close still carries the book's 500. */
typedef int (*MuroHttpHandler)(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user);

typedef struct MuroHttpConfig {
  const char *bind_host; /* NULL is 127.0.0.1. IPv4 only. */
  int port;
  int idle_ms;
  int max_conn;
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

/* 0 after SIGINT or SIGTERM. -1 if listen or setup fails.
   -2 if the bind host is empty or not an IPv4 address.
   Port 0 prints "listening <host> <port>". */
int muro_http_serve(const MuroHttpConfig *cfg, MuroHttpHandler handler, void *user);

#endif
