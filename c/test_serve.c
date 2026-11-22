/* muro_http_serve refuses a connection cap outside 1..1024 before it
   allocates or listens. host_test_skipped_tick drives the read and
   idle arm of on_tick while the fd is on the body-read skip list. */
#include "muro_http.h"

#include <stdio.h>

/* mode-read = 0 and mode-idle = 3, the same tags as conn.muro. */
extern int host_test_skipped_tick(int phase);

static int on_req(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  (void)req;
  (void)resp;
  (void)user;
  return 0;
}

static int refuse(int max_conn) {
  MuroHttpConfig cfg;
  muro_http_config_init(&cfg);
  cfg.max_conn = max_conn;
  cfg.port = 0;
  if (muro_http_serve(&cfg, on_req, NULL) != -1) {
    fprintf(stderr, "FAIL max_conn %d was accepted\n", max_conn);
    return 1;
  }
  return 0;
}

int main(void) {
  if (host_test_skipped_tick(0) != 0) {
    fprintf(stderr, "FAIL skipped tick in read left body_idle unset\n");
    return 1;
  }
  if (host_test_skipped_tick(3) != 0) {
    fprintf(stderr, "FAIL skipped tick in idle left body_idle unset\n");
    return 1;
  }
  if (refuse(0) || refuse(-1) || refuse(1025)) return 1;
  printf("OK\n");
  return 0;
}
