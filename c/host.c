/* One connection is one image and one session.
   The image is conn.step. The moves are proto_admit, which applies
   the emitted admitChoice. This file does not choose a move. It
   feeds bytes, peer eof, write progress, write done, write error,
   an idle tick, and the handler's reply. When the image asks for
   the handler, this file builds a request view, calls that callback,
   and copies the book's framing of the handler's status, headers,
   and body.

   Origin 1 is a malformed request. The bytes are httpMalformed.
   Origin 2 is a transfer coding the book does not implement. The
   bytes are httpNotImplemented. A committed handler that fails
   before a status byte leaves is httpInternal. This file also writes
   100 Continue, 413, and 417 when host_in_policy says so, and 501
   for method code 1. host_in_policy does not answer an unknown
   transfer coding. The image still chooses 400 and that 501.

   The response copy is c/host_render.c, which includes render.h.
   conn.h and render.h both put the prelude under MURO__PRELUDE, so
   one translation unit can include both. This file calls
   host_copy_response and does not include render.h. */

#include "shim.h"
#include "conn.h"
#include "proto.h"
#include "muro_http.h"
#include "host_poll.h"
#include "host_render.h"
#include "host_date.h"
#include "host_stream.h"

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
  PH_READ = 0,
  PH_DECIDE = 1,
  PH_WRITE = 2,
  PH_IDLE = 3,
  PH_CLOSED = 4,
  HOST_ACT_NONE = 0,
  HOST_ACT_READ = 1,
  HOST_ACT_WRITE = 2,
  HOST_ACT_CLOSE = 3,
  HOST_ACT_HANDLER = 4,
  EV_BYTES = 0,
  EV_EOF = 1,
  EV_REPLY = 6,
  IN_CAP = 65536,
  OUT_CAP = 69632,
  PUMP_FUSE = 100000
};

static volatile sig_atomic_t stop_flag;
static volatile sig_atomic_t draining_flag;
static volatile sig_atomic_t parent_stop;
static uint64_t drain_deadline_ms;

int host_io_should_stop(void) {
  if (!draining_flag) return 0;
  return host_poll_now_ms() >= drain_deadline_ms;
}

static void on_signal(int sig) {
  (void)sig;
  stop_flag = 1;
}

static void parent_on_signal(int sig) {
  (void)sig;
  parent_stop = 1;
}

static const char *cfg_name(const MuroHttpConfig *cfg) {
  if (cfg && cfg->name && cfg->name[0]) return cfg->name;
  return "muro-http";
}

static uint64_t field(Conn st, int i) {
  switch (i) {
  case 0: return st.muro__as.conn.muro__f_0;
  case 1: return st.muro__as.conn.muro__f_1;
  case 2: return st.muro__as.conn.muro__f_2;
  case 3: return st.muro__as.conn.muro__f_3;
  case 4: return st.muro__as.conn.muro__f_4;
  case 5: return st.muro__as.conn.muro__f_5;
  case 6: return st.muro__as.conn.muro__f_6;
  case 7: return st.muro__as.conn.muro__f_7;
  case 8: return st.muro__as.conn.muro__f_8;
  case 9: return st.muro__as.conn.muro__f_9;
  case 10: return st.muro__as.conn.muro__f_10;
  case 11: return st.muro__as.conn.muro__f_11;
  case 12: return st.muro__as.conn.muro__f_12;
  default: return 0;
  }
}

