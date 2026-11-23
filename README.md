<table>
  <tr>
    <td align="center">
      <a href="https://github.com/doctorcorral/muro-http"><img src="assets/muro-http-logo.png" alt="muro-http" width="140"></a>
    </td>
    <td>
      <h3>An HTTP/1.1 core written in muro.<br>Checked, then served from C.</h3>
      <a href="https://github.com/doctorcorral/muro-http/actions/workflows/muro.yml"><img src="https://img.shields.io/github/actions/workflow/status/doctorcorral/muro-http/muro.yml?branch=main&style=flat-square&label=ci&labelColor=111111&color=e8e8e8" alt="CI"></a>
      <a href="https://hex.pm/packages/muro"><img src="https://img.shields.io/badge/muro-0.22.19-e8e8e8?style=flat-square&labelColor=111111" alt="muro 0.22.19"></a>
    </td>
  </tr>
</table>

# muro-http

`muro-http` is a separate library. It is not part of muro's standard
library and it is not a book inside murolang/muro. This repository is
an HTTP/1.1 core: muro books for the request, the connection image,
admission, and the response bytes, plus a C host that dispatches to a
handler. The challenge server is an example on that core. This
repository is version `0.1.0-dev`.

It pins the released Hex package muro `0.22.19` (`{:muro, "~> 0.22.19"}`
in `mix.exs`, `muro: "0.22.19"` in `muro.exs`). It does not depend on a
compiler checkout.

It is hosted at https://github.com/doctorcorral/muro-http. There is no Hex publish.

## The request classifier

`priv/muro/http/request.muro` classifies one receive buffer. It does
not name an application route. `classify` takes a `Slice` of the
request line, the headers, and any body bytes already in that buffer.
It returns a `Classified` value:

- status: `status-ok`, `status-malformed`, `status-need-more`, or
  `status-unimplemented`
- keepalive, from the HTTP version and `Connection` tokens

`methodView`, `targetView`, `pathView`, `queryView`, `versionView`,
and `bodyView` are zero-copy views. `pathView` strips the query at
`?`. `queryView` is the bytes after `?`, or empty when there is no
`?`. `headerCount`, `headerName`, `headerValue`, and `findHeader`
walk the header block. `findHeader` is the first match, ASCII
case-insensitive, value trimmed. A missing header is the
out-of-range empty. An empty value is a zero-length slice inside
the buffer. `contentLength` is the parsed length when the header
is well-formed.

A header block longer than 16384 is `status-malformed`. A
`Content-Length` of 16384 that has not arrived stays
`status-need-more`. A longer declared length that has not all
arrived is `status-ok`: the host streams it. `bodyView` stays the
bytes already in the buffer, and it is empty for a chunked body.
The host decodes a finished chunked message, or streams one that
does not finish inside 16384 bytes. The receive buffer is 65536.

`Transfer-Encoding: chunked` is the only coding the host reads.
Extensions on a chunk size are skipped. A trailer line needs a colon;
the host drops the trailers. A transfer-coding with a parameter is
`status-malformed`. RFC 9112 7.1 says chunked defines no parameters
and their presence SHOULD be an error. RFC 9112 7.2 says the same
SHOULD for a compression coding. `chunked;q=1`, `chunked ; q=1`,
`gzip;q=1`, `chu;nked`, and `chu;n=1` are all 400. Accepting the
parameter, or stripping it, would let a proxy disagree about where
the body ends. `chunked` has to be the final coding (RFC 9112 6.1):
`chunked, gzip` is `status-malformed`. `chunked;q=1, gzip`,
`chunked;q=1, chunked`, and `gzip;q=1, chunked` are malformed
because of the parameter. A later `chunked` does not make
`gzip;q=1, chunked` unimplemented.
`Transfer-Encoding` together
with `Content-Length` is `status-malformed` (RFC 9112 6.3). Chunked
on HTTP/1.0 is `status-malformed`. A chunk size that does not fit in
a `U64` is `status-malformed`. A coding other than `chunked`,
including `gzip` and `gzip, chunked`, is `status-unimplemented`.
`gzip;q=1` is malformed, not unimplemented.
The image stores origin 2 and the host copies `httpNotImplemented`.
`host_in_policy` does not return 501 for that coding. It returns
501 only for method code 1. The book does not know the configured
max body. A well-formed `Content-Length` above 16384 whose bytes
have not all arrived is `status-ok`. A `Content-Length` of at most
16384 whose bytes have not arrived is `status-need-more`, including
when that length is above the max. The host answers 413 for a known
length over the max once the headers are done.

Known methods are `GET`, `HEAD`, `POST`, `PUT`, `DELETE`, `OPTIONS`,
and `PATCH`, case-sensitive. `OPTIONS` with target `*` is known. Any
other method with target `*` is `status-malformed`. A well-formed
method outside that set is not malformed. The host answers 501 and
does not call the handler. `Expect: 100-continue` is one value. Any
other Expect value is not malformed. On HTTP/1.1 the host answers
417.

