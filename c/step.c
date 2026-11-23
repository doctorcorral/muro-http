/* HTTP/1.1 for one connection: classify, admit, the handler, keep or
   close, the body limit, and the idle deadline. The bytes to write
   already live in the buffer the adapter owns. This file does not
   wait, accept, or transfer on a socket. c/check_step_pure.sh greps
   this translation unit.

   The image is conn.step. The moves are proto_admit. Origin 1 is
   httpMalformed. A header block the book rejects for passing 16384
   bytes is 414 when the request line itself passes that cap, and
   431 otherwise. Origin 2 is httpNotImplemented. A committed handler
   that fails before a status byte leaves is httpInternal. 100, 413,
   and 417 come from host_in_policy, and 501 from method code 1.
   host_in_policy does not answer an unknown transfer coding. */

#include "step.h"
#include "shim.h"
#include "host_render.h"
#include "host_stream.h"

#include <stdlib.h>
#include <string.h>

enum {
  ACT_NONE = 0,
  ACT_READ = 1,
  ACT_WRITE = 2,
  ACT_CLOSE = 3,
  ACT_HANDLER = 4,
  EV_BYTES = 0,
  EV_EOF = 1,
  EV_REPLY = 6,
  PEND_NONE = 0,
  PEND_CONT = 1,
  PEND_OUT = 2,
  /* close_conn, phase closed, or -1. Later facts must not run. */
  PEND_SHUT = 3,
  PUMP_FUSE = 100000,
  /* Same cap as gt16384 in the request book. which 6 is 414.
     which 7 is 431. */
  HDR_CAP = 16384
};

/* 6, 7, or 0. The book already calls a block past HDR_CAP malformed.
   A request line with no LF inside the cap is 414. A finished block
   that ends past the cap, or an unfinished block past the cap whose
   request line fit, is 431. A short malformed request stays 0 so
   the caller still sends 400. Bytes after a short header block are
   a body, not a second header cap. */
static int limit_which(const uint8_t *buf, size_t n) {
  size_t i;
  if (!buf || n <= (size_t)HDR_CAP) return 0;
  for (i = 0; i < (size_t)HDR_CAP; i++) {
    if (buf[i] == '\n') break;
  }
  if (i == (size_t)HDR_CAP) return 6;
  for (i = 0; i + 3 < n; i++) {
    if (i >= (size_t)HDR_CAP) return 7;
    if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
      if (i + 4 > (size_t)HDR_CAP) return 7;
      return 0;
    }
  }
  return 7;
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

static void interests(HttpStep *st, StepOut *out, int rd, int wr) {
  if (st->read_more) *st->read_more = rd;
  if (st->write_more) *st->write_more = wr;
  if (out) {
    out->read_more = rd;
    out->write_more = wr;
  }
}

static void add_note(StepOut *out, int note, int phase, int ev, size_t n) {
  if (!out) return;
  if (!out->note) {
    out->note = note;
    out->note_phase = phase;
    out->note_ev = ev;
    out->note_n = n;
    return;
  }
  if (!out->note2) {
    out->note2 = note;
    out->note_n2 = n;
  }
}

static void copy_date(const StepIn *in, char date[30]) {
  if (in && in->date && in->date_n >= 29) {
    memcpy(date, in->date, 29);
    date[29] = 0;
    return;
  }
  memcpy(date, "Thu, 01 Jan 1970 00:00:00 GMT", 30);
}

static int step_ev(HttpStep *st, StepOut *out, Ev ev) {
  int phase = (int)field(st->st, 0);
  int half = (int)field(st->st, 1);
  int keep = (int)field(st->st, 2);
  int tag = (int)ev.muro__as.ev.muro__f_0;
  muro__slice buf = ev.muro__as.ev.muro__f_1;
  int status = -1;
  int reply = -1;
  int observe = 0;
  int admitted;

  if (!st->proto) return 0;
  if (proto_live(st->proto) && !proto_done(st->proto) &&
      (phase == MURO_STEP_PH_READ || phase == MURO_STEP_PH_IDLE) &&
      (tag == EV_BYTES || (tag == EV_EOF && buf.n > 0))) {
    Classified *c = classify(buf);
    status = c->muro__as.classified.muro__f_0->muro__field_tag;
  }
  /* n1 is 1 for an HTTP status 400..599. That is reply-bad and
     closes the connection. Any other handler status is reply-ok. */
  if (proto_live(st->proto) && !proto_done(st->proto) && phase == MURO_STEP_PH_DECIDE &&
      tag == EV_REPLY) {
    uint64_t bad_reply = ev.muro__as.ev.muro__f_3;
    reply = bad_reply == 0 ? 0 : 1;
    observe = 0;
  }
  admitted = proto_admit(st->proto, phase, tag, buf.n, status, reply, observe, half || keep);
  if (admitted != 1) {
    muro_http_reset();
    add_note(out, MURO_STEP_NOTE_ADMIT, phase, tag, 0);
    return 0;
  }
  st->st = step(st->st, ev);
  muro_http_reset();
  return 1;
}

