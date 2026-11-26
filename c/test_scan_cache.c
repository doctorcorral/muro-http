/* Cached scan, crlfEnd, and classify must match the uncached book
   on the fuzz corpus and on the conformance request strings.
   test_classify links the book without the cache. The server and
   test_conn link c/scan_cache.c. */
#include "shim.h"
#include "conn.h"
#include "proto.h"
#include "admit_glue.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

HSt scan_uncached(muro__slice s);
uint64_t crlfEnd_uncached(muro__slice s);
uint64_t methodCode_uncached(muro__slice s);
uint64_t teCode_uncached(muro__slice s);
uint64_t framedEnd_uncached(muro__slice s);
uint64_t streamTake_uncached(muro__slice s);
muro__slice bodyView_uncached(muro__slice s);
Conn step_uncached(Conn st, Ev ev);
void step_memo_reset(void);
void view_memo_reset(void);
void admit_memo_reset(void);
unsigned admit_memo_misses(void);
void sess_memo_reset(void);
unsigned sess_memo_misses(void);

static int failures;
static int compared;

static void bad(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s: %s\n", name, what);
  failures += 1;
}

static int hst_eq(HSt a, HSt b) {
  int i;
  const uint64_t *ap;
  const uint64_t *bp;
  if (a.muro__field_tag != b.muro__field_tag) return 0;
  ap = &a.muro__as.hst.muro__f_0;
  bp = &b.muro__as.hst.muro__f_0;
  for (i = 0; i < 9; i++) {
    if (ap[i] != bp[i]) return 0;
  }
  return 1;
}

static int check_views(const char *name, muro__slice s);
static int check_steps(const char *name, const uint8_t *p, size_t n);

static int class_eq(Classified *a, Classified *b) {
  if (!a || !b) return 0;
  if (a->muro__field_tag != b->muro__field_tag) return 0;
  if (a->muro__as.classified.muro__f_0 != b->muro__as.classified.muro__f_0)
    return 0;
  if (a->muro__as.classified.muro__f_1 != b->muro__as.classified.muro__f_1)
    return 0;
  return 1;
}

static int field_u64(const char *name, const char *what, uint64_t u, uint64_t m, uint64_t h) {
  if (u != m || u != h) {
    bad(name, what);
    return 0;
  }
  return 1;
}

static int points_at(const uint8_t *p, muro__slice buf) {
  size_t i;
  if (!p || !buf.p) return 0;
  for (i = 0; i <= buf.n; i++) {
    if (p == buf.p + i) return 1;
  }
  return 0;
}

/* Uncached, miss, and hit must agree, and a view that the book places
   in the caller buffer must keep that offset. muro__slice_of(..., 0)
   would hide a hit that fell back to the empty sentinel. */
static int check_body(const char *name, muro__slice s, muro__slice u, muro__slice m,
                      muro__slice h) {
  if (u.n != m.n || u.n != h.n) {
    bad(name, "bodyView n");
    return 0;
  }
  if (points_at(u.p, s)) {
    size_t off = (size_t)(u.p - s.p);
    if (m.p != s.p + off || h.p != s.p + off) {
      bad(name, "bodyView off");
      return 0;
    }
    if (u.n > 0 && (memcmp(m.p, u.p, u.n) != 0 || memcmp(h.p, u.p, u.n) != 0)) {
      bad(name, "bodyView bytes");
      return 0;
    }
    return 1;
  }
  if (points_at(m.p, s) || points_at(h.p, s)) {
    bad(name, "bodyView sentinel");
    return 0;
  }
  return 1;
}

/* Same bytes in a second buffer must hit with an offset into that
   buffer, not a stale pointer into the first. */
static void check_body_rebind(const char *name, const char *raw) {
  size_t n = strlen(raw);
  uint8_t *a;
  uint8_t *b;
  muro__slice sa, sb, u, miss, hit, hit2;
  size_t off;
  a = malloc(n ? n : 1);
  b = malloc(n ? n : 1);
  if (!a || !b) {
    bad(name, "alloc");
    free(a);
    free(b);
    return;
  }
  if (n) {
    memcpy(a, raw, n);
    memcpy(b, raw, n);
  }
  sa = (muro__slice){a, n};
  sb = (muro__slice){b, n};
  view_memo_reset();
  muro_http_reset();
  u = bodyView_uncached(sa);
  miss = bodyView(sa);
  hit = bodyView(sa);
  if (!check_body(name, sa, u, miss, hit)) {
    free(a);
    free(b);
    return;
  }
  hit2 = bodyView(sb);
  if (hit2.n != u.n) {
    bad(name, "rebind n");
    free(a);
    free(b);
    return;
  }
  if (points_at(u.p, sa)) {
    off = (size_t)(u.p - sa.p);
    if (hit2.p != sb.p + off) bad(name, "rebind off");
    if (hit2.p == sa.p + off && a != b) bad(name, "rebind stale");
  }
  free(a);
  free(b);
}

