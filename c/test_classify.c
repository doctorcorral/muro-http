/* Feed raw request bytes to the generated classifier.
   Pure views must allocate nothing and stay inside the buffer.
   classify allocates one Classified. Routes are not part of this book. */

#include "shim.h"
#include "request.h"

#include <stdio.h>
#include <string.h>

enum { ST_OK = 0, ST_BAD = 1, ST_NEED = 2, K_YES = 0, K_NO = 1 };

static int failures;

static void bad(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s: %s\n", name, what);
  failures += 1;
}

static muro__slice of(const char *lit) { return muro__slice_of((const uint8_t *)lit, strlen(lit)); }

static int same(muro__slice s, const char *lit) {
  size_t n = strlen(lit);
  if (s.n != n) return 0;
  if (n == 0) return 1;
  return memcmp(s.p, lit, n) == 0;
}

static int points_at(const uint8_t *p, muro__slice buf) {
  size_t i;
  for (i = 0; i <= buf.n; i++) {
    if (p == buf.p + i) return 1;
  }
  return 0;
}

static int inside(muro__slice v, muro__slice buf) {
  if (v.n > buf.n) return 0;
  if (!points_at(v.p, buf)) return 0;
  return (size_t)(v.p - buf.p) + v.n <= buf.n;
}

static void views_alloc0(const char *name, muro__slice s) {
  muro_http_reset();
  (void)methodView(s);
  (void)targetView(s);
  (void)pathView(s);
  (void)queryView(s);
  (void)versionView(s);
  (void)bodyView(s);
  if (muro_http_allocs() != 0) bad(name, "pure view allocated");
}

static unsigned long classify_of(const char *name, const char *raw, uint8_t st, int check_keep,
                                uint8_t keep) {
  muro__slice s = of(raw);
  Classified *c;
  unsigned long n;
  views_alloc0(name, s);
  muro_http_reset();
  c = classify(s);
  n = muro_http_allocs();
  if (!c || c->muro__field_tag != 0) {
    bad(name, "classified");
    return n;
  }
  if (c->muro__as.classified.muro__f_0->muro__field_tag != st) bad(name, "status");
  if (check_keep && c->muro__as.classified.muro__f_1->muro__field_tag != keep) bad(name, "keep");
  return n;
}

static void expect_view(const char *name, const char *raw, const char *method, const char *path,
                        const char *query, const char *version, const char *body, int body_empty) {
  muro__slice s = of(raw);
  muro__slice q;
  muro_http_reset();
  if (!same(methodView(s), method) || !inside(methodView(s), s)) bad(name, "method");
  muro_http_reset();
  if (!same(pathView(s), path) || !inside(pathView(s), s)) bad(name, "path");
  muro_http_reset();
  q = queryView(s);
  if (query[0] == 0) {
    if (q.n != 0) bad(name, "query");
  } else if (!same(q, query) || !inside(q, s)) {
    bad(name, "query");
  }
  muro_http_reset();
  if (!same(versionView(s), version) || !inside(versionView(s), s)) bad(name, "version");
  muro_http_reset();
  if (body_empty) {
    if (bodyView(s).n != 0) bad(name, "body");
  } else if (!same(bodyView(s), body) || !inside(bodyView(s), s)) {
    bad(name, "body");
  }
}

static void expect_header(const char *name, const char *raw) {
  muro__slice s = of(raw);
  muro__slice miss;
  muro__slice empty;
  muro__slice host;
  muro_http_reset();
  if (headerCount(s) != 3 || muro_http_allocs() != 0) bad(name, "count");
  muro_http_reset();
  if (!same(headerName(s, 0), "Host") || !inside(headerName(s, 0), s)) bad(name, "name");
  muro_http_reset();
  host = findHeader(s, of("host"));
  if (!same(host, "t") || !inside(host, s) || muro_http_allocs() != 0) bad(name, "find host");
  muro_http_reset();
  empty = findHeader(s, of("x-empty"));
  if (empty.n != 0 || !points_at(empty.p, s)) bad(name, "empty value");
  muro_http_reset();
  miss = findHeader(s, of("zzz"));
  if (miss.n != 0 || points_at(miss.p, s)) bad(name, "missing header");
}

