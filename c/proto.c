/* Hand-written host map from one HTTP event to protocol moves.

   The stepper is the one muro emits from priv/muro/http/session.muro
   (`c/generated/session.c`). This file is not that emit. It chooses
   which moves to offer: talk or timeout, open or FIN, the piece
   constructor, an immediate reply-bad, reply-ok or reply-bad from a
   decide code, and the keep or close bit. Each offered move is one
   call of muro__sess_conn_step. The stepper accepts or refuses it.
   A refused move leaves the session where it was.

   The HTTP image (classify, decide, write ranges, keep-alive, the
   idle timer, the half-close flag, pipelined endOff/more) stays in
   the caller. This file does not classify.

   A FIN that arrives while a reply is already in progress is not a
   session event. The session chooses half-close before the piece.
   The image records that FIN as the half flag and forces the later
   keep-or-close choice to close. That choice is still a stepper move.

   There is no down step.

   Positions below are `st.at` on this emit of the conn stepper
   (muro 0.21.14). muro__sess_conn_at names them. The admit map does
   not compare the text muro__sess_conn_expected prints. */

#include "proto.h"
#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
  PH_READ = 0,
  PH_DECIDE = 1,
  PH_WRITE = 2,
  PH_IDLE = 3,
  PH_CLOSED = 4,
  EV_BYTES = 0,
  EV_EOF = 1,
  EV_DONE = 3,
  EV_ERROR = 4,
  EV_TICK = 5,
  EV_DB = 6,
  ST_OK = 0,
  ST_BAD = 1,
  /* Send-pick positions: outer talk/timeout, then keep/close. */
  AT_TALK = 1,
  AT_KEEP = 5,
  /* Send-Reply positions. Open request, open bad, half chunk,
     half request, half bad. */
  AT_REPLY = 4,
  AT_REPLY_BAD = 7,
  AT_REPLY_HALF_CHUNK = 10,
  AT_REPLY_HALF_REQ = 11,
  AT_REPLY_HALF_BAD = 12,
  PROTO_MOVE_MAX = 8
};

struct Proto {
  muro__sess_st st;
  int live;
  int nmove;
  ProtoMove moves[PROTO_MOVE_MAX];
};

/* A pick is a bit 0 or 1. Reject anything else before the uint8_t
   store, so 256 cannot become 0. */
static int fill_pick(unsigned bit, muro__sess_val *v) {
  if (bit > 1) return 0;
  memset(v, 0, sizeof *v);
  v->tag = MURO_SESS_PICK;
  v->bit = (uint8_t)bit;
  return 1;
}

static muro__sess_val data_val(const char *ctor) {
  muro__sess_val v;
  memset(&v, 0, sizeof v);
  v.tag = MURO_SESS_DATA;
  v.ctor = ctor;
  v.n = 0;
  return v;
}

static int remember(Proto *p, int dir, const muro__sess_val *v) {
  ProtoMove *m;
  if (p->nmove >= PROTO_MOVE_MAX) return 0;
  m = &p->moves[p->nmove];
  m->dir = dir;
  m->bit = 0;
  m->ctor = NULL;
  if (v->tag == MURO_SESS_PICK) {
    m->kind = PROTO_PICK;
    m->bit = v->bit;
  } else {
    m->kind = PROTO_DATA;
    m->ctor = v->ctor;
  }
  p->nmove += 1;
  return 1;
}

static int step_val(Proto *p, int dir, muro__sess_val v) {
  muro__sess_res res = muro__sess_conn_step(p->st, dir, &v);
  if (res.status != MURO_SESS_OK) return 0;
  if (!remember(p, dir, &v)) return 0;
  p->st = res.state;
  return 1;
}

static int step_pick(Proto *p, int dir, unsigned bit) {
  muro__sess_val v;
  if (!fill_pick(bit, &v)) return 0;
  return step_val(p, dir, v);
}

static int step_data(Proto *p, int dir, const char *ctor) {
  return step_val(p, dir, data_val(ctor));
}

static void expect(const Proto *p, char *buf, size_t n) {
  muro__sess_conn_expected(p->st, buf, n);
}

/* True when this emit's conn stepper is waiting to send a Reply.
   Keyed off st.at, not off the expected-action printer. */
static int at_reply(const Proto *p) {
  uint16_t at;
  if (!p || p->st.key != 1 || p->st.stuck) return 0;
  at = p->st.at;
  return at == AT_REPLY || at == AT_REPLY_BAD || at == AT_REPLY_HALF_CHUNK ||
         at == AT_REPLY_HALF_REQ || at == AT_REPLY_HALF_BAD;
}

/* True when this emit's conn stepper is waiting to send a pick.
   at 1 is talk or timeout. at 5 is keep or close. */
