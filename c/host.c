/* One connection is one image and one session.
   The image is conn.step. The moves are proto_admit, which applies
   the emitted admitChoice. This file does not choose a move, a
   status, or a response. It feeds bytes, peer eof, write progress,
   write done, write error, an idle tick, and the handler's reply.
   When the image asks for the handler, this file builds a request
   view, calls that callback, and copies the book's framing of the
   handler's status, headers, and body.

   A malformed request is origin 1. The bytes are httpMalformed.
   A committed handler that fails before a status byte leaves is
   httpInternal. This file does not invent those words.

   c/generated/render.h cannot sit in this file. It and conn.h both
   define the muro prelude. c/host_render.c includes render.h. */

#include "shim.h"
#include "conn.h"
#include "proto.h"
#include "muro_http.h"
#include "host_poll.h"
#include "host_render.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

static void on_signal(int sig) {
  (void)sig;
  stop_flag = 1;
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
} Server;

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
static void status_before_close(Server *sv, Slot *s) {
  uint8_t tmp[512];
  size_t n = 0;
  const uint8_t *msg;
  if (!s->owe || s->fd < 0 || s->out_off > 0) return;
  if (s->rendered && s->out && s->out_len > 0) {
    msg = s->out;
    n = s->out_len;
  } else {
    n = host_copy_internal(tmp, sizeof tmp, NULL);
    msg = tmp;
    fprintf(stderr, "%s: committed write has no status; sending 500\n", sv->name);
  }
  if (n > 0) write_status(sv, s->fd, msg, n);
  s->owe = 0;
}

static int fill_request(Slot *s, MuroHttpRequest *req) {
  uint64_t end = field(s->st, 11);
  muro__slice buf;
  if (end == 0 || end > (uint64_t)s->in_len || !s->in) return -1;
  buf.p = s->in;
  buf.n = (size_t)end;
  memset(req, 0, sizeof *req);
  req->method = to_pub(methodView(buf));
  req->target = to_pub(targetView(buf));
  req->path = to_pub(pathView(buf));
  req->query = to_pub(queryView(buf));
  req->version = to_pub(versionView(buf));
  req->body = to_pub(bodyView(buf));
  req->buf = s->in;
  req->buf_n = (size_t)end;
  return 0;
}

