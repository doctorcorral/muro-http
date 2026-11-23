/* Level-triggered readiness. kqueue is the macOS default, epoll is
   the Linux default, and poll(2) is compiled on both. MURO_HTTP_WAIT
   selects among the backends this file can open. The idle tick is an
   absolute deadline shared by every backend: an earlier ready socket
   does not stretch it. ms == 0 disarms the tick.
   The Makefile defines _GNU_SOURCE, which exposes POLLRDHUP and
   epoll on Linux. POLLRDHUP is optional. POLLIN still reports a
   peer FIN.
   poll(2) collects ready events from a rotating cursor. kqueue and
   epoll return whatever the kernel queued, capped at `max`. They
   do not invent a cursor. A fd left out of this batch is still
   level-triggered, so a later wait can report it. When the idle
   tick is due, every backend reserves one slot for it and does
   not overwrite an fd event. */

#include "host_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#endif
#if defined(__linux__)
#include <sys/epoll.h>
#endif

struct Watch {
  int fd;
  int read;
  int write;
};

struct HostPoll {
  const char *backend;
  struct pollfd *pf;
  struct Watch *w;
  int n;
  int cap;
  int wn;
  int wcap;
  int kq;
  int timer_on;
  uint64_t timer_at;
  int next_scan;
};

uint64_t host_poll_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

const char *host_poll_backend(const HostPoll *p) {
  if (!p) return NULL;
  return p->backend;
}

int host_poll_supported(const char *name) {
  if (!name) return 0;
  if (strcmp(name, "poll") == 0) return 1;
#if defined(__APPLE__)
  if (strcmp(name, "kqueue") == 0) return 1;
#endif
#if defined(__linux__)
  if (strcmp(name, "epoll") == 0) return 1;
#endif
  return 0;
}

