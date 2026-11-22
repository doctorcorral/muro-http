#ifndef MURO_HTTP_HOST_RENDER_H
#define MURO_HTTP_HOST_RENDER_H

#include <stddef.h>
#include <stdint.h>

/* Copy one emitted response into dst. render.h is included only by
   c/host_render.c. Returns the byte count copied, or 0 when the
   slice is missing, empty, or larger than cap, or when the reason
   or the extra block would break CRLF framing. Connection,
   Content-Length, Transfer-Encoding, and Date in the extra block
   are dropped. *got is the slice length in every case, or 0 when
   the handler bytes are rejected. The copy resets the bump arena.
   date is the 29-byte IMF-fixdate the host formatted. omit 1 keeps
   Content-Length and drops the body bytes (HEAD). */

size_t host_copy_response(uint8_t *dst, size_t cap, uint64_t status, const uint8_t *reason,
                          size_t reason_n, const uint8_t *date, size_t date_n, const uint8_t *extra,
                          size_t extra_n, const uint8_t *body, size_t body_n, uint64_t keep,
                          uint64_t omit, size_t *got);

/* which: 0 malformed, 1 internal, 2 not implemented, 3 too large,
   4 expectation failed, 5 100 Continue. omit 1 is HEAD: same
   headers, no body. 100 Continue ignores omit. */
size_t host_copy_literal(uint8_t *dst, size_t cap, int which, const uint8_t *date, size_t date_n,
                         uint64_t omit, size_t *got);

size_t host_copy_head(uint8_t *dst, size_t cap, uint64_t status, const uint8_t *reason,
                      size_t reason_n, const uint8_t *date, size_t date_n, const uint8_t *extra,
                      size_t extra_n, uint64_t nlen, uint64_t keep, size_t *got);

size_t host_copy_chunked(uint8_t *dst, size_t cap, uint64_t status, const uint8_t *reason,
                         size_t reason_n, const uint8_t *date, size_t date_n, const uint8_t *extra,
                         size_t extra_n, uint64_t keep, size_t *got);

size_t host_copy_delim(uint8_t *dst, size_t cap, uint64_t status, const uint8_t *reason,
                       size_t reason_n, const uint8_t *date, size_t date_n, const uint8_t *extra,
                       size_t extra_n, size_t *got);

/* One chunk. body_n 0 returns 0: the last chunk is host_copy_last. */
size_t host_copy_chunk(uint8_t *dst, size_t cap, const uint8_t *body, size_t body_n, size_t *got);

size_t host_copy_last(uint8_t *dst, size_t cap, size_t *got);

#endif
