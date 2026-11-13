/* Feed raw request bytes to the generated classifier.
   Pure views must allocate nothing and stay inside the buffer.
   classify itself allocates on muro 0.20.36 (see GAPS.md). */

#include "shim.h"
#include "request.h"

#include <stdio.h>
#include <string.h>

/* Tags are the declaration order in c/generated/request.h. */
enum {
  ST_OK = 0,
  ST_BAD = 1,
  ST_NEED = 2,
  V_GET = 0,
  V_POST = 1,
  V_OTHER = 2,
  P_HEALTH = 0,
  P_FEED = 1,
  P_POSTS = 2,
  P_ID = 3,
  P_LIKE = 4,
  P_OTHER = 5,
  I_NONE = 0,
  I_BAD = 1,
  I_UNK = 2,
  K_YES = 0,
  K_NO = 1,
  A_ABS = 0,
  A_PRES = 1,
  B_NONE = 0
};

enum { SL_STATIC = 0, SL_EMPTY = 1, SL_BYTES = 2 };

static int failures;
static unsigned long view_allocs;

static void bad(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s: %s\n", name, what);
  failures += 1;
}

static muro__slice of(const char *lit) {
  return muro__slice_of((const uint8_t *)lit, strlen(lit));
}

static int same(muro__slice s, const char *lit) {
  size_t n = strlen(lit);
  if (s.n != n) return 0;
  if (n == 0) return 1;
  return memcmp(s.p, lit, n) == 0;
}

/* Equality, not ordering: the out-of-range empty lives in the generated
   file's static array, a different object from this file's copy. */
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

static int static_empty(muro__slice s, muro__slice buf) {
  return s.n == 0 && !points_at(s.p, buf);
}

static void count_views(const char *name, muro__slice s) {
  unsigned long before = view_allocs;
  muro_http_reset();
  (void)methodView(s);
  view_allocs += muro_http_allocs();
  muro_http_reset();
  (void)targetView(s);
  view_allocs += muro_http_allocs();
  muro_http_reset();
  (void)pathView(s);
  view_allocs += muro_http_allocs();
  muro_http_reset();
  (void)versionView(s);
  view_allocs += muro_http_allocs();
  if (view_allocs != before) bad(name, "pure view allocated");
}

static const uint8_t *shared_empty;

static void note_empty(const char *name, const char *what, muro__slice v) {
  if (shared_empty == 0) shared_empty = v.p;
  if (v.p != shared_empty) bad(name, what);
}

static void check_slice(const char *name, const char *what, muro__slice v,
                        muro__slice buf, int kind, const char *bytes) {
  if (kind == SL_STATIC) {
    if (!static_empty(v, buf)) bad(name, what);
    note_empty(name, what, v);
    return;
  }
  if (static_empty(v, buf) || !inside(v, buf)) bad(name, what);
  if (kind == SL_EMPTY) {
    if (v.n != 0) bad(name, what);
    return;
  }
  if (!same(v, bytes)) bad(name, what);
}

static unsigned long expect(const char *name, const char *raw, uint8_t st,
                            uint8_t verb, uint8_t path, uint8_t idc, uint8_t keep,
                            uint8_t auth, int64_t idn, uint64_t cl, const char *method,
                            const char *pathb, int auth_kind, const char *authb,
                            int body_kind, const char *bodyb) {
  muro__slice s = of(raw);
  unsigned long nalloc;
  Classified *c;
  count_views(name, s);
  muro_http_reset();
  c = classify(s);
  nalloc = muro_http_allocs();
  if (c == 0 || c->muro__field_tag != 0) {
    bad(name, "classified");
    return nalloc;
  }
  if (c->muro__as.classified.muro__f_0->muro__field_tag != st) bad(name, "status");
  if (c->muro__as.classified.muro__f_1->muro__field_tag != verb) bad(name, "verb");
  if (c->muro__as.classified.muro__f_2->muro__field_tag != path) bad(name, "path");
  if (c->muro__as.classified.muro__f_3->muro__field_tag != idc) bad(name, "id");
  if (c->muro__as.classified.muro__f_4->muro__field_tag != keep) bad(name, "keep");
  if (c->muro__as.classified.muro__f_5->muro__field_tag != auth) bad(name, "auth");
  if (c->muro__as.classified.muro__f_6->muro__field_tag != B_NONE) bad(name, "body class");
  muro_http_reset();
  if (idNumber(s) != idn) bad(name, "id number");
  muro_http_reset();
  if (contentLength(s) != cl) bad(name, "content-length");
  muro_http_reset();
  check_slice(name, "method", methodView(s), s, SL_BYTES, method);
  muro_http_reset();
  check_slice(name, "path", pathView(s), s, pathb[0] == 0 ? SL_EMPTY : SL_BYTES, pathb);
  muro_http_reset();
  check_slice(name, "auth value", authValue(s), s, auth_kind, authb);
  muro_http_reset();
  check_slice(name, "body", bodyView(s), s, body_kind, bodyb);
  return nalloc;
}

