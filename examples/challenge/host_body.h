#ifndef MURO_HTTP_HOST_BODY_H
#define MURO_HTTP_HOST_BODY_H

#include <stddef.h>
#include <stdint.h>

/* body.muro, called from its own translation unit. conn.h and
   body.h both put the prelude under MURO__PRELUDE, so one
   translation unit can include both. The socket loop does not
   include body.h. The results are BodyClass constructor order and
   the raw text span. They allocate in the shared bump arena. */

uint64_t host_body_class(const uint8_t *p, size_t n);
uint64_t host_text_off(const uint8_t *p, size_t n);
uint64_t host_text_len(const uint8_t *p, size_t n);

#endif
