/* The HTTP/1.1 core without the challenge app. GET answers hello,
   POST echoes the body, and any other method is 405. /health is not
   a special route. A request with no Host is the book's 400. */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static const char *bin_path = "c/muro-hello";
static char stream_ready[96];

static int wait_file(const char *path, int ms) {
  int left = ms;
  while (left > 0) {
    if (access(path, R_OK) == 0) return 0;
    usleep(20000);
    left -= 20;
  }
  return -1;
}

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

static pid_t start_hello(const char *env_bind, const char *bind_host, int *port,
                        const char *drain_ms, const char *idle_ms) {
  int sp[2];
  pid_t pid;
  char buf[128];
  size_t n = 0;
  const char *host = (bind_host && bind_host[0]) ? bind_host : "127.0.0.1";
  if (pipe(sp) < 0) return -1;
  pid = fork();
  if (pid < 0) {
    close(sp[0]);
    close(sp[1]);
    return -1;
  }
  if (pid == 0) {
    char *av[12];
    int argc = 0;
    alarm(0);
    if (dup2(sp[1], STDOUT_FILENO) < 0) _exit(127);
    close(sp[0]);
    close(sp[1]);
    unsetenv("MURO_HTTP_BIND");
    if (env_bind && env_bind[0]) setenv("MURO_HTTP_BIND", env_bind, 1);
    unsetenv("MURO_HTTP_PORT");
    unsetenv("MURO_HTTP_IDLE_MS");
    unsetenv("MURO_HTTP_MAX_CONNECTIONS");
    unsetenv("MURO_HTTP_MAX_BODY");
    unsetenv("MURO_HTTP_DRAIN_MS");
    unsetenv("MURO_HTTP_WORKERS");
    unsetenv("MURO_HTTP_WAIT");
    unsetenv("MURO_HTTP_DB");
    unsetenv("MURO_HTTP_JWT_KEY");
    unsetenv("SQLITE_PATH");
    av[argc++] = "muro-hello";
    av[argc++] = "--port";
    av[argc++] = "0";
    if (bind_host && bind_host[0]) {
      av[argc++] = "--bind";
      av[argc++] = (char *)host;
    }
    if (!idle_ms || !idle_ms[0]) idle_ms = "5000";
    av[argc++] = "--idle-ms";
    av[argc++] = (char *)idle_ms;
    if (drain_ms) {
      av[argc++] = "--drain-ms";
      av[argc++] = (char *)drain_ms;
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
      break;
    }
    n += (size_t)r;
  }
  close(sp[0]);
  buf[n < sizeof buf ? n : sizeof buf - 1] = 0;
  *port = parse_port(buf, host);
  if (*port < 0) {
    fprintf(stderr, "FAIL start: banner '%s'\n", buf);
    failures += 1;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }
  return pid;
}

static void stop_server(pid_t pid) {
  int status = 0;
  int i;
  if (pid <= 0) return;
  kill(pid, SIGTERM);
  for (i = 0; i < 50; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) fail("shutdown", "status");
      return;
    }
    if (got < 0) return;
    usleep(50000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  fail("shutdown", "did not stop");
}

