/* Feed raw request bytes to the generated classifier.
   Pure views must allocate nothing and stay inside the buffer.
   classify allocates one Classified. Routes are not part of this book. */

#include "shim.h"
#include "request.h"

#include <stdio.h>
#include <string.h>

enum { ST_OK = 0, ST_BAD = 1, ST_NEED = 2, ST_UNIMPL = 3, K_YES = 0, K_NO = 1 };

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

static int is_true(Bool *b) { return b && b->muro__field_tag == 0; }

static void expect_chunk(const char *name, const char *raw, int done, int bad_frame, uint64_t total) {
  Ck ck;
  muro_http_reset();
  ck = chunkScan(of(raw));
  if (done != is_true(ckDone(ck))) bad(name, "done");
  if (bad_frame != is_true(ckBad(ck))) bad(name, "bad");
  if (ckTotal(ck) != total) bad(name, "total");
  if (muro_http_allocs() != 0) bad(name, "alloc");
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
  classify_of("enclose", "GET /health HTTP/1.1\r\nHost: t\r\nConnection: enclose\r\n\r\n", ST_OK, 1,
              K_YES);
  classify_of("close-foo", "GET /health HTTP/1.1\r\nHost: t\r\nConnection: close, foo\r\n\r\n",
              ST_OK, 1, K_NO);
  classify_of("closeable", "GET /health HTTP/1.1\r\nHost: t\r\nConnection: closeable\r\n\r\n",
              ST_OK, 1, K_YES);
  classify_of("keep-alive-10", "GET /health HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", ST_OK, 1,
              K_YES);
  classify_of("close-then-alive",
              "GET /health HTTP/1.1\r\nHost: t\r\nConnection: close\r\nConnection: keep-alive\r\n\r\n",
              ST_OK, 1, K_NO);
  classify_of("alive-then-close",
              "GET /health HTTP/1.1\r\nHost: t\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n",
              ST_OK, 1, K_NO);
  classify_of("close-ws", "GET /health HTTP/1.1\r\nHost: t\r\nConnection:  close \r\n\r\n", ST_OK, 1,
              K_NO);
  classify_of("empty-target", "GET  HTTP/1.1\r\nHost: t\r\n\r\n", ST_BAD, 0, 0);
  classify_of("bare-lf", "GET /health HTTP/1.1\nHost: t\n\n", ST_BAD, 0, 0);
  classify_of("bare-lf-one", "GET /health HTTP/1.1\n", ST_BAD, 0, 0);
  classify_of("cl-plus", "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: +1\r\n\r\n", ST_BAD, 0, 0);
  classify_of("te", "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n", ST_NEED, 0,
              0);
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

  {
    const char *hi =
        "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n";
    const char *ext =
        "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n5;foo=1\r\nhello\r\n0\r\n\r\n";
    const char *trail =
        "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\nX-A: b\r\n\r\n";
    const char *pipe =
        "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\nGET /x HTTP/1.1\r\n";
    char big[160];
    char cap[160];
    muro_http_reset();
    if (methodCode(of("HEAD /x HTTP/1.1\r\nHost: t\r\n\r\n")) != 0) bad("meth-head", "code");
    muro_http_reset();
    if (methodCode(of("PUT /x HTTP/1.1\r\nHost: t\r\n\r\n")) != 0) bad("meth-put", "code");
    muro_http_reset();
    if (methodCode(of("DELETE /x HTTP/1.1\r\nHost: t\r\n\r\n")) != 0) bad("meth", "delete");
    muro_http_reset();
    if (methodCode(of("OPTIONS /x HTTP/1.1\r\nHost: t\r\n\r\n")) != 0) bad("meth", "options");
    muro_http_reset();
    if (methodCode(of("PATCH /x HTTP/1.1\r\nHost: t\r\n\r\n")) != 0) bad("meth", "patch");
    muro_http_reset();
    if (methodCode(of("TRACE /x HTTP/1.1\r\nHost: t\r\n\r\n")) != 1) bad("meth-trace", "code");
    muro_http_reset();
    if (methodCode(of("CONNECT x HTTP/1.1\r\nHost: t\r\n\r\n")) != 1) bad("meth", "connect");
    muro_http_reset();
    if (methodCode(of("get /x HTTP/1.1\r\nHost: t\r\n\r\n")) != 1) bad("meth", "lower");
    muro_http_reset();
    if (methodCode(of("G(ET /x HTTP/1.1\r\nHost: t\r\n\r\n")) != 2) bad("meth-bad", "code");
    muro_http_reset();
    if (methodCode(of("OPTIONS * HTTP/1.1\r\nHost: t\r\n\r\n")) != 0) bad("meth-star", "code");
    muro_http_reset();
    if (methodCode(of("GET * HTTP/1.1\r\nHost: t\r\n\r\n")) != 2) bad("meth-get-star", "code");
    classify_of("opt-star", "OPTIONS * HTTP/1.1\r\nHost: t\r\n\r\n", ST_OK, 1, K_YES);
    classify_of("get-star", "GET * HTTP/1.1\r\nHost: t\r\n\r\n", ST_BAD, 0, 0);
    classify_of("meth-bad", "G(ET /x HTTP/1.1\r\nHost: t\r\n\r\n", ST_BAD, 0, 0);
    classify_of("trace-ok", "TRACE /x HTTP/1.1\r\nHost: t\r\n\r\n", ST_OK, 1, K_YES);
    classify_of("put-ok", "PUT /x HTTP/1.1\r\nHost: t\r\n\r\n", ST_OK, 1, K_YES);
    muro_http_reset();
    if (teCode(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n")) != 1)
      bad("te-chunk", "code");
    muro_http_reset();
    if (teCode(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: Chunked\r\n\r\n")) != 1)
      bad("te", "case");
    muro_http_reset();
    if (teCode(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip\r\n\r\n")) != 2)
      bad("te-gzip", "code");
    muro_http_reset();
    if (teCode(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked, gzip\r\n\r\n")) != 3)
      bad("te-not-final", "code");
    muro_http_reset();
    if (teCode(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip, chunked\r\n\r\n")) != 2)
      bad("te-gzip-code", "code");
    muro_http_reset();
    if (teCode(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked, chunked\r\n\r\n")) != 3)
      bad("te", "twice");
    muro_http_reset();
    if (teCode(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked;q=1\r\n\r\n")) != 3)
      bad("te", "param");
    classify_of("te-cl",
                "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nContent-Length: 1\r\n\r\n",
                ST_BAD, 0, 0);
    classify_of("cl-te",
                "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
                ST_BAD, 0, 0);
    classify_of("te-10", "POST /p HTTP/1.0\r\nTransfer-Encoding: chunked\r\n\r\n", ST_BAD, 0, 0);
    classify_of("te-two",
                "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
                "chunked\r\n\r\n",
                ST_BAD, 0, 0);
    /* Keep here is the Connection bit. No close token, so HTTP/1.1
       stays keep-yes. The image forces close for origin 2. That
       wire close is not this tag. */
    classify_of("te-gzip", "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip\r\n\r\n",
                ST_UNIMPL, 1, K_YES);
    classify_of("te-list", "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked, gzip\r\n\r\n",
                ST_BAD, 0, 0);
    classify_of("te-gzip-final",
                "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", ST_UNIMPL,
                1, K_YES);
    classify_of("te-hi", hi, ST_OK, 1, K_YES);
    muro_http_reset();
    if (bodyView(of(hi)).n != 0) bad("te-hi", "body view");
    muro_http_reset();
    if (expectCode(of("POST /p HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nContent-Length: "
                      "1\r\n\r\n")) != 1)
      bad("exp-100", "code");
    muro_http_reset();
    if (expectCode(of("POST /p HTTP/1.1\r\nHost: t\r\nExpect: 100-Continue\r\nContent-Length: "
                      "1\r\n\r\n")) != 1)
      bad("exp", "case");
    muro_http_reset();
    if (expectCode(of(
            "POST /p HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\nExpect: 100-continue\r\n\r\n")) !=
        2)
      bad("exp-two", "code");
    muro_http_reset();
    if (expectCode(of("POST /p HTTP/1.1\r\nHost: t\r\nExpect:\r\n\r\n")) != 2) bad("exp", "empty");
    expect_chunk("ck-empty",
                 "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 1, 0,
                 0);
    expect_chunk("ck-hi", hi, 1, 0, 2);
    expect_chunk("ck-ext", ext, 1, 0, 5);
    expect_chunk("ck-trail", trail, 1, 0, 2);
    expect_chunk("ck-bad-size",
                 "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\nz\r\n", 0, 1, 0);
    expect_chunk("ck-overflow",
                 "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "fffffffffffffffff\r\n",
                 0, 1, 0);
    expect_chunk("ck-nocolon",
                 "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nFoo\r\n\r\n",
                 0, 1, 0);
    classify_of("ck-bad-status",
                "POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\nz\r\n", ST_BAD, 0,
                0);
    muro_http_reset();
    if (framedEnd(of(hi)) != strlen(hi)) bad("frame", "chunk");
    muro_http_reset();
    if (framedEnd(of(pipe)) >= strlen(pipe)) bad("frame", "pipe");
    muro_http_reset();
    if (framedEnd(of(post)) != strlen(post)) bad("frame", "post");
    /* Header only. stream-cl and cap-cl are C checks in this file,
       not book equations. The skipped refls are stream-take-hi and
       stream-take-edge. */
    snprintf(big, sizeof big, "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 16385\r\n\r\n");
    classify_of("stream-cl", big, ST_OK, 1, K_YES);
    muro_http_reset();
    if (contentLength(of(big)) != 16385) bad("stream-cl", "length");
    muro_http_reset();
    if (streamTake(of(big)) != 1) bad("stream-take-hi", "take");
    snprintf(cap, sizeof cap, "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 16384\r\n\r\n");
    classify_of("cap-cl", cap, ST_NEED, 0, 0);
    muro_http_reset();
    if (streamTake(of(cap)) != 0) bad("stream-take-edge", "take");
    muro_http_reset();
    if (streamTake(of("POST /posts HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\n")) != 0)
      bad("post-take-short", "take");
    muro_http_reset();
    if (streamTake(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n")) != 0)
      bad("ck-take-hdr", "take");
    muro_http_reset();
    if (headerEnd(of("GET /health HTTP/1.1\nHost: t\n\n")) !=
        strlen("GET /health HTTP/1.1\nHost: t\n\n") + 1)
      bad("bare-lf", "header-end");
    muro_http_reset();
    if (is_true(headersDone(of("GET /health HTTP/1.1\nHost: t\n\n")))) bad("bare-lf", "headers-done");
    muro_http_reset();
    if (streamTake(of("POST /p HTTP/1.1\nHost: t\nContent-Length: 16385\n\n")) != 0)
      bad("stream-take-lf", "take");
    /* Same inputs as the ExUnit-skipped U64 refls. ExUnit drops them
       because the normal form is a machine term. These compare the
       numbers the refls name. */
    muro_http_reset();
    if (crlfEnd(of("\r\n\r\n")) != 4) bad("crlf-end", "off");
    muro_http_reset();
    if (lfEnd(of("\n\n")) != 2) bad("lf-end", "off");
    muro_http_reset();
    if (headerEnd(of("GET /health HTTP/1.1\r\n\r\n")) != 24) bad("hdr-prefers-crlf", "off");
    muro_http_reset();
    if (headerCount(of("GET /health HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")) != 2)
      bad("header-count", "n");
    muro_http_reset();
    if (!same(headerName(of("GET /health HTTP/1.1\r\nHost: t\r\n\r\n"), 0), "Host"))
      bad("header-name-0", "name");
    muro_http_reset();
    if (framedEnd(of("POST /posts HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\nab")) != 54)
      bad("post-framed", "end");
    muro_http_reset();
    if (framedEnd(of("POST /posts HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\n")) != 52)
      bad("post-framed-short", "end");
    muro_http_reset();
    if (framedEnd(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n")) !=
        62)
      bad("ck-framed", "end");
    muro_http_reset();
    if (framedEnd(of("POST /p HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n")) != 57)
      bad("ck-framed-hdr", "end");
    muro_http_reset();
    if (framedEnd(of(big)) != 52) bad("stream-end-hi", "end");
    muro_http_reset();
    if (framedEnd(of(cap)) != 52) bad("stream-end-edge", "end");
    classify_of("cl-overflow",
                "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 99999999999999999999\r\n\r\n",
                ST_BAD, 0, 0);
    classify_of("cl-max", "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 4294967295\r\n\r\n",
                ST_OK, 1, K_YES);
    muro_http_reset();
    if (contentLength(of("POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 4294967295\r\n\r\n")) !=
        4294967295ull)
      bad("cl-max", "value");
    classify_of("cl-bound", "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 4294967296\r\n\r\n",
                ST_BAD, 0, 0);
    classify_of("cl-wide", "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: 8589934592\r\n\r\n",
                ST_BAD, 0, 0);
    classify_of("cl-long",
                "POST /p HTTP/1.1\r\nHost: t\r\nContent-Length: "
                "0000000000000000000000000000000000000000\r\n\r\n",
                ST_BAD, 0, 0);
  }

  if (failures != 0) {
    fprintf(stderr, "FAIL %d\n", failures);
    return 1;
  }
  printf("health-allocs %lu\n", health_allocs);
  printf("post-allocs %lu\n", post_allocs);
  printf("OK\n");
  return 0;
}