static int span_is(MuroHttpSlice s, const char *lit) {
  size_t n = strlen(lit);
  if (s.n != n) return 0;
  if (n == 0) return 1;
  return memcmp(s.p, lit, n) == 0;
}

static void note_version(HttpStep *st, const MuroHttpRequest *req) {
  st->is_head = span_is(req->method, "HEAD");
  st->ver11 = span_is(req->version, "HTTP/1.1");
}

static int fill_bound(HttpStep *st, size_t *n) {
  uint64_t end = field(st->st, 11);
  if (end == 0 || end > (uint64_t)st->in_len || !st->in) return -1;
  *n = (size_t)end;
  return 0;
}

static int copy_fixed(HttpStep *st, const StepIn *in, int which) {
  char date[30];
  size_t got = 0;
  size_t n;
  copy_date(in, date);
  n = host_copy_literal(st->out, st->out_cap, which, (const uint8_t *)date, 29,
                        st->is_head ? 1u : 0u, &got);
  if (n == 0) return -1;
  st->out_len = n;
  st->out_off = 0;
  st->rendered = 1;
  st->resp_mode = 0;
  st->pull = NULL;
  return 0;
}

static int begin_reply(HttpStep *st, const StepIn *in, StepOut *out, const MuroHttpResponse *resp) {
  int half = (int)field(st->st, 1);
  int keep = (int)field(st->st, 2);
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
  int omit = st->is_head ? 1 : 0;
  copy_date(in, date);
  st->pull = NULL;
  st->pull_user = NULL;
  st->pull_left = 0;
  st->pull_done = 0;
  st->pull_until_end = 0;
  st->resp_mode = 0;
  if (resp->pull && resp->has_length) {
    n = host_copy_head(st->out, st->out_cap, (uint64_t)resp->status, reason, reason_n,
                       (const uint8_t *)date, 29, extra, extra_n, resp->content_length, 0, &got);
    if (!omit) {
      st->resp_mode = 1;
      st->pull = resp->pull;
      st->pull_user = resp->pull_user;
      st->pull_left = resp->content_length;
    }
  } else if (resp->pull) {
    bit = omit || !st->ver11 ? 1 : bit;
    if (omit || !st->ver11) {
      n = host_copy_delim(st->out, st->out_cap, (uint64_t)resp->status, reason, reason_n,
                          (const uint8_t *)date, 29, extra, extra_n, &got);
      if (!omit) {
        st->resp_mode = 1;
        st->pull = resp->pull;
        st->pull_user = resp->pull_user;
        st->pull_until_end = 1;
      }
    } else {
      n = host_copy_chunked(st->out, st->out_cap, (uint64_t)resp->status, reason, reason_n,
                            (const uint8_t *)date, 29, extra, extra_n, 0, &got);
      st->resp_mode = 2;
      st->pull = resp->pull;
      st->pull_user = resp->pull_user;
    }
  } else {
    n = host_copy_response(st->out, st->out_cap, (uint64_t)resp->status, reason, reason_n,
                           (const uint8_t *)date, 29, extra, extra_n, body, body_n, 0, (uint64_t)omit,
                           &got);
  }
  eff = (half || keep || bit) ? 1u : 0u;
  /* The book already ran with keep 0. A forced close has to be the
     value the book saw, so rebuild when eff is 1 and the first copy
     assumed keep-alive. */
  if (eff == 1 && n > 0 && !(resp->pull && !resp->has_length && (omit || !st->ver11))) {
    if (resp->pull && resp->has_length) {
      n = host_copy_head(st->out, st->out_cap, (uint64_t)resp->status, reason, reason_n,
                         (const uint8_t *)date, 29, extra, extra_n, resp->content_length, 1, &got);
    } else if (resp->pull && st->ver11 && !omit) {
      n = host_copy_chunked(st->out, st->out_cap, (uint64_t)resp->status, reason, reason_n,
                            (const uint8_t *)date, 29, extra, extra_n, 1, &got);
    } else if (!resp->pull) {
      n = host_copy_response(st->out, st->out_cap, (uint64_t)resp->status, reason, reason_n,
                             (const uint8_t *)date, 29, extra, extra_n, body, body_n, 1,
                             (uint64_t)omit, &got);
    }
  }
  if (n == 0 && resp->owe_status) {
    n = host_copy_literal(st->out, st->out_cap, 1, (const uint8_t *)date, 29, (uint64_t)omit, NULL);
    bit = 1;
    bad = 1;
    eff = 1;
    st->resp_mode = 0;
    st->pull = NULL;
    if (n > 0) add_note(out, MURO_STEP_NOTE_TOO_BIG_COMMIT, 0, 0, got);
  }
  if (n == 0) {
    add_note(out, MURO_STEP_NOTE_TOO_BIG, 0, 0, got);
    return -1;
  }
  st->out_len = n;
  st->out_off = 0;
  st->rendered = 1;
  st->owe = resp->owe_status ? 1 : 0;
  if (!step_ev(st, out, evReplyOf((uint64_t)bit, (uint64_t)bad))) return -1;
  return 0;
}