A line without a colon, whitespace before the colon, a bad version,
a missing space, an empty request-target, a second `Content-Length`,
a duplicate `Host`, or a `Content-Length` of 4294967296 or more is
`status-malformed`. `4294967295` is a legal length in the book. The
host does not stream it. A known `Content-Length` over the max body
(default 8 MiB) is 413 at the headers, and the host does not read
that body. A body past 16384 is streamed only up to that max. A
leading `+` is malformed (RFC
9110 8.6). A trimmed digit string of 32 bytes or more is malformed,
so the digit fold does not walk an unbounded value. Space or tab
after the colon is still legal. The request line and the header
lines end in CRLF (RFC 9112 2.2). A bare LF there is
`status-malformed` as soon as that byte is seen, including a buffer
that has not reached a blank line. `headerEnd` returns only the CRLF
block end, so `headersDone` is false, `streamTake` is 0, and the
host does not copy a prefix of that buffer. `OPTIONS *` is a request-target.
`GET  HTTP/1.1` is not. HTTP/1.1 with the headers finished and not
exactly one `Host` is `status-malformed` (RFC 9112). HTTP/1.0 may
omit `Host`. A second `Host` is malformed for both. The previous
challenge server accepted an HTTP/1.1 request that omitted `Host`.
That request is now 400. `Connection` is a comma-separated list of
tokens, compared without regard to ASCII case. `enclose` and
`closeable` do not match `close`. `close, foo` and ` close ` do.
`close` in any `Connection` field closes the connection. A later
`keep-alive` does not override an earlier `close`, and `close` after
`keep-alive` still closes (RFC 9110). `Keep-Alive` on HTTP/1.0 keeps
the connection. A buffer that has not
reached a CRLF header terminator, or that is shorter than a
`Content-Length` of at most 16384, is `status-need-more`. The host
check runs only after the headers are done, so a partial request
that still uses CRLF stays need-more. The function is total.
`framedEnd` is the first byte after this message. `streamTake` is 1
when the host must read the rest from the socket. Both have `refl`s
for a short `Content-Length`, an empty chunk, and the 16384 / 16385
boundary with no body bytes (`stream-take-hi`, `stream-end-hi`,
`stream-take-edge`, `stream-end-edge`). Those terms are the header
block alone. `c/test_classify` checks the same header-only buffers.
`stream-cl` and `cap-cl` are C checks in that file, not book
equations. They name the classify status of those buffers, not a
body of 16385 or 16384 bytes. A body past 16384 bytes is
`c/test_http11` (a response of 1 MiB + 1, and a 20000-byte echo)
and `c/test_hello` (a 20000-byte echo). The hello echo sends the
headers, waits until `muro_http_body_read` is entered, then sends
the body.

Routing lives in the handler. The challenge routes are
`priv/muro/challenge/route.muro` (`routeVerb`, `routePath`, `routeId`,
`routeIdn`). `priv/muro/challenge/decide.muro` is the 46-equation
challenge policy. The request book does not import it.

## One connection step

`priv/muro/http/conn.muro` is one finite step of an HTTP/1 connection.
The host owns the socket, the buffer, and the clock. It calls `step`
and performs the action. The book does not loop. It does not call
`decide`.

`Conn` is unboxed data: thirteen machine words, passed by value. `Ev`
is unboxed. `step` returns the next `Conn`. The phase is read, decide,
write, idle, or closed. Half-closed is a flag. While it is set the
peer has sent FIN: the step may still write the pending response, and
it does not read again. Closed is terminal. Every event returns that
same state. Its action is close, which asks for no read and no write.

Events from the shim are bytes, peer eof, write progress, write done,
write error, an idle tick, and a handler reply. The reply event is
tag 6. Its `n0` is the handler's close-after bit. `n1` is 1 when the
HTTP status is 4xx or 5xx. That forces close, and the host admits
`reply-bad`. Any other status is `reply-ok` with the observe bit
clear. Actions
back are hold, want read, write, close, and call the handler. The
call-handler action is still named `do-decide` and the phase is
still `mode-decide`, so the admission map and the session traces
keep their positions. A write carries a range of length 16. That
range is not the message. The host owns the response bytes. The
book chooses a malformed request: origin 1, keep close, action
write, and the bytes are `httpMalformed`. An unknown transfer
coding is the same write with origin 2, and the bytes are
`httpNotImplemented`. `endOff` is the first byte
after the request. `more` is set when the buffer holds bytes past
that, so the shim can re-step a pipelined tail. The step does not
slice the buffer.

Keep-alive follows the classifier. HTTP/1.1 defaults to keep-alive.
`Connection: close` and HTTP/1.0 close after the response. The
handler may force close. A malformed request writes an error and
then closes. Need-more stays in read, unless the peer has already
sent FIN, which is an error response. An idle tick closes a
connection that is reading or idle, including the first request. A
tick during the handler phase or write holds in the book, and the
host does not deliver one there. A streamed body blocked in
`muro_http_body_read` still closes when that same idle deadline
passes. The wait is the host clock. It is not an image tick. The
echo pull reads the body while the image is in write.
`c/test_http11` `check_body_idle` sees 200 and then a close. A
handler that reads the body does it while the image is in decide.
`examples/hello` returns 400 when that read fails. `host_in_after`
sees the read error and copies `httpMalformed`. `c/test_hello`
`check_decide_idle` sees 400, body `malformed`, and a close.
These servers do not read a body while the image is in read or
idle. `c/test_serve` still runs that skipped tick:
`host_test_skipped_tick` marks the fd skipped, sets the phase to
read and to idle, and calls `on_tick`. It checks that `body_idle`
is set, the deadline is cleared, and the slot stays open. That
call is not a wire exchange.
`conn.muro` imports
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
`conn` from that position: a `run` is not a `conform`. The checker
has `prefix` for a list of directed moves from a position. This book
does not declare one.
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
compares. The prelude is under `MURO__PRELUDE`, so that file includes
`session.h` and `admit.h` in one translation unit.
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
timeout, the connection cap (1 through 1024 per worker), a log-name
prefix, and a worker count (1 through 64, default 1).
It does not carry a database path or a JWT key.
`muro_http_config_init` sets port 8080, 5000 ms, 128 connections,
a max body of 8388608, a drain of 1000 ms, and 1 worker.
`muro_http_config_env` reads `MURO_HTTP_BIND`, `MURO_HTTP_PORT`,
`MURO_HTTP_IDLE_MS`, `MURO_HTTP_MAX_CONNECTIONS`,
`MURO_HTTP_MAX_BODY`, `MURO_HTTP_DRAIN_MS`, `MURO_HTTP_WORKERS`,
and `MURO_HTTP_WAIT`.
`muro_http_config_arg` consumes `--bind`, `--port`, `--idle-ms`,
`--max-connections`, `--max-body`, `--drain-ms`, `--workers`,
`--wait`, and `--help`. A
bind host longer than 15 characters, or one `inet_pton` rejects,
makes `muro_http_serve` return -2. A connection cap outside
1..1024, a worker count outside 1..64, more than one worker on
port 0, a negative max body or drain time, a wait name this OS
cannot open, or a listen failure, returns -1. SIGINT and SIGTERM
drain and return 0. Every successful listen prints
`listening <host> <port>`, including port 0. More than one worker
prints one line per listening socket.

