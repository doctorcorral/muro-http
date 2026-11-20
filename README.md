# muro-http

`muro-http` is a separate library. It is not part of muro's standard
library and it is not a book inside murolang/muro. This repository is
an HTTP/1.1 core: muro books for the request, the connection image,
admission, and the response bytes, plus a C host that dispatches to a
handler. The challenge server is an example on that core. This
repository is version `0.1.0-dev`.

It pins the released Hex package muro `0.21.14` (`{:muro, "~> 0.21.14"}`
in `mix.exs`, `muro: "0.21.14"` in `muro.exs`). It does not depend on a
compiler checkout.

It is hosted at https://github.com/doctorcorral/muro-http. There is no Hex publish.

## The request classifier

`priv/muro/http/request.muro` classifies one receive buffer. It does
not name an application route. `classify` takes a `Slice` of the
request line, the headers, and any body bytes already in that buffer.
It returns a `Classified` value:

- status: `status-ok`, `status-malformed`, or `status-need-more`
- keepalive, from the HTTP version and an optional `Connection` token

`methodView`, `targetView`, `pathView`, `queryView`, `versionView`,
and `bodyView` are zero-copy views. `pathView` strips the query at
`?`. `queryView` is the bytes after `?`, or empty when there is no
`?`. `headerCount`, `headerName`, `headerValue`, and `findHeader`
walk the header block. `findHeader` is the first match, ASCII
case-insensitive, value trimmed. A missing header is the
out-of-range empty. An empty value is a zero-length slice inside
the buffer. `contentLength` is the parsed length when the header
is well-formed.

`HEADER_MAX` and `BODY_MAX` are 16384. `Transfer-Encoding` is rejected.
A line without a colon, a bad version, a missing space, a second
`Content-Length`, a duplicate `Host`, or a length above the cap is
`status-malformed`. HTTP/1.1 with the headers finished and not exactly
one `Host` is `status-malformed` (RFC 9112). HTTP/1.0 may omit `Host`.
A second `Host` is malformed for both. A buffer that has not reached
the header terminator, or that is shorter than `Content-Length`, is
`status-need-more`. The host check runs only after the headers are
done, so a partial request stays need-more. The function is total.

Routing lives in the handler. The challenge routes are
`priv/muro/challenge/route.muro` (`routeVerb`, `routePath`, `routeId`,
`routeIdn`). `priv/muro/challenge/decide.muro` is the 46-equation
challenge policy. The request book does not import it.

## One connection step

`priv/muro/http/conn.muro` is one finite step of an HTTP/1 connection.
The host owns the socket, the buffer, and the clock. It calls `step`
and performs the action. The book does not loop. It does not call
`decide`.

`Conn` is unboxed data: fifteen machine words, passed by value. `Ev`
is unboxed. `step` returns the next `Conn`. The phase is read, decide,
write, idle, or closed. Half-closed is a flag. While it is set the
peer has sent FIN: the step may still write the pending response, and
it does not read again. Closed is terminal. Every event returns that
same state. Its action is close, which asks for no read and no write.

Events from the shim are bytes, peer eof, write progress, write done,
write error, an idle tick, and a handler reply. The reply event is
tag 6. Its `n0` is the handler's close-after bit. The core host
always reports that reply as `reply-ok` with the observe bit clear:
an HTTP 4xx or 5xx is still a completed handler response. Actions
back are hold, want read, write, close, and call the handler. The
call-handler action is still named `do-decide` and the phase is
still `mode-decide`, so the admission map and the session traces
keep their positions. A write carries a range of length 16. That
range is not the message. The host owns the response bytes. The
book chooses a malformed request: origin 1, keep close, action
write, and the bytes are `httpMalformed`. `endOff` is the first byte
after the request. `more` is set when the buffer holds bytes past
that, so the shim can re-step a pipelined tail. The step does not
slice the buffer.

