/* A second server on the HTTP/1.1 core. GET answers hello.
   POST / answers the same static hello, so a bench can compare that
   body with other servers. Any other POST still echoes the body.
   Any other method is 405. No SQLite, no JWT, and no challenge route. */

#include "muro_http.h"

#include <stdlib.h>
#include <string.h>

/* Pointers stored on resp must outlive this function. The host copies
   them after the return. type is static. The reasons and the fixed
   bodies are string literals. A buffered POST echoes the request
   body, which stays live through that copy. A streamed body is copied
   into echo_buf before the return. The host copies that too, before
   another request reuses the buffer. A pointer into this stack would
   dangle. */
static uint8_t *echo_buf;
static size_t echo_len;
static size_t echo_cap;

static int slurp_body(const MuroHttpRequest *req, const uint8_t **p, size_t *n) {
  if (req->body.n > 0 && req->body.p) {
    *p = req->body.p;
    *n = req->body.n;
    return 0;
  }
  if (!req->body_in) {
    *p = (const uint8_t *)"";
    *n = 0;
    return 0;
  }
  echo_len = 0;
  for (;;) {
    int r;
    if (echo_len == echo_cap) {
      size_t nc = echo_cap == 0 ? 4096 : echo_cap * 2;
      uint8_t *np = realloc(echo_buf, nc);
      if (!np) return -1;
      echo_buf = np;
      echo_cap = nc;
    }
    r = muro_http_body_read(req->body_in, echo_buf + echo_len, echo_cap - echo_len);
    if (r < 0) return -1;
    if (r == 0) break;
    echo_len += (size_t)r;
  }
  *p = echo_buf;
  *n = echo_len;
  return 0;
}

static int on_req(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  static const char type[] = "Content-Type: text/plain\r\n";
  const uint8_t *body = NULL;
  size_t body_n = 0;
  (void)user;
  resp->headers = type;
  resp->headers_len = sizeof type - 1;
  if (req->method.n == 3 && memcmp(req->method.p, "GET", 3) == 0) {
    resp->status = 200;
    resp->reason = "OK";
    resp->reason_len = 2;
    resp->body = (const uint8_t *)"hello\n";
    resp->body_len = 6;
    return 0;
  }
  if (req->method.n == 4 && memcmp(req->method.p, "POST", 4) == 0) {
    /* POST / is the bench route: the same static body as GET.
       A buffered body is already consumed. A streamed body is
       discarded so the next request on the connection stays framed.
       POST /echo and every other path still echo, for the tests. */
    if (req->path.n == 1 && req->path.p && req->path.p[0] == '/') {
      if (!(req->body.n > 0 && req->body.p) && req->body_in) {
        uint8_t dump[512];
        for (;;) {
          int r = muro_http_body_read(req->body_in, dump, sizeof dump);
          if (r < 0) {
            resp->status = 400;
            resp->reason = "Bad Request";
            resp->reason_len = 11;
            resp->body = (const uint8_t *)"bad\n";
            resp->body_len = 4;
            return 0;
          }
          if (r == 0) break;
        }
      }
      resp->status = 200;
      resp->reason = "OK";
      resp->reason_len = 2;
      resp->body = (const uint8_t *)"hello\n";
      resp->body_len = 6;
      return 0;
    }
    if (slurp_body(req, &body, &body_n) < 0) {
      resp->status = 400;
      resp->reason = "Bad Request";
      resp->reason_len = 11;
      resp->body = (const uint8_t *)"bad\n";
      resp->body_len = 4;
      return 0;
    }
    resp->status = 200;
    resp->reason = "OK";
    resp->reason_len = 2;
    resp->body = body;
    resp->body_len = body_n;
    return 0;
  }
  resp->status = 405;
  resp->reason = "Method Not Allowed";
  resp->reason_len = 18;
  resp->body = (const uint8_t *)"method\n";
  resp->body_len = 7;
  return 0;
}

int main(int argc, char **argv) {
  MuroHttpConfig cfg;
  int rc;
  muro_http_config_init(&cfg);
  cfg.name = "muro-hello";
  rc = muro_http_config_parse(&cfg, argc, argv);
  if (rc > 0) return 0;
  if (rc < 0) return 2;
  rc = muro_http_serve(&cfg, on_req, NULL);
  if (rc == -2) return 2;
  return rc == 0 ? 0 : 1;
}
