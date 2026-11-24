/* Drive http_step_stepFact from the buffers and the handler the host
   already uses. One fact is one call. A parked span is rendered once
   and then advanced. cover 1 closes the connection and sets note
   MURO_BOOK_NOTE_COVER. A later fact on that step stays closed. */

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
  if (!b->in || b->in_len == 0) return;
  sl = muro__slice_of(b->in, b->in_len);
  m = http_step_methodView(sl);
  b->is_head = m.n == 4 && m.p && memcmp(m.p, "HEAD", 4) == 0;
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
  if (phase == 2 && status_now) b->image = MURO_STEP_PH_READ;
  else if (phase == 2) b->image = MURO_STEP_PH_CLOSED;
  else if (phase == 0) b->image = MURO_STEP_PH_READ;
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

/* 0 when out holds the handler span. -1 outside the book (pull, a
   forced close the book did not already say, bind status). -2 when
   the handler refused and owe_status asked for the 500 literal. */
static int render_handler(BookStep *b, const StepIn *in, uint64_t book_keep, int *note) {
  MuroHttpRequest req;
  MuroHttpResponse resp;
  int dec;
  int rc;
  int force = 0;
  int over = 0;
  int keep;
  int omit;
  char date[30];
  size_t n;
  size_t got = 0;
  const uint8_t *reason;
  const uint8_t *extra;
  const uint8_t *body;
  if (!b->hin || !b->handler || !b->in) return -1;
  host_in_reset(b->hin);
  dec = host_in_bind(b->hin, b->in, b->in_len, &req);
  if (dec) return -1;
  note_head(b);
  memset(&resp, 0, sizeof resp);
  rc = b->handler(&req, &resp, b->handler_user);
  if (rc < 0) {
    if (resp.owe_status) {
      b->owe = 1;
      if (render_literal(b, in, 0, 500, note) == 0) *note = MURO_STEP_NOTE_HANDLER_500;
      return -2;
    }
    return -1;
  }
  if (resp.pull) return -1;
  host_in_after(b->hin, 0, &force, &over);
  if (over) return -1;
  keep = book_keep ? 1 : 0;
  if (resp.close_after || (resp.status >= 400 && resp.status <= 599) || force) keep = 1;
  if ((uint64_t)keep != book_keep) return -1;
  omit = b->is_head ? 1 : 0;
  copy_date(in, date);
  reason = resp.reason ? (const uint8_t *)resp.reason : NULL;
  extra = resp.headers ? (const uint8_t *)resp.headers : NULL;
  body = resp.body;
  n = host_copy_response(b->out, b->out_cap, (uint64_t)resp.status, reason,
                         reason ? resp.reason_len : 0, (const uint8_t *)date, 29, extra,
                         extra ? resp.headers_len : 0, body, body ? resp.body_len : 0,
                         (uint64_t)keep, (uint64_t)omit, &got);
  if (n == 0) {
    if (resp.owe_status) {
      b->owe = 1;
      if (render_literal(b, in, 0, 500, note) == 0) *note = MURO_STEP_NOTE_TOO_BIG_COMMIT;
      return -2;
    }
    *note = MURO_STEP_NOTE_TOO_BIG;
    return -1;
  }
  b->out_len = n;
  b->out_off = 0;
  b->rendered = 1;
  b->owe = resp.owe_status ? 1 : 0;
  return 0;
}

static void park(BookStep *b, StepOut *out) {
  out->bytes = b->out + b->out_off;
  out->len = b->out_len - b->out_off;
  out->interim = 0;
  out->cont = 0;
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

int muro_book_step(BookStep *st, const StepIn *in, StepOut *out) {
  muro__slice sl;
  http_step_Out bk;
  uint64_t owed;
  uint64_t prev_phase, prev_code, prev_left, prev_keep;
  uint64_t phase, code, close_b, status_b, rd, wr, span, cover, left, wake, keep;
  int finishing = 0;
  int fresh = 0;
  int note = 0;
  int rendered_rc;
  if (!st || !in || !out) return -1;
  memset(out, 0, sizeof *out);
  st->last_cover = 0;
  if (st->shut) {
    out->close_conn = 1;
    interests(st, out, 0, 0);
    return 0;
  }
  owed = (st->rendered && st->out_len > st->out_off) ? (uint64_t)(st->out_len - st->out_off) : 0;
  prev_phase = st->bphase;
  prev_code = st->bcode;
  prev_left = st->bleft;
  prev_keep = st->bkeep;
  sl = muro__slice_of(st->in ? st->in : (const uint8_t *)"", st->in_len);
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
    uint64_t end = 0;
    if ((uint64_t)in->n >= base) finishing = 1;
    if (finishing && prev_keep == 0) end = http_step_framedEnd(sl);
    muro_http_reset();
    if (finishing && prev_keep == 0) consume(st, end);
  } else {
    muro_http_reset();
  }
  remember(st, phase, code, left, keep);
  if (cover) {
    st->last_cover = 1;
    return closed_out(st, out, MURO_BOOK_NOTE_COVER, 1);
  }
  image_of(st, phase, status_b == 1);
  out->wake_at = wake;
  out->close_conn = close_b ? 1 : 0;
  out->status_now = status_b ? 1 : 0;
  interests(st, out, rd ? 1 : 0, wr ? 1 : 0);
  if (close_b || status_b) st->shut = 1;
  if (phase == 0 || phase == 4) {
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
    if (st->out_off >= st->out_len) return closed_out(st, out, MURO_BOOK_NOTE_COVER, 0);
    park(st, out);
    return 0;
  }
  st->rendered = 0;
  st->out_len = 0;
  st->out_off = 0;
  if (phase == 3 && code == 0) rendered_rc = render_handler(st, in, keep, &note);
  else if (phase == 1) rendered_rc = render_literal(st, in, 0, code, &note);
  else return closed_out(st, out, MURO_BOOK_NOTE_COVER, 0);
  if (rendered_rc < 0) return closed_out(st, out, note ? note : MURO_BOOK_NOTE_COVER, 0);
  if (!st->rendered || st->out_off >= st->out_len) return closed_out(st, out, MURO_BOOK_NOTE_COVER, 0);
  park(st, out);
  (void)prev_code;
  return 0;
}

#ifdef MURO_STEP_BOOK

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
  b->shut = st->pend_kind == 3;
  b->blind = st->pull_done;
  b->image = (int)st->st.muro__as.conn.muro__f_0;
  b->is_head = st->is_head;
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
}

static void store_http(HttpStep *st, const BookStep *b, StepOut *out) {
  st->in_len = b->in_len;
  st->out_len = b->out_len;
  st->out_off = b->out_off;
  st->rendered = b->rendered;
  st->owe = b->owe;
  st->pend_kind = b->shut ? 3 : 0;
  st->pull_done = b->blind;
  st->is_head = b->is_head;
  st->spare_len = b->spare_len;
  st->st.muro__as.conn.muro__f_0 = (uint64_t)b->image;
  st->st.muro__as.conn.muro__f_1 = b->bphase;
  st->st.muro__as.conn.muro__f_2 = b->bcode;
  st->st.muro__as.conn.muro__f_3 = b->bleft;
  st->st.muro__as.conn.muro__f_4 = b->bkeep;
  if (b->spare_len && b->spare_len <= sizeof st->spare) memcpy(st->spare, b->spare, b->spare_len);
  if (out && out->status && b->spare_len && out->status >= b->spare &&
      out->status < b->spare + sizeof b->spare) {
    out->status = st->spare + (size_t)(out->status - b->spare);
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
