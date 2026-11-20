#ifndef CHALLENGE_ROUTE_GLUE_H
#define CHALLENGE_ROUTE_GLUE_H

#include <stddef.h>
#include <stdint.h>

/* route.muro, from its own translation unit. The numbers are the
   constructor order of Verb and PathShape, plus the route id class:
   0 none, 1 bad, 2 a decimal id. routeIdn is that id, or 0. */

uint64_t challenge_verb(const uint8_t *p, size_t n);
uint64_t challenge_path(const uint8_t *p, size_t n);
uint64_t challenge_id(const uint8_t *p, size_t n);
int64_t challenge_idn(const uint8_t *p, size_t n);

#endif
