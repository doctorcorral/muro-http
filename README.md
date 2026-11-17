# muro-http

`muro-http` is a separate library. It is not part of muro's standard
library and it is not a book inside murolang/muro. A muro web server
will live here. This repository is version `0.1.0-dev`.

It pins the released Hex package muro `0.21.14` (`{:muro, "~> 0.21.14"}`
in `mix.exs`, `muro: "0.21.14"` in `muro.exs`). It does not depend on a
compiler checkout.

It is hosted at https://github.com/doctorcorral/muro-http. There is no Hex publish.

## What step 1 covers

`priv/muro/http/request.muro` classifies one receive buffer. `classify`
takes a `Slice` of the request line, the headers, and any body bytes
already in that buffer. It returns a `Classified` value:

- status: `status-ok`, `status-malformed`, or `status-need-more`
- verb: `verb-get`, `verb-post`, or `verb-other`
- path shape: `path-health`, `path-feed`, `path-posts`, `path-post-id`,
  `path-post-like`, or `path-other` (the query string is stripped at `?`)
- id class: `id-unknown` with a parsed number, `id-bad`, or `id-none`
  (`id-present` and `id-missing` stay host facts for `decide`)
- keepalive, from the HTTP version and an optional `Connection` token
- an authorization mark, with a zero-copy `Slice` of the trimmed value
  when the header is present
- `Content-Length`, and a zero-copy body `Slice` when those bytes are
  in the buffer

`HEADER_MAX` and `BODY_MAX` are 16384. `Transfer-Encoding` is rejected.
A line without a colon, a bad version, a missing space, a second
`Content-Length`, or a length above the cap is `status-malformed`.
A buffer that has not reached the header terminator, or that is shorter
than `Content-Length`, is `status-need-more`. The function is total.

Method, path, the authorization value, and the body are views into the
original buffer. `methodView`, `targetView`, `pathView`, and `versionView`
are only `sliceUntil`, `sliceAfter`, and `slice`.

`priv/muro/challenge/decide.muro` is the existing 46-equation challenge
policy. `request.muro` imports it so the two books share `Verb`,
`PathShape`, `IdClass`, and `BodyClass`. `decide` still takes auth class,
like, and db facts from the host.

## What step 2 covers

`priv/muro/http/conn.muro` is one finite step of an HTTP/1 connection.
The C shim owns the socket, the buffer, and the clock. It calls `step`
and performs the action. The book does not loop.

`Conn` is unboxed data: fifteen machine words, passed by value. `Ev`
is unboxed. `step` returns the next `Conn`. The phase is read, decide,
write, idle, or closed. Half-closed is a flag. While it is set the
peer has sent FIN: the step may still write the pending response, and
it does not read again. Closed is terminal. Every event returns that
same state. Its action is close, which asks for no read and no write.

Events from the shim are bytes, peer eof, write progress, write done,
write error, an idle tick, and a db result. The db event carries the
host facts `decide` still needs: auth, body, like, db, and id class,
as tags in `decide`'s constructor order. Actions back are hold, want
read, write, close, and call decide. A write carries the status, error,
and act tags, plus a range of length 16. The shim owns the response
bytes. `endOff` is the first byte after the request. `more` is set
when the buffer holds bytes past that, so the shim can re-step a
pipelined tail. The step does not slice the buffer.

Keep-alive follows the classifier. HTTP/1.1 defaults to keep-alive.
`Connection: close` and HTTP/1.0 close after the response. A malformed
request writes an error and then closes. Need-more stays in read,
unless the peer has already sent FIN, which is an error response.
An idle tick closes a connection that is reading or idle, including
the first request. A tick during decide or write holds. `conn.muro`
imports `request.muro` and calls `classify` and `decide`. It does not
copy them.

### The session stepper

`priv/muro/http/session.muro` is the protocol. `--backend c` emits it
as a stepper (`c/generated/session.c`). The host may offer a sequence
of moves for one image event. Each move is one call to
`muro__sess_conn_step` with the current state. The function returns
the next state, or refuses a move the protocol does not allow. The
host keeps the socket. The stepper does not loop.

