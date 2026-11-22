/* workers=1 is the process that called muro_http_serve.
   workers=2 is two processes, and both answer. A bad count, or
   workers with port 0, is refused before listen. Each run is
   forced onto poll and onto the native wait. A close-mode flood
   with workers > 1 must finish with 0 errors. SIGKILL of one
   worker stops the pool. SIGTERM stops the workers. */

#include "muro_http.h"

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
static const char *active_wait = "default";
static int hold_gate = -1;
static int hold_first = 0;

static void fail(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s %s: %s\n", active_wait, name, what);
  failures += 1;
}

static int on_req(const MuroHttpRequest *req, MuroHttpResponse *resp, void *user) {
  static char body[32];
  int n;
  (void)req;
  (void)user;
  if (hold_first && hold_gate >= 0) {
    char b = 0;
    hold_first = 0;
    while (read(hold_gate, &b, 1) < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
  }
  n = snprintf(body, sizeof body, "%ld\n", (long)getpid());
  if (n < 0) return -1;
  resp->status = 200;
  resp->reason = "OK";
  resp->reason_len = 2;
  resp->body = (const uint8_t *)body;
  resp->body_len = (size_t)n;
  return 0;
}

static int free_port(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  socklen_t len = sizeof addr;
  int port;
  if (fd < 0) return -1;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
      getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
    close(fd);
    return -1;
  }
  port = (int)ntohs(addr.sin_port);
  close(fd);
  return port;
}