static int fill_pull(HttpStep *st) {
  uint8_t tmp[8192];
  int r;
  size_t cap;
  if (!st->pull || st->resp_mode == 0) return 0;
  if (st->resp_mode == 2) {
    r = st->pull(tmp, sizeof tmp, st->pull_user);
    if (r < 0) return -1;
    if (r == 0) {
      st->out_len = host_copy_last(st->out, st->out_cap, NULL);
      st->out_off = 0;
      st->pull_done = 1;
      st->resp_mode = 0;
      st->pull = NULL;
      return st->out_len > 0 ? 0 : -1;
    }
    st->out_len = host_copy_chunk(st->out, st->out_cap, tmp, (size_t)r, NULL);
    st->out_off = 0;
    return st->out_len > 0 ? 0 : -1;
  }
  if (!st->pull_until_end && st->pull_left == 0) {
    st->pull_done = 1;
    st->resp_mode = 0;
    st->pull = NULL;
    st->out_len = 0;
    st->out_off = 0;
    return 0;
  }
  cap = st->out_cap < sizeof tmp ? st->out_cap : sizeof tmp;
  if (!st->pull_until_end && st->pull_left < cap) cap = (size_t)st->pull_left;
  if (cap == 0) return -1;
  r = st->pull(st->out, cap, st->pull_user);
  if (r < 0) return -1;
  if (r == 0) {
    if (!st->pull_until_end && st->pull_left != 0) return -1;
    st->pull_done = 1;
    st->resp_mode = 0;
    st->pull = NULL;
    st->out_len = 0;
    st->out_off = 0;
    return 0;
  }
  if (!st->pull_until_end && (uint64_t)r > st->pull_left) return -1;
  if (!st->pull_until_end) st->pull_left -= (uint64_t)r;
  st->out_len = (size_t)r;
  st->out_off = 0;
  return 0;
}

static void consume(HttpStep *st, uint64_t end_off) {
  size_t n;
  if (end_off == 0 || end_off > (uint64_t)st->in_len) return;
  n = (size_t)end_off;
  memmove(st->in, st->in + n, st->in_len - n);
  st->in_len -= n;
}

static int which_of(int pol) {
  if (pol == 413) return 3;
  if (pol == 417) return 4;
  if (pol == 501) return 2;
  return 0;
}

static void note_head_buf(HttpStep *st, const uint8_t *buf, size_t n) {
  muro__slice sl;
  muro__slice m;
  muro__slice v;
  if (!buf || n == 0) return;
  sl.p = buf;
  sl.n = n;
  m = methodView(sl);
  v = versionView(sl);
  st->is_head = m.n == 4 && m.p && memcmp(m.p, "HEAD", 4) == 0;
  st->ver11 = v.n == 8 && v.p && memcmp(v.p, "HTTP/1.1", 8) == 0;
  muro_http_reset();
}

