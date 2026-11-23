/* Socket adapter for one worker. Poll, kqueue, or epoll, the
   listen socket, worker processes, and the SCM_RIGHTS handoff live
   here. HTTP/1.1 (classify, admit, the handler, keep or close, the
   body limit, the idle deadline) is c/step.c. This file turns an
   accepted fd, arrived bytes, a finished write, a peer close, and
   the clock into step facts, then writes the span the step returns.
   That span points at the connection's own send buffer. */

#include "shim.h"
#include "conn.h"
#include "proto.h"
#include "muro_http.h"
#include "host_poll.h"
#include "host_render.h"
#include "host_date.h"
#include "host_stream.h"
#include "step.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
  IN_CAP = 65536,
  OUT_CAP = 69632
};

static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t draining_flag;
static volatile sig_atomic_t parent_stop;
static uint64_t drain_deadline_ms;

int host_io_should_stop(void) {
  if (!draining_flag) return 0;
  return host_poll_now_ms() >= drain_deadline_ms;
}

/* sa_flags stays 0 on these handlers. SA_RESTART would hide the
   EINTR that lets an accept loop or a wait notice the flag.
   kevent changes, sendmsg, and the handoff poll retry EINTR
   themselves.

   The flag alone is not enough. The handler can run after the
   loop has read the flag and before poll, epoll, or kevent
   blocks with no timer. The write makes that wait return. A
   full pipe already has a byte, so EAGAIN is fine. */
static volatile sig_atomic_t stop_wake = -1;
static volatile sig_atomic_t parent_wake = -1;

static void poke_fd(int fd) {
  int saved = errno;
  char b = 1;
  if (fd < 0) {
    errno = saved;
    return;
  }
  for (;;) {
    if (write(fd, &b, 1) < 0 && errno == EINTR) continue;
    break;
  }
  errno = saved;
}

static void drain_wake(int fd) {
  char buf[64];
  if (fd < 0) return;
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;
  }
}

static void on_signal(int sig) {
  (void)sig;
  stop_flag = 1;
  poke_fd((int)stop_wake);
}

static void parent_on_signal(int sig) {
  (void)sig;
  parent_stop = 1;
  poke_fd((int)parent_wake);
}

static void on_sigchld(int sig) {
  (void)sig;
  poke_fd((int)parent_wake);
}

static const char *cfg_name(const MuroHttpConfig *cfg) {
  if (cfg && cfg->name && cfg->name[0]) return cfg->name;
  return "muro-http";
}

typedef struct Slot {
  int fd;
  int used;
  int want_read;
  int want_write;
  uint64_t deadline;
  int body_idle;
  struct HostIn hin;
  HttpStep step;
  const uint8_t *pend;
  size_t pend_len;
  int pend_interim; /* first byte of a 100 has not been written */
  int pend_cont; /* the parked span is 100 Continue, partial or not */
} Slot;

typedef struct Server {
  HostPoll *pq;
  Slot *slots;
  int max_conn;
  int nconn;
  int listen_fd;
  int idle_ms;
  int fatal;
  size_t write_limit;
  int write_fail_after; /* 0 off; else fail hard once this many writes succeeded */
  int writes_ok;
  const char *write_log;
  size_t in_cap;
  uint64_t armed_deadline;
  char name[32];
  char bind_host[16];
  MuroHttpHandler handler;
  void *user;
  uint64_t max_body;
  int drain_ms;
  int draining;
  uint64_t drain_deadline;
  int stall_100;
  int channel; /* listen_fd carries SCM_RIGHTS, it is not a TCP listener */
  int ack_owed; /* SCM_RIGHTS receipts not yet acked to the parent */
  int wake_r; /* self-pipe. SIGINT/SIGTERM write the other end. */
} Server;

/* The body reader re-enters this loop. NULL outside muro_http_serve. */
static Server *live_sv;
static int io_depth;
static int io_skip[8];

static int slice_in(const MuroHttpRequest *req, const uint8_t *p) {
  size_t i;
  if (!req || !req->buf || !p) return 0;
  for (i = 0; i <= req->buf_n; i++) {
    if (p == req->buf + i) return 1;
  }
  return 0;
}

/* A miss is the out-of-range empty. The C emitter turns that into the
   static empty, which does not point at this buffer. An empty value
   is a zero-length slice inside the buffer. */
static int header_present(const MuroHttpRequest *req, muro__slice v) {
  if (v.n == 0 && !slice_in(req, v.p)) return 0;
  return 1;
}

static MuroHttpSlice to_pub(muro__slice s) {
  MuroHttpSlice v;
  v.p = s.p;
  v.n = s.n;
  return v;
}

size_t muro_http_header_count(const MuroHttpRequest *req) {
  muro__slice buf;
  if (!req || !req->buf) return 0;
  buf.p = req->buf;
  buf.n = req->buf_n;
  return (size_t)headerCount(buf);
}

int muro_http_header_at(const MuroHttpRequest *req, size_t index, MuroHttpSlice *name,
                        MuroHttpSlice *value) {
  muro__slice buf;
  muro__slice n;
  muro__slice v;
  if (!req || !req->buf) return 0;
  buf.p = req->buf;
  buf.n = req->buf_n;
  n = headerName(buf, (uint64_t)index);
  if (!header_present(req, n)) return 0;
  v = headerValue(buf, (uint64_t)index);
  if (name) *name = to_pub(n);
  if (value) *value = to_pub(v);
  return 1;
}

int muro_http_header_find(const MuroHttpRequest *req, const char *name, MuroHttpSlice *value) {
  muro__slice buf;
  muro__slice v;
  if (!req || !req->buf || !name) return 0;
  buf.p = req->buf;
  buf.n = req->buf_n;
  v = findHeader(buf, muro__slice_of((const uint8_t *)name, strlen(name)));
  if (!header_present(req, v)) return 0;
  if (value) *value = to_pub(v);
  return 1;
}

static int set_nb_cloexec(int fd) {
  int fl = fcntl(fd, F_GETFL, 0);
  int fdfl;
  if (fl < 0) return -1;
  if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return -1;
  fdfl = fcntl(fd, F_GETFD, 0);
  if (fdfl < 0) return -1;
  return fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
}

static int set_int(int fd, int level, int opt, int value) {
  return setsockopt(fd, level, opt, &value, sizeof value);
}

/* MURO_TEST_WRITE_LOG is a test hook. It records how many bytes each
   socket write accepted. It does not choose a response. */
static void note_write(const Server *sv, size_t n) {
  char line[32];
  int len;
  int fd;
  int saved;
  size_t off = 0;
  if (!sv->write_log) return;
  len = snprintf(line, sizeof line, "%zu\n", n);
  if (len <= 0) return;
  saved = errno;
  fd = open(sv->write_log, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd >= 0) {
    while (off < (size_t)len) {
      ssize_t w = write(fd, line + off, (size_t)len - off);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) break;
      off += (size_t)w;
    }
    close(fd);
  }
  errno = saved;
}

/* MURO_TEST_IN_CAP lowers the receive buffer under the image's
   16384-byte malformed scan. Absent, the cap is IN_CAP. A full
   buffer the image did not consume still closes with no status. */
static size_t env_in_cap(void) {
  const char *e = getenv("MURO_TEST_IN_CAP");
  char *end = NULL;
  unsigned long cap;
  if (!e || !*e) return (size_t)IN_CAP;
  errno = 0;
  cap = strtoul(e, &end, 10);
  if (errno || end == e || *end || cap < 128ul || cap > (unsigned long)IN_CAP) return (size_t)IN_CAP;
  return (size_t)cap;
}

static size_t env_write_limit(void) {
  const char *e = getenv("MURO_TEST_WRITE_LIMIT");
  char *end = NULL;
  unsigned long cap;
  if (!e || !*e) return 0;
  errno = 0;
  cap = strtoul(e, &end, 10);
  if (errno || end == e || *end || cap == 0 || cap > 1024ul * 1024ul) return 0;
  return (size_t)cap;
}

/* MURO_TEST_WRITE_FAIL_AFTER is how many successful socket writes
   happen before the next one fails hard. The failure repeats until
   that connection closes, then writes work again. 0 leaves writes
   alone. Sampled once at startup, like MURO_TEST_WRITE_LIMIT. */
static int env_write_fail_after(void) {
  const char *e = getenv("MURO_TEST_WRITE_FAIL_AFTER");
  char *end = NULL;
  unsigned long n;
  if (!e || !*e) return 0;
  errno = 0;
  n = strtoul(e, &end, 10);
  if (errno || end == e || *end || n == 0 || n > 1000000ul) return 0;
  return (int)n;
}

/* MURO_TEST_100_STALL is how many 20ms pauses happen before the
   first interim byte, and only while the socket has no further
   bytes queued. MURO_TEST_100_READY is a path created on the first
   pause, so a client can send the body after the headers were taken
   and before any interim byte. The slot keeps the same rendered
   bytes. Production leaves the counter at 0. */
