#ifndef MURO_HTTP_HOST_H
#define MURO_HTTP_HOST_H

/* The socket, the buffers, and the clock. Decisions stay in the
   emitted books. host_run binds 127.0.0.1 only. It returns 0 after
   SIGINT or SIGTERM, and -1 if the listen socket cannot be opened
   or the listen watch cannot be registered. */

typedef struct HostConfig {
  int port;
  int idle_ms;
  int max_conn;
} HostConfig;

int host_run(const HostConfig *cfg);

#endif