typedef struct Slot {
  int fd;
  int used;
  int want_read;
  int want_write;
  int rendered;
  int owe;
  Proto *proto;
  Conn st;
  uint8_t *in;
  size_t in_len;
  uint8_t *out;
  size_t out_cap;
  size_t out_len;
  size_t out_off;
  uint64_t deadline;
  int body_idle;
  struct HostIn hin;
  int sent_100;
  uint8_t cont[160];
  size_t cont_len;
  size_t cont_off;
  int is_head;
  int ver11;
  int resp_mode;
  int pull_until_end;
  int pull_done;
  MuroHttpPull pull;
  void *pull_user;
  uint64_t pull_left;
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
  if (host_poll_watch(sv->pq, sv->listen_fd, on, 0) < 0) {
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
  status_before_close(sv, s);
  host_poll_watch(sv->pq, s->fd, 0, 0);
  close(s->fd);
  proto_free(s->proto);
  host_in_close(&s->hin);
  free(s->in);
  free(s->out);
  memset(s, 0, sizeof *s);
  s->fd = -1;
  if (sv->nconn > 0) sv->nconn -= 1;
  (void)sync_listen(sv);
}

static int step_ev(Server *sv, Slot *s, Ev ev) {
  int phase = (int)field(s->st, 0);
  int half = (int)field(s->st, 1);
  int keep = (int)field(s->st, 2);
  int tag = (int)ev.muro__as.ev.muro__f_0;
  muro__slice buf = ev.muro__as.ev.muro__f_1;
  int status = -1;
  int reply = -1;
  int observe = 0;
  int admitted;

  if (!s->proto) return 0;
  if (proto_live(s->proto) && !proto_done(s->proto) && (phase == PH_READ || phase == PH_IDLE) &&
      (tag == EV_BYTES || (tag == EV_EOF && buf.n > 0))) {
    Classified *c = classify(buf);
    status = c->muro__as.classified.muro__f_0->muro__field_tag;
  }
  /* n1 is 1 for an HTTP status 400..599. That is reply-bad and
     closes the connection. Any other handler status is reply-ok. */
  if (proto_live(s->proto) && !proto_done(s->proto) && phase == PH_DECIDE && tag == EV_REPLY) {
    uint64_t bad_reply = ev.muro__as.ev.muro__f_3;
    reply = bad_reply == 0 ? 0 : 1;
    observe = 0;
  }
  admitted = proto_admit(s->proto, phase, tag, buf.n, status, reply, observe, half || keep);
  if (admitted != 1) {
    muro_http_reset();
    fprintf(stderr, "%s: admit refused phase %d ev %d\n", sv->name, phase, tag);
    return 0;
  }
  s->st = step(s->st, ev);
  muro_http_reset();
  return 1;
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
   rendered buffer is that status. Otherwise the book supplies 500. */
static void load_date(char date[30]) { host_date_fill(date); }

static void status_before_close(Server *sv, Slot *s) {
  uint8_t tmp[512];
  char date[30];
  size_t n = 0;
  const uint8_t *msg;
  if (!s->owe || s->fd < 0 || s->out_off > 0) return;
  if (s->rendered && s->out && s->out_len > 0) {
    msg = s->out;
    n = s->out_len;
  } else {
    load_date(date);
    n = host_copy_literal(tmp, sizeof tmp, 1, (const uint8_t *)date, 29, s->is_head ? 1u : 0u, NULL);
    msg = tmp;
    fprintf(stderr, "%s: committed write has no status; sending 500\n", sv->name);
  }
  if (n > 0) write_status(sv, s->fd, msg, n);
  s->owe = 0;
}

static int span_is(MuroHttpSlice s, const char *lit) {
  size_t n = strlen(lit);
  if (s.n != n) return 0;
  if (n == 0) return 1;
  return memcmp(s.p, lit, n) == 0;
}

static void note_version(Slot *s, const MuroHttpRequest *req) {
  s->is_head = span_is(req->method, "HEAD");
  s->ver11 = span_is(req->version, "HTTP/1.1");
}

static int fill_bound(Slot *s, size_t *n) {
  uint64_t end = field(s->st, 11);
  if (end == 0 || end > (uint64_t)s->in_len || !s->in) return -1;
  *n = (size_t)end;
  return 0;
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

/* 1 when HTTP/1.1 100 Continue has been fully written. 0 when some
   bytes remain and the slot wants a write. -1 on a hard write error
   or a literal that does not fit. One successful write per call, so
   a short write or EAGAIN returns to poll. sent_100 is set only
   after the last byte. The date is rendered once into s->cont. */
static int write_100(Server *sv, Slot *s) {
  int spins = 0;
  if (s->sent_100) return 1;
  if (s->cont_len == 0) {
    char date[30];
    size_t nlen;
    load_date(date);
    nlen = host_copy_literal(s->cont, sizeof s->cont, 5, (const uint8_t *)date, 29, 0, NULL);
    if (nlen == 0 || nlen > sizeof s->cont) return -1;
    s->cont_len = nlen;
    s->cont_off = 0;
  }
  if (s->cont_off >= s->cont_len) {
    s->sent_100 = 1;
    s->cont_len = 0;
    s->cont_off = 0;
    return 1;
  }
  if (sv->stall_100 > 0 && s->cont_off == 0) {
    struct timespec ts;
    note_100_ready();
    /* No interim byte yet. Wait until the peer has queued more
       bytes, so a body and a pipelined request are in the kernel
       during the short write. A pause that ignores the socket loses
       the race on a slow accept and the first response is 200. */
    if (!sock_pending(s->fd)) {
      sv->stall_100 -= 1;
      s->want_write = 1;
      ts.tv_sec = 0;
      ts.tv_nsec = 20000000L;
      while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        continue;
      return 0;
    }
    sv->stall_100 = 0;
  }
  while (spins < 8) {
    size_t chunk = s->cont_len - s->cont_off;
    ssize_t n;
    if (sv->write_limit > 0 && chunk > sv->write_limit) chunk = sv->write_limit;
    n = write(s->fd, s->cont + s->cont_off, chunk);
    if (n < 0 && errno == EINTR) {
      spins += 1;
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      s->want_write = 1;
      return 0;
    }
    if (n <= 0) return -1;
    s->cont_off += (size_t)n;
    note_write(sv, (size_t)n);
    break;
  }
  if (s->cont_off < s->cont_len) {
    s->want_write = 1;
    return 0;
  }
  s->sent_100 = 1;
  s->cont_len = 0;
  s->cont_off = 0;
  return 1;
}

/* 1 when 100 Continue is fully written. 0 when bytes remain: watch
   POLLOUT and not POLLIN. A read during a short interim steps leftover
   bytes into HANDLE, and onHandle holds them until the client writes
   again. -1 on a hard write error. A READ-phase timer still arms the
   idle deadline, so a peer that never takes the interim still times
   out. HANDLE has the book timer off, so that deadline clears.
   A stalled muro_http_body_read arms its own deadline and closes
   after the idle timeout. That close is not an image tick. */
static int hold_100(Server *sv, Slot *s) {
  int w100 = write_100(sv, s);
  int timer;
  if (w100 != 0) return w100;
  s->want_read = 0;
  s->want_write = 1;
  timer = (int)field(s->st, 5);
  if (timer) {
    if (s->deadline == 0) s->deadline = host_poll_now_ms() + (uint64_t)sv->idle_ms;
  } else {
    s->deadline = 0;
  }
  return 0;
}

static int copy_fixed(Slot *s, int which) {
  char date[30];
  size_t got = 0;
  size_t n;
  load_date(date);
  n = host_copy_literal(s->out, s->out_cap, which, (const uint8_t *)date, 29, s->is_head ? 1u : 0u, &got);
  if (n == 0) return -1;
  s->out_len = n;
  s->out_off = 0;
  s->rendered = 1;
  s->resp_mode = 0;
  s->pull = NULL;
  return 0;
}

static int begin_reply(Server *sv, Slot *s, const MuroHttpResponse *resp) {
  int half = (int)field(s->st, 1);
  int keep = (int)field(s->st, 2);
  int bad = (resp->status >= 400 && resp->status <= 599) ? 1 : 0;
  int bit = (resp->close_after || bad) ? 1 : 0;
  uint64_t eff;
  size_t got = 0;
  size_t n = 0;
  char date[30];
  const uint8_t *reason = resp->reason ? (const uint8_t *)resp->reason : NULL;
  const uint8_t *extra = resp->headers ? (const uint8_t *)resp->headers : NULL;
  const uint8_t *body = resp->body;
  size_t reason_n = reason ? resp->reason_len : 0;
  size_t extra_n = extra ? resp->headers_len : 0;
  size_t body_n = body ? resp->body_len : 0;
  int omit = s->is_head ? 1 : 0;
  load_date(date);
  s->pull = NULL;
  s->pull_user = NULL;
  s->pull_left = 0;
  s->pull_done = 0;
  s->pull_until_end = 0;
  s->resp_mode = 0;
  if (resp->pull && resp->has_length) {
    if (omit) {
      n = host_copy_head(s->out, s->out_cap, (uint64_t)resp->status, reason, reason_n,
                         (const uint8_t *)date, 29, extra, extra_n, resp->content_length, 0, &got);
    } else {
      n = host_copy_head(s->out, s->out_cap, (uint64_t)resp->status, reason, reason_n,
                         (const uint8_t *)date, 29, extra, extra_n, resp->content_length, 0, &got);
      s->resp_mode = 1;
      s->pull = resp->pull;
      s->pull_user = resp->pull_user;
      s->pull_left = resp->content_length;
    }
  } else if (resp->pull) {
    bit = omit || !s->ver11 ? 1 : bit;
    if (omit || !s->ver11) {
      n = host_copy_delim(s->out, s->out_cap, (uint64_t)resp->status, reason, reason_n,
                          (const uint8_t *)date, 29, extra, extra_n, &got);
      if (!omit) {
        s->resp_mode = 1;
        s->pull = resp->pull;
        s->pull_user = resp->pull_user;
        s->pull_until_end = 1;
      }
    } else {
      n = host_copy_chunked(s->out, s->out_cap, (uint64_t)resp->status, reason, reason_n,
                            (const uint8_t *)date, 29, extra, extra_n, 0, &got);
      s->resp_mode = 2;
      s->pull = resp->pull;
      s->pull_user = resp->pull_user;
    }
  } else {
    n = host_copy_response(s->out, s->out_cap, (uint64_t)resp->status, reason, reason_n,
                           (const uint8_t *)date, 29, extra, extra_n, body, body_n, 0, (uint64_t)omit,
                           &got);
  }
  eff = (half || keep || bit) ? 1u : 0u;
  /* The book already ran with keep 0. A forced close has to be the
     value the book saw, so rebuild when eff is 1 and the first copy
     assumed keep-alive. */
  if (eff == 1 && n > 0 && !(resp->pull && !resp->has_length && (omit || !s->ver11))) {
    if (resp->pull && resp->has_length) {
      n = host_copy_head(s->out, s->out_cap, (uint64_t)resp->status, reason, reason_n,
                         (const uint8_t *)date, 29, extra, extra_n, resp->content_length, 1, &got);
    } else if (resp->pull && s->ver11 && !omit) {
      n = host_copy_chunked(s->out, s->out_cap, (uint64_t)resp->status, reason, reason_n,
                            (const uint8_t *)date, 29, extra, extra_n, 1, &got);
    } else if (!resp->pull) {
      n = host_copy_response(s->out, s->out_cap, (uint64_t)resp->status, reason, reason_n,
                             (const uint8_t *)date, 29, extra, extra_n, body, body_n, 1, (uint64_t)omit,
                             &got);
    }
  }
  if (n == 0 && resp->owe_status) {
    n = host_copy_literal(s->out, s->out_cap, 1, (const uint8_t *)date, 29, (uint64_t)omit, NULL);
    bit = 1;
    bad = 1;
    eff = 1;
    s->resp_mode = 0;
    s->pull = NULL;
    if (n > 0) fprintf(stderr, "%s: response is %zu bytes after commit; sending 500\n", sv->name, got);
  }
  if (n == 0) {
    fprintf(stderr, "%s: response is %zu bytes\n", sv->name, got);
    return -1;
  }
  s->out_len = n;
  s->out_off = 0;
  s->rendered = 1;
  s->owe = resp->owe_status ? 1 : 0;
  if (!step_ev(sv, s, evReplyOf((uint64_t)bit, (uint64_t)bad))) return -1;
  return 0;
}

static int fill_pull(Slot *s) {
  uint8_t tmp[8192];
  int r;
  size_t cap;
  if (!s->pull || s->resp_mode == 0) return 0;
  if (s->resp_mode == 2) {
    r = s->pull(tmp, sizeof tmp, s->pull_user);
    if (r < 0) return -1;
    if (r == 0) {
      s->out_len = host_copy_last(s->out, s->out_cap, NULL);
      s->out_off = 0;
      s->pull_done = 1;
      s->resp_mode = 0;
      s->pull = NULL;
      return s->out_len > 0 ? 0 : -1;
    }
    s->out_len = host_copy_chunk(s->out, s->out_cap, tmp, (size_t)r, NULL);
    s->out_off = 0;
    return s->out_len > 0 ? 0 : -1;
  }
  if (!s->pull_until_end && s->pull_left == 0) {
    s->pull_done = 1;
    s->resp_mode = 0;
    s->pull = NULL;
    s->out_len = 0;
    s->out_off = 0;
    return 0;
  }
  cap = s->out_cap < sizeof tmp ? s->out_cap : sizeof tmp;
  if (!s->pull_until_end && s->pull_left < cap) cap = (size_t)s->pull_left;
  if (cap == 0) return -1;
  r = s->pull(s->out, cap, s->pull_user);
  if (r < 0) return -1;
  if (r == 0) {
    if (!s->pull_until_end && s->pull_left != 0) return -1;
    s->pull_done = 1;
    s->resp_mode = 0;
    s->pull = NULL;
    s->out_len = 0;
    s->out_off = 0;
    return 0;
  }
  if (!s->pull_until_end && (uint64_t)r > s->pull_left) return -1;
  if (!s->pull_until_end) s->pull_left -= (uint64_t)r;
  s->out_len = (size_t)r;
  s->out_off = 0;
  return 0;
}

static void consume(Slot *s, uint64_t end_off) {
  size_t n;
  if (end_off == 0 || end_off > (uint64_t)s->in_len) return;
  n = (size_t)end_off;
  memmove(s->in, s->in + n, s->in_len - n);
  s->in_len -= n;
}

static int which_of(int pol) {
  if (pol == 413) return 3;
  if (pol == 417) return 4;
  if (pol == 501) return 2;
  return 0;
}

static void note_head_buf(Slot *s, const uint8_t *buf, size_t n) {
  muro__slice sl;
  muro__slice m;
  muro__slice v;
  if (!buf || n == 0) return;
  sl.p = buf;
  sl.n = n;
  m = methodView(sl);
  v = versionView(sl);
  s->is_head = m.n == 4 && m.p && memcmp(m.p, "HEAD", 4) == 0;
  s->ver11 = v.n == 8 && v.p && memcmp(v.p, "HTTP/1.1", 8) == 0;
  muro_http_reset();
}

static void prepare_in(Slot *s, int fresh) {
  int phase = (int)field(s->st, 0);
  if (phase != PH_READ && phase != PH_IDLE) return;
  host_in_reset(&s->hin);
  if (fresh) {
    s->sent_100 = 0;
    s->cont_len = 0;
    s->cont_off = 0;
  }
  if (s->in && s->in_len) host_in_capture(&s->hin, s->in, s->in_len);
}

static int step_buf(Server *sv, Slot *s) {
  if (!s->in) return 0;
  return step_ev(sv, s, evBytes(muro__slice_of(s->in, s->in_len)));
}

static void send_and_close(Server *sv, Slot *s, int which) {
  uint8_t tmp[512];
  char date[30];
  size_t n;
  load_date(date);
  n = host_copy_literal(tmp, sizeof tmp, which, (const uint8_t *)date, 29, s->is_head ? 1u : 0u, NULL);
  if (n > 0) write_status(sv, s->fd, tmp, n);
  s->owe = 0;
  s->out_off = 1;
  close_slot(sv, s);
}

static int reply_fixed(Server *sv, Slot *s, int which) {
  if (copy_fixed(s, which) < 0) return -1;
  if (!step_ev(sv, s, evReplyOf(1, 1))) return -1;
  return 0;
}

static int inject_next(Server *sv, Slot *s) {
  uint8_t *q = NULL;
  size_t qn = 0;
  if (!host_in_take_next(&s->hin, &q, &qn)) return 0;
  if (qn > sv->in_cap || !s->in) {
    free(q);
    return -1;
  }
  memcpy(s->in, q, qn);
  s->in_len = qn;
  free(q);
  prepare_in(s, 1);
  return step_buf(sv, s) ? 1 : -1;
}

/* Returns 1 to keep pumping, 0 when the slot is waiting or gone. */
static int pump(Server *sv, Slot *s) {
  int spins = 0;
  while (s->used && spins < PUMP_FUSE) {
    int phase = (int)field(s->st, 0);
    int action = (int)field(s->st, 4);
    int timer = (int)field(s->st, 5);
    uint64_t end_off = field(s->st, 11);
    uint64_t more = field(s->st, 12);
    spins += 1;
    if (s->cont_len > 0 && !s->sent_100) {
      int w100 = hold_100(sv, s);
      if (w100 < 0) {
        close_slot(sv, s);
        return 0;
      }
      if (w100 == 0) return 0;
    }
    if (phase == PH_CLOSED || action == HOST_ACT_CLOSE) {
      close_slot(sv, s);
      return 0;
    }
    if (phase == PH_READ || phase == PH_IDLE) {
      s->rendered = 0;
      s->owe = 0;
      s->out_len = 0;
      s->out_off = 0;
      s->resp_mode = 0;
      s->pull = NULL;
    }
    if (action == HOST_ACT_HANDLER) {
      MuroHttpRequest req;
      MuroHttpResponse resp;
      size_t span = 0;
      int rc;
      int send100 = 0;
      int pol;
      int dec;
      int force = 0;
      int over = 0;
      if (fill_bound(s, &span) < 0 || !sv->handler) {
        close_slot(sv, s);
        return 0;
      }
      note_head_buf(s, s->in, span);
      pol = host_in_policy(s->in, span, sv->max_body, &send100);
      if (pol) {
        if (reply_fixed(sv, s, which_of(pol)) < 0) {
          close_slot(sv, s);
          return 0;
        }
        continue;
      }
      if (send100 && !s->sent_100) {
        int w100 = hold_100(sv, s);
        if (w100 < 0) {
          close_slot(sv, s);
          return 0;
        }
        if (w100 == 0) return 0;
      }
      dec = host_in_bind(&s->hin, s->in, span, &req);
      if (dec) {
        if (reply_fixed(sv, s, which_of(dec)) < 0) {
          close_slot(sv, s);
          return 0;
        }
        continue;
      }
      note_version(s, &req);
      memset(&resp, 0, sizeof resp);
      rc = sv->handler(&req, &resp, sv->user);
      if (rc < 0) {
        if (resp.owe_status) {
          s->owe = 1;
          if (copy_fixed(s, 1) == 0)
            fprintf(stderr, "%s: handler failed after a commit; sending 500\n", sv->name);
        }
        close_slot(sv, s);
        return 0;
      }
      host_in_after(&s->hin, resp.pull != NULL && !s->is_head, &force, &over);
      if (over) {
        if (reply_fixed(sv, s, which_of(over)) < 0) {
          close_slot(sv, s);
          return 0;
        }
        continue;
      }
      if (force) resp.close_after = 1;
      if (begin_reply(sv, s, &resp) < 0) {
        if (resp.owe_status) s->owe = 1;
        close_slot(sv, s);
        return 0;
      }
      continue;
    }
    if (action == HOST_ACT_WRITE) {
      ssize_t n;
      size_t chunk;
      if (!s->rendered) {
        int origin = (int)field(s->st, 6);
        int which;
        note_head_buf(s, s->in, s->in_len);
        if (origin == 1) which = 0;
        else if (origin == 2) which = 2;
        else {
          close_slot(sv, s);
          return 0;
        }
        if (copy_fixed(s, which) < 0) {
          close_slot(sv, s);
          return 0;
        }
      }
      s->want_read = 0;
      if (s->out_off < s->out_len) {
        chunk = s->out_len - s->out_off;
        if (sv->write_limit > 0 && chunk > sv->write_limit) chunk = sv->write_limit;
        n = write(s->fd, s->out + s->out_off, chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          s->want_write = 1;
          return 0;
        }
        if (n <= 0) {
          if (!step_ev(sv, s, evError())) {
            close_slot(sv, s);
            return 0;
          }
          continue;
        }
        s->out_off += (size_t)n;
        if (s->out_off > 0) s->owe = 0;
        s->want_write = 0;
        note_write(sv, (size_t)n);
        if (!step_ev(sv, s, evProgress((uint64_t)n))) {
          close_slot(sv, s);
          return 0;
        }
        continue;
      }
      if (s->resp_mode != 0) {
        /* Headers for this response are already written. A pull
           error, including a chunk past max_body, cannot be a 413.
           close_slot drops the fd. A chunked body then has no
           last-chunk. */
        if (fill_pull(s) < 0) {
          close_slot(sv, s);
          return 0;
        }
        if (s->out_len > 0) continue;
      }
      s->want_write = 0;
      s->rendered = 0;
      s->out_len = 0;
      s->out_off = 0;
      if (!step_ev(sv, s, evDone())) {
        close_slot(sv, s);
        return 0;
      }
      if (s->used && s->hin.open) {
        close_slot(sv, s);
        return 0;
      }
      continue;
    }
    if (action == HOST_ACT_READ) {
      int injected;
      s->want_write = 0;
      if (more && end_off > 0 && end_off <= (uint64_t)s->in_len) {
        consume(s, end_off);
        s->want_read = 0;
        prepare_in(s, 1);
        if (!step_buf(sv, s)) {
          close_slot(sv, s);
          return 0;
        }
        continue;
      }
      if (!more && end_off > 0 && end_off <= (uint64_t)s->in_len) consume(s, end_off);
      injected = inject_next(sv, s);
      if (injected < 0) {
        close_slot(sv, s);
        return 0;
      }
      if (injected > 0) continue;
      if (phase == PH_READ && s->in && s->in_len > 0) {
        int send100 = 0;
        int pol = host_in_policy(s->in, s->in_len, sv->max_body, &send100);
        if (pol) {
          note_head_buf(s, s->in, s->in_len);
          send_and_close(sv, s, which_of(pol));
          return 0;
        }
        if (send100 && !s->sent_100) {
          int w100 = hold_100(sv, s);
          if (w100 < 0) {
            close_slot(sv, s);
            return 0;
          }
          if (w100 == 0) return 0;
        }
      }
      s->want_read = 1;
      if (timer) {
        if (s->deadline == 0) s->deadline = host_poll_now_ms() + (uint64_t)sv->idle_ms;
      } else {
        s->deadline = 0;
      }
      return 0;
    }
    s->want_read = (phase == PH_READ || phase == PH_IDLE);
    s->want_write = 0;
    return 0;
  }
  if (s->used) {
    fprintf(stderr, "%s: pump fuel exhausted\n", sv->name);
    close_slot(sv, s);
  }
  return 0;
}

static void after_io(Server *sv, Slot *s) {
  pump(sv, s);
  if (s->used) sync_watch(sv, s);
}

static void on_readable(Server *sv, Slot *s, int hangup) {
  int got = 0;
  int hit_eof = 0;
  int fresh = s->in_len == 0;
  while (s->in_len < sv->in_cap) {
    ssize_t n = read(s->fd, s->in + s->in_len, sv->in_cap - s->in_len);
    if (n > 0) {
      s->in_len += (size_t)n;
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
    if (s->in_len >= sv->in_cap) close_slot(sv, s);
    return;
  }
  s->deadline = 0;
  prepare_in(s, fresh);
  if (hit_eof || hangup) {
    if (!step_ev(sv, s, evEof(muro__slice_of(s->in, s->in_len)))) {
      close_slot(sv, s);
      return;
    }
  } else if (!step_buf(sv, s)) {
    close_slot(sv, s);
    return;
  }
  after_io(sv, s);
}

static int fd_skipped(int fd);

static void on_tick(Server *sv, Slot *s, uint64_t now) {
  int phase;
  if (!s->used || s->deadline == 0 || s->deadline > now) return;
  phase = (int)field(s->st, 0);
  /* A socket on io_skip is inside muro_http_body_read. Do not step
     the image: the handler or the pull is still on the stack.
     Decide and write leave the book timer off, so a deadline there
     was armed by that wait. Read and idle arm the book timer too.
     Clearing the deadline without body_idle lets the next
     host_io_once arm a new window, and the wait never ends. Set
     body_idle in every phase. The wait returns and that caller
     closes. Do not close_slot here. */
  if (fd_skipped(s->fd) || (phase != PH_READ && phase != PH_IDLE)) {
    s->deadline = 0;
    s->body_idle = 1;
    return;
  }
  s->deadline = 0;
  if (!step_ev(sv, s, evTick())) {
    close_slot(sv, s);
    return;
  }
  after_io(sv, s);
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
  s.st.muro__as.conn.muro__f_0 = (uint64_t)phase;
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
  s->in = malloc(sv->in_cap);
  s->out = malloc(OUT_CAP);
  s->out_cap = s->out ? (size_t)OUT_CAP : 0;
  s->proto = proto_begin();
  if (!s->in || !s->out || !s->proto) {
    proto_free(s->proto);
    free(s->in);
    free(s->out);
    close(fd);
    memset(s, 0, sizeof *s);
    s->fd = -1;
    fprintf(stderr, "%s: connection setup failed\n", sv->name);
    return -1;
  }
  s->st = begin();
  host_in_init(&s->hin, fd, sv->max_body);
  s->ver11 = 1;
  s->used = 1;
  s->want_read = 1;
  s->deadline = host_poll_now_ms() + (uint64_t)sv->idle_ms;
  sv->nconn += 1;
  sync_watch(sv, s);
  if (sv->fatal) return -1;
  return 0;
}

/* One SCM_RIGHTS message. 1 and *out set, 0 on a clean close, -1 on
   error. *out is -1 when the message carried no fd. */
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
  for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
    if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
        cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
      memcpy(out, CMSG_DATA(cmsg), sizeof *out);
      break;
    }
  }
  return 1;
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
    if (adopt_fd(sv, fd) < 0) break;
  }
  (void)sync_listen(sv);
}