int main(void) {
  unsigned long health_allocs;
  unsigned long post_allocs;
  const char *health = "GET /health HTTP/1.1\r\nHost: t\r\n\r\n";
  const char *post = "POST /posts HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\nab";
  const char *headers =
      "GET /health HTTP/1.1\r\nHost: t\r\nX-Empty:\r\nConnection: close\r\n\r\n";
  const char *glued = "GET/feed\r\n";
  muro__slice g;
  Classified *c;

  health_allocs = classify_of("health", health, ST_OK, 1, K_YES);
  if (health_allocs != 1) bad("health", "allocs");
  expect_view("health", health, "GET", "/health", "", "HTTP/1.1", "", 1);
  muro_http_reset();
  if (contentLength(of(health)) != 0) bad("health", "content-length");

  classify_of("host-missing", "GET /health HTTP/1.1\r\n\r\n", ST_BAD, 0, 0);
  classify_of("host-dup", "GET /health HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", ST_BAD, 0, 0);
  classify_of("http10", "GET /health HTTP/1.0\r\n\r\n", ST_OK, 1, K_NO);
  classify_of("http10-dup", "GET /health HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n", ST_BAD, 0, 0);
  classify_of("close", "GET /health HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n", ST_OK, 1,
              K_NO);
  classify_of("te", "GET /health HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n", ST_BAD,
              0, 0);
  classify_of("glued-line", "GET/feed HTTP/1.1\r\n\r\n", ST_BAD, 0, 0);
  classify_of("partial", "GET /health HTTP/1.1\r\n", ST_NEED, 0, 0);
  classify_of("need-body", "POST /posts HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\n", ST_NEED,
              0, 0);
  post_allocs = classify_of("post", post, ST_OK, 1, K_YES);
  if (post_allocs != 1) bad("post", "allocs");
  expect_view("post", post, "POST", "/posts", "", "HTTP/1.1", "ab", 0);
  muro_http_reset();
  if (contentLength(of(post)) != 2) bad("post", "content-length");
  {
    const char *spaced = "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length : 2\r\n\r\nab";
    const char *tabbed = "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length\t: 2\r\n\r\nab";
    const char *lead = "POST /p HTTP/1.1\r\nHost: t\r\n Content-Length: 2\r\n\r\nab";
    const char *ows = "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length:  2\r\n\r\nab";
    muro__slice sp = of(spaced);
    muro__slice cl;
    classify_of("cl-ws", spaced, ST_BAD, 0, 0);
    muro_http_reset();
    if (contentLength(sp) != 0) bad("cl-ws", "content-length");
    muro_http_reset();
    if (bodyView(sp).n != 0) bad("cl-ws", "body");
    muro_http_reset();
    cl = findHeader(sp, of("content-length"));
    if (cl.n != 0 || points_at(cl.p, sp)) bad("cl-ws", "find");
    classify_of("cl-tab", tabbed, ST_BAD, 0, 0);
    classify_of("cl-lead", lead, ST_BAD, 0, 0);
    classify_of("cl-ows", ows, ST_OK, 1, K_YES);
    muro_http_reset();
    if (contentLength(of(ows)) != 2) bad("cl-ows", "content-length");
    muro_http_reset();
    if (!same(bodyView(of(ows)), "ab")) bad("cl-ows", "body");
  }
  expect_view("query", "GET /feed?x=1 HTTP/1.1\r\nHost: t\r\n\r\n", "GET", "/feed", "x=1",
              "HTTP/1.1", "", 1);
  classify_of("query", "GET /feed?x=1 HTTP/1.1\r\nHost: t\r\n\r\n", ST_OK, 1, K_YES);
  expect_header("headers", headers);

  g = of(glued);
  muro_http_reset();
  c = classify(g);
  if (c->muro__as.classified.muro__f_0->muro__field_tag != ST_NEED) bad("no-space", "status");
  if (c->muro__as.classified.muro__f_1->muro__field_tag != K_YES) bad("no-space", "keep");
  muro_http_reset();
  if (!same(methodView(g), "GET/feed\r") || !inside(methodView(g), g)) bad("no-space", "method");

  if (failures != 0) {
    fprintf(stderr, "FAIL %d\n", failures);
    return 1;
  }
  printf("health-allocs %lu\n", health_allocs);
  printf("post-allocs %lu\n", post_allocs);
  printf("OK\n");
  return 0;
}