static int env_stall_100(void) {
  const char *e = getenv("MURO_TEST_100_STALL");
  char *end = NULL;
  unsigned long n;
  if (!e || !*e) return 0;
  errno = 0;
  n = strtoul(e, &end, 10);
  if (errno || end == e || *end || n > 1000ul) return 0;
  return (int)n;
}

static int sync_listen(Server *sv) {
  int on;
  int err;
  if (sv->listen_fd < 0) return 0;
  on = sv->nconn < sv->max_conn;
  if (host_poll_watch(sv->pq, sv->listen_fd, on, sv->ack_owed > 0) < 0) {
    err = errno;
    fprintf(stderr, "%s: listen watch failed: %s\n", sv->name, strerror(err));
    sv->fatal = 1;
    return -1;
  }
  return 0;
}

static void close_slot(Server *sv, Slot *s);
static void status_before_close(Server *sv, Slot *s);

static void sync_watch(Server *sv, Slot *s) {
  int err;
  if (!s->used) return;
  if (host_poll_watch(sv->pq, s->fd, s->want_read, s->want_write) < 0) {
    err = errno;
    fprintf(stderr, "%s: watch %d failed: %s\n", sv->name, s->fd, strerror(err));
    close_slot(sv, s);
  }
}

static void close_slot(Server *sv, Slot *s) {
  if (!s->used) return;
  /* The forced failure has done its job once this slot closes.
     A later connection must still be able to write. */
  if (sv->write_fail_after > 0 && sv->writes_ok >= sv->write_fail_after) sv->write_fail_after = 0;
  status_before_close(sv, s);
  host_poll_watch(sv->pq, s->fd, 0, 0);
  close(s->fd);
  muro_http_step_shutdown(&s->step);
  host_in_close(&s->hin);
  free(s->step.in);
  free(s->step.out);
  memset(s, 0, sizeof *s);
  s->fd = -1;
  if (sv->nconn > 0) sv->nconn -= 1;
  (void)sync_listen(sv);
}

/* Best-effort status before close. The fd is non-blocking. A short
   write is retried immediately. EAGAIN or EWOULDBLOCK returns
   without waiting: a poll or a sleep here would stall every other
   socket. The peer may miss this last status if its receive window
   is already full. */
static void write_status(Server *sv, int fd, const uint8_t *p, size_t n) {
  size_t off = 0;
  if (fd < 0 || !p || n == 0) return;
  while (off < n) {
    ssize_t w = write(fd, p + off, n - off);
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (w <= 0) return;
    off += (size_t)w;
    note_write(sv, (size_t)w);
  }
}

/* owe is set when the handler committed a side effect. If no status
   byte has left the socket, send one before the fd is closed. A
   rendered buffer is that status. Otherwise the book supplies 500.
   The fd is non-blocking. write_status does not wait. */
static void status_before_close(Server *sv, Slot *s) {
  const uint8_t *msg = NULL;
  size_t n = 0;
  int log500 = 0;
  char date[30];
  if (s->fd < 0) return;
  host_date_fill(date);
  muro_http_step_close_status(&s->step, (const uint8_t *)date, 29, &msg, &n, &log500);
  if (log500) fprintf(stderr, "%s: committed write has no status; sending 500\n", sv->name);
  if (n > 0) write_status(sv, s->fd, msg, n);
}

/* One create. The test process waits on this path before it sends
   the body, so the body cannot land in the same read as the headers. */
static void note_100_ready(void) {
  const char *path;
  int fd;
  static int done = 0;
  if (done) return;
  path = getenv("MURO_TEST_100_READY");
  if (!path || !path[0]) {
    done = 1;
    return;
  }
  fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) return;
  if (write(fd, "1\n", 2) < 0) {
    close(fd);
    return;
  }
  close(fd);
  done = 1;
}

static int sock_pending(int fd) {
  int pending = 0;
  if (ioctl(fd, FIONREAD, &pending) < 0 || pending <= 0) return 0;
  return 1;
}

/* The step returns a span. This writes it, or parks it when the
   socket cannot take more. A 100 Continue span can pause before the
   first byte when MURO_TEST_100_STALL is set and the socket has
   nothing queued. */
static void log_step(const Server *sv, const StepOut *out) {
  int i;
  for (i = 0; i < 2; i++) {
    int note = i == 0 ? out->note : out->note2;
    size_t n = i == 0 ? out->note_n : out->note_n2;
    if (note == MURO_STEP_NOTE_ADMIT)
      fprintf(stderr, "%s: admit refused phase %d ev %d\n", sv->name, out->note_phase, out->note_ev);
    else if (note == MURO_STEP_NOTE_TOO_BIG_COMMIT)
      fprintf(stderr, "%s: response is %zu bytes after commit; sending 500\n", sv->name, n);
    else if (note == MURO_STEP_NOTE_TOO_BIG)
      fprintf(stderr, "%s: response is %zu bytes\n", sv->name, n);
    else if (note == MURO_STEP_NOTE_HANDLER_500)
      fprintf(stderr, "%s: handler failed after a commit; sending 500\n", sv->name);
    else if (note == MURO_STEP_NOTE_FUEL)
      fprintf(stderr, "%s: pump fuel exhausted\n", sv->name);
  }
}

/* 1 and *wrote set when the socket accepted one or more bytes, even
   when that is less than len. The caller reports MURO_STEP_WROTE for
   that count. 0 when no byte was accepted: EAGAIN, the first-byte
   stall, or the EINTR cap. The span stays outstanding. -1 on a hard
   write error. A signal retry stays here. */
