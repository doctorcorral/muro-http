#ifndef MURO_HTTP_HOST_RENDER_H
#define MURO_HTTP_HOST_RENDER_H

#include <stddef.h>
#include <stdint.h>

/* Copy the emitted httpRender slice into dst. render.h is included
   only by c/host_render.c, so a signature change fails that build.
   row is the body for a row act. The renderer decides whether to
   use it. Returns the byte count copied. Returns 0 when the slice
   is missing, empty, or larger than cap. *got is the slice length
   in every case. */
size_t host_copy_render(uint8_t *dst, size_t cap, uint64_t code, uint64_t err, uint64_t act,
                        uint64_t keep, const uint8_t *row, size_t row_n, size_t *got);

#endif