static void prepare_in(HttpStep *st, int fresh) {
  int phase = (int)field(st->st, 0);
  if (phase != MURO_STEP_PH_READ && phase != MURO_STEP_PH_IDLE) return;
  if (st->hin) host_in_reset(st->hin);
  if (fresh) {
    st->sent_100 = 0;
    st->cont_len = 0;
    st->cont_off = 0;
  }
  if (st->hin && st->in && st->in_len) host_in_capture(st->hin, st->in, st->in_len);
}

static int step_buf(HttpStep *st, StepOut *out) {
  if (!st->in) return 0;
  return step_ev(st, out, evBytes(muro__slice_of(st->in, st->in_len)));
}

static void arm_idle(HttpStep *st, const StepIn *in, StepOut *out, int timer) {
  if (timer) {
    if (in->deadline == 0) out->wake_at = in->now_ms + (uint64_t)st->idle_ms;
    else out->wake_at = in->deadline;
  } else {
    out->wake_at = 0;
  }
}

/* 1 when the interim is fully written. 0 when `out` holds the rest
   of it. -1 when the literal does not fit. */
static int park_interim(HttpStep *st, const StepIn *in, StepOut *out) {
  int timer;
  if (st->sent_100) return 1;
  if (st->cont_len == 0) {
    char date[30];
    size_t nlen;
    copy_date(in, date);
    nlen = host_copy_literal(st->cont, sizeof st->cont, 5, (const uint8_t *)date, 29, 0, NULL);
    if (nlen == 0 || nlen > sizeof st->cont) return -1;
    st->cont_len = nlen;
    st->cont_off = 0;
  }
  if (st->cont_off >= st->cont_len) {
    st->sent_100 = 1;
    st->cont_len = 0;
    st->cont_off = 0;
    return 1;
  }
  st->pend_kind = PEND_CONT;
  out->bytes = st->cont + st->cont_off;
  out->len = st->cont_len - st->cont_off;
  /* interim is only the first-byte stall. cont stays set until the
     last byte of this 100 Continue is accepted. */
  out->interim = st->cont_off == 0;
  out->cont = 1;
  /* The live read interest stays until the adapter parks. A 100
     Continue that the socket takes in one write must not clear it
     before the handler runs. */
  out->read_more = 0;
  out->write_more = 1;
  timer = (int)field(st->st, 5);
  arm_idle(st, in, out, timer);
  return 0;
}

static int reply_fixed(HttpStep *st, const StepIn *in, StepOut *out, int which) {
  if (copy_fixed(st, in, which) < 0) return -1;
  if (!step_ev(st, out, evReplyOf(1, 1))) return -1;
  return 0;
}

static void hand_status(HttpStep *st, const StepIn *in, StepOut *out, int which) {
  char date[30];
  size_t n;
  copy_date(in, date);
  n = host_copy_literal(st->spare, sizeof st->spare, which, (const uint8_t *)date, 29,
                        st->is_head ? 1u : 0u, NULL);
  st->spare_len = n;
  out->status = n ? st->spare : NULL;
  out->status_len = n;
  out->status_now = 1;
  out->close_conn = 1;
  out->wake_at = 0;
  st->owe = 0;
  st->out_off = 1;
  interests(st, out, 0, 0);
}

static int inject_next(HttpStep *st, StepOut *out) {
  uint8_t *q = NULL;
  size_t qn = 0;
  if (!st->hin || !host_in_take_next(st->hin, &q, &qn)) return 0;
  if (qn > st->in_cap || !st->in) {
    free(q);
    return -1;
  }
  memcpy(st->in, q, qn);
  st->in_len = qn;
  free(q);
  prepare_in(st, 1);
  return step_buf(st, out) ? 1 : -1;
}

static void park_out(HttpStep *st, const StepIn *in, StepOut *out) {
  (void)in;
  st->pend_kind = PEND_OUT;
  out->bytes = st->out + st->out_off;
  out->len = st->out_len - st->out_off;
  out->interim = 0;
  out->cont = 0;
  /* WRITE has the book timer off. The pre-split pump did not arm a
     deadline on a short response write. A deadline carried in from
     the previous idle or 100 wait is stale here. The body-wait
     deadline is armed by the adapter while a pull is on the stack,
     and this park runs only after that wait has returned. */
  out->wake_at = 0;
  interests(st, out, 0, 1);
}

