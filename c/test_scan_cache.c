/* Cached scan, crlfEnd, and classify must match the uncached book
   on the fuzz corpus and on the conformance request strings.
   test_classify links the book without the cache. The server and
   test_conn link c/scan_cache.c. */
#include "shim.h"
#include "conn.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

HSt scan_uncached(muro__slice s);
uint64_t crlfEnd_uncached(muro__slice s);

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

static int class_eq(Classified *a, Classified *b) {
  if (!a || !b) return 0;
  if (a->muro__field_tag != b->muro__field_tag) return 0;
  if (!a->muro__as.classified.muro__f_0 || !b->muro__as.classified.muro__f_0) return 0;
  if (a->muro__as.classified.muro__f_0->muro__field_tag != b->muro__as.classified.muro__f_0->muro__field_tag)
    return 0;
  if (!a->muro__as.classified.muro__f_1 || !b->muro__as.classified.muro__f_1) return 0;
  if (a->muro__as.classified.muro__f_1->muro__field_tag != b->muro__as.classified.muro__f_1->muro__field_tag)
    return 0;
  return 1;
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