static int emit_span(Server *sv, Slot *s, const uint8_t *p, size_t len, int interim, size_t *wrote) {
  int spins = 0;
  /* Stall only before the first interim byte, and only while the
     socket has nothing queued. A body already waiting clears the
     counter so the pause cannot land on a later 100. */
  if (interim && sv->stall_100 > 0) {
    struct timespec ts;
    note_100_ready();
    if (!sock_pending(s->fd)) {
      sv->stall_100 -= 1;
      s->want_read = 0;
      s->want_write = 1;
      ts.tv_sec = 0;
      ts.tv_nsec = 20000000L;
      while (nanosleep(&ts, &ts) < 0 && errno == EINTR) continue;
      return 0;
    }
    sv->stall_100 = 0;
  }
  if (sv->write_fail_after > 0 && sv->writes_ok >= sv->write_fail_after) return -1;
  while (spins < 64) {
    size_t chunk = len;
    ssize_t n;
    if (sv->write_limit > 0 && chunk > sv->write_limit) chunk = sv->write_limit;
    n = write(s->fd, p, chunk);
    if (n < 0 && errno == EINTR) {
      spins += 1;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (n <= 0) return -1;
    *wrote = (size_t)n;
    sv->writes_ok += 1;
    note_write(sv, (size_t)n);
    return 1;
  }
  return 0;
}

static void drive(Server *sv, Slot *s, StepIn *in);

static void drive(Server *sv, Slot *s, StepIn *in) {
  char date[30];
  int guard = 0;
  /* One clock sample for this turn. The idle deadline is armed from
     it. Refreshing it on every partial write would add a clock read
     to the keep-alive path. */
  host_date_fill(date);
  in->date = (const uint8_t *)date;
  in->date_n = 29;
  in->now_ms = host_poll_now_ms();
  while (s->used && guard < 1000000) {
    StepOut out;
    int rc;
    size_t wrote = 0;
    int er;
    guard += 1;
    in->deadline = s->deadline;
    rc = muro_http_step(&s->step, in, &out);
    log_step(sv, &out);
    if (rc < 0) {
      close_slot(sv, s);
      return;
    }
    if (out.close_conn) {
      if (out.status_now && out.status && out.status_len)
        write_status(sv, s->fd, out.status, out.status_len);
      close_slot(sv, s);
      return;
    }
    if (out.bytes && out.len) {
      er = emit_span(sv, s, out.bytes, out.len, out.interim, &wrote);
      if (er == 0) {
        s->want_read = out.read_more;
        s->want_write = 1;
        s->deadline = out.wake_at;
        s->pend = out.bytes;
        s->pend_len = out.len;
        s->pend_interim = out.interim;
        s->pend_cont = out.cont;
        return;
      }
      s->pend = NULL;
      s->pend_len = 0;
      s->pend_interim = 0;
      s->pend_cont = 0;
      if (er < 0) {
        /* Any byte of a 100, including after a short write. The
           pre-split pump closed on that failure and did not step
           evError, which would hold in READ and spin. */
        if (out.cont) {
          close_slot(sv, s);
          return;
        }
        in->kind = MURO_STEP_WRITE_FAILED;
        in->n = 0;
        continue;
      }
      in->kind = MURO_STEP_WROTE;
      in->n = wrote;
      continue;
    }
    s->want_read = out.read_more;
    s->want_write = out.write_more;
    s->deadline = out.wake_at;
    s->pend = NULL;
    s->pend_len = 0;
    s->pend_interim = 0;
    s->pend_cont = 0;
    return;
  }
  if (s->used) {
    fprintf(stderr, "%s: pump fuel exhausted\n", sv->name);
    close_slot(sv, s);
  }
}
static void resume_write(Server *sv, Slot *s) {
  StepIn in;
  size_t wrote = 0;
  int rc;
  if (!s->used || !s->pend || s->pend_len == 0) return;
  rc = emit_span(sv, s, s->pend, s->pend_len, s->pend_interim, &wrote);
  if (rc == 0) {
    s->want_write = 1;
    sync_watch(sv, s);
    return;
  }
  if (rc < 0 && s->pend_cont) {
    close_slot(sv, s);
    return;
  }
  memset(&in, 0, sizeof in);
  if (rc < 0) in.kind = MURO_STEP_WRITE_FAILED;
  else {
    in.kind = MURO_STEP_WROTE;
    in.n = wrote;
  }
  s->pend = NULL;
  s->pend_len = 0;
  s->pend_interim = 0;
  s->pend_cont = 0;
  drive(sv, s, &in);
  if (s->used) sync_watch(sv, s);
}

static void on_readable(Server *sv, Slot *s, int hangup) {
  uint8_t *p = NULL;
  size_t len = 0;
  size_t cap = 0;
  int got = 0;
  int hit_eof = 0;
  int fresh;
  StepIn in;
  muro_http_step_recv(&s->step, &p, &len, &cap);
  fresh = len == 0;
  while (p && len < cap) {
    ssize_t n = read(s->fd, p + len, cap - len);
    if (n > 0) {
      len += (size_t)n;
      muro_http_step_set_in_len(&s->step, len);
      got = 1;
      continue;
    }
    if (n == 0) {
      hit_eof = 1;
      break;
    }
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
    close_slot(sv, s);
    return;
  }
  if (!got && !hit_eof && !hangup) {
    if (len >= cap) close_slot(sv, s);
    return;
  }
  s->deadline = 0;
  memset(&in, 0, sizeof in);
  in.kind = (hit_eof || hangup) ? MURO_STEP_PEER_CLOSED : MURO_STEP_BYTES;
  in.empty_before = fresh;
  drive(sv, s, &in);
  if (s->used) sync_watch(sv, s);
}

static int fd_skipped(int fd);

static void on_tick(Server *sv, Slot *s, uint64_t now) {
  int phase;
  StepIn in;
  if (!s->used || s->deadline == 0 || s->deadline > now) return;
  /* A socket on io_skip is inside muro_http_body_read. Do not step
     the image: the handler or the pull is still on the stack.
     Decide and write leave the book timer off, so a deadline there
     was armed by that wait. Read and idle arm the book timer too.
     Clearing the deadline without body_idle lets the next
     host_io_once arm a new window, and the wait never ends. Set
     body_idle in every phase. The wait returns and that caller
     closes. Do not close_slot here. */
  if (fd_skipped(s->fd)) {
    s->deadline = 0;
    s->body_idle = 1;
    return;
  }
  phase = muro_http_step_phase(&s->step);
  if (phase != MURO_STEP_PH_READ && phase != MURO_STEP_PH_IDLE) {
    s->deadline = 0;
    s->body_idle = 1;
    return;
  }
  s->deadline = 0;
  memset(&in, 0, sizeof in);
  in.kind = MURO_STEP_NOW;
  drive(sv, s, &in);
  if (s->used) sync_watch(sv, s);
}

/* These servers do not call muro_http_body_read while the image is
   in read or idle. c/test_serve still runs this branch: a skipped
   fd, phase read or idle, deadline already due. Returns 0 when
   on_tick sets body_idle, clears the deadline, and leaves the slot
   open. A fall-through would step the image or close the slot. */
int host_test_skipped_tick(int phase) {
  Server sv;
  Slot s;
  int saved_depth;
  int saved_skip;
  int rc;
  saved_depth = io_depth;
  saved_skip = io_skip[0];
  memset(&sv, 0, sizeof sv);
  memset(&s, 0, sizeof s);
  sv.listen_fd = -1;
  s.used = 1;
  /* -1 is not a live socket. close(-1) does not drop a real fd
     if on_tick falls through into close_slot. */
  s.fd = -1;
  s.deadline = 1;
  muro_http_step_set_phase(&s.step, phase);
  io_skip[0] = s.fd;
  io_depth = 1;
  on_tick(&sv, &s, 2);
  rc = (s.used && s.body_idle == 1 && s.deadline == 0) ? 0 : -1;
  io_depth = saved_depth;
  io_skip[0] = saved_skip;
  return rc;
}

static int take_conn(int lfd, struct sockaddr_in *peer, socklen_t *len) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
  int fd = accept4(lfd, (struct sockaddr *)peer, len, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (fd >= 0 || errno != ENOSYS) return fd;
#endif
  return accept(lfd, (struct sockaddr *)peer, len);
}

/* 0 adopted, 1 skipped this fd, -1 stop accepting. Closes fd unless
   it was adopted. */
static int adopt_fd(Server *sv, int fd) {
  Slot *s = NULL;
  int i;
  uint8_t *inb;
  uint8_t *outb;
  StepIn in;
  if (set_nb_cloexec(fd) < 0) {
    close(fd);
    return 1;
  }
  if (set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1) < 0) {
    close(fd);
    return 1;
  }
#ifdef SO_NOSIGPIPE
  if (set_int(fd, SOL_SOCKET, SO_NOSIGPIPE, 1) < 0) {
    close(fd);
    return 1;
  }
#endif
  for (i = 0; i < sv->max_conn; i++) {
    if (!sv->slots[i].used) {
      s = &sv->slots[i];
      break;
    }
  }
  if (!s) {
    close(fd);
    return -1;
  }
  memset(s, 0, sizeof *s);
  s->fd = fd;
  inb = malloc(sv->in_cap);
  outb = malloc((size_t)OUT_CAP);
  if (!inb || !outb) {
    free(inb);
    free(outb);
    close(fd);
    memset(s, 0, sizeof *s);
    s->fd = -1;
    fprintf(stderr, "%s: connection setup failed\n", sv->name);
    return -1;
  }
  muro_http_step_bind(&s->step, inb, sv->in_cap, outb, (size_t)OUT_CAP, &s->hin, &s->want_read,
                      &s->want_write, sv->handler, sv->user, sv->max_body, sv->idle_ms);
  host_in_init(&s->hin, fd, sv->max_body);
  s->used = 1;
  sv->nconn += 1;
  memset(&in, 0, sizeof in);
  in.kind = MURO_STEP_ACCEPTED;
  drive(sv, s, &in);
  if (!s->used) return -1;
  sync_watch(sv, s);
  if (sv->fatal) return -1;
  return 0;
}

/* Close a complete SCM_RIGHTS fd that arrived in a truncated
   control buffer. A short header is not a socket to close. */
static void close_truncated_fds(struct msghdr *msg, char *cbuf, size_t cap) {
  struct cmsghdr *cmsg;
  unsigned char *base = (unsigned char *)cbuf;
  if (msg->msg_controllen > cap) return;
  for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
    unsigned char *hdr = (unsigned char *)cmsg;
    int passed = -1;
    size_t off;
    if (hdr < base) break;
    off = (size_t)(hdr - base);
    if (off + sizeof(struct cmsghdr) > msg->msg_controllen) break;
    if (cmsg->cmsg_len < CMSG_LEN(sizeof(int))) continue;
    if (off + cmsg->cmsg_len > msg->msg_controllen) continue;
    if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
    memcpy(&passed, CMSG_DATA(cmsg), sizeof passed);
    if (passed >= 0) close(passed);
  }
}

/* One SCM_RIGHTS message. 1 and *out set, 0 on a clean close, -1 on
   error. *out is -1 when the message carried no fd. MSG_CTRUNC is
   an error: it is not "no fd" and it is not a socket to adopt. */
static int recv_passed(int ch, int *out) {
  struct msghdr msg;
  struct iovec iov;
  char dummy = 0;
  char cbuf[CMSG_SPACE(sizeof(int))];
  struct cmsghdr *cmsg;
  ssize_t n;
  *out = -1;
  memset(&msg, 0, sizeof msg);
  memset(cbuf, 0, sizeof cbuf);
  iov.iov_base = &dummy;
  iov.iov_len = 1;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof cbuf;
  n = recvmsg(ch, &msg, 0);
  if (n < 0) return -1;
  if (n == 0) return 0;
  if (msg.msg_flags & MSG_CTRUNC) {
    close_truncated_fds(&msg, cbuf, sizeof cbuf);
    errno = EMSGSIZE;
    return -1;
  }
  for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
        cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
      memcpy(out, CMSG_DATA(cmsg), sizeof *out);
      break;
    }
  }
  return 1;
}

/* One byte per received fd. The parent keeps its own copy until
   these bytes arrive, so it must read them or the socketpair fills. */