Keep-alive follows the classifier. HTTP/1.1 defaults to keep-alive.
`Connection: close` and HTTP/1.0 close after the response. The
handler may force close. A malformed request writes an error and
then closes. Need-more stays in read, unless the peer has already
sent FIN, which is an error response. An idle tick closes a
connection that is reading or idle, including the first request. A
tick during the handler phase or write holds. `conn.muro` imports
`request.muro` and calls `classify`. It does not import `decide.muro`.

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
from `session.muro` and checks it against frozen scenario traces and
`Conn.check_trace`. That check is conformance. A two-request
keep-alive is deeper than the walk above.

`c/proto.c` applies the offer. It calls `muro__sess_conn_step` once
per move, rolls back when the stepper refuses, and passes `st.at`.
It does not choose talk versus timeout, open versus FIN, the piece
constructor, an immediate reply, the handler reply, or the
keep-or-close bit. `c/admit_glue.c` unpacks the unboxed offer and
turns the book's constructor tag into the name string the stepper
compares. Those two generated headers both define `struct muro__nat`,
so they cannot be included from one translation unit.
`c/test_conn` calls `proto_admit` before the HTTP image. A refused
move does not run the image.

`priv/muro/http/conn.muro` is the HTTP image: classify, call the
handler, a write range, keep-alive, the idle timer, the half-close
flag, and a pipelined offset. It does not name the session. A FIN that arrives
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

## The host loop

`c/muro_http.h` is the reusable server. `muro_http_serve` takes a
`MuroHttpConfig` and a handler `Request -> Response`. The config is
the bind host (NULL is `127.0.0.1`, IPv4 only), the port, the idle
timeout, the connection cap, and a log-name prefix. It does not carry
a database path or a JWT key. `muro_http_config_init` sets port 8080,
5000 ms, and 128 connections. `muro_http_config_env` reads
`MURO_HTTP_BIND`, `MURO_HTTP_PORT`, `MURO_HTTP_IDLE_MS`, and
`MURO_HTTP_MAX_CONNECTIONS`. `muro_http_config_arg` consumes `--bind`,
`--port`, `--idle-ms`, `--max-connections`, and `--help`. A bind host
longer than 15 characters, or one `inet_pton` rejects, makes
`muro_http_serve` return -2. Listen failure returns -1. SIGINT and
SIGTERM return 0. Port 0 prints `listening <host> <port>`.

The handler returns 0 to send the response, or -1 to close. Slices
on the request point at the receive buffer and are valid only until
the handler returns. Extra headers are a block that already ends in
CRLF. The handler must not supply `Connection` or `Content-Length`.
`close_after` forces `Connection: close`. `owe_status` means a side
effect has committed and a status must still leave the socket. If
that response does not fit, or the socket closes before any status
byte, the host copies the book's `httpInternal` bytes. It does not
choose them. A malformed request never calls the handler. The host
copies `httpMalformed`.

`c/host.c` owns the sockets, the buffers, the connection table, the
clock, and `poll`. `make c-check` compiles it with
`-Wall -Wextra -Werror -std=c11`, then runs the drivers. The loop
does not choose a move, a status, a header, or a body.

`c/host_poll.c` waits with `poll(2)` on macOS and on Linux. The idle
tick is the poll timeout, armed for the soonest connection deadline.
An earlier ready socket does not move that deadline. Ready events
are taken from a rotating cursor, so a wait that fills its event
cap still visits high-index sockets on the next wait. A write-only
`POLLHUP` without `POLLOUT` is an error and closes that slot; a
peer FIN while the host is still reading becomes `evEof`, and a
request that already arrived can still be answered. The image's
timer field says whether that phase is waiting. A tick during the
handler phase or write is not delivered. A socket that has sent only part of a
header, or only part of a body, stays in the set. The loop keeps
serving the other sockets. A close that still owes a status writes
it and returns. That write does not wait for the peer: `EAGAIN`
returns immediately, so one full send buffer cannot stall the table.
The peer may miss that last status if its window is already full.