The handler returns 0 to send the response, or -1 to close. Request
slices point at the receive buffer. That buffer stays live while the
host frames the response, after the handler returns. `reason`,
`headers`, and `body` must also stay live until that copy. A pointer
into the handler's stack does not. Extra headers are a block of
`Name: value` lines. A CR or LF in the reason, or a line break that
is not CRLF, is rejected. The host drops `Connection`,
`Content-Length`, `Transfer-Encoding`, and `Date` from that block
and writes its own. Every response gets `Date` in IMF-fixdate. The
host caches that rendering for one second. `close_after` forces
`Connection: close`. A status from 400 through 599 does too.
`owe_status` means a side effect has committed and a status must
still leave the socket. Return -1 with `owe_status` set still sends
the book's 500 before close when no status byte has been written.
Return -1 with `owe_status` clear sends nothing. If the response
does not fit, or the socket closes before any status byte, the host
copies the book's `httpInternal` bytes. It does not choose them. A
malformed request never calls the handler. The host copies
`httpMalformed`. 501, 413, and 417 are the same kind of copy
(`httpNotImplemented`, `httpTooLarge`, `httpExpect`) and do not call
the handler. `HEAD` sets the omit bit: the headers stay, including
`Content-Length` when the length is known, and no body bytes are
written.

`req->body_in` reads the body. A buffered body is already in
`req->body` as well. A streamed body has `req->body.n == 0` and
`muro_http_body_read` pulls the rest, through the server wait
(kqueue, epoll, or poll) when the socket has nothing queued. That
wait accepts and serves
other connections. It also arms the idle deadline while the image
is in decide or write, where the book timer is off. No body byte
before the deadline makes that read return. The caller then closes.
A byte of the body starts a new window. The echo pull is the write
phase: the 200 headers are already written, and
`c/test_http11` `check_body_idle` sees 200 and then a close. A
handler read is the decide phase, before any status byte.
`examples/hello` maps a failed `muro_http_body_read` to 400 with
body `bad\n`. `host_in_after` sees error 400 and copies
`httpMalformed` instead of that handler body, then closes.
`c/test_hello` `check_decide_idle` stalls `Content-Length: 20000`
and sees 400, body `malformed`, and a close. A tick for a socket
already inside that read does not step the image. In read and
idle, as in decide and write, it sets `body_idle` and clears the
deadline, so the next wait cannot arm a fresh window. The wire
tests cover decide and write. These servers do not read a body
while the image is in read or idle. `c/test_serve` still runs
that branch: `host_test_skipped_tick` marks the fd skipped, sets
the phase to read and to idle, and calls `on_tick`. It checks
that `body_idle` is set, the deadline is cleared, and the slot
stays open. That call is not a wire exchange. A nested read does not
watch a socket that is already inside a read, so those bytes stay
in the kernel until the inner read returns. Past eight nested
reads, the wait is `poll(2)` on this socket alone, and the same
deadline still bounds it. If the handler returns without reading a
streamed body, and the response pull will not read it either, the
host closes. That keeps unread body bytes from being parsed as the
next request. `MuroHttpResponse.pull` is the response side. With
`has_length`, the host writes `Content-Length` and then the pull's
bytes. Without a length, HTTP/1.1 is chunked and HTTP/1.0 is the
raw bytes followed by close. `HEAD` does not call `pull`. The
default max body is 8388608. `0` rejects every positive length.
A length equal to the max is accepted, including 0. `--max-body`
and `MURO_HTTP_MAX_BODY` set it. Once the headers are
done and a `Content-Length` is known, `host_in_policy` returns 413
when that length is over the max. The host copies `httpTooLarge`
and closes. It does not read the body to the end first. That holds
for a length of at most 16384 and for a longer one, including
`4294967295`. `c/test_http11` sends a prefix of that body and still
gets 413. A length of at
most 16384 that is within the max stays buffered: the image waits
until those bytes have arrived. A chunk whose size is already in
the buffer at that point, and whose decoded length would pass the
max, is 413 before the handler. A later chunk, read only after the
200 has been written, cannot become a 413. The host stops before
that chunk's data is copied out and closes the connection. The
chunked response has no last-chunk. `c/test_http11` checks that
close.

