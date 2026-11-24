#ifndef MURO_HTTP_STEP_BOOK_H
#define MURO_HTTP_STEP_BOOK_H

/* Buffer and handler adapter over the generated step slice.
   Not linked into the production host. c/step.c remains that step.
   Compile this file with -DMURO_STEP_BOOK to export muro_http_step
   in its place (see the muro-hello-book target).

   cover 1 fails closed: close_conn, image closed, note
   MURO_BOOK_NOTE_COVER, no read or write interest, and no span.
   A later fact on that step stays shut. Note
   MURO_BOOK_NOTE_STUCK is an invariant failure, not a cover.
   The host logger only prints notes 1..5.
   There is no fallback into c/step.c. */

#include "step.h"

enum { MURO_BOOK_NOTE_COVER = 6, MURO_BOOK_NOTE_STUCK = 7 };

typedef struct BookStep {
  int *read_more;
  int *write_more;
  uint8_t *in;
  size_t in_len;
  size_t in_cap;
  uint8_t *out;
  size_t out_cap;
  size_t out_len;
  size_t out_off;
  int rendered;
  int owe;
  int shut;
  int blind;
  int image;
  int is_head;
  int ver11;
  int resp_mode;
  int pull_until_end;
  MuroHttpPull pull;
  void *pull_user;
  uint64_t pull_left;
  int last_cover;
  uint64_t bphase;
  uint64_t bcode;
  uint64_t bleft;
  uint64_t bkeep;
  struct HostIn *hin;
  MuroHttpHandler handler;
  void *handler_user;
  uint64_t max_body;
  int idle_ms;
  uint8_t spare[512];
  size_t spare_len;
} BookStep;

void muro_book_bind(BookStep *st, uint8_t *in, size_t in_cap, uint8_t *out, size_t out_cap,
                    struct HostIn *hin, int *read_more, int *write_more, MuroHttpHandler handler,
                    void *user, uint64_t max_body, int idle_ms);
void muro_book_shutdown(BookStep *st);
int muro_book_step(BookStep *st, const StepIn *in, StepOut *out);
int muro_book_phase(const BookStep *st);
size_t muro_book_in_len(const BookStep *st);
void muro_book_set_in_len(BookStep *st, size_t len);

#endif