/* Uncached, then a miss, then a hit. The slot is cold so the first
   memo call fills it. */
static int check_views(const char *name, muro__slice s) {
  uint64_t u, m, h;
  muro__slice bu, bm, bh;
  view_memo_reset();
  muro_http_reset();
  u = methodCode_uncached(s);
  muro_http_reset();
  m = methodCode(s);
  h = methodCode(s);
  if (!field_u64(name, "methodCode", u, m, h)) return 0;
  u = teCode_uncached(s);
  muro_http_reset();
  m = teCode(s);
  h = teCode(s);
  if (!field_u64(name, "teCode", u, m, h)) return 0;
  u = framedEnd_uncached(s);
  muro_http_reset();
  m = framedEnd(s);
  h = framedEnd(s);
  if (!field_u64(name, "framedEnd", u, m, h)) return 0;
  u = streamTake_uncached(s);
  muro_http_reset();
  m = streamTake(s);
  h = streamTake(s);
  if (!field_u64(name, "streamTake", u, m, h)) return 0;
  muro_http_reset();
  bu = bodyView_uncached(s);
  muro_http_reset();
  bm = bodyView(s);
  bh = bodyView(s);
  if (!check_body(name, s, bu, bm, bh)) return 0;
  return 1;
}

static int conn_eq(Conn a, Conn b) {
  int i;
  if (a.muro__field_tag != b.muro__field_tag) return 0;
  for (i = 0; i < 13; i++) {
    uint64_t av;
    uint64_t bv;
    switch (i) {
    case 0: av = a.muro__as.conn.muro__f_0; bv = b.muro__as.conn.muro__f_0; break;
    case 1: av = a.muro__as.conn.muro__f_1; bv = b.muro__as.conn.muro__f_1; break;
    case 2: av = a.muro__as.conn.muro__f_2; bv = b.muro__as.conn.muro__f_2; break;
    case 3: av = a.muro__as.conn.muro__f_3; bv = b.muro__as.conn.muro__f_3; break;
    case 4: av = a.muro__as.conn.muro__f_4; bv = b.muro__as.conn.muro__f_4; break;
    case 5: av = a.muro__as.conn.muro__f_5; bv = b.muro__as.conn.muro__f_5; break;
    case 6: av = a.muro__as.conn.muro__f_6; bv = b.muro__as.conn.muro__f_6; break;
    case 7: av = a.muro__as.conn.muro__f_7; bv = b.muro__as.conn.muro__f_7; break;
    case 8: av = a.muro__as.conn.muro__f_8; bv = b.muro__as.conn.muro__f_8; break;
    case 9: av = a.muro__as.conn.muro__f_9; bv = b.muro__as.conn.muro__f_9; break;
    case 10: av = a.muro__as.conn.muro__f_10; bv = b.muro__as.conn.muro__f_10; break;
    case 11: av = a.muro__as.conn.muro__f_11; bv = b.muro__as.conn.muro__f_11; break;
    default: av = a.muro__as.conn.muro__f_12; bv = b.muro__as.conn.muro__f_12; break;
    }
    if (av != bv) return 0;
  }
  return 1;
}

/* step_uncached, then a miss, then a hit. The returned state is the
   uncached one, so the next event starts from the book. */
static Conn check_step_ev(const char *name, Conn st, Ev ev, int *ok) {
  Conn u, m, h;
  muro_http_reset();
  u = step_uncached(st, ev);
  muro_http_reset();
  m = step(st, ev);
  h = step(st, ev);
  if (!conn_eq(u, m) || !conn_eq(u, h)) {
    bad(name, "step");
    *ok = 0;
  }
  return u;
}

static int check_steps(const char *name, const uint8_t *p, size_t n) {
  muro__slice s = muro__slice_of(p, n);
  Conn st = begin();
  int ok = 1;
  step_memo_reset();
  st = check_step_ev(name, st, evBytes(s), &ok);
  st = check_step_ev(name, st, evEof(s), &ok);
  (void)st;
  return ok;
}

