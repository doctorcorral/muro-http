/* host_poll: absolute timer, empty set, hangup/error flags, and
   scan-cursor fairness. */

#include "host_poll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

static int failures;
static const char *suite = "default";

static void fail(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s %s: %s\n", suite, name, what);
  failures += 1;
}

static int wait_retry(HostPoll *p, HostEv *out, int max) {
  int n;
  int i;
  for (i = 0; i < 8; i++) {
    n = host_poll_wait(p, out, max);
    if (n != 0) return n;
  }
  return n;
}

static void close_pair(int a, int b) {
  if (a >= 0) close(a);
  if (b >= 0) close(b);
}

static void drop_fd(HostPoll *p, int fd) {
  if (p && fd >= 0) host_poll_watch(p, fd, 0, 0);
}

/* An already-ready socket must not stretch the armed deadline. */
static void check_deadline(void) {
  HostPoll *p = host_poll_open();
  HostEv evs[4];
  int sp[2] = {-1, -1};
  uint64_t t0;
  uint64_t t1;
  int n;
  if (!p) {
    fail("deadline", "open");
    return;
  }
  if (pipe(sp) < 0) {
    fail("deadline", "pipe");
    host_poll_close(p);
    return;
  }
  if (write(sp[1], "x", 1) != 1 || host_poll_watch(p, sp[0], 1, 0) < 0) {
    fail("deadline", "watch");
    close_pair(sp[0], sp[1]);
    host_poll_close(p);
    return;
  }
  t0 = host_poll_now_ms();
  if (host_poll_arm(p, 200) < 0) {
    fail("deadline", "arm");
    close_pair(sp[0], sp[1]);
    host_poll_close(p);
    return;
  }
  n = wait_retry(p, evs, 4);
  if (n < 1 || evs[0].fd != sp[0] || evs[0].timer) {
    fail("deadline", "first wait");
    close_pair(sp[0], sp[1]);
    host_poll_close(p);
    return;
  }
  if (host_poll_watch(p, sp[0], 0, 0) < 0) {
    fail("deadline", "unwatch");
    close_pair(sp[0], sp[1]);
    host_poll_close(p);
    return;
  }
  n = wait_retry(p, evs, 4);
  t1 = host_poll_now_ms();
  if (n != 1 || !evs[0].timer || evs[0].fd != -1) {
    fail("deadline", "timer");
  } else if (t1 < t0 + 120 || t1 > t0 + 600) {
    fprintf(stderr, "FAIL deadline: elapsed %llu\n",
            (unsigned long long)(t1 - t0));
    failures += 1;
  }
  close_pair(sp[0], sp[1]);
  host_poll_close(p);
}

/* No watched fds: the wait still delivers the armed tick. */
static void check_empty(void) {
  HostPoll *p = host_poll_open();
  HostEv evs[2];
  uint64_t t0;
  uint64_t t1;
  int n;
  if (!p) {
    fail("empty", "open");
    return;
  }
  t0 = host_poll_now_ms();
  if (host_poll_arm(p, 80) < 0) {
    fail("empty", "arm");
    host_poll_close(p);
    return;
  }
  n = wait_retry(p, evs, 2);
  t1 = host_poll_now_ms();
  if (n != 1 || !evs[0].timer || evs[0].fd != -1) {
    fail("empty", "timer");
  } else if (t1 < t0 + 40 || t1 > t0 + 600) {
    fprintf(stderr, "FAIL empty: elapsed %llu\n",
            (unsigned long long)(t1 - t0));
    failures += 1;
  }
  host_poll_close(p);
}

static int wait_one(HostPoll *p, HostEv *e, const char *name) {
  HostEv evs[4];
  int n = wait_retry(p, evs, 4);
  if (n < 1) {
    fail(name, "wait");
    return -1;
  }
  *e = evs[0];
  return 0;
}