static int dial(const char *host, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  struct timeval tv;
  if (fd < 0) return -1;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
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

static int find_cl(const char *buf, int hend, int *cl) {
  int i;
  for (i = 0; i + 16 < hend; i++) {
    if (memcmp(buf + i, "Content-Length: ", 16) == 0) {
      int v = 0;
      int j = i + 16;
      if (buf[j] < '0' || buf[j] > '9') return -1;
      while (j < hend && buf[j] >= '0' && buf[j] <= '9') {
        v = v * 10 + (buf[j] - '0');
        j += 1;
      }
      *cl = v;
      return 0;
    }
  }
  return -1;
}

static int read_response(int fd, int *status, int *conn_close, char *body, size_t body_cap,
                         size_t *body_n) {
  char buf[2048];
  size_t n = 0;
  for (;;) {
    int hend = -1;
    size_t i;
    int cl = -1;
    size_t total;
    ssize_t r;
    for (i = 0; i + 3 < n; i++) {
      if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
        hend = (int)i;
        break;
      }
    }
    if (hend >= 0 && find_cl(buf, hend, &cl) == 0) {
      total = (size_t)hend + 4 + (size_t)cl;
      if (n >= total) {
        int code = 0;
        if (sscanf(buf, "HTTP/1.1 %d", &code) != 1) return -1;
        *status = code;
        *conn_close = memmem(buf, (size_t)hend, "Connection: close", 17) != NULL;
        if ((size_t)cl >= body_cap) return -1;
        memcpy(body, buf + hend + 4, (size_t)cl);
        body[cl] = 0;
        *body_n = (size_t)cl;
        return 0;
      }
    }
    if (n + 1 >= sizeof buf) return -1;
    r = read(fd, buf + n, sizeof buf - n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    n += (size_t)r;
  }
}


/* Read one complete response from `held`, pulling more bytes from fd
   if needed. Leftover bytes stay in `held` for a pipelined follow-up. */
static int read_held(int fd, char *held, size_t *held_n, size_t held_cap,
                     int *status, int *conn_close, char *body, size_t body_cap,
                     size_t *body_n) {
  for (;;) {
    int hend = -1;
    size_t i;
    int cl = -1;
    size_t total;
    ssize_t r;
    for (i = 0; i + 3 < *held_n; i++) {
      if (held[i] == '\r' && held[i + 1] == '\n' && held[i + 2] == '\r' && held[i + 3] == '\n') {
        hend = (int)i;
        break;
      }
    }
    if (hend >= 0 && find_cl(held, hend, &cl) == 0) {
      total = (size_t)hend + 4 + (size_t)cl;
      if (*held_n >= total) {
        int code = 0;
        if (sscanf(held, "HTTP/1.1 %d", &code) != 1) return -1;
        *status = code;
        *conn_close = memmem(held, (size_t)hend, "Connection: close", 17) != NULL;
        if ((size_t)cl >= body_cap) return -1;
        memcpy(body, held + hend + 4, (size_t)cl);
        body[cl] = 0;
        *body_n = (size_t)cl;
        memmove(held, held + total, *held_n - total);
        *held_n -= total;
        return 0;
      }
    }
    if (*held_n + 1 >= held_cap) return -1;
    r = read(fd, held + *held_n, held_cap - *held_n - 1);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    *held_n += (size_t)r;
  }
}

static void check_pipeline_post_slash(const char *host, int port) {
  int fd = dial(host, port);
  char held[2048];
  size_t held_n = 0;
  int status = 0;
  int conn_close = 0;
  char body[128];
  size_t body_n = 0;
  if (fd < 0) {
    fail("pipeline-post-slash", "connect");
    return;
  }
  if (send_all(fd, "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping"
                   "GET / HTTP/1.1\r\nHost: t\r\n\r\n") < 0) {
    fail("pipeline-post-slash", "send");
    close(fd);
    return;
  }
  if (read_held(fd, held, &held_n, sizeof held, &status, &conn_close, body, sizeof body, &body_n) < 0 ||
      status != 200 || strcmp(body, "hello\n") != 0 || conn_close) {
    fail("pipeline-post-slash", "first");
    close(fd);
    return;
  }
  if (read_held(fd, held, &held_n, sizeof held, &status, &conn_close, body, sizeof body, &body_n) < 0 ||
      status != 200 || strcmp(body, "hello\n") != 0 || conn_close) {
    fail("pipeline-post-slash", "second");
    close(fd);
    return;
  }
  close(fd);
}

static void expect_one(const char *host, int port, const char *name, const char *req, int want_status,
                       const char *want_body, int want_close) {
  int fd = dial(host, port);
  int status = 0;
  int conn_close = 0;
  char body[128];
  size_t body_n = 0;
  if (fd < 0) {
    fail(name, "connect");
    return;
  }
  if (send_all(fd, req) < 0 ||
      read_response(fd, &status, &conn_close, body, sizeof body, &body_n) < 0) {
    fail(name, "exchange");
    close(fd);
    return;
  }
  if (status != want_status || strcmp(body, want_body) != 0) {
    fprintf(stderr, "FAIL %s: status %d body '%s'\n", name, status, body);
    failures += 1;
  }
  if (conn_close != want_close) fail(name, want_close ? "wanted close" : "wanted keep-alive");
  close(fd);
}

static void check_bad_bind(void) {
  int sp[2];
  pid_t pid;
  char buf[512];
  size_t n = 0;
  int status = 0;
  int i;
  int reaped = 0;
  if (pipe(sp) < 0) {
    fail("bad-bind", "pipe");
    return;
  }
  pid = fork();
  if (pid < 0) {
    close(sp[0]);
    close(sp[1]);
    fail("bad-bind", "fork");
    return;
  }
  if (pid == 0) {
    char *av[] = {"muro-hello", "--bind", "nope", "--port", "0", NULL};
    alarm(0);
    if (dup2(sp[1], STDOUT_FILENO) < 0) _exit(127);
    if (dup2(sp[1], STDERR_FILENO) < 0) _exit(127);
    close(sp[0]);
    close(sp[1]);
    unsetenv("MURO_HTTP_BIND");
    unsetenv("MURO_HTTP_WORKERS");
    unsetenv("MURO_HTTP_WAIT");
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
    waitpid(pid, NULL, 0);
    fail("bad-bind", "still running");
    return;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 2) fail("bad-bind", "status");
  if (strstr(buf, "listening") != NULL) fail("bad-bind", "listened");
  if (strstr(buf, "bad bind host") == NULL) fail("bad-bind", "message");
}

static void check_help(void) {
  int sp[2];
  pid_t pid;
  char buf[1024];
  size_t n = 0;
  int status = 0;
  int i;
  if (pipe(sp) < 0) {
    fail("help", "pipe");
    return;
  }
  pid = fork();
  if (pid < 0) {
    close(sp[0]);
    close(sp[1]);
    fail("help", "fork");
    return;
  }
  if (pid == 0) {
    char *av[] = {"muro-hello", "--help", NULL};
    alarm(0);
    if (dup2(sp[1], STDOUT_FILENO) < 0) _exit(127);
    close(sp[0]);
    close(sp[1]);
    execv(bin_path, av);
    _exit(127);
  }
  close(sp[1]);
  while (n + 1 < sizeof buf) {
    ssize_t r = read(sp[0], buf + n, sizeof buf - 1 - n);
    if (r <= 0) break;
    n += (size_t)r;
  }
  close(sp[0]);
  buf[n] = 0;
  for (i = 0; i < 40; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) break;
    if (got < 0) break;
    usleep(20000);
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) fail("help", "status");
  if (strstr(buf, "--bind") == NULL) fail("help", "usage");
}

