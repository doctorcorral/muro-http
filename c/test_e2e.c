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

static pid_t start_server(int idle_ms, int *port) {
  int sp[2];
  pid_t pid;
  char idle[32];
  char buf[128];
  size_t n = 0;
  if (pipe(sp) < 0) return -1;
  pid = fork();
  if (pid < 0) {
    close(sp[0]);
    close(sp[1]);
    return -1;
  }
  if (pid == 0) {
    alarm(0);
    if (dup2(sp[1], STDOUT_FILENO) < 0) _exit(127);
    close(sp[0]);
    close(sp[1]);
    snprintf(idle, sizeof idle, "%d", idle_ms);
    execl(bin_path, "muro-httpd", "--port", "0", "--idle-ms", idle, "--max-connections", "16",
          (char *)NULL);
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
  char buf[4096];
  char body[256];
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

int main(int argc, char **argv) {
  pid_t pid;
  int port = -1;
  if (argc > 1) bin_path = argv[1];
  signal(SIGPIPE, SIG_IGN);
  alarm(60);
  pid = start_server(10000, &port);
  if (pid < 0) return 1;
  check_one(port, "health", "GET /health HTTP/1.1\r\n\r\n", 200, "ok", 0, 0);
  check_keepalive(port);
  check_pipeline(port);
  check_one(port, "close", "GET /health HTTP/1.1\r\nConnection: close\r\n\r\n", 200, "ok", 1, 1);
  check_one(port, "malformed", "GET/feed HTTP/1.1\r\n\r\n", 400, "malformed", 1, 1);
  check_fin(port);
  check_one(port, "feed", "GET /feed HTTP/1.1\r\n\r\n", 500, "internal", 0, 0);
  check_one(port, "missing", "GET /missing HTTP/1.1\r\n\r\n", 404, "not-found", 0, 0);
  check_one(port, "post", "POST /posts HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}", 401, "missing-bearer",
            0, 0);
  if (stop_server(pid) < 0) return 1;
  pid = start_server(300, &port);
  if (pid < 0) return 1;
  check_idle_bare(port);
  check_idle_after(port);
  if (stop_server(pid) < 0) return 1;
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
