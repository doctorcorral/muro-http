/* The emitted renderer. conn.h and render.h both define the muro
   prelude, so the socket loop cannot include render.h. This file
   does. It copies the slice and does not choose a status or a body. */

#include "shim.h"
#include "render.h"

#include <stdlib.h>
#include <string.h>

/* The handler does not own the framing headers, and it cannot put a
   CR or LF into the reason or end the header block early. A bad
   reason or a bad extra block fails the copy. Connection,
   Content-Length, and Transfer-Encoding in the extra block are
   dropped. The book still writes Connection and Content-Length. */

static int bytes_have(const uint8_t *p, size_t n, uint8_t a, uint8_t b) {
  size_t i;
  if (!p) return 0;
  for (i = 0; i < n; i++) {
    if (p[i] == a || p[i] == b) return 1;
  }
  return 0;
}

static int name_is(const uint8_t *p, size_t n, const char *lit) {
  size_t i;
  size_t m = strlen(lit);
  if (n != m) return 0;
  for (i = 0; i < n; i++) {
    unsigned char c = p[i];
    if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
    if (c != (unsigned char)lit[i]) return 0;
  }
  return 1;
}

static int owned_name(const uint8_t *p, size_t n) {
  return name_is(p, n, "connection") || name_is(p, n, "content-length") ||
         name_is(p, n, "transfer-encoding");
}

/* 0 keeps *out (malloc'd, or NULL when nothing remains). -1 rejects. */
static int filter_extra(const uint8_t *extra, size_t n, uint8_t **out, size_t *out_n) {
  size_t i = 0;
  size_t used = 0;
  uint8_t *buf;
  *out = NULL;
  *out_n = 0;
  if (!extra || n == 0) return 0;
  buf = malloc(n + 2);
  if (!buf) return -1;
  while (i < n) {
    size_t start = i;
    size_t line_end;
    size_t colon = 0;
    int saw = 0;
    while (i < n && extra[i] != '\r' && extra[i] != '\n') i++;
    line_end = i;
    if (i < n) {
      if (i + 1 >= n || extra[i] != '\r' || extra[i + 1] != '\n') {
        free(buf);
        return -1;
      }
      i += 2;
    }
    if (line_end == start) {
      free(buf);
      return -1;
    }
    for (colon = start; colon < line_end; colon++) {
      if (extra[colon] == ':') {
        saw = 1;
        break;
      }
      if (extra[colon] == ' ' || extra[colon] == '\t') {
        free(buf);
        return -1;
      }
    }
    if (!saw || colon == start) {
      free(buf);
      return -1;
    }
    if (owned_name(extra + start, colon - start)) continue;
    if (used + (line_end - start) + 2 > n + 2) {
      free(buf);
      return -1;
    }
    memcpy(buf + used, extra + start, line_end - start);
    used += line_end - start;
    buf[used++] = '\r';
    buf[used++] = '\n';
  }
  if (used == 0) {
    free(buf);
    return 0;
  }
  *out = buf;
  *out_n = used;
  return 0;
}

static muro__slice as_slice(const uint8_t *p, size_t n) {
  if (!p || n == 0) return muro__slice_empty();
  return muro__slice_of(p, n);
}

static size_t copy_out(uint8_t *dst, size_t cap, muro__slice msg, size_t *got) {
  size_t n = 0;
  if (got) *got = msg.n;
  if (dst && msg.p && msg.n > 0 && msg.n <= cap) {
    memcpy(dst, msg.p, msg.n);
    n = msg.n;
  }
  muro_http_reset();
  return n;
}

size_t host_copy_response(uint8_t *dst, size_t cap, uint64_t status, const uint8_t *reason,
                          size_t reason_n, const uint8_t *extra, size_t extra_n,
                          const uint8_t *body, size_t body_n, uint64_t keep, size_t *got) {
  uint8_t *filtered = NULL;
  size_t filtered_n = 0;
  muro__slice msg;
  size_t n;
  if (bytes_have(reason, reason_n, '\r', '\n') ||
      filter_extra(extra, extra_n, &filtered, &filtered_n) < 0) {
    if (got) *got = 0;
    muro_http_reset();
    return 0;
  }
  msg = httpRender(status, as_slice(reason, reason_n), as_slice(filtered, filtered_n),
                   as_slice(body, body_n), keep);
  n = copy_out(dst, cap, msg, got);
  free(filtered);
  return n;
}

size_t host_copy_malformed(uint8_t *dst, size_t cap, size_t *got) {
  return copy_out(dst, cap, httpMalformed(), got);
}

size_t host_copy_internal(uint8_t *dst, size_t cap, size_t *got) {
  return copy_out(dst, cap, httpInternal(), got);
}