Each accepted socket is one `Conn` and one session. Bytes, peer FIN,
write progress, write done, write error, and the idle tick go through
`proto_admit` and then `step`, the same order as `c/test_conn`. A
refusal closes the socket. The image does not run. A read error
closes the socket and is not `evError`: that event holds while the
phase is read. A write error is `evError`, and the image shuts.

The write range in the image stays length 16. `priv/muro/http/render.muro`
is `httpRender(status, reason, extraHeaders, body, keep)`. The book
writes the status line, then `Connection`, then `Content-Length`,
then the handler's extra headers, then the body. `--backend c` emits
it. The host copies that slice into its own buffer and writes it,
reports `evProgress` for the bytes the socket took, then `evDone`.
Pipelined bytes stay in the receive buffer. `endOff` and `more` say
where the next request starts. The host does not read the socket
while a response is in progress. The arena resets after that copy.
Header walks do not reset it, so a handler may still hold slices.

## The challenge example

`c/muro-httpd` is that server with the challenge handler. `make`
builds it from `examples/challenge/`. The same four routes answer
the same way: `GET /health`, `GET /feed`, `POST /posts`, and
`POST /posts/:id/like`. `examples/challenge/httpd.c` parses the core
flags and then `--db` and `--jwt-key`. SQLite, the JWT key, and the
JSON helpers are not in `c/muro_http.h`.

The handler classifies the route, the bearer, and the body, then
calls `decide`. An `act-observe` runs one SQLite statement and
`decide` runs again. A second observe closes the socket. That loop
stays inside the handler. It does not touch the stepper. `decide`
chooses the status, the error token, and the act. The handler turns
those tags into the response. The core frames them.

`GET /health` and `GET /feed` query when `decide` returns
`act-observe`. `POST /posts` queries only after auth is `auth-ok` and
the body class is `body-ok`. `GET /posts/:id` and
`POST /posts/:id/like` query when the id is still `id-unknown`. One
query sets the terminal db, id, and like facts together. A later ask
closes the socket. `id-present` is that completed lookup. The host
does not pair it with `db-down`. If the write fails after the lookup
was going to report `id-present`, the host sends `db-down` and leaves
the id unknown. `POST /posts` reaches the query only for `body-ok`,
which has already stored the text. A missing text closes the socket.
That path is a host bug, not a `500`.

The store is the swap boundary. `decide` never sees SQLite. The
handler asks for rows and fills the response. `store_open` runs
`PRAGMA journal_mode=WAL` and `PRAGMA synchronous=NORMAL`, then reads
both back, and sets `sqlite3_busy_timeout` to 5000 ms. `NORMAL` is 1.
The challenge names that mode as the durability floor for WAL. With
WAL and `synchronous=NORMAL`, a commit survives a process crash and
may be lost on power loss. The commit finishes before create returns
0 or like returns 1 or 2. The reopen tests prove a clean close and
reopen, not crash or power-loss durability.
A file database whose journal mode is not `wal`, or whose synchronous
value is below `NORMAL`, is not opened. `:memory:` cannot be WAL.
It remains the test-only default, journal mode `memory`, and it is
not a durable database. The path is `--db`, then `MURO_HTTP_DB`,
then `SQLITE_PATH`, then `:memory:`. An empty `--db`, `MURO_HTTP_DB`,
or `SQLITE_PATH` exits 2. Tables are
`posts(id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL)` and
`likes(post_id INTEGER NOT NULL, sub TEXT NOT NULL, PRIMARY KEY(post_id, sub))`.
One connection, `SQLITE_OPEN_NOMUTEX`. Create and like render the row
inside the write transaction and commit only when that row fits. If
it does not fit, the store rolls back and the host passes `db-down`,
so `decide` answers 500. A path that cannot be opened leaves the
process listening. Every db fact is then `db-down`, and stderr says
`muro-httpd: database is unavailable`. After a committed create or
like, `MURO_TEST_FAIL_DB_ADMIT` makes the handler answer 500
`internal` with `Connection: close` and `owe_status` set. The row
stays. The handler returns 0. The core does not refuse the reply.

