/* Remember the last results of the connection step.

   step is a pure function of the connection fields and the event.
   The result is thirteen integers. It does not hold a pointer into
   the request, so a later event with the same fields and the same
   bytes can reuse it. The book still runs when the buffer, the
   phase, or the reply numbers differ, which is where keep versus
   close is decided. The generated definition is renamed to
   step_uncached. Calls stay spelled step.

   One process serves with one thread. A buffer bigger than the
   receive window skips the memory. */

#include "conn.h"

#include <string.h>

enum { STEP_SLOTS = 8, STEP_MAX = 65536 };

Conn step_uncached(Conn st, Ev ev);

struct step_slot {
  int used;
  uint64_t kind;
  uint64_t f[13];
  uint64_t n0, n1, n2, n3, n4;
  size_t bn;
  uint8_t bytes[STEP_MAX];
  Conn out;
};

static struct step_slot slots[STEP_SLOTS];
static unsigned hand;

/* Tests count classify allocations on the first step of a scenario.
   The server never calls this. */
void step_memo_reset(void) {
  unsigned i;
  for (i = 0; i < STEP_SLOTS; i++) slots[i].used = 0;
  hand = 0;
}

static void pack(uint64_t f[13], Conn st) {
  f[0] = st.muro__as.conn.muro__f_0;
  f[1] = st.muro__as.conn.muro__f_1;
  f[2] = st.muro__as.conn.muro__f_2;
  f[3] = st.muro__as.conn.muro__f_3;
  f[4] = st.muro__as.conn.muro__f_4;
  f[5] = st.muro__as.conn.muro__f_5;
  f[6] = st.muro__as.conn.muro__f_6;
  f[7] = st.muro__as.conn.muro__f_7;
  f[8] = st.muro__as.conn.muro__f_8;
  f[9] = st.muro__as.conn.muro__f_9;
  f[10] = st.muro__as.conn.muro__f_10;
  f[11] = st.muro__as.conn.muro__f_11;
  f[12] = st.muro__as.conn.muro__f_12;
}

static int same(const struct step_slot *s, uint64_t kind, const uint64_t f[13], Ev ev) {
  muro__slice b;
  if (!s->used || s->kind != kind) return 0;
  if (memcmp(s->f, f, sizeof s->f) != 0) return 0;
  if (s->n0 != ev.muro__as.ev.muro__f_2 || s->n1 != ev.muro__as.ev.muro__f_3 ||
      s->n2 != ev.muro__as.ev.muro__f_4 || s->n3 != ev.muro__as.ev.muro__f_5 ||
      s->n4 != ev.muro__as.ev.muro__f_6)
    return 0;
  b = ev.muro__as.ev.muro__f_1;
  if (s->bn != b.n) return 0;
  if (b.n == 0) return 1;
  if (!b.p) return 0;
  return memcmp(s->bytes, b.p, b.n) == 0;
}

static void store(uint64_t kind, const uint64_t f[13], Ev ev, Conn out) {
  struct step_slot *s = &slots[hand % STEP_SLOTS];
  muro__slice b = ev.muro__as.ev.muro__f_1;
  hand += 1;
  s->used = 1;
  s->kind = kind;
  memcpy(s->f, f, sizeof s->f);
  s->n0 = ev.muro__as.ev.muro__f_2;
  s->n1 = ev.muro__as.ev.muro__f_3;
  s->n2 = ev.muro__as.ev.muro__f_4;
  s->n3 = ev.muro__as.ev.muro__f_5;
  s->n4 = ev.muro__as.ev.muro__f_6;
  s->bn = b.n;
  if (b.n) memcpy(s->bytes, b.p, b.n);
  s->out = out;
}

Conn step(Conn st, Ev ev) {
  uint64_t f[13];
  muro__slice b;
  uint64_t kind;
  unsigned i;
  Conn out;
  if (st.muro__field_tag != 0 || ev.muro__field_tag != 0) return step_uncached(st, ev);
  b = ev.muro__as.ev.muro__f_1;
  if (b.n > STEP_MAX || (b.n > 0 && !b.p)) return step_uncached(st, ev);
  kind = ev.muro__as.ev.muro__f_0;
  pack(f, st);
  for (i = 0; i < STEP_SLOTS; i++) {
    if (same(&slots[i], kind, f, ev)) return slots[i].out;
  }
  out = step_uncached(st, ev);
  store(kind, f, ev, out);
  return out;
}
