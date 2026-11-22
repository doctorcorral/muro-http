#ifndef MURO_HTTP_HOST_POLL_H
#define MURO_HTTP_HOST_POLL_H

#include <stdint.h>

/* Level-triggered readiness. The default is kqueue on macOS and epoll
   on Linux. poll(2) is the portable fallback. MURO_HTTP_WAIT=poll
   (or kqueue, or epoll where that backend is compiled) selects one
   in a binary that has the native backend too. An unknown name, or
   a name this OS cannot open, makes host_poll_open return NULL.

   The timer is a one-shot deadline. Arming it again replaces that
   deadline. ms == 0 disarms it. A ready socket that wakes the wait
   early does not move the deadline. host_poll_wait reports at most
   `max` events (and at most 64). When the deadline is due, one of
   those slots is the timer. poll, kqueue, and epoll leave that
   slot empty instead of overwriting an fd event.

   poll(2) scans from a rotating cursor, so a full batch does not
   starve high-index fds. kqueue and epoll do not have that cursor.
   They return kernel readiness order. A level-triggered fd that did
   not fit in this batch stays ready for a later wait. */

typedef struct HostPoll HostPoll;

typedef struct HostEv {
  int fd;
  int timer;
  int readable;
  int writable;
  int hangup;
  int error;
} HostEv;

HostPoll *host_poll_open(void);
void host_poll_close(HostPoll *p);

/* "poll", "kqueue", or "epoll". NULL if p is NULL. */
const char *host_poll_backend(const HostPoll *p);

/* 1 if this process can open `name`. */
int host_poll_supported(const char *name);

/* Monotonic milliseconds. Shared with the host loop. */
uint64_t host_poll_now_ms(void);

/* read and write are interests. Both clear removes the fd. */
int host_poll_watch(HostPoll *p, int fd, int read, int write);

int host_poll_arm(HostPoll *p, unsigned ms);

/* Blocks until an event, a signal, or an error.
   Returns the number of events, 0 on EINTR, or -1. */
int host_poll_wait(HostPoll *p, HostEv *out, int max);

#endif
