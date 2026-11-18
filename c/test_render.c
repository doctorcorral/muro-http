/* Check the emitted response bytes. Eight equations in render.muro
   are reduction evidence (refl). This driver checks those strings,
   plus a two-byte row and a ten-byte row. Those two reductions do
   not finish. It also checks Content-Length for every tag
   combination. The _Static_asserts below only pin decision_tags.h
   to itself. emitted_tags_match calls the generated *From helpers. */

#include "shim.h"
#include "render.h"
#include "decision_tags.h"
#include "tag_pin.h"

#include <stdio.h>
#include <string.h>

/* Header against itself. Not a check of the emitted constructors. */
_Static_assert(ST_200 == 0 && ST_201 == 1 && ST_400 == 2 && ST_401 == 3, "status tags");
_Static_assert(ST_404 == 4 && ST_500 == 5 && ST_503 == 6 && ST_OBSERVE == 7, "status tags");
_Static_assert(ERR_NONE == 0 && ERR_NOT_FOUND == 1 && ERR_MALFORMED == 7, "err tags");
_Static_assert(ERR_DB == 11 && ERR_OBSERVE == 12 && ERR_COUNT == 13, "err tags");
_Static_assert(ACT_HEALTH == 0 && ACT_ERROR == 6 && ACT_OBSERVE == 7 && ACT_COUNT == 8, "act tags");

static int failures;

static void bad(const char *what) {
  fprintf(stderr, "FAIL %s\n", what);
  failures += 1;
}

static int starts(const muro__slice s, const char *lit) {
  size_t n = strlen(lit);
  return s.n >= n && memcmp(s.p, lit, n) == 0;
}

static int contains(const muro__slice s, const char *lit) {
  size_t n = strlen(lit);
  size_t i;
  if (n > s.n) return 0;
  for (i = 0; i + n <= s.n; i++) {
    if (memcmp(s.p + i, lit, n) == 0) return 1;
  }
  return 0;
}

static int content_length(const muro__slice s, size_t *cl, size_t *body) {
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

static void expect_eq(const char *name, uint64_t code, uint64_t err, uint64_t act, uint64_t keep,
                      const uint8_t *row, size_t row_n, const char *lit) {
  muro__slice s = httpRender(code, err, act, keep, muro__slice_of(row, row_n));
  size_t n = strlen(lit);
  if (s.n != n || memcmp(s.p, lit, n) != 0) {
    fprintf(stderr, "FAIL %s: got %zu bytes\n", name, s.n);
    failures += 1;
  }
  muro_http_reset();
}

static const char *line_of(unsigned code) {
  switch (code) {
  case 0: return "HTTP/1.1 200 OK\r\n";
  case 1: return "HTTP/1.1 201 Created\r\n";
  case 2: return "HTTP/1.1 400 Bad Request\r\n";
  case 3: return "HTTP/1.1 401 Unauthorized\r\n";
  case 4: return "HTTP/1.1 404 Not Found\r\n";
  case 5: return "HTTP/1.1 500 Internal Server Error\r\n";
  case 6: return "HTTP/1.1 503 Service Unavailable\r\n";
  default: return "HTTP/1.1 500 Internal Server Error\r\n";
  }
}

int main(void) {
  unsigned code, err, act, keep;
  if (!emitted_tags_match()) return 1;
  expect_eq("health-keep", 0, 0, 0, 0, NULL, 0,
            "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\nok");
  expect_eq("health-close", 0, 0, 0, 1, NULL, 0,
            "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 2\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\nok");
  expect_eq("malformed", 2, 7, 6, 1, NULL, 0,
            "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 9\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\nmalformed");
  expect_eq("missing-bearer", 3, 4, 6, 0, NULL, 0,
            "HTTP/1.1 401 Unauthorized\r\nConnection: keep-alive\r\nContent-Length: 14\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\nmissing-bearer");
  expect_eq("not-found", 4, 1, 6, 0, NULL, 0,
            "HTTP/1.1 404 Not Found\r\nConnection: keep-alive\r\nContent-Length: 9\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\nnot-found");
  expect_eq("db", 6, 11, 6, 1, NULL, 0,
            "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 2\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\ndb");
  expect_eq("empty-row", 0, 0, 1, 0, NULL, 0,
            "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 0\r\n"
            "Content-Type: application/json; charset=utf-8\r\n\r\n");
  expect_eq("feed-empty", 0, 0, 1, 0, (const uint8_t *)"[]", 2,
            "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n"
            "Content-Type: application/json; charset=utf-8\r\n\r\n[]");
  expect_eq("row-ten", 0, 0, 2, 0, (const uint8_t *)"1234567890", 10,
            "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 10\r\n"
            "Content-Type: application/json; charset=utf-8\r\n\r\n1234567890");
  expect_eq("code-out-of-range", 7, 0, 0, 0, NULL, 0,
            "HTTP/1.1 500 Internal Server Error\r\nConnection: keep-alive\r\nContent-Length: 2\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n\r\nok");
  for (code = 0; code < 8; code++) {
    for (err = 0; err < 13; err++) {
      for (act = 0; act < 8; act++) {
        for (keep = 0; keep < 2; keep++) {
          muro__slice s = httpRender(code, err, act, keep, muro__slice_empty());
          size_t cl = 0;
          size_t body = 0;
          if (!starts(s, line_of(code))) bad("status line");
          if (!contains(s, keep ? "Connection: close\r\n" : "Connection: keep-alive\r\n"))
            bad("connection");
          if (!content_length(s, &cl, &body) || cl != body) bad("content-length");
          muro_http_reset();
        }
      }
    }
  }
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