The actions are the ones `MuroHttp.SessionFsm` takes. muro checks the
emitter by a walk of at most 14 states and depth at most 6, including
events the protocol does not allow. That walk is not a proof, and
this stepper is not "the same step". There is no `down` step. This
session's init takes no nat. A stepper that does takes `uint64_t`, so
a negative cannot be supplied. A wrong key, a bad position, or an
unknown session name returns `MURO_SESS_UNEXPECTED` from the step.
Init of an unknown name returns `MURO_SESS_INIT`.

`priv/muro/http/admit.muro` is the event-to-move map. `--backend c`
emits it as `admitChoice` (`c/generated/admit.c`). The inputs are the
host event and `st.at` on this stepper. The output is the sequence of
moves to offer, or a refusal, or no move. muro checks that function
by reduction. It does not check that an offer is a legal prefix of
`conn` from that position: a `run` is not a `conform`, and there is
no judgment that takes a position and a list of directed moves.
`test/sess_agree_test.exs` steps each offer on the machine emitted
from `session.muro` and checks it against the move list the previous
hand-written map produced. That check is conformance. A two-request
keep-alive is deeper than the walk above.

`c/proto.c` applies the offer. It calls `muro__sess_conn_step` once
per move, rolls back when the stepper refuses, and passes `st.at`.
It does not choose talk versus timeout, open versus FIN, the piece
constructor, an immediate reply, the decide reply, or the
keep-or-close bit. `c/admit_glue.c` unpacks the unboxed offer and
turns the book's constructor tag into the name string the stepper
compares. Those two generated headers both define `struct muro__nat`,
so they cannot be included from one translation unit.
`c/test_conn` calls `proto_admit` before the HTTP image. A refused
move does not run the image.

`priv/muro/http/conn.muro` is the HTTP image: classify, decide, a
write range, keep-alive, the idle timer, the half-close flag, and a
pipelined offset. It does not name the session. A FIN that arrives
while a reply is already in progress is that flag. The session
chooses half-close before the piece. The later keep-or-close choice
is still a move on the stepper.

`test/conn_test.exs` replays `MuroHttp.SessionFsm.Conn.check_trace`
on the same exchanges the image runs. Three books under
`priv/muro/http/neg/` are refused by `mix muro.check`: a write after
`end`, a read after `end`, and a read after half-close. They are not
in `muro.exs`, so `mix muro.gen` does not emit them. After close, the
image returns the same closed state for every event. `shut` is that
terminal image and has no live session.

The server end is `conn`. The client end `peer` is `dual conn`.

```
session conn :=
  rec Loop.
    choose
      (branch
        (recv (p : Piece) (case p
          | piece-chunk => Loop
          | piece-req => send Reply (choose Loop end)
          | piece-bad => send Reply end))
        (branch
          (recv (p : Piece) (case p
            | piece-chunk => send Reply end
            | piece-req => send Reply end
            | piece-bad => send Reply end))
          end))
      end
```

The outer choice is talk, or an idle timeout that ends. The peer
then chooses an open connection or a half-close. An open piece is a
chunk, a full request, or a bad request. A request is answered, and
this end chooses keep or close. A bad request is answered and the
connection ends. On a half-close the peer chooses a piece or a bare
FIN. A piece is answered and the connection ends, so this end does
not read again. Conforms and meets in the book check keep, close,
timeout, a bad request, half-close, bare FIN, and a chunk.

## What this step does not do

- JWT HMAC verification. The classifier records the raw `Authorization`
  value. `AuthClass` is a host input to `decide`, carried on the db
  event.
- JSON body class. A POST body is the raw `Slice` plus the length.
  `BodyClass` from `classify` is `body-none`. The db event carries the
  host's body class.
- The id integer, and SQLite. The db event carries those facts.
- Response bytes. Every response range has length 16. The shim writes
  the bytes.
- The socket and the event loop. `c/shim.c` is a bump arena and a
  slice wrap. `step` handles one event and does not call itself.
- A wrapped `U64` in `addCap`. The driver reports a delta of 8.

## Check

```sh
mise exec -- mix deps.get
mise exec -- mix muro.gen --check
mise exec -- mix test
```

`mix muro.gen --check` type-checks each book once, then checks that
the committed C, Elixir, and ExUnit files still match a fresh emit
from that copy. It writes nothing.