static const char *wanted(void) {
  const char *e = getenv("MURO_HTTP_WAIT");
  if (e && e[0]) return e;
#if defined(__APPLE__)
  return "kqueue";
#elif defined(__linux__)
  return "epoll";
#else
  return "poll";
#endif
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
  if (p->n <= 0) p->next_scan = 0;
  else if (p->next_scan >= p->n) p->next_scan = 0;
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

static int grow_pf(HostPoll *p) {
  int cap = p->cap < 8 ? 8 : p->cap * 2;
  struct pollfd *np;
  if (cap > 2048) return -1;
  np = realloc(p->pf, (size_t)cap * sizeof *np);
  if (!np) return -1;
  p->pf = np;
  p->cap = cap;
  return 0;
}

static int find_w(const HostPoll *p, int fd) {
  int i;
  for (i = 0; i < p->wn; i++) {
    if (p->w[i].fd == fd) return i;
  }
  return -1;
}

static void drop_w(HostPoll *p, int i) {
  p->wn -= 1;
  if (i < p->wn) p->w[i] = p->w[p->wn];
}

static int grow_w(HostPoll *p) {
  int cap = p->wcap < 8 ? 8 : p->wcap * 2;
  struct Watch *nw;
  if (cap > 2048) return -1;
  nw = realloc(p->w, (size_t)cap * sizeof *nw);
  if (!nw) return -1;
  p->w = nw;
  p->wcap = cap;
  return 0;
}

static int wait_timeout(const HostPoll *p) {
  uint64_t now;
  uint64_t left;
  if (!p->timer_on) return -1;
  now = host_poll_now_ms();
  if (p->timer_at <= now) return 0;
  left = p->timer_at - now;
  if (left > 86400000ull) return 86400000;
  return (int)left;
}

static void fill_poll(HostEv *e, const struct pollfd *pf) {
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
  /* POLLHUP is unmaskable. A write-only watch that gets hangup
     without POLLOUT has nothing to do; report error so the loop
     closes the fd instead of spinning. A read interest still
     reports hangup alone so a peer FIN can become evEof and the
     pending response can be written. */
  if (e->hangup && !(pf->events & POLLIN) && !e->writable) e->error = 1;
}

static int timer_due(const HostPoll *p) {
  return p->timer_on && host_poll_now_ms() >= p->timer_at;
}

/* Leave one slot empty when the tick is due so finish_timer can
   write it. An fd that does not fit stays level-triggered. */
static int event_room(const HostPoll *p, int max) {
  int room = max;
  if (timer_due(p) && room > 0) room -= 1;
  return room;
}

static int finish_timer(HostPoll *p, HostEv *out, int count, int max) {
  if (!timer_due(p)) return count;
  if (count >= max) return count;
  memset(&out[count], 0, sizeof out[count]);
  out[count].fd = -1;
  out[count].timer = 1;
  p->timer_on = 0;
  return count + 1;
}

static int poll_watch(HostPoll *p, int fd, int read, int write) {
  int i = find_fd(p, fd);
  short ev;
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
  if (p->n == p->cap && grow_pf(p) < 0) return -1;
  p->pf[p->n].fd = fd;
  p->pf[p->n].events = ev;
  p->pf[p->n].revents = 0;
  p->n += 1;
  return 0;
}

static int poll_wait(HostPoll *p, HostEv *out, int max) {
  struct pollfd scratch;
  struct pollfd *pf;
  nfds_t nfds;
  int timeout;
  int n;
  int i;
  int count;
  int room;
  int start;
  int seen;
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
  room = event_room(p, max);
  count = 0;
  if (n > 0 && p->n > 0) {
    start = p->next_scan;
    if (start < 0 || start >= p->n) start = 0;
    for (seen = 0; seen < p->n && count < room; seen++) {
      i = start + seen;
      if (i >= p->n) i -= p->n;
      if (p->pf[i].revents == 0) continue;
      fill_poll(&out[count], &p->pf[i]);
      count += 1;
    }
    p->next_scan = start + seen;
    if (p->next_scan >= p->n) p->next_scan -= p->n;
    if (p->next_scan < 0 || p->next_scan >= p->n) p->next_scan = 0;
  }
  return finish_timer(p, out, count, max);
}

#if defined(__APPLE__)
/* Test seams. Unset in production.
   MURO_HTTP_TEST_KQ_EINTR=N makes the next N change calls return
   EINTR once, then run the real kevent.
   MURO_HTTP_TEST_KQ_FAIL_AFTER=N lets N change calls reach kevent,
   then fails one call with EIO.
   MURO_HTTP_TEST_KQ_FAIL_TIMES=M, with FAIL_AFTER, fails M calls
   in a row. The default is 1. */
static int kq_env_take(const char *name, int *left) {
  const char *e = getenv(name);
  char *end = NULL;
  char buf[16];
  long v;
  if (!e || !*e) return 0;
  errno = 0;
  v = strtol(e, &end, 10);
  if (errno || !end || *end || v < 0 || v > 1000000L) return 0;
  *left = (int)v;
  if (*left == 0) {
    unsetenv(name);
    return 1;
  }
  snprintf(buf, sizeof buf, "%d", *left - 1);
  if (setenv(name, buf, 1) != 0) return 0;
  return 2;
}

/* One filter. 0 applied, -1 failed and errno is set. EINTR is
   retried. ENOENT on delete means the filter is already gone.
   A two-filter change is two calls so a failure cannot leave the
   first filter applied without a rollback. Darwin reports a
   changelist error as EV_ERROR when the event list has room, and
   as -1 otherwise. */
static int kq_one(HostPoll *p, int fd, int16_t filt, int add) {
  for (;;) {
    struct kevent in;
    struct kevent out;
    int rc;
    int left = 0;
    int took = kq_env_take("MURO_HTTP_TEST_KQ_EINTR", &left);
    if (took == 2) {
      errno = EINTR;
      continue;
    }
    took = kq_env_take("MURO_HTTP_TEST_KQ_FAIL_AFTER", &left);
    if (took == 1) {
      const char *times = getenv("MURO_HTTP_TEST_KQ_FAIL_TIMES");
      char *tend = NULL;
      long n = 1;
      if (times && *times) {
        errno = 0;
        n = strtol(times, &tend, 10);
        if (errno || !tend || *tend || n < 1 || n > 1000000L) n = 1;
      }
      if (n > 1) {
        char buf[16];
        snprintf(buf, sizeof buf, "%ld", n - 1);
        if (setenv("MURO_HTTP_TEST_KQ_FAIL_TIMES", buf, 1) == 0) {
          (void)setenv("MURO_HTTP_TEST_KQ_FAIL_AFTER", "0", 1);
        }
      } else {
        unsetenv("MURO_HTTP_TEST_KQ_FAIL_TIMES");
      }
      errno = EIO;
      return -1;
    }
    memset(&out, 0, sizeof out);
    EV_SET(&in, (uintptr_t)fd, filt, add ? EV_ADD : EV_DELETE, 0, 0, NULL);
    /* A null timeout waits for a readiness event. This call only
       applies one change, so the wait is zero. */
    {
      struct timespec zero;
      zero.tv_sec = 0;
      zero.tv_nsec = 0;
      rc = kevent(p->kq, &in, 1, &out, 1, &zero);
    }
    if (rc < 0) {
      if (errno == EINTR) continue;
      if (!add && errno == ENOENT) return 0;
      return -1;
    }
    if (rc > 0 && (out.flags & EV_ERROR) && out.ident == in.ident && out.filter == in.filter) {
      if (out.data == EINTR) continue;
      if (!add && out.data == ENOENT) return 0;
      errno = (int)out.data;
      return -1;
    }
    return 0;
  }
}

static int kq_apply(HostPoll *p, int fd, int16_t filt, int had, int want) {
  if (had == want) return 0;
  return kq_one(p, fd, filt, want);
}

/* 0: kernel matches the new interests. -1: kernel still matches
   had_*. -2: rollback failed and both filters were deleted, so
   the caller must drop any watch row. */
static int kq_change(HostPoll *p, int fd, int had_r, int had_w, int read, int write) {
  if (kq_apply(p, fd, EVFILT_READ, had_r, read) < 0) return -1;
  if (kq_apply(p, fd, EVFILT_WRITE, had_w, write) < 0) {
    /* The read filter already matches `read`. Put it back so a
       failed write change does not stick in the kernel alone. */
    if (kq_apply(p, fd, EVFILT_READ, read, had_r) < 0) {
      int err = errno;
      /* The undo failed. Userland has not updated the row, but
         the kernel may still hold the new read knote. Delete
         both filters so that knote cannot outlive the row. */
      (void)kq_one(p, fd, EVFILT_READ, 0);
      (void)kq_one(p, fd, EVFILT_WRITE, 0);
      errno = err;
      return -2;
    }
    return -1;
  }
  return 0;
}

static int kq_watch(HostPoll *p, int fd, int read, int write) {
  int i = find_w(p, fd);
  int had_r = 0;
  int had_w = 0;
  int rc;
  if (i >= 0) {
    had_r = p->w[i].read;
    had_w = p->w[i].write;
  }
  if (!read && !write) {
    if (i >= 0) {
      /* Drop the row even when delete fails. close_slot closes the
         fd after this returns, and a later socket can reuse the
         number. A stale row whose interests already match would
         skip EV_ADD, so the new socket would not be watched. */
      rc = kq_change(p, fd, had_r, had_w, 0, 0);
      drop_w(p, i);
      if (rc < 0) return -1;
    }
    return 0;
  }
  if (i < 0 && p->wn == p->wcap && grow_w(p) < 0) return -1;
  rc = kq_change(p, fd, had_r, had_w, read, write);
  if (rc < 0) {
    /* -2 means the kernel watch was cleared after a failed
       rollback. Drop a leftover row so a reused fd gets EV_ADD. */
    if (rc == -2 && i >= 0) drop_w(p, i);
    return -1;
  }
  if (i >= 0) {
    p->w[i].read = read;
    p->w[i].write = write;
    return 0;
  }
  p->w[p->wn].fd = fd;
  p->w[p->wn].read = read;
  p->w[p->wn].write = write;
  p->wn += 1;
  return 0;
}

static int kq_wait(HostPoll *p, HostEv *out, int max) {
  struct kevent evs[64];
  struct timespec ts;
  struct timespec *tsp = NULL;
  int timeout = wait_timeout(p);
  int n;
  int i;
  int count = 0;
  int room;
  if (timeout >= 0) {
    ts.tv_sec = timeout / 1000;
    ts.tv_nsec = (long)(timeout % 1000) * 1000000L;
    tsp = &ts;
  }
  n = kevent(p->kq, NULL, 0, evs, max, tsp);
  if (n < 0) return errno == EINTR ? 0 : -1;
  room = event_room(p, max);
  for (i = 0; i < n; i++) {
    int fd = (int)evs[i].ident;
    int slot = -1;
    int j;
    int wi;
    HostEv *e;
    for (j = 0; j < count; j++) {
      if (out[j].fd == fd) {
        slot = j;
        break;
      }
    }
    if (slot < 0) {
      if (count >= room) continue;
      memset(&out[count], 0, sizeof out[count]);
      out[count].fd = fd;
      slot = count;
      count += 1;
    }
    e = &out[slot];
    if (evs[i].flags & EV_ERROR) {
      e->error = 1;
      continue;
    }
    if (evs[i].filter == EVFILT_READ) {
      e->readable = 1;
      if (evs[i].flags & EV_EOF) e->hangup = 1;
    } else if (evs[i].filter == EVFILT_WRITE) {
      e->writable = 1;
      if (evs[i].flags & EV_EOF) e->hangup = 1;
    }
    wi = find_w(p, fd);
    if (e->hangup && (wi < 0 || !p->w[wi].read) && !e->writable) e->error = 1;
  }
  return finish_timer(p, out, count, max);
}
#endif

#if defined(__linux__)
static uint32_t ep_bits(int read, int write) {
  uint32_t ev = 0;
  if (read) ev |= EPOLLIN | EPOLLRDHUP;
  if (write) ev |= EPOLLOUT;
  return ev;
}

static int ep_ctl(HostPoll *p, int op, int fd, struct epoll_event *ee) {
  for (;;) {
    if (epoll_ctl(p->kq, op, fd, ee) == 0) return 0;
    if (errno == EINTR) continue;
    return -1;
  }
}

static int ep_apply(HostPoll *p, int fd, int existed, uint32_t bits) {
  struct epoll_event ee;
  int op;
  memset(&ee, 0, sizeof ee);
  ee.data.fd = fd;
  ee.events = bits;
  if (!bits) {
    if (!existed) return 0;
    if (ep_ctl(p, EPOLL_CTL_DEL, fd, &ee) < 0 && errno != ENOENT) return -1;
    return 0;
  }
  op = existed ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
  if (ep_ctl(p, op, fd, &ee) < 0) return -1;
  return 0;
}

static int ep_watch(HostPoll *p, int fd, int read, int write) {
  int i = find_w(p, fd);
  uint32_t bits = ep_bits(read, write);
  if (bits && i < 0 && p->wn == p->wcap && grow_w(p) < 0) return -1;
  if (ep_apply(p, fd, i >= 0, bits) < 0) return -1;
  if (!read && !write) {
    if (i >= 0) drop_w(p, i);
    return 0;
  }
  if (i >= 0) {
    p->w[i].read = read;
    p->w[i].write = write;
    return 0;
  }
  p->w[p->wn].fd = fd;
  p->w[p->wn].read = read;
  p->w[p->wn].write = write;
  p->wn += 1;
  return 0;
}

static int ep_wait(HostPoll *p, HostEv *out, int max) {
  struct epoll_event evs[64];
  int timeout = wait_timeout(p);
  int n;
  int i;
  int room;
  n = epoll_wait(p->kq, evs, max, timeout);
  if (n < 0) return errno == EINTR ? 0 : -1;
  room = event_room(p, max);
  if (n > room) n = room;
  for (i = 0; i < n; i++) {
    uint32_t r = evs[i].events;
    int fd = evs[i].data.fd;
    int wi = find_w(p, fd);
    HostEv *e = &out[i];
    memset(e, 0, sizeof *e);
    e->fd = fd;
    if (r & EPOLLIN) e->readable = 1;
    if (r & EPOLLOUT) e->writable = 1;
    if (r & (EPOLLHUP | EPOLLRDHUP)) e->hangup = 1;
    if (r & EPOLLERR) e->error = 1;
    if (e->hangup && (wi < 0 || !p->w[wi].read) && !e->writable) e->error = 1;
  }
  return finish_timer(p, out, n, max);
}
#endif

static HostPoll *new_poll(void) {
  HostPoll *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->backend = "poll";
  p->kq = -1;
  p->cap = 8;
  p->pf = calloc((size_t)p->cap, sizeof *p->pf);
  if (!p->pf) {
    free(p);
    return NULL;
  }
  return p;
}

#if defined(__APPLE__)
static int cloexec_fd(int fd) {
  int fl = fcntl(fd, F_GETFD, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

static HostPoll *new_kqueue(void) {
  HostPoll *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->backend = "kqueue";
  p->kq = kqueue();
  if (p->kq < 0 || cloexec_fd(p->kq) < 0) {
    if (p->kq >= 0) close(p->kq);
    free(p);
    return NULL;
  }
  p->wcap = 8;
  p->w = calloc((size_t)p->wcap, sizeof *p->w);
  if (!p->w) {
    close(p->kq);
    free(p);
    return NULL;
  }
  return p;
}
#endif

#if defined(__linux__)
static HostPoll *new_epoll(void) {
  HostPoll *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->backend = "epoll";
  p->kq = epoll_create1(EPOLL_CLOEXEC);
  if (p->kq < 0) {
    free(p);
    return NULL;
  }
  p->wcap = 8;
  p->w = calloc((size_t)p->wcap, sizeof *p->w);
  if (!p->w) {
    close(p->kq);
    free(p);
    return NULL;
  }
  return p;
}
#endif

HostPoll *host_poll_open(void) {
  const char *name = wanted();
  if (!host_poll_supported(name)) {
    errno = EINVAL;
    return NULL;
  }
  if (strcmp(name, "poll") == 0) return new_poll();
#if defined(__APPLE__)
  if (strcmp(name, "kqueue") == 0) return new_kqueue();
#endif
#if defined(__linux__)
  if (strcmp(name, "epoll") == 0) return new_epoll();
#endif
  errno = EINVAL;
  return NULL;
}

void host_poll_close(HostPoll *p) {
  if (!p) return;
  if (p->kq >= 0) close(p->kq);
  free(p->pf);
  free(p->w);
  free(p);
}

int host_poll_watch(HostPoll *p, int fd, int read, int write) {
  if (!p || fd < 0) return -1;
  if (strcmp(p->backend, "poll") == 0) return poll_watch(p, fd, read, write);
#if defined(__APPLE__)
  if (strcmp(p->backend, "kqueue") == 0) return kq_watch(p, fd, read, write);
#endif
#if defined(__linux__)
  if (strcmp(p->backend, "epoll") == 0) return ep_watch(p, fd, read, write);
#endif
  return -1;
}

int host_poll_arm(HostPoll *p, unsigned ms) {
  if (!p) return -1;
  if (ms == 0) {
    p->timer_on = 0;
    return 0;
  }
  p->timer_on = 1;
  p->timer_at = host_poll_now_ms() + (uint64_t)ms;
  return 0;
}

int host_poll_wait(HostPoll *p, HostEv *out, int max) {
  if (!p || !out || max <= 0) return -1;
  if (max > 64) max = 64;
  if (strcmp(p->backend, "poll") == 0) return poll_wait(p, out, max);
#if defined(__APPLE__)
  if (strcmp(p->backend, "kqueue") == 0) return kq_wait(p, out, max);
#endif
#if defined(__linux__)
  if (strcmp(p->backend, "epoll") == 0) return ep_wait(p, out, max);
#endif
  return -1;
}