static void flush_acks(Server *sv) {
  if (!sv->channel || sv->listen_fd < 0) return;
  while (sv->ack_owed > 0) {
    char buf[64];
    int n = sv->ack_owed > 64 ? 64 : sv->ack_owed;
    ssize_t w;
    memset(buf, 'a', (size_t)n);
    w = send(sv->listen_fd, buf, (size_t)n, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (w <= 0) {
      sv->ack_owed = 0;
      return;
    }
    sv->ack_owed -= (int)w;
  }
}

static void take_passed(Server *sv) {
  while (sv->nconn < sv->max_conn && !sv->fatal && sv->listen_fd >= 0) {
    int fd = -1;
    int rc = recv_passed(sv->listen_fd, &fd);
    if (rc < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      fprintf(stderr, "%s: recv fd: %s\n", sv->name, strerror(errno));
      break;
    }
    if (rc == 0) {
      int lfd = sv->listen_fd;
      sv->listen_fd = -1;
      if (sv->pq) (void)host_poll_watch(sv->pq, lfd, 0, 0);
      close(lfd);
      break;
    }
    if (fd < 0) continue;
    sv->ack_owed += 1;
    flush_acks(sv);
    if (adopt_fd(sv, fd) < 0) break;
  }
  (void)sync_listen(sv);
}

static void accept_some(Server *sv) {
  if (sv->listen_fd < 0) return;
  if (sv->channel) {
    flush_acks(sv);
    if (!sv->draining) take_passed(sv);
    else (void)sync_listen(sv);
    return;
  }
  if (sv->draining) return;
  while (sv->nconn < sv->max_conn && !sv->fatal) {
    struct sockaddr_in peer;
    socklen_t len = sizeof peer;
    int fd = take_conn(sv->listen_fd, &peer, &len);
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      fprintf(stderr, "%s: accept: %s\n", sv->name, strerror(errno));
      return;
    }
    if (adopt_fd(sv, fd) < 0) return;
  }
  (void)sync_listen(sv);
}

static Slot *find_slot(Server *sv, int fd) {
  int i;
  if (fd < 0) return NULL;
  for (i = 0; i < sv->max_conn; i++) {
    if (sv->slots[i].used && sv->slots[i].fd == fd) return &sv->slots[i];
  }
  return NULL;
}

static void arm_soonest(Server *sv) {
  uint64_t soon = 0;
  uint64_t now;
  unsigned ms;
  int i;
  int any = 0;
  for (i = 0; i < sv->max_conn; i++) {
    Slot *s = &sv->slots[i];
    if (!s->used || s->deadline == 0) continue;
    if (!any || s->deadline < soon) {
      soon = s->deadline;
      any = 1;
    }
  }
  if (sv->draining && sv->drain_deadline != 0 && (!any || sv->drain_deadline < soon)) {
    soon = sv->drain_deadline;
    any = 1;
  }
  if (!any) {
    if (sv->armed_deadline != 0) {
      host_poll_arm(sv->pq, 0);
      sv->armed_deadline = 0;
    }
    return;
  }
  if (soon == sv->armed_deadline) return;
  now = host_poll_now_ms();
  if (soon <= now) ms = 1;
  else if (soon - now > 86400000ull) ms = 86400000u;
  else ms = (unsigned)(soon - now);
  if (host_poll_arm(sv->pq, ms) < 0) {
    fprintf(stderr, "%s: timer: %s\n", sv->name, strerror(errno));
    return;
  }
  sv->armed_deadline = soon;
}

static int listen_on(const char *host, int port, int reuseport) {
#if defined(SOCK_CLOEXEC)
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
  int fd = socket(AF_INET, SOCK_STREAM, 0);
#endif
  struct sockaddr_in addr;
  if (fd < 0) return -1;
  if (set_int(fd, SOL_SOCKET, SO_REUSEADDR, 1) < 0) {
    close(fd);
    return -1;
  }
  if (reuseport) {
#ifdef SO_REUSEPORT
    if (set_int(fd, SOL_SOCKET, SO_REUSEPORT, 1) < 0) {
      close(fd);
      return -1;
    }
#else
    (void)reuseport;
    close(fd);
    errno = ENOTSUP;
    return -1;
#endif
  }
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(fd);
    return -2;
  }
  if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    close(fd);
    return -1;
  }
  /* The kernel clamps this to somaxconn. A busy handoff stops
     accept and this queue is the backpressure. */
  if (listen(fd, SOMAXCONN) < 0) {
    close(fd);
    return -1;
  }
  if (set_nb_cloexec(fd) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int bound_port(int fd) {
  struct sockaddr_in addr;
  socklen_t len = sizeof addr;
  if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) return -1;
  return (int)ntohs(addr.sin_port);
}

static int parse_i(const char *s, long lo, long hi, int *out) {
  char *end = NULL;
  long v;
  if (!s || !*s) return -1;
  errno = 0;
  v = strtol(s, &end, 10);
  if (errno || !end || *end || v < lo || v > hi) return -1;
  *out = (int)v;
  return 0;
}

void muro_http_config_init(MuroHttpConfig *cfg) {
  if (!cfg) return;
  memset(cfg, 0, sizeof *cfg);
  cfg->port = 8080;
  cfg->idle_ms = 5000;
  cfg->max_conn = 128;
  cfg->max_body = 8 * 1024 * 1024;
  cfg->drain_ms = 1000;
  cfg->workers = 1;
}

void muro_http_config_usage(FILE *out, const MuroHttpConfig *cfg) {
  const char *name = cfg_name(cfg);
  if (!out) return;
  fprintf(out,
          "%s — HTTP/1.1\n"
          "\n"
          "  --bind HOST            IPv4 address. default 127.0.0.1\n"
          "  --port N               listen port (0 picks one). default 8080\n"
          "  --idle-ms N            idle tick. default 5000\n"
          "  --max-connections N    accept cap. default 128, max 1024\n"
          "  --max-body N           body cap in bytes. default 8388608\n"
          "  --drain-ms N           SIGINT/SIGTERM drain. default 1000\n"
          "  --workers N            processes. default 1, max 64\n"
          "  --wait NAME            poll, kqueue, or epoll. default is native\n"
          "\n"
          "Environment, used when the flag is absent: MURO_HTTP_BIND,\n"
          "MURO_HTTP_PORT, MURO_HTTP_IDLE_MS, MURO_HTTP_MAX_CONNECTIONS,\n"
          "MURO_HTTP_MAX_BODY, MURO_HTTP_DRAIN_MS, MURO_HTTP_WORKERS,\n"
          "MURO_HTTP_WAIT.\n"
          "The default bind host is 127.0.0.1. One worker is one process.\n"
          "max-connections is per worker. SIGINT and SIGTERM stop\n"
          "accepting, finish an in-flight request, and close idle\n"
          "connections. Each worker drains itself. The process exits\n"
          "when the drain time is up.\n",
          name);
}

static int take_env_i(const MuroHttpConfig *cfg, const char *env, long lo, long hi, int *slot) {
  const char *v = getenv(env);
  if (!v) return 0;
  if (parse_i(v, lo, hi, slot) < 0) {
    fprintf(stderr, "%s: bad %s\n", cfg_name(cfg), env);
    return -1;
  }
  return 0;
}

int muro_http_config_env(MuroHttpConfig *cfg) {
  const char *bind;
  if (!cfg) return -1;
  if (take_env_i(cfg, "MURO_HTTP_PORT", 0, 65535, &cfg->port) < 0) return -1;
  if (take_env_i(cfg, "MURO_HTTP_IDLE_MS", 1, 86400000, &cfg->idle_ms) < 0) return -1;
  if (take_env_i(cfg, "MURO_HTTP_MAX_CONNECTIONS", 1, 1024, &cfg->max_conn) < 0) return -1;
  if (take_env_i(cfg, "MURO_HTTP_MAX_BODY", 0, 2147483647L, &cfg->max_body) < 0) return -1;
  if (take_env_i(cfg, "MURO_HTTP_DRAIN_MS", 0, 86400000L, &cfg->drain_ms) < 0) return -1;
  if (take_env_i(cfg, "MURO_HTTP_WORKERS", 1, 64, &cfg->workers) < 0) return -1;
  {
    const char *wait = getenv("MURO_HTTP_WAIT");
    if (wait && (!wait[0] || !host_poll_supported(wait))) {
      fprintf(stderr, "%s: bad MURO_HTTP_WAIT\n", cfg_name(cfg));
      return -1;
    }
  }
  bind = getenv("MURO_HTTP_BIND");
  if (bind) {
    if (!bind[0]) {
      fprintf(stderr, "%s: bad MURO_HTTP_BIND\n", cfg_name(cfg));
      return -1;
    }
    cfg->bind_host = bind;
  }
  return 0;
}