static void check_step_walk(const char *name, const char *raw) {
  muro__slice s = muro__slice_of((const uint8_t *)raw, strlen(raw));
  Conn st = begin();
  int ok = 1;
  step_memo_reset();
  st = check_step_ev(name, st, evBytes(s), &ok);
  st = check_step_ev(name, st, evReply(0), &ok);
  st = check_step_ev(name, st, evProgress(4), &ok);
  st = check_step_ev(name, st, evDone(), &ok);
  st = check_step_ev(name, st, evTick(), &ok);
  st = check_step_ev(name, st, evError(), &ok);
  st = check_step_ev(name, st, evReplyOf(1, 1), &ok);
  (void)st;
  (void)ok;
}

static int move_eq(AdmitMove a, AdmitMove b) {
  if (a.dir != b.dir || a.tag != b.tag || a.bit != b.bit) return 0;
  if (a.ctor == b.ctor) return 1;
  if (!a.ctor || !b.ctor) return 0;
  return strcmp(a.ctor, b.ctor) == 0;
}

static int offer_eq(AdmitOffer a, AdmitOffer b) {
  int i;
  if (a.ok != b.ok || a.n != b.n || a.n < 0 || a.n > ADMIT_MOVES) return 0;
  for (i = 0; i < a.n; i++) {
    if (!move_eq(a.m[i], b.m[i])) return 0;
  }
  return 1;
}

static void check_admit_memo(void) {
  AdmitOffer keep, keep2, close, ok, ok2, rep_bad;
  unsigned misses;
  admit_memo_reset();
  keep = admit_offer(2, 3, 0, 0, 0, 0, 0, 0, 0, 5, 0, 1, 0);
  misses = admit_memo_misses();
  if (misses == 0) bad("admit", "keep stored nothing");
  keep2 = admit_offer(2, 3, 0, 0, 0, 0, 0, 0, 0, 5, 0, 1, 0);
  if (admit_memo_misses() != misses) bad("admit", "keep hit called the book");
  if (!offer_eq(keep, keep2)) bad("admit", "keep hit");
  if (!keep.ok || keep.n != 1 || keep.m[0].bit != 0) bad("admit", "keep bit");
  close = admit_offer(2, 3, 0, 0, 0, 0, 0, 0, 1, 5, 0, 1, 0);
  if (admit_memo_misses() != misses + 1) bad("admit", "close miss");
  if (offer_eq(keep, close) || !close.ok || close.n != 1 || close.m[0].bit != 1)
    bad("admit", "close bit");
  admit_memo_reset();
  ok = admit_offer(1, 6, 0, 0, 0, 1, 0, 0, 0, 4, 0, 1, 0);
  misses = admit_memo_misses();
  ok2 = admit_offer(1, 6, 0, 0, 0, 1, 0, 0, 0, 4, 0, 1, 0);
  if (admit_memo_misses() != misses) bad("admit", "reply hit called the book");
  if (!offer_eq(ok, ok2)) bad("admit", "reply hit");
  if (!ok.ok || ok.n != 1 || !ok.m[0].ctor || strcmp(ok.m[0].ctor, "reply-ok") != 0)
    bad("admit", "reply-ok");
  rep_bad = admit_offer(1, 6, 0, 0, 0, 1, 1, 0, 0, 4, 0, 1, 0);
  if (admit_memo_misses() != misses + 1) bad("admit", "rep miss");
  if (offer_eq(ok, rep_bad) || !rep_bad.ok || rep_bad.n != 1 || !rep_bad.m[0].ctor ||
      strcmp(rep_bad.m[0].ctor, "reply-bad") != 0)
    bad("admit", "reply-bad");
}

static int must_admit(const char *name, Proto *p, int phase, int ev, size_t n, int status,
                      int reply, int close_after) {
  char why[128];
  int rc;
  if (!p) {
    bad(name, "no session");
    return 0;
  }
  rc = proto_admit(p, phase, ev, n, status, reply, 0, close_after);
  if (rc != 1) {
    proto_where(p, why, sizeof why);
    fprintf(stderr, "FAIL %s: admit %d at %s\n", name, rc, why);
    failures += 1;
    return 0;
  }
  return 1;
}

static int proto_where_eq(Proto *a, Proto *b) {
  char wa[256];
  char wb[256];
  proto_where(a, wa, sizeof wa);
  proto_where(b, wb, sizeof wb);
  return strcmp(wa, wb) == 0;
}

static int proto_moves_eq(Proto *a, Proto *b) {
  int n;
  int i;
  n = proto_move_count(a);
  if (!b || n != proto_move_count(b)) return 0;
  for (i = 0; i < n; i++) {
    ProtoMove x, y;
    if (!proto_move_at(a, i, &x) || !proto_move_at(b, i, &y)) return 0;
    if (x.dir != y.dir || x.kind != y.kind || x.bit != y.bit) return 0;
    if (x.ctor == y.ctor) continue;
    if (!x.ctor || !y.ctor || strcmp(x.ctor, y.ctor) != 0) return 0;
  }
  return 1;
}