The JWT key is `--jwt-key` or `MURO_HTTP_JWT_KEY`. The flag wins.
The bytes are the HMAC key, at most 256. A longer key exits 2. An
empty key checks no bearer: a presented token is `auth-bad-token`,
and a missing header stays `auth-absent`. The value must be `Bearer `
plus three base64url segments. A final partial quartet with non-zero
leftover bits is rejected. `alg` must be `HS256`. `none` and any
other alg are `auth-bad-token`. `typ` may be absent. When present it
must be `JWT` in any ASCII case. Any other `typ` is `auth-bad-token`.
The MAC compare always runs over 32 bytes. A decoded signature of
any other length is compared with a dummy and then rejected. The
signature is checked before the payload. `sub` is a non-empty string
of at most 128 bytes. `exp` and `nbf` are optional. The challenge
SPEC says to verify `exp`. It does not say a missing `exp` is an
error, and the upstream tests always send one. When present, each
must be a JSON integer with no fraction, no exponent, and no leading
zero, so a float `exp` or `nbf`, or an `exp` of `01`, is
`auth-bad-payload`. They are compared with
`time(NULL)`. A token is rejected when `exp <= now` and when
`nbf > now`. `nbf == now` is valid. When both claims are present the
valid window is `nbf <= now < exp`. A bad `sub`, `exp`, or `nbf` is
`auth-bad-payload`. The test key `muro-http-test-key` lives in the
test sources. It is not a server default.

`BodyClass` for `POST /posts` is the emitted `bodyClass`. Other
routes pass `body-none`. A class of `body-ok` is unescaped and
checked as UTF-8 before the first `decide`. A bad escape or
ill-formed UTF-8 becomes `body-malformed`. That change is a fact.

Feed, create, and like bytes come from the store. Read bytes are
built in the handler from the stored text. The handler passes that
body to the core, which prints `Content-Length`. Feed is a JSON
array ordered by id. Read and create are `{"id":N,"text":"..."}`.
Like is `{"id":N,"likes":M}`. The same subject twice is `like-new`
(201) and then `like-again` (200), and the count stays 1. Health is
the body `ok`. An error act is the plain token.

Flags `--bind`, `--port`, `--idle-ms`, `--max-connections`, and
`--jwt-key` override `MURO_HTTP_BIND`, `MURO_HTTP_PORT`,
`MURO_HTTP_IDLE_MS`, `MURO_HTTP_MAX_CONNECTIONS`, and
`MURO_HTTP_JWT_KEY`. The database path is `--db`, then `MURO_HTTP_DB`,
then `SQLITE_PATH`, then `:memory:`. The defaults are `127.0.0.1`,
port 8080, 5000 ms, 128 connections, `:memory:`, and no key. Port 0
prints `listening <host> <port>`. SIGINT and SIGTERM close the
sockets and exit 0.

## The hello example

`examples/hello/hello.c` is a second server on `c/muro_http.h`.
`make` builds `c/muro-hello`. It does not link SQLite. GET answers
`hello\n`. POST echoes the body. Any other method is 405 `method\n`.
`GET /health` is that same hello. It is not the challenge route.
`c/test_hello` checks those responses, a missing `Host` (the book's
400 `malformed`), keep-alive, and `--bind 127.0.0.1` winning over
`MURO_HTTP_BIND=nope`. A bind host that is not IPv4 exits 2 and does
not listen.