static int at_choice(const Proto *p) {
  uint16_t at;
  if (!p || p->st.key != 1 || p->st.stuck) return 0;
  at = p->st.at;
  return at == AT_TALK || at == AT_KEEP;
}

static const char *piece_name(int status) {
  if (status == ST_OK) return "piece-req";
  if (status == ST_BAD) return "piece-bad";
  return "piece-chunk";
}

/* An open malformed request is answered now. A half-closed need-more
   or malformed request is answered now. A full request waits until
   decide commits the reply. */
static int reply_now(int status, int fin) {
  if (fin) return status != ST_OK;
  return status == ST_BAD;
}

static int admit_turn(Proto *p, int ev, size_t n, int status) {
  int fin;
  if (ev == EV_TICK) return step_pick(p, MURO_SESS_SEND, 1);
  if (ev == EV_EOF && n == 0) {
    return step_pick(p, MURO_SESS_SEND, 0) && step_pick(p, MURO_SESS_RECV, 1) &&
           step_pick(p, MURO_SESS_RECV, 1);
  }
  if (ev != EV_BYTES && !(ev == EV_EOF && n > 0)) return 1;
  if (status < 0) return 0;
  fin = ev == EV_EOF;
  if (!step_pick(p, MURO_SESS_SEND, 0)) return 0;
  if (!step_pick(p, MURO_SESS_RECV, fin ? 1 : 0)) return 0;
  if (fin && !step_pick(p, MURO_SESS_RECV, 0)) return 0;
  if (!step_data(p, MURO_SESS_RECV, piece_name(status))) return 0;
  if (reply_now(status, fin)) return step_data(p, MURO_SESS_SEND, "reply-bad");
  return 1;
}

static int admit_decide(Proto *p, int ev, int reply, int observe) {
  if (ev != EV_DB) return 1;
  if (!at_reply(p)) return 0;
  if (observe) return 1;
  if (reply < 0) return 0;
  return step_data(p, MURO_SESS_SEND, reply ? "reply-bad" : "reply-ok");
}

static int admit_write(Proto *p, int ev, int close_after) {
  int bit;
  if (ev != EV_DONE && ev != EV_ERROR) return 1;
  if (proto_done(p)) return 1;
  if (!at_choice(p)) return 0;
  bit = (ev == EV_ERROR || close_after) ? 1 : 0;
  return step_pick(p, MURO_SESS_SEND, (unsigned)bit);
}

Proto *proto_begin(void) {
  Proto *p = calloc(1, sizeof *p);
  muro__sess_res res;
  if (!p) return NULL;
  res = muro__sess_conn_init();
  if (res.status != MURO_SESS_OK) {
    free(p);
    return NULL;
  }
  p->st = res.state;
  p->live = 1;
  return p;
}

Proto *proto_shut(void) { return calloc(1, sizeof(Proto)); }

void proto_free(Proto *p) { free(p); }

int proto_live(const Proto *p) { return p && p->live; }

int proto_done(const Proto *p) {
  return p && p->live && muro__sess_conn_done(p->st);
}

int proto_move_count(const Proto *p) { return p ? p->nmove : 0; }

int proto_move_at(const Proto *p, int i, ProtoMove *out) {
  if (!p || !out || i < 0 || i >= p->nmove) return 0;
  *out = p->moves[i];
  return 1;
}

int proto_admit(Proto *p, int phase, int ev_tag, size_t buf_n, int status, int reply,
                int observe, int close_after) {
  muro__sess_st saved;
  int ok;
  if (!p || !p->live) return 1;
  p->nmove = 0;
  if (proto_done(p)) return 1;
  saved = p->st;
  if (phase == PH_READ || phase == PH_IDLE) ok = admit_turn(p, ev_tag, buf_n, status);
  else if (phase == PH_DECIDE) ok = admit_decide(p, ev_tag, reply, observe);
  else if (phase == PH_WRITE) ok = admit_write(p, ev_tag, close_after);
  else ok = 0;
  if (!ok) {
    p->st = saved;
    p->nmove = 0;
  }
  return ok;
}

int proto_refuses(Proto *p, int dir, int kind, unsigned bit, const char *ctor) {
  muro__sess_st saved;
  muro__sess_val v;
  muro__sess_res res;
  if (!p || !p->live) return 0;
  saved = p->st;
  if (kind == PROTO_PICK) {
    if (!fill_pick(bit, &v)) return 1;
  } else {
    v = data_val(ctor);
  }
  res = muro__sess_conn_step(p->st, dir, &v);
  p->st = saved;
  return res.status == MURO_SESS_UNEXPECTED;
}

void proto_where(const Proto *p, char *buf, size_t n) {
  if (!buf || n == 0) return;
  if (!p || !p->live) {
    snprintf(buf, n, "no-session");
    return;
  }
  expect(p, buf, n);
}