`muro.exs` sets fuel to 5000000. The closed evidence walks the header
scanner, and the default fuel of 2000 is not enough.

## C driver

`c/shim.c` defines `muro__ALLOC` as a bump arena. The driver links
`c/generated/request.c` and the shim. It does not link `decide.c`:
`request.muro` already imports that book, so the symbols would be
duplicated.

```sh
cc -Wall -Wextra -Werror -std=c11 -I c -I c/generated -include c/shim.h \
  -c -o c/test_classify.o c/test_classify.c
cc -Wall -Wextra -Werror -std=c11 -I c -c -o c/shim.o c/shim.c
cc -Wall -Wextra -Werror -std=c11 \
  -I c -I c/generated -include c/shim.h \
  -c -o c/request.o c/generated/request.c
cc -o c/test_classify c/test_classify.o c/request.o c/shim.o
./c/test_classify
```

The shim, the driver, and the generated file are compiled with
`-Wall -Wextra -Werror`. The generated file does not need
`-Wno-missing-field-initializers`, `-Wno-unused-variable`, or
`-Wno-unused-function`.

The driver prints `OK` and allocation counts. `methodView`,
`targetView`, `pathView`, and `versionView` allocate 0. On muro
0.21.14, as on 0.20.42, `classify` of `GET /health` allocates 1 and
`classify` of `POST /posts` with a two-byte body allocates 1. That
object is the `Classified` result. See `GAPS.md`.
Method, path, authorization, and body views still point at the
original buffer, or at the out-of-range empty when the span is absent.

`c/test_conn` drives the HTTP image. Protocol moves go through
`c/proto.c`, which applies `admitChoice` from `c/generated/admit.c`
and calls the stepper in `c/generated/session.c`. The link does not
include `request.o` or `decide.o`: `conn.muro` already imports those
books. `session.c` and `admit.c` are separate translation units.
Their headers both define `struct muro__nat`, so `c/admit_glue.c`
is the only file that includes `admit.h`.

```sh
cc -Wall -Wextra -Werror -std=c11 -I c -I c/generated -include c/shim.h \
  -c -o c/test_conn.o c/test_conn.c
cc -Wall -Wextra -Werror -std=c11 -I c -c -o c/shim.o c/shim.c
cc -Wall -Wextra -Werror -std=c11 \
  -I c -I c/generated -include c/shim.h \
  -c -o c/conn.o c/generated/conn.c
cc -Wall -Wextra -Werror -std=c11 \
  -I c -I c/generated -include c/shim.h \
  -c -o c/session.o c/generated/session.c
cc -Wall -Wextra -Werror -std=c11 \
  -I c -I c/generated -include c/shim.h \
  -c -o c/admit.o c/generated/admit.c
cc -Wall -Wextra -Werror -std=c11 -I c -I c/generated \
  -c -o c/admit_glue.o c/admit_glue.c
cc -Wall -Wextra -Werror -std=c11 -I c -I c/generated \
  -c -o c/proto.o c/proto.c
cc -o c/test_conn c/test_conn.o c/conn.o c/session.o c/admit.o c/admit_glue.o c/proto.o c/shim.o
./c/test_conn
```

A step that does not call `classify` or `decide` allocates 0. That
includes `begin`, write progress, write done, write error, an idle
tick, eof of an empty buffer, eof while already deciding or writing,
and every event after close. A step that classifies allocates 1, the
`Classified` result. A step that calls `decide` allocates 1, the
`Decision`. The stepper's own moves allocate 0. `proto_begin` is
outside that count.

`c/test_sess` checks the stepper by itself. With no arguments it
prints `OK`.

```sh
cc -Wall -Wextra -Werror -std=c11 \
  -I c -I c/generated -include c/shim.h \
  -c -o c/session.o c/generated/session.c
cc -Wall -Wextra -Werror -std=c11 -I c -I c/generated \
  -c -o c/test_sess.o c/test_sess.c
cc -Wall -Wextra -Werror -std=c11 -I c -c -o c/shim.o c/shim.c
cc -o c/test_sess c/test_sess.o c/session.o c/shim.o
./c/test_sess
```
