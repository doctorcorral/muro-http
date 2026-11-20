/* Challenge routes. The core classifier does not name these paths. */

#include "shim.h"
#include "route.h"

#include <stdio.h>
#include <string.h>

static int failures;

static muro__slice of(const char *lit) { return muro__slice_of((const uint8_t *)lit, strlen(lit)); }

static void eq_u(const char *name, uint64_t got, uint64_t want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %llu want %llu\n", name, (unsigned long long)got,
            (unsigned long long)want);
    failures += 1;
  }
  muro_http_reset();
}

static void eq_i(const char *name, int64_t got, int64_t want) {
  if (got != want) {
    fprintf(stderr, "FAIL %s: got %lld want %lld\n", name, (long long)got, (long long)want);
    failures += 1;
  }
  muro_http_reset();
}

int main(void) {
  eq_u("verb-get", routeVerb(of("GET")), 0);
  eq_u("verb-post", routeVerb(of("POST")), 1);
  eq_u("verb-other", routeVerb(of("PUT")), 2);
  eq_u("health", routePath(of("/health")), 0);
  eq_u("feed", routePath(of("/feed")), 1);
  eq_u("posts", routePath(of("/posts")), 2);
  eq_u("id", routePath(of("/posts/42")), 3);
  eq_u("like", routePath(of("/posts/42/like")), 4);
  eq_u("tail", routePath(of("/posts/42/likes")), 5);
  eq_u("case", routePath(of("/Posts/42")), 3);
  eq_u("id-42", routeId(of("/posts/42")), 2);
  eq_i("idn-42", routeIdn(of("/posts/42")), 42);
  eq_u("id-zero", routeId(of("/posts/0")), 1);
  eq_u("id-abc", routeId(of("/posts/abc")), 1);
  eq_u("id-lead", routeId(of("/posts/01")), 1);
  eq_u("like-id", routeId(of("/posts/42/like")), 2);
  eq_i("like-idn", routeIdn(of("/posts/42/like")), 42);
  eq_u("health-id", routeId(of("/health")), 0);
  eq_i("health-idn", routeIdn(of("/health")), 0);
  eq_u("max", routeId(of("/posts/9223372036854775807")), 2);
  eq_i("max-n", routeIdn(of("/posts/9223372036854775807")), INT64_C(9223372036854775807));
  eq_u("over", routeId(of("/posts/9223372036854775808")), 1);
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
