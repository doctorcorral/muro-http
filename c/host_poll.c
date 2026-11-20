/* Level-triggered readiness on poll(2). macOS and Linux share this
   file. The idle tick is an absolute deadline: an earlier ready
   socket does not stretch it. ms == 0 disarms the tick.
   The Makefile defines _GNU_SOURCE, which exposes POLLRDHUP on Linux.
   That flag is optional. POLLIN still reports a peer FIN. */

#include "host_poll.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct HostPoll {
  struct pollfd *pf;
  int n;
  int cap;
  int timer_on;
  uint64_t timer_at;
};

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static int find_fd(const HostPoll *p, int fd) {
  int i;
  for (i = 0; i < p->n; i++) {
    if (p->pf[i].fd == fd) return i;
  }
  return -1;
}

static void drop_at(HostPoll *p, int i) {
  p->n -= 1;
  if (i < p->n) p->pf[i] = p->pf[p->n];
}

static short interest(int read, int write) {
  short ev = 0;
  if (read) {
    ev = (short)(ev | POLLIN);
#ifdef POLLRDHUP
    ev = (short)(ev | POLLRDHUP);
#endif
  }
  if (write) ev = (short)(ev | POLLOUT);
  return ev;
}

static int grow(HostPoll *p) {
  int cap = p->cap < 8 ? 8 : p->cap * 2;
  struct pollfd *np;
  /* One listen socket plus the connection cap (1024). */
  if (cap > 2048) return -1;
  np = realloc(p->pf, (size_t)cap * sizeof *np);
  if (!np) return -1;
  p->pf = np;
  p->cap = cap;
  return 0;
}

HostPoll *host_poll_open(void) {
  HostPoll *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->cap = 8;
  p->pf = calloc((size_t)p->cap, sizeof *p->pf);
  if (!p->pf) {
    free(p);
    return NULL;
  }
  return p;
}

void host_poll_close(HostPoll *p) {
  if (!p) return;
  free(p->pf);
  free(p);
}

int host_poll_watch(HostPoll *p, int fd, int read, int write) {
  int i;
  short ev;
  if (!p || fd < 0) return -1;
  i = find_fd(p, fd);
  if (!read && !write) {
    if (i >= 0) drop_at(p, i);
    return 0;
  }
  ev = interest(read, write);
  if (i >= 0) {
    p->pf[i].events = ev;
    p->pf[i].revents = 0;
    return 0;
  }
  if (p->n == p->cap && grow(p) < 0) return -1;
  p->pf[p->n].fd = fd;
  p->pf[p->n].events = ev;
  p->pf[p->n].revents = 0;
  p->n += 1;
  return 0;
}

int host_poll_arm(HostPoll *p, unsigned ms) {
  if (!p) return -1;
  if (ms == 0) {
    p->timer_on = 0;
    return 0;
  }
  p->timer_on = 1;
  p->timer_at = mono_ms() + (uint64_t)ms;
  return 0;
}

static int wait_timeout(const HostPoll *p) {
  uint64_t now;
  uint64_t left;
  if (!p->timer_on) return -1;
  now = mono_ms();
  if (p->timer_at <= now) return 0;
  left = p->timer_at - now;
  if (left > 86400000ull) return 86400000;
  return (int)left;
}

static void fill_ev(HostEv *e, const struct pollfd *pf) {
  short r = pf->revents;
  memset(e, 0, sizeof *e);
  e->fd = pf->fd;
  if (r & (POLLERR | POLLNVAL)) e->error = 1;
  if (r & POLLIN) e->readable = 1;
  if (r & POLLOUT) e->writable = 1;
  if (r & POLLHUP) e->hangup = 1;
#ifdef POLLRDHUP
  if (r & POLLRDHUP) e->hangup = 1;
#endif
}

int host_poll_wait(HostPoll *p, HostEv *out, int max) {
  struct pollfd scratch;
  struct pollfd *pf;
  nfds_t nfds;
  int timeout;
  int n;
  int i;
  int count;
  int timer_due;
  int room;
  if (!p || !out || max <= 0) return -1;
  if (max > 64) max = 64;
  /* poll with no fds is not portable. A negative fd waits for the
     timeout and contributes no readiness. */
  if (p->n == 0) {
    scratch.fd = -1;
    scratch.events = 0;
    scratch.revents = 0;
    pf = &scratch;
    nfds = 1;
  } else {
    pf = p->pf;
    nfds = (nfds_t)p->n;
  }
  timeout = wait_timeout(p);
  n = poll(pf, nfds, timeout);
  if (n < 0) return errno == EINTR ? 0 : -1;
  timer_due = p->timer_on && mono_ms() >= p->timer_at;
  room = max;
  if (timer_due && room > 0) room -= 1;
  count = 0;
  if (n > 0 && p->n > 0) {
    for (i = 0; i < p->n && count < room; i++) {
      if (p->pf[i].revents == 0) continue;
      fill_ev(&out[count], &p->pf[i]);
      count += 1;
    }
  }
  /* Socket events come first. A byte that arrived in this wait can
     refresh a deadline before the tick is delivered. */
  if (timer_due) {
    HostEv *e = &out[count++];
    memset(e, 0, sizeof *e);
    e->fd = -1;
    e->timer = 1;
    p->timer_on = 0;
  }
  return count;
}