`Expect: 100-continue` on an otherwise acceptable HTTP/1.1 request
writes `HTTP/1.1 100 Continue` and `Date` before the body is read,
unless the body is already in the buffer or the method is `HEAD`.
A short write leaves the interim outstanding. Until the last byte
is written the socket is not watched for read, so bytes that arrive
during that write, including a pipelined request, stay in the kernel
and are classified together after the interim. The driver waits
until those headers have been taken, then sends the body and the
next request before the interim finishes. The host does not
send 100 on HTTP/1.0, and it does not turn that Expect into 417. Any
other Expect value on HTTP/1.1 is 417 and the body is not read.

SIGINT and SIGTERM set a flag. The loop then stops accepting,
closes idle connections (idle, or read with an empty buffer), and
lets a request that has already reached the handler finish. The
process exits 0 when no connection remains or `drain_ms` has passed.
The default is 1000. `--drain-ms` and `MURO_HTTP_DRAIN_MS` set it,
from 0 through 86400000. A negative max body or drain time makes
`muro_http_serve` return -1. The flag is noticed between waits and
inside `muro_http_body_read`. That arms the drain at once, on that
same turn: the listen socket closes, idle connections close, and
the body read keeps going until the body ends, `drain_ms` passes,
or the idle deadline passes with no further body byte. The process
then exits 0. One worker is that process. Above one worker, the
parent forwards SIGINT and SIGTERM and waits. Each worker drains
itself. The parent returns 0 when every worker exits 0.

`c/host.c` is the socket adapter. It owns the sockets, the buffers,
the connection table, the clock, and the wait. HTTP/1.1 for one
connection (classify, admit, the handler, keep or close, the body
limit, the idle deadline) is `c/step.c`. The adapter reports facts
and writes the span the step returns. That span points at the
connection's own send buffer. `docs/protocol-backend.md` is the
contract. `make c-check` compiles both with
`-Wall -Wextra -Werror -std=c11 -O2`, then runs the drivers. The
loop does not choose a session move. The step writes 100, 413, and
417, and it writes 501 for a method the book marks with method code
1. It copies the book's 400 and the book's unimplemented transfer
coding (origin 2). `host_in_policy` does not answer that coding.
The adapter formats `Date` and passes 29 bytes in. The image
chooses malformed and unimplemented. A partial `100 Continue`
stays unsent until the wait can write the rest, and that socket is
not watched for read while the interim is short. `sent_100` is set
only after the last byte.

`c/host_poll.c` waits with kqueue on macOS and epoll on Linux.
`poll(2)` is compiled on both and is the fallback. `--wait poll`
or `MURO_HTTP_WAIT=poll` selects it. The idle tick is a one-shot
deadline on every backend, armed for the soonest connection
deadline. An earlier ready socket does not move that deadline.
`poll(2)` takes ready events from a rotating cursor, so a wait
that fills its event cap still visits high-index sockets on the
next wait. kqueue and epoll return kernel order. They do not keep
that cursor. A level-triggered fd that did not fit in the batch
stays ready for a later wait. When the idle tick is due, one
slot in the batch is the timer on every backend. An fd event is
not written into that slot. A write-only hangup without
writability is an error and closes that slot; a peer FIN while the
host is still reading becomes `evEof`, and a request that already
arrived can still be answered. The image's timer field says whether
that phase is waiting. A tick during the handler phase or write is
not delivered: the book would hold. While `muro_http_body_read` is
blocked in those phases, the host arms this same deadline.
`c/test_http11` sends `POST /echo` headers with
`Content-Length: 20000` and then no body. The stream server writes
200 and the socket closes after the idle timeout. A socket that
has sent only part of a header, or only part of a body, stays in
the set. The loop keeps serving the other sockets. A close that
still owes a status writes it and returns. That write does not
wait for the peer: `EAGAIN` returns immediately, so one full send
buffer cannot stall the table. The peer may miss that last status
if its window is already full.

The default is one worker. That path does not fork and does not
set `SO_REUSEPORT`. The books, the shim arena, and this loop are
single-threaded, so extra workers are processes, not threads.
Each has its own event loop, connection table, and arena.
`max_conn` is per worker. On Linux each worker binds the same
address with `SO_REUSEPORT` and accepts on it. macOS
`SO_REUSEPORT` does not spread accepted TCP connections, so there
the parent binds once, accepts with `poll(2)`, and passes each
socket to a worker with `SCM_RIGHTS`, round-robin. The listen
backlog is `SOMAXCONN`. If every worker channel is full, the
parent waits until one can take the socket and then scans the
workers again. It does not close an accepted socket because the
workers are busy. The parent's copy stays open until the worker
has received the fd and written one ack byte back. `EINTR` on
that send is retried. A channel that
errors is removed from the rotation. A worker that dies from a
signal or a non-zero status stops the rest of the pool. A drop
that does happen (the process is shutting down, or no worker
channel is left) is counted and written as
`handoff drop N: reason`, and the parent exits with
`handoff drops N`. The workers still use the selected wait. A
count that cannot be set up returns -1. The parent is not an
HTTP worker.

Each accepted socket is one `Conn` and one session. The adapter
turns an accepted fd, arrived bytes, a finished write, a hard write
error, a peer close, and a due deadline into `StepIn`. `c/step.c`
feeds those through `proto_admit` and then `step`, the same order
as `c/test_conn`. A refusal closes the socket. The image does not
run. A read error closes the socket and is not `evError`: that
event holds while the phase is read. A response write error is
`evError`, and the image shuts. A `100 Continue` write error closes
with no `evError`, including when some of that span was already
written. That flag stays set until the last byte is accepted. A
short write is not a fact. The adapter retries the same pointer.