static int send_n(int fd, const void *p, size_t n) {
  const char *s = p;
  size_t off = 0;
  while (off < n) {
    ssize_t w = send(fd, s + off, n - off, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

static void check_stream_echo(const char *host, int port) {
  int fd = dial(host, port);
  char head[128];
  char *payload = NULL;
  char *buf = NULL;
  int hn;
  size_t n = 0;
  int status = 0;
  int conn_close = 0;
  int content_length = -1;
  size_t body_n = 0;
  size_t i;
  hn = snprintf(head, sizeof head, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 20000\r\n\r\n");
  payload = malloc(20000);
  buf = malloc(22000);
  if (fd < 0 || hn < 0 || !payload || !buf) {
    fail("stream-echo", "setup");
    if (fd >= 0) close(fd);
    free(payload);
    free(buf);
    return;
  }
  memset(payload, 'z', 20000);
  /* Headers only. The rest waits until muro_http_body_read is entered
     for this streamed body. Sending both at once lets one read hold
     the whole message, streamTake stays 0, and hello echoes req->body
     without that read. A sleep raced a slow accept. */
  unlink(stream_ready);
  if (send_n(fd, head, (size_t)hn) < 0) {
    fail("stream-echo", "send");
    close(fd);
    free(payload);
    free(buf);
    return;
  }
  if (stream_ready[0] == 0 || wait_file(stream_ready, 5000) < 0) {
    fail("stream-echo", "body-read");
    close(fd);
    free(payload);
    free(buf);
    return;
  }
  if (send_n(fd, payload, 20000) < 0) {
    fail("stream-echo", "body");
    close(fd);
    free(payload);
    free(buf);
    return;
  }
  while (n < 22000) {
    ssize_t r = read(fd, buf + n, 22000 - n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    n += (size_t)r;
    if (n > 200) {
      int hend = -1;
      size_t k;
      for (k = 0; k + 3 < n; k++) {
        if (buf[k] == '\r' && buf[k + 1] == '\n' && buf[k + 2] == '\r' && buf[k + 3] == '\n') {
          hend = (int)k;
          break;
        }
      }
      if (hend >= 0 && n >= (size_t)hend + 4 + 20000) break;
    }
  }
  buf[n < 22000 ? n : 21999] = 0;
  if (sscanf(buf, "HTTP/1.1 %d", &status) != 1 || status != 200) {
    fprintf(stderr, "FAIL stream-echo: status %d\n", status);
    failures += 1;
  }
  conn_close = memmem(buf, n, "Connection: close", 17) != NULL;
  if (conn_close) fail("stream-echo", "closed");
  {
    char *eoh = memmem(buf, n, "\r\n\r\n", 4);
    if (!eoh) {
      fail("stream-echo", "headers");
    } else {
      unsigned char *body = (unsigned char *)eoh + 4;
      body_n = n - (size_t)(body - (unsigned char *)buf);
      if (find_cl(buf, (int)(eoh - buf), &content_length) != 0 || content_length != 20000)
        fail("stream-echo", "content-length");
      if (body_n != 20000) {
        fprintf(stderr, "FAIL stream-echo: body %zu\n", body_n);
        failures += 1;
      } else {
        for (i = 0; i < body_n; i++) {
          if (body[i] != 'z') {
            fail("stream-echo", "byte");
            break;
          }
        }
      }
    }
  }
  close(fd);
  free(payload);
  free(buf);
}

static void check_stream_hello(void) {
  const char *host = "127.0.0.1";
  pid_t pid;
  int port = -1;
  int fd;
  char head[128];
  char ready[96];
  char *payload = NULL;
  char buf[1024];
  int hn;
  size_t n = 0;
  int status = 0;
  int conn_close = 0;
  int content_length = -1;
  if (snprintf(ready, sizeof ready, "/tmp/muro-hello-stream-slash-%ld", (long)getpid()) < 0) {
    fail("stream-hello", "path");
    return;
  }
  unlink(ready);
  setenv("MURO_TEST_STREAM_READY", ready, 1);
  pid = start_hello("nope", host, &port, NULL, NULL);
  unsetenv("MURO_TEST_STREAM_READY");
  if (pid < 0) {
    unlink(ready);
    fail("stream-hello", "start");
    return;
  }
  fd = dial(host, port);
  hn = snprintf(head, sizeof head, "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 20000\r\n\r\n");
  payload = malloc(20000);
  if (fd < 0 || hn < 0 || !payload) {
    fail("stream-hello", "setup");
    if (fd >= 0) close(fd);
    free(payload);
    stop_server(pid);
    unlink(ready);
    return;
  }
  memset(payload, 'z', 20000);
  if (send_n(fd, head, (size_t)hn) < 0) {
    fail("stream-hello", "send");
    close(fd);
    free(payload);
    stop_server(pid);
    unlink(ready);
    return;
  }
  if (wait_file(ready, 5000) < 0) {
    fail("stream-hello", "body-read");
    close(fd);
    free(payload);
    stop_server(pid);
    unlink(ready);
    return;
  }
  if (send_n(fd, payload, 20000) < 0) {
    fail("stream-hello", "body");
    close(fd);
    free(payload);
    stop_server(pid);
    unlink(ready);
    return;
  }
  while (n < sizeof buf - 1) {
    ssize_t r = read(fd, buf + n, sizeof buf - 1 - n);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    n += (size_t)r;
    if (memmem(buf, n, "\r\n\r\n", 4)) {
      int hend = -1;
      size_t k;
      for (k = 0; k + 3 < n; k++) {
        if (buf[k] == '\r' && buf[k + 1] == '\n' && buf[k + 2] == '\r' && buf[k + 3] == '\n') {
          hend = (int)k;
          break;
        }
      }
      if (hend >= 0 && find_cl(buf, hend, &content_length) == 0 &&
          n >= (size_t)hend + 4 + (size_t)content_length)
        break;
    }
  }
  buf[n] = 0;
  if (sscanf(buf, "HTTP/1.1 %d", &status) != 1 || status != 200) {
    fprintf(stderr, "FAIL stream-hello: status %d\n", status);
    failures += 1;
  }
  conn_close = memmem(buf, n, "Connection: close", 17) != NULL;
  if (conn_close) fail("stream-hello", "closed");
  {
    char *eoh = memmem(buf, n, "\r\n\r\n", 4);
    if (!eoh) {
      fail("stream-hello", "headers");
    } else if (find_cl(buf, (int)(eoh - buf), &content_length) != 0 || content_length != 6) {
      fail("stream-hello", "content-length");
    } else if (n < (size_t)(eoh - buf) + 4 + 6 || memcmp(eoh + 4, "hello\n", 6) != 0) {
      fail("stream-hello", "body");
    }
  }
  close(fd);
  free(payload);
  stop_server(pid);
  unlink(ready);
}

static void check_drain_before_status(void) {
  pid_t pid;
  int port = -1;
  int fd;
  char head[128];
  char partial[64];
  char ready[96];
  int hn;
  int i;
  int closed = 0;
  int status = 0;
  int saw = 0;
  if (snprintf(ready, sizeof ready, "/tmp/muro-hello-drain-%ld", (long)getpid()) < 0) {
    fail("drain-before", "path");
    return;
  }
  unlink(ready);
  setenv("MURO_TEST_STREAM_READY", ready, 1);
  pid = start_hello(NULL, "127.0.0.1", &port, "400", NULL);
  unsetenv("MURO_TEST_STREAM_READY");
  if (pid < 0) {
    unlink(ready);
    return;
  }
  fd = dial("127.0.0.1", port);
  hn = snprintf(head, sizeof head, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 20000\r\n\r\n");
  memset(partial, 'q', sizeof partial);
  if (fd < 0 || hn < 0 || send_n(fd, head, (size_t)hn) < 0) {
    fail("drain-before", "send");
    if (fd >= 0) close(fd);
    stop_server(pid);
    unlink(ready);
    return;
  }
  /* Same gate as stream-echo: the signal is delivered only after
     muro_http_body_read has been entered for this streamed body. */
  if (wait_file(ready, 5000) < 0) {
    fail("drain-before", "body-read");
    close(fd);
    stop_server(pid);
    unlink(ready);
    return;
  }
  if (send_n(fd, partial, sizeof partial) < 0) {
    fail("drain-before", "partial");
    close(fd);
    stop_server(pid);
    unlink(ready);
    return;
  }
  kill(pid, SIGTERM);
  for (i = 0; i < 25; i++) {
    int nfd = dial("127.0.0.1", port);
    if (nfd < 0) {
      closed = 1;
      break;
    }
    close(nfd);
    usleep(20000);
  }
  if (!closed) fail("drain-before", "listen stayed open");
  close(fd);
  for (i = 0; i < 40; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      saw = 1;
      break;
    }
    usleep(50000);
  }
  if (!saw) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    fail("drain-before", "did not exit");
    unlink(ready);
    return;
  }
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) fail("drain-before", "status");
  unlink(ready);
}

/* Headers only, Content-Length past the window, then no body bytes.
   hello calls muro_http_body_read from on_req, which is decide.
   The idle deadline must end that read. hello returns 400 with body
   "bad\n". host_in_after sees the read error 400 and copies
   httpMalformed instead, then closes. A 200 would mean the body
   was treated as already buffered. */
static void check_decide_idle(void) {
  pid_t pid;
  int port = -1;
  int fd = -1;
  char ready[96];
  char head[128];
  char body[128];
  size_t body_n = 0;
  int hn;
  int status = 0;
  int conn_close = 0;
  char extra;
  ssize_t tail;
  if (snprintf(ready, sizeof ready, "/tmp/muro-hello-idle-%ld", (long)getpid()) < 0) {
    fail("decide-idle", "path");
    return;
  }
  unlink(ready);
  setenv("MURO_TEST_STREAM_READY", ready, 1);
  pid = start_hello(NULL, "127.0.0.1", &port, NULL, "500");
  unsetenv("MURO_TEST_STREAM_READY");
  if (pid < 0) {
    unlink(ready);
    return;
  }
  fd = dial("127.0.0.1", port);
  hn = snprintf(head, sizeof head, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 20000\r\n\r\n");
  if (fd < 0 || hn < 0 || send_n(fd, head, (size_t)hn) < 0) {
    fail("decide-idle", "send");
    if (fd >= 0) close(fd);
    stop_server(pid);
    unlink(ready);
    return;
  }
  if (wait_file(ready, 5000) < 0) {
    fail("decide-idle", "body-read");
    close(fd);
    stop_server(pid);
    unlink(ready);
    return;
  }
  if (read_response(fd, &status, &conn_close, body, sizeof body, &body_n) < 0) {
    fail("decide-idle", "exchange");
    close(fd);
    stop_server(pid);
    unlink(ready);
    return;
  }
  if (status != 400 || strcmp(body, "malformed") != 0) {
    fprintf(stderr, "FAIL decide-idle: status %d body '%s'\n", status, body);
    failures += 1;
  }
  if (!conn_close) fail("decide-idle", "wanted close");
  tail = read(fd, &extra, 1);
  if (tail != 0) fail("decide-idle", "still open");
  close(fd);
  stop_server(pid);
  unlink(ready);
}

int main(int argc, char **argv) {
  const char *host = "127.0.0.1";
  pid_t pid;
  int port = -1;
  int fd;
  int status = 0;
  int conn_close = 0;
  char body[128];
  size_t body_n = 0;
  if (argc > 1) bin_path = argv[1];
  signal(SIGPIPE, SIG_IGN);
  alarm(60);
  check_help();
  check_bad_bind();
  check_decide_idle();
  if (snprintf(stream_ready, sizeof stream_ready, "/tmp/muro-hello-stream-%ld", (long)getpid()) <
      0)
    stream_ready[0] = 0;
  else {
    unlink(stream_ready);
    setenv("MURO_TEST_STREAM_READY", stream_ready, 1);
  }
  pid = start_hello("nope", host, &port, NULL, NULL);
  unsetenv("MURO_TEST_STREAM_READY");
  if (pid < 0) {
    unlink(stream_ready);
    return 1;
  }
  expect_one(host, port, "hello", "GET / HTTP/1.1\r\nHost: t\r\n\r\n", 200, "hello\n", 0);
  expect_one(host, port, "health-is-hello", "GET /health HTTP/1.1\r\nHost: t\r\n\r\n", 200, "hello\n",
             0);
  expect_one(host, port, "echo", "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping", 200,
             "ping", 0);
  expect_one(host, port, "post-slash", "POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping", 200,
             "hello\n", 0);
  expect_one(host, port, "method", "PUT / HTTP/1.1\r\nHost: t\r\n\r\n", 405, "method\n", 1);
  expect_one(host, port, "no-host", "GET / HTTP/1.1\r\n\r\n", 400, "malformed", 1);
  fd = dial(host, port);
  if (fd < 0) {
    fail("keep", "connect");
  } else if (send_all(fd, "GET / HTTP/1.1\r\nHost: t\r\n\r\n") < 0 ||
             read_response(fd, &status, &conn_close, body, sizeof body, &body_n) < 0 ||
             status != 200 || strcmp(body, "hello\n") != 0 || conn_close) {
    fail("keep", "first");
    close(fd);
  } else if (send_all(fd, "POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\nbye") < 0 ||
             read_response(fd, &status, &conn_close, body, sizeof body, &body_n) < 0 ||
             status != 200 || strcmp(body, "bye") != 0 || conn_close) {
    fail("keep", "second");
    close(fd);
  } else {
    close(fd);
  }
  check_pipeline_post_slash(host, port);
  check_stream_echo(host, port);
  unlink(stream_ready);
  stop_server(pid);
  check_stream_hello();
  check_drain_before_status();
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
