/* One connection is one image and one session.
   The image is conn.step. The moves are proto_admit, which applies
   the emitted admitChoice. This file does not choose a move, a
   status, or a response. It feeds bytes, peer eof, write progress,
   write done, write error, an idle tick, and one stub db event.
   It performs the action the image returns.

   c/generated/render.h cannot sit in this file. It and conn.h both
   define the muro prelude. c/host_render.c includes render.h and
   copies the slice. This file does not declare httpRender. */

#include "shim.h"
#include "conn.h"
#include "proto.h"
#include "decision_tags.h"
#include "host.h"
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
#include <time.h>
#include <unistd.h>

enum {
  PH_READ = 0,
  PH_DECIDE = 1,
  PH_WRITE = 2,
  PH_IDLE = 3,
  PH_CLOSED = 4,
  /* Connection actions from conn.step. Not the decide act tags. */
  HOST_ACT_NONE = 0,
  HOST_ACT_READ = 1,
  HOST_ACT_WRITE = 2,
  HOST_ACT_CLOSE = 3,
  HOST_ACT_DECIDE = 4,
  EV_BYTES = 0,
  EV_EOF = 1,
  EV_DB = 6,
  IN_CAP = 65536,
  OUT_CAP = 2048,
  PUMP_FUSE = 100000
};

static volatile sig_atomic_t stop_flag;

static void on_signal(int sig) {
  (void)sig;
  stop_flag = 1;
}

static uint64_t mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
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
  case 13: return st.muro__as.conn.muro__f_13;
  case 14: return st.muro__as.conn.muro__f_14;
  default: return 0;
  }
}

typedef struct Slot {
  int fd;
  int used;
  int want_read;
  int want_write;
  int rendered;
  int db_sent;
  Proto *proto;
  Conn st;
  uint8_t *in;
  size_t in_len;
  uint8_t out[OUT_CAP];
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
} Server;

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
    fprintf(stderr, "muro-httpd: listen watch failed: %s\n", strerror(err));
    sv->fatal = 1;
    return -1;
  }
  return 0;
}

static void close_slot(Server *sv, Slot *s);

static void sync_watch(Server *sv, Slot *s) {
  int err;
  if (!s->used) return;
  if (host_poll_watch(sv->pq, s->fd, s->want_read, s->want_write) < 0) {
    err = errno;
    fprintf(stderr, "muro-httpd: watch %d failed: %s\n", s->fd, strerror(err));
    close_slot(sv, s);
  }
}

static void close_slot(Server *sv, Slot *s) {
  if (!s->used) return;
  host_poll_watch(sv->pq, s->fd, 0, 0);
  close(s->fd);
  proto_free(s->proto);
  free(s->in);
  memset(s, 0, sizeof *s);
  s->fd = -1;
  if (sv->nconn > 0) sv->nconn -= 1;
  (void)sync_listen(sv);
}

/* Stub facts. Not a decision and not a database.
   auth-absent: this host does not check a bearer token.
   body-none: classify does not parse JSON, and this host does not either.
   like-none: there is no like table.
   id is the classifier tag already stored on the connection.
   id-present and id-missing are not supplied.
   GET /health gets db-ok. Health does not read a row. The process is up.
   Every other route gets db-down. db-unknown would leave the image
   observing, and this host has no second fact. */
static void stub_facts(const Slot *s, uint64_t *auth, uint64_t *body, uint64_t *like,
                       uint64_t *db, uint64_t *idc) {
  uint64_t verb = field(s->st, 3);
  uint64_t path = field(s->st, 4);
  *auth = AUTH_ABSENT;
  *body = BODY_NONE;
  *like = LIKE_NONE;
  *idc = field(s->st, 5);
  if (verb == VERB_GET && path == PATH_HEALTH) *db = DB_OK;
  else *db = DB_DOWN;
}

