/* Host glue from one HTTP event to the emitted conn stepper.

   Which moves to offer is `admitChoice` in c/generated/admit.c, from
   priv/muro/http/admit.muro. This file does not choose them. It reads
   the session the stepper holds, turns the host's signed sentinels
   into the book's words, and calls `muro__sess_conn_step` once per
   offered move. A refused move leaves the session where it was.

   c/admit_glue.c unpacks the offer. The prelude is under
   MURO__PRELUDE, so that file includes admit.h and session.h
   together.

   The HTTP image (classify, the handler, write ranges, keep-alive, the
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

/* One keep-alive GET applies five session moves. Each move is a pure
   function of the session word and the value. The book still runs on
   a miss, including a different pick bit or a different constructor,
   so keep versus close stays a book result. A hit keeps the new
   state and skips the stepper's expected-string fill. Nested values
   are not remembered. The constructor is the literal pointer the
   book passed; a different pointer is a miss even when the text
   matches. Eight slots cover the hot cycle. One worker is one thread. */
enum { SESS_SLOTS = 8 };

struct sess_key {
  uint16_t key;
  uint16_t at;
  uint8_t stuck;
  uint8_t set0;
  uint8_t tag;
  uint8_t bit;
  int dir;
  uint64_t nat0;
  uint64_t vnat;
  uint64_t vn;
  const char *ctor;
};

struct sess_slot {
  int used;
  struct sess_key key;
  int status;
  muro__sess_st state;
};

static struct sess_slot sess_slots[SESS_SLOTS];
static unsigned sess_hand;

static int sess_plain(const muro__sess_val *v) {
  return v && !v->a && !v->b && !v->names;
}

/* nat[0] and set[0] are the whole state only while the book has one
   slot. A wider header must not reuse this key. */
_Static_assert(MURO__SESS_SLOTS == 1, "sess_key_of keys only set[0] and nat[0]");

static unsigned sess_misses;

/* Tests isolate moves. The server never calls these. */
void sess_memo_reset(void) {
  unsigned i;
  for (i = 0; i < SESS_SLOTS; i++) sess_slots[i].used = 0;
  sess_hand = 0;
  sess_misses = 0;
}

unsigned sess_memo_misses(void) { return sess_misses; }

static void sess_key_of(struct sess_key *k, const muro__sess_st *st, int dir,
                        const muro__sess_val *v) {
  memset(k, 0, sizeof *k);
  k->key = st->key;
  k->at = st->at;
  k->stuck = st->stuck;
  k->set0 = st->set[0];
  k->tag = v->tag;
  k->bit = v->bit;
  k->dir = dir;
  k->nat0 = st->nat[0];
  k->vnat = v->nat;
  k->vn = v->n;
  k->ctor = v->ctor;
}

static int sess_find(const struct sess_key *k, muro__sess_res *out) {
  unsigned i;
  for (i = 0; i < SESS_SLOTS; i++) {
    if (!sess_slots[i].used) continue;
    if (memcmp(&sess_slots[i].key, k, sizeof *k) != 0) continue;
    out->status = sess_slots[i].status;
    out->state = sess_slots[i].state;
    return 1;
  }
  return 0;
}

static void sess_store(const struct sess_key *k, int status, muro__sess_st state) {
  struct sess_slot *s = &sess_slots[sess_hand % SESS_SLOTS];
  sess_hand += 1;
  s->used = 1;
  s->key = *k;
  s->status = status;
  s->state = state;
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
  muro__sess_res res;
  struct sess_key key;
  int hit = 0;
  if (sess_plain(&v)) {
    sess_key_of(&key, &p->st, dir, &v);
    hit = sess_find(&key, &res);
    if (!hit) {
      sess_misses += 1;
      res = muro__sess_conn_step(p->st, dir, &v);
      if (res.status == MURO_SESS_OK) sess_store(&key, res.status, res.state);
    }
  } else {
    sess_misses += 1;
    res = muro__sess_conn_step(p->st, dir, &v);
  }
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
      return -1;
    }
  }
  return 1;
}

void proto_force_stuck(Proto *p, int stuck) {
  if (!p || !p->live) return;
  p->st.stuck = stuck ? 1 : 0;
}

void proto_force_key(Proto *p, unsigned key) {
  if (!p || !p->live) return;
  p->st.key = (uint16_t)key;
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