int main(void) {
  unsigned long health_allocs;
  unsigned long post_allocs;
  const char *glued = "GET/feed\r\n";
  muro__slice g;
  Classified *c;

  health_allocs = expect(
      "health", "GET /health HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_HEALTH, I_NONE, K_YES,
      A_ABS, 0, 0, "GET", "/health", SL_STATIC, "", SL_EMPTY, "");
  expect("feed", "GET /feed HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_FEED, I_NONE, K_YES, A_ABS,
         0, 0, "GET", "/feed", SL_STATIC, "", SL_EMPTY, "");
  expect("posts", "GET /posts HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_POSTS, I_NONE, K_YES, A_ABS,
         0, 0, "GET", "/posts", SL_STATIC, "", SL_EMPTY, "");
  expect("id", "GET /posts/42 HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_ID, I_UNK, K_YES, A_ABS, 42,
         0, "GET", "/posts/42", SL_STATIC, "", SL_EMPTY, "");
  expect("like", "POST /posts/42/like HTTP/1.1\r\n\r\n", ST_OK, V_POST, P_LIKE, I_UNK, K_YES,
         A_ABS, 42, 0, "POST", "/posts/42/like", SL_STATIC, "", SL_EMPTY, "");
  expect("id0", "GET /posts/0 HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_ID, I_BAD, K_YES, A_ABS, 0, 0,
         "GET", "/posts/0", SL_STATIC, "", SL_EMPTY, "");
  expect("idabc", "GET /posts/abc HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_ID, I_BAD, K_YES, A_ABS, 0,
         0, "GET", "/posts/abc", SL_STATIC, "", SL_EMPTY, "");
  expect("idlead", "GET /posts/01 HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_ID, I_BAD, K_YES, A_ABS, 0,
         0, "GET", "/posts/01", SL_STATIC, "", SL_EMPTY, "");
  expect("query", "GET /feed?x=1 HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_FEED, I_NONE, K_YES, A_ABS,
         0, 0, "GET", "/feed", SL_STATIC, "", SL_EMPTY, "");
  expect("http10", "GET /health HTTP/1.0\r\n\r\n", ST_OK, V_GET, P_HEALTH, I_NONE, K_NO, A_ABS,
         0, 0, "GET", "/health", SL_STATIC, "", SL_EMPTY, "");
  expect("close", "GET /health HTTP/1.1\r\nConnection: close\r\n\r\n", ST_OK, V_GET, P_HEALTH,
         I_NONE, K_NO, A_ABS, 0, 0, "GET", "/health", SL_STATIC, "", SL_EMPTY, "");
  expect("te", "GET /health HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n", ST_BAD, V_GET,
         P_HEALTH, I_NONE, K_YES, A_ABS, 0, 0, "GET", "/health", SL_STATIC, "", SL_EMPTY, "");
  expect("glued-line", "GET/feed HTTP/1.1\r\n\r\n", ST_BAD, V_OTHER, P_OTHER, I_NONE, K_YES,
         A_ABS, 0, 0, "GET/feed", "HTTP/1.1\r", SL_STATIC, "", SL_EMPTY, "");
  expect("auth", "GET /feed HTTP/1.1\r\nAuthorization: Bearer z\r\n\r\n", ST_OK, V_GET, P_FEED,
         I_NONE, K_YES, A_PRES, 0, 0, "GET", "/feed", SL_BYTES, "Bearer z", SL_EMPTY, "");
  expect("need", "POST /posts HTTP/1.1\r\nContent-Length: 2\r\n\r\n", ST_NEED, V_POST, P_POSTS,
         I_NONE, K_YES, A_ABS, 0, 2, "POST", "/posts", SL_STATIC, "", SL_EMPTY, "");
  post_allocs = expect("post", "POST /posts HTTP/1.1\r\nContent-Length: 2\r\n\r\nab", ST_OK,
                       V_POST, P_POSTS, I_NONE, K_YES, A_ABS, 0, 2, "POST", "/posts", SL_STATIC,
                       "", SL_BYTES, "ab");
  expect("partial", "GET /health HTTP/1.1\r\n", ST_NEED, V_GET, P_HEALTH, I_NONE, K_YES, A_ABS,
         0, 0, "GET", "/health", SL_STATIC, "", SL_STATIC, "");
  expect("gap", "GET  HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_OTHER, I_NONE, K_YES, A_ABS, 0, 0, "GET",
         "", SL_STATIC, "", SL_EMPTY, "");
  expect("tail", "GET /posts/42/likes HTTP/1.1\r\n\r\n", ST_OK, V_GET, P_OTHER, I_NONE, K_YES,
         A_ABS, 0, 0, "GET", "/posts/42/likes", SL_STATIC, "", SL_EMPTY, "");
  expect("multi",
         "POST /posts/42 HTTP/1.1\r\nHost: example\r\nAuthorization: Bearer z\r\n"
         "Content-Length: 2\r\nConnection: close\r\n\r\nab",
         ST_OK, V_POST, P_ID, I_UNK, K_NO, A_PRES, 42, 2, "POST", "/posts/42", SL_BYTES,
         "Bearer z", SL_BYTES, "ab");

  g = of(glued);
  count_views("no-space", g);
  muro_http_reset();
  if (!same(methodView(g), "GET/feed\r") || !inside(methodView(g), g))
    bad("no-space", "method");
  muro_http_reset();
  {
    muro__slice p = pathView(g);
    if (!static_empty(p, g)) bad("no-space", "path");
    note_empty("no-space", "path", p);
  }
  muro_http_reset();
  c = classify(g);
  if (c->muro__as.classified.muro__f_0->muro__field_tag != ST_NEED) bad("no-space", "status");
  if (c->muro__as.classified.muro__f_1->muro__field_tag != V_OTHER) bad("no-space", "verb");

  muro_http_reset();
  if (!same(versionView(of("GET /health HTTP/1.1\r\n")), "HTTP/1.1"))
    bad("version", "bytes");
  if (muro_http_allocs() != 0) bad("version", "alloc");

  if (failures != 0) {
    fprintf(stderr, "FAIL %d\n", failures);
    return 1;
  }
  printf("view-allocs %lu\n", view_allocs);
  printf("health-allocs %lu\n", health_allocs);
  printf("post-allocs %lu\n", post_allocs);
  printf("OK\n");
  return 0;
}
