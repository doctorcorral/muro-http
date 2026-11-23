/* The emitted renderer. This file includes render.h and copies the
   slice. It does not choose a status or a body. conn.h and render.h
   both put the prelude under MURO__PRELUDE, so one translation unit
   can include both. */

#include "shim.h"
#include "render.h"

#include <stdlib.h>
#include <string.h>

/* The handler does not own the framing headers, and it cannot put a
   CR or LF into the reason or end the header block early. A bad
   reason or a bad extra block fails the copy. Connection,
   Content-Length, Transfer-Encoding, and Date in the extra block
   are dropped. The book still writes Date, Connection, and either
   Content-Length or Transfer-Encoding. */

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
         name_is(p, n, "transfer-encoding") || name_is(p, n, "date");
}

/* A well-formed block that drops nothing is already the bytes httpRender
   should see. The caller then does not free it. */
static int extra_passthrough(const uint8_t *extra, size_t n) {
  size_t i = 0;
  if (!extra || n == 0) return 0;
  while (i < n) {
    size_t start = i;
    size_t colon;
    int saw = 0;
    while (i < n && extra[i] != '\r' && extra[i] != '\n') i++;
    if (i == start || i + 1 >= n || extra[i] != '\r' || extra[i + 1] != '\n') return 0;
    for (colon = start; colon < i; colon++) {
      if (extra[colon] == ':') {
        saw = 1;
        break;
      }
      if (extra[colon] == ' ' || extra[colon] == '\t') return 0;
    }
    if (!saw || colon == start || owned_name(extra + start, colon - start)) return 0;
    i += 2;
  }
  return 1;
}

/* 0 keeps *out (malloc'd, the caller's bytes, or NULL when nothing
   remains). -1 rejects. */
static int filter_extra(const uint8_t *extra, size_t n, uint8_t **out, size_t *out_n) {
  size_t i = 0;
  size_t used = 0;
  uint8_t *buf;
  *out = NULL;
  *out_n = 0;
  if (!extra || n == 0) return 0;
  if (extra_passthrough(extra, n)) {
    *out = (uint8_t *)extra;
    *out_n = n;
    return 0;
  }
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
                          size_t reason_n, const uint8_t *date, size_t date_n, const uint8_t *extra,
                          size_t extra_n, const uint8_t *body, size_t body_n, uint64_t keep,
                          uint64_t omit, size_t *got) {
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
  msg = httpRender(status, as_slice(reason, reason_n), as_slice(date, date_n),
                   as_slice(filtered, filtered_n), as_slice(body, body_n), keep, omit);
  n = copy_out(dst, cap, msg, got);
  if (filtered != extra) free(filtered);
  return n;
}

static muro__slice literal_of(int which, muro__slice date, uint64_t omit) {
  static const uint8_t type[] = "Content-Type: text/plain; charset=utf-8\r\n";
  muro__slice extra = muro__slice_of(type, sizeof type - 1);
  /* 414 and 431 are not book literals. httpRender prints the code. */
  if (which == 6 || which == 7) {
    const char *reason = which == 6 ? "URI Too Long" : "Request Header Fields Too Large";
    const char *body = which == 6 ? "uri too long" : "header fields";
    uint64_t status = which == 6 ? 414 : 431;
    return httpRender(status, muro__slice_of((const uint8_t *)reason, strlen(reason)), date, extra,
                      muro__slice_of((const uint8_t *)body, strlen(body)), 1, omit == 1 ? 1 : 0);
  }
  if (omit == 1 && which != 5) {
    uint64_t status = 400;
    muro__slice reason = muro__slice_of((const uint8_t *)"Bad Request", 11);
    muro__slice body = muro__slice_of((const uint8_t *)"malformed", 9);
    if (which == 1) {
      status = 500;
      reason = muro__slice_of((const uint8_t *)"Internal Server Error", 21);
      body = muro__slice_of((const uint8_t *)"internal", 8);
    } else if (which == 2) {
      status = 501;
      reason = muro__slice_of((const uint8_t *)"Not Implemented", 15);
      body = muro__slice_of((const uint8_t *)"unimplemented", 13);
    } else if (which == 3) {
      status = 413;
      reason = muro__slice_of((const uint8_t *)"Content Too Large", 17);
      body = muro__slice_of((const uint8_t *)"too large", 9);
    } else if (which == 4) {
      status = 417;
      reason = muro__slice_of((const uint8_t *)"Expectation Failed", 18);
      body = muro__slice_of((const uint8_t *)"expectation", 11);
    }
    return httpRender(status, reason, date, extra, body, 1, 1);
  }
  if (which == 1) return httpInternal(date);
  if (which == 2) return httpNotImplemented(date);
  if (which == 3) return httpTooLarge(date);
  if (which == 4) return httpExpect(date);
  if (which == 5) return httpContinue(date);
  return httpMalformed(date);
}

