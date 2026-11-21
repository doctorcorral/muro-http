/* Chunked and streamed request bodies. The book classifies the bytes.
   This file reads the socket when the body does not fit the window,
   and it checks the configured maximum. One read polls only this
   connection. The other connections wait. */

#include "host_stream.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { IN_NONE = 0, IN_MEM = 1, IN_CL = 2, IN_CHUNK = 3 };

static int is_true(Bool *b) { return b && b->muro__field_tag == 0; }

static int span_eq(muro__slice s, const char *lit) {
  size_t n = strlen(lit);
  if (s.n != n) return 0;
  if (n == 0) return 1;
  return memcmp(s.p, lit, n) == 0;
}

static uint64_t ck_acc(Ck st) { return st.muro__as.ck.muro__f_1; }

void host_in_init(struct HostIn *in, int fd, uint64_t max_body) {
  if (!in) return;
  memset(in, 0, sizeof *in);
  in->fd = fd;
  in->max_body = max_body;
  in->pub.in = in;
}

void host_in_reset(struct HostIn *in) {
  int fd;
  uint64_t max_body;
  if (!in) return;
  fd = in->fd;
  max_body = in->max_body;
  free(in->hold);
  free(in->decoded);
  free(in->nextq);
  memset(in, 0, sizeof *in);
  in->fd = fd;
  in->max_body = max_body;
  in->pub.in = in;
}

void host_in_close(struct HostIn *in) { host_in_reset(in); }

void host_in_capture(struct HostIn *in, const uint8_t *buf, size_t n) {
  muro__slice sl;
  uint64_t hend;
  size_t prefix;
  if (!in || !buf) return;
  sl.p = buf;
  sl.n = n;
  /* streamTake is 0 unless headerEnd found CRLF CRLF. A bare LF
     stays unframed here. classify still rejects that buffer. */
  if (streamTake(sl) != 1) {
    muro_http_reset();
    return;
  }
  hend = headerEnd(sl);
  muro_http_reset();
  if (hend > n) return;
  prefix = n - (size_t)hend;
  free(in->hold);
  in->hold = NULL;
  in->hold_len = 0;
  in->hold_off = 0;
  if (prefix > 0) {
    in->hold = malloc(prefix);
    if (!in->hold) {
      in->err = 400;
      return;
    }
    memcpy(in->hold, buf + hend, prefix);
    in->hold_len = prefix;
  }
  in->captured = 1;
}

static int chunk_over(Ck ck, uint64_t max_body) {
  uint64_t total = ckTotal(ck);
  uint64_t rem = ckRem(ck);
  uint64_t ph = ckPh(ck);
  if (is_true(ckBad(ck))) return 0;
  if (total > max_body) return 1;
  if (ph == 3 || ph == 2 || ph == 4 || ph == 5) {
    if (rem > max_body) return 1;
    if (total > max_body - rem) return 1;
  }
  if (ph == 0 && ck_acc(ck) > max_body) return 1;
  return 0;
}

int host_in_policy(const uint8_t *buf, size_t n, uint64_t max_body, int *send_100) {
  muro__slice sl;
  muro__slice ver;
  uint64_t meth;
  uint64_t te;
  uint64_t exp;
  int ver11;
  int head;
  int complete = 1;
  int rc = 0;
  if (send_100) *send_100 = 0;
  if (!buf || n == 0) return 0;
  sl.p = buf;
  sl.n = n;
  if (!is_true(headersDone(sl))) {
    muro_http_reset();
    return 0;
  }
  meth = methodCode(sl);
  te = teCode(sl);
  exp = expectCode(sl);
  ver = versionView(sl);
  ver11 = span_eq(ver, "HTTP/1.1");
  head = span_eq(methodView(sl), "HEAD");
  if (meth == 2 || te == 3) {
    muro_http_reset();
    return 0;
  }
  if (meth == 1) {
    muro_http_reset();
    return 501;
  }
  if (te == 1) {
    Ck ck = chunkScan(sl);
    if (is_true(ckBad(ck))) {
      muro_http_reset();
      return 0;
    }
    complete = is_true(ckDone(ck));
    if (chunk_over(ck, max_body) || (complete && ckTotal(ck) > max_body)) rc = 413;
  } else if (is_true(sawCL(sl))) {
    uint64_t cl = contentLength(sl);
    complete = !is_true(bodyShort(sl));
    if (cl > max_body) rc = 413;
  }
  if (rc == 0 && ver11 && exp == 2) rc = 417;
  if (rc == 0 && send_100 && ver11 && exp == 1 && !complete && !head) *send_100 = 1;
  muro_http_reset();
  return rc;
}