The write range in the image stays length 16. `priv/muro/http/render.muro`
is `httpRender(status, reason, date, extraHeaders, body, keep, omit)`.
The book writes the status line, then `Date`, then `Connection`,
then `Content-Length` or `Transfer-Encoding: chunked` or neither
(close-delimited), then the handler's extra headers, then the body
unless `omit` is 1. `--backend c` emits it. The host copies that
slice into its own buffer and writes it, reports `evProgress` for
the bytes the socket took, then `evDone`. A pulled body is more
than one write: the image stays in write until the last byte, and
`evDone` is that end. Pipelined bytes stay in the receive buffer.
`endOff` and `more` say where the next request starts. For a body
the host will stream, `endOff` is the end of the bytes already
buffered, so that prefix is consumed and not parsed again. The host
does not read the next request while a response is in progress. The
arena resets after each copy, including each chunk. Header walks do
not reset it, so a handler may still hold slices. A buffered
response has to fit the 69632-byte write buffer. A larger body is a
pull. A decoded chunked payload near 64 KiB can fail that copy and
the host closes. That limit is the 69632-byte write buffer. It is
not the stream threshold. Streaming starts above 16384. The receive
buffer is 65536. The stream example uses a pull for a body that
size.

### Reply order per key

`priv/muro/http/family.muro` is a hold rule over a fixed script. It
is not the poll loop. One key is one connection. `accept` opens on
arrival and is never held. `reply` stays held while any earlier
instance of that same key is unresolved. An earlier accept counts,
and so does an earlier reply. Another key does not. The rule does
not order replies across different connections. `--backend fsm` emits
the dispatcher as `MuroHttp.FamilyFsm.Conns`. `--backend c` omits a
family, so `c/host.c` never calls it. The poll loop still admits
one socket through `admitChoice` and `muro__sess_conn_step`.

`mix muro.check` walks one fixed script: key `a` is accept, then
reply, then reply, and key `b` is accept. These lines hold:

- `drained`, on the one-reply meeting: 3 reachable states, 3
  transitions. When the host has taken the reply, its inbox is empty.
- `reply-order`: 3721 reachable states, 10521 transitions. An opened
  reply has every earlier reply of that key, known when it opened,
  resolved.
- `accept-free`: the same 3721 states and 10521 transitions. An
  accept is not held.
- `reply-opens`: the same 3721 states and 10521 transitions. A held
  reply is later opened. This is the script. It has no fairness
  filter.

`reply-order`, `accept-free`, and `reply-opens` share one graph.
3721 and 10521 are a regression pin of that script. 10521 is the
successor table, not a breadth-first parent map. A different count
means the script, the meeting, the hold rule, or the checker
explored a different graph. `test/family_test.exs` runs
`Muro.CLI.Check.run` on `family.muro` and asserts the `drained`
line and the three 3721 / 10521 lines. `mix test` runs that file.
The `check` job runs `mix test`, so those counts are a CI result.
The jobs do not print the counts as their own log line. The walk
is not a proof for every key count, and it is not a bound on live
sockets. The pin proves the hold rule for any number of keys and
any number of instances (`agda/Muro/Family.agda` in muro 0.22.19).
This repository does not contain that proof. The program that
writes the dispatcher is not proved. `test/family_test.exs` also
replays two keys on the emitted
dispatcher, in host order: accept, then reply. An accept on one key
opens while the other key has a live reply. A later reply stays held
until the earlier reply of that key resolves. A session step on a
held reply is refused. Withdrawing the held reply does not open it.
A reply whose turn has finished is reported until `resolved`. A
second send on `reply-out` is refused. That is the emitted machine.

The same pin does not prove three properties of the poll loop.

- `nconn` stays at most `max_conn`. The family has no numeric cap.
  `accept_some` accepts while `nconn < max_conn`. `c/test_serve`
  refuses a cap outside 1..1024. The same program runs
  `host_test_skipped_tick` for a skipped fd in read and in idle.
  That check is the host `body_idle` flag, not this cap.
  `c/test_e2e` fills the table.
- An accepted connection is eventually closed. `reply-opens` is the
  script above. A keep-alive connection stays open. The hold rule
  does not make the host resolve.
- A closed connection never arrives another reply. A later `reply`
  of that key can still open. On one connection, `neg/write_closed.muro`
  is refused, the stepper refuses a send after `end`, `admitChoice`
  on phase closed refuses, and the image returns the closed state.
  `close_slot` drops the fd. Those checks are not the family.

An older reply that arrives after a newer one has opened runs beside
it. The host does not do that. It does not read the next request
while a response is in progress. The formula does not say so.

## Writing a backend

A backend owns the bytes and the clock. It does not own the
HTTP/1.1 decisions. `c/step.h` is the contract.
`docs/protocol-backend.md` lists the facts and the actions.

Bind a recv buffer and a send buffer with `muro_http_step_bind`.
Report one fact at a time: a connection was accepted
(`MURO_STEP_ACCEPTED`), request bytes are already in the recv
buffer (`MURO_STEP_BYTES`), the peer closed
(`MURO_STEP_PEER_CLOSED`), a write of `n` bytes finished
(`MURO_STEP_WROTE`), that write failed hard
(`MURO_STEP_WRITE_FAILED`), or the armed deadline is due
(`MURO_STEP_NOW`). Pass monotonic time and 29 bytes of IMF-fixdate.
The step does not read the clock and does not call `read`,
`write`, `accept`, or `poll`.

Perform what comes back. Write `StepOut.bytes` from the pointer
the step returned. On the keep-alive path that pointer is inside
the send buffer passed to `muro_http_step_bind`. Copying it again
is the regression this split avoids. A short write retries that
same span and reports `MURO_STEP_WROTE` only for the bytes taken.
Arm `wake_at` (0 disarms). Close when `close_conn` is set. If
`status_now` is set, write `status` first without waiting out a
full send buffer.