static void close_out(HttpStep *st, StepOut *out) {
  out->close_conn = 1;
  out->bytes = NULL;
  out->len = 0;
  out->cont = 0;
  out->interim = 0;
  /* The call copied in->deadline into wake_at up front. A close
     must not leave that deadline armed. */
  out->wake_at = 0;
  interests(st, out, 0, 0);
}

/* 0 parked in `out`. -1 the adapter closes. */
static int pump(HttpStep *st, const StepIn *in, StepOut *out) {
  while (st->spins < PUMP_FUSE) {
    int phase = (int)field(st->st, 0);
    int action = (int)field(st->st, 4);
    int timer = (int)field(st->st, 5);
    uint64_t end_off = field(st->st, 11);
    uint64_t more = field(st->st, 12);
    st->spins += 1;
    /* A tick that shuts must close even when a 100 is still
       outstanding. Parking the interim first returns that span with
       the book timer off, so wake_at is 0 and the peer can sit
       forever. */
    if (phase == MURO_STEP_PH_CLOSED || action == ACT_CLOSE) {
      st->cont_len = 0;
      st->cont_off = 0;
      st->pend_kind = PEND_NONE;
      close_out(st, out);
      return 0;
    }
    if (st->cont_len > 0 && !st->sent_100) {
      int w100 = park_interim(st, in, out);
      if (w100 < 0) {
        close_out(st, out);
        return 0;
      }
      if (w100 == 0) return 0;
    }
    if (phase == MURO_STEP_PH_READ || phase == MURO_STEP_PH_IDLE) {
      st->rendered = 0;
      st->owe = 0;
      st->out_len = 0;
      st->out_off = 0;
      st->resp_mode = 0;
      st->pull = NULL;
    }
    if (action == ACT_HANDLER) {
      MuroHttpRequest req;
      MuroHttpResponse resp;
      size_t span = 0;
      int rc;
      int send100 = 0;
      int pol;
      int dec;
      int force = 0;
      int over = 0;
      if (fill_bound(st, &span) < 0 || !st->handler || !st->hin) {
        close_out(st, out);
        return 0;
      }
      note_head_buf(st, st->in, span);
      pol = host_in_policy(st->in, span, st->max_body, &send100);
      if (pol) {
        if (reply_fixed(st, in, out, which_of(pol)) < 0) {
          close_out(st, out);
          return 0;
        }
        continue;
      }
      if (send100 && !st->sent_100) {
        int w100 = park_interim(st, in, out);
        if (w100 < 0) {
          close_out(st, out);
          return 0;
        }
        if (w100 == 0) return 0;
      }
      dec = host_in_bind(st->hin, st->in, span, &req);
      if (dec) {
        if (reply_fixed(st, in, out, which_of(dec)) < 0) {
          close_out(st, out);
          return 0;
        }
        continue;
      }
      note_version(st, &req);
      memset(&resp, 0, sizeof resp);
      rc = st->handler(&req, &resp, st->handler_user);
      if (rc < 0) {
        if (resp.owe_status) {
          st->owe = 1;
          if (copy_fixed(st, in, 1) == 0) add_note(out, MURO_STEP_NOTE_HANDLER_500, 0, 0, 0);
        }
        close_out(st, out);
        return 0;
      }
      host_in_after(st->hin, resp.pull != NULL && !st->is_head, &force, &over);
      if (over) {
        if (reply_fixed(st, in, out, which_of(over)) < 0) {
          close_out(st, out);
          return 0;
        }
        continue;
      }
      if (force) resp.close_after = 1;
      if (begin_reply(st, in, out, &resp) < 0) {
        if (resp.owe_status) st->owe = 1;
        close_out(st, out);
        return 0;
      }
      continue;
    }
    if (action == ACT_WRITE) {
      if (st->read_more) *st->read_more = 0;
      out->read_more = 0;
      if (!st->rendered) {
        int origin = (int)field(st->st, 6);
        int which;
        note_head_buf(st, st->in, st->in_len);
        if (origin == 1) {
          int lim = limit_which(st->in, st->in_len);
          which = lim ? lim : 0;
        } else if (origin == 2) which = 2;
        else {
          close_out(st, out);
          return 0;
        }
        if (copy_fixed(st, in, which) < 0) {
          close_out(st, out);
          return 0;
        }
      }
      if (st->out_off < st->out_len) {
        park_out(st, in, out);
        return 0;
      }
      if (st->write_more) *st->write_more = 0;
      out->write_more = 0;
      if (st->resp_mode != 0) {
        /* Headers for this response are already written. A pull
           error, including a chunk past max_body, cannot be a 413.
           The adapter drops the connection. A chunked body then has
           no last-chunk. */
        if (fill_pull(st) < 0) {
          close_out(st, out);
          return 0;
        }
        if (st->out_len > 0) continue;
      }
      st->rendered = 0;
      st->out_len = 0;
      st->out_off = 0;
      if (!step_ev(st, out, evDone())) {
        close_out(st, out);
        return 0;
      }
      if (st->hin && st->hin->open) {
        close_out(st, out);
        return 0;
      }
      continue;
    }
    if (action == ACT_READ) {
      int injected;
      if (st->write_more) *st->write_more = 0;
      out->write_more = 0;
      if (more && end_off > 0 && end_off <= (uint64_t)st->in_len) {
        consume(st, end_off);
        interests(st, out, 0, 0);
        prepare_in(st, 1);
        if (!step_buf(st, out)) {
          close_out(st, out);
          return 0;
        }
        continue;
      }
      if (!more && end_off > 0 && end_off <= (uint64_t)st->in_len) consume(st, end_off);
      injected = inject_next(st, out);
      if (injected < 0) {
        close_out(st, out);
        return 0;
      }
      if (injected > 0) continue;
      if (phase == MURO_STEP_PH_READ && st->in && st->in_len > 0) {
        int send100 = 0;
        int pol = host_in_policy(st->in, st->in_len, st->max_body, &send100);
        if (pol) {
          note_head_buf(st, st->in, st->in_len);
          hand_status(st, in, out, which_of(pol));
          return 0;
        }
        if (send100 && !st->sent_100) {
          int w100 = park_interim(st, in, out);
          if (w100 < 0) {
            close_out(st, out);
            return 0;
          }
          if (w100 == 0) return 0;
        }
      }
      interests(st, out, 1, 0);
      arm_idle(st, in, out, timer);
      return 0;
    }
    interests(st, out, phase == MURO_STEP_PH_READ || phase == MURO_STEP_PH_IDLE, 0);
    out->wake_at = in->deadline;
    return 0;
  }
  add_note(out, MURO_STEP_NOTE_FUEL, 0, 0, 0);
  return -1;
}

