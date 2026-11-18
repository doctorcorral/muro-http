/* The Makefile passes -D_GNU_SOURCE. timerfd needs it under -std=c11. */

#include "host_poll.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#include <sys/time.h>
#define HOST_POLL_KQUEUE 1
#elif defined(__linux__)
#include <sys/epoll.h>
#include <sys/timerfd.h>
#define HOST_POLL_EPOLL 1
#else
#error "muro-httpd needs kqueue or epoll"
#endif

#if HOST_POLL_KQUEUE
/* Not a socket fd. kqueue timer idents are their own space, but a
   high value keeps a glance at the event from looking like stdin. */
static const uintptr_t host_timer_ident = (uintptr_t)0x4d55524f;
#endif

struct HostPoll {
  int q;
  int timer_fd;
  int timer_on;
};

static int set_cloexec(int fd) {
  int fl = fcntl(fd, F_GETFD, 0);
  if (fl < 0) return -1;
  return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

HostPoll *host_poll_open(void) {
  HostPoll *p = calloc(1, sizeof *p);
  if (!p) return NULL;
  p->timer_fd = -1;
#if HOST_POLL_KQUEUE
  p->q = kqueue();
#else
  p->q = epoll_create1(EPOLL_CLOEXEC);
  if (p->q >= 0) {
    p->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (p->timer_fd < 0) {
      close(p->q);
      p->q = -1;
    } else {
      struct epoll_event ev;
      memset(&ev, 0, sizeof ev);
      ev.events = EPOLLIN;
      ev.data.fd = p->timer_fd;
      if (epoll_ctl(p->q, EPOLL_CTL_ADD, p->timer_fd, &ev) < 0) {
        close(p->timer_fd);
        close(p->q);
        p->q = -1;
      }
    }
  }
#endif
  if (p->q < 0) {
    free(p);
    return NULL;
  }
  if (set_cloexec(p->q) < 0) {
    host_poll_close(p);
    return NULL;
  }
  return p;
}

void host_poll_close(HostPoll *p) {
  if (!p) return;
  if (p->timer_fd >= 0) close(p->timer_fd);
  if (p->q >= 0) close(p->q);
  free(p);
}

#if HOST_POLL_KQUEUE
static int kq_set(int q, uintptr_t ident, short filter, int on) {
  struct kevent ch;
  EV_SET(&ch, ident, filter, on ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, NULL);
  if (kevent(q, &ch, 1, NULL, 0, NULL) < 0) {
    if (!on && errno == ENOENT) return 0;
    return -1;
  }
  return 0;
}
#endif

int host_poll_watch(HostPoll *p, int fd, int read, int write) {
  if (!p || fd < 0) return -1;
#if HOST_POLL_KQUEUE
  if (kq_set(p->q, (uintptr_t)fd, EVFILT_READ, read) < 0) return -1;
  if (kq_set(p->q, (uintptr_t)fd, EVFILT_WRITE, write) < 0) return -1;
  return 0;
#else
  {
    struct epoll_event ev;
    memset(&ev, 0, sizeof ev);
    ev.data.fd = fd;
    if (!read && !write) {
      if (epoll_ctl(p->q, EPOLL_CTL_DEL, fd, &ev) < 0 && errno != ENOENT && errno != EBADF)
        return -1;
      return 0;
    }
    ev.events = EPOLLRDHUP;
    if (read) ev.events |= EPOLLIN;
    if (write) ev.events |= EPOLLOUT;
    if (epoll_ctl(p->q, EPOLL_CTL_MOD, fd, &ev) < 0) {
      if (errno != ENOENT) return -1;
      if (epoll_ctl(p->q, EPOLL_CTL_ADD, fd, &ev) < 0) return -1;
    }
    return 0;
  }
#endif
}

int host_poll_arm(HostPoll *p, unsigned ms) {
  if (!p) return -1;
#if HOST_POLL_KQUEUE
  {
    struct kevent ch;
    if (ms == 0) {
      if (!p->timer_on) return 0;
      EV_SET(&ch, host_timer_ident, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
      if (kevent(p->q, &ch, 1, NULL, 0, NULL) < 0 && errno != ENOENT) return -1;
      p->timer_on = 0;
      return 0;
    }
    EV_SET(&ch, host_timer_ident, EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_ONESHOT, 0,
           (intptr_t)ms, NULL);
    if (kevent(p->q, &ch, 1, NULL, 0, NULL) < 0) return -1;
    p->timer_on = 1;
    return 0;
  }
#else
  {
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    if (ms > 0) {
      its.it_value.tv_sec = (time_t)(ms / 1000u);
      its.it_value.tv_nsec = (long)(ms % 1000u) * 1000000L;
    }
    if (timerfd_settime(p->timer_fd, 0, &its, NULL) < 0) return -1;
    p->timer_on = ms > 0;
    return 0;
  }
#endif
}

int host_poll_wait(HostPoll *p, HostEv *out, int max) {
  int n;
  int i;
  int count;
  if (!p || !out || max <= 0) return -1;
  if (max > 64) max = 64;
#if HOST_POLL_KQUEUE
  {
    struct kevent evs[64];
    n = kevent(p->q, NULL, 0, evs, max, NULL);
    if (n < 0) return errno == EINTR ? 0 : -1;
    count = 0;
    for (i = 0; i < n; i++) {
      HostEv *e = &out[count++];
      memset(e, 0, sizeof *e);
      e->fd = -1;
      if (evs[i].filter == EVFILT_TIMER) {
        e->timer = 1;
        continue;
      }
      e->fd = (int)evs[i].ident;
      if (evs[i].flags & EV_ERROR) e->error = 1;
      if (evs[i].filter == EVFILT_READ) {
        e->readable = 1;
        if (evs[i].flags & EV_EOF) e->hangup = 1;
      } else if (evs[i].filter == EVFILT_WRITE) {
        e->writable = 1;
        if (evs[i].flags & EV_EOF) e->hangup = 1;
      }
    }
    return count;
  }
#else
  {
    struct epoll_event evs[64];
    n = epoll_wait(p->q, evs, max, -1);
    if (n < 0) return errno == EINTR ? 0 : -1;
    count = 0;
    for (i = 0; i < n; i++) {
      HostEv *e = &out[count++];
      memset(e, 0, sizeof *e);
      e->fd = evs[i].data.fd;
      if (e->fd == p->timer_fd) {
        /* One expiration is 8 bytes. A short read leaves the fd
           readable, and level-triggered epoll returns at once.
           Read until 8 bytes or EAGAIN. A hard error stops the wait
           so the process does not spin. */
        uint8_t raw[8];
        size_t have = 0;
        while (have < sizeof raw) {
          ssize_t got = read(p->timer_fd, raw + have, sizeof raw - have);
          if (got < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
          }
          if (got == 0) return -1;
          have += (size_t)got;
        }
        e->fd = -1;
        e->timer = 1;
        continue;
      }
      if (evs[i].events & (EPOLLERR | EPOLLHUP)) e->error = 1;
      if (evs[i].events & EPOLLIN) e->readable = 1;
      if (evs[i].events & EPOLLOUT) e->writable = 1;
      if (evs[i].events & (EPOLLRDHUP | EPOLLHUP)) e->hangup = 1;
    }
    return count;
  }
#endif
}