static int last_ctor_is(Proto *p, const char *ctor) {
  ProtoMove m;
  int n = proto_move_count(p);
  if (n < 1 || !proto_move_at(p, n - 1, &m) || !m.ctor) return 0;
  return strcmp(m.ctor, ctor) == 0;
}

static int last_bit_is(Proto *p, unsigned bit) {
  ProtoMove m;
  int n = proto_move_count(p);
  if (n < 1 || !proto_move_at(p, n - 1, &m) || m.kind != PROTO_PICK) return 0;
  return m.bit == bit;
}

static int admit_read(Proto *p) { return must_admit("sess-read", p, 0, 0, 24, 0, -1, 0); }

static int admit_reply(Proto *p, int bad_reply) {
  return must_admit(bad_reply ? "sess-bad" : "sess-ok", p, 1, 6, 0, -1, bad_reply ? 1 : 0, 0);
}

static void check_session_memo(void) {
  Proto *p1 = NULL;
  Proto *p2 = NULL;
  Proto *p3 = NULL;
  Proto *pbad = NULL;
  unsigned before;
  sess_memo_reset();
  p1 = proto_begin();
  p2 = proto_begin();
  pbad = proto_begin();
  p3 = proto_begin();
  if (!p1 || !p2 || !pbad || !p3) {
    bad("sess", "init");
    goto done;
  }
  before = sess_memo_misses();
  if (!admit_read(p1)) goto done;
  if (sess_memo_misses() == before) bad("sess", "read stored nothing");
  before = sess_memo_misses();
  if (!admit_read(p2)) goto done;
  if (sess_memo_misses() != before) bad("sess", "read hit called the book");
  if (!proto_moves_eq(p1, p2)) bad("sess", "read hit moves");
  if (!proto_where_eq(p1, p2)) bad("sess", "read hit where");
  if (!admit_read(pbad)) goto done;
  before = sess_memo_misses();
  if (!admit_reply(pbad, 1)) goto done;
  if (sess_memo_misses() == before) bad("sess", "reply-bad stored nothing");
  if (!last_ctor_is(pbad, "reply-bad")) bad("sess", "reply-bad ctor");
  before = sess_memo_misses();
  if (!admit_reply(p1, 0)) goto done;
  if (sess_memo_misses() == before) bad("sess", "reply-ok stored nothing");
  if (!last_ctor_is(p1, "reply-ok")) bad("sess", "reply-ok ctor");
  before = sess_memo_misses();
  if (!admit_reply(p2, 0)) goto done;
  if (sess_memo_misses() != before) bad("sess", "reply hit called the book");
  if (!proto_moves_eq(p1, p2)) bad("sess", "reply hit moves");
  if (!proto_where_eq(p1, p2)) bad("sess", "reply hit where");
  before = sess_memo_misses();
  if (!must_admit("sess-keep", p1, 2, 3, 0, -1, -1, 0)) goto done;
  if (sess_memo_misses() == before) bad("sess", "keep stored nothing");
  if (!last_bit_is(p1, 0) || proto_done(p1)) bad("sess", "keep bit");
  before = sess_memo_misses();
  if (!must_admit("sess-keep-hit", p2, 2, 3, 0, -1, -1, 0)) goto done;
  if (sess_memo_misses() != before) bad("sess", "keep hit called the book");
  if (!last_bit_is(p2, 0) || proto_done(p2)) bad("sess", "keep hit bit");
  if (!proto_where_eq(p1, p2)) bad("sess", "keep hit where");
  if (!admit_read(p3) || !admit_reply(p3, 0)) goto done;
  before = sess_memo_misses();
  if (!must_admit("sess-close", p3, 2, 3, 0, -1, -1, 1)) goto done;
  if (sess_memo_misses() == before) bad("sess", "close stored nothing");
  if (!last_bit_is(p3, 1) || !proto_done(p3)) bad("sess", "close bit");
done:
  proto_free(p1);
  proto_free(p2);
  proto_free(p3);
  proto_free(pbad);
}