static int step_ev(Slot *s, Ev ev) {
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
  if (proto_live(s->proto) && !proto_done(s->proto) &&
      (phase == PH_READ || phase == PH_IDLE) &&
      (tag == EV_BYTES || (tag == EV_EOF && buf.n > 0))) {
    Classified *c = classify(buf);
    status = c->muro__as.classified.muro__f_0->muro__field_tag;
  }
  if (proto_live(s->proto) && !proto_done(s->proto) && phase == PH_DECIDE && tag == EV_DB) {
    Decision *d = decide(verbFrom(field(s->st, 3)), pathFrom(field(s->st, 4)),
                         idFrom(ev.muro__as.ev.muro__f_6), authFrom(ev.muro__as.ev.muro__f_2),
                         bodyFrom(ev.muro__as.ev.muro__f_3), likeFrom(ev.muro__as.ev.muro__f_4),
                         dbFrom(ev.muro__as.ev.muro__f_5));
    int act = d->muro__as.decided.muro__f_2->muro__field_tag;
    int code = d->muro__as.decided.muro__f_0->muro__field_tag;
    if (act == ACT_OBSERVE) observe = 1;
    else reply = code <= ST_201 ? 0 : 1;
  }
  admitted = proto_admit(s->proto, phase, tag, buf.n, status, reply, observe, half || keep);
  if (admitted != 1) {
    muro_http_reset();
    fprintf(stderr, "muro-httpd: admit refused phase %d ev %d\n", phase, tag);
    return 0;
  }
  s->st = step(s->st, ev);
  muro_http_reset();
  return 1;
}

static int render_out(Slot *s) {
  size_t got = 0;
  size_t n = host_copy_render(s->out, OUT_CAP, field(s->st, 8), field(s->st, 9), field(s->st, 10),
                              field(s->st, 2), &got);
  if (n == 0) {
    muro_http_reset();
    fprintf(stderr, "muro-httpd: response is %zu bytes\n", got);
    return 0;
  }
  s->out_len = n;
  s->out_off = 0;
  s->rendered = 1;
  muro_http_reset();
  return 1;
}

static void consume(Slot *s, uint64_t end_off) {
  size_t n;
  if (end_off == 0 || end_off > (uint64_t)s->in_len) return;
  n = (size_t)end_off;
  memmove(s->in, s->in + n, s->in_len - n);
  s->in_len -= n;
}

