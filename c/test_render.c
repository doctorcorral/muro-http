/* Check the emitted response bytes. The equations in render.muro
   cover an empty body. A positive Content-Length is checked here,
   where rdShow runs to completion. */

#include "shim.h"
#include "render.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void bad(const char *what) {
  fprintf(stderr, "FAIL %s\n", what);
  failures += 1;
}

static muro__slice of(const char *lit) { return muro__slice_of((const uint8_t *)lit, strlen(lit)); }

static int contains(muro__slice s, const char *lit) {
  size_t n = strlen(lit);
  size_t i;
  if (n > s.n) return 0;
  for (i = 0; i + n <= s.n; i++) {
    if (memcmp(s.p + i, lit, n) == 0) return 1;
  }
  return 0;
}

static int content_length(muro__slice s, size_t *cl, size_t *body) {
  size_t i;
  int hend = -1;
  for (i = 0; i + 3 < s.n; i++) {
    if (s.p[i] == '\r' && s.p[i + 1] == '\n' && s.p[i + 2] == '\r' && s.p[i + 3] == '\n') {
      hend = (int)i;
      break;
    }
  }
  if (hend < 0) return 0;
  *body = s.n - (size_t)hend - 4;
  for (i = 0; i + 16 < (size_t)hend; i++) {
    if (memcmp(s.p + i, "Content-Length: ", 16) == 0) {
      size_t v = 0;
      size_t j = i + 16;
      if (j >= (size_t)hend || s.p[j] < '0' || s.p[j] > '9') return 0;
      while (j < (size_t)hend && s.p[j] >= '0' && s.p[j] <= '9') {
        v = v * 10u + (size_t)(s.p[j] - '0');
        j += 1;
      }
      *cl = v;
      return 1;
    }
  }
  return 0;
}

static void expect_eq(const char *name, muro__slice s, const char *lit) {
  size_t n = strlen(lit);
  if (s.n != n || memcmp(s.p, lit, n) != 0) {
    fprintf(stderr, "FAIL %s: got %zu bytes\n", name, s.n);
    failures += 1;
  }
  muro_http_reset();
}

static void expect_render(const char *name, uint64_t status, const char *reason, const char *extra,
                          const char *body, uint64_t keep, const char *lit) {
  muro__slice s = httpRender(status, of(reason), of(extra), of(body), keep);
  expect_eq(name, s, lit);
}

int main(void) {
  const char *type = "Content-Type: text/plain; charset=utf-8\r\n";
  const char *json = "Content-Type: application/json; charset=utf-8\r\n";
  muro__slice row;
  size_t cl = 0;
  size_t body = 0;
  expect_eq("malformed", httpMalformed(),
            "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 9\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\nmalformed");
  expect_eq("internal", httpInternal(),
            "HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: 8\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\ninternal");
  expect_render("health-keep", 200, "OK", type, "ok", 0,
                "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n\r\nok");
  expect_render("health-close", 200, "OK", type, "ok", 1,
                "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n\r\nok");
  expect_render("empty-keep", 200, "OK", type, "", 0,
                "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n\r\n");
  expect_render("no-content", 204, "No Content", "", "", 0,
                "HTTP/1.1 204 No Content\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n");
  expect_render("json", 200, "OK", json, "[]", 0,
                "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n"
                "Content-Type: application/json; charset=utf-8\r\n\r\n[]");
  expect_render("method", 405, "Method Not Allowed", type, "method\n", 0,
                "HTTP/1.1 405 Method Not Allowed\r\nConnection: keep-alive\r\nContent-Length: 7\r\n"
                "Content-Type: text/plain; charset=utf-8\r\n\r\nmethod\n");
  row = httpRender(201, of("Created"), of(json), of("1234567890"), 0);
  if (!content_length(row, &cl, &body) || cl != 10 || body != 10 || !contains(row, "1234567890"))
    bad("row-ten");
  muro_http_reset();
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