static void check_buf(const char *name, const uint8_t *p, size_t n) {
  muro__slice s = muro__slice_of(p, n);
  HSt uncached, miss, hit;
  uint64_t crlf_u, crlf_m, crlf_h;
  Classified *cls_u, *cls_m, *cls_h;

  muro_http_reset();
  uncached = scan_uncached(s);
  miss = scan(s);
  hit = scan(s);
  if (!hst_eq(uncached, miss) || !hst_eq(uncached, hit)) {
    bad(name, "scan");
    return;
  }

  crlf_u = crlfEnd_uncached(s);
  crlf_m = crlfEnd(s);
  crlf_h = crlfEnd(s);
  if (crlf_u != crlf_m || crlf_u != crlf_h) {
    bad(name, "crlfEnd");
    return;
  }

  muro_http_reset();
  cls_u = classifySt(uncached, s);
  cls_m = classify(s);
  cls_h = classify(s);
  if (!class_eq(cls_u, cls_m) || !class_eq(cls_u, cls_h)) {
    bad(name, "classify");
    return;
  }
  if (!check_views(name, s) || !check_steps(name, p, n)) return;
  compared += 1;
}

static void check_lit(const char *name, const char *raw) {
  check_buf(name, (const uint8_t *)raw, strlen(raw));
}

static int is_dot(const char *name) { return name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)); }

static void check_corpus(const char *dir) {
  DIR *d = opendir(dir);
  struct dirent *ent;
  char path[512];
  uint8_t *buf = NULL;
  size_t cap = 0;
  int nfiles = 0;
  if (!d) {
    bad("corpus", "open");
    return;
  }
  while ((ent = readdir(d)) != NULL) {
    struct stat st;
    FILE *f;
    size_t n;
    if (is_dot(ent->d_name)) continue;
    if (snprintf(path, sizeof path, "%s/%s", dir, ent->d_name) >= (int)sizeof path) {
      bad(ent->d_name, "path");
      continue;
    }
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) continue;
    if (st.st_size < 0) continue;
    n = (size_t)st.st_size;
    if (n + 1 > cap) {
      uint8_t *grew = realloc(buf, n + 1);
      if (!grew) {
        bad(ent->d_name, "alloc");
        continue;
      }
      buf = grew;
      cap = n + 1;
    }
    f = fopen(path, "rb");
    if (!f) {
      bad(ent->d_name, "read");
      continue;
    }
    if (n > 0 && fread(buf, 1, n, f) != n) {
      fclose(f);
      bad(ent->d_name, "short");
      continue;
    }
    fclose(f);
    check_buf(ent->d_name, buf, n);
    nfiles += 1;
  }
  closedir(d);
  free(buf);
  if (nfiles == 0) bad("corpus", "empty");
}

int main(void) {
  uint8_t *big;
  size_t i;

  check_lit("empty", "");
  check_lit("get", "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
  check_lit("post-slash", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping");
  check_lit("cl-zero", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
  check_lit("post-echo", "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping");
  check_lit("partial", "GET / HTTP/1.1\r\n");
  check_lit("chunk", "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n");
  check_lit("te-cl", "POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nContent-Length: 4\r\n\r\n0\r\n\r\n");
  check_lit("need-body", "POST /posts HTTP/1.1\r\nHost: t\r\nContent-Length: 2\r\n\r\n");
  check_lit("http10", "GET / HTTP/1.0\r\n\r\n");
  check_lit("close", "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
  check_lit("bare-lf", "GET / HTTP/1.1\nHost: t\n\n");
  check_lit("line-long", "GET /aaaaaaaaaaaaaaaa HTTP/1.1\r\nHost: t\r\n\r\n");
  check_lit("pipeline", "GET / HTTP/1.1\r\nHost: t\r\n\r\nGET / HTTP/1.1\r\nHost: t\r\n\r\n");

  check_step_walk("walk-get", "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
  check_step_walk("walk-post", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping");
  check_step_walk("walk-chunk",
                  "POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n");
  check_step_walk("walk-close", "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n");
  check_admit_memo();
  check_session_memo();
  check_body_rebind("rebind-get", "GET / HTTP/1.1\r\nHost: t\r\n\r\n");
  check_body_rebind("rebind-cl-zero", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n");
  check_body_rebind("rebind-post", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping");

  check_corpus("fuzz/corpus");

  /* Larger than SCAN_MAX skips the memory and still has to match. */
  big = malloc(65536 + 64);
  if (!big) {
    bad("oversize", "alloc");
  } else {
    const char *pref = "GET / HTTP/1.1\r\nHost: t\r\nX-Pad: ";
    size_t pn = strlen(pref);
    memcpy(big, pref, pn);
    for (i = 0; i < 65536; i++) big[pn + i] = 'x';
    memcpy(big + pn + 65536, "\r\n\r\n", 4);
    check_buf("oversize", big, pn + 65536 + 4);
    free(big);
  }

  if (failures) {
    fprintf(stderr, "%d failures, %d compared\n", failures, compared);
    return 1;
  }
  printf("OK %d compared\n", compared);
  return 0;
}
