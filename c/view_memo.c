/* Remember pure reads of one request buffer.

   classify, the method and transfer-encoding codes, the frame end,
   and the body view are functions of the bytes only. A later read of
   the same bytes reuses the integers. The body view is stored as an
   offset, so it still points into the caller's buffer. classify's
   status and keep words are the book's static values, copied into a
   slot that outlives the arena.

   The connection step still runs when its own inputs differ. This
   memory does not choose keep versus close and does not build a
   response. One process serves with one thread. A buffer bigger than
   the receive window skips the memory. */

#include "conn.h"

#include <stdint.h>
#include <string.h>

enum { VIEW_SLOTS = 8, VIEW_MAX = 65536 };

Classified *classify_uncached(muro__slice s);
uint64_t methodCode_uncached(muro__slice s);
uint64_t teCode_uncached(muro__slice s);
uint64_t framedEnd_uncached(muro__slice s);
uint64_t streamTake_uncached(muro__slice s);
muro__slice bodyView_uncached(muro__slice s);

struct view_slot {
  int used;
  size_t n;
  uint8_t bytes[VIEW_MAX];
  int has_cls;
  Classified cls;
  int has_method;
  uint64_t method;
  int has_te;
  uint64_t te;
  int has_frame;
  uint64_t frame;
  int has_take;
  uint64_t take;
  int has_body;
  int body_in_buf;
  size_t body_off;
  size_t body_n;
};

static struct view_slot slots[VIEW_SLOTS];
static unsigned hand;

void view_memo_reset(void) {
  unsigned i;
  for (i = 0; i < VIEW_SLOTS; i++) slots[i].used = 0;
  hand = 0;
}

static struct view_slot *find(muro__slice s) {
  unsigned i;
  if (s.n > VIEW_MAX || (s.n > 0 && !s.p)) return 0;
  for (i = 0; i < VIEW_SLOTS; i++) {
    struct view_slot *sl = &slots[i];
    if (!sl->used || sl->n != s.n) continue;
    if (s.n == 0 || memcmp(sl->bytes, s.p, s.n) == 0) return sl;
  }
  return 0;
}

static struct view_slot *slot_for(muro__slice s) {
  struct view_slot *sl = find(s);
  if (sl) return sl;
  sl = &slots[hand % VIEW_SLOTS];
  hand += 1;
  sl->used = 1;
  sl->n = s.n;
  sl->has_cls = 0;
  sl->has_method = 0;
  sl->has_te = 0;
  sl->has_frame = 0;
  sl->has_take = 0;
  sl->has_body = 0;
  sl->body_in_buf = 0;
  if (s.n) memcpy(sl->bytes, s.p, s.n);
  return sl;
}

/* The pointer addresses a ring slot, not a heap object that lives
   for the process. A later classify can overwrite that slot after
   VIEW_SLOTS newer entries. A buffer that skips the ring returns
   the book's pointer instead. */
Classified *classify(muro__slice s) {
  struct view_slot *sl;
  Classified *c;
  if (s.n > VIEW_MAX || (s.n > 0 && !s.p)) return classify_uncached(s);
  sl = slot_for(s);
  if (sl->has_cls) return &sl->cls;
  c = classify_uncached(s);
  if (!c || !c->muro__as.classified.muro__f_0 || !c->muro__as.classified.muro__f_1) return c;
  sl->cls.muro__field_tag = c->muro__field_tag;
  sl->cls.muro__as.classified.muro__f_0 = c->muro__as.classified.muro__f_0;
  sl->cls.muro__as.classified.muro__f_1 = c->muro__as.classified.muro__f_1;
  sl->has_cls = 1;
  return &sl->cls;
}

uint64_t methodCode(muro__slice s) {
  struct view_slot *sl;
  if (s.n > VIEW_MAX || (s.n > 0 && !s.p)) return methodCode_uncached(s);
  sl = slot_for(s);
  if (sl->has_method) return sl->method;
  sl->method = methodCode_uncached(s);
  sl->has_method = 1;
  return sl->method;
}

uint64_t teCode(muro__slice s) {
  struct view_slot *sl;
  if (s.n > VIEW_MAX || (s.n > 0 && !s.p)) return teCode_uncached(s);
  sl = slot_for(s);
  if (sl->has_te) return sl->te;
  sl->te = teCode_uncached(s);
  sl->has_te = 1;
  return sl->te;
}

uint64_t framedEnd(muro__slice s) {
  struct view_slot *sl;
  if (s.n > VIEW_MAX || (s.n > 0 && !s.p)) return framedEnd_uncached(s);
  sl = slot_for(s);
  if (sl->has_frame) return sl->frame;
  sl->frame = framedEnd_uncached(s);
  sl->has_frame = 1;
  return sl->frame;
}

uint64_t streamTake(muro__slice s) {
  struct view_slot *sl;
  if (s.n > VIEW_MAX || (s.n > 0 && !s.p)) return streamTake_uncached(s);
  sl = slot_for(s);
  if (sl->has_take) return sl->take;
  sl->take = streamTake_uncached(s);
  sl->has_take = 1;
  return sl->take;
}

/* r sits inside s, including a zero-length view at s.p + s.n.
   Integer addresses only: r.p + r.n is undefined if that sum leaves
   the object r.p points at. */
static int body_in_slice(muro__slice s, muro__slice r, size_t *off) {
  uintptr_t base;
  uintptr_t start;
  uintptr_t lim;
  if (!s.p || !r.p || r.n > s.n) return 0;
  base = (uintptr_t)s.p;
  start = (uintptr_t)r.p;
  lim = base + s.n;
  if (start < base || start > lim) return 0;
  if (start - base > s.n - r.n) return 0;
  *off = (size_t)(start - base);
  return 1;
}

muro__slice bodyView(muro__slice s) {
  struct view_slot *sl;
  muro__slice r;
  size_t off;
  if (s.n > VIEW_MAX || (s.n > 0 && !s.p)) return bodyView_uncached(s);
  sl = slot_for(s);
  if (sl->has_body) {
    /* muro__slice_of(..., 0) collapses to the TU-local empty
       sentinel. An in-buffer empty view must keep its offset in s. */
    if (sl->body_in_buf && s.p)
      return (muro__slice){s.p + sl->body_off, sl->body_n};
    return muro__slice_empty();
  }
  r = bodyView_uncached(s);
  if (body_in_slice(s, r, &off)) {
    sl->body_in_buf = 1;
    sl->body_off = off;
    sl->body_n = r.n;
    sl->has_body = 1;
    return r;
  }
  if (r.n == 0) {
    sl->body_in_buf = 0;
    sl->body_off = 0;
    sl->body_n = 0;
    sl->has_body = 1;
    return r;
  }
  return r;
}