static int dial(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  if (fd < 0) return -1;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int send_all(int fd, const char *p) {
  size_t n = strlen(p);
  size_t off = 0;
  while (off < n) {
    ssize_t w = send(fd, p + off, n - off, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

static int body_pid(int fd, long *pid) {
  char buf[1024];
  size_t n = 0;
  char *end;
  char *cl;
  long len = -1;
  int code = 0;
  while (n + 1 < sizeof buf) {
    ssize_t r = recv(fd, buf + n, sizeof buf - 1 - n, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) break;
    n += (size_t)r;
    buf[n] = 0;
    end = strstr(buf, "\r\n\r\n");
    if (!end) continue;
    if (sscanf(buf, "HTTP/1.%*d %d", &code) != 1 || code != 200) return -1;
    cl = strstr(buf, "Content-Length:");
    if (!cl || cl > end) return -1;
    len = strtol(cl + 15, NULL, 10);
    if (len < 1 || (size_t)(end + 4 - buf) + (size_t)len > n) {
      if ((size_t)(end + 4 - buf) + (size_t)len > sizeof buf - 1) return -1;
      continue;
    }
    *pid = strtol(end + 4, NULL, 10);
    return *pid > 0 ? 0 : -1;
  }
  return -1;
}

static int one_pid(int port, long *pid) {
  int fd = dial(port);
  if (fd < 0) return -1;
  if (send_all(fd, "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n") < 0 ||
      body_pid(fd, pid) < 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int wait_line(int fd, int lines) {
  char buf[512];
  size_t n = 0;
  int got = 0;
  int spins = 0;
  while (got < lines && spins < 100) {
    ssize_t r;
    char *p;
    if (n + 1 >= sizeof buf) return -1;
    r = read(fd, buf + n, sizeof buf - 1 - n);
    if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      usleep(20000);
      spins += 1;
      continue;
    }
    if (r <= 0) return -1;
    n += (size_t)r;
    buf[n] = 0;
    p = buf;
    got = 0;
    while ((p = strstr(p, "listening ")) != NULL) {
      got += 1;
      p += 10;
    }
  }
  return got >= lines ? 0 : -1;
}

static pid_t spawn(int workers, int port, const char *wait, int *out_fd) {
  int sp[2];
  pid_t pid;
  if (pipe(sp) < 0) return -1;
  pid = fork();
  if (pid < 0) {
    close(sp[0]);
    close(sp[1]);
    return -1;
  }
  if (pid == 0) {
    MuroHttpConfig cfg;
    int rc;
    if (dup2(sp[1], STDOUT_FILENO) < 0) _exit(127);
    close(sp[0]);
    close(sp[1]);
    unsetenv("MURO_HTTP_WORKERS");
    if (wait) setenv("MURO_HTTP_WAIT", wait, 1);
    else unsetenv("MURO_HTTP_WAIT");
    muro_http_config_init(&cfg);
    cfg.name = "muro-workers";
    cfg.workers = workers;
    cfg.port = port;
    cfg.max_conn = 64;
    cfg.bind_host = "127.0.0.1";
    rc = muro_http_serve(&cfg, on_req, NULL);
    _exit(rc == 0 ? 0 : 1);
  }
  close(sp[1]);
  {
    int fl = fcntl(sp[0], F_GETFL, 0);
    if (fl >= 0) fcntl(sp[0], F_SETFL, fl | O_NONBLOCK);
  }
  *out_fd = sp[0];
  return pid;
}

static void stop_pid(pid_t pid) {
  int status = 0;
  int i;
  if (pid <= 0) return;
  kill(pid, SIGTERM);
  for (i = 0; i < 50; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) return;
    usleep(20000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, &status, 0);
}

static int listen_lines(int workers) {
#if defined(__linux__)
  return workers > 1 ? workers : 1;
#else
  (void)workers;
  return 1;
#endif
}

static void check_one(const char *wait) {
  int port = free_port();
  int out = -1;
  pid_t pid;
  long body = 0;
  if (port < 0) {
    fail("one", "port");
    return;
  }
  pid = spawn(1, port, wait, &out);
  if (pid < 0 || wait_line(out, 1) < 0 || one_pid(port, &body) < 0) {
    fail("one", "serve");
    stop_pid(pid);
    if (out >= 0) close(out);
    return;
  }
  if (body != (long)pid) fail("one", "pid");
  stop_pid(pid);
  close(out);
}

static void check_two(const char *wait) {
  int port = free_port();
  int out = -1;
  pid_t pid;
  long seen[4];
  int nseen = 0;
  int i;
  if (port < 0) {
    fail("two", "port");
    return;
  }
  pid = spawn(2, port, wait, &out);
  if (pid < 0 || wait_line(out, listen_lines(2)) < 0) {
    fail("two", "listen");
    stop_pid(pid);
    if (out >= 0) close(out);
    return;
  }
  for (i = 0; i < 40; i++) {
    long body = 0;
    int k;
    int tries;
    int got = 0;
    for (tries = 0; tries < 20 && !got; tries++) {
      if (one_pid(port, &body) == 0) got = 1;
      else usleep(20000);
    }
    if (!got) {
      fail("two", "request");
      break;
    }
    if (body == (long)pid) fail("two", "supervisor served");
    for (k = 0; k < nseen; k++) {
      if (seen[k] == body) break;
    }
    if (k == nseen && nseen < 4) seen[nseen++] = body;
  }
  if (nseen != 2) {
    fprintf(stderr, "FAIL two: pids %d\n", nseen);
    failures += 1;
  }
  stop_pid(pid);
  close(out);
}

static int dial_nb(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  int fl;
  int rc;
  if (fd < 0) return -1;
  fl = fcntl(fd, F_GETFL, 0);
  if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) {
    close(fd);
    return -1;
  }
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
  if (rc < 0 && errno != EINPROGRESS) {
    close(fd);
    return -1;
  }
  return fd;
}

static int wait_connected(int fd) {
  struct pollfd pfd;
  int err = 0;
  socklen_t len = sizeof err;
  pfd.fd = fd;
  pfd.events = POLLOUT;
  if (poll(&pfd, 1, 1000) <= 0) return -1;
  if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) return -1;
  return 0;
}

static int write_req(int fd) {
  const char *req = "GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n";
  size_t n = strlen(req);
  size_t off = 0;
  int spins = 0;
  while (off < n && spins < 40) {
    ssize_t w = send(fd, req + off, n - off, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      struct pollfd pfd;
      pfd.fd = fd;
      pfd.events = POLLOUT;
      if (poll(&pfd, 1, 200) <= 0) return -1;
      spins += 1;
      continue;
    }
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return off == n ? 0 : -1;
}

/* 0 complete 200, 1 need more bytes, -1 not a 200. */
static int response_ok(char *buf, size_t n, size_t cap) {
  char *end;
  char *cl;
  long clen;
  int code = 0;
  size_t header_n;
  if (n + 1 >= cap) return -1;
  buf[n] = 0;
  end = strstr(buf, "\r\n\r\n");
  if (!end) return 1;
  if (sscanf(buf, "HTTP/1.%*d %d", &code) != 1 || code != 200) return -1;
  cl = strstr(buf, "Content-Length:");
  if (!cl || cl > end) return -1;
  clen = strtol(cl + 15, NULL, 10);
  if (clen < 1) return -1;
  header_n = (size_t)(end + 4 - buf);
  if (n < header_n + (size_t)clen) return 1;
  return 0;
}

/* workers > 1, Connection: close, more clients than the handoff
   send buffer can queue while each worker is blocked in its first
   request. Every client must get a 200. */
static void check_flood(const char *wait) {
  enum { N = 32 };
  int workers = 4;
  int port = free_port();
  int out = -1;
  int gw[2] = {-1, -1};
  int fds[32];
  char bufs[32][512];
  size_t lens[32];
  int done[32];
  pid_t pid = -1;
  int errors = 0;
  int i;
  if (port < 0) {
    fail("flood", "port");
    return;
  }
  for (i = 0; i < N; i++) fds[i] = -1;
  if (pipe(gw) < 0) {
    fail("flood", "pipe");
    return;
  }
  hold_gate = gw[0];
  hold_first = 1;
  setenv("MURO_HTTP_TEST_HANDOFF_SNDBUF", "256", 1);
  pid = spawn(workers, port, wait, &out);
  hold_first = 0;
  hold_gate = -1;
  close(gw[0]);
  gw[0] = -1;
  unsetenv("MURO_HTTP_TEST_HANDOFF_SNDBUF");
  if (pid < 0 || wait_line(out, listen_lines(workers)) < 0) {
    fail("flood", "listen");
    goto done_flood;
  }
  for (i = 0; i < N; i++) {
    fds[i] = dial_nb(port);
    if (fds[i] < 0 || wait_connected(fds[i]) < 0 || write_req(fds[i]) < 0) {
      errors += 1;
      if (fds[i] >= 0) {
        close(fds[i]);
        fds[i] = -1;
      }
    }
    if (i + 1 == workers) usleep(300000);
  }
  usleep(100000);
  if (write(gw[1], "xxxx", workers) != workers) {
    fail("flood", "release");
    goto done_flood;
  }
  for (i = 0; i < N; i++) {
    lens[i] = 0;
    done[i] = fds[i] < 0;
  }
  {
    int spins;
    for (spins = 0; spins < 80; spins++) {
      struct pollfd pfds[32];
      int map[32];
      int np = 0;
      int pending = 0;
      int pr;
      for (i = 0; i < N; i++) {
        if (done[i] || fds[i] < 0) continue;
        pending += 1;
        pfds[np].fd = fds[i];
        pfds[np].events = POLLIN;
        map[np] = i;
        np += 1;
      }
      if (pending == 0) break;
      pr = poll(pfds, (nfds_t)np, 50);
      if (pr < 0 && errno == EINTR) continue;
      if (pr <= 0) continue;
      for (i = 0; i < np; i++) {
        int k = map[i];
        ssize_t r;
        int st;
        if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        if (lens[k] + 1 >= sizeof bufs[k]) {
          done[k] = 1;
          errors += 1;
          continue;
        }
        r = recv(fds[k], bufs[k] + lens[k], sizeof bufs[k] - 1 - lens[k], 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
        if (r <= 0) {
          done[k] = 1;
          errors += 1;
          continue;
        }
        lens[k] += (size_t)r;
        st = response_ok(bufs[k], lens[k], sizeof bufs[k]);
        if (st == 0) done[k] = 1;
        else if (st < 0) {
          done[k] = 1;
          errors += 1;
        }
      }
    }
  }
  for (i = 0; i < N; i++) {
    if (fds[i] >= 0 && !done[i]) errors += 1;
  }
  if (errors) {
    fprintf(stderr, "FAIL %s flood: %d errors\n", active_wait, errors);
    failures += 1;
  }
done_flood:
  for (i = 0; i < N; i++) {
    if (fds[i] >= 0) close(fds[i]);
  }
  if (gw[0] >= 0) close(gw[0]);
  if (gw[1] >= 0) close(gw[1]);
  stop_pid(pid);
  if (out >= 0) close(out);
  hold_first = 0;
  hold_gate = -1;
  unsetenv("MURO_HTTP_TEST_HANDOFF_SNDBUF");
}

/* A worker killed with SIGKILL is a bad exit. The parent reaps it
   and stops the other worker. The listener does not stay up. */
static void check_death(const char *wait) {
  int port = free_port();
  int out = -1;
  pid_t pid;
  long seen[2];
  int nseen = 0;
  int i;
  int status = 0;
  int reaped = 0;
  pid_t victim;
  pid_t sibling;
  if (port < 0) {
    fail("death", "port");
    return;
  }
  pid = spawn(2, port, wait, &out);
  if (pid < 0 || wait_line(out, listen_lines(2)) < 0) {
    fail("death", "listen");
    stop_pid(pid);
    if (out >= 0) close(out);
    return;
  }
  for (i = 0; i < 40 && nseen < 2; i++) {
    long body = 0;
    int k;
    if (one_pid(port, &body) < 0) {
      usleep(20000);
      continue;
    }
    if (body == (long)pid) {
      fail("death", "supervisor served");
      break;
    }
    for (k = 0; k < nseen; k++) {
      if (seen[k] == body) break;
    }
    if (k == nseen) seen[nseen++] = body;
  }
  if (nseen != 2) {
    fail("death", "pids");
    stop_pid(pid);
    if (out >= 0) close(out);
    return;
  }
  victim = (pid_t)seen[0];
  sibling = (pid_t)seen[1];
  if (kill(victim, SIGKILL) < 0) fail("death", "kill");
  for (i = 0; i < 200; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      reaped = 1;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    fail("death", "timeout");
    stop_pid(pid);
  } else if (!WIFEXITED(status) || WEXITSTATUS(status) == 0) {
    fail("death", "status");
  }
  for (i = 0; i < 200; i++) {
    if (kill(sibling, 0) < 0) break;
    usleep(20000);
  }
  if (kill(sibling, 0) == 0) fail("death", "sibling");
  {
    int fd = dial(port);
    if (fd >= 0) {
      fail("death", "still up");
      close(fd);
    }
  }
  if (out >= 0) close(out);
}

static void check_refuse(void) {
  MuroHttpConfig cfg;
  int idx;
  char *ok[] = {"t", "--workers", "3", "--wait", "poll"};
  char *bad[] = {"t", "--workers", "0"};
  muro_http_config_init(&cfg);
  cfg.workers = 65;
  cfg.port = 9;
  if (muro_http_serve(&cfg, on_req, NULL) != -1) fail("refuse", "65");
  muro_http_config_init(&cfg);
  cfg.workers = 2;
  cfg.port = 0;
  if (muro_http_serve(&cfg, on_req, NULL) != -1) fail("refuse", "port0");
  muro_http_config_init(&cfg);
  idx = 1;
  if (muro_http_config_arg(&cfg, &idx, 5, ok) != 1 || cfg.workers != 3) fail("refuse", "flag");
  idx = 3;
  if (muro_http_config_arg(&cfg, &idx, 5, ok) != 1) fail("refuse", "wait");
  if (!getenv("MURO_HTTP_WAIT") || strcmp(getenv("MURO_HTTP_WAIT"), "poll") != 0)
    fail("refuse", "env");
  unsetenv("MURO_HTTP_WAIT");
  idx = 1;
  if (muro_http_config_arg(&cfg, &idx, 3, bad) != -1) fail("refuse", "zero");
  setenv("MURO_HTTP_WAIT", "nope", 1);
  muro_http_config_init(&cfg);
  if (muro_http_config_env(&cfg) != -1) fail("refuse", "badwait");
  unsetenv("MURO_HTTP_WAIT");
}

static void check_term(const char *wait) {
  int port = free_port();
  int out = -1;
  pid_t pid;
  int status = 0;
  long body = 0;
  int i;
  int reaped = 0;
  if (port < 0) {
    fail("term", "port");
    return;
  }
  pid = spawn(2, port, wait, &out);
  if (pid < 0 || wait_line(out, listen_lines(2)) < 0 || one_pid(port, &body) < 0) {
    fail("term", "serve");
    stop_pid(pid);
    if (out >= 0) close(out);
    return;
  }
  if (kill(pid, SIGTERM) < 0) fail("term", "kill");
  for (i = 0; i < 100; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      reaped = 1;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    fail("term", "timeout");
    stop_pid(pid);
  } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fail("term", "status");
  }
  {
    int fd = dial(port);
    if (fd >= 0) {
      fail("term", "still up");
      close(fd);
    }
  }
  if (out >= 0) close(out);
}

static void run_backend(const char *wait) {
  active_wait = wait ? wait : "default";
  check_one(wait);
  check_two(wait);
  check_term(wait);
  check_flood(wait);
  check_death(wait);
}

int main(void) {
  const char *native = "poll";
  signal(SIGPIPE, SIG_IGN);
  unsetenv("MURO_HTTP_WAIT");
  unsetenv("MURO_HTTP_WORKERS");
  unsetenv("MURO_HTTP_TEST_HANDOFF_SNDBUF");
  active_wait = "refuse";
  check_refuse();
#if defined(__APPLE__)
  native = "kqueue";
#elif defined(__linux__)
  native = "epoll";
#endif
  run_backend(NULL);
  run_backend("poll");
  if (strcmp(native, "poll") != 0) run_backend(native);
  unsetenv("MURO_HTTP_WAIT");
  if (failures) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
