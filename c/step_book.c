/* Drive http_step_stepFact from the buffers and the handler the host
   already uses. One fact is one call. A parked span is rendered once
   and then advanced. cover 1 closes the connection and sets note
   MURO_BOOK_NOTE_COVER. A later fact on that step stays closed.
   An invariant failure uses MURO_BOOK_NOTE_STUCK, not the cover note.
   proto_admit runs on the same facts as c/step.c. */

#include "step_book.h"
#include "host_render.h"
#include "host_stream.h"
#include "proto.h"
#include "step_slice.h"

#include <string.h>

static void interests(BookStep *b, StepOut *out, int rd, int wr) {
  if (b->read_more) *b->read_more = rd;
  if (b->write_more) *b->write_more = wr;
  if (out) {
    out->read_more = rd;
    out->write_more = wr;
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

static int which_of(uint64_t code) {
  if (code == 413) return 3;
  if (code == 417) return 4;
  if (code == 501) return 2;
  if (code == 500) return 1;
  if (code == 414) return 6;
  if (code == 431) return 7;
  return 0;
}

static void note_head(BookStep *b) {
  muro__slice sl;
  muro__slice m;
  muro__slice v;
  if (!b->in || b->in_len == 0) return;
  sl = muro__slice_of(b->in, b->in_len);
  m = http_step_methodView(sl);
  v = http_step_versionView(sl);
  b->is_head = m.n == 4 && m.p && memcmp(m.p, "HEAD", 4) == 0;
  b->ver11 = v.n == 8 && v.p && memcmp(v.p, "HTTP/1.1", 8) == 0;
  muro_http_reset();
}

static void consume(BookStep *b, uint64_t end) {
  size_t n;
  if (!b->in || end == 0 || end > b->in_len) return;
  n = (size_t)end;
  memmove(b->in, b->in + n, b->in_len - n);
  b->in_len -= n;
}

static uint64_t out_field(http_step_Out o, int i) {
  switch (i) {
  case 0: return o.muro__as.out.muro__f_0;
  case 1: return o.muro__as.out.muro__f_1;
  case 2: return o.muro__as.out.muro__f_2;
  case 3: return o.muro__as.out.muro__f_3;
  case 4: return o.muro__as.out.muro__f_4;
  case 5: return o.muro__as.out.muro__f_5;
  case 6: return o.muro__as.out.muro__f_6;
  case 7: return o.muro__as.out.muro__f_7;
  case 8: return o.muro__as.out.muro__f_8;
  case 10: return o.muro__as.out.muro__f_10;
  case 9: return o.muro__as.out.muro__f_9;
  default: return 0;
  }
}

static http_step_St book_st(const BookStep *b) {
  return http_step_take(http_step_still(b->bphase, b->bcode, b->bleft, b->bkeep));
}

static void remember(BookStep *b, uint64_t phase, uint64_t code, uint64_t left, uint64_t keep) {
  b->bphase = phase;
  b->bcode = code;
  b->bleft = left;
  b->bkeep = keep;
}

static void image_of(BookStep *b, uint64_t phase, int status_now) {
  if (phase == 5) b->image = MURO_STEP_PH_DECIDE;
  else if (phase == 2 && status_now) b->image = MURO_STEP_PH_READ;
  else if (phase == 2) b->image = MURO_STEP_PH_CLOSED;
  else if (phase == 0 || phase == 7) b->image = MURO_STEP_PH_READ;
  else if (phase == 4) b->image = MURO_STEP_PH_IDLE;
  else if (phase == 1 || phase == 3) b->image = MURO_STEP_PH_WRITE;
  else b->image = MURO_STEP_PH_CLOSED;
}

/* 0 rendered into out or spare. -1 the book and the handler disagree,
   or the bytes did not fit. Sets *which_note. */
static int render_literal(BookStep *b, const StepIn *in, int into_spare, uint64_t code,
                          int *note) {
  char date[30];
  uint8_t *dst;
  size_t cap;
  size_t n;
  note_head(b);
  copy_date(in, date);
  if (into_spare) {
    dst = b->spare;
    cap = sizeof b->spare;
  } else {
    dst = b->out;
    cap = b->out_cap;
  }
  n = host_copy_literal(dst, cap, which_of(code), (const uint8_t *)date, 29, b->is_head ? 1u : 0u,
                        NULL);
  if (n == 0) {
    *note = MURO_STEP_NOTE_TOO_BIG;
    return -1;
  }
  if (into_spare) b->spare_len = n;
  else {
    b->out_len = n;
    b->out_off = 0;
    b->rendered = 1;
    b->owe = 0;
  }
  return 0;
}

/* Same place as c/step.c prepare_in: reset when the image is still
   reading or idle, then capture the bytes after a streamed header
   block. A complete buffered body leaves streamTake at 0, so capture
   copies nothing. empty_before clears a pending 100, and the book
   then classifies this buffer as a fresh read. */
static void prepare_in(BookStep *b, const StepIn *in) {
  int phase = b->img_phase;
  if (phase != MURO_STEP_PH_READ && phase != MURO_STEP_PH_IDLE) return;
  if (b->hin) host_in_reset(b->hin);
  if (in && in->empty_before && (b->bphase == 6 || b->bphase == 7 || b->bphase == 8)) {
    b->sent_100 = 0;
    b->cont_len = 0;
    b->cont_off = 0;
    b->bphase = 0;
    b->bcode = 0;
    b->bleft = 0;
    b->bkeep = 0;
  }
  if (b->hin && b->in && b->in_len) host_in_capture(b->hin, b->in, b->in_len);
}

/* 0 when out holds the interim. -1 when the literal does not fit.
   left 0 is the full span. A positive left is the unwritten tail.
   The image stays wherever admit left it: the 100 is not a reply. */
static int park_cont(BookStep *b, const StepIn *in, StepOut *out, uint64_t left, uint64_t wake) {
  if (b->cont_len == 0) {
    char date[30];
    size_t nlen;
    copy_date(in, date);
    nlen = host_copy_literal(b->cont, sizeof b->cont, 5, (const uint8_t *)date, 29, 0, NULL);
    if (nlen == 0 || nlen > sizeof b->cont) return -1;
    b->cont_len = nlen;
    b->cont_off = 0;
  }
  if (left > 0 && left <= (uint64_t)b->cont_len) b->cont_off = b->cont_len - (size_t)left;
  else b->cont_off = 0;
  if (b->cont_off > b->cont_len) return -1;
  out->bytes = b->cont + b->cont_off;
  out->len = b->cont_len - b->cont_off;
  out->interim = b->cont_off == 0;
  out->cont = 1;
  /* Not interests(): like park_interim in c/step.c, only the StepOut
     interests change here. The live read interest pointer stays set
     until the adapter parks, so a 100 Continue that the socket takes
     in one write does not clear it before the handler runs. */
  out->read_more = 0;
  out->write_more = 1;
  out->close_conn = 0;
  out->status_now = 0;
  out->wake_at = wake;
  b->sent_100 = 0;
  b->image = b->img_phase;
  return 0;
}

static void clear_pull(BookStep *b) {
  b->resp_mode = 0;
  b->pull = NULL;
  b->pull_user = NULL;
  b->pull_left = 0;
  b->pull_until_end = 0;
}

/* The header span is already written. Same continuation as c/step.c
   fill_pull: the next chunk, the last chunk, or a finished body.
   0 when out_len is the next span, or 0 because the body is done.
   -1 closes. The image stays in write either way. */
static int fill_pull(BookStep *b) {
  uint8_t tmp[8192];
  int r;
  size_t cap;
  if (!b->pull || b->resp_mode == 0) return 0;
  if (b->resp_mode == 2) {
    r = b->pull(tmp, sizeof tmp, b->pull_user);
    if (r < 0) return -1;
    if (r == 0) {
      b->out_len = host_copy_last(b->out, b->out_cap, NULL);
      b->out_off = 0;
      b->rendered = b->out_len > 0;
      clear_pull(b);
      return b->out_len > 0 ? 0 : -1;
    }
    b->out_len = host_copy_chunk(b->out, b->out_cap, tmp, (size_t)r, NULL);
    b->out_off = 0;
    b->rendered = b->out_len > 0;
    return b->out_len > 0 ? 0 : -1;
  }
  if (!b->pull_until_end && b->pull_left == 0) {
    clear_pull(b);
    b->out_len = 0;
    b->out_off = 0;
    return 0;
  }
  cap = b->out_cap < sizeof tmp ? b->out_cap : sizeof tmp;
  if (!b->pull_until_end && b->pull_left < cap) cap = (size_t)b->pull_left;
  if (cap == 0) return -1;
  r = b->pull(b->out, cap, b->pull_user);
  if (r < 0) return -1;
  if (r == 0) {
    if (!b->pull_until_end && b->pull_left != 0) return -1;
    clear_pull(b);
    b->out_len = 0;
    b->out_off = 0;
    return 0;
  }
  if (!b->pull_until_end && (uint64_t)r > b->pull_left) return -1;
  if (!b->pull_until_end) b->pull_left -= (uint64_t)r;
  b->out_len = (size_t)r;
  b->out_off = 0;
  b->rendered = 1;
  return 0;
}

static size_t copy_hand_span(BookStep *b, const StepIn *in, const MuroHttpResponse *resp,
                             uint64_t span, uint64_t keep) {
  char date[30];
  size_t got = 0;
  size_t n;
  int omit = b->is_head ? 1 : 0;
  const uint8_t *reason = resp->reason ? (const uint8_t *)resp->reason : NULL;
  const uint8_t *extra = resp->headers ? (const uint8_t *)resp->headers : NULL;
  const uint8_t *body = resp->body;
  size_t reason_n = reason ? resp->reason_len : 0;
  size_t extra_n = extra ? resp->headers_len : 0;
  size_t body_n = body ? resp->body_len : 0;
  copy_date(in, date);
  clear_pull(b);
  if (span == 3) {
    n = host_copy_head(b->out, b->out_cap, (uint64_t)resp->status, reason, reason_n,
                       (const uint8_t *)date, 29, extra, extra_n, resp->content_length, keep, &got);
    if (n > 0 && !omit) {
      b->resp_mode = 1;
      b->pull = resp->pull;
      b->pull_user = resp->pull_user;
      b->pull_left = resp->content_length;
    }
  } else if (span == 4) {
    n = host_copy_chunked(b->out, b->out_cap, (uint64_t)resp->status, reason, reason_n,
                          (const uint8_t *)date, 29, extra, extra_n, keep, &got);
    if (n > 0) {
      b->resp_mode = 2;
      b->pull = resp->pull;
      b->pull_user = resp->pull_user;
    }
  } else if (span == 5) {
    n = host_copy_delim(b->out, b->out_cap, (uint64_t)resp->status, reason, reason_n,
                        (const uint8_t *)date, 29, extra, extra_n, &got);
    if (n > 0 && !omit) {
      b->resp_mode = 1;
      b->pull = resp->pull;
      b->pull_user = resp->pull_user;
      b->pull_until_end = 1;
    }
  } else {
    n = host_copy_response(b->out, b->out_cap, (uint64_t)resp->status, reason, reason_n,
                           (const uint8_t *)date, 29, extra, extra_n, body, body_n, keep,
                           (uint64_t)omit, &got);
  }
  return n;
}

/* http_step_hand decides close_after, a 4xx/5xx, a refused handler,
   a pulled body, and a buffered body that does not fit. 0 parks the
   span hand named. 2 is phase 5: the image stays in decide, the
   span is not parked, and the caller shuts. -1 is an invariant
   failure (MURO_BOOK_NOTE_STUCK). */
static int render_handler(BookStep *b, const StepIn *in, uint64_t book_keep, int *note) {
  MuroHttpRequest req;
  MuroHttpResponse resp;
  int dec;
  int rc;
  int over = 0;
  int force = 0;
  int fit = 1;
  int href;
  uint64_t hclose;
  uint64_t hbad;
  uint64_t howe;
  uint64_t hpull;
  uint64_t hver;
  uint64_t hhead;
  if (!b->hin || !b->handler || !b->in) return -1;
  /* prepare_in already ran for a read or idle fact. Resetting again
     would drop that capture, so capture once more after this reset.
     A pipelined tail is rendered in the same call, after consume. */
  host_in_reset(b->hin);
  if (b->in && b->in_len) host_in_capture(b->hin, b->in, b->in_len);
  dec = host_in_bind(b->hin, b->in, b->in_len, &req);
  if (dec) return -1;
  b->is_head = req.method.n == 4 && req.method.p && memcmp(req.method.p, "HEAD", 4) == 0;
  b->ver11 = req.version.n == 8 && req.version.p && memcmp(req.version.p, "HTTP/1.1", 8) == 0;
  memset(&resp, 0, sizeof resp);
  rc = b->handler(&req, &resp, b->handler_user);
  href = rc < 0 ? 1 : 0;
  /* c/step.c checks a refusal before host_in_after and never runs it
     on a refused handler. */
  if (!href) {
    host_in_after(b->hin, resp.pull != NULL && !b->is_head, &force, &over);
    /* 400 and 413 are an owed span. overStatus leaves every other
       code outside, and that stays the stuck note. A refusal never
       reaches this call. */
    if (over) {
      http_step_Out ob;
      uint64_t ophase;
      uint64_t ocode;
      uint64_t ocover;
      muro_http_reset();
      ob = http_step_overStatus((uint64_t)over);
      muro_http_reset();
      ophase = out_field(ob, 0);
      ocode = out_field(ob, 1);
      ocover = out_field(ob, 7);
      if (ocover || ophase != 1) return -1;
      clear_pull(b);
      if (render_literal(b, in, 0, ocode, note) < 0) return -1;
      remember(b, 1, ocode, 0, 1);
      b->reply_on = 1;
      b->reply_bit = 1;
      b->reply_bad = 1;
      b->owe = 0;
      return 0;
    }
    if (force) resp.close_after = 1;
  }
  hclose = resp.close_after ? 1u : 0u;
  hbad = (resp.status >= 400 && resp.status <= 599) ? 1u : 0u;
  howe = resp.owe_status ? 1u : 0u;
  hpull = (!href && resp.pull) ? (resp.has_length ? 1u : 2u) : 0u;
  hver = b->ver11 ? 1u : 0u;
  hhead = b->is_head ? 1u : 0u;
  for (;;) {
    http_step_Out bk;
    uint64_t phase;
    uint64_t code;
    uint64_t span;
    uint64_t keep;
    size_t n;
    muro_http_reset();
    bk = http_step_hand(book_keep, hclose, hbad, howe, (uint64_t)href, hpull, hver, hhead,
                        (uint64_t)fit);
    muro_http_reset();
    phase = out_field(bk, 0);
    code = out_field(bk, 1);
    span = out_field(bk, 6);
    keep = out_field(bk, 10);
    if (phase == 5) {
      clear_pull(b);
      remember(b, 5, code, 0, 1);
      b->out_len = 0;
      b->out_off = 0;
      b->rendered = 0;
      b->owe = 0;
      *note = 0;
      if (code == 500) {
        if (render_literal(b, in, 0, 500, note) == 0) {
          *note = MURO_STEP_NOTE_HANDLER_500;
          b->owe = 1;
        } else {
          *note = 0;
          b->rendered = 0;
          b->out_len = 0;
        }
      } else if (!fit && !href) {
        *note = MURO_STEP_NOTE_TOO_BIG;
      }
      return 2;
    }
    if (phase == 3 && code == 500) {
      clear_pull(b);
      if (render_literal(b, in, 0, 500, note) < 0) {
        *note = MURO_STEP_NOTE_TOO_BIG;
        remember(b, 5, 0, 0, 1);
        b->rendered = 0;
        b->owe = howe ? 1 : 0;
        return 2;
      }
      *note = MURO_STEP_NOTE_TOO_BIG_COMMIT;
      b->owe = 1;
      remember(b, 3, 500, 0, 1);
      b->reply_on = 1;
      b->reply_bit = 1;
      b->reply_bad = 1;
      return 0;
    }
    if (phase != 3 || (span != 2 && span != 3 && span != 4 && span != 5)) return -1;
    n = copy_hand_span(b, in, &resp, span, keep);
    if (n > 0) {
      b->out_len = n;
      b->out_off = 0;
      b->rendered = 1;
      b->owe = resp.owe_status ? 1 : 0;
      remember(b, 3, 0, 0, keep);
      b->reply_on = 1;
      /* Span 5 is close-delimited: HEAD or not HTTP/1.1. c/step.c
         forces the reply bit, so the image keep becomes 1. */
      b->reply_bit = (hclose || hbad || span == 5) ? 1 : 0;
      b->reply_bad = hbad ? 1 : 0;
      return 0;
    }
    clear_pull(b);
    if (!fit) {
      *note = MURO_STEP_NOTE_TOO_BIG;
      remember(b, 5, 0, 0, 1);
      b->rendered = 0;
      b->out_len = 0;
      b->owe = 0;
      return 2;
    }
    /* The buffered body, or a pull header, did not fit. hand with
       hfit 0 chooses the committed 500 span or a decide-shut. */
    fit = 0;
    href = 0;
    hpull = 0;
  }
}

static void park(BookStep *b, StepOut *out) {
  out->bytes = b->out + b->out_off;
  out->len = b->out_len - b->out_off;
  out->interim = 0;
  out->cont = 0;
}

enum {
  BK_EV_BYTES = 0,
  BK_EV_EOF = 1,
  BK_EV_PROGRESS = 2,
  BK_EV_DONE = 3,
  BK_EV_ERROR = 4,
  BK_EV_TICK = 5,
  BK_EV_REPLY = 6
};

static void note_admit(StepOut *out) {
  if (!out) return;
  if (!out->note) out->note = MURO_STEP_NOTE_ADMIT;
  else if (!out->note2) out->note2 = MURO_STEP_NOTE_ADMIT;
}

/* 1 accepted, or no session yet (a direct step that never accepted).
   0 refused. The caller matches c/step.c: -1 for a fact admit, and
   close_conn 0 for a reply or a done. */
static int admit_at(BookStep *b, StepOut *out, int tag, size_t buf_n, int status, int reply) {
  int admitted;
  int phase;
  int close_after;
  if (!b->proto) return 1;
  phase = b->img_phase;
  close_after = (b->img_half || b->img_keep) ? 1 : 0;
  if (!(proto_live(b->proto) && !proto_done(b->proto) &&
        (phase == MURO_STEP_PH_READ || phase == MURO_STEP_PH_IDLE) &&
        (tag == BK_EV_BYTES || (tag == BK_EV_EOF && buf_n > 0))))
    status = -1;
  if (!(proto_live(b->proto) && !proto_done(b->proto) && phase == MURO_STEP_PH_DECIDE &&
        tag == BK_EV_REPLY))
    reply = -1;
  admitted = proto_admit(b->proto, phase, tag, buf_n, status, reply, 0, close_after);
  muro_http_reset();
  if (admitted != 1) {
    note_admit(out);
    return 0;
  }
  return 1;
}

static void read_class(const uint8_t *p, size_t n, int *status, int *keep_no) {
  muro__slice sl;
  Classified *c;
  sl = muro__slice_of(p ? p : (const uint8_t *)"", n);
  c = classify(sl);
  *status = (int)c->muro__as.classified.muro__f_0->muro__field_tag;
  *keep_no = (int)c->muro__as.classified.muro__f_1->muro__field_tag;
  muro_http_reset();
}

/* The image transition conn.step would make for this event. Admit
   has already used the phase from before the transition. */
static void apply_class(BookStep *b, int fin, int status, int keep_no, size_t n) {
  int phase = b->img_phase;
  if (phase == MURO_STEP_PH_WRITE || phase == MURO_STEP_PH_DECIDE) {
    if (fin) {
      b->img_half = 1;
      b->img_keep = 1;
    }
    return;
  }
  if (phase != MURO_STEP_PH_READ && phase != MURO_STEP_PH_IDLE) return;
  if (fin && n == 0) {
    b->img_phase = MURO_STEP_PH_CLOSED;
    b->img_half = 0;
    b->img_keep = 1;
    return;
  }
  if (status == 0) {
    b->img_phase = MURO_STEP_PH_DECIDE;
    b->img_half = fin ? 1 : 0;
    b->img_keep = fin ? 1 : (keep_no ? 1 : 0);
    return;
  }
  if (status == 2 && !fin) {
    b->img_phase = MURO_STEP_PH_READ;
    b->img_half = 0;
    b->img_keep = 0;
    return;
  }
  b->img_phase = MURO_STEP_PH_WRITE;
  b->img_half = fin ? 1 : 0;
  b->img_keep = 1;
}

static void apply_reply(BookStep *b) {
  int nk;
  if (b->reply_bad) nk = 1;
  else if (b->img_half || b->reply_bit) nk = 1;
  else nk = b->img_keep;
  b->img_phase = MURO_STEP_PH_WRITE;
  b->img_keep = nk;
}

static void apply_done(BookStep *b) {
  if (b->img_half || b->img_keep) {
    b->img_phase = MURO_STEP_PH_CLOSED;
    b->img_half = 0;
    b->img_keep = 1;
  } else {
    b->img_phase = MURO_STEP_PH_IDLE;
    b->img_half = 0;
    b->img_keep = 0;
  }
}

static int admit_refused(BookStep *b, StepOut *out, const StepIn *in, int saved_image) {
  b->shut = 1;
  b->image = saved_image;
  memset(out, 0, sizeof *out);
  out->note = MURO_STEP_NOTE_ADMIT;
  out->wake_at = in ? in->deadline : 0;
  interests(b, out, 0, 0);
  return -1;
}

static int admit_close(BookStep *b, StepOut *out, int image) {
  int n1 = out->note;
  int n2 = out->note2;
  memset(out, 0, sizeof *out);
  out->note = n1;
  out->note2 = n2;
  note_admit(out);
  out->close_conn = 1;
  b->shut = 1;
  b->image = image;
  interests(b, out, 0, 0);
  return 0;
}

static int closed_out(BookStep *b, StepOut *out, int note, int blind) {
  memset(out, 0, sizeof *out);
  out->close_conn = 1;
  out->note = note;
  out->wake_at = 0;
  b->shut = 1;
  b->image = MURO_STEP_PH_CLOSED;
  if (blind) b->blind = 1;
  interests(b, out, 0, 0);
  return 0;
}

void muro_book_bind(BookStep *st, uint8_t *in, size_t in_cap, uint8_t *out, size_t out_cap,
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
  st->image = MURO_STEP_PH_READ;
}

void muro_book_shutdown(BookStep *st) {
  if (!st) return;
  proto_free(st->proto);
  st->proto = NULL;
  st->hin = NULL;
  st->handler = NULL;
}

int muro_book_phase(const BookStep *st) {
  if (!st) return MURO_STEP_PH_CLOSED;
  return st->image;
}

size_t muro_book_in_len(const BookStep *st) {
  if (!st) return 0;
  return st->in_len;
}

void muro_book_set_in_len(BookStep *st, size_t len) {
  if (!st) return;
  st->in_len = len;
}

/* 1 when the bytes or the eof were admitted. -1 when the host would
   return -1. Classifies only from read or idle, matching step_ev. */
static int admit_buf(BookStep *b, StepOut *out, const StepIn *in, int tag, int fin, int saved) {
  size_t bn = b->in_len;
  int status = -1;
  int keep_no = 0;
  if ((b->img_phase == MURO_STEP_PH_READ || b->img_phase == MURO_STEP_PH_IDLE) &&
      (tag == BK_EV_BYTES || (tag == BK_EV_EOF && bn > 0))) {
    read_class(b->in, bn, &status, &keep_no);
    b->saw_status = status;
  }
  if (!admit_at(b, out, tag, bn, status, -1)) return admit_refused(b, out, in, saved);
  apply_class(b, fin, status, keep_no, bn);
  return 1;
}

int muro_book_step(BookStep *st, const StepIn *in, StepOut *out) {
  muro__slice sl;
  http_step_Out bk;
  uint64_t owed;
  uint64_t prev_phase, prev_code, prev_left, prev_keep;
  uint64_t phase, code, close_b, status_b, rd, wr, span, cover, left, wake, keep;
  uint64_t fin_end = 0;
  int finishing = 0;
  int fresh = 0;
  int note = 0;
  int rendered_rc;
  int saved_image;
  int did_progress = 0;
  int born = 0;
  if (!st || !in || !out) return -1;
  memset(out, 0, sizeof *out);
  out->wake_at = in->deadline;
  st->last_cover = 0;
  st->reply_on = 0;
  st->saw_status = -1;
  st->img_phase = st->image;
  if (st->shut) {
    out->close_conn = 1;
    out->wake_at = 0;
    interests(st, out, 0, 0);
    return 0;
  }
  saved_image = st->image;
  if (in->kind == MURO_STEP_ACCEPTED) {
    proto_free(st->proto);
    st->proto = proto_begin();
    st->img_phase = MURO_STEP_PH_READ;
    st->img_half = 0;
    st->img_keep = 0;
    st->ver11 = 1;
    st->image = MURO_STEP_PH_READ;
    saved_image = MURO_STEP_PH_READ;
    born = 1;
  }
  owed = (st->rendered && st->out_len > st->out_off) ? (uint64_t)(st->out_len - st->out_off) : 0;
  prev_phase = st->bphase;
  prev_code = st->bcode;
  prev_left = st->bleft;
  prev_keep = st->bkeep;
  if ((in->kind == MURO_STEP_BYTES || in->kind == MURO_STEP_PEER_CLOSED) && !st->shut)
    prepare_in(st, in);
  /* Progress is take_fact, before the pump fills another pull span.
     A refused progress is -1 and does not move the image. */
  if (in->kind == MURO_STEP_WROTE && (prev_phase == 1 || prev_phase == 3)) {
    if (!admit_at(st, out, BK_EV_PROGRESS, 0, -1, -1))
      return admit_refused(st, out, in, saved_image);
    did_progress = 1;
  }
  /* A pulled body stays in book phase 3 until the pull returns 0.
     The header-finishing write, and each chunk write, parks the next
     span here and does not ask stepFact to finish the response. */
  if (st->resp_mode != 0 && in->kind == MURO_STEP_WROTE && (prev_phase == 1 || prev_phase == 3)) {
    uint64_t base = prev_left == 0 ? owed : prev_left;
    if ((uint64_t)in->n >= base) {
      int fr = fill_pull(st);
      if (fr < 0) {
        clear_pull(st);
        st->shut = 1;
        st->image = MURO_STEP_PH_WRITE;
        out->close_conn = 1;
        out->wake_at = 0;
        interests(st, out, 0, 0);
        return 0;
      }
      if (st->out_len > 0) {
        remember(st, prev_phase, prev_code, 0, prev_keep);
        st->image = MURO_STEP_PH_WRITE;
        st->rendered = 1;
        out->wake_at = 0;
        interests(st, out, 0, 1);
        park(st, out);
        return 0;
      }
    }
  }
  sl = muro__slice_of(st->in ? st->in : (const uint8_t *)"", st->in_len);
  /* The 100 is not in the response buffer, so the out-buffer owed
     is 0 and every wrote would finish it. The remaining interim
     bytes are the base. prepare_in has already cleared them when
     empty_before rewinds phase 6, 7, or 8 to a fresh read. */
  if ((st->bphase == 6 || st->bphase == 8) && st->cont_len > st->cont_off)
    owed = (uint64_t)(st->cont_len - st->cont_off);
  muro_http_reset();
  bk = http_step_stepFact(book_st(st), (uint64_t)in->kind, sl, st->max_body, (uint64_t)in->n, owed,
                          in->now_ms, in->deadline, (uint64_t)st->idle_ms);
  phase = out_field(bk, 0);
  code = out_field(bk, 1);
  close_b = out_field(bk, 2);
  status_b = out_field(bk, 3);
  rd = out_field(bk, 4);
  wr = out_field(bk, 5);
  span = out_field(bk, 6);
  cover = out_field(bk, 7);
  left = out_field(bk, 8);
  wake = out_field(bk, 9);
  keep = out_field(bk, 10);
  if ((prev_phase == 1 || prev_phase == 3) && in->kind == MURO_STEP_WROTE) {
    uint64_t base = prev_left == 0 ? owed : prev_left;
    if ((uint64_t)in->n >= base) finishing = 1;
    if (finishing && prev_keep == 0) fin_end = http_step_framedEnd(sl);
    muro_http_reset();
  } else {
    muro_http_reset();
  }
  remember(st, phase, code, left, keep);
  if (cover) {
    st->last_cover = 1;
    return closed_out(st, out, MURO_BOOK_NOTE_COVER, 1);
  }
  if (born && !st->proto) return admit_refused(st, out, in, saved_image);
  if (in->kind == MURO_STEP_BYTES) {
    int arc = admit_buf(st, out, in, BK_EV_BYTES, 0, saved_image);
    if (arc < 0) return arc;
  } else if (in->kind == MURO_STEP_PEER_CLOSED) {
    int arc = admit_buf(st, out, in, BK_EV_EOF, 1, saved_image);
    if (arc < 0) return arc;
  } else if (in->kind == MURO_STEP_NOW) {
    if (!admit_at(st, out, BK_EV_TICK, 0, -1, -1)) return admit_refused(st, out, in, saved_image);
    if (st->img_phase == MURO_STEP_PH_READ || st->img_phase == MURO_STEP_PH_IDLE) {
      st->img_phase = MURO_STEP_PH_CLOSED;
      st->img_half = 0;
      st->img_keep = 1;
    }
  } else if (in->kind == MURO_STEP_WRITE_FAILED) {
    if (!admit_at(st, out, BK_EV_ERROR, 0, -1, -1)) return admit_refused(st, out, in, saved_image);
    if (st->img_phase == MURO_STEP_PH_WRITE) {
      st->img_phase = MURO_STEP_PH_CLOSED;
      st->img_half = 0;
      st->img_keep = 1;
    }
  }
  /* Done is admitted before the request is consumed. A refusal closes
     with the image still in write. A keep-alive tail is then bytes
     at idle, and maybe a reply, in this same call. */
  if (finishing && did_progress) {
    int half = st->img_half;
    int kbit = st->img_keep;
    if (!admit_at(st, out, BK_EV_DONE, 0, -1, -1))
      return admit_close(st, out, MURO_STEP_PH_WRITE);
    apply_done(st);
    if (!half && !kbit && prev_keep == 0) {
      int status = -1;
      int keep_no = 0;
      consume(st, fin_end);
      /* The done already landed. A refused tail is close_out, not -1,
         and the image stays idle. */
      if (st->in_len > 0) {
        read_class(st->in, st->in_len, &status, &keep_no);
        st->saw_status = status;
        if (!admit_at(st, out, BK_EV_BYTES, st->in_len, status, -1))
          return admit_close(st, out, st->img_phase);
        apply_class(st, 0, status, keep_no, st->in_len);
      }
    }
  }
  /* Span 6 is the interim. Admit already moved the image. Do not
     map book phase 6 onto that image, and do not admit a reply. */
  if (span == 6) {
    if (park_cont(st, in, out, left, wake) < 0) return closed_out(st, out, MURO_STEP_NOTE_TOO_BIG, 0);
    return 0;
  }
  st->cont_len = 0;
  st->cont_off = 0;
  if (phase == 7) st->sent_100 = 1;
  image_of(st, phase, status_b == 1);
  out->wake_at = wake;
  out->close_conn = close_b ? 1 : 0;
  out->status_now = status_b ? 1 : 0;
  interests(st, out, rd ? 1 : 0, wr ? 1 : 0);
  if (close_b || status_b) st->shut = 1;
  if (phase == 0 || phase == 4 || phase == 7) {
    clear_pull(st);
    st->rendered = 0;
    st->owe = 0;
    st->out_len = 0;
    st->out_off = 0;
    return 0;
  }
  if (status_b) {
    if (render_literal(st, in, 1, code, &note) < 0) return closed_out(st, out, note, 0);
    out->status = st->spare_len ? st->spare : NULL;
    out->status_len = st->spare_len;
    out->status_now = 1;
    out->close_conn = 1;
    out->wake_at = 0;
    interests(st, out, 0, 0);
    return 0;
  }
  if (!(wr && span == 2)) return 0;
  fresh = finishing || !st->rendered || st->out_off >= st->out_len;
  if (!fresh) {
    if (in->kind == MURO_STEP_WROTE) {
      size_t adv = (size_t)in->n;
      size_t rem = st->out_len - st->out_off;
      if (adv > rem) adv = rem;
      st->out_off += adv;
    }
    if (st->out_off >= st->out_len) return closed_out(st, out, MURO_BOOK_NOTE_STUCK, 0);
    park(st, out);
    return 0;
  }
  st->rendered = 0;
  st->out_len = 0;
  st->out_off = 0;
  if (phase == 3 && code == 0) rendered_rc = render_handler(st, in, keep, &note);
  else if (phase == 1) rendered_rc = render_literal(st, in, 0, code, &note);
  else return closed_out(st, out, MURO_BOOK_NOTE_STUCK, 0);
  /* Phase 5 shuts in decide. The 500 literal stays in the send
     buffer for close_status and is not a parked span. */
  if (rendered_rc == 2) {
    image_of(st, st->bphase, 0);
    st->shut = 1;
    out->close_conn = 1;
    out->note = note;
    out->wake_at = 0;
    out->status_now = 0;
    out->bytes = NULL;
    out->len = 0;
    interests(st, out, 0, 0);
    return 0;
  }
  if (rendered_rc < 0) return closed_out(st, out, note ? note : MURO_BOOK_NOTE_STUCK, 0);
  /* A complete request the policy rejects is status-ok, then
     reply_fixed. Malformed and unimplemented owes do not reply. */
  if (phase == 1 && st->saw_status == 0) {
    st->reply_on = 1;
    st->reply_bit = 1;
    st->reply_bad = 1;
  }
  if (note) out->note = note;
  if (st->reply_on) {
    if (!admit_at(st, out, BK_EV_REPLY, 0, -1, st->reply_bad))
      return admit_close(st, out, MURO_STEP_PH_DECIDE);
    apply_reply(st);
  }
  image_of(st, st->bphase, 0);
  if (!st->rendered || st->out_off >= st->out_len) return closed_out(st, out, MURO_BOOK_NOTE_STUCK, 0);
  park(st, out);
  (void)prev_code;
  return 0;
}

#ifdef MURO_STEP_BOOK

/* Mirrors PEND_SHUT = 3 in c/step.c (and TEST_PEND_SHUT in c/test_step_slice.c). */
#define BOOK_PEND_SHUT 3

static void load_http(BookStep *b, HttpStep *st) {
  memset(b, 0, sizeof *b);
  b->read_more = st->read_more;
  b->write_more = st->write_more;
  b->in = st->in;
  b->in_len = st->in_len;
  b->in_cap = st->in_cap;
  b->out = st->out;
  b->out_cap = st->out_cap;
  b->out_len = st->out_len;
  b->out_off = st->out_off;
  b->rendered = st->rendered;
  b->owe = st->owe;
  b->shut = st->pend_kind == BOOK_PEND_SHUT;
  b->blind = st->pull_done;
  b->image = (int)st->st.muro__as.conn.muro__f_0;
  b->is_head = st->is_head;
  b->ver11 = st->ver11;
  b->resp_mode = st->resp_mode;
  b->pull_until_end = st->pull_until_end;
  b->pull = st->pull;
  b->pull_user = st->pull_user;
  b->pull_left = st->pull_left;
  b->proto = st->proto;
  b->img_half = (int)st->st.muro__as.conn.muro__f_5;
  b->img_keep = (int)st->st.muro__as.conn.muro__f_6;
  b->bphase = st->st.muro__as.conn.muro__f_1;
  b->bcode = st->st.muro__as.conn.muro__f_2;
  b->bleft = st->st.muro__as.conn.muro__f_3;
  b->bkeep = st->st.muro__as.conn.muro__f_4;
  b->hin = st->hin;
  b->handler = st->handler;
  b->handler_user = st->handler_user;
  b->max_body = st->max_body;
  b->idle_ms = st->idle_ms;
  b->spare_len = st->spare_len;
  if (st->spare_len > sizeof b->spare) b->spare_len = sizeof b->spare;
  if (b->spare_len) memcpy(b->spare, st->spare, b->spare_len);
  b->sent_100 = st->sent_100;
  b->cont_len = st->cont_len;
  b->cont_off = st->cont_off;
  if (b->cont_len > sizeof b->cont) b->cont_len = sizeof b->cont;
  if (b->cont_off > b->cont_len) b->cont_off = b->cont_len;
  if (b->cont_len) memcpy(b->cont, st->cont, b->cont_len);
}

static void store_http(HttpStep *st, const BookStep *b, StepOut *out) {
  st->in_len = b->in_len;
  st->out_len = b->out_len;
  st->out_off = b->out_off;
  st->rendered = b->rendered;
  st->owe = b->owe;
  st->pend_kind = b->shut ? BOOK_PEND_SHUT : 0;
  st->pull_done = b->blind;
  st->is_head = b->is_head;
  st->ver11 = b->ver11;
  st->resp_mode = b->resp_mode;
  st->pull_until_end = b->pull_until_end;
  st->pull = b->pull;
  st->pull_user = b->pull_user;
  st->pull_left = b->pull_left;
  st->proto = b->proto;
  st->spare_len = b->spare_len;
  st->st.muro__as.conn.muro__f_0 = (uint64_t)b->image;
  st->st.muro__as.conn.muro__f_5 = (uint64_t)b->img_half;
  st->st.muro__as.conn.muro__f_6 = (uint64_t)b->img_keep;
  st->st.muro__as.conn.muro__f_1 = b->bphase;
  st->st.muro__as.conn.muro__f_2 = b->bcode;
  st->st.muro__as.conn.muro__f_3 = b->bleft;
  st->st.muro__as.conn.muro__f_4 = b->bkeep;
  if (b->spare_len && b->spare_len <= sizeof st->spare) memcpy(st->spare, b->spare, b->spare_len);
  st->sent_100 = b->sent_100;
  st->cont_len = b->cont_len;
  st->cont_off = b->cont_off;
  if (st->cont_len > sizeof st->cont) st->cont_len = sizeof st->cont;
  if (st->cont_len) memcpy(st->cont, b->cont, st->cont_len);
  if (out && out->status && b->spare_len && out->status >= b->spare &&
      out->status < b->spare + sizeof b->spare) {
    out->status = st->spare + (size_t)(out->status - b->spare);
  }
  if (out && out->bytes && b->cont_len && out->bytes >= b->cont &&
      out->bytes < b->cont + sizeof b->cont) {
    out->bytes = st->cont + (size_t)(out->bytes - b->cont);
  }
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
}

void muro_http_step_shutdown(HttpStep *st) {
  if (!st) return;
  proto_free(st->proto);
  st->proto = NULL;
}

int muro_http_step(HttpStep *st, const StepIn *in, StepOut *out) {
  BookStep b;
  int rc;
  if (!st || !in || !out) return -1;
  load_http(&b, st);
  rc = muro_book_step(&b, in, out);
  store_http(st, &b, out);
  return rc;
}

int muro_http_step_phase(const HttpStep *st) {
  if (!st) return MURO_STEP_PH_CLOSED;
  return (int)st->st.muro__as.conn.muro__f_0;
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

#endif
