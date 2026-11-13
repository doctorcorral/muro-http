#ifndef MURO_HTTP_SHIM_H
#define MURO_HTTP_SHIM_H

#include <stddef.h>
#include <stdint.h>

/* Bump arena for muro__ALLOC. Include this header, or pass
   -include c/shim.h, before the generated header so the default
   malloc definition is not used. */

void *muro_http_alloc(size_t n);
void muro_http_reset(void);
unsigned long muro_http_allocs(void);

#define muro__ALLOC(n) muro_http_alloc(n)

#endif
