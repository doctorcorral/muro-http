#ifndef MURO_HTTP_STEP_H
#define MURO_HTTP_STEP_H

/* HTTP/1.1 step. No sockets, file descriptors, processes, or wait.
   The adapter owns the connection's recv and send buffers and reports
   facts. The step returns bytes to write, when it next needs waking,
   and whether to close.

   Facts are data, not kernel readiness:
     MURO_STEP_ACCEPTED      a connection was accepted; buffers are bound
     MURO_STEP_BYTES         request bytes are already in the recv buffer
     MURO_STEP_PEER_CLOSED   the peer closed; bytes already read stay
     MURO_STEP_WROTE         the adapter accepted `n` bytes of the span
     MURO_STEP_WRITE_FAILED  that write failed hard
     MURO_STEP_NOW           monotonic time is `now_ms` and the deadline
                             the step armed is due

   A readable or writable socket is not a fact. The adapter writes the
   returned span, and it retries that same span after a short write.
   It reports MURO_STEP_WROTE only for bytes the peer's window took.

   StepOut.bytes points into the connection's send buffer, the
   interim area, or the close area. The keep-alive response is rendered
   once into the send buffer the adapter passed to muro_http_step_bind.
   The step does not allocate a second copy of those bytes.

   `date` is 29 bytes of IMF-fixdate from the adapter's clock. The step
   does not read the clock. `deadline` is the absolute monotonic time
   already armed, or 0. `empty_before` is 1 when the recv buffer was
   empty before this arrival (MURO_STEP_BYTES and MURO_STEP_PEER_CLOSED).

   read_more 1 means the step still needs request bytes. The adapter
   turns that into a read interest. It is not a readiness event.
   write_more is set while a span is outstanding. interim 1 means no
   byte of this 100 Continue has been written yet: do not read, and
   the adapter may stall before the first byte. cont 1 means the span
   is 100 Continue for its whole life, including after a short write.
   A hard error on a cont span closes. It is not
   MURO_STEP_WRITE_FAILED.

   wake_at 0 disarms. Otherwise it is an absolute monotonic millisecond.
   close_conn 1 means the adapter should close. status_now 1 means
   write `status` first, without waiting out a full send buffer, then
   close. That pointer is the close area or the send buffer.

   The image, the admit wall (c/proto.c), the request views, and the
   response bytes (c/host_render.c) stay where they were. This step
   calls them. A later muro step can take the same StepIn and return
   the same StepOut; the adapter does not change.

   Nested body reads are the adapter. It does not report MURO_STEP_NOW
   for a connection already inside one. The image timer is off in the
   handler and write phases; that deadline is the adapter's wait. */

#include "muro_http.h"
#include "conn.h"
#include "proto.h"

#include <stddef.h>
#include <stdint.h>

struct HostIn;

enum {
  MURO_STEP_ACCEPTED = 1,
  MURO_STEP_BYTES = 2,
  MURO_STEP_PEER_CLOSED = 3,
  MURO_STEP_WROTE = 4,
  MURO_STEP_WRITE_FAILED = 5,
  MURO_STEP_NOW = 6
};

enum {
  MURO_STEP_PH_READ = 0,
  MURO_STEP_PH_DECIDE = 1,
  MURO_STEP_PH_WRITE = 2,
  MURO_STEP_PH_IDLE = 3,
  MURO_STEP_PH_CLOSED = 4
};

enum {
  MURO_STEP_NOTE_ADMIT = 1,
  MURO_STEP_NOTE_TOO_BIG_COMMIT = 2,
  MURO_STEP_NOTE_TOO_BIG = 3,
  MURO_STEP_NOTE_HANDLER_500 = 4,
  MURO_STEP_NOTE_FUEL = 5
};

typedef struct StepIn {
  int kind;
  size_t n;
  uint64_t now_ms;
  uint64_t deadline;
  const uint8_t *date;
  size_t date_n;
  int empty_before;
} StepIn;

typedef struct StepOut {
  const uint8_t *bytes;
  size_t len;
  int read_more;
  int write_more;
  int interim;
  int cont;
  int close_conn;
  int status_now;
  const uint8_t *status;
  size_t status_len;
  uint64_t wake_at;
  int note;
  int note2;
  int note_phase;
  int note_ev;
  size_t note_n;
  size_t note_n2;
} StepOut;

/* C state for one connection. The fields below the interest pointers
   are the image and the buffers. A new backend uses StepIn and StepOut.
   It does not read these fields. read_more and write_more are the
   adapter's own ints. The step writes them, including while a handler
   or a response pull is still on the stack, so a nested body read sees
   the interest the image has reached. */
typedef struct HttpStep {
  int *read_more;
  int *write_more;
  Proto *proto;
  Conn st;
  uint8_t *in;
  size_t in_len;
  size_t in_cap;
  uint8_t *out;
  size_t out_cap;
  size_t out_len;
  size_t out_off;
  int rendered;
  int owe;
  int sent_100;
  uint8_t cont[160];
  size_t cont_len;
  size_t cont_off;
  int is_head;
  int ver11;
  int resp_mode;
  int pull_until_end;
  int pull_done;
  MuroHttpPull pull;
  void *pull_user;
  uint64_t pull_left;
  struct HostIn *hin;
  MuroHttpHandler handler;
  void *handler_user;
  uint64_t max_body;
  int idle_ms;
  int pend_kind;
  int spins;
  uint8_t spare[512];
  size_t spare_len;
  /* Header-block end carried across steps. 0 means unknown. The book
     adapter (muro_http_step in step_book.c) load/stores this as
     BookStep.bhend so muro-hello-book can reuse a stored end.
     te, sawcl, and cl are the first-scan framing facts stored next
     to it (chunked TE, saw Content-Length, and that length). */
  uint64_t hend;
  uint64_t te;
  uint64_t sawcl;
  uint64_t cl;
} HttpStep;

void muro_http_step_bind(HttpStep *st, uint8_t *in, size_t in_cap, uint8_t *out, size_t out_cap,
                         struct HostIn *hin, int *read_more, int *write_more, MuroHttpHandler handler,
                         void *user, uint64_t max_body, int idle_ms);

void muro_http_step_shutdown(HttpStep *st);

/* 0 when `out` is filled. -1 when the connection must be closed
   (admit refused, fuel, setup). `out->note` may be set either way. */
int muro_http_step(HttpStep *st, const StepIn *in, StepOut *out);

int muro_http_step_phase(const HttpStep *st);
size_t muro_http_step_in_len(const HttpStep *st);
void muro_http_step_recv(const HttpStep *st, uint8_t **p, size_t *len, size_t *cap);
void muro_http_step_set_in_len(HttpStep *st, size_t len);

/* Bytes to write before the adapter closes, when the step did not
   already return them. Points at the send buffer or the close area.
   *log_500 is 1 when the book is supplying 500 because a committed
   response never left. */
void muro_http_step_close_status(HttpStep *st, const uint8_t *date, size_t date_n,
                                 const uint8_t **msg, size_t *n, int *log_500);

/* c/test_serve sets the image phase of a skipped connection. */
void muro_http_step_set_phase(HttpStep *st, int phase);

#endif
