#ifndef MURO_HTTP_PROTO_H
#define MURO_HTTP_PROTO_H

#include <stddef.h>

/* Directions and value kinds for proto_refuses and the move trace.
   They are the stepper's send/recv and pick/data tags. */
#define PROTO_SEND 1
#define PROTO_RECV 2
#define PROTO_PICK 1
#define PROTO_DATA 2

/* One end of the emitted `conn` session. The HTTP image stays in the
   caller. This object is the protocol stepper plus the offer from
   `admitChoice` (c/generated/admit.c). proto.c applies that offer.
   It does not choose the moves. */
typedef struct Proto Proto;

/* One move proto_admit committed on its last call. ctor aliases a
   string literal. It is NULL for a pick. */
typedef struct ProtoMove {
  int dir;
  int kind;
  unsigned bit;
  const char *ctor;
} ProtoMove;

/* `muro__sess_conn_init`. A fresh connection is at the first choice.
   Returns NULL if allocation fails or the stepper init is not
   MURO_SESS_OK. The caller must not admit on NULL. */
Proto *proto_begin(void);

/* The HTTP `shut` image has no live session. Events on it are not
   protocol moves. Returns NULL if allocation fails. */
Proto *proto_shut(void);

void proto_free(Proto *p);

int proto_live(const Proto *p);
int proto_done(const Proto *p);

/* Admit the protocol moves for one HTTP event, before the image runs.
   phase is the image phase (0 read, 1 decide, 2 write, 3 idle, 4 closed).
   ev_tag is the Ev tag. status is the classifier tag when this event
   classifies a buffer, otherwise -1. reply is 0 for reply-ok, 1 for
   reply-bad, or -1 when this event does not send one. observe is 1 when
   a db result stays in decide. close_after is 1 when the image will
   close after the response.

   The host may offer several moves for this one event. Each move is
   one stepper call. Which moves are offered is `admitChoice`, emitted
   from priv/muro/http/admit.muro. The emitted stepper accepts or
   refuses each one.

   Returns 1 when every move was accepted, or when the event is host
   state and moves nothing. Returns 0 when the book refuses. Returns
   -1 when the book offered a move and the stepper refused it. On 0
   and on -1 the session is unchanged and the caller must not run the
   image. A NULL session is not a refusal: shut has no session. */
int proto_admit(Proto *p, int phase, int ev_tag, size_t buf_n, int status,
                int reply, int observe, int close_after);

/* The image does not call these. The admit test places the session in
   a state the stepper can already report, then shows that the book
   refuses a move. stuck is 0 or 1. key is the stepper's uint16_t. */
void proto_force_stuck(Proto *p, int stuck);
void proto_force_key(Proto *p, unsigned key);

/* Moves committed by the last proto_admit. Empty when it refused,
   when the event moved nothing, or when the session was already done.
   The pointer from proto_move_at is valid until the next admit or free. */
int proto_move_count(const Proto *p);
int proto_move_at(const Proto *p, int i, ProtoMove *out);

/* Try one move and restore the session. Returns 1 when the stepper
   refuses it. A pick bit above 1 is refused here, before the value
   is stored in a uint8_t. Returns 0 when there is no live session. */
int proto_refuses(Proto *p, int dir, int kind, unsigned bit, const char *ctor);

/* The action the stepper expects now, for a refusal message.
   This is display text. The admit map does not branch on it. */
void proto_where(const Proto *p, char *buf, size_t n);

#endif