/* Peer FIN on a read watch is hangup/readable, not a forced error. */
static void check_read_hangup(void) {
  HostPoll *p = host_poll_open();
  HostEv ev;
  int sp[2] = {-1, -1};
  char b;
  ssize_t r;
  if (!p) {
    fail("read-hup", "open");
    return;
  }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) {
    fail("read-hup", "socketpair");
    host_poll_close(p);
    return;
  }
  if (host_poll_watch(p, sp[0], 1, 0) < 0) {
    fail("read-hup", "watch");
    close_pair(sp[0], sp[1]);
    host_poll_close(p);
    return;
  }
  if (shutdown(sp[1], SHUT_WR) < 0) {
    fail("read-hup", "shutdown");
    close_pair(sp[0], sp[1]);
    host_poll_close(p);
    return;
  }
  if (wait_one(p, &ev, "read-hup") < 0) {
    close_pair(sp[0], sp[1]);
    host_poll_close(p);
    return;
  }
  if (ev.fd != sp[0] || ev.timer || ev.error) fail("read-hup", "flags");
  if (!ev.hangup && !ev.readable) fail("read-hup", "no hangup");
  r = read(sp[0], &b, 1);
  if (r != 0) fail("read-hup", "eof");
  drop_fd(p, sp[0]);
  close_pair(sp[0], sp[1]);
  host_poll_close(p);
}

/* Write-only hangup without POLLOUT must be reported as error so
   the host loop cannot spin. A pipe write end with the read end
   closed is POLLERR and/or hangup. */
static void check_write_hangup(void) {
  HostPoll *p = host_poll_open();
  HostEv ev;
  int sp[2] = {-1, -1};
  int pr[2] = {-1, -1};
  if (!p) {
    fail("write-hup", "open");
    return;
  }
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) {
    fail("write-hup", "socketpair");
    host_poll_close(p);
    return;
  }
  if (host_poll_watch(p, sp[0], 0, 1) < 0) {
    fail("write-hup", "watch");
    close_pair(sp[0], sp[1]);
    host_poll_close(p);
    return;
  }
  close(sp[1]);
  sp[1] = -1;
  if (wait_one(p, &ev, "write-hup") < 0) {
    close_pair(sp[0], sp[1]);
    host_poll_close(p);
    return;
  }
  if (ev.fd != sp[0] || ev.timer) fail("write-hup", "fd");
  /* The spin state is hangup with no error and no writable. */
  if (ev.hangup && !ev.error && !ev.writable && !ev.readable)
    fail("write-hup", "spin flags");
  if (!ev.hangup && !ev.error && !ev.writable) fail("write-hup", "silent");
  drop_fd(p, sp[0]);
  close_pair(sp[0], sp[1]);

  if (pipe(pr) < 0) {
    fail("write-err", "pipe");
    host_poll_close(p);
    return;
  }
  if (host_poll_watch(p, pr[1], 0, 1) < 0) {
    fail("write-err", "watch");
    close_pair(pr[0], pr[1]);
    host_poll_close(p);
    return;
  }
  close(pr[0]);
  pr[0] = -1;
  if (wait_one(p, &ev, "write-err") < 0) {
    close_pair(pr[0], pr[1]);
    host_poll_close(p);
    return;
  }
  if (ev.fd != pr[1] || ev.timer) fail("write-err", "fd");
  if (!ev.error && !ev.hangup) fail("write-err", "flags");
  if (ev.hangup && !ev.error && !ev.writable && !ev.readable)
    fail("write-err", "spin flags");
  drop_fd(p, pr[1]);
  close_pair(pr[0], pr[1]);
  host_poll_close(p);
}

/* More than `max` ready fds: the second wait must include sockets
   the first wait omitted. */
