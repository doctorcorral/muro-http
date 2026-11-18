#ifndef MURO_HTTP_HOST_BODY_H
#define MURO_HTTP_HOST_BODY_H

#include <stddef.h>
#include <stdint.h>

/* body.muro, called from its own translation unit. conn.h and
   body.h both define the muro prelude, so the socket loop cannot
   include body.h. The results are BodyClass constructor order and
   the raw text span. They allocate in the shared bump arena. */

uint64_t host_body_class(const uint8_t *p, size_t n);
uint64_t host_text_off(const uint8_t *p, size_t n);
uint64_t host_text_len(const uint8_t *p, size_t n);

#endif
