#ifndef MURO_HTTP_PROTO_H
#define MURO_HTTP_PROTO_H

#include <stddef.h>

/* Directions and value kinds for proto_refuses. They are the stepper's
   send/recv and pick/data tags. */
#define PROTO_SEND 1
#define PROTO_RECV 2
#define PROTO_PICK 1
#define PROTO_DATA 2

/* One end of the emitted `conn` session. The HTTP image stays in the
   caller. This object is only the protocol stepper. */
typedef struct Proto Proto;

/* `muro__sess_conn_init`. A fresh connection is at the first choice. */
Proto *proto_begin(void);

/* The HTTP `shut` image has no live session. Events on it are not
   protocol moves. */
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
   Returns 1 when every move was accepted, or when the event is host
   state and moves nothing. Returns 0 when the stepper refuses. On 0
   the session is unchanged and the caller must not run the image. */
int proto_admit(Proto *p, int phase, int ev_tag, size_t buf_n, int status,
                int reply, int observe, int close_after);

/* Try one move and restore the session. Returns 1 when the stepper
   refuses it. */
int proto_refuses(Proto *p, int dir, int kind, unsigned bit, const char *ctor);

/* The action the stepper expects now, for a refusal message. */
void proto_where(const Proto *p, char *buf, size_t n);

#endif