static int feed_ck(struct HostIn *in, uint8_t b, uint8_t *emit, int *did) {
  uint64_t prev = ckPh(in->ck);
  uint64_t rem = ckRem(in->ck);
  uint64_t total = ckTotal(in->ck);
  *did = 0;
  if (prev == 8 || prev == 9) return 0;
  if (prev == 2 && b == '\n' && rem > 0) {
    /* The size line just finished. Reject before any of this chunk
       is copied out. If the 200 is already on the wire, the caller
       closes. That close is not a second status. */
    if (rem > in->max_body || total > in->max_body || rem > in->max_body - total) {
      in->err = 413;
      return -1;
    }
  }
  if (prev == 0) {
    in->ck = ckByte(in->ck, b);
    if (is_true(ckBad(in->ck))) {
      in->err = 400;
      return -1;
    }
    if (ckPh(in->ck) == 0 && ck_acc(in->ck) > in->max_body) {
      in->err = 413;
      return -1;
    }
    return 0;
  }
  in->ck = ckByte(in->ck, b);
  if (is_true(ckBad(in->ck))) {
    in->err = 400;
    return -1;
  }
  if (prev == 3) {
    if (ckTotal(in->ck) > in->max_body) {
      in->err = 413;
      return -1;
    }
    *emit = b;
    *did = 1;
  }
  return 0;
}

static int decode_chunked(struct HostIn *in, const uint8_t *p, size_t n) {
  size_t i;
  size_t used = 0;
  size_t cap = 0;
  uint8_t *out = NULL;
  in->ck = ckStart();
  for (i = 0; i < n; i++) {
    uint8_t em = 0;
    int did = 0;
    if (feed_ck(in, p[i], &em, &did) < 0) {
      free(out);
      return -1;
    }
    if (did) {
      if (used == cap) {
        size_t nc = cap == 0 ? 64 : cap * 2;
        uint8_t *np;
        if (nc < used + 1) nc = used + 1;
        np = realloc(out, nc);
        if (!np) {
          free(out);
          in->err = 400;
          return -1;
        }
        out = np;
        cap = nc;
      }
      out[used++] = em;
    }
    if (is_true(ckDone(in->ck))) break;
  }
  if (!is_true(ckDone(in->ck))) {
    free(out);
    in->err = 400;
    return -1;
  }
  if (ckTotal(in->ck) > in->max_body) {
    free(out);
    in->err = 413;
    return -1;
  }
  in->decoded = out;
  in->mem = out;
  in->mem_len = used;
  in->mem_off = 0;
  return 0;
}

static MuroHttpSlice to_pub(muro__slice s) {
  MuroHttpSlice v;
  v.p = s.p;
  v.n = s.n;
  return v;
}

