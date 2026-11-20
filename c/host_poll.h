#ifndef MURO_HTTP_HOST_POLL_H
#define MURO_HTTP_HOST_POLL_H

#include <stdint.h>

/* Level-triggered readiness on poll(2), on macOS and on Linux.
   The timer is a one-shot deadline. Arming it again replaces that
   deadline. ms == 0 disarms it. A ready socket that wakes the wait
   early does not move the deadline. host_poll_wait reports at most
   `max` events, scanning from a rotating cursor so a full set does
   not starve high-index fds. */

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

/* Monotonic milliseconds. Shared with the host loop. */
uint64_t host_poll_now_ms(void);

/* read and write are interests. Both clear removes the fd. */
int host_poll_watch(HostPoll *p, int fd, int read, int write);

int host_poll_arm(HostPoll *p, unsigned ms);

/* Blocks until an event, a signal, or an error.
   Returns the number of events, 0 on EINTR, or -1. */
int host_poll_wait(HostPoll *p, HostEv *out, int max);

#endif