static int take_fact(HttpStep *st, const StepIn *in, StepOut *out) {
  if (in->kind == MURO_STEP_ACCEPTED) {
    st->st = begin();
    st->proto = proto_begin();
    st->ver11 = 1;
    st->pend_kind = PEND_NONE;
    if (!st->proto) return -1;
    interests(st, out, 1, 0);
    out->wake_at = in->now_ms + (uint64_t)st->idle_ms;
    return 1;
  }
  if (in->kind == MURO_STEP_WROTE) {
    size_t n = in->n;
    if (st->pend_kind == PEND_CONT) {
      if (n > st->cont_len - st->cont_off) n = st->cont_len - st->cont_off;
      st->cont_off += n;
      if (st->cont_off >= st->cont_len) {
        st->sent_100 = 1;
        st->cont_len = 0;
        st->cont_off = 0;
      }
      st->pend_kind = PEND_NONE;
      return 0;
    }
    if (st->pend_kind == PEND_OUT) {
      if (n > st->out_len - st->out_off) n = st->out_len - st->out_off;
      st->out_off += n;
      if (st->out_off > 0) st->owe = 0;
      st->pend_kind = PEND_NONE;
      if (!step_ev(st, out, evProgress((uint64_t)n))) return -1;
      return 0;
    }
    return 0;
  }
  if (in->kind == MURO_STEP_WRITE_FAILED) {
    st->pend_kind = PEND_NONE;
    if (!step_ev(st, out, evError())) return -1;
    return 0;
  }
  if (in->kind == MURO_STEP_BYTES || in->kind == MURO_STEP_PEER_CLOSED) {
    prepare_in(st, in->empty_before);
    if (in->kind == MURO_STEP_PEER_CLOSED) {
      if (!step_ev(st, out, evEof(muro__slice_of(st->in ? st->in : (uint8_t *)"", st->in_len))))
        return -1;
    } else if (!step_buf(st, out)) {
      return -1;
    }
    return 0;
  }
  if (in->kind == MURO_STEP_NOW) {
    /* Drop a half-sent 100 before the image runs. An idle tick shuts,
       and the next pump must not hand the span out again. */
    st->cont_len = 0;
    st->cont_off = 0;
    st->pend_kind = PEND_NONE;
    if (!step_ev(st, out, evTick())) return -1;
    return 0;
  }
  return -1;
}

