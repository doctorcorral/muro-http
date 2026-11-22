/* Start muro-httpd on 127.0.0.1 and an ephemeral port. Raw sockets.
   The binary path is argv[1], or c/muro-httpd. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "jwt.h"

static int failures;
static const char *bin_path = "c/muro-httpd";
/* Child flags. The test key is not a server default. */
static const char *srv_db = ":memory:";
static const char *srv_jwt = NULL;
static int pass_db = 1;
static const char *srv_bind = NULL;
static const char *srv_bind_env = NULL;
static const char *dial_addr = "127.0.0.1";
static const char *env_sqlite = NULL;
static const char *env_muro_db = NULL;
static const char *env_fail_admit = NULL;
static int last_json;

static void fail(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s: %s\n", name, what);
  failures += 1;
}

static int parse_port(const char *line, const char *host) {
  char prefix[80];
  int port = -1;
  int n;
  if (!host || !host[0]) host = "127.0.0.1";
  n = snprintf(prefix, sizeof prefix, "listening %s ", host);
  if (n < 0 || (size_t)n >= sizeof prefix) return -1;
  if (strncmp(line, prefix, (size_t)n) != 0) return -1;
  if (sscanf(line + n, "%d", &port) != 1) return -1;
  if (port <= 0 || port > 65535) return -1;
  return port;
}

static pid_t start_server(int idle_ms, int max_conn, int *port, const char *write_limit,
                          const char *write_log, const char *in_cap) {
  int sp[2];
  pid_t pid;
  char idle[32];
  char maxc[32];
  char buf[128];
  size_t n = 0;
  int use_db = pass_db;
  const char *bind = srv_bind;
  const char *bind_env = srv_bind_env;
  const char *banner_host = (bind && bind[0]) ? bind : "127.0.0.1";
  const char *sqlite_env = env_sqlite;
  const char *muro_env = env_muro_db;
  const char *fail_admit = env_fail_admit;
  pass_db = 1;
  srv_bind = NULL;
  srv_bind_env = NULL;
  env_sqlite = NULL;
  env_muro_db = NULL;
  env_fail_admit = NULL;
  if (max_conn < 1) return -1;
  if (pipe(sp) < 0) return -1;
  pid = fork();
  if (pid < 0) {
    close(sp[0]);
    close(sp[1]);
    return -1;
  }
  if (pid == 0) {
    char *av[20];
    int argc = 0;
    const char *db = srv_db && srv_db[0] ? srv_db : ":memory:";
    alarm(0);
    if (dup2(sp[1], STDOUT_FILENO) < 0) _exit(127);
    close(sp[0]);
    close(sp[1]);
    snprintf(idle, sizeof idle, "%d", idle_ms);
    snprintf(maxc, sizeof maxc, "%d", max_conn);
    unsetenv("MURO_HTTP_BIND");
    unsetenv("MURO_HTTP_MAX_BODY");
    unsetenv("MURO_HTTP_DRAIN_MS");
    unsetenv("MURO_HTTP_WORKERS");
    unsetenv("MURO_HTTP_WAIT");
    if (bind_env && bind_env[0]) setenv("MURO_HTTP_BIND", bind_env, 1);
    unsetenv("MURO_HTTP_DB");
    unsetenv("MURO_HTTP_JWT_KEY");
    unsetenv("SQLITE_PATH");
    unsetenv("MURO_TEST_FAIL_DB_ADMIT");
    if (sqlite_env && sqlite_env[0]) setenv("SQLITE_PATH", sqlite_env, 1);
    if (muro_env && muro_env[0]) setenv("MURO_HTTP_DB", muro_env, 1);
    if (fail_admit && fail_admit[0]) setenv("MURO_TEST_FAIL_DB_ADMIT", fail_admit, 1);
    if (write_limit && write_limit[0]) setenv("MURO_TEST_WRITE_LIMIT", write_limit, 1);
    if (write_log && write_log[0]) setenv("MURO_TEST_WRITE_LOG", write_log, 1);
    if (in_cap && in_cap[0]) setenv("MURO_TEST_IN_CAP", in_cap, 1);
    av[argc++] = "muro-httpd";
    av[argc++] = "--port";
    av[argc++] = "0";
    av[argc++] = "--idle-ms";
    av[argc++] = idle;
    av[argc++] = "--max-connections";
    av[argc++] = maxc;
    if (bind && bind[0]) {
      av[argc++] = "--bind";
      av[argc++] = (char *)bind;
    }
    if (use_db) {
      av[argc++] = "--db";
      av[argc++] = (char *)db;
    }
    if (srv_jwt && srv_jwt[0]) {
      av[argc++] = "--jwt-key";
      av[argc++] = (char *)srv_jwt;
    }
    av[argc] = NULL;
    execv(bin_path, av);
    perror("exec");
    _exit(127);
  }
  close(sp[1]);
  while (n + 1 < sizeof buf) {
    struct pollfd pfd;
    ssize_t r;
    pfd.fd = sp[0];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 3000) <= 0) break;
    r = read(sp[0], buf + n, 1);
    if (r <= 0) break;
    if (buf[n] == '\n') {
      buf[n] = 0;
      n += 1;
      break;
    }
    n += (size_t)r;
  }
  close(sp[0]);
  buf[n < sizeof buf ? n : sizeof buf - 1] = 0;
  *port = parse_port(buf, banner_host);
  if (*port < 0) {
    fprintf(stderr, "FAIL start: banner '%s'\n", buf);
    failures += 1;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }
  return pid;
}

static void kill_server(pid_t pid) {
  if (pid <= 0) return;
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
}

static int stop_with(pid_t pid, int sig, const char *name) {
  int status = 0;
  int i;
  if (pid <= 0) return -1;
  kill(pid, sig);
  for (i = 0; i < 50; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
      fprintf(stderr, "FAIL %s: status %d\n", name, status);
      failures += 1;
      return -1;
    }
    if (got < 0) return -1;
    usleep(50000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  fail(name, "did not stop the server");
  return -1;
}

static int stop_server(pid_t pid) { return stop_with(pid, SIGTERM, "shutdown"); }

static int dial(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  struct timeval tv;
  if (fd < 0) return -1;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, dial_addr, &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    close(fd);
    return -1;
  }
  tv.tv_sec = 5;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  return fd;
}