static void check_scan(void) {
  enum { N = 40, BATCH = 32 };
  HostPoll *p = host_poll_open();
  HostEv evs[BATCH];
  int sv[N][2];
  int seen[N];
  int i;
  int n;
  int got;
  int first_unique;
  for (i = 0; i < N; i++) {
    sv[i][0] = sv[i][1] = -1;
    seen[i] = 0;
  }
  if (!p) {
    fail("scan", "open");
    return;
  }
  for (i = 0; i < N; i++) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv[i]) < 0) {
      fail("scan", "socketpair");
      goto done;
    }
    if (host_poll_watch(p, sv[i][0], 1, 0) < 0 || write(sv[i][1], "x", 1) != 1) {
      fail("scan", "watch");
      goto done;
    }
  }
  n = wait_retry(p, evs, BATCH);
  if (n != BATCH) {
    fprintf(stderr, "FAIL scan: first %d\n", n);
    failures += 1;
    goto done;
  }
  for (i = 0; i < n; i++) {
    int k;
    for (k = 0; k < N; k++) {
      if (evs[i].fd == sv[k][0]) {
        seen[k] = 1;
        break;
      }
    }
    if (k == N) fail("scan", "unknown fd");
  }
  first_unique = 0;
  for (i = 0; i < N; i++)
    if (seen[i]) first_unique += 1;
  if (first_unique != BATCH) {
    fprintf(stderr, "FAIL scan: first unique %d\n", first_unique);
    failures += 1;
    goto done;
  }
  n = wait_retry(p, evs, BATCH);
  if (n != BATCH) {
    fprintf(stderr, "FAIL scan: second %d\n", n);
    failures += 1;
    goto done;
  }
  for (i = 0; i < n; i++) {
    int k;
    for (k = 0; k < N; k++) {
      if (evs[i].fd == sv[k][0]) {
        seen[k] = 1;
        break;
      }
    }
    if (k == N) fail("scan", "unknown fd later");
  }
  got = 0;
  for (i = 0; i < N; i++)
    if (seen[i]) got += 1;
  if (got != N) {
    fprintf(stderr, "FAIL scan: covered %d of %d\n", got, N);
    failures += 1;
  }

done:
  if (p) {
    for (i = 0; i < N; i++) {
      if (sv[i][0] >= 0) host_poll_watch(p, sv[i][0], 0, 0);
    }
  }
  for (i = 0; i < N; i++) close_pair(sv[i][0], sv[i][1]);
  host_poll_close(p);
}

/* Unwatch after a partial scan must leave the cursor in range. */
static void check_drop(void) {
  HostPoll *p = host_poll_open();
  HostEv evs[2];
  int sp[3][2];
  int i;
  int n;
  for (i = 0; i < 3; i++) sp[i][0] = sp[i][1] = -1;
  if (!p) {
    fail("drop", "open");
    return;
  }
  for (i = 0; i < 3; i++) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp[i]) < 0 ||
        host_poll_watch(p, sp[i][0], 1, 0) < 0 || write(sp[i][1], "x", 1) != 1) {
      fail("drop", "setup");
      goto done;
    }
  }
  n = wait_retry(p, evs, 1);
  if (n != 1) {
    fail("drop", "first");
    goto done;
  }
  if (host_poll_watch(p, evs[0].fd, 0, 0) < 0) {
    fail("drop", "unwatch");
    goto done;
  }
  n = wait_retry(p, evs, 2);
  if (n < 1) fail("drop", "after unwatch");

done:
  if (p) {
    for (i = 0; i < 3; i++) {
      if (sp[i][0] >= 0) host_poll_watch(p, sp[i][0], 0, 0);
    }
  }
  for (i = 0; i < 3; i++) close_pair(sp[i][0], sp[i][1]);
  host_poll_close(p);
}

static void run_suite(const char *name) {
  HostPoll *p;
  const char *got;
  suite = name ? name : "default";
  if (name) setenv("MURO_HTTP_WAIT", name, 1);
  else unsetenv("MURO_HTTP_WAIT");
  p = host_poll_open();
  if (!p) {
    fail("open", "backend");
    return;
  }
  got = host_poll_backend(p);
  if (!got || (name && strcmp(got, name) != 0)) fail("open", "name");
  if (!name) {
#if defined(__APPLE__)
    if (!got || strcmp(got, "kqueue") != 0) fail("open", "default");
#elif defined(__linux__)
    if (!got || strcmp(got, "epoll") != 0) fail("open", "default");
#else
    if (!got || strcmp(got, "poll") != 0) fail("open", "default");
#endif
  }
  host_poll_close(p);
  check_deadline();
  check_empty();
  check_read_hangup();
  check_write_hangup();
  check_scan();
  check_drop();
}

static void reject(const char *name) {
  HostPoll *p;
  suite = name;
  setenv("MURO_HTTP_WAIT", name, 1);
  p = host_poll_open();
  if (p) {
    fail("reject", "opened");
    host_poll_close(p);
  }
  if (host_poll_supported(name)) fail("reject", "supported");
}

int main(void) {
  run_suite(NULL);
  run_suite("poll");
#if defined(__APPLE__)
  run_suite("kqueue");
  reject("epoll");
#elif defined(__linux__)
  run_suite("epoll");
  reject("kqueue");
#endif
  reject("select");
  unsetenv("MURO_HTTP_WAIT");
  if (failures) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
