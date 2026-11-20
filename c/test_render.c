/* Check the emitted response bytes. The equations in render.muro
   cover an empty body. A positive Content-Length is checked here,
   where rdShow runs to completion. */

#include "shim.h"
#include "render.h"
#include "host_render.h"

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

static int has_lit(const uint8_t *p, size_t n, const char *lit) {
  size_t m = strlen(lit);
  size_t i;
  if (m == 0 || m > n) return 0;
  for (i = 0; i + m <= n; i++) {
    if (memcmp(p + i, lit, m) == 0) return 1;
  }
  return 0;
}

static int count_lit(const uint8_t *p, size_t n, const char *lit) {
  size_t m = strlen(lit);
  size_t i;
  int c = 0;
  if (m == 0 || m > n) return 0;
  for (i = 0; i + m <= n; i++) {
    if (memcmp(p + i, lit, m) == 0) c += 1;
  }
  return c;
}

static void expect_copy(const char *name, uint64_t status, const char *reason, const char *extra,
                        const char *body, uint64_t keep, int want_ok) {
  uint8_t dst[2048];
  size_t got = 99;
  size_t n = host_copy_response(dst, sizeof dst, status, (const uint8_t *)reason,
                                reason ? strlen(reason) : 0, (const uint8_t *)extra,
                                extra ? strlen(extra) : 0, (const uint8_t *)body,
                                body ? strlen(body) : 0, keep, &got);
  if (!want_ok) {
    if (n != 0 || got != 0) bad(name);
    return;
  }
  if (n == 0 || !has_lit(dst, n, "\r\n\r\n")) {
    bad(name);
    return;
  }
  if (reason && strstr(reason, "OK") && !has_lit(dst, n, "OK")) bad(name);
  if (extra && strstr(extra, "X-Trace") && !has_lit(dst, n, "X-Trace: 1")) bad(name);
  if (count_lit(dst, n, "Content-Length:") != 1) bad(name);
  if (has_lit(dst, n, "Content-Length: 0") && body && body[0]) bad(name);
  if (has_lit(dst, n, "Transfer-Encoding")) bad(name);
  if (keep == 0 && has_lit(dst, n, "Connection: close")) bad(name);
  if (keep == 1 && !has_lit(dst, n, "Connection: close")) bad(name);
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
  expect_copy("reason-crlf", 200, "OK\r\nX: y", "X-Trace: 1\r\n", "ab", 0, 0);
  expect_copy("extra-blank", 200, "OK", "X-Trace: 1\r\n\r\n", "ab", 0, 0);
  expect_copy("extra-lf", 200, "OK", "X-Trace: 1\n", "ab", 0, 0);
  expect_copy("extra-ws", 200, "OK", "X-Trace : 1\r\n", "ab", 0, 0);
  expect_copy("extra-owned", 200, "OK",
              "Content-Length: 0\r\nConnection: close\r\nTransfer-Encoding: chunked\r\nX-Trace: 1\r\n",
              "ab", 0, 1);
  expect_copy("extra-good", 200, "OK", "X-Trace: 1\r\n", "ab", 0, 1);
  expect_copy("extra-bare", 200, "OK", "X-Trace: 1", "ab", 0, 1);
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