int muro_http_config_arg(MuroHttpConfig *cfg, int *index, int argc, char **argv) {
  const char *a;
  const char *val;
  int *slot = NULL;
  long lo = 0;
  long hi = 0;
  if (!cfg || !index || !argv || *index < 0 || *index >= argc) return -1;
  a = argv[*index];
  if (!a) return -1;
  if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) return 2;
  if (strcmp(a, "--bind") == 0) {
    if (*index + 1 >= argc) {
      fprintf(stderr, "%s: --bind needs a value\n", cfg_name(cfg));
      return -1;
    }
    *index += 1;
    val = argv[*index];
    if (!val || !val[0]) {
      fprintf(stderr, "%s: bad --bind\n", cfg_name(cfg));
      return -1;
    }
    cfg->bind_host = val;
    return 1;
  }
  if (strcmp(a, "--wait") == 0) {
    if (*index + 1 >= argc) {
      fprintf(stderr, "%s: --wait needs a value\n", cfg_name(cfg));
      return -1;
    }
    *index += 1;
    val = argv[*index];
    if (!val || !host_poll_supported(val)) {
      fprintf(stderr, "%s: bad --wait\n", cfg_name(cfg));
      return -1;
    }
    if (setenv("MURO_HTTP_WAIT", val, 1) != 0) return -1;
    return 1;
  }
  if (strcmp(a, "--port") == 0) {
    slot = &cfg->port;
    lo = 0;
    hi = 65535;
  } else if (strcmp(a, "--idle-ms") == 0) {
    slot = &cfg->idle_ms;
    lo = 1;
    hi = 86400000;
  } else if (strcmp(a, "--max-connections") == 0) {
    slot = &cfg->max_conn;
    lo = 1;
    hi = 1024;
  } else if (strcmp(a, "--max-body") == 0) {
    slot = &cfg->max_body;
    lo = 0;
    hi = 2147483647L;
  } else if (strcmp(a, "--drain-ms") == 0) {
    slot = &cfg->drain_ms;
    lo = 0;
    hi = 86400000L;
  } else if (strcmp(a, "--workers") == 0) {
    slot = &cfg->workers;
    lo = 1;
    hi = 64;
  } else {
    return 0;
  }
  if (*index + 1 >= argc) {
    fprintf(stderr, "%s: %s needs a value\n", cfg_name(cfg), a);
    return -1;
  }
  *index += 1;
  val = argv[*index];
  if (parse_i(val, lo, hi, slot) < 0) {
    fprintf(stderr, "%s: bad %s %s\n", cfg_name(cfg), a, val ? val : "");
    return -1;
  }
  return 1;
}

int muro_http_config_parse(MuroHttpConfig *cfg, int argc, char **argv) {
  int i;
  if (!cfg) return -1;
  if (muro_http_config_env(cfg) < 0) return -1;
  for (i = 1; i < argc; i++) {
    int rc = muro_http_config_arg(cfg, &i, argc, argv);
    if (rc == 2) {
      muro_http_config_usage(stdout, cfg);
      return 1;
    }
    if (rc < 0) return -1;
    if (rc == 0) {
      fprintf(stderr, "%s: unknown argument %s\n", cfg_name(cfg), argv[i]);
      muro_http_config_usage(stderr, cfg);
      return -1;
    }
  }
  return 0;
}

static int slot_idle(Slot *s) {
  int phase;
  if (!s->used) return 0;
  phase = muro_http_step_phase(&s->step);
  if (phase == MURO_STEP_PH_IDLE) return 1;
  if (phase == MURO_STEP_PH_READ && muro_http_step_in_len(&s->step) == 0) return 1;
  return 0;
}

static void close_idles(Server *sv) {
  int i;
  for (i = 0; i < sv->max_conn; i++) {
    if (slot_idle(&sv->slots[i])) close_slot(sv, &sv->slots[i]);
  }
}

static void begin_drain(Server *sv) {
  if (sv->draining) return;
  sv->draining = 1;
  draining_flag = 1;
  sv->drain_deadline = host_poll_now_ms() + (uint64_t)sv->drain_ms;
  drain_deadline_ms = sv->drain_deadline;
  if (sv->listen_fd >= 0) {
    int lfd = sv->listen_fd;
    sv->listen_fd = -1;
    if (sv->pq) (void)host_poll_watch(sv->pq, lfd, 0, 0);
    close(lfd);
  }
}

/* begin_drain is idempotent. While draining, close idle slots on
   every turn, including a turn that started inside a body read.
   The connection inside the handler is HANDLE, not idle. */
static void note_drain(Server *sv) {
  if (stop_flag) begin_drain(sv);
  if (sv->draining) close_idles(sv);
}

static int fd_skipped(int fd) {
  int i;
  for (i = 0; i < io_depth; i++) {
    if (io_skip[i] == fd) return 1;
  }
  return 0;
}

/* 1 when the socket is readable or hung up. 0 on timeout or EINTR.
   -1 when poll fails. */
static int poll_one(int fd, int ms) {
  struct pollfd pfd;
  int pr;
  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;
  pr = poll(&pfd, 1, ms);
  if (pr < 0 && errno == EINTR) return 0;
  if (pr < 0) return -1;
  if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) return 1;
  return 0;
}

/* One turn of the server poll from inside a body read. The watched
   fd is not dispatched: its handler is already on the stack, and a
   level-triggered read there would steal or spin. Other fds are
   accepted, read, and written. Depth past 8 waits on this fd only.
   SIGTERM arms the drain here, because the main loop is not polling. */
int host_io_once(int watch_fd) {
  Server *sv = live_sv;
  HostEv evs[32];
  Slot *body = NULL;
  int saved_rd = 0;
  int saved_wr = 0;
  int saw_body = 0;
  int n;
  int i;
  if (watch_fd < 0) return -1;
  if (!sv) {
    if (host_io_should_stop()) return -1;
    return poll_one(watch_fd, 200);
  }
  note_drain(sv);
  if (sv->draining && (sv->nconn == 0 || host_poll_now_ms() >= sv->drain_deadline)) return -1;
  if (io_depth >= 8) {
    Slot *deep = find_slot(sv, watch_fd);
    int wait_ms = 200;
    uint64_t now = host_poll_now_ms();
    int rc;
    if (deep && deep->used) {
      uint64_t left;
      if (deep->deadline == 0) deep->deadline = now + (uint64_t)sv->idle_ms;
      if (deep->deadline <= now) {
        rc = poll_one(watch_fd, 0);
        if (rc > 0) return 0;
        if (rc < 0) return -1;
        deep->deadline = 0;
        deep->body_idle = 1;
        return -1;
      }
      left = deep->deadline - now;
      if (left > 86400000ull) left = 86400000ull;
      wait_ms = (int)left;
    }
    rc = poll_one(watch_fd, wait_ms);
    note_drain(sv);
    if (sv->draining && host_poll_now_ms() >= sv->drain_deadline) return -1;
    if (rc < 0) return -1;
    if (rc == 0 && deep && deep->used && deep->deadline != 0 &&
        host_poll_now_ms() >= deep->deadline) {
      deep->deadline = 0;
      deep->body_idle = 1;
      return -1;
    }
    return 0;
  }
  body = find_slot(sv, watch_fd);
  if (body) {
    uint64_t now = host_poll_now_ms();
    saved_rd = body->want_read;
    saved_wr = body->want_write;
    /* POLLIN only. A writable socket is level-triggered and would
       wake this wait on every turn while the handler is still
       reading the body. */
    body->want_read = 1;
    body->want_write = 0;
    sync_watch(sv, body);
    /* The book timer is off in decide and write. Arm the host idle
       deadline so a stalled body still expires. A byte of progress
       clears it. */
    body->body_idle = 0;
    if (body->deadline == 0) body->deadline = now + (uint64_t)sv->idle_ms;
  }
  io_skip[io_depth] = watch_fd;
  io_depth += 1;
  arm_soonest(sv);
  n = host_poll_wait(sv->pq, evs, 32);
  if (body && body->used && body->fd == watch_fd) {
    body->want_read = saved_rd;
    body->want_write = saved_wr;
    sync_watch(sv, body);
  }
  if (n < 0) {
    io_depth -= 1;
    return -1;
  }
  note_drain(sv);
  for (i = 0; i < n; i++) {
    Slot *s;
    if (body && evs[i].fd == watch_fd && (evs[i].readable || evs[i].hangup)) saw_body = 1;
    if (evs[i].timer) {
      uint64_t now = host_poll_now_ms();
      int k;
      sv->armed_deadline = 0;
      for (k = 0; k < sv->max_conn; k++) on_tick(sv, &sv->slots[k], now);
      continue;
    }
    if (fd_skipped(evs[i].fd)) continue;
    if (evs[i].fd == sv->wake_r) {
      drain_wake(sv->wake_r);
      continue;
    }
    if (sv->listen_fd >= 0 && evs[i].fd == sv->listen_fd) {
      accept_some(sv);
      continue;
    }
    s = find_slot(sv, evs[i].fd);
    if (!s) continue;
    if (evs[i].error && !evs[i].readable && !evs[i].writable) {
      close_slot(sv, s);
      continue;
    }
    if (s->used && evs[i].writable && s->want_write) resume_write(sv, s);
    if (s->used && evs[i].readable && s->want_read) on_readable(sv, s, evs[i].hangup);
    else if (s->used && evs[i].hangup && s->want_read) on_readable(sv, s, 1);
    else if (s->used && evs[i].hangup && !evs[i].writable) close_slot(sv, s);
  }
  io_depth -= 1;
  if (body && body->used && body->fd == watch_fd && body->body_idle && !saw_body) return -1;
  if (sv->draining && (sv->nconn == 0 || host_poll_now_ms() >= sv->drain_deadline)) return -1;
  return 0;
}

