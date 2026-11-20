#ifndef MURO_HTTP_HOST_RENDER_H
#define MURO_HTTP_HOST_RENDER_H

#include <stddef.h>
#include <stdint.h>

/* Copy one emitted response into dst. render.h is included only by
   c/host_render.c. Returns the byte count copied, or 0 when the
   slice is missing, empty, or larger than cap. *got is the slice
   length in every case. The copy resets the bump arena. */

size_t host_copy_response(uint8_t *dst, size_t cap, uint64_t status, const uint8_t *reason,
                          size_t reason_n, const uint8_t *extra, size_t extra_n,
                          const uint8_t *body, size_t body_n, uint64_t keep, size_t *got);

size_t host_copy_malformed(uint8_t *dst, size_t cap, size_t *got);
size_t host_copy_internal(uint8_t *dst, size_t cap, size_t *got);

#endif