static void accept_some(Server *sv) {
  if (sv->draining || sv->listen_fd < 0) return;
  if (sv->channel) {
    take_passed(sv);
    return;
  }
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
  if (listen(fd, 128) < 0) {
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
  phase = (int)field(s->st, 0);
  if (phase == PH_IDLE) return 1;
  if (phase == PH_READ && s->in_len == 0) return 1;
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
    if (s->used && evs[i].writable && s->want_write) after_io(sv, s);
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
  io_depth = 0;
  live_sv = NULL;
  memset(&sv, 0, sizeof sv);
  sv.listen_fd = -1;
  sv.idle_ms = cfg->idle_ms;
  sv.max_conn = cfg->max_conn;
  sv.max_body = (uint64_t)cfg->max_body;
  sv.drain_ms = cfg->drain_ms;
  sv.channel = channel;
  sv.in_cap = env_in_cap();
  sv.write_limit = env_write_limit();
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
      if (s->used && evs[i].writable && s->want_write) after_io(&sv, s);
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

static int reap_children(pid_t *pids, int n) {
  int left = 0;
  int i;
  int failed = 0;
  int signaled = 0;
  for (i = 0; i < n; i++) {
    if (pids[i] > 0) left += 1;
  }
  while (left > 0) {
    int status = 0;
    pid_t pid;
    if (parent_stop && !signaled) {
      signal_pids(pids, n, SIGTERM);
      signaled = 1;
    }
    pid = waitpid(-1, &status, 0);
    if (pid < 0) {
      if (errno == EINTR) continue;
      return -1;
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
  if (sendmsg(sock, &msg, 0) < 0) return -1;
  return 0;
}

static int serve_handoff(const MuroHttpConfig *cfg, MuroHttpHandler handler, void *user) {
  pid_t pids[64];
  int parent_end[64];
  int n = cfg->workers;
  int lfd = -1;
  int i;
  int rr = 0;
  int failed = 0;
  int port;
  sigset_t block;
  sigset_t old;
  struct sigaction sa;
  const char *name = cfg_name(cfg);
  const char *bind = (cfg->bind_host && cfg->bind_host[0]) ? cfg->bind_host : "127.0.0.1";
  for (i = 0; i < 64; i++) {
    pids[i] = 0;
    parent_end[i] = -1;
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
      int c = take_conn(lfd, &peer, &len);
      int tries;
      int sent = 0;
      int alive = 0;
      if (c < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        fprintf(stderr, "%s: accept: %s\n", name, strerror(errno));
        failed = 1;
        parent_stop = 1;
        break;
      }
      for (tries = 0; tries < n; tries++) {
        int w = (rr + tries) % n;
        if (parent_end[w] < 0) continue;
        if (send_fd(parent_end[w], c) == 0) {
          rr = (w + 1) % n;
          sent = 1;
          break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          close(parent_end[w]);
          parent_end[w] = -1;
        }
      }
      if (!sent) {
        struct pollfd pfds[64];
        int map[64];
        int np = 0;
        int k;
        for (tries = 0; tries < n; tries++) {
          if (parent_end[tries] < 0) continue;
          pfds[np].fd = parent_end[tries];
          pfds[np].events = POLLOUT;
          map[np] = tries;
          np += 1;
        }
        if (np > 0 && poll(pfds, (nfds_t)np, 20) > 0) {
          for (k = 0; k < np; k++) {
            if (!(pfds[k].revents & POLLOUT)) continue;
            if (send_fd(parent_end[map[k]], c) == 0) {
              rr = (map[k] + 1) % n;
              sent = 1;
              break;
            }
          }
        }
      }
      close(c);
      if (sent) continue;
      for (tries = 0; tries < n; tries++) {
        if (parent_end[tries] >= 0) alive = 1;
      }
      if (!alive) {
        failed = 1;
        parent_stop = 1;
        break;
      }
    }
  }
  parent_stop = 1;
  signal_pids(pids, n, SIGTERM);
  for (i = 0; i < n; i++) {
    if (parent_end[i] >= 0) close(parent_end[i]);
  }
  close(lfd);
  if (reap_children(pids, n) < 0) failed = 1;
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
