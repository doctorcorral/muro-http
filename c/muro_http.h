#ifndef MURO_HTTP_H
#define MURO_HTTP_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* A reusable HTTP/1.1 server. The books choose framing, admission,
   keep-alive, and close. The handler chooses the status, the extra
   headers, and the body for a request the book accepted. This
   worker process owns its sockets, its buffers, its connection
   table, its clock, and its wait. The wait is kqueue on macOS,
   epoll on Linux, and poll otherwise. MURO_HTTP_WAIT=poll forces
   poll. It writes 100 Continue, 413, and 417, and it writes 501
   for a method the book marks with method code 1.
   host_in_policy does not answer an unknown transfer coding. The
   host copies the book's 400 and the book's unimplemented transfer
   coding, which is origin 2. It does not invent a status the book
   did not name. */

typedef struct MuroHttpSlice {
  const uint8_t *p;
  size_t n;
} MuroHttpSlice;

/* Request slices point at the connection receive buffer, except a
   decoded chunked body: that body is a side buffer, freed when the
   connection finishes the response. The receive buffer stays live
   while the host frames the response, which is after the handler
   returns. A slice into the handler's own stack does not. buf is
   the request span the image accepted. A streamed body leaves
   body.n at 0 and sets body_in. */
typedef struct MuroHttpBody MuroHttpBody;

typedef struct MuroHttpRequest {
  MuroHttpSlice method;
  MuroHttpSlice target;
  MuroHttpSlice path;
  MuroHttpSlice query;
  MuroHttpSlice version;
  MuroHttpSlice body;
  const uint8_t *buf;
  size_t buf_n;
  MuroHttpBody *body_in;
} MuroHttpRequest;

/* >0 bytes copied, 0 at the end, -1 on a framing error or a body
   past the configured maximum. A buffered body can be read here as
   well as through body. A streamed body must be read here, or passed
   to the response pull, or the host closes after the response. The
   read waits in the server wait (kqueue, epoll, or poll). Other
   connections are accepted and served on that wait. A nested read
   does not watch the socket already inside a read. Past eight
   nested reads, the wait is poll(2) on this socket alone. */
int muro_http_body_read(MuroHttpBody *body, uint8_t *dst, size_t cap);

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
   Connection, Content-Length, Transfer-Encoding, and Date in that
   block are dropped. The book writes Date, Connection, and
   Content-Length or Transfer-Encoding. close_after 1 forces
   Connection: close. A status from 400 through 599 also closes.
   owe_status 1 means a side effect has committed and a status must
   still leave the socket. pull is NULL for a buffered body. When
   pull is set and has_length is 1, content_length is the body size
   and the bytes come from pull. HEAD still sends that length and
   does not call pull. When pull is set and has_length is 0, an
   HTTP/1.1 response is chunked and an HTTP/1.0 response is
   close-delimited. HEAD with an unknown length sends no
   Content-Length and no Transfer-Encoding, closes, and does not
   call pull. pull returns >0, 0 at the end, or -1. The host calls
   it only after the previous window has been written. */
typedef int (*MuroHttpPull)(uint8_t *dst, size_t cap, void *user);

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
  MuroHttpPull pull;
  void *pull_user;
  int has_length;
  uint64_t content_length;
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
  int max_conn; /* 1..1024 per worker. serve returns -1 outside that. */
  const char *name; /* log prefix. NULL is muro-http. */
  int max_body; /* default 8388608. 0 rejects every positive length. */
  int drain_ms; /* default 1000. SIGINT and SIGTERM wait this long. */
  int workers;  /* 1..64 processes. Default 1. max_conn is per worker. */
} MuroHttpConfig;

void muro_http_config_init(MuroHttpConfig *cfg);
/* 0 ok, -1 a bad environment value. */
int muro_http_config_env(MuroHttpConfig *cfg);
/* 1 consumed a flag, 0 not one of ours, -1 bad value, 2 help. */
int muro_http_config_arg(MuroHttpConfig *cfg, int *index, int argc, char **argv);
/* Environment, then arguments. 0 ok, -1 error, >0 help. */
int muro_http_config_parse(MuroHttpConfig *cfg, int argc, char **argv);
void muro_http_config_usage(FILE *out, const MuroHttpConfig *cfg);

/* 0 after SIGINT or SIGTERM, once in-flight requests have finished
   or drain_ms has passed. -1 if listen or setup fails, if max_conn
   is outside 1..1024, if workers is outside 1..64, if workers is
   above 1 and the port is 0, or if max_body or drain_ms is negative.
   -2 if the bind host is empty or not an IPv4 address. Every
   successful listen prints "listening <host> <port>", including
   port 0. workers above 1 prints one line per listening socket.
   Default is one process and does not fork. Above that, each
   worker is its own process, event loop, and connection table.
   On Linux each worker binds with SO_REUSEPORT. Elsewhere the
   parent binds once and passes accepted sockets. */
int muro_http_serve(const MuroHttpConfig *cfg, MuroHttpHandler handler, void *user);

#endif