size_t host_copy_literal(uint8_t *dst, size_t cap, int which, const uint8_t *date, size_t date_n,
                         uint64_t omit, size_t *got) {
  return copy_out(dst, cap, literal_of(which, as_slice(date, date_n), omit), got);
}

static int reject_reason(const uint8_t *reason, size_t reason_n, const uint8_t *extra, size_t extra_n,
                         uint8_t **filtered, size_t *filtered_n) {
  if (bytes_have(reason, reason_n, '\r', '\n') || filter_extra(extra, extra_n, filtered, filtered_n) < 0) {
    muro_http_reset();
    return -1;
  }
  return 0;
}

size_t host_copy_head(uint8_t *dst, size_t cap, uint64_t status, const uint8_t *reason, size_t reason_n,
                      const uint8_t *date, size_t date_n, const uint8_t *extra, size_t extra_n,
                      uint64_t nlen, uint64_t keep, size_t *got) {
  uint8_t *filtered = NULL;
  size_t filtered_n = 0;
  muro__slice msg;
  size_t n;
  if (reject_reason(reason, reason_n, extra, extra_n, &filtered, &filtered_n) < 0) {
    if (got) *got = 0;
    return 0;
  }
  msg = httpHead(status, as_slice(reason, reason_n), as_slice(date, date_n), as_slice(filtered, filtered_n),
                 nlen, keep);
  n = copy_out(dst, cap, msg, got);
  if (filtered != extra) free(filtered);
  return n;
}

size_t host_copy_chunked(uint8_t *dst, size_t cap, uint64_t status, const uint8_t *reason,
                         size_t reason_n, const uint8_t *date, size_t date_n, const uint8_t *extra,
                         size_t extra_n, uint64_t keep, size_t *got) {
  uint8_t *filtered = NULL;
  size_t filtered_n = 0;
  muro__slice msg;
  size_t n;
  if (reject_reason(reason, reason_n, extra, extra_n, &filtered, &filtered_n) < 0) {
    if (got) *got = 0;
    return 0;
  }
  msg = httpChunked(status, as_slice(reason, reason_n), as_slice(date, date_n),
                    as_slice(filtered, filtered_n), keep);
  n = copy_out(dst, cap, msg, got);
  if (filtered != extra) free(filtered);
  return n;
}

size_t host_copy_delim(uint8_t *dst, size_t cap, uint64_t status, const uint8_t *reason, size_t reason_n,
                       const uint8_t *date, size_t date_n, const uint8_t *extra, size_t extra_n,
                       size_t *got) {
  uint8_t *filtered = NULL;
  size_t filtered_n = 0;
  muro__slice msg;
  size_t n;
  if (reject_reason(reason, reason_n, extra, extra_n, &filtered, &filtered_n) < 0) {
    if (got) *got = 0;
    return 0;
  }
  msg = httpDelim(status, as_slice(reason, reason_n), as_slice(date, date_n), as_slice(filtered, filtered_n));
  n = copy_out(dst, cap, msg, got);
  if (filtered != extra) free(filtered);
  return n;
}

size_t host_copy_chunk(uint8_t *dst, size_t cap, const uint8_t *body, size_t body_n, size_t *got) {
  if (!body || body_n == 0) {
    if (got) *got = 0;
    muro_http_reset();
    return 0;
  }
  return copy_out(dst, cap, httpChunk(as_slice(body, body_n)), got);
}

size_t host_copy_last(uint8_t *dst, size_t cap, size_t *got) {
  return copy_out(dst, cap, httpLastChunk(), got);
}