/* A body byte arrived. The next wait starts a new idle window. */
void host_io_progress(int fd) {
  Slot *s;
  if (!live_sv || fd < 0) return;
  s = find_slot(live_sv, fd);
  if (!s || !s->used) return;
  s->deadline = 0;
  s->body_idle = 0;
}

static int serve_one(const MuroHttpConfig *cfg, MuroHttpHandler handler, void *user,
                     int existing_fd, int channel, int reuseport) {
  Server sv;
  struct sigaction sa;
  const char *bind;
  const char *name;
  int port = 0;
  int rc = 0;
  /* The flag parser already caps 1..1024. A caller that fills the
     struct directly gets the same refusal, before any allocation. */
  if (!cfg || !handler || cfg->port < 0 || cfg->port > 65535 || cfg->idle_ms < 1 ||
      cfg->max_conn < 1 || cfg->max_conn > 1024 || cfg->max_body < 0 || cfg->drain_ms < 0)
    return -1;
  stop_flag = 0;
  draining_flag = 0;
  drain_deadline_ms = 0;
  stop_wake = -1;
  io_depth = 0;
  live_sv = NULL;
  memset(&sv, 0, sizeof sv);
  sv.listen_fd = -1;
  sv.wake_r = -1;
  sv.idle_ms = cfg->idle_ms;
  sv.max_conn = cfg->max_conn;
  sv.max_body = (uint64_t)cfg->max_body;
  sv.drain_ms = cfg->drain_ms;
  sv.channel = channel;
  sv.in_cap = env_in_cap();
  sv.write_limit = env_write_limit();
  sv.write_fail_after = env_write_fail_after();
  sv.stall_100 = env_stall_100();
  sv.write_log = getenv("MURO_TEST_WRITE_LOG");
  sv.handler = handler;
  sv.user = user;
  if (sv.write_log && !sv.write_log[0]) sv.write_log = NULL;
  name = cfg_name(cfg);
  snprintf(sv.name, sizeof sv.name, "%s", name);
  bind = (cfg->bind_host && cfg->bind_host[0]) ? cfg->bind_host : "127.0.0.1";
  if (strlen(bind) >= sizeof sv.bind_host) {
    fprintf(stderr, "%s: bad bind host\n", sv.name);
    return -2;
  }
  memcpy(sv.bind_host, bind, strlen(bind) + 1);
  {
    struct in_addr probe;
    if (inet_pton(AF_INET, sv.bind_host, &probe) != 1) {
      fprintf(stderr, "%s: bad bind host\n", sv.name);
      return -2;
    }
  }
  sv.slots = calloc((size_t)cfg->max_conn, sizeof(Slot));
  sv.pq = host_poll_open();
  if (sv.slots) {
    int i;
    for (i = 0; i < sv.max_conn; i++) sv.slots[i].fd = -1;
  }
  if (!sv.slots || !sv.pq) {
    fprintf(stderr, "%s: setup failed\n", sv.name);
    rc = -1;
    goto out;
  }
  fprintf(stderr, "%s: wait %s\n", sv.name, host_poll_backend(sv.pq));
  if (existing_fd >= 0) {
    sv.listen_fd = existing_fd;
  } else {
    sv.listen_fd = listen_on(sv.bind_host, cfg->port, reuseport);
    if (sv.listen_fd == -2) {
      fprintf(stderr, "%s: bad bind host\n", sv.name);
      rc = -2;
      goto out;
    }
    if (sv.listen_fd < 0) {
      fprintf(stderr, "%s: listen %s:%d: %s\n", sv.name, sv.bind_host, cfg->port, strerror(errno));
      rc = -1;
      goto out;
    }
  }
  if (!channel) {
    port = bound_port(sv.listen_fd);
    if (port < 0) {
      fprintf(stderr, "%s: getsockname: %s\n", sv.name, strerror(errno));
      rc = -1;
      goto out;
    }
  }
  {
    int sp[2] = {-1, -1};
    if (pipe(sp) < 0 || set_nb_cloexec(sp[0]) < 0 || set_nb_cloexec(sp[1]) < 0 ||
        host_poll_watch(sv.pq, sp[0], 1, 0) < 0) {
      fprintf(stderr, "%s: wake: %s\n", sv.name, strerror(errno));
      if (sp[0] >= 0) close(sp[0]);
      if (sp[1] >= 0) close(sp[1]);
      rc = -1;
      goto out;
    }
    sv.wake_r = sp[0];
    stop_wake = sp[1];
  }
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  {
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGINT);
    sigaddset(&unblock, SIGTERM);
    sigprocmask(SIG_UNBLOCK, &unblock, NULL);
  }
  if (sync_listen(&sv) < 0) {
    rc = -1;
    goto out;
  }
  if (!channel) {
    printf("listening %s %d\n", sv.bind_host, port);
    fflush(stdout);
  }
  live_sv = &sv;
  while (!sv.fatal) {
    HostEv evs[32];
    int n;
    int i;
    if (stop_flag) begin_drain(&sv);
    if (sv.draining) {
      close_idles(&sv);
      if (sv.nconn == 0 || host_poll_now_ms() >= sv.drain_deadline) break;
    }
    arm_soonest(&sv);
    n = host_poll_wait(sv.pq, evs, 32);
    if (n < 0) {
      fprintf(stderr, "%s: wait: %s\n", sv.name, strerror(errno));
      rc = -1;
      break;
    }
    if (sv.fatal) break;
    if (stop_flag) begin_drain(&sv);
    for (i = 0; i < n; i++) {
      Slot *s;
      if (sv.fatal) break;
      if (evs[i].timer) {
        uint64_t now = host_poll_now_ms();
        int k;
        sv.armed_deadline = 0;
        for (k = 0; k < sv.max_conn; k++) on_tick(&sv, &sv.slots[k], now);
        continue;
      }
      if (evs[i].fd == sv.wake_r) {
        drain_wake(sv.wake_r);
        continue;
      }
      if (sv.listen_fd >= 0 && evs[i].fd == sv.listen_fd) {
        accept_some(&sv);
        continue;
      }
      s = find_slot(&sv, evs[i].fd);
      if (!s) continue;
      if (evs[i].error && !evs[i].readable && !evs[i].writable) {
        close_slot(&sv, s);
        continue;
      }
      if (s->used && evs[i].writable && s->want_write) resume_write(&sv, s);
      if (s->used && evs[i].readable && s->want_read) on_readable(&sv, s, evs[i].hangup);
      else if (s->used && evs[i].hangup && s->want_read) on_readable(&sv, s, 1);
      else if (s->used && evs[i].hangup && !evs[i].writable) {
        close_slot(&sv, s);
      }
    }
    if (sv.draining) {
      close_idles(&sv);
      if (sv.nconn == 0 || host_poll_now_ms() >= sv.drain_deadline) break;
    }
    if (sv.fatal) {
      rc = -1;
      break;
    }
  }
  if (sv.fatal) rc = -1;
  live_sv = NULL;
out:
  {
    int wr = (int)stop_wake;
    stop_wake = -1;
    if (sv.wake_r >= 0) {
      int rd = sv.wake_r;
      sv.wake_r = -1;
      if (sv.pq) (void)host_poll_watch(sv.pq, rd, 0, 0);
      close(rd);
    }
    if (wr >= 0) close(wr);
  }
  if (sv.listen_fd >= 0) {
    int lfd = sv.listen_fd;
    sv.listen_fd = -1;
    if (sv.pq) (void)host_poll_watch(sv.pq, lfd, 0, 0);
    close(lfd);
  }
  if (sv.slots) {
    int i;
    for (i = 0; i < sv.max_conn; i++) {
      if (sv.slots[i].used) close_slot(&sv, &sv.slots[i]);
    }
  }
  host_poll_close(sv.pq);
  free(sv.slots);
  return rc;
}

static void signal_pids(pid_t *pids, int n, int sig) {
  int i;
  for (i = 0; i < n; i++) {
    if (pids[i] > 0) kill(pids[i], sig);
  }
}

/* Blocking waitpid misses SIGTERM that arrives after the flag
   check and before the wait. The pipe is written from the
   SIGTERM handler and from SIGCHLD, so the parent wakes for
   either. Without the pipe, fall back to the blocking wait. */