Readable and writable are not facts. `:gen_tcp` has no readiness
event, and neither does a muro-beam process. Both can still report
"bytes arrived" and "write of n bytes finished".

`examples/adapter/mem.c` is that loop with no socket. `make c-check`
builds it as `c/test_step`. The reference server is still
`c/host.c`. Workers, the listen backlog, and `--wait poll` stay
there. `MURO_HTTP_WORKERS` and `--workers` are 1 through 64. The
default is 1 and does not fork. `--wait poll` or
`MURO_HTTP_WAIT=poll` forces `poll(2)`.

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
0 or like returns 1 or 2. The reopen tests check a clean close and
reopen, not crash or power-loss durability.
A file database whose journal mode is not `wal`, or whose synchronous
value is below `NORMAL`, is not opened. `:memory:` cannot be WAL.
It remains the test-only default, journal mode `memory`, and it is
not a durable database. The path is `--db`, then `MURO_HTTP_DB`,
then `SQLITE_PATH`, then `:memory:`. An empty `--db`, `MURO_HTTP_DB`,
or `SQLITE_PATH` exits 2. Tables are
`posts(id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL)` and
`likes(post_id INTEGER NOT NULL, sub TEXT NOT NULL, PRIMARY KEY(post_id, sub))`.
One connection, `SQLITE_OPEN_NOMUTEX`. Each worker process opens
its own connection. `:memory:` is not shared across workers. A WAL
file is. Create and like render the row
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

Flags `--bind`, `--port`, `--idle-ms`, `--max-connections`,
`--max-body`, `--drain-ms`, `--workers`, `--wait`, and `--jwt-key`
override `MURO_HTTP_BIND`, `MURO_HTTP_PORT`, `MURO_HTTP_IDLE_MS`,
`MURO_HTTP_MAX_CONNECTIONS`, `MURO_HTTP_MAX_BODY`,
`MURO_HTTP_DRAIN_MS`, `MURO_HTTP_WORKERS`, `MURO_HTTP_WAIT`, and
`MURO_HTTP_JWT_KEY`. The database path is `--db`, then `MURO_HTTP_DB`,
then `SQLITE_PATH`, then `:memory:`. The defaults are `127.0.0.1`,
port 8080, 5000 ms, 128 connections, one worker, `:memory:`, and no
key. Every successful listen prints `listening <host> <port>`,
including port 0. SIGINT and SIGTERM drain and exit 0.

## The hello example

`examples/hello/hello.c` is a second server on `c/muro_http.h`.
`make` builds `c/muro-hello`. It does not link SQLite. GET answers
`hello\n`. POST echoes the body. A buffered body is the bytes
already in `req->body`. A streamed body is read with
`muro_http_body_read` into a buffer that outlives the handler, and
that echo is the response. The connection can stay up.
PUT, DELETE, OPTIONS, PATCH, and HEAD are 405 `method\n`. HEAD
carries that `Content-Length` and no body. A method the library
does not know is 501 and does not reach this handler.
`GET /health` is that same hello. It is not the challenge route.
`c/test_hello` checks those responses, a missing `Host` (the book's
400 `malformed`), keep-alive, `--bind 127.0.0.1` winning over
`MURO_HTTP_BIND=nope`, a 20000-byte POST echo that sends the
headers and waits until `muro_http_body_read` is entered before
the rest of the body, and SIGTERM while a 20000-byte body has
only partly arrived (the listen socket closes and the process
exits 0). A bind host that is not IPv4 exits 2 and does not listen.

## The stream example

`examples/stream/stream.c` is a third server, `c/muro-stream`. GET
and HEAD `/` answer `ok\n`. GET and HEAD `/blob` pull 1 MiB + 1 of
`x` with a known length. GET `/live` pulls `abc` with no length, so
HTTP/1.1 is chunked and HTTP/1.0 is close-delimited. HEAD `/live`
closes and writes no body. POST, PUT, and PATCH `/echo` pull the
request body back out. DELETE `/echo` is 204. OPTIONS, including
`OPTIONS *`, answers `Allow`. One pull is in progress at a time.
`c/test_http11` drives this server: methods, `Date`, HEAD, chunked
requests and responses, a body over 1 MiB in each direction, 413,
100-continue, and SIGTERM while a 20000-byte body is still arriving.

`c/test_e2e` starts `c/muro-httpd` on an ephemeral port. It checks
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