int host_in_bind(struct HostIn *in, const uint8_t *buf, size_t n, MuroHttpRequest *req) {
  muro__slice sl;
  uint64_t te;
  if (!in || !req || !buf) return 400;
  sl.p = buf;
  sl.n = n;
  memset(req, 0, sizeof *req);
  req->method = to_pub(methodView(sl));
  req->target = to_pub(targetView(sl));
  req->path = to_pub(pathView(sl));
  req->query = to_pub(queryView(sl));
  req->version = to_pub(versionView(sl));
  req->body = to_pub(bodyView(sl));
  req->buf = buf;
  req->buf_n = n;
  if (in->err) {
    muro_http_reset();
    return in->err;
  }
  te = teCode(sl);
  if (te == 1 && in->captured) {
    in->kind = IN_CHUNK;
    in->ck = ckStart();
    in->open = 1;
    req->body.p = NULL;
    req->body.n = 0;
    req->body_in = &in->pub;
    muro_http_reset();
    return 0;
  }
  if (te == 1) {
    uint64_t hend = headerEnd(sl);
    uint64_t end = framedEnd(sl);
    const uint8_t *tail;
    size_t tn;
    if (end < hend || end > n) {
      muro_http_reset();
      in->err = 400;
      return 400;
    }
    tail = buf + (size_t)hend;
    tn = (size_t)(end - hend);
    muro_http_reset();
    if (decode_chunked(in, tail, tn) < 0) return in->err ? in->err : 400;
    in->kind = IN_MEM;
    req->body.p = in->mem;
    req->body.n = in->mem_len;
    req->body_in = &in->pub;
    return 0;
  }
  if (in->captured) {
    in->kind = IN_CL;
    in->need = contentLength(sl);
    in->got = 0;
    in->open = 1;
    req->body.p = NULL;
    req->body.n = 0;
    req->body_in = &in->pub;
    muro_http_reset();
    return 0;
  }
  if (req->body.n > 0 && req->body.p) {
    in->kind = IN_MEM;
    in->mem = req->body.p;
    in->mem_len = req->body.n;
    in->mem_off = 0;
    req->body_in = &in->pub;
  }
  muro_http_reset();
  return 0;
}

void host_in_after(struct HostIn *in, int pull_owns, int *force_close, int *override) {
  if (!in) return;
  in->pull_owns = pull_owns ? 1 : 0;
  if (override && (in->err == 400 || in->err == 413)) *override = in->err;
  if (force_close && in->open && !in->pull_owns) *force_close = 1;
}

int host_in_take_next(struct HostIn *in, uint8_t **p, size_t *n) {
  if (!in || !in->nextq || in->nextq_len == 0) return 0;
  *p = in->nextq;
  *n = in->nextq_len;
  in->nextq = NULL;
  in->nextq_len = 0;
  return 1;
}

static int wait_read(struct HostIn *in) {
  if (host_io_should_stop()) {
    in->err = 400;
    return -1;
  }
  if (host_io_once(in->fd) < 0) {
    in->err = 400;
    return -1;
  }
  return 0;
}

static void save_rest(struct HostIn *in) {
  size_t hold_left = 0;
  size_t scratch_left = 0;
  size_t n;
  uint8_t *p;
  if (in->hold_off < in->hold_len) hold_left = in->hold_len - in->hold_off;
  if (in->scratch_off < in->scratch_len) scratch_left = in->scratch_len - in->scratch_off;
  n = hold_left + scratch_left;
  if (n == 0 || in->nextq) {
    in->hold_off = in->hold_len;
    in->scratch_off = in->scratch_len;
    return;
  }
  p = malloc(n);
  if (!p) {
    in->err = 400;
    return;
  }
  if (hold_left) memcpy(p, in->hold + in->hold_off, hold_left);
  if (scratch_left) memcpy(p + hold_left, in->scratch + in->scratch_off, scratch_left);
  in->nextq = p;
  in->nextq_len = n;
  in->hold_off = in->hold_len;
  in->scratch_off = in->scratch_len;
}

static int take_byte(struct HostIn *in, uint8_t *b) {
  ssize_t r;
  for (;;) {
  if (in->hold_off < in->hold_len) {
    *b = in->hold[in->hold_off++];
    return 1;
  }
  if (in->scratch_off < in->scratch_len) {
    *b = in->scratch[in->scratch_off++];
    return 1;
  }
  if (wait_read(in) < 0) return -1;
  r = read(in->fd, in->scratch, sizeof in->scratch);
  if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
  if (r < 0) {
    in->err = 400;
    return -1;
  }
  if (r == 0) return 0;
  in->scratch_len = (size_t)r;
  in->scratch_off = 0;
  *b = in->scratch[in->scratch_off++];
  return 1;
  }
}