static int reap_children(pid_t *pids, int n) {
  int sp[2] = {-1, -1};
  struct sigaction sa;
  struct sigaction old_chld;
  int left = 0;
  int i;
  int failed = 0;
  int signaled = 0;
  int have_wake = 0;
  memset(&old_chld, 0, sizeof old_chld);
  for (i = 0; i < n; i++) {
    if (pids[i] > 0) left += 1;
  }
  if (pipe(sp) == 0 && set_nb_cloexec(sp[0]) == 0 && set_nb_cloexec(sp[1]) == 0) {
    parent_wake = sp[1];
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGCHLD, &sa, &old_chld) == 0) have_wake = 1;
  }
  if (!have_wake) {
    parent_wake = -1;
    if (sp[0] >= 0) close(sp[0]);
    if (sp[1] >= 0) close(sp[1]);
    sp[0] = -1;
    sp[1] = -1;
  }
  while (left > 0) {
    int status = 0;
    pid_t pid;
    if (parent_stop && !signaled) {
      signal_pids(pids, n, SIGTERM);
      signaled = 1;
    }
    pid = waitpid(-1, &status, have_wake ? WNOHANG : 0);
    if (pid < 0) {
      if (errno == EINTR) continue;
      failed = 1;
      break;
    }
    if (pid == 0) {
      struct pollfd pfd;
      pfd.fd = sp[0];
      pfd.events = POLLIN;
      if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
        failed = 1;
        break;
      }
      drain_wake(sp[0]);
      continue;
    }
    left -= 1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      failed = 1;
      if (!signaled) {
        signal_pids(pids, n, SIGTERM);
        signaled = 1;
      }
    }
  }
  parent_wake = -1;
  if (have_wake) (void)sigaction(SIGCHLD, &old_chld, NULL);
  if (sp[0] >= 0) close(sp[0]);
  if (sp[1] >= 0) close(sp[1]);
  return failed ? -1 : 0;
}

#if defined(__linux__)
static int serve_reuseport(const MuroHttpConfig *cfg, MuroHttpHandler handler, void *user) {
  pid_t pids[64];
  int n = cfg->workers;
  int i;
  sigset_t block;
  sigset_t old;
  struct sigaction sa;
  memset(pids, 0, sizeof pids);
  parent_stop = 0;
  sigemptyset(&block);
  sigaddset(&block, SIGINT);
  sigaddset(&block, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &block, &old) < 0) return -1;
  fflush(stdout);
  fflush(stderr);
  for (i = 0; i < n; i++) {
    pid_t pid = fork();
    if (pid < 0) {
      fprintf(stderr, "%s: fork: %s\n", cfg_name(cfg), strerror(errno));
      signal_pids(pids, i, SIGTERM);
      parent_stop = 1;
      sigprocmask(SIG_SETMASK, &old, NULL);
      reap_children(pids, i);
      return -1;
    }
    if (pid == 0) {
      int rc = serve_one(cfg, handler, user, -1, 0, 1);
      _exit(rc == 0 ? 0 : 1);
    }
    pids[i] = pid;
  }
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = parent_on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  sigprocmask(SIG_SETMASK, &old, NULL);
  fprintf(stderr, "%s: workers %d reuseport\n", cfg_name(cfg), n);
  return reap_children(pids, n);
}
#else
static int send_fd(int sock, int fd) {
  struct msghdr msg;
  struct iovec iov;
  char dummy = 'c';
  char cbuf[CMSG_SPACE(sizeof(int))];
  struct cmsghdr *cmsg;
  memset(&msg, 0, sizeof msg);
  memset(cbuf, 0, sizeof cbuf);
  iov.iov_base = &dummy;
  iov.iov_len = 1;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof cbuf;
  cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg) return -1;
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &fd, sizeof fd);
  for (;;) {
    if (sendmsg(sock, &msg, 0) < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    return 0;
  }
}

/* MURO_HTTP_TEST_HANDOFF_SNDBUF is a test seam. A small send
   buffer makes the handoff channel return EAGAIN after a few
   queued fds. Unset in production, where the socket keeps the
   system default. Values below 256 are ignored: one SCM_RIGHTS
   message has to fit or the parent would block forever. */
static void test_handoff_sndbuf(int fd) {
  const char *e = getenv("MURO_HTTP_TEST_HANDOFF_SNDBUF");
  char *end = NULL;
  long n;
  int sz;
  if (!e || !*e) return;
  errno = 0;
  n = strtol(e, &end, 10);
  if (errno || !end || *end || n < 256 || n > 1024L * 1024L) return;
  sz = (int)n;
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
}

/* In-flight SCM_RIGHTS the parent has not seen acked yet. The
   parent's copy stays open until the ack: closing it while the
   only other reference sits in the socketpair makes the client
   see EOF on this kernel. */
#define HANDOFF_FLIGHT 32

static void note_drop(const char *name, unsigned long *drops, const char *why) {
  *drops += 1;
  fprintf(stderr, "%s: handoff drop %lu: %s\n", name, *drops, why);
}

static void drop_inflight(const char *name, int *q, int nq, unsigned long *drops, const char *why) {
  int i;
  for (i = 0; i < nq; i++) {
    if (q[i] < 0) continue;
    note_drop(name, drops, why);
    close(q[i]);
    q[i] = -1;
  }
}

/* 1 if the caller should read again, 0 on EAGAIN, -1 if this
   channel is closed and its in-flight fds were dropped. */
static int drain_acks(const char *name, int w, int *parent_end, int flight[][HANDOFF_FLIGHT],
                      int *nflight, unsigned long *drops) {
  char buf[64];
  ssize_t n;
  int i;
  int ch = parent_end[w];
  int nq;
  if (ch < 0) return 0;
  nq = nflight[w];
  n = recv(ch, buf, sizeof buf, 0);
  if (n < 0) {
    if (errno == EINTR) return 1;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    fprintf(stderr, "%s: handoff worker %d: %s\n", name, w, strerror(errno));
    drop_inflight(name, flight[w], nq, drops, "channel");
    nflight[w] = 0;
    close(ch);
    parent_end[w] = -1;
    return -1;
  }
  if (n == 0 || n > nq) {
    if (n > nq) fprintf(stderr, "%s: handoff worker %d: ack\n", name, w);
    drop_inflight(name, flight[w], nq, drops, "channel");
    nflight[w] = 0;
    close(ch);
    parent_end[w] = -1;
    return -1;
  }
  for (i = 0; i < (int)n; i++) {
    close(flight[w][i]);
    flight[w][i] = -1;
  }
  nflight[w] = nq - (int)n;
  if (nflight[w] > 0)
    memmove(flight[w], flight[w] + n, (size_t)nflight[w] * sizeof flight[w][0]);
  return 1;
}

static void drain_all(const char *name, int *parent_end, int n, int flight[][HANDOFF_FLIGHT],
                      int *nflight, unsigned long *drops) {
  int w;
  for (w = 0; w < n; w++) {
    int rc;
    do {
      rc = drain_acks(name, w, parent_end, flight, nflight, drops);
    } while (rc > 0);
  }
}

/* Reap exited workers without blocking. A signalled or non-zero
   exit stops the pool the way the Linux reuseport parent does.
   A clean exit only removes that worker. 0 if the accept loop
   can continue, -1 if it must stop. */
static int reap_handoff(const char *name, pid_t *pids, int *parent_end, int n,
                        int flight[][HANDOFF_FLIGHT], int *nflight, int *failed,
                        unsigned long *drops) {
  for (;;) {
    int status = 0;
    int idx;
    int bad;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid < 0) {
      if (errno == EINTR) continue;
      if (errno == ECHILD) return 0;
      fprintf(stderr, "%s: wait: %s\n", name, strerror(errno));
      *failed = 1;
      parent_stop = 1;
      return -1;
    }
    if (pid == 0) return 0;
    idx = -1;
    {
      int i;
      for (i = 0; i < n; i++) {
        if (pids[i] == pid) {
          idx = i;
          break;
        }
      }
    }
    if (idx >= 0) {
      int rc;
      pids[idx] = 0;
      do {
        rc = drain_acks(name, idx, parent_end, flight, nflight, drops);
      } while (rc > 0);
      if (parent_end[idx] >= 0) {
        drop_inflight(name, flight[idx], nflight[idx], drops, "worker exit");
        nflight[idx] = 0;
        close(parent_end[idx]);
        parent_end[idx] = -1;
      }
    }
    bad = !WIFEXITED(status) || WEXITSTATUS(status) != 0;
    if (!bad) continue;
    if (WIFSIGNALED(status)) {
      fprintf(stderr, "%s: worker %ld signal %d\n", name, (long)pid, WTERMSIG(status));
    } else {
      fprintf(stderr, "%s: worker %ld exit %d\n", name, (long)pid,
              WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    }
    *failed = 1;
    parent_stop = 1;
    signal_pids(pids, n, SIGTERM);
    return -1;
  }
}

