/* A second server on the HTTP/1.1 core. GET answers hello.
   POST echoes the body. Any other method is 405. No SQLite, no JWT,
   and no challenge route. */

#include "muro_http.h"

#include <string.h>

static int on_req(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  static const char type[] = "Content-Type: text/plain\r\n";
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
    resp->status = 200;
    resp->reason = "OK";
    resp->reason_len = 2;
    resp->body = req->body.p;
    resp->body_len = req->body.n;
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
