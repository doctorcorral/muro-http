/* Host unpack of the emitted offer. The choice is `admitChoice`
   in c/generated/admit.c. This file does not decide a move. It copies
   the unboxed result into a flat list and turns the book's ctor word
   into the constructor name the stepper compares.

   dir and tag are the stepper enums in session.h (MURO_SESS_SEND,
   MURO_SESS_RECV, MURO_SESS_PICK, MURO_SESS_DATA). admit.muro cannot
   name that enum, so it copies the numbers. The asserts below fail
   if this emit's enums move away from those copies.

   admit.h and session.h both define struct muro__nat, and the emitter
   does not put that prelude under one include guard (GAPS.md). The
   session prelude is renamed for this translation unit so the enums
   are visible beside admit.h.

   Constructor words are not stepper enums. session.h only comments
   the Piece tags (0, 1, 2) and the Reply tags (0, 1). The book uses
   one numbering for both, so reply-ok is 3 and reply-bad is 4.
   There is no session.h constant to assert those against. */

#include "admit_glue.h"

#define muro__nat muro__sess_nat_hide
#define muro__unit muro__sess_unit_hide
#include "session.h"
#undef muro__nat
#undef muro__unit

#include "admit.h"

#include <stddef.h>

_Static_assert(MURO_SESS_SEND == 1, "admit.muro dirSend");
_Static_assert(MURO_SESS_RECV == 2, "admit.muro dirRecv");
_Static_assert(MURO_SESS_PICK == 4, "admit.muro tagPick");
_Static_assert(MURO_SESS_DATA == 8, "admit.muro tagData");

static const char *ctor_name(uint64_t ctor) {
  if (ctor == 0) return "piece-chunk";
  if (ctor == 1) return "piece-req";
  if (ctor == 2) return "piece-bad";
  if (ctor == 3) return "reply-ok";
  if (ctor == 4) return "reply-bad";
  return NULL;
}

static int slot(Offer o, int i, Move *out) {
  switch (i) {
    case 0: *out = o.muro__as.offer.muro__f_2; return 1;
    case 1: *out = o.muro__as.offer.muro__f_3; return 1;
    case 2: *out = o.muro__as.offer.muro__f_4; return 1;
    case 3: *out = o.muro__as.offer.muro__f_5; return 1;
    case 4: *out = o.muro__as.offer.muro__f_6; return 1;
    default: return 0;
  }
}

static AdmitOffer none(void) {
  AdmitOffer out;
  int i;
  out.ok = 0;
  out.n = 0;
  for (i = 0; i < ADMIT_MOVES; i++) {
    out.m[i].dir = 0;
    out.m[i].tag = 0;
    out.m[i].bit = 0;
    out.m[i].ctor = NULL;
  }
  return out;
}

AdmitOffer admit_offer(uint64_t phase, uint64_t ev, uint64_t n, uint64_t has_cls,
                       uint64_t cls, uint64_t has_rep, uint64_t rep, uint64_t observe,
                       uint64_t close_after, uint64_t at, uint64_t stuck, uint64_t key,
                       uint64_t sess_done) {
  Offer o = admitChoice(phase, ev, n, has_cls, cls, has_rep, rep, observe, close_after, at,
                        stuck, key, sess_done);
  AdmitOffer out = none();
  uint64_t count;
  uint64_t i;
  if (o.muro__as.offer.muro__f_0 != 1) return out;
  count = o.muro__as.offer.muro__f_1;
  if (count > ADMIT_MOVES) return out;
  out.ok = 1;
  out.n = (int)count;
  for (i = 0; i < count; i++) {
    Move m;
    uint64_t present, dir, tag, bit, ctor;
    const char *name = NULL;
    if (!slot(o, (int)i, &m)) return none();
    present = m.muro__as.mv.muro__f_0;
    dir = m.muro__as.mv.muro__f_1;
    tag = m.muro__as.mv.muro__f_2;
    bit = m.muro__as.mv.muro__f_3;
    ctor = m.muro__as.mv.muro__f_4;
    if (present != 1 || (dir != MURO_SESS_SEND && dir != MURO_SESS_RECV)) return none();
    if (tag == MURO_SESS_PICK) {
      if (bit > 1) return none();
    } else if (tag == MURO_SESS_DATA) {
      name = ctor_name(ctor);
      if (!name) return none();
    } else {
      return none();
    }
    out.m[i].dir = (int)dir;
    out.m[i].tag = (int)tag;
    out.m[i].bit = (unsigned)bit;
    out.m[i].ctor = name;
  }
  return out;
}