static int read_mem(struct HostIn *in, uint8_t *dst, size_t cap) {
  size_t left;
  size_t n;
  if (in->mem_off >= in->mem_len || !in->mem) return 0;
  left = in->mem_len - in->mem_off;
  n = left < cap ? left : cap;
  memcpy(dst, in->mem + in->mem_off, n);
  in->mem_off += n;
  return (int)n;
}

static int read_cl(struct HostIn *in, uint8_t *dst, size_t cap) {
  size_t left;
  size_t n;
  ssize_t r;
  if (in->got >= in->need) {
    in->open = 0;
    return 0;
  }
  left = (size_t)(in->need - in->got);
  if (left > cap) left = cap;
  if (in->hold_off < in->hold_len) {
    size_t h = in->hold_len - in->hold_off;
    if (h > left) h = left;
    memcpy(dst, in->hold + in->hold_off, h);
    in->hold_off += h;
    in->got += h;
    if (in->got >= in->need) in->open = 0;
    return (int)h;
  }
  for (;;) {
    if (wait_read(in) < 0) return -1;
    r = read(in->fd, dst, left);
    if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (r < 0 || r == 0) {
      in->err = 400;
      in->open = 1;
      return -1;
    }
    n = (size_t)r;
    if (n > left) n = left;
    in->got += n;
    if (in->got >= in->need) in->open = 0;
    return (int)n;
  }
}

static int read_chunk(struct HostIn *in, uint8_t *dst, size_t cap) {
  size_t produced = 0;
  if (!in->open && is_true(ckDone(in->ck))) return 0;
  while (produced < cap) {
    uint8_t b = 0;
    uint8_t em = 0;
    int did = 0;
    int tb;
    if (is_true(ckDone(in->ck))) {
      save_rest(in);
      in->open = 0;
      break;
    }
    tb = take_byte(in, &b);
    if (tb < 0) return -1;
    if (tb == 0) {
      in->err = 400;
      return -1;
    }
    if (feed_ck(in, b, &em, &did) < 0) return -1;
    if (did) dst[produced++] = em;
    if (is_true(ckDone(in->ck))) {
      save_rest(in);
      in->open = 0;
      break;
    }
  }
  return (int)produced;
}

/* MURO_TEST_STREAM_READY is a path created on the first entry to
   muro_http_body_read for a streamed body (Content-Length past the
   window, or chunked), before any of those bytes are copied out.
   A buffered body does not create it. Production leaves it unset.
   The hello echo test waits for this path before sending the rest
   of the body, so those bytes cannot land in the classify buffer. */
static void note_stream_ready(void) {
  const char *path;
  int fd;
  static int done = 0;
  if (done) return;
  path = getenv("MURO_TEST_STREAM_READY");
  if (!path || !path[0]) {
    done = 1;
    return;
  }
  fd = open(path, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  if (write(fd, "1\n", 2) < 0) {
    close(fd);
    return;
  }
  close(fd);
  done = 1;
}

int muro_http_body_read(MuroHttpBody *body, uint8_t *dst, size_t cap) {
  struct HostIn *in;
  if (!body || !body->in || !dst || cap == 0) return -1;
  in = body->in;
  if (in->err) return -1;
  if (cap > 1u << 20) cap = 1u << 20;
  if (in->kind == IN_CL || in->kind == IN_CHUNK) note_stream_ready();
  if (in->kind == IN_MEM) return read_mem(in, dst, cap);
  if (in->kind == IN_CL) return read_cl(in, dst, cap);
  if (in->kind == IN_CHUNK) return read_chunk(in, dst, cap);
  return 0;
}
