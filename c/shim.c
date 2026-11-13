#include "shim.h"

#include <stdio.h>
#include <stdlib.h>

/* Step 1 stub. The classifier is generated Muro, not this file.
   Later steps own the syscall surface, which is not here:
   socket, bind, listen, accept, close, fcntl,
   epoll, kqueue, poll, read, write, and SQLite.
   No request parsing and no sockets. */

#define ARENA_BYTES (64u * 1024u * 1024u)

static unsigned char arena[ARENA_BYTES];
static size_t used;
static unsigned long count;

void *muro_http_alloc(size_t n) {
  size_t align = (n + (size_t)7) & ~(size_t)7;
  if (align < n || used > ARENA_BYTES - align) {
    fprintf(stderr, "arena exhausted at %zu + %zu\n", used, n);
    abort();
  }
  void *p = arena + used;
  used += align;
  count += 1;
  return p;
}

void muro_http_reset(void) {
  used = 0;
  count = 0;
}

unsigned long muro_http_allocs(void) { return count; }
