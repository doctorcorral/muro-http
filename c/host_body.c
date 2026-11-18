/* The emitted JSON body class. This file is the only one that
   includes body.h. */

#include "shim.h"
#include "body.h"
#include "host_body.h"

uint64_t host_body_class(const uint8_t *p, size_t n) { return bodyClass(muro__slice_of(p, n)); }

uint64_t host_text_off(const uint8_t *p, size_t n) { return textOff(muro__slice_of(p, n)); }

uint64_t host_text_len(const uint8_t *p, size_t n) { return textLen(muro__slice_of(p, n)); }
