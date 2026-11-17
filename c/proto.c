/* Host glue from one HTTP event to the emitted conn stepper.

   Which moves to offer is `admitChoice` in c/generated/admit.c, from
   priv/muro/http/admit.muro. This file does not choose them. It reads
   the session the stepper holds, turns the host's signed sentinels
   into the book's words, and calls `muro__sess_conn_step` once per
   offered move. A refused move leaves the session where it was.

   admit.h and session.h cannot share a translation unit. c/admit_glue.c
   is the unpack between them.

   The HTTP image (classify, decide, write ranges, keep-alive, the
   idle timer, the half-close flag, pipelined endOff/more) stays in
   the caller. This file does not classify.

   A FIN that arrives while a reply is already in progress is not a
   session event. The book offers half-close only from a read or idle
   bytes-or-eof. The image records a later FIN as the half flag and
   passes close-after on the write. That bit is still a stepper move.

   There is no down step. The admit map does not compare the text
   muro__sess_conn_expected prints. It passes `st.at`. */

#include "proto.h"
#include "session.h"
#include "admit_glue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PROTO_MOVE_MAX = 8 };

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

static int step_move(Proto *p, const AdmitMove *m) {
  if (m->tag == MURO_SESS_PICK) return step_pick(p, m->dir, m->bit);
  if (m->tag == MURO_SESS_DATA && m->ctor) return step_data(p, m->dir, m->ctor);
  return 0;
}

static void expect(const Proto *p, char *buf, size_t n) {
  muro__sess_conn_expected(p->st, buf, n);
}

static void restore(Proto *p, muro__sess_st saved) {
  p->st = saved;
  p->nmove = 0;
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
  AdmitOffer o;
  int i;
  if (!p || !p->live) return 1;
  p->nmove = 0;
  saved = p->st;
  o = admit_offer((uint64_t)phase, (uint64_t)ev_tag, (uint64_t)buf_n, status < 0 ? 0 : 1,
                  status < 0 ? 0 : (uint64_t)status, reply < 0 ? 0 : 1,
                  reply < 0 ? 0 : (uint64_t)reply, (uint64_t)observe, (uint64_t)close_after,
                  (uint64_t)p->st.at, (uint64_t)p->st.stuck, (uint64_t)p->st.key,
                  proto_done(p) ? 1 : 0);
  if (!o.ok) {
    restore(p, saved);
    return 0;
  }
  for (i = 0; i < o.n; i++) {
    if (!step_move(p, &o.m[i])) {
      restore(p, saved);
      return 0;
    }
  }
  return 1;
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