/* Hand c to a worker. On success the worker's flight queue owns c
   and this does not close it. -1 drops c. Busy workers are not a
   drop: this blocks until a channel has room, rescanning every
   worker after each wait. */
static int pass_conn(const char *name, int *parent_end, pid_t *pids, int n, int *rr, int c,
                     int flight[][HANDOFF_FLIGHT], int *nflight, int *failed,
                     unsigned long *drops) {
  for (;;) {
    int tries;
    int alive;
    struct pollfd pfds[64];
    int np;
    int pr;
    if (reap_handoff(name, pids, parent_end, n, flight, nflight, failed, drops) < 0 ||
        parent_stop) {
      note_drop(name, drops, "shutting down");
      close(c);
      return -1;
    }
    drain_all(name, parent_end, n, flight, nflight, drops);
    for (tries = 0; tries < n; tries++) {
      int w = (*rr + tries) % n;
      if (parent_end[w] < 0) continue;
      if (nflight[w] >= HANDOFF_FLIGHT) continue;
      if (send_fd(parent_end[w], c) == 0) {
        flight[w][nflight[w]] = c;
        nflight[w] += 1;
        *rr = (w + 1) % n;
        return 0;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
      fprintf(stderr, "%s: handoff worker %d: %s\n", name, w, strerror(errno));
      drop_inflight(name, flight[w], nflight[w], drops, "channel");
      nflight[w] = 0;
      close(parent_end[w]);
      parent_end[w] = -1;
    }
    np = 0;
    alive = 0;
    for (tries = 0; tries < n; tries++) {
      short ev = 0;
      if (parent_end[tries] < 0) continue;
      alive = 1;
      if (nflight[tries] < HANDOFF_FLIGHT) ev = (short)(ev | POLLOUT);
      if (nflight[tries] > 0) ev = (short)(ev | POLLIN);
      if (!ev) continue;
      pfds[np].fd = parent_end[tries];
      pfds[np].events = ev;
      pfds[np].revents = 0;
      np += 1;
    }
    if (!alive || np == 0) {
      note_drop(name, drops, "no workers");
      close(c);
      *failed = 1;
      parent_stop = 1;
      return -1;
    }
    pr = poll(pfds, (nfds_t)np, -1);
    if (pr < 0) {
      if (errno == EINTR) continue;
      note_drop(name, drops, "poll");
      close(c);
      *failed = 1;
      parent_stop = 1;
      return -1;
    }
  }
}

static int serve_handoff(const MuroHttpConfig *cfg, MuroHttpHandler handler, void *user) {
  pid_t pids[64];
  int parent_end[64];
  int flight[64][HANDOFF_FLIGHT];
  int nflight[64];
  int n = cfg->workers;
  int lfd = -1;
  int i;
  int rr = 0;
  int failed = 0;
  unsigned long drops = 0;
  int port;
  sigset_t block;
  sigset_t old;
  struct sigaction sa;
  const char *name = cfg_name(cfg);
  const char *bind = (cfg->bind_host && cfg->bind_host[0]) ? cfg->bind_host : "127.0.0.1";
  for (i = 0; i < 64; i++) {
    int j;
    pids[i] = 0;
    parent_end[i] = -1;
    nflight[i] = 0;
    for (j = 0; j < HANDOFF_FLIGHT; j++) flight[i][j] = -1;
  }
  lfd = listen_on(bind, cfg->port, 0);
  if (lfd == -2) {
    fprintf(stderr, "%s: bad bind host\n", name);
    return -2;
  }
  if (lfd < 0) {
    fprintf(stderr, "%s: listen %s:%d: %s\n", name, bind, cfg->port, strerror(errno));
    return -1;
  }
  parent_stop = 0;
  sigemptyset(&block);
  sigaddset(&block, SIGINT);
  sigaddset(&block, SIGTERM);
  if (sigprocmask(SIG_BLOCK, &block, &old) < 0) {
    close(lfd);
    return -1;
  }
  fflush(stdout);
  fflush(stderr);
  for (i = 0; i < n; i++) {
    int sp[2] = {-1, -1};
    pid_t pid;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) < 0) {
      fprintf(stderr, "%s: socketpair: %s\n", name, strerror(errno));
      failed = 1;
      break;
    }
    if (set_nb_cloexec(sp[0]) < 0 || set_nb_cloexec(sp[1]) < 0) {
      fprintf(stderr, "%s: socketpair: %s\n", name, strerror(errno));
      close(sp[0]);
      close(sp[1]);
      failed = 1;
      break;
    }
    pid = fork();
    if (pid < 0) {
      fprintf(stderr, "%s: fork: %s\n", name, strerror(errno));
      close(sp[0]);
      close(sp[1]);
      failed = 1;
      break;
    }
    if (pid == 0) {
      int j;
      int rc;
      close(lfd);
      close(sp[0]);
      for (j = 0; j < i; j++) {
        if (parent_end[j] >= 0) close(parent_end[j]);
      }
      rc = serve_one(cfg, handler, user, sp[1], 1, 0);
      _exit(rc == 0 ? 0 : 1);
    }
    close(sp[1]);
    test_handoff_sndbuf(sp[0]);
    parent_end[i] = sp[0];
    pids[i] = pid;
  }
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = parent_on_signal;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  sigprocmask(SIG_SETMASK, &old, NULL);
  if (failed) {
    parent_stop = 1;
    signal_pids(pids, n, SIGTERM);
    for (i = 0; i < n; i++) {
      if (parent_end[i] >= 0) close(parent_end[i]);
    }
    close(lfd);
    reap_children(pids, n);
    return -1;
  }
  port = bound_port(lfd);
  if (port < 0) {
    fprintf(stderr, "%s: getsockname: %s\n", name, strerror(errno));
    parent_stop = 1;
    signal_pids(pids, n, SIGTERM);
    for (i = 0; i < n; i++) {
      if (parent_end[i] >= 0) close(parent_end[i]);
    }
    close(lfd);
    reap_children(pids, n);
    return -1;
  }
  fprintf(stderr, "%s: workers %d handoff\n", name, n);
  printf("listening %s %d\n", bind, port);
  fflush(stdout);
  while (!parent_stop) {
    struct pollfd pfd;
    int pr;
    if (reap_handoff(name, pids, parent_end, n, flight, nflight, &failed, &drops) < 0) break;
    drain_all(name, parent_end, n, flight, nflight, &drops);
    pfd.fd = lfd;
    pfd.events = POLLIN;
    pr = poll(&pfd, 1, 200);
    if (pr < 0) {
      if (errno == EINTR) continue;
      failed = 1;
      break;
    }
    if (pr == 0) continue;
    while (!parent_stop) {
      struct sockaddr_in peer;
      socklen_t len = sizeof peer;
      int c;
      if (reap_handoff(name, pids, parent_end, n, flight, nflight, &failed, &drops) < 0) break;
      c = take_conn(lfd, &peer, &len);
      if (c < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        fprintf(stderr, "%s: accept: %s\n", name, strerror(errno));
        failed = 1;
        parent_stop = 1;
        break;
      }
      if (pass_conn(name, parent_end, pids, n, &rr, c, flight, nflight, &failed, &drops) < 0)
        break;
    }
  }
  parent_stop = 1;
  drain_all(name, parent_end, n, flight, nflight, &drops);
  signal_pids(pids, n, SIGTERM);
  for (i = 0; i < n; i++) {
    drop_inflight(name, flight[i], nflight[i], &drops, "shutting down");
    nflight[i] = 0;
    if (parent_end[i] >= 0) close(parent_end[i]);
  }
  close(lfd);
  if (reap_children(pids, n) < 0) failed = 1;
  fprintf(stderr, "%s: handoff drops %lu\n", name, drops);
  return failed ? -1 : 0;
}
#endif

int muro_http_serve(const MuroHttpConfig *cfg, MuroHttpHandler handler, void *user) {
  int workers;
  const char *bind;
  struct in_addr probe;
  if (!cfg || !handler || cfg->port < 0 || cfg->port > 65535 || cfg->idle_ms < 1 ||
      cfg->max_conn < 1 || cfg->max_conn > 1024 || cfg->max_body < 0 || cfg->drain_ms < 0)
    return -1;
  workers = cfg->workers;
  if (workers < 1 || workers > 64) {
    fprintf(stderr, "%s: bad workers\n", cfg_name(cfg));
    return -1;
  }
  if (workers == 1) return serve_one(cfg, handler, user, -1, 0, 0);
  if (cfg->port == 0) {
    fprintf(stderr, "%s: workers need a fixed port\n", cfg_name(cfg));
    return -1;
  }
  bind = (cfg->bind_host && cfg->bind_host[0]) ? cfg->bind_host : "127.0.0.1";
  if (inet_pton(AF_INET, bind, &probe) != 1) {
    fprintf(stderr, "%s: bad bind host\n", cfg_name(cfg));
    return -2;
  }
#if defined(__linux__)
  return serve_reuseport(cfg, handler, user);
#else
  return serve_handoff(cfg, handler, user);
#endif
}