static int send_all(int fd, const char *s) {
  size_t n = strlen(s);
  size_t off = 0;
  while (off < n) {
    ssize_t w = send(fd, s + off, n - off, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

/* Several writes, each at most `chunk` bytes. A short send is retried. */
static int send_chunks(int fd, const char *s, size_t chunk) {
  size_t n = strlen(s);
  size_t off = 0;
  if (chunk == 0) return -1;
  while (off < n) {
    size_t m = n - off;
    ssize_t w;
    if (m > chunk) m = chunk;
    w = send(fd, s + off, m, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

static int find_cl(const char *buf, size_t n, int hend, int *cl) {
  size_t i;
  for (i = 0; i + 16 < (size_t)hend; i++) {
    if (memcmp(buf + i, "Content-Length: ", 16) == 0) {
      int v = 0;
      size_t j = i + 16;
      if (j >= (size_t)hend || buf[j] < '0' || buf[j] > '9') return -1;
      while (j < (size_t)hend && buf[j] >= '0' && buf[j] <= '9') {
        v = v * 10 + (buf[j] - '0');
        j += 1;
      }
      *cl = v;
      return 0;
    }
  }
  (void)n;
  return -1;
}

/* Read one response. Returns bytes in buf, sets *used to the end of it.
   Leaves any pipelined tail at the front of buf and sets *kept. */
static int read_response(int fd, char *buf, size_t cap, size_t have, size_t *used, int *status,
                         int *conn_close, char *body, size_t body_cap, size_t *body_n) {
  size_t n = have;
  for (;;) {
    int hend = -1;
    size_t i;
    int cl = -1;
    size_t total;
    for (i = 0; i + 3 < n; i++) {
      if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
        hend = (int)i;
        break;
      }
    }
    if (hend >= 0 && find_cl(buf, n, hend, &cl) == 0) {
      total = (size_t)hend + 4 + (size_t)cl;
      if (n >= total) {
        int code = 0;
        if (sscanf(buf, "HTTP/1.1 %d", &code) != 1) return -1;
        *status = code;
        *conn_close = memmem(buf, (size_t)hend, "Connection: close", 17) != NULL;
        last_json = memmem(buf, (size_t)hend, "Content-Type: application/json", 30) != NULL;
        if ((size_t)cl > body_cap) return -1;
        memcpy(body, buf + hend + 4, (size_t)cl);
        body[(size_t)cl] = 0;
        *body_n = (size_t)cl;
        if (n > total) memmove(buf, buf + total, n - total);
        *used = n > total ? n - total : 0;
        return 0;
      }
    }
    if (n >= cap) return -1;
    {
      ssize_t r = read(fd, buf + n, cap - n);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) return -1;
      n += (size_t)r;
    }
  }
}

static int still_open(int fd) {
  char b;
  int flags = fcntl(fd, F_GETFL, 0);
  ssize_t r;
  if (flags < 0) return 0;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return 0;
  r = recv(fd, &b, 1, MSG_PEEK);
  fcntl(fd, F_SETFL, flags);
  if (r == 0) return 0;
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
  return r > 0;
}

static int expect_eof(int fd) {
  char b;
  ssize_t r = read(fd, &b, 1);
  return r == 0;
}

static int check_one(int port, const char *name, const char *req, int want_status, const char *want_body,
                     int want_close, int want_eof) {
  int fd = dial(port);
  char buf[8192];
  char body[4096];
  size_t body_n = 0;
  size_t used = 0;
  int status = 0;
  int conn_close = 0;
  if (fd < 0) {
    fail(name, "connect");
    return -1;
  }
  if (send_all(fd, req) < 0) {
    fail(name, "send");
    close(fd);
    return -1;
  }
  if (read_response(fd, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail(name, "response");
    close(fd);
    return -1;
  }
  if (status != want_status) {
    fprintf(stderr, "FAIL %s: status %d\n", name, status);
    failures += 1;
  }
  if (strcmp(body, want_body) != 0) {
    fprintf(stderr, "FAIL %s: body '%s'\n", name, body);
    failures += 1;
  }
  if (body_n > 0 && (body[0] == '{' || body[0] == '[') && !last_json) fail(name, "content-type");
  if (conn_close != want_close) fail(name, want_close ? "wanted close" : "wanted keep-alive");
  if (want_eof) {
    if (!expect_eof(fd)) fail(name, "expected eof");
  } else if (!still_open(fd)) {
    fail(name, "connection closed");
  }
  close(fd);
  return 0;
}

static void check_keepalive(int port) {
  int fd = dial(port);
  char buf[4096];
  char body[64];
  size_t body_n = 0;
  size_t used = 0;
  int status = 0;
  int conn_close = 0;
  if (fd < 0) {
    fail("keep-alive", "connect");
    return;
  }
  if (send_all(fd, "GET /health HTTP/1.1\r\nHost: t\r\n\r\n") < 0 ||
      read_response(fd, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("keep-alive", "first");
    close(fd);
    return;
  }
  if (status != 200 || strcmp(body, "ok") != 0 || conn_close) fail("keep-alive", "first response");
  if (send_all(fd, "GET /health HTTP/1.1\r\nHost: t\r\n\r\n") < 0 ||
      read_response(fd, buf, sizeof buf, used, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("keep-alive", "second");
    close(fd);
    return;
  }
  if (status != 200 || strcmp(body, "ok") != 0 || conn_close) fail("keep-alive", "second response");
  if (!still_open(fd)) fail("keep-alive", "closed after two requests");
  close(fd);
}

static void check_pipeline(int port) {
  int fd = dial(port);
  char buf[4096];
  char body[64];
  size_t body_n = 0;
  size_t used = 0;
  int status = 0;
  int conn_close = 0;
  const char *req = "GET /health HTTP/1.1\r\nHost: t\r\n\r\nGET /missing HTTP/1.1\r\nHost: t\r\n\r\n";
  if (fd < 0) {
    fail("pipeline", "connect");
    return;
  }
  if (send_all(fd, req) < 0) {
    fail("pipeline", "send");
    close(fd);
    return;
  }
  if (read_response(fd, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("pipeline", "first");
    close(fd);
    return;
  }
  if (status != 200 || strcmp(body, "ok") != 0 || conn_close) fail("pipeline", "first response");
  if (read_response(fd, buf, sizeof buf, used, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("pipeline", "second");
    close(fd);
    return;
  }
  if (status != 404 || strcmp(body, "not-found") != 0 || !conn_close) fail("pipeline", "second response");
  close(fd);
}

static void check_fin(int port) {
  int fd = dial(port);
  char buf[4096];
  char body[64];
  size_t body_n = 0;
  size_t used = 0;
  int status = 0;
  int conn_close = 0;
  if (fd < 0) {
    fail("fin", "connect");
    return;
  }
  if (send_all(fd, "GET /health HTTP/1.1\r\n") < 0) {
    fail("fin", "send");
    close(fd);
    return;
  }
  shutdown(fd, SHUT_WR);
  if (read_response(fd, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("fin", "response");
    close(fd);
    return;
  }
  if (status != 400 || strcmp(body, "malformed") != 0) fail("fin", "status");
  if (!expect_eof(fd)) fail("fin", "expected eof");
  close(fd);
}

static void check_idle_bare(int port) {
  int fd = dial(port);
  struct pollfd pfd;
  char b;
  if (fd < 0) {
    fail("idle-bare", "connect");
    return;
  }
  pfd.fd = fd;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, 4000) <= 0) {
    fail("idle-bare", "timed out");
    close(fd);
    return;
  }
  if (read(fd, &b, 1) != 0) fail("idle-bare", "not eof");
  close(fd);
}

static void check_idle_after(int port) {
  int fd = dial(port);
  char buf[4096];
  char body[64];
  size_t body_n = 0;
  size_t used = 0;
  int status = 0;
  int conn_close = 0;
  struct pollfd pfd;
  char b;
  if (fd < 0) {
    fail("idle-after", "connect");
    return;
  }
  if (send_all(fd, "GET /health HTTP/1.1\r\nHost: t\r\n\r\n") < 0 ||
      read_response(fd, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0 ||
      status != 200) {
    fail("idle-after", "response");
    close(fd);
    return;
  }
  pfd.fd = fd;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, 4000) <= 0) {
    fail("idle-after", "timed out");
    close(fd);
    return;
  }
  if (read(fd, &b, 1) != 0) fail("idle-after", "not eof");
  close(fd);
}

/* Headers with Content-Length, then the body in a later write.
   The image stays in need-more until those bytes arrive. The 401
   closes, the same as any other 4xx. */
static void check_split_body(int port) {
  int fd = dial(port);
  struct pollfd pfd;
  char buf[4096];
  char body[64];
  size_t body_n = 0;
  size_t used = 0;
  int status = 0;
  int conn_close = 0;
  int pr;
  if (fd < 0) {
    fail("split-body", "connect");
    return;
  }
  if (send_all(fd, "POST /posts HTTP/1.1\r\nContent-Length: 2\r\nHost: t\r\n\r\n") < 0) {
    fail("split-body", "headers");
    close(fd);
    return;
  }
  pfd.fd = fd;
  pfd.events = POLLIN;
  pr = poll(&pfd, 1, 400);
  if (pr < 0) fail("split-body", "poll");
  if (pr > 0) fail("split-body", "responded before the body");
  if (send_all(fd, "{}") < 0 ||
      read_response(fd, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("split-body", "response");
    close(fd);
    return;
  }
  if (status != 401 || strcmp(body, "missing-bearer") != 0 || !conn_close)
    fail("split-body", "status");
  close(fd);
}

/* The image leaves a short body in the buffer (need-more) until
   Content-Length is met, and it calls a longer unfinished header
   malformed at 16384. MURO_TEST_IN_CAP=512 fills the host buffer
   while the body is still short. The host closes. That close is not
   a status line. */
static int send_buf(int fd, const char *p, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = send(fd, p + off, n - off, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

static void check_full_buffer(void) {
  pid_t pid;
  int port = -1;
  int fd = -1;
  char msg[2048];
  size_t hdr_n;
  char got[64];
  size_t nread = 0;
  int dead = 0;
  int budget = 2000;
  int saw_http = 0;
  pid = start_server(10000, 16, &port, NULL, NULL, "512");
  if (pid < 0) return;
  fd = dial(port);
  if (fd < 0) {
    fail("full-buffer", "connect");
    stop_server(pid);
    return;
  }
  hdr_n = (size_t)snprintf(msg, sizeof msg, "POST /posts HTTP/1.1\r\nContent-Length: 2000\r\nHost: t\r\n\r\n");
  if (hdr_n == 0 || hdr_n >= sizeof msg) {
    fail("full-buffer", "setup");
    close(fd);
    stop_server(pid);
    return;
  }
  memset(msg + hdr_n, 'x', sizeof msg - hdr_n);
  if (send_buf(fd, msg, sizeof msg) < 0 && errno != EPIPE && errno != ECONNRESET)
    fail("full-buffer", "send");
  while (budget > 0 && !dead && !saw_http) {
    struct pollfd pfd;
    ssize_t r;
    int pr;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pr = poll(&pfd, 1, 100);
    if (pr < 0 && errno == EINTR) continue;
    if (pr == 0) {
      budget -= 100;
      continue;
    }
    if (pr < 0) break;
    r = read(fd, got, sizeof got);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (r < 0 && (errno == ECONNRESET || errno == EPIPE)) {
      dead = 1;
      break;
    }
    if (r < 0) break;
    if (r == 0) {
      dead = 1;
      break;
    }
    nread = (size_t)r;
    saw_http = 1;
    break;
  }
  if (!dead || saw_http) {
    fprintf(stderr, "FAIL full-buffer: dead %d bytes %zu\n", dead, nread);
    failures += 1;
  }
  close(fd);
  if (stop_server(pid) < 0) return;
}

static void check_max_conn(void) {
  pid_t pid;
  int port = -1;
  int fd1;
  int fd2;
  struct pollfd pfd;
  char buf[4096];
  char body[64];
  size_t body_n = 0;
  size_t used = 0;
  int status = 0;
  int conn_close = 0;
  int pr;
  pid = start_server(10000, 1, &port, NULL, NULL, NULL);
  if (pid < 0) return;
  fd1 = dial(port);
  if (fd1 < 0 || send_all(fd1, "GET /health HTTP/1.1\r\nHost: t\r\n\r\n") < 0 ||
      read_response(fd1, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0 ||
      status != 200) {
    fail("max-conn", "first");
    if (fd1 >= 0) close(fd1);
    stop_server(pid);
    return;
  }
  fd2 = dial(port);
  if (fd2 < 0 || send_all(fd2, "GET /health HTTP/1.1\r\nHost: t\r\n\r\n") < 0) {
    fail("max-conn", "second connect");
    close(fd1);
    if (fd2 >= 0) close(fd2);
    stop_server(pid);
    return;
  }
  pfd.fd = fd2;
  pfd.events = POLLIN;
  pr = poll(&pfd, 1, 800);
  if (pr < 0) fail("max-conn", "poll");
  if (pr > 0) fail("max-conn", "served while the cap was full");
  close(fd1);
  if (pr == 0) {
    if (read_response(fd2, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                      &body_n) < 0 ||
        status != 200 || strcmp(body, "ok") != 0)
      fail("max-conn", "after a slot freed");
  }
  close(fd2);
  if (stop_server(pid) < 0) return;
}

static const char health_keep[] =
    "HTTP/1.1 200 OK\r\nDate: Sun, 06 Nov 1994 08:49:37 GMT\r\n"
    "Connection: keep-alive\r\nContent-Length: 2\r\n"
    "Content-Type: text/plain; charset=utf-8\r\n\r\nok";

static void check_partial_send(void) {
  pid_t pid;
  int port = -1;
  int fd;
  char tmpl[] = "/tmp/muro-httpd-wlog-XXXXXX";
  int tfd;
  char buf[4096];
  char body[64];
  size_t body_n = 0;
  size_t used = 0;
  int status = 0;
  int conn_close = 0;
  FILE *log;
  char line[64];
  size_t sum = 0;
  int nwrite = 0;
  int tries;
  int bad_line = 0;
  size_t want;
  tfd = mkstemp(tmpl);
  if (tfd < 0) {
    fail("partial-send", "log");
    return;
  }
  close(tfd);
  pid = start_server(10000, 16, &port, "1", tmpl, NULL);
  if (pid < 0) {
    unlink(tmpl);
    return;
  }
  fd = dial(port);
  if (fd < 0 || send_chunks(fd, "GET /health HTTP/1.1\r\nHost: t\r\n\r\n", 1) < 0 ||
      read_response(fd, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("partial-send", "response");
    if (fd >= 0) close(fd);
    stop_server(pid);
    unlink(tmpl);
    return;
  }
  close(fd);
  if (status != 200 || strcmp(body, "ok") != 0 || conn_close) fail("partial-send", "status");
  want = strlen(health_keep);
  /* note_write runs after write() returns. The response can arrive
     before that last line is in the log. Re-read until it catches up. */
  for (tries = 0; tries < 100; tries++) {
    bad_line = 0;
    sum = 0;
    nwrite = 0;
    log = fopen(tmpl, "r");
    if (log) {
      while (fgets(line, sizeof line, log)) {
        char *end = NULL;
        long v = strtol(line, &end, 10);
        if (end == line || v != 1) {
          fprintf(stderr, "FAIL partial-send: write '%s'\n", line);
          failures += 1;
          bad_line = 1;
          break;
        }
        sum += 1;
        nwrite += 1;
      }
      fclose(log);
      if (bad_line) break;
      if (nwrite >= 2 && sum == want) break;
    }
    poll(NULL, 0, 10);
  }
  if (!bad_line && (nwrite < 2 || sum != want)) {
    fprintf(stderr, "FAIL partial-send: writes %d sum %zu want %zu\n", nwrite, sum, want);
    failures += 1;
  }
  unlink(tmpl);
  if (stop_server(pid) < 0) return;
}

/* HS256 fixtures signed with the test key muro-http-test-key. */
static const char tok_ada[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEifQ.3KE6Im9uhSMiXa5L1JtMwmzrpKhdX6M3BvI4x3brFts";
static const char tok_exp[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEiLCJleHAiOjk5OTk5OTk5OTl9.Epjenj7XS31B0sLSJP-s-TiWA-YPjZ5NC6IhmtQKqB0";
static const char tok_nbf[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEiLCJuYmYiOjF9.BH2rRpEPRSrkyv8B4BUpQODlzBSGPofoKu-_vTsIIwE";
static const char tok_exp_only[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJleHAiOjF9.6O0vDw5xVpA0Vc5reohZXUgr71QOA86t3Yd4R7QHPyg";
static const char tok_empty_sub[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiIifQ.3cHkEIQwrbEhQ0j2DFdg1fDQV24zE0me48Cvz9_oZSM";
static const char tok_sub_num[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOjF9.aQRXrUwRnMtL8wGhhFGC3iMJElpzIx6mkvG99u0Mxzw";
static const char tok_expired[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEiLCJleHAiOjF9.x6mPeg8JrI4FpgdQ5GgdiH0Ba__Ig0COpNp_LIZu0NU";
static const char tok_nbf_far[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEiLCJuYmYiOjk5OTk5OTk5OTl9.uAiBGBuaNzU9ec4vg5IwpVrAF4Syhl4HmKZQp_SZATg";
static const char tok_none[] =
    "eyJhbGciOiJub25lIiwidHlwIjoiSldUIn0.eyJzdWIiOiJhZGEifQ.8NI1blacf6p2nNGEpD22a4xFD8e3zVHm1ClUuEbbzGs";
static const char tok_bad[] =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJzdWIiOiJhZGEifQ.3KE6Im9uhSMiXa5L1JtMwmzrpKhdX6M3BvI4x3brFtA";

static void post_json(char *dst, size_t cap, const char *token, const char *json) {
  if (token && token[0]) {
    snprintf(dst, cap,
             "POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: %zu\r\nHost: t\r\n\r\n%s",
             token, strlen(json), json);
  } else {
    snprintf(dst, cap, "POST /posts HTTP/1.1\r\nContent-Length: %zu\r\nHost: t\r\n\r\n%s", strlen(json), json);
  }
}

static const char b64tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int b64url_enc(const uint8_t *in, size_t n, char *dst, size_t cap) {
  size_t i = 0;
  size_t o = 0;
  while (i + 2 < n) {
    unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | (unsigned)in[i + 2];
    if (o + 4 >= cap) return -1;
    dst[o++] = b64tab[(v >> 18) & 63];
    dst[o++] = b64tab[(v >> 12) & 63];
    dst[o++] = b64tab[(v >> 6) & 63];
    dst[o++] = b64tab[v & 63];
    i += 3;
  }
  if (i < n) {
    unsigned v = (unsigned)in[i] << 16;
    int rem = (int)(n - i);
    if (rem == 2) v |= (unsigned)in[i + 1] << 8;
    if (o + (rem == 2 ? 3 : 2) >= cap) return -1;
    dst[o++] = b64tab[(v >> 18) & 63];
    dst[o++] = b64tab[(v >> 12) & 63];
    if (rem == 2) dst[o++] = b64tab[(v >> 6) & 63];
  }
  if (o >= cap) return -1;
  dst[o] = 0;
  return (int)o;
}

static int sign_hs(char *dst, size_t cap, const char *header, const char *payload) {
  char hb[256];
  char pb[512];
  char sb[64];
  char mac_in[800];
  uint8_t mac[32];
  int hn;
  int pn;
  int sn;
  int n;
  const char *key = "muro-http-test-key";
  if (!header) header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
  hn = b64url_enc((const uint8_t *)header, strlen(header), hb, sizeof hb);
  pn = b64url_enc((const uint8_t *)payload, strlen(payload), pb, sizeof pb);
  if (hn < 0 || pn < 0) return -1;
  n = snprintf(mac_in, sizeof mac_in, "%s.%s", hb, pb);
  if (n < 0 || (size_t)n >= sizeof mac_in) return -1;
  jwt_hmac_sha256((const uint8_t *)key, strlen(key), (const uint8_t *)mac_in, (size_t)n, mac);
  sn = b64url_enc(mac, 32, sb, sizeof sb);
  if (sn < 0) return -1;
  n = snprintf(dst, cap, "%s.%s", mac_in, sb);
  if (n < 0 || (size_t)n >= cap) return -1;
  return n;
}

static void unlink_db(const char *path) {
  char side[256];
  if (!path || !path[0]) return;
  unlink(path);
  snprintf(side, sizeof side, "%s-wal", path);
  unlink(side);
  snprintf(side, sizeof side, "%s-shm", path);
  unlink(side);
}

static void post_like(char *dst, size_t cap, const char *id, const char *token) {
  if (token && token[0]) {
    snprintf(dst, cap, "POST /posts/%s/like HTTP/1.1\r\nAuthorization: Bearer %s\r\nHost: t\r\n\r\n", id, token);
  } else {
    snprintf(dst, cap, "POST /posts/%s/like HTTP/1.1\r\nHost: t\r\n\r\n", id);
  }
}

static void check_routes(int port) {
  char req[4096];
  char longbody[1200];
  char tok[800];
  char payload[96];
  int64_t edge_now = (int64_t)time(NULL);
  snprintf(payload, sizeof payload, "{\"sub\":\"ada\",\"exp\":%lld}", (long long)edge_now);
  if (sign_hs(tok, sizeof tok, NULL, payload) < 0) fail("exp-eq", "sign");
  else {
    post_json(req, sizeof req, tok, "{\"text\":\"nope\"}");
    check_one(port, "exp-eq", req, 401, "bad-payload", 1, 1);
  }
  snprintf(payload, sizeof payload, "{\"sub\":\"ada\",\"nbf\":%lld}", (long long)edge_now);
  if (sign_hs(tok, sizeof tok, NULL, payload) < 0) fail("nbf-eq", "sign");
  else {
    post_json(req, sizeof req, tok, "{}");
    check_one(port, "nbf-eq", req, 400, "body-required", 1, 1);
  }
  if (sign_hs(tok, sizeof tok, NULL, "{\"sub\":\"ada\",\"iat\":1.5}") < 0) fail("float-iat", "sign");
  else {
    post_json(req, sizeof req, tok, "{}");
    check_one(port, "float-iat", req, 400, "body-required", 1, 1);
  }
  if (sign_hs(tok, sizeof tok, NULL, "{\"sub\":\"ada\",\"exp\":1.5}") < 0) fail("float-exp", "sign");
  else {
    post_json(req, sizeof req, tok, "{\"text\":\"nope\"}");
    check_one(port, "float-exp", req, 401, "bad-payload", 1, 1);
  }
  if (sign_hs(tok, sizeof tok, "{\"alg\":\"HS256\",\"typ\":\"jwt\"}", "{\"sub\":\"ada\"}") < 0)
    fail("typ-lower", "sign");
  else {
    post_json(req, sizeof req, tok, "{}");
    check_one(port, "typ-lower", req, 400, "body-required", 1, 1);
  }
  if (sign_hs(tok, sizeof tok, "{\"alg\":\"HS256\"}", "{\"sub\":\"ada\"}") < 0) fail("typ-absent", "sign");
  else {
    post_json(req, sizeof req, tok, "{}");
    check_one(port, "typ-absent", req, 400, "body-required", 1, 1);
  }
  if (sign_hs(tok, sizeof tok, "{\"alg\":\"HS256\",\"typ\":\"JWS\"}", "{\"sub\":\"ada\"}") < 0)
    fail("typ-jws", "sign");
  else {
    post_json(req, sizeof req, tok, "{\"text\":\"nope\"}");
    check_one(port, "typ-jws", req, 401, "bad-token", 1, 1);
  }
  post_json(req, sizeof req, NULL, "{}");
  check_one(port, "post-no-auth", req, 401, "missing-bearer", 1, 1);
  post_json(req, sizeof req, tok_bad, "{\"text\":\"hi\"}");
  check_one(port, "post-bad-token", req, 401, "bad-token", 1, 1);
  post_json(req, sizeof req, tok_none, "{\"text\":\"hi\"}");
  check_one(port, "post-alg-none", req, 401, "bad-token", 1, 1);
  snprintf(req, sizeof req,
           "POST /posts HTTP/1.1\r\nAuthorization: bearer %s\r\nContent-Length: 13\r\nHost: t\r\n\r\n"
           "{\"text\":\"hi\"}",
           tok_ada);
  check_one(port, "post-lower-bearer", req, 401, "bad-token", 1, 1);
  check_one(port, "post-empty-bearer",
            "POST /posts HTTP/1.1\r\nAuthorization: Bearer \r\nContent-Length: 13\r\nHost: t\r\n\r\n"
            "{\"text\":\"hi\"}",
            401, "bad-token", 1, 1);
  post_json(req, sizeof req, tok_empty_sub, "{\"text\":\"hi\"}");
  check_one(port, "post-empty-sub", req, 401, "bad-payload", 1, 1);
  post_json(req, sizeof req, tok_sub_num, "{\"text\":\"hi\"}");
  check_one(port, "post-sub-num", req, 401, "bad-payload", 1, 1);
  post_json(req, sizeof req, tok_expired, "{\"text\":\"hi\"}");
  check_one(port, "post-expired", req, 401, "bad-payload", 1, 1);
  post_json(req, sizeof req, tok_nbf_far, "{\"text\":\"hi\"}");
  check_one(port, "post-nbf", req, 401, "bad-payload", 1, 1);
  post_json(req, sizeof req, tok_exp_only, "{\"text\":\"hi\"}");
  check_one(port, "post-no-sub", req, 401, "bad-payload", 1, 1);
  post_json(req, sizeof req, tok_ada, "{}");
  check_one(port, "post-empty-obj", req, 400, "body-required", 1, 1);
  post_json(req, sizeof req, tok_ada, "{");
  check_one(port, "post-trunc", req, 400, "malformed", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"\"}");
  check_one(port, "post-empty-text", req, 400, "body-required", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":1}");
  check_one(port, "post-num", req, 400, "body-required", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"a\":{\"text\":\"hi\"}}");
  check_one(port, "post-nested", req, 400, "body-required", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"\\u0074ext\":\"hi\"}");
  check_one(port, "post-key-esc", req, 400, "body-required", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"a\",\"text\":\"b\"}");
  check_one(port, "post-dup", req, 400, "malformed", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"\\uD800\"}");
  check_one(port, "post-surrogate", req, 400, "malformed", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}x");
  check_one(port, "post-trail", req, 400, "malformed", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\",}");
  check_one(port, "post-comma", req, 400, "malformed", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"\xff\"}");
  check_one(port, "post-utf8", req, 400, "malformed", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"\n\"}");
  check_one(port, "post-control", req, 400, "malformed", 1, 1);
  memset(longbody, 'x', sizeof longbody);
  memcpy(longbody, "{\"text\":\"", 9);
  memcpy(longbody + 9 + 1100, "\"}", 2);
  longbody[9 + 1100 + 2] = 0;
  post_json(req, sizeof req, tok_ada, longbody);
  if (strlen(longbody) <= 1024) fail("post-long", "fixture");
  check_one(port, "post-long", req, 400, "body-long", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "create", req, 201, "{\"id\":1,\"text\":\"hi\"}", 0, 0);
  post_json(req, sizeof req, tok_exp, "{\"text\":\"\\u0041\"}");
  check_one(port, "create-exp", req, 201, "{\"id\":2,\"text\":\"A\"}", 0, 0);
  post_json(req, sizeof req, tok_nbf, "{\"text\":\"a\\\"b\"}");
  check_one(port, "create-nbf", req, 201, "{\"id\":3,\"text\":\"a\\\"b\"}", 0, 0);
  check_one(port, "read-1", "GET /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n", 200, "{\"id\":1,\"text\":\"hi\"}", 0, 0);
  check_one(port, "read-2", "GET /posts/2 HTTP/1.1\r\nHost: t\r\n\r\n", 200, "{\"id\":2,\"text\":\"A\"}", 0, 0);
  check_one(port, "read-missing", "GET /posts/9 HTTP/1.1\r\nHost: t\r\n\r\n", 404, "post-missing", 1, 1);
  check_one(port, "read-zero", "GET /posts/0 HTTP/1.1\r\nHost: t\r\n\r\n", 400, "invalid-id", 1, 1);
  check_one(port, "read-pad", "GET /posts/01 HTTP/1.1\r\nHost: t\r\n\r\n", 400, "invalid-id", 1, 1);
  check_one(port, "get-posts", "GET /posts HTTP/1.1\r\nHost: t\r\n\r\n", 404, "not-found", 1, 1);
  snprintf(req, sizeof req,
           "POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\nHost: t\r\n\r\n", tok_ada);
  check_one(port, "post-empty-body", req, 400, "malformed", 1, 1);
  check_one(port, "post-on-id", "POST /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n", 404, "not-found", 1, 1);
  post_like(req, sizeof req, "1", NULL);
  check_one(port, "like-no-auth", req, 401, "missing-bearer", 1, 1);
  post_like(req, sizeof req, "1", tok_expired);
  check_one(port, "like-bad-payload", req, 401, "bad-payload", 1, 1);
  post_like(req, sizeof req, "1", tok_bad);
  check_one(port, "like-bad-token", req, 401, "bad-token", 1, 1);
  post_like(req, sizeof req, "0", tok_ada);
  check_one(port, "like-bad-id", req, 400, "invalid-id", 1, 1);
  post_like(req, sizeof req, "1", tok_ada);
  check_one(port, "like-new", req, 201, "{\"id\":1,\"likes\":1}", 0, 0);
  post_like(req, sizeof req, "1", tok_ada);
  check_one(port, "like-again", req, 200, "{\"id\":1,\"likes\":1}", 0, 0);
  post_like(req, sizeof req, "9", tok_ada);
  check_one(port, "like-missing", req, 404, "post-missing", 1, 1);
  check_one(port, "feed-rows", "GET /feed HTTP/1.1\r\nHost: t\r\n\r\n", 200,
            "[{\"id\":1,\"text\":\"hi\"},{\"id\":2,\"text\":\"A\"},{\"id\":3,\"text\":\"a\\\"b\"}]", 0, 0);
  check_one(port, "post-health", "POST /health HTTP/1.1\r\nHost: t\r\n\r\n", 404, "not-found", 1, 1);
  check_one(port, "post-feed", "POST /feed HTTP/1.1\r\nHost: t\r\n\r\n", 404, "not-found", 1, 1);
  check_one(port, "get-like", "GET /posts/1/like HTTP/1.1\r\nHost: t\r\n\r\n", 404, "not-found", 1, 1);
  check_one(port, "put-health", "PUT /health HTTP/1.1\r\nHost: t\r\n\r\n", 404, "not-found", 1, 1);
  snprintf(req, sizeof req, "GET /health HTTP/1.1\r\nAuthorization: Bearer %s\r\nHost: t\r\n\r\n", tok_bad);
  check_one(port, "health-bad-auth", req, 200, "ok", 0, 0);
}

static void check_db_down(void) {
  pid_t pid;
  int port = -1;
  char req[1024];
  const char *saved = srv_db;
  srv_db = "/no/such/directory/muro-http.db";
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  srv_db = saved;
  if (pid < 0) return;
  check_one(port, "down-health", "GET /health HTTP/1.1\r\nHost: t\r\n\r\n", 503, "db", 1, 1);
  check_one(port, "down-feed", "GET /feed HTTP/1.1\r\nHost: t\r\n\r\n", 500, "internal", 1, 1);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "down-create", req, 500, "internal", 1, 1);
  check_one(port, "down-read", "GET /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n", 500, "internal", 1, 1);
  post_like(req, sizeof req, "1", tok_ada);
  check_one(port, "down-like", req, 500, "internal", 1, 1);
  if (stop_server(pid) < 0) return;
}

static void check_memory(void) {
  pid_t pid;
  int port = -1;
  char req[1024];
  const char *saved = srv_db;
  srv_db = ":memory:";
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  srv_db = saved;
  if (pid < 0) return;
  check_one(port, "memory-health", "GET /health HTTP/1.1\r\nHost: t\r\n\r\n", 200, "ok", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "memory-create", req, 201, "{\"id\":1,\"text\":\"hi\"}", 0, 0);
  check_one(port, "memory-read", "GET /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n", 200, "{\"id\":1,\"text\":\"hi\"}", 0,
            0);
  if (stop_server(pid) < 0) return;
}

static void check_reopen(void) {
  char path[] = "/tmp/muro-http-reopen-XXXXXX";
  char req[1024];
  const char *saved = srv_db;
  pid_t pid;
  int port = -1;
  int fd = mkstemp(path);
  if (fd < 0) {
    fail("reopen", "mkstemp");
    return;
  }
  close(fd);
  srv_db = path;
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  if (pid < 0) {
    srv_db = saved;
    unlink_db(path);
    return;
  }
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "reopen-create", req, 201, "{\"id\":1,\"text\":\"hi\"}", 0, 0);
  if (stop_server(pid) < 0) {
    srv_db = saved;
    unlink_db(path);
    return;
  }
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  if (pid < 0) {
    srv_db = saved;
    unlink_db(path);
    return;
  }
  check_one(port, "reopen-read", "GET /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n", 200, "{\"id\":1,\"text\":\"hi\"}", 0,
            0);
  stop_server(pid);
  srv_db = saved;
  unlink_db(path);
}

static void check_precedence(void) {
  char file_a[] = "/tmp/muro-http-dba-XXXXXX";
  char file_b[] = "/tmp/muro-http-dbb-XXXXXX";
  char req[1024];
  const char *saved = srv_db;
  pid_t pid;
  int port = -1;
  int fd_a = mkstemp(file_a);
  int fd_b = mkstemp(file_b);
  if (fd_a < 0 || fd_b < 0) {
    fail("precedence", "mkstemp");
    if (fd_a >= 0) {
      close(fd_a);
      unlink_db(file_a);
    }
    if (fd_b >= 0) {
      close(fd_b);
      unlink_db(file_b);
    }
    return;
  }
  close(fd_a);
  close(fd_b);
  srv_db = file_a;
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  if (pid < 0) {
    srv_db = saved;
    unlink_db(file_a);
    unlink_db(file_b);
    return;
  }
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "prec-create", req, 201, "{\"id\":1,\"text\":\"hi\"}", 0, 0);
  if (stop_server(pid) < 0) {
    srv_db = saved;
    unlink_db(file_a);
    unlink_db(file_b);
    return;
  }
  srv_db = ":memory:";
  env_sqlite = file_a;
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  if (pid >= 0) {
    check_one(port, "prec-flag", "GET /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n", 404, "post-missing", 1, 1);
    stop_server(pid);
  }
  pass_db = 0;
  env_sqlite = file_a;
  env_muro_db = file_b;
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  if (pid >= 0) {
    check_one(port, "prec-muro-db", "GET /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n", 404, "post-missing", 1, 1);
    stop_server(pid);
  }
  pass_db = 0;
  env_sqlite = file_a;
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  if (pid >= 0) {
    check_one(port, "prec-sqlite-path", "GET /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n", 200,
              "{\"id\":1,\"text\":\"hi\"}", 0, 0);
    stop_server(pid);
  }
  srv_db = saved;
  unlink_db(file_a);
  unlink_db(file_b);
}

/* An empty path is rejected before listen. Exit 2, no banner. */
static void check_exit2(const char *name, char **av, const char *sqlite_env, const char *muro_env,
                        const char *err_needle) {
  int sp[2];
  pid_t pid;
  char buf[512];
  size_t n = 0;
  int status = 0;
  int i;
  int reaped = 0;
  if (pipe(sp) < 0) {
    fail(name, "pipe");
    return;
  }
  pid = fork();
  if (pid < 0) {
    close(sp[0]);
    close(sp[1]);
    fail(name, "fork");
    return;
  }
  if (pid == 0) {
    alarm(0);
    if (dup2(sp[1], STDOUT_FILENO) < 0) _exit(127);
    if (dup2(sp[1], STDERR_FILENO) < 0) _exit(127);
    close(sp[0]);
    close(sp[1]);
    unsetenv("MURO_HTTP_BIND");
    unsetenv("MURO_HTTP_DB");
    unsetenv("MURO_HTTP_JWT_KEY");
    unsetenv("MURO_HTTP_PORT");
    unsetenv("MURO_HTTP_IDLE_MS");
    unsetenv("MURO_HTTP_MAX_CONNECTIONS");
    unsetenv("MURO_HTTP_MAX_BODY");
    unsetenv("MURO_HTTP_DRAIN_MS");
    unsetenv("MURO_HTTP_WORKERS");
    unsetenv("MURO_HTTP_WAIT");
    unsetenv("SQLITE_PATH");
    unsetenv("MURO_TEST_FAIL_DB_ADMIT");
    unsetenv("MURO_TEST_WRITE_LIMIT");
    unsetenv("MURO_TEST_WRITE_LOG");
    unsetenv("MURO_TEST_IN_CAP");
    if (sqlite_env) setenv("SQLITE_PATH", sqlite_env, 1);
    if (muro_env) setenv("MURO_HTTP_DB", muro_env, 1);
    execv(bin_path, av);
    _exit(127);
  }
  close(sp[1]);
  for (;;) {
    struct pollfd pfd;
    ssize_t r;
    if (n + 1 >= sizeof buf) break;
    pfd.fd = sp[0];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 2000) <= 0) break;
    r = read(sp[0], buf + n, sizeof buf - 1 - n);
    if (r <= 0) break;
    n += (size_t)r;
  }
  close(sp[0]);
  buf[n] = 0;
  for (i = 0; i < 40; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      reaped = 1;
      break;
    }
    if (got < 0) break;
    usleep(50000);
  }
  if (!reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    fail(name, "still running");
    return;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 2) {
    fprintf(stderr, "FAIL %s: status %d out '%s'\n", name, status, buf);
    failures += 1;
    return;
  }
  if (strstr(buf, "listening") != NULL) fail(name, "listened");
  if (err_needle && strstr(buf, err_needle) == NULL) {
    fprintf(stderr, "FAIL %s: stderr '%s'\n", name, buf);
    failures += 1;
  }
}

/* --bind wins over MURO_HTTP_BIND. The environment value is not an
   address, so a listening banner means the flag was applied. */
static void check_bind(void) {
  const char *saved_db = srv_db;
  pid_t pid;
  int port = -1;
  srv_db = ":memory:";
  srv_bind = "127.0.0.1";
  srv_bind_env = "nope";
  pid = start_server(5000, 4, &port, NULL, NULL, NULL);
  srv_db = saved_db;
  if (pid < 0) return;
  check_one(port, "bind", "GET /health HTTP/1.1\r\nHost: t\r\n\r\n", 200, "ok", 0, 0);
  stop_server(pid);
}

static void check_empty_db(void) {
  char *flag[] = {"muro-httpd", "--port", "0", "--db", "", NULL};
  char *envonly[] = {"muro-httpd", "--port", "0", NULL};
  check_exit2("empty-db-flag", flag, NULL, NULL, "bad --db");
  check_exit2("empty-muro-db", envonly, NULL, "", "bad MURO_HTTP_DB");
  check_exit2("empty-sqlite-path", envonly, "", NULL, "bad SQLITE_PATH");
}

/* MURO_TEST_FAIL_DB_ADMIT refuses the evDb admit after the store
   commit. The client still gets 500 with Connection: close, and the
   row is still there. */
static void check_commit_status(void) {
  char path[] = "/tmp/muro-http-admit-XXXXXX";
  char req[1024];
  const char *saved = srv_db;
  pid_t pid;
  int port = -1;
  int fd = mkstemp(path);
  if (fd < 0) {
    fail("commit-status", "mkstemp");
    return;
  }
  close(fd);
  srv_db = path;
  env_fail_admit = "2";
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  if (pid < 0) {
    srv_db = saved;
    unlink_db(path);
    return;
  }
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "commit-create", req, 500, "internal", 1, 1);
  post_like(req, sizeof req, "1", tok_ada);
  check_one(port, "commit-like", req, 500, "internal", 1, 1);
  check_one(port, "commit-read", "GET /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n", 200, "{\"id\":1,\"text\":\"hi\"}", 0,
            0);
  post_like(req, sizeof req, "1", tok_ada);
  check_one(port, "commit-like-again", req, 200, "{\"id\":1,\"likes\":1}", 0, 0);
  stop_server(pid);
  srv_db = saved;
  unlink_db(path);
}

static void check_no_key(void) {
  pid_t pid;
  int port = -1;
  char req[1024];
  const char *saved = srv_jwt;
  srv_jwt = NULL;
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  srv_jwt = saved;
  if (pid < 0) return;
  check_one(port, "nokey-health", "GET /health HTTP/1.1\r\nHost: t\r\n\r\n", 200, "ok", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "nokey-token", req, 401, "bad-token", 1, 1);
  check_one(port, "nokey-absent", "POST /posts HTTP/1.1\r\nContent-Length: 2\r\nHost: t\r\n\r\n{}", 401,
            "missing-bearer", 1, 1);
  if (stop_server(pid) < 0) return;
}

/* One response already on the socket. want_body NULL checks only the
   status line. A JSON body must carry the JSON content type. */
static int take_resp(int fd, const char *name, int want_status, const char *want_body, int want_close,
                     int want_eof) {
  char buf[8192];
  char body[4096];
  size_t body_n = 0;
  size_t used = 0;
  int status = 0;
  int conn_close = 0;
  struct pollfd pfd;
  int pr;
  pfd.fd = fd;
  pfd.events = POLLIN;
  pr = poll(&pfd, 1, 2000);
  if (pr < 0) {
    fail(name, "poll");
    return -1;
  }
  if (pr == 0) {
    fail(name, "timed out");
    return -1;
  }
  if (read_response(fd, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail(name, "response");
    return -1;
  }
  if (status != want_status) {
    fprintf(stderr, "FAIL %s: status %d body '%s'\n", name, status, body);
    failures += 1;
    return -1;
  }
  if (want_body && strcmp(body, want_body) != 0) {
    fprintf(stderr, "FAIL %s: body '%s'\n", name, body);
    failures += 1;
    return -1;
  }
  if (want_body && (want_body[0] == '{' || want_body[0] == '[') && !last_json)
    fail(name, "content-type");
  if (conn_close != want_close) {
    fail(name, want_close ? "wanted close" : "wanted keep-alive");
    return -1;
  }
  if (want_eof && !expect_eof(fd)) {
    fail(name, "expected eof");
    return -1;
  }
  return 0;
}

static int expect_silent(int fd, const char *name) {
  struct pollfd pfd;
  int pr;
  pfd.fd = fd;
  pfd.events = POLLIN;
  pr = poll(&pfd, 1, 200);
  if (pr < 0) {
    fail(name, "poll");
    return -1;
  }
  if (pr > 0) {
    fail(name, "responded early");
    return -1;
  }
  return 0;
}

/* A partial header and a partial body stay open. Other sockets still
   serve health, feed, create, and like. The partial header is then
   finished on the same connection. */
static void check_interleaved(void) {
  char path[] = "/tmp/muro-http-interleave-XXXXXX";
  char req[2048];
  char feed_req[1600];
  const char *saved = srv_db;
  const char *partial = "GET /health HTTP/1.1\r\nHost: slow\r\n";
  const char *part_body = "POST /posts HTTP/1.1\r\nContent-Length: 20\r\nHost: t\r\n\r\n{\"tex";
  pid_t pid;
  int port = -1;
  int slow = -1;
  int body = -1;
  int health = -1;
  int feed = -1;
  int post = -1;
  int bad = -1;
  int like = -1;
  int readp = -1;
  int feed2 = -1;
  int dbfd;
  size_t i;
  dbfd = mkstemp(path);
  if (dbfd < 0) {
    fail("interleave", "mkstemp");
    return;
  }
  close(dbfd);
  srv_db = path;
  pid = start_server(10000, 8, &port, NULL, NULL, NULL);
  srv_db = saved;
  if (pid < 0) {
    unlink_db(path);
    return;
  }
  slow = dial(port);
  body = dial(port);
  if (slow < 0 || body < 0) {
    fail("interleave", "connect");
    goto done;
  }
  for (i = 0; partial[i]; i++) {
    if (send_buf(slow, partial + i, 1) < 0) {
      fail("interleave", "drip");
      goto done;
    }
    if ((i % 8) == 7)
      check_one(port, "interleave-drip", "GET /health HTTP/1.1\r\nHost: t\r\n\r\n", 200, "ok", 0, 0);
  }
  if (send_all(body, part_body) < 0) {
    fail("interleave", "partial body");
    goto done;
  }
  if (expect_silent(slow, "interleave-slow") < 0) goto done;
  if (expect_silent(body, "interleave-body") < 0) goto done;

  health = dial(port);
  feed = dial(port);
  post = dial(port);
  bad = dial(port);
  if (health < 0 || feed < 0 || post < 0 || bad < 0) {
    fail("interleave", "fast connect");
    goto done;
  }
  snprintf(feed_req, sizeof feed_req, "GET /feed HTTP/1.1\r\nAuthorization: Bearer %s\r\nHost: t\r\n\r\n", tok_ada);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"concurrent\"}");
  if (send_all(health, "GET /health HTTP/1.1\r\nHost: t\r\n\r\n") < 0 || send_all(feed, feed_req) < 0 ||
      send_all(post, req) < 0 || send_all(bad, "GET/feed HTTP/1.1\r\n\r\n") < 0) {
    fail("interleave", "fast send");
    goto done;
  }
  if (take_resp(health, "interleave-health", 200, "ok", 0, 0) < 0) goto done;
  if (take_resp(feed, "interleave-feed", 200, "[]", 0, 0) < 0) goto done;
  if (take_resp(post, "interleave-create", 201, "{\"id\":1,\"text\":\"concurrent\"}", 0, 0) < 0)
    goto done;
  if (take_resp(bad, "interleave-bad", 400, "malformed", 1, 1) < 0) goto done;
  close(health);
  close(feed);
  close(post);
  close(bad);
  health = feed = post = bad = -1;
  if (expect_silent(slow, "interleave-slow-held") < 0) goto done;
  if (expect_silent(body, "interleave-body-held") < 0) goto done;

  like = dial(port);
  readp = dial(port);
  feed2 = dial(port);
  if (like < 0 || readp < 0 || feed2 < 0) {
    fail("interleave", "second connect");
    goto done;
  }
  post_like(req, sizeof req, "1", tok_ada);
  snprintf(feed_req, sizeof feed_req, "GET /feed HTTP/1.1\r\nAuthorization: Bearer %s\r\nHost: t\r\n\r\n", tok_ada);
  if (send_all(like, req) < 0 || send_all(readp, "GET /posts/1 HTTP/1.1\r\nHost: t\r\n\r\n") < 0 ||
      send_all(feed2, feed_req) < 0) {
    fail("interleave", "second send");
    goto done;
  }
  if (take_resp(like, "interleave-like", 201, "{\"id\":1,\"likes\":1}", 0, 0) < 0) goto done;
  if (take_resp(readp, "interleave-read", 200, "{\"id\":1,\"text\":\"concurrent\"}", 0, 0) < 0)
    goto done;
  if (take_resp(feed2, "interleave-feed-row", 200, "[{\"id\":1,\"text\":\"concurrent\"}]", 0, 0) < 0)
    goto done;
  close(like);
  close(readp);
  close(feed2);
  like = readp = feed2 = -1;
  if (expect_silent(slow, "interleave-still") < 0) goto done;

  if (send_all(slow, "\r\n") < 0) {
    fail("interleave", "finish");
    goto done;
  }
  if (take_resp(slow, "interleave-finish", 200, "ok", 0, 0) < 0) goto done;
  if (expect_silent(body, "interleave-body-end") < 0) goto done;

  /* part_body left {"tex of a 20-byte JSON object. Fifteen more
     bytes complete it. No bearer, so the finished request is 401
     missing-bearer. */
  if (send_all(body, "t\":\"paused-ok\"}") < 0) {
    fail("interleave", "finish body");
    goto done;
  }
  if (take_resp(body, "interleave-body-finish", 401, "missing-bearer", 1, 1) < 0)
    goto done;

done:
  if (slow >= 0) close(slow);
  if (body >= 0) close(body);
  if (health >= 0) close(health);
  if (feed >= 0) close(feed);
  if (post >= 0) close(post);
  if (bad >= 0) close(bad);
  if (like >= 0) close(like);
  if (readp >= 0) close(readp);
  if (feed2 >= 0) close(feed2);
  stop_server(pid);
  unlink_db(path);
}

/* A client RST mid-response must close that slot and not spin the
   poll loop, so a later request on another socket is served at once. */
static int rst_close(int fd) {
  struct linger lg;
  lg.l_onoff = 1;
  lg.l_linger = 0;
  if (fd < 0) return -1;
  (void)setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  close(fd);
  return 0;
}

static void check_abort_write(void) {
  const char *saved = srv_db;
  pid_t pid;
  int port = -1;
  int fd = -1;
  char buf[16];
  ssize_t r;
  struct pollfd pfd;
  int pr;
  srv_db = ":memory:";
  pid = start_server(10000, 8, &port, "1", NULL, NULL);
  srv_db = saved;
  if (pid < 0) return;
  fd = dial(port);
  if (fd < 0 || send_all(fd, "GET /health HTTP/1.1\r\nHost: t\r\n\r\n") < 0) {
    fail("abort-write", "send");
    if (fd >= 0) close(fd);
    stop_server(pid);
    return;
  }
  pfd.fd = fd;
  pfd.events = POLLIN;
  pr = poll(&pfd, 1, 2000);
  if (pr <= 0) {
    fail("abort-write", "no response");
    close(fd);
    stop_server(pid);
    return;
  }
  r = recv(fd, buf, 1, 0);
  if (r <= 0) {
    fail("abort-write", "first byte");
    close(fd);
    stop_server(pid);
    return;
  }
  rst_close(fd);
  fd = -1;
  if (check_one(port, "abort-write-next", "GET /health HTTP/1.1\r\nHost: t\r\n\r\n", 200, "ok", 0, 0) < 0) {
    stop_server(pid);
    return;
  }
  if (stop_server(pid) < 0) return;
}

/* A partial header must not keep the process alive after the signal. */
static void check_signal(int sig, const char *name) {
  const char *saved = srv_db;
  pid_t pid;
  int port = -1;
  int fd = -1;
  srv_db = ":memory:";
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  srv_db = saved;
  if (pid < 0) return;
  fd = dial(port);
  if (fd < 0 || send_all(fd, "GET /hea") < 0) {
    fail(name, "partial");
    if (fd >= 0) close(fd);
    kill_server(pid);
    return;
  }
  if (stop_with(pid, sig, name) < 0) {
    close(fd);
    return;
  }
  close(fd);
}

int main(int argc, char **argv) {
  pid_t pid;
  int port = -1;
  char dbpath[] = "/tmp/muro-http-e2e-XXXXXX";
  int dbfd;
  if (argc > 1) bin_path = argv[1];
  signal(SIGPIPE, SIG_IGN);
  alarm(180);
  srv_jwt = "muro-http-test-key";
  dbfd = mkstemp(dbpath);
  if (dbfd < 0) {
    fail("main-db", "mkstemp");
    return 1;
  }
  close(dbfd);
  srv_db = dbpath;
  pid = start_server(10000, 16, &port, NULL, NULL, NULL);
  if (pid < 0) {
    unlink_db(dbpath);
    return 1;
  }
  check_one(port, "health", "GET /health HTTP/1.1\r\nHost: t\r\n\r\n", 200, "ok", 0, 0);
  check_keepalive(port);
  check_pipeline(port);
  check_one(port, "close", "GET /health HTTP/1.1\r\nConnection: close\r\nHost: t\r\n\r\n", 200, "ok", 1, 1);
  check_one(port, "malformed", "GET/feed HTTP/1.1\r\n\r\n", 400, "malformed", 1, 1);
  check_fin(port);
  check_one(port, "feed", "GET /feed HTTP/1.1\r\nHost: t\r\n\r\n", 200, "[]", 0, 0);
  check_one(port, "missing", "GET /missing HTTP/1.1\r\nHost: t\r\n\r\n", 404, "not-found", 1, 1);
  check_one(port, "post", "POST /posts HTTP/1.1\r\nContent-Length: 2\r\nHost: t\r\n\r\n{}", 401, "missing-bearer",
            1, 1);
  check_split_body(port);
  check_routes(port);
  check_full_buffer();
  if (stop_server(pid) < 0) {
    unlink_db(dbpath);
    return 1;
  }
  pid = start_server(300, 16, &port, NULL, NULL, NULL);
  if (pid < 0) {
    unlink_db(dbpath);
    return 1;
  }
  check_idle_bare(port);
  check_idle_after(port);
  if (stop_server(pid) < 0) {
    unlink_db(dbpath);
    return 1;
  }
  check_max_conn();
  check_partial_send();
  check_interleaved();
  check_abort_write();
  check_signal(SIGINT, "sigint");
  check_signal(SIGTERM, "sigterm");
  check_db_down();
  check_no_key();
  check_memory();
  check_reopen();
  check_precedence();
  check_empty_db();
  check_bind();
  check_commit_status();
  unlink_db(dbpath);
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
