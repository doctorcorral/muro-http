/* The emitted renderer. conn.h and render.h both define the muro
   prelude, so the socket loop cannot include render.h. This file
   does. It copies the slice and does not choose a status or a body. */

#include "shim.h"
#include "render.h"

#include <string.h>

size_t host_copy_render(uint8_t *dst, size_t cap, uint64_t code, uint64_t err, uint64_t act,
                        uint64_t keep, const uint8_t *row, size_t row_n, size_t *got) {
  muro__slice msg = httpRender(code, err, act, keep, muro__slice_of(row, row_n));
  if (got) *got = msg.n;
  if (dst == NULL || msg.p == NULL || msg.n == 0 || msg.n > cap) return 0;
  memcpy(dst, msg.p, msg.n);
  return msg.n;
}
