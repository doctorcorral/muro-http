#ifndef MURO_HTTP_ADMIT_GLUE_H
#define MURO_HTTP_ADMIT_GLUE_H

#include <stdint.h>

/* Unpack of the offer `admitChoice` returns. This is not the choice.
   `c/generated/admit.h` and `c/generated/session.h` each define
   `struct muro__nat`, so one translation unit cannot include both.
   The stepper also takes a constructor name string. The tag-to-name
   map for that string is here. The words of the offer are the book's. */

#define ADMIT_MOVES 5

typedef struct AdmitMove {
  int dir;
  int tag;
  unsigned bit;
  const char *ctor;
} AdmitMove;

typedef struct AdmitOffer {
  int ok;
  int n;
  AdmitMove m[ADMIT_MOVES];
} AdmitOffer;

/* phase, ev, and n are the image's numbers. has_cls is 0 when status
   was negative. has_rep is 0 when reply was negative. observe and
   close_after are the host words, zero or not. at, stuck, and key are
   the conn stepper's. sess_done is 1 when that stepper reports done. */
AdmitOffer admit_offer(uint64_t phase, uint64_t ev, uint64_t n, uint64_t has_cls,
                       uint64_t cls, uint64_t has_rep, uint64_t rep, uint64_t observe,
                       uint64_t close_after, uint64_t at, uint64_t stuck, uint64_t key,
                       uint64_t sess_done);

#endif
