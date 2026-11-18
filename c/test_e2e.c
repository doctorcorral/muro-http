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
#include <unistd.h>

static int failures;
static const char *bin_path = "c/muro-httpd";
/* Child flags. The test key is not a server default. */
static const char *srv_db = ":memory:";
static const char *srv_jwt = NULL;
static int last_json;

static void fail(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s: %s\n", name, what);
  failures += 1;
}

static int parse_port(const char *line) {
  int port = -1;
  if (sscanf(line, "listening 127.0.0.1 %d", &port) != 1) return -1;
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
  if (max_conn < 1) return -1;
  if (pipe(sp) < 0) return -1;
  pid = fork();
  if (pid < 0) {
    close(sp[0]);
    close(sp[1]);
    return -1;
  }
  if (pid == 0) {
    char *av[16];
    int argc = 0;
    const char *db = srv_db && srv_db[0] ? srv_db : ":memory:";
    alarm(0);
    if (dup2(sp[1], STDOUT_FILENO) < 0) _exit(127);
    close(sp[0]);
    close(sp[1]);
    snprintf(idle, sizeof idle, "%d", idle_ms);
    snprintf(maxc, sizeof maxc, "%d", max_conn);
    unsetenv("MURO_HTTP_DB");
    unsetenv("MURO_HTTP_JWT_KEY");
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
    av[argc++] = "--db";
    av[argc++] = (char *)db;
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
  *port = parse_port(buf);
  if (*port < 0) {
    fprintf(stderr, "FAIL start: banner '%s'\n", buf);
    failures += 1;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }
  return pid;
}

static int stop_server(pid_t pid) {
  int status = 0;
  int i;
  if (pid <= 0) return -1;
  kill(pid, SIGTERM);
  for (i = 0; i < 50; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
      fprintf(stderr, "FAIL shutdown: status %d\n", status);
      failures += 1;
      return -1;
    }
    if (got < 0) return -1;
    usleep(50000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  fail("shutdown", "SIGTERM did not stop the server");
  return -1;
}

static int dial(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  struct timeval tv;
  if (fd < 0) return -1;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
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
  if (send_all(fd, "GET /health HTTP/1.1\r\n\r\n") < 0 ||
      read_response(fd, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("keep-alive", "first");
    close(fd);
    return;
  }
  if (status != 200 || strcmp(body, "ok") != 0 || conn_close) fail("keep-alive", "first response");
  if (send_all(fd, "GET /missing HTTP/1.1\r\n\r\n") < 0 ||
      read_response(fd, buf, sizeof buf, used, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("keep-alive", "second");
    close(fd);
    return;
  }
  if (status != 404 || strcmp(body, "not-found") != 0 || conn_close) fail("keep-alive", "second response");
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
  const char *req = "GET /health HTTP/1.1\r\n\r\nGET /missing HTTP/1.1\r\n\r\n";
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
  if (status != 200 || strcmp(body, "ok") != 0) fail("pipeline", "first response");
  if (read_response(fd, buf, sizeof buf, used, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0) {
    fail("pipeline", "second");
    close(fd);
    return;
  }
  if (status != 404 || strcmp(body, "not-found") != 0) fail("pipeline", "second response");
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
  if (send_all(fd, "GET /health HTTP/1.1\r\n\r\n") < 0 ||
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
   The image stays in need-more until those bytes arrive. */
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
  if (send_all(fd, "POST /posts HTTP/1.1\r\nContent-Length: 2\r\n\r\n") < 0) {
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
  if (status != 401 || strcmp(body, "missing-bearer") != 0 || conn_close)
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
  hdr_n = (size_t)snprintf(msg, sizeof msg, "POST /posts HTTP/1.1\r\nContent-Length: 2000\r\n\r\n");
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
  if (fd1 < 0 || send_all(fd1, "GET /health HTTP/1.1\r\n\r\n") < 0 ||
      read_response(fd1, buf, sizeof buf, 0, &used, &status, &conn_close, body, sizeof body - 1,
                    &body_n) < 0 ||
      status != 200) {
    fail("max-conn", "first");
    if (fd1 >= 0) close(fd1);
    stop_server(pid);
    return;
  }
  fd2 = dial(port);
  if (fd2 < 0 || send_all(fd2, "GET /health HTTP/1.1\r\n\r\n") < 0) {
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
    "HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 2\r\n"
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
  if (fd < 0 || send_chunks(fd, "GET /health HTTP/1.1\r\n\r\n", 1) < 0 ||
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
  log = fopen(tmpl, "r");
  if (!log) {
    fail("partial-send", "no write log");
  } else {
    while (fgets(line, sizeof line, log)) {
      char *end = NULL;
      long v = strtol(line, &end, 10);
      if (end == line || v != 1) {
        fprintf(stderr, "FAIL partial-send: write '%s'\n", line);
        failures += 1;
        break;
      }
      sum += 1;
      nwrite += 1;
    }
    fclose(log);
    if (nwrite < 2 || sum != want) {
      fprintf(stderr, "FAIL partial-send: writes %d sum %zu want %zu\n", nwrite, sum, want);
      failures += 1;
    }
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
             "POST /posts HTTP/1.1\r\nAuthorization: Bearer %s\r\nContent-Length: %zu\r\n\r\n%s",
             token, strlen(json), json);
  } else {
    snprintf(dst, cap, "POST /posts HTTP/1.1\r\nContent-Length: %zu\r\n\r\n%s", strlen(json), json);
  }
}

static void post_like(char *dst, size_t cap, const char *id, const char *token) {
  if (token && token[0]) {
    snprintf(dst, cap, "POST /posts/%s/like HTTP/1.1\r\nAuthorization: Bearer %s\r\n\r\n", id, token);
  } else {
    snprintf(dst, cap, "POST /posts/%s/like HTTP/1.1\r\n\r\n", id);
  }
}

static void check_routes(int port) {
  char req[4096];
  char longbody[1200];
  post_json(req, sizeof req, NULL, "{}");
  check_one(port, "post-no-auth", req, 401, "missing-bearer", 0, 0);
  post_json(req, sizeof req, tok_bad, "{\"text\":\"hi\"}");
  check_one(port, "post-bad-token", req, 401, "bad-token", 0, 0);
  post_json(req, sizeof req, tok_none, "{\"text\":\"hi\"}");
  check_one(port, "post-alg-none", req, 401, "bad-token", 0, 0);
  snprintf(req, sizeof req,
           "POST /posts HTTP/1.1\r\nAuthorization: bearer %s\r\nContent-Length: 13\r\n\r\n"
           "{\"text\":\"hi\"}",
           tok_ada);
  check_one(port, "post-lower-bearer", req, 401, "bad-token", 0, 0);
  check_one(port, "post-empty-bearer",
            "POST /posts HTTP/1.1\r\nAuthorization: Bearer \r\nContent-Length: 13\r\n\r\n"
            "{\"text\":\"hi\"}",
            401, "bad-token", 0, 0);
  post_json(req, sizeof req, tok_empty_sub, "{\"text\":\"hi\"}");
  check_one(port, "post-empty-sub", req, 401, "bad-payload", 0, 0);
  post_json(req, sizeof req, tok_sub_num, "{\"text\":\"hi\"}");
  check_one(port, "post-sub-num", req, 401, "bad-payload", 0, 0);
  post_json(req, sizeof req, tok_expired, "{\"text\":\"hi\"}");
  check_one(port, "post-expired", req, 401, "bad-payload", 0, 0);
  post_json(req, sizeof req, tok_nbf_far, "{\"text\":\"hi\"}");
  check_one(port, "post-nbf", req, 401, "bad-payload", 0, 0);
  post_json(req, sizeof req, tok_exp_only, "{\"text\":\"hi\"}");
  check_one(port, "post-no-sub", req, 401, "bad-payload", 0, 0);
  post_json(req, sizeof req, tok_ada, "{}");
  check_one(port, "post-empty-obj", req, 400, "body-required", 0, 0);
  post_json(req, sizeof req, tok_ada, "{");
  check_one(port, "post-trunc", req, 400, "malformed", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"\"}");
  check_one(port, "post-empty-text", req, 400, "body-required", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":1}");
  check_one(port, "post-num", req, 400, "body-required", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"a\":{\"text\":\"hi\"}}");
  check_one(port, "post-nested", req, 400, "body-required", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"\\u0074ext\":\"hi\"}");
  check_one(port, "post-key-esc", req, 400, "body-required", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"a\",\"text\":\"b\"}");
  check_one(port, "post-dup", req, 400, "malformed", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"\\uD800\"}");
  check_one(port, "post-surrogate", req, 400, "malformed", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}x");
  check_one(port, "post-trail", req, 400, "malformed", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\",}");
  check_one(port, "post-comma", req, 400, "malformed", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"\xff\"}");
  check_one(port, "post-utf8", req, 400, "malformed", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"\n\"}");
  check_one(port, "post-control", req, 400, "malformed", 0, 0);
  memset(longbody, 'x', sizeof longbody);
  memcpy(longbody, "{\"text\":\"", 9);
  memcpy(longbody + 9 + 1100, "\"}", 2);
  longbody[9 + 1100 + 2] = 0;
  post_json(req, sizeof req, tok_ada, longbody);
  if (strlen(longbody) <= 1024) fail("post-long", "fixture");
  check_one(port, "post-long", req, 400, "body-long", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "create", req, 201, "{\"id\":1,\"text\":\"hi\"}", 0, 0);
  post_json(req, sizeof req, tok_exp, "{\"text\":\"\\u0041\"}");
  check_one(port, "create-exp", req, 201, "{\"id\":2,\"text\":\"A\"}", 0, 0);
  post_json(req, sizeof req, tok_nbf, "{\"text\":\"a\\\"b\"}");
  check_one(port, "create-nbf", req, 201, "{\"id\":3,\"text\":\"a\\\"b\"}", 0, 0);
  check_one(port, "read-1", "GET /posts/1 HTTP/1.1\r\n\r\n", 200, "{\"id\":1,\"text\":\"hi\"}", 0, 0);
  check_one(port, "read-2", "GET /posts/2 HTTP/1.1\r\n\r\n", 200, "{\"id\":2,\"text\":\"A\"}", 0, 0);
  check_one(port, "read-missing", "GET /posts/9 HTTP/1.1\r\n\r\n", 404, "post-missing", 0, 0);
  check_one(port, "read-zero", "GET /posts/0 HTTP/1.1\r\n\r\n", 400, "invalid-id", 0, 0);
  check_one(port, "read-pad", "GET /posts/01 HTTP/1.1\r\n\r\n", 400, "invalid-id", 0, 0);
  check_one(port, "get-posts", "GET /posts HTTP/1.1\r\n\r\n", 404, "not-found", 0, 0);
  post_like(req, sizeof req, "1", NULL);
  check_one(port, "like-no-auth", req, 401, "missing-bearer", 0, 0);
  post_like(req, sizeof req, "1", tok_bad);
  check_one(port, "like-bad-token", req, 401, "bad-token", 0, 0);
  post_like(req, sizeof req, "0", tok_ada);
  check_one(port, "like-bad-id", req, 400, "invalid-id", 0, 0);
  post_like(req, sizeof req, "1", tok_ada);
  check_one(port, "like-new", req, 201, "{\"id\":1,\"likes\":1}", 0, 0);
  post_like(req, sizeof req, "1", tok_ada);
  check_one(port, "like-again", req, 200, "{\"id\":1,\"likes\":1}", 0, 0);
  post_like(req, sizeof req, "9", tok_ada);
  check_one(port, "like-missing", req, 404, "post-missing", 0, 0);
  check_one(port, "feed-rows", "GET /feed HTTP/1.1\r\n\r\n", 200,
            "[{\"id\":1,\"text\":\"hi\"},{\"id\":2,\"text\":\"A\"},{\"id\":3,\"text\":\"a\\\"b\"}]", 0, 0);
  check_one(port, "post-health", "POST /health HTTP/1.1\r\n\r\n", 404, "not-found", 0, 0);
  check_one(port, "post-feed", "POST /feed HTTP/1.1\r\n\r\n", 404, "not-found", 0, 0);
  check_one(port, "get-like", "GET /posts/1/like HTTP/1.1\r\n\r\n", 404, "not-found", 0, 0);
  check_one(port, "put-health", "PUT /health HTTP/1.1\r\n\r\n", 404, "not-found", 0, 0);
  snprintf(req, sizeof req, "GET /health HTTP/1.1\r\nAuthorization: Bearer %s\r\n\r\n", tok_bad);
  check_one(port, "health-bad-auth", req, 200, "ok", 0, 0);
}

static void check_db_down(void) {
  pid_t pid;
  int port = -1;
  char req[1024];
  srv_db = "/no/such/directory/muro-http.db";
  pid = start_server(10000, 4, &port, NULL, NULL, NULL);
  srv_db = ":memory:";
  if (pid < 0) return;
  check_one(port, "down-health", "GET /health HTTP/1.1\r\n\r\n", 503, "db", 0, 0);
  check_one(port, "down-feed", "GET /feed HTTP/1.1\r\n\r\n", 500, "internal", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "down-create", req, 500, "internal", 0, 0);
  check_one(port, "down-read", "GET /posts/1 HTTP/1.1\r\n\r\n", 500, "internal", 0, 0);
  post_like(req, sizeof req, "1", tok_ada);
  check_one(port, "down-like", req, 500, "internal", 0, 0);
  if (stop_server(pid) < 0) return;
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
  check_one(port, "nokey-health", "GET /health HTTP/1.1\r\n\r\n", 200, "ok", 0, 0);
  post_json(req, sizeof req, tok_ada, "{\"text\":\"hi\"}");
  check_one(port, "nokey-token", req, 401, "bad-token", 0, 0);
  check_one(port, "nokey-absent", "POST /posts HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}", 401,
            "missing-bearer", 0, 0);
  if (stop_server(pid) < 0) return;
}

int main(int argc, char **argv) {
  pid_t pid;
  int port = -1;
  if (argc > 1) bin_path = argv[1];
  signal(SIGPIPE, SIG_IGN);
  alarm(180);
  srv_jwt = "muro-http-test-key";
  pid = start_server(10000, 16, &port, NULL, NULL, NULL);
  if (pid < 0) return 1;
  check_one(port, "health", "GET /health HTTP/1.1\r\n\r\n", 200, "ok", 0, 0);
  check_keepalive(port);
  check_pipeline(port);
  check_one(port, "close", "GET /health HTTP/1.1\r\nConnection: close\r\n\r\n", 200, "ok", 1, 1);
  check_one(port, "malformed", "GET/feed HTTP/1.1\r\n\r\n", 400, "malformed", 1, 1);
  check_fin(port);
  check_one(port, "feed", "GET /feed HTTP/1.1\r\n\r\n", 200, "[]", 0, 0);
  check_one(port, "missing", "GET /missing HTTP/1.1\r\n\r\n", 404, "not-found", 0, 0);
  check_one(port, "post", "POST /posts HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}", 401, "missing-bearer",
            0, 0);
  check_split_body(port);
  check_routes(port);
  check_full_buffer();
  if (stop_server(pid) < 0) return 1;
  pid = start_server(300, 16, &port, NULL, NULL, NULL);
  if (pid < 0) return 1;
  check_idle_bare(port);
  check_idle_after(port);
  if (stop_server(pid) < 0) return 1;
  check_max_conn();
  check_partial_send();
  check_db_down();
  check_no_key();
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