`c/test_e2e` starts that binary on an ephemeral port. It checks
`GET /health`, a malformed request, two requests on one connection, a
pipelined pair, `Connection: close`, an idle timeout, a peer FIN
mid-request, a body that arrives after the headers, a response the
socket takes in more than one write, the connection cap, and a
receive buffer that fills before the image consumes it. One client
writes a header a byte at a time and stops before the blank line.
Another stops in the middle of a body. While those sockets stay
open, other clients complete `GET /health`, an authenticated
`GET /feed`, `POST /posts`, and `POST /posts/:id/like`. A malformed
request on a further socket is answered and closed. The partial
header is then finished on its own socket. SIGINT and SIGTERM each
stop the process with status 0 while a partial header is still open.
It also
checks each challenge route: a good JWT, a bad signature, `alg`
`none`, a missing bearer, a bad payload, a missing and a present id,
a like and the same like again, valid and invalid JSON, and a
database path that cannot be opened, `exp == now`, `nbf == now`,
a float `exp`, a leading-zero `exp`, a float ignored claim, `typ`
absent or `jwt` or `JWS`, a temp file whose create is still there
after a clean close and reopen (not a crash or power-loss test), an
empty `--db`, `MURO_HTTP_DB`, or `SQLITE_PATH` (exit 2), a committed
create and like whose db admit then fails (500 `internal`, and the
rows remain), and the path order `--db`, then `MURO_HTTP_DB`, then
`SQLITE_PATH`.
A `:memory:` server is still checked. The child drops
`MURO_HTTP_DB`, `SQLITE_PATH`, `MURO_HTTP_JWT_KEY`, `MURO_HTTP_BIND`,
and `MURO_TEST_FAIL_DB_ADMIT`, and passes `--db` and `--jwt-key`,
except in the path-order cases, the bind case, and the admit-fail
case, so a developer environment does not change the result. The
bind case sets `MURO_HTTP_BIND=nope` and `--bind 127.0.0.1`, then
checks `GET /health`. Those are tests.

The reduction equations in `render.muro` are the reduction evidence
(refl) for the strings they name. There are five: an empty keep
response, an empty close response, 204, `httpMalformed`, and
`httpInternal`. A positive `Content-Length` walks `rdShow`, and that
reduction does not finish. `c/test_render` checks those five strings
in the emitted C, plus the challenge health bytes, a JSON array, a
405, and a ten-byte body. That is a test.

muro 0.21.15 and 0.21.16 add a temporal check and a synthesis cover.
They do not change response bytes or this loop. The pin stays 0.21.14.

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

## What is emitted, and what is host code

`--backend c` emits the core books `request.muro` (classify, the
views, `findHeader`), `conn.muro`, `session.muro`, `admit.muro`, and
`render.muro` (`httpRender`, `httpMalformed`, `httpInternal`). It
emits the challenge books `decide.muro`, `route.muro`, and
`body.muro` (`bodyClass`, `textOff`, `textLen`).

Hand-written core: `c/muro_http.h`, `c/host.c`, `c/host_poll.c`,
`c/host_render.c`, `c/proto.c`, `c/admit_glue.c`, `c/shim.c`.
Hand-written challenge, under `examples/challenge/`: `httpd.c` (main),
`app.c` (the handler), `route_glue.c`, `store.c` (SQLite), `jwt.c`
(SHA-256, HMAC-SHA256, base64url, the HS256 check), `json_text.c`
(string unescape, the UTF-8 check, and the row JSON), and
`host_body.c` (the call into `bodyClass`). `examples/hello/hello.c`
is the second server. Each generated header defines the prelude, so
one translation unit includes only one of them. `conn.o` already
contains `request.muro`. Do not also link `request.o`. `conn.o`
emits `not`, `and`, and `or` because it imports `std:bool.muro`. A
book linked beside it must not import that book. `decide.muro` and
`route.muro` do not.

## Limits

- The core binds one IPv4 address. The default is `127.0.0.1`.
  `--bind` and `MURO_HTTP_BIND` select it. The stored host is at
  most 15 characters.
