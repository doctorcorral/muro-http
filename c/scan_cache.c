/* Remember the last scan of a byte string.

   classify, teCode, expectCode, bodyView, streamTake, and framedEnd
   each call scan on the same request. crlfEnd is the same kind of
   walk, from headerEnd. On this host one scan of a 65-byte GET is
   about 3.6 us, and a keep-alive hello does that walk several times.

   Both functions are pure in the bytes. The result is integers and
   indexes, not pointers into the buffer, so a later slice with the
   same bytes can reuse it. The generated book is what the Makefile
   renames to scan_uncached and crlfEnd_uncached. Calls inside that
   file stay spelled scan and crlfEnd, which resolve here. A linker
   --wrap does not: gcc emits a direct call, and the link relaxes it
   onto the definition in the same object.

   One process serves with one thread. A buffer bigger than the
   window skips the memory and calls the book. */

#include "conn.h"

#include <string.h>

/* The receive window is 65536. A larger slice is a test buffer. */
enum { SCAN_MAX = 65536 };

HSt scan_uncached(muro__slice s);
uint64_t crlfEnd_uncached(muro__slice s);

static int scan_ok;
static size_t scan_n;
static HSt scan_st;
static uint8_t scan_bytes[SCAN_MAX];

static int crlf_ok;
static size_t crlf_n;
static uint64_t crlf_at;
static uint8_t crlf_bytes[SCAN_MAX];

static int same(int ok, size_t have, const uint8_t *saved, muro__slice s) {
  if (!ok || have != s.n) return 0;
  if (s.n == 0) return 1;
  return memcmp(saved, s.p, s.n) == 0;
}

HSt scan(muro__slice s) {
  if (s.n > SCAN_MAX || (s.n > 0 && !s.p)) return scan_uncached(s);
  if (same(scan_ok, scan_n, scan_bytes, s)) return scan_st;
  scan_st = scan_uncached(s);
  if (s.n) memcpy(scan_bytes, s.p, s.n);
  scan_n = s.n;
  scan_ok = 1;
  return scan_st;
}

uint64_t crlfEnd(muro__slice s) {
  if (s.n > SCAN_MAX || (s.n > 0 && !s.p)) return crlfEnd_uncached(s);
  if (same(crlf_ok, crlf_n, crlf_bytes, s)) return crlf_at;
  crlf_at = crlfEnd_uncached(s);
  if (s.n) memcpy(crlf_bytes, s.p, s.n);
  crlf_n = s.n;
  crlf_ok = 1;
  return crlf_at;
}
