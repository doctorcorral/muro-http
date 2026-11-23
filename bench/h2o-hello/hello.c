#define _GNU_SOURCE
/* HTTP/1.1 static hello on libh2o (evloop, not libuv).
   GET / and POST / return hello\n, Content-Type: text/plain.
   HTTP/2 cleartext upgrade is off. No TLS.
   threads == 1 binds one socket. threads > 1 is h2o's tcp-reuseport
   model: each thread binds 127.0.0.1 with SO_REUSEPORT. Backlog is
   SOMAXCONN (4096 on this host). Accepted sockets set TCP_NODELAY,
   matching muro-http, nginx tcp_nodelay, and Go. The bench pins the
   whole process with taskset; threads are not pinned one-to-one. */

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "h2o.h"

static h2o_globalconf_t config;
static int opt_port = 18182;
static int opt_threads = 1;

struct slot {
  h2o_context_t ctx;
  h2o_accept_ctx_t accept_ctx;
};

static int on_req(h2o_handler_t *self, h2o_req_t *req) {
  int get = h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET"));
  int post = h2o_memis(req->method.base, req->method.len, H2O_STRLIT("POST"));
  (void)self;
  if (!get && !post) {
    req->res.status = 405;
    req->res.reason = "Method Not Allowed";
    req->res.content_length = 7;
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain"));
    h2o_send_inline(req, H2O_STRLIT("method\n"));
    return 0;
  }
  /* h2o_send_inline leaves content_length at SIZE_MAX, which is
     chunked. The other three servers send Content-Length: 6, and
     bench/loadgen requires that header. */
  req->res.status = 200;
  req->res.reason = "OK";
  req->res.content_length = 6;
  h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, H2O_STRLIT("text/plain"));
  h2o_send_inline(req, H2O_STRLIT("hello\n"));
  return 0;
}

static void on_accept(h2o_socket_t *listener, const char *err) {
  struct slot *slot = listener->data;
  h2o_socket_t *sock;
  int one = 1;
  if (err != NULL) return;
  sock = h2o_evloop_socket_accept(listener);
  if (sock == NULL) return;
  setsockopt(h2o_socket_get_fd(sock), IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  h2o_accept(&slot->accept_ctx, sock);
}

static int listen_on(int port, int reuseport) {
  int fd;
  int one = 1;
  struct sockaddr_in addr;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (reuseport && setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one) != 0) {
    close(fd);
    return -1;
  }
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(0x7f000001);
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, SOMAXCONN) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static void *thread_main(void *arg) {
  struct slot *slot = arg;
  int fd;
  h2o_socket_t *ls;
  h2o_context_init(&slot->ctx, h2o_evloop_create(), &config);
  slot->accept_ctx.ctx = &slot->ctx;
  slot->accept_ctx.hosts = config.hosts;
  fd = listen_on(opt_port, opt_threads > 1);
  if (fd < 0) {
    fprintf(stderr, "h2o-hello: listen: %s\n", strerror(errno));
    _exit(1);
  }
  ls = h2o_evloop_socket_create(slot->ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
  ls->data = slot;
  h2o_socket_read_start(ls, on_accept);
  fprintf(stderr, "listening 127.0.0.1 %d\n", opt_port);
  while (h2o_evloop_run(slot->ctx.loop, INT32_MAX) == 0) {
  }
  return NULL;
}

static int parse_i(const char *s, int lo, int hi, int *out) {
  char *end = NULL;
  long v;
  errno = 0;
  v = strtol(s, &end, 10);
  if (errno || !end || *end || v < lo || v > hi) return -1;
  *out = (int)v;
  return 0;
}

int main(int argc, char **argv) {
  h2o_hostconf_t *hostconf;
  h2o_pathconf_t *pathconf;
  h2o_handler_t *handler;
  struct slot *slots;
  pthread_t *tids;
  int i;
  signal(SIGPIPE, SIG_IGN);
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (strcmp(a, "--port") == 0 && val) {
      if (parse_i(val, 1, 65535, &opt_port) < 0) return 2;
      i++;
    } else if (strcmp(a, "--threads") == 0 && val) {
      if (parse_i(val, 1, 64, &opt_threads) < 0) return 2;
      i++;
    } else {
      fprintf(stderr, "usage: h2o-hello --port N --threads N\n");
      return 2;
    }
  }
  h2o_config_init(&config);
  /* HTTP/1 only. Timeouts match muro-hello --idle-ms 60000.
     The library defaults are 10s for the request and 5s for I/O. */
  config.http1.upgrade_to_http2 = 0;
  config.http1.req_timeout = 60000;
  config.http1.req_io_timeout = 60000;
  hostconf = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
  pathconf = h2o_config_register_path(hostconf, "/", 0);
  handler = h2o_create_handler(pathconf, sizeof *handler);
  handler->on_req = on_req;
  slots = calloc((size_t)opt_threads, sizeof *slots);
  tids = calloc((size_t)opt_threads, sizeof *tids);
  if (!slots || !tids) return 1;
  fprintf(stderr, "h2o-hello: threads %d %s\n", opt_threads, opt_threads > 1 ? "reuseport" : "one-socket");
  if (opt_threads == 1) {
    thread_main(&slots[0]);
    return 0;
  }
  for (i = 0; i < opt_threads; i++) {
    if (pthread_create(&tids[i], NULL, thread_main, &slots[i]) != 0) {
      fprintf(stderr, "h2o-hello: pthread_create\n");
      return 1;
    }
  }
  for (i = 0; i < opt_threads; i++) pthread_join(tids[i], NULL);
  return 0;
}