void muro_http_step_bind(HttpStep *st, uint8_t *in, size_t in_cap, uint8_t *out, size_t out_cap,
                         struct HostIn *hin, int *read_more, int *write_more, MuroHttpHandler handler,
                         void *user, uint64_t max_body, int idle_ms) {
  if (!st) return;
  memset(st, 0, sizeof *st);
  st->in = in;
  st->in_cap = in_cap;
  st->out = out;
  st->out_cap = out_cap;
  st->hin = hin;
  st->read_more = read_more;
  st->write_more = write_more;
  st->handler = handler;
  st->handler_user = user;
  st->max_body = max_body;
  st->idle_ms = idle_ms;
  st->ver11 = 1;
}

void muro_http_step_shutdown(HttpStep *st) {
  if (!st) return;
  proto_free(st->proto);
  st->proto = NULL;
}

static void mark_shut(HttpStep *st) { st->pend_kind = PEND_SHUT; }

int muro_http_step(HttpStep *st, const StepIn *in, StepOut *out) {
  int took;
  int rc;
  if (!st || !in || !out) return -1;
  memset(out, 0, sizeof *out);
  /* 413, 417, and 501 return close_conn from hand_status while the
     image is still in read. A later fact classified that buffer
     again and wrote a second response. -1 and phase closed end the
     same way. The adapter has already been told to close. */
  if (st->pend_kind == PEND_SHUT) {
    out->close_conn = 1;
    return 0;
  }
  out->wake_at = in->deadline;
  /* One call is one budget. A large body under a one-byte window is
     a long run of MURO_STEP_WROTE, and the fuse used to survive
     those calls. */
  st->spins = 0;
  took = take_fact(st, in, out);
  if (took < 0) {
    mark_shut(st);
    return -1;
  }
  if (took > 0) return 0;
  rc = pump(st, in, out);
  if (rc < 0 || out->close_conn || (int)field(st->st, 0) == MURO_STEP_PH_CLOSED) mark_shut(st);
  return rc;
}

int muro_http_step_phase(const HttpStep *st) {
  if (!st) return MURO_STEP_PH_CLOSED;
  return (int)field(st->st, 0);
}

size_t muro_http_step_in_len(const HttpStep *st) {
  if (!st) return 0;
  return st->in_len;
}

void muro_http_step_recv(const HttpStep *st, uint8_t **p, size_t *len, size_t *cap) {
  if (p) *p = st ? st->in : NULL;
  if (len) *len = st ? st->in_len : 0;
  if (cap) *cap = st ? st->in_cap : 0;
}

void muro_http_step_set_in_len(HttpStep *st, size_t len) {
  if (!st) return;
  st->in_len = len;
}

void muro_http_step_close_status(HttpStep *st, const uint8_t *date, size_t date_n,
                                 const uint8_t **msg, size_t *n, int *log_500) {
  char buf[30];
  size_t got = 0;
  if (n) *n = 0;
  if (msg) *msg = NULL;
  if (log_500) *log_500 = 0;
  if (!st || st->owe == 0 || st->out_off > 0) return;
  if (st->rendered && st->out && st->out_len > 0) {
    if (msg) *msg = st->out;
    if (n) *n = st->out_len;
    st->owe = 0;
    return;
  }
  if (date && date_n >= 29) {
    memcpy(buf, date, 29);
    buf[29] = 0;
  } else {
    memcpy(buf, "Thu, 01 Jan 1970 00:00:00 GMT", 30);
  }
  got = host_copy_literal(st->spare, sizeof st->spare, 1, (const uint8_t *)buf, 29,
                          st->is_head ? 1u : 0u, NULL);
  st->spare_len = got;
  st->owe = 0;
  if (got == 0) return;
  if (msg) *msg = st->spare;
  if (n) *n = got;
  if (log_500) *log_500 = 1;
}

void muro_http_step_set_phase(HttpStep *st, int phase) {
  if (!st) return;
  st->st.muro__as.conn.muro__f_0 = (uint64_t)phase;
}