The reduction equations in `render.muro` are the `refl`s for the
strings they name: an empty keep response, an empty close response,
204, `HEAD` of an empty body, `httpMalformed`, `httpInternal`,
`httpNotImplemented`, `httpTooLarge`, `httpExpect`, `httpContinue`,
the last-chunk bytes `0\r\n\r\n` from `httpChunk` of an empty body
(`chunk-last`), the chunked header block, the
close-delimited header block, and a declared length of 0.
`last-chunk` reduces `httpLastChunk` to `httpChunk` of an empty body.
The host calls `httpLastChunk`.
`mix muro.gen` typechecks them and reduces each `refl` under the
fuel bound. That reduction is the evidence term. It is not a
separate program, and it is not the poll loop. ExUnit does not run
this book. ExUnit also skips thirty-four `request.muro` equations and
three `conn.muro` equations because a result or an argument normal
form is a machine term. The request names are `crlf-end`, `lf-end`,
`hdr-prefers-crlf`, `meth-head`, `meth-put`, `meth-trace`,
`meth-bad`, `meth-star`, `meth-get-star`, `te-chunk`, `te-gzip`,
`exp-100`, `exp-two`, `ck-empty`, `ck-hi`, `header-count`,
`header-name-0`, `te-not-final`, `te-gzip-code`, `te-chunk-param`,
`te-chunk-ows`, `te-gzip-param`, `te-param-not-final`, `post-framed`,
`post-framed-short`, `post-take-short`, `ck-framed`,
`ck-framed-hdr`, `ck-take-hdr`, `stream-take-hi`, `stream-end-hi`,
`stream-take-edge`, `stream-end-edge`, and `stream-take-lf`.
`te-param-quote` (`chunked;q="a,b"`) and `te-param-empty-q`
(`chunked;q=""`) are C checks in `c/test_classify` and
`c/test_http11`, not book equations. They need a `"` inside the
header, and the book byte strings have none. `c/test_http11`
expects 400 and body `malformed` for both.
`te-param-junk`, `te-param-twice`, and `te-gzip-param-final` are
`statusOf` equations. Each reduces to `status-malformed`, and
`mix test` runs them. `c/test_classify` also checks `teCode` of
those three buffers (code 3). `c/test_http11` checks the 400.
`te-cut-st`
and `te-unknown-st` are `statusOf` equations for `chu;nked` and
`chu;n=1`. Both are `status-malformed`. `c/test_classify` also
checks `teCode` of those two buffers (`te-param-cut`,
`te-param-unknown`), and `c/test_http11` checks the 400 on the
wire. `c/test_classify` checks each skipped request result, and the
failure name is the equation name. `test/route_test.exs` skips
sixteen `route.muro` equations because the result normal form is a
machine term: `route-verb-get`, `route-verb-post`,
`route-verb-other`, `route-health`, `route-feed`, `route-posts`,
`route-id`, `route-like`, `route-tail`, `route-id-42`,
`route-idn-42`, `route-id-zero`, `route-id-abc`, `route-id-lead`,
`route-like-id`, and `route-health-id`. `c/test_route` checks
those results. Its names drop the `route-` prefix. `stream-take-hi` and `stream-take-edge`
are the header-only buffers. `stream-cl` and `cap-cl` are C checks
in `c/test_classify`, not book equations. They name the
classify status of those same buffers, not a body of that size.
The connection names are `unimpl-code` (ExUnit runs `unimpl-origin` instead, and
`c/test_conn` wants origin 2), `after-close`, and
`after-close-tick`. A positive `Content-Length` walks `rdShow`, and that
reduction does not finish, so it is not an equation. `c/test_render`
calls `httpRender`, `httpHead`, `httpChunked`, `httpDelim`,
`httpChunk`, `httpLastChunk`, and the six literals. It checks the
emitted bytes, plus the challenge health bytes, a JSON array, a 405,
a ten-byte body, `HEAD` omit of a non-empty body, one chunk of `hi`,
and the fixed date. That is a test. It does not sweep tag
combinations. `c/test_tags` checks the decide `*From` helpers.

muro 0.21.15 and 0.21.16 add a temporal check and a synthesis cover.
They do not change response bytes or this loop. The pin is 0.22.19.
The per-key reply-order check is `priv/muro/http/family.muro`. It is
a hold rule over a fixed script, not this loop.

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
`render.muro` (`httpRender`, `httpHead`, `httpChunked`, `httpDelim`,
`httpChunk`, `httpLastChunk`, `httpMalformed`, `httpInternal`,
`httpNotImplemented`, `httpTooLarge`, `httpExpect`, `httpContinue`).
`request.muro` also emits `streamTake` and `framedEnd`. The same
backend emits the challenge books `decide.muro`, `route.muro`, and
`body.muro` (`bodyClass`, `textOff`, `textLen`). `--backend fsm`
emits `family.muro` as `MuroHttp.FamilyFsm`. That backend is the one
that emits a keyed family. The C backend omits it, and `c/host.c`
does not call the dispatcher.

Hand-written core: `c/muro_http.h`, `c/step.h`, `c/step.c`,
`c/host.c`, `c/host_poll.c`, `c/host_render.c`, `c/host_date.c`,
`c/host_stream.c`, `c/proto.c`, `c/admit_glue.c`, `c/shim.c`.
`examples/adapter/mem.c` is the socket-free backend. `c/step.c`
calls the books. It does not replace them. A later muro step can
take the same `StepIn` and return the same `StepOut`. The adapter
stays.
Hand-written challenge, under `examples/challenge/`: `httpd.c` (main),
`app.c` (the handler), `route_glue.c`, `store.c` (SQLite), `jwt.c`
(SHA-256, HMAC-SHA256, base64url, the HS256 check), `json_text.c`
(string unescape, the UTF-8 check, and the row JSON), and
`host_body.c` (the call into `bodyClass`). `examples/hello/hello.c`
is the second server. `examples/stream/stream.c` is the third. The
prelude is under `MURO__PRELUDE`.
`c/admit_glue.c` includes `admit.h` and `session.h` together.
`conn.o` already
contains `request.muro`. Do not also link `request.o`. `conn.o`
emits `not`, `and`, and `or` because it imports `std:bool.muro`. A
book linked beside it must not import that book. `decide.muro` and
`route.muro` do not.

## Limits

- The core binds one IPv4 address. The default is `127.0.0.1`.
  `--bind` and `MURO_HTTP_BIND` select it. The stored host is at
  most 15 characters.
- The wait is kqueue on macOS, epoll on Linux, and `poll(2)`
  otherwise. `--wait` and `MURO_HTTP_WAIT` select `poll`, `kqueue`,
  or `epoll` when that backend is compiled. Past eight nested
  `muro_http_body_read` calls, the wait on that socket is `poll(2)`.