static int begin_reply(Server *sv, Slot *s, const MuroHttpResponse *resp) {
  int half = (int)field(s->st, 1);
  int keep = (int)field(s->st, 2);
  int bad = (resp->status >= 400 && resp->status <= 599) ? 1 : 0;
  int bit = (resp->close_after || bad) ? 1 : 0;
  uint64_t eff = (half || keep || bit) ? 1u : 0u;
  size_t got = 0;
  size_t n;
  const uint8_t *reason = resp->reason ? (const uint8_t *)resp->reason : NULL;
  const uint8_t *extra = resp->headers ? (const uint8_t *)resp->headers : NULL;
  const uint8_t *body = resp->body;
  size_t reason_n = reason ? resp->reason_len : 0;
  size_t extra_n = extra ? resp->headers_len : 0;
  size_t body_n = body ? resp->body_len : 0;
  n = host_copy_response(s->out, s->out_cap, (uint64_t)resp->status, reason, reason_n, extra,
                         extra_n, body, body_n, eff, &got);
  if (n == 0 && resp->owe_status) {
    n = host_copy_internal(s->out, s->out_cap, NULL);
    bit = 1;
    bad = 1;
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

static void consume(Slot *s, uint64_t end_off) {
  size_t n;
  if (end_off == 0 || end_off > (uint64_t)s->in_len) return;
  n = (size_t)end_off;
  memmove(s->in, s->in + n, s->in_len - n);
  s->in_len -= n;
}

static int copy_fixed(Slot *s, int which) {
  size_t got = 0;
  size_t n = which ? host_copy_internal(s->out, s->out_cap, &got)
                   : host_copy_malformed(s->out, s->out_cap, &got);
  if (n == 0) return -1;
  s->out_len = n;
  s->out_off = 0;
  s->rendered = 1;
  return 0;
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
    if (phase == PH_CLOSED || action == HOST_ACT_CLOSE) {
      close_slot(sv, s);
      return 0;
    }
    if (phase == PH_READ || phase == PH_IDLE) {
      s->rendered = 0;
      s->owe = 0;
      s->out_len = 0;
      s->out_off = 0;
    }
    if (action == HOST_ACT_HANDLER) {
      MuroHttpRequest req;
      MuroHttpResponse resp;
      int rc;
      if (fill_request(s, &req) < 0 || !sv->handler) {
        close_slot(sv, s);
        return 0;
      }
      memset(&resp, 0, sizeof resp);
      rc = sv->handler(&req, &resp, sv->user);
      if (rc < 0) {
        if (resp.owe_status) {
          s->owe = 1;
          if (copy_fixed(s, 1) == 0) {
            fprintf(stderr, "%s: handler failed after a commit; sending 500\n", sv->name);
          }
        }
        close_slot(sv, s);
        return 0;
      }
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
        /* Origin 1 is the book's malformed response. A handler
           reply was copied before the image entered write. */
        if (field(s->st, 6) != 1 || copy_fixed(s, 0) < 0) {
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
      s->want_write = 0;
      s->rendered = 0;
      s->out_len = 0;
      s->out_off = 0;
      if (!step_ev(sv, s, evDone())) {
        close_slot(sv, s);
        return 0;
      }
      continue;
    }
    if (action == HOST_ACT_READ) {
      s->want_write = 0;
      if (more && end_off > 0 && end_off <= (uint64_t)s->in_len) {
        consume(s, end_off);
        s->want_read = 0;
        if (!step_ev(sv, s, evBytes(muro__slice_of(s->in, s->in_len)))) {
          close_slot(sv, s);
          return 0;
        }
        continue;
      }
      if (!more && end_off > 0 && end_off <= (uint64_t)s->in_len) consume(s, end_off);
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
  if (hit_eof || hangup) {
    if (!step_ev(sv, s, evEof(muro__slice_of(s->in, s->in_len)))) {
      close_slot(sv, s);
      return;
    }
  } else if (!step_ev(sv, s, evBytes(muro__slice_of(s->in, s->in_len)))) {
    close_slot(sv, s);
    return;
  }
  after_io(sv, s);
}

static void on_tick(Server *sv, Slot *s, uint64_t now) {
  int phase;
  if (!s->used || s->deadline == 0 || s->deadline > now) return;
  phase = (int)field(s->st, 0);
  s->deadline = 0;
  if (phase != PH_READ && phase != PH_IDLE) return;
  if (!step_ev(sv, s, evTick())) {
    close_slot(sv, s);
    return;
  }
  after_io(sv, s);
}

static int take_conn(int lfd, struct sockaddr_in *peer, socklen_t *len) {
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
  int fd = accept4(lfd, (struct sockaddr *)peer, len, SOCK_CLOEXEC | SOCK_NONBLOCK);
  if (fd >= 0 || errno != ENOSYS) return fd;
#endif
  return accept(lfd, (struct sockaddr *)peer, len);
}

static void accept_some(Server *sv) {
  while (sv->nconn < sv->max_conn && !sv->fatal) {
    struct sockaddr_in peer;
    socklen_t len = sizeof peer;
    int fd = take_conn(sv->listen_fd, &peer, &len);
    Slot *s;
    int i;
    if (fd < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;
      fprintf(stderr, "%s: accept: %s\n", sv->name, strerror(errno));
      return;
    }
    if (set_nb_cloexec(fd) < 0) {
      close(fd);
      continue;
    }
    if (set_int(fd, IPPROTO_TCP, TCP_NODELAY, 1) < 0) {
      close(fd);
      continue;
    }
#ifdef SO_NOSIGPIPE
    if (set_int(fd, SOL_SOCKET, SO_NOSIGPIPE, 1) < 0) {
      close(fd);
      continue;
    }
#endif
    s = NULL;
    for (i = 0; i < sv->max_conn; i++) {
      if (!sv->slots[i].used) {
        s = &sv->slots[i];
        break;
      }
    }
    if (!s) {
      close(fd);
      return;
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
      return;
    }
    s->st = begin();
    s->used = 1;
    s->want_read = 1;
    s->deadline = host_poll_now_ms() + (uint64_t)sv->idle_ms;
    sv->nconn += 1;
    sync_watch(sv, s);
    if (sv->fatal) return;
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

static int listen_on(const char *host, int port) {
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
          "\n"
          "Environment, used when the flag is absent: MURO_HTTP_BIND,\n"
          "MURO_HTTP_PORT, MURO_HTTP_IDLE_MS, MURO_HTTP_MAX_CONNECTIONS.\n"
          "The default bind host is 127.0.0.1. SIGINT and SIGTERM stop it.\n",
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

int muro_http_serve(const MuroHttpConfig *cfg, MuroHttpHandler handler, void *user) {
  Server sv;
  struct sigaction sa;
  const char *bind;
  const char *name;
  int port;
  int rc = 0;
  if (!cfg || !handler || cfg->port < 0 || cfg->port > 65535 || cfg->idle_ms < 1 ||
      cfg->max_conn < 1)
    return -1;
  stop_flag = 0;
  memset(&sv, 0, sizeof sv);
  sv.listen_fd = -1;
  sv.idle_ms = cfg->idle_ms;
  sv.max_conn = cfg->max_conn;
  sv.in_cap = env_in_cap();
  sv.write_limit = env_write_limit();
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
  sv.listen_fd = listen_on(sv.bind_host, cfg->port);
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
  port = bound_port(sv.listen_fd);
  if (port < 0) {
    fprintf(stderr, "%s: getsockname: %s\n", sv.name, strerror(errno));
    rc = -1;
    goto out;
  }
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);
  signal(SIGPIPE, SIG_IGN);
  if (sync_listen(&sv) < 0) {
    rc = -1;
    goto out;
  }
  printf("listening %s %d\n", sv.bind_host, port);
  fflush(stdout);
  while (!stop_flag && !sv.fatal) {
    HostEv evs[32];
    int n;
    int i;
    arm_soonest(&sv);
    n = host_poll_wait(sv.pq, evs, 32);
    if (n < 0) {
      fprintf(stderr, "%s: poll: %s\n", sv.name, strerror(errno));
      rc = -1;
      break;
    }
    if (stop_flag || sv.fatal) break;
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
      if (evs[i].fd == sv.listen_fd) {
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
    if (sv.fatal) {
      rc = -1;
      break;
    }
  }
  if (sv.fatal) rc = -1;
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
