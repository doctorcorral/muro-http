/* The only translation unit that includes route.h. */

#include "shim.h"
#include "route.h"
#include "route_glue.h"

uint64_t challenge_verb(const uint8_t *p, size_t n) { return routeVerb(muro__slice_of(p, n)); }

uint64_t challenge_path(const uint8_t *p, size_t n) { return routePath(muro__slice_of(p, n)); }

uint64_t challenge_id(const uint8_t *p, size_t n) { return routeId(muro__slice_of(p, n)); }

int64_t challenge_idn(const uint8_t *p, size_t n) { return routeIdn(muro__slice_of(p, n)); }
