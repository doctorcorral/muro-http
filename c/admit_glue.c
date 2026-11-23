/* Host unpack of the emitted offer. The choice is `admitChoice`
   in c/generated/admit.c. This file does not decide a move. It copies
   the unboxed result into a flat list and turns the book's ctor word
   into the constructor name the stepper compares.

   dir and tag are the stepper enums in session.h (MURO_SESS_SEND,
   MURO_SESS_RECV, MURO_SESS_PICK, MURO_SESS_DATA). admit.muro cannot
   name that enum, so it copies the numbers. The asserts below fail
   if this emit's enums move away from those copies.

   The prelude is under MURO__PRELUDE, so this file includes session.h
   and admit.h together.

   The flat constructor word is muro__word_session_*. The book numbers
   piece-chunk, piece-req, piece-bad, reply-ok, and reply-bad as
   0, 1, 2, 3, and 4. The per-type tag of reply-ok is 0. That is not
   the flat word. */

#include "admit_glue.h"
#include "session.h"
#include "admit.h"

#include <stddef.h>
#include <string.h>

_Static_assert(MURO_SESS_SEND == 1, "admit.muro dirSend");
_Static_assert(MURO_SESS_RECV == 2, "admit.muro dirRecv");
_Static_assert(MURO_SESS_PICK == 4, "admit.muro tagPick");
_Static_assert(MURO_SESS_DATA == 8, "admit.muro tagData");
_Static_assert(muro__word_session_Piece_piecezdchunk == 0, "piece-chunk");
_Static_assert(muro__word_session_Piece_piecezdreq == 1, "piece-req");
_Static_assert(muro__word_session_Piece_piecezdbad == 2, "piece-bad");
_Static_assert(muro__word_session_Reply_replyzdok == 3, "reply-ok");
_Static_assert(muro__word_session_Reply_replyzdbad == 4, "reply-bad");

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

/* admitChoice is a pure function of these integers. One keep-alive
   GET repeats four offers. The book still runs when any integer
   differs, including the reply bit and close-after, so keep versus
   close stays a book result. Eight slots cover that cycle and a
   second connection shape without dropping the hot keys. */
enum { ADMIT_SLOTS = 8 };

struct admit_key {
  uint64_t w[13];
};

struct admit_slot {
  int used;
  struct admit_key key;
  AdmitOffer offer;
};

static struct admit_slot admit_slots[ADMIT_SLOTS];
static unsigned admit_hand;

static struct admit_key admit_key_of(uint64_t phase, uint64_t ev, uint64_t n, uint64_t has_cls,
                                     uint64_t cls, uint64_t has_rep, uint64_t rep, uint64_t observe,
                                     uint64_t close_after, uint64_t at, uint64_t stuck, uint64_t key,
                                     uint64_t sess_done) {
  struct admit_key k;
  k.w[0] = phase;
  k.w[1] = ev;
  k.w[2] = n;
  k.w[3] = has_cls;
  k.w[4] = cls;
  k.w[5] = has_rep;
  k.w[6] = rep;
  k.w[7] = observe;
  k.w[8] = close_after;
  k.w[9] = at;
  k.w[10] = stuck;
  k.w[11] = key;
  k.w[12] = sess_done;
  return k;
}

static void admit_store(struct admit_key key, AdmitOffer offer) {
  struct admit_slot *s = &admit_slots[admit_hand % ADMIT_SLOTS];
  admit_hand += 1;
  s->used = 1;
  s->key = key;
  s->offer = offer;
}

AdmitOffer admit_offer(uint64_t phase, uint64_t ev, uint64_t n, uint64_t has_cls,
                       uint64_t cls, uint64_t has_rep, uint64_t rep, uint64_t observe,
                       uint64_t close_after, uint64_t at, uint64_t stuck, uint64_t key,
                       uint64_t sess_done) {
  struct admit_key akey = admit_key_of(phase, ev, n, has_cls, cls, has_rep, rep, observe, close_after,
                                       at, stuck, key, sess_done);
  Offer o;
  AdmitOffer out;
  uint64_t count;
  uint64_t i;
  for (i = 0; i < ADMIT_SLOTS; i++) {
    if (admit_slots[i].used && memcmp(&admit_slots[i].key, &akey, sizeof akey) == 0)
      return admit_slots[i].offer;
  }
  o = admitChoice(phase, ev, n, has_cls, cls, has_rep, rep, observe, close_after, at, stuck, key,
                  sess_done);
  out = none();
  if (o.muro__as.offer.muro__f_0 != 1) {
    admit_store(akey, out);
    return out;
  }
  count = o.muro__as.offer.muro__f_1;
  if (count > ADMIT_MOVES) {
    admit_store(akey, out);
    return out;
  }
  out.ok = 1;
  out.n = (int)count;
  for (i = 0; i < count; i++) {
    Move m;
    uint64_t present, dir, tag, bit, ctor;
    const char *name = NULL;
    if (!slot(o, (int)i, &m)) {
      out = none();
      break;
    }
    present = m.muro__as.mv.muro__f_0;
    dir = m.muro__as.mv.muro__f_1;
    tag = m.muro__as.mv.muro__f_2;
    bit = m.muro__as.mv.muro__f_3;
    ctor = m.muro__as.mv.muro__f_4;
    if (present != 1 || (dir != MURO_SESS_SEND && dir != MURO_SESS_RECV)) {
      out = none();
      break;
    }
    if (tag == MURO_SESS_PICK) {
      if (bit > 1) {
        out = none();
        break;
      }
    } else if (tag == MURO_SESS_DATA) {
      name = ctor_name(ctor);
      if (!name) {
        out = none();
        break;
      }
    } else {
      out = none();
      break;
    }
    out.m[i].dir = (int)dir;
    out.m[i].tag = (int)tag;
    out.m[i].bit = (unsigned)bit;
    out.m[i].ctor = name;
  }
  admit_store(akey, out);
  return out;
}
