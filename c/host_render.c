/* The emitted renderer. conn.h and render.h both define the muro
   prelude, so the socket loop cannot include render.h. This file
   does. It copies the slice and does not choose a status or a body. */

#include "shim.h"
#include "render.h"

#include <string.h>

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
  muro__slice msg =
      httpRender(status, as_slice(reason, reason_n), as_slice(extra, extra_n),
                 as_slice(body, body_n), keep);
  return copy_out(dst, cap, msg, got);
}

size_t host_copy_malformed(uint8_t *dst, size_t cap, size_t *got) {
  return copy_out(dst, cap, httpMalformed(), got);
}

size_t host_copy_internal(uint8_t *dst, size_t cap, size_t *got) {
  return copy_out(dst, cap, httpInternal(), got);
}
