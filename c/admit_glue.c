/* Host unpack of the emitted offer. The choice is `admitChoice`
   in c/generated/admit.c. This file does not decide a move. It copies
   the unboxed result into a flat list and turns the book's ctor word
   into the constructor name the stepper compares.

   ctor words, from priv/muro/http/admit.muro:
     0 piece-chunk, 1 piece-req, 2 piece-bad, 3 reply-ok, 4 reply-bad.
   dir 1 and 2 are send and recv. tag 4 is a pick and tag 8 is data.
   Those are the stepper enums this emit uses. */

#include "admit_glue.h"
#include "admit.h"

#include <stddef.h>

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
    if (present != 1 || (dir != 1 && dir != 2)) return none();
    if (tag == 4) {
      if (bit > 1) return none();
    } else if (tag == 8) {
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