/* Returns 1 to keep pumping, 0 when the slot is waiting or gone. */
static int pump(Server *sv, Slot *s) {
  int spins = 0;
  while (s->used && spins < PUMP_FUSE) {
    int phase = (int)field(s->st, 0);
    int action = (int)field(s->st, 6);
    int timer = (int)field(s->st, 7);
    uint64_t end_off = field(s->st, 13);
    uint64_t more = field(s->st, 14);
    spins += 1;
    if (phase != PH_DECIDE) s->db_sent = 0;
    if (phase == PH_CLOSED || action == HOST_ACT_CLOSE) {
      close_slot(sv, s);
      return 0;
    }
    if (action == HOST_ACT_DECIDE) {
      uint64_t auth, body, like, db, idc;
      if (s->db_sent) {
        fprintf(stderr, "muro-httpd: decide asked again; no further fact\n");
        close_slot(sv, s);
        return 0;
      }
      s->db_sent = 1;
      stub_facts(s, &auth, &body, &like, &db, &idc);
      if (!step_ev(s, evDb(auth, body, like, db, idc))) {
        close_slot(sv, s);
        return 0;
      }
      continue;
    }
    if (action == HOST_ACT_WRITE) {
      ssize_t n;
      size_t chunk;
      if (!s->rendered) {
        if (!render_out(s)) {
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
          if (!step_ev(s, evError())) {
            close_slot(sv, s);
            return 0;
          }
          continue;
        }
        s->out_off += (size_t)n;
        s->want_write = 0;
        note_write(sv, (size_t)n);
        if (!step_ev(s, evProgress((uint64_t)n))) {
          close_slot(sv, s);
          return 0;
        }
        continue;
      }
      s->want_write = 0;
      s->rendered = 0;
      s->out_len = 0;
      s->out_off = 0;
      if (!step_ev(s, evDone())) {
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
        if (!step_ev(s, evBytes(muro__slice_of(s->in, s->in_len)))) {
          close_slot(sv, s);
          return 0;
        }
        continue;
      }
      if (!more && end_off > 0 && end_off <= (uint64_t)s->in_len) consume(s, end_off);
      s->want_read = 1;
      if (timer) {
        if (s->deadline == 0) s->deadline = mono_ms() + (uint64_t)sv->idle_ms;
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
    fprintf(stderr, "muro-httpd: pump fuel exhausted\n");
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
    /* A full buffer that the image did not consume cannot take another
       byte. Closing is a host limit. It is not a status. */
    if (s->in_len >= sv->in_cap) close_slot(sv, s);
    return;
  }
  s->deadline = 0;
  if (hit_eof || hangup) {
    if (!step_ev(s, evEof(muro__slice_of(s->in, s->in_len)))) {
      close_slot(sv, s);
      return;
    }
  } else if (!step_ev(s, evBytes(muro__slice_of(s->in, s->in_len)))) {
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
  if (!step_ev(s, evTick())) {
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
      fprintf(stderr, "muro-httpd: accept: %s\n", strerror(errno));
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
    s->proto = proto_begin();
    if (!s->in || !s->proto) {
      proto_free(s->proto);
      free(s->in);
      close(fd);
      memset(s, 0, sizeof *s);
      s->fd = -1;
      fprintf(stderr, "muro-httpd: connection setup failed\n");
      return;
    }
    s->st = begin();
    s->used = 1;
    s->want_read = 1;
    s->deadline = mono_ms() + (uint64_t)sv->idle_ms;
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
  now = mono_ms();
  if (soon <= now) ms = 1;
  else if (soon - now > 86400000ull) ms = 86400000u;
  else ms = (unsigned)(soon - now);
  if (host_poll_arm(sv->pq, ms) < 0) {
    fprintf(stderr, "muro-httpd: timer: %s\n", strerror(errno));
    return;
  }
  sv->armed_deadline = soon;
}

static int listen_on(int port) {
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
  if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
    close(fd);
    return -1;
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

int host_run(const HostConfig *cfg) {
  Server sv;
  struct sigaction sa;
  int port;
  int rc = 0;
  if (!cfg || cfg->port < 0 || cfg->port > 65535 || cfg->idle_ms < 1 || cfg->max_conn < 1)
    return -1;
  stop_flag = 0;
  memset(&sv, 0, sizeof sv);
  sv.listen_fd = -1;
  sv.idle_ms = cfg->idle_ms;
  sv.max_conn = cfg->max_conn;
  sv.in_cap = env_in_cap();
  sv.write_limit = env_write_limit();
  sv.write_log = getenv("MURO_TEST_WRITE_LOG");
  if (sv.write_log && !sv.write_log[0]) sv.write_log = NULL;
  sv.slots = calloc((size_t)cfg->max_conn, sizeof(Slot));
  sv.pq = host_poll_open();
  if (sv.slots) {
    int i;
    for (i = 0; i < sv.max_conn; i++) sv.slots[i].fd = -1;
  }
  if (!sv.slots || !sv.pq) {
    fprintf(stderr, "muro-httpd: setup failed\n");
    rc = -1;
    goto out;
  }
  sv.listen_fd = listen_on(cfg->port);
  if (sv.listen_fd < 0) {
    fprintf(stderr, "muro-httpd: listen 127.0.0.1:%d: %s\n", cfg->port, strerror(errno));
    rc = -1;
    goto out;
  }
  port = bound_port(sv.listen_fd);
  if (port < 0) {
    fprintf(stderr, "muro-httpd: getsockname: %s\n", strerror(errno));
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
  printf("listening 127.0.0.1 %d\n", port);
  fflush(stdout);
  while (!stop_flag && !sv.fatal) {
    HostEv evs[32];
    int n;
    int i;
    arm_soonest(&sv);
    n = host_poll_wait(sv.pq, evs, 32);
    if (n < 0) {
      fprintf(stderr, "muro-httpd: poll: %s\n", strerror(errno));
      rc = -1;
      break;
    }
    if (stop_flag || sv.fatal) break;
    for (i = 0; i < n; i++) {
      Slot *s;
      if (sv.fatal) break;
      if (evs[i].timer) {
        uint64_t now = mono_ms();
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
