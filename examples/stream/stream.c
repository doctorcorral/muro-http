/* Streaming and chunked bodies on the HTTP/1.1 core.
   GET / is "ok\n". GET /blob pulls 1 MiB + 1 of 'x' with a known
   length. GET /live pulls three bytes with no declared length.
   POST, PUT, and PATCH /echo pull the request body back out.
   DELETE /echo is 204. OPTIONS answers Allow. One pull is in
   progress at a time: this process is single-threaded. */

#include "muro_http.h"

#include <string.h>

enum { BLOB = 1024 * 1024 + 1 };

static size_t blob_left;
static size_t live_off;

static int pull_blob(uint8_t *dst, size_t cap, void *user) {
  size_t *left = user;
  size_t n;
  if (!left) return -1;
  n = cap;
  if (n > *left) n = *left;
  if (n == 0) return 0;
  memset(dst, 'x', n);
  *left -= n;
  return (int)n;
}

static int pull_live(uint8_t *dst, size_t cap, void *user) {
  static const char live[] = "abc";
  size_t left;
  (void)user;
  if (live_off >= 3) return 0;
  left = 3 - live_off;
  if (left > cap) left = cap;
  memcpy(dst, live + live_off, left);
  live_off += left;
  return (int)left;
}

static int pull_req(uint8_t *dst, size_t cap, void *user) {
  if (!user) return 0;
  return muro_http_body_read(user, dst, cap);
}

static int method_is(const MuroHttpRequest *req, const char *lit) {
  size_t n = strlen(lit);
  return req->method.n == n && req->method.p && memcmp(req->method.p, lit, n) == 0;
}

static int path_is(const MuroHttpRequest *req, const char *lit) {
  size_t n = strlen(lit);
  return req->path.n == n && req->path.p && memcmp(req->path.p, lit, n) == 0;
}

static int digits(MuroHttpSlice s, uint64_t *out) {
  size_t i;
  uint64_t v = 0;
  if (!s.p || s.n == 0) return -1;
  for (i = 0; i < s.n; i++) {
    uint64_t d;
    if (s.p[i] < '0' || s.p[i] > '9') return -1;
    d = (uint64_t)(s.p[i] - '0');
    if (v > (UINT64_MAX - d) / 10) return -1;
    v = v * 10 + d;
  }
  *out = v;
  return 0;
}

static void ok(MuroHttpResponse *resp) {
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
}

static int on_req(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  static const char type[] = "Content-Type: text/plain\r\n";
  static const char allow[] = "Allow: GET, HEAD, POST, PUT, DELETE, OPTIONS, PATCH\r\n";
  MuroHttpSlice cl;
  (void)user;
  resp->headers = type;
  resp->headers_len = sizeof type - 1;
  if (method_is(req, "OPTIONS")) {
    ok(resp);
    resp->headers = allow;
    resp->headers_len = sizeof allow - 1;
    return 0;
  }
  if (method_is(req, "GET") || method_is(req, "HEAD")) {
    if (path_is(req, "/") || req->path.n == 0) {
      ok(resp);
      resp->body = (const uint8_t *)"ok\n";
      resp->body_len = 3;
      return 0;
    }
    if (path_is(req, "/blob")) {
      blob_left = BLOB;
      ok(resp);
      resp->pull = pull_blob;
      resp->pull_user = &blob_left;
      resp->has_length = 1;
      resp->content_length = BLOB;
      return 0;
    }
    if (path_is(req, "/live")) {
      live_off = 0;
      ok(resp);
      resp->pull = pull_live;
      resp->has_length = 0;
      return 0;
    }
  }
  if (method_is(req, "DELETE") && path_is(req, "/echo")) {
    resp->status = 204;
    resp->reason = "No Content";
    resp->reason_len = 10;
    return 0;
  }
  if ((method_is(req, "POST") || method_is(req, "PUT") || method_is(req, "PATCH")) &&
      path_is(req, "/echo")) {
    ok(resp);
    resp->pull = pull_req;
    resp->pull_user = req->body_in;
    if (muro_http_header_find(req, "content-length", &cl) && digits(cl, &resp->content_length) == 0)
      resp->has_length = 1;
    return 0;
  }
  resp->status = 404;
  resp->reason = "Not Found";
  resp->reason_len = 9;
  resp->body = (const uint8_t *)"missing\n";
  resp->body_len = 8;
  return 0;
}

int main(int argc, char **argv) {
  MuroHttpConfig cfg;
  int rc;
  muro_http_config_init(&cfg);
  cfg.name = "muro-stream";
  rc = muro_http_config_parse(&cfg, argc, argv);
  if (rc > 0) return 0;
  if (rc < 0) return 2;
  rc = muro_http_serve(&cfg, on_req, NULL);
  if (rc == -2) return 2;
  return rc == 0 ? 0 : 1;
}