- `addCap` still wraps a `U64`. The driver reports a delta of 8.
- `bc-empty`, `bc-off-empty`, `bc-len-empty`, and `bc-long` reduce.
  They return before `jbScan`. A refl that walks the scanner does not
  finish. One byte of that scan took the checker past 30 GB.
  `c/test_body.c` checks the scanner.
- `render-empty-keep` reduces (`Content-Length: 0`). A positive
  length walks `rdShow`, and that reduction does not finish.
  `c/test_render` checks a ten-byte body in the emitted C.
- The body book accepts any octet inside a JSON string, including a
  byte that is not UTF-8. The host turns that `body-ok` into
  `body-malformed` before `decide`. A `\u` surrogate
  (U+D800 through U+DFFF) is malformed in the book. Surrogate pairs
  are not decoded.
- A JWT claim name longer than 32 bytes is rejected. Header and
  payload JSON are decoded into a 4096-byte buffer.
- The row buffer is 65536 bytes and the write buffer is 69632. A feed
  or a read that does not fit closes the socket. Create and like
  render inside the transaction. If that row does not fit, the store
  rolls back and the fact is `db-down`, so `decide` answers 500.
  The HTTP header on a row is under 256 bytes, so a row that fits in
  the row buffer fits in the write buffer. If the rendered response
  still does not fit after a committed write, or the host closes
  before any status byte is written, it sends 500 and the body
  `internal`. No path leaves a committed write with no HTTP status.
  The image's write range stays length 16. The host sends the
  rendered buffer, then `evDone`.
- `:memory:` is the default and is test-only. It cannot be WAL, so
  it is not a durable database and it does not meet the challenge
  rule that forbids an in-memory database. A file path does.
  `synchronous=NORMAL` is the value read back on every open store,
  including `:memory:`. With WAL and `synchronous=NORMAL`, a commit
  survives a process crash and may be lost on power loss. The reopen
  tests prove a clean close and reopen, not crash or power-loss
  durability.
- An authorization value longer than 16384 bytes is `auth-bad-token`.

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

`make c-check` compiles the drivers, `c/muro-httpd`, and `c/muro-hello`
with `-Wall -Wextra -Werror -std=c11` and runs them. `c/test_hello`
runs the hello server. `c/test_route` and `c/test_tags` check the
challenge route book and the decide constructor tags. `c/test_admit`
is not
in that target. It reads a trace on stdin. It is not a pass/fail
driver.

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
`targetView`, `pathView`, `queryView`, `versionView`, and `bodyView`
allocate 0. On muro 0.21.14, as on 0.20.42, `classify` of
`GET /health` allocates 1 and `classify` of `POST /posts` with a
two-byte body allocates 1. That object is the `Classified` result.
See `GAPS.md`. `bodyClass`, `textOff`, and `textLen` allocate 0,
including on a 1024-byte body. Those counts are the bump arena on
this pin. The host copies the rendered slice and resets the arena.
Method, path, query, and body views still point at the original
buffer. A missing header is the out-of-range empty. An empty header
value points inside the buffer and has length 0.

`c/test_conn` drives the HTTP image. Protocol moves go through
`c/proto.c`, which applies `admitChoice` from `c/generated/admit.c`
and calls the stepper in `c/generated/session.c`. The link does not
include `request.o`: `conn.muro` already imports that book. It does
not include `decide.o`: the image does not call `decide`. `session.c` and `admit.c` are separate translation units.
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

A step that does not call `classify` allocates 0. That includes
`begin`, the handler reply, write progress, write done, write error,
an idle tick, eof of an empty buffer, eof while the handler is in
progress or while writing, and every event after close. A step that
classifies allocates 1, the `Classified` result. The stepper's own
moves allocate 0. `proto_begin` is outside that count. `decide`
allocates its one `Decision` inside the challenge handler, which
resets the arena before the next call.

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