- Workers are 1 through 64 processes. The default is 1 and does
  not fork. More than one worker needs a fixed port. `max_conn` is
  per worker. Linux workers share the port with `SO_REUSEPORT`.
  Other systems pass accepted sockets from one listener. The
  backlog is `SOMAXCONN`. A busy handoff waits; it does not drop
  the accepted socket. The parent keeps its copy until the worker
  acks. `handoff drops N` on stderr is the count.
- `addCap` still wraps a `U64`. The driver reports a delta of 8.
- `bc-empty`, `bc-off-empty`, `bc-len-empty`, and `bc-long` reduce.
  They return before `jbScan`. A refl that walks the scanner does not
  finish. One byte of that scan took the checker past 30 GB.
  `c/test_body.c` checks the scanner.
- `render-empty-keep` typechecks (`Content-Length: 0`). `render.muro`
  is emitted as C only, so ExUnit does not run it. A positive length
  walks `rdShow` and is not an equation. `c/test_render` calls
  `httpRender`, `httpHead`, `httpChunked`, `httpDelim`, `httpChunk`,
  `httpLastChunk`, and the six literals. It checks a ten-byte body,
  `HEAD` omit of a non-empty body, one chunk of `hi`, the last-chunk
  bytes `0\r\n\r\n` that `httpLastChunk` returns, and the fixed date
  `Sun, 06 Nov 1994 08:49:37 GMT` in the emitted C. The host clock
  (`gmtime_r`, English names) is not that equation.
- The body book accepts any octet inside a JSON string, including a
  byte that is not UTF-8. The host turns that `body-ok` into
  `body-malformed` before `decide`. A `\u` surrogate
  (U+D800 through U+DFFF) is malformed in the book. Surrogate pairs
  are not decoded.
- A JWT claim name longer than 32 bytes is rejected. Header and
  payload JSON are decoded into a 4096-byte buffer.
- The receive buffer is 65536 bytes and the write buffer is 69632.
  A header block past 16384 is 400. A body past 16384 is streamed,
  up to the max body (default 8 MiB). A known `Content-Length` over
  that max is 413 at the headers, including one of at most 16384.
  The host does not read that body to the end first. A
  buffered response that does not fit the write buffer closes. A
  feed or a read that does not fit closes the socket. Create and like
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
  tests check a clean close and reopen, not crash or power-loss
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

`make c-check` compiles the drivers, `c/muro-httpd`, `c/muro-hello`,
and `c/muro-stream` with `-Wall -Wextra -Werror -std=c11 -O2` and
runs them. It also runs `sh c/check_step_pure.sh` and `c/test_step`.
The script rejects an OS call or an OS header in `c/step.c` and
`c/step.h`. `c/test_step` drives `examples/adapter/mem.c`: two
keep-alive responses whose bytes point into the caller-owned send
buffer, then a peer close of a partial request (400, body
`malformed`). `c/test_hello` runs the hello server. `c/test_http11`
runs the stream server. `c/test_route` and `c/test_tags` check the
challenge route book and the decide constructor tags. `c/test_admit`
is not
in that target. It reads a trace on stdin. It is not a pass/fail
driver.

`c/shim.c` defines `muro__ALLOC` as a bump arena. The classifier
driver links `c/generated/request.c` and the shim. It does not link
`decide.c`. `request.muro` does not import that book. Do not link
`request.o` with `conn.o`: `conn.muro` already imports the request
book, so the symbols would be duplicated.

```sh
cc -Wall -Wextra -Werror -std=c11 -O2 -I c -I c/generated -include c/shim.h \
  -c -o c/test_classify.o c/test_classify.c
cc -Wall -Wextra -Werror -std=c11 -O2 -I c -c -o c/shim.o c/shim.c
cc -Wall -Wextra -Werror -std=c11 -O2 \
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
allocate 0. On muro 0.22.19, as on 0.22.14 and 0.20.42, `classify` of
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
Their headers share `MURO__PRELUDE`. `c/admit_glue.c` includes both
and unpacks the offer.

```sh
cc -Wall -Wextra -Werror -std=c11 -O2 -I c -I c/generated -include c/shim.h \
  -c -o c/test_conn.o c/test_conn.c
cc -Wall -Wextra -Werror -std=c11 -O2 -I c -c -o c/shim.o c/shim.c
cc -Wall -Wextra -Werror -std=c11 -O2 \
  -I c -I c/generated -include c/shim.h \
  -c -o c/conn.o c/generated/conn.c
cc -Wall -Wextra -Werror -std=c11 -O2 \
  -I c -I c/generated -include c/shim.h \
  -c -o c/session.o c/generated/session.c
cc -Wall -Wextra -Werror -std=c11 -O2 \
  -I c -I c/generated -include c/shim.h \
  -c -o c/admit.o c/generated/admit.c
cc -Wall -Wextra -Werror -std=c11 -O2 -I c -I c/generated \
  -c -o c/admit_glue.o c/admit_glue.c
cc -Wall -Wextra -Werror -std=c11 -O2 -I c -I c/generated \
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
cc -Wall -Wextra -Werror -std=c11 -O2 \
  -I c -I c/generated -include c/shim.h \
  -c -o c/session.o c/generated/session.c
cc -Wall -Wextra -Werror -std=c11 -O2 -I c -I c/generated \
  -c -o c/test_sess.o c/test_sess.c
cc -Wall -Wextra -Werror -std=c11 -O2 -I c -c -o c/shim.o c/shim.c
cc -o c/test_sess c/test_sess.o c/session.o c/shim.o
./c/test_sess
```
