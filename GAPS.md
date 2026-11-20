# Compiler gaps

muro 0.22.14. The books check, and the runs emit. The generated C is
not edited. The three limits below were real on muro 0.20.36. They
are closed as of muro 0.20.39. The header record is unboxed as of
muro 0.20.42. On 0.22.14, as on 0.21.14, `classify` still allocates
the one `Classified` result, and the C driver still prints
`view-allocs 0`, `health-allocs 1`, `post-allocs 1`.

## Closed on muro 0.20.39

`match` eliminates `WBool` and `Opt`. A `match` whose result is a
`Slice` initializes that struct, so `dropCR` and `bodyView` return
the view from the match. Fieldless constructors
initialize the tag and the unused union member. A binder the arm does
not read is not declared. A static function the book does not call is
marked unused.

`c/generated/request.c` and `c/generated/decide.c` compile with
`-Wall -Wextra -Werror -std=c11`. They do not need
`-Wno-missing-field-initializers`, `-Wno-unused-variable`, or
`-Wno-unused-function`.

Word comparisons and byte reads allocate nothing. `methodView`,
`targetView`, `pathView`, `queryView`, and `versionView` are only
`sliceUntil`, `sliceAfter`, and `slice`. `queryView` is the bytes
after `?`, or empty when the target has no `?`.

## What classify allocates

`HSt` is `unboxed data`: nine machine words, passed by value. The
`hst` constructor does not call `muro__ALLOC`. The fold rebuilds that
product when a colon or a line end changes phase, flags, or a mark,
and returns the same value on an ordinary byte. Neither allocates.
`classify` reads that one scan.

Comparing a header name, counting leading space, finding the trailing
trim, and reading Content-Length are `sliceFold`s of a machine word.
`muro__ALLOC` is a real bump arena. Those folds do not call it.

The C driver reports `view-allocs 0`. `classify` of `GET /health`
allocates 1 object. `classify` of `POST /posts` with a two-byte body
allocates 1. That object is the `Classified` result. On muro 0.20.40
those counts were 4 and 6: the initial `HSt`, the request-line `HSt`,
the blank-line `HSt` that finishes the headers, and one `Classified`.
The POST added the colon that opens `Content-Length` and the `HSt`
that closes that line. Before that cut they were 201 and 685. On muro
0.20.36 they were 130676 and 519011.

The 201 was 200 copies of the nine-field record, one per byte of each
of eight scans, plus one `Classified`. The 685 was 396 header records,
135 from `toNat` of a header-name length inside `nameEq`, 108 from
`leadCount`, 18 from `trailEnd`, 18 `cl-ok`, 9 `cl-out`, and one
`Classified`.

`subLen` is gone. `byteIndex` subtracts the unconsumed tail directly.
`leadCount` masks with `noti leadStop`. On muro 0.20.40 Elixir emit
needed a helper and a literal so a nested word operation had a word
kind. muro 0.20.42 carries that kind through a name and through a
nested operation.

A `data` record is still one heap object per constructor call. A pair
is still a heap cell. Words, `Slice`, and `unboxed data` are passed by
value. The `sliceFold` step binders are affine, so a step that needs
the accumulator twice calls a helper. The helper does not allocate.

## What remains

`findHeader`, `headerCount`, `headerName`, and `headerValue` walk
the header block a second time. That walk is an unboxed `Hd`. It is
off the counted `classify` path. `classify` itself is status and
keep-alive. It does not return a verb, a path shape, or an id.
HTTP/1.1 with the headers finished requires exactly one `Host`.
HTTP/1.0 may omit it. A duplicate `Host` is malformed for both.
The previous challenge server accepted an HTTP/1.1 request that
omitted `Host`. That request is now 400. Whitespace before a field
colon is malformed on both `classify` and `findHeader`. Space after
the colon is still legal. The host check runs only after the blank
line, so a partial request stays need-more.

C, Rust, and Zig refuse `Opt` of unboxed data (`c:unboxed`). This book
does not build that option. An `Opt` of a byte or a word is a value.

The core no longer has `idNumber`. `MuroHttp.Request` and
`MuroHttp.Conn` do not warn on `:id_missing` or `:id_present`.
`decide.muro` still names those constructors. They are facts the
challenge handler passes in, not results of the classifier. Route
ids come from `routeId` and `routeIdn` in
`priv/muro/challenge/route.muro`. CI does not pass
`--warnings-as-errors`.

`bodyView` is the raw body `Slice`. `priv/muro/challenge/body.muro`
is a separate book. It does not import `decide.muro` or
`std:bool.muro`. A `def` is a global C symbol (gap 6). `conn.o`
already emits `not`, `and`, and `or` from that import, and it
already contains every `def` from `request.muro`. Linking
`request.o` with `conn.o` duplicates those symbols. A book linked
beside `conn.o` must not import `std:bool.muro` and must not import
`request.muro`. `decide.muro` and `route.muro` do neither.
`bodyClass` allocates 0. Four equations reduce, all of them before
the scan. A refl through `jbScan` does not finish. `c/test_body`
checks the scanner. The challenge handler calls `bodyClass` only
for `POST /posts`.

A miss from `findHeader` is the out-of-range empty. The C emitter
turns that into the static empty of the translation unit that
defined `muro__slice_sub`, which is not the including file's copy.
The host treats a header as present when the pointer lies inside
the request buffer, including a zero-length value. Comparing the
pointer with the including file's `muro__empty_bytes` does not
detect a miss.

## The connection stepper, on muro 0.22.14

`--backend c` emits `priv/muro/http/session.muro` as a stepper
(`c/generated/session.c`). That file is generated. The emitted stepper
enforces each move. It does not choose the sequence.

An event the protocol does not allow is refused, and the image does
not run. The actions are the ones `MuroHttp.SessionFsm` takes. muro
checks the emitter by a walk of at most 14 states and depth at most 6,
including events the protocol does not allow. That walk is not a proof.
`test/sess_agree_test.exs` checks this book's emitted stepper against
that machine, including a two-request trace, and checks that
`proto_admit` offers the same move list as `SessionFsm.Conn.check_trace`
for each HTTP scenario. The check is conformance.

The host may offer several moves for one image event. Each move is
one call of `muro__sess_conn_step`.

## The event-to-move map is emitted

`priv/muro/http/admit.muro` is that map. `--backend c` emits it as
`admitChoice` in `c/generated/admit.c`. Do not hand-edit that file.
The book is a total function over machine words. The inputs are the
host event (phase, event tag, buffer length, classifier tag, reply
word, observe bit, close-after bit) and the current session position
(`st.at`, `st.stuck`, `st.key`, and whether the stepper reports done).
The output is an unboxed offer: a refusal, no moves, or up to five
moves. Each move is five words (present, direction, value tag, pick
bit, constructor tag).

The choices that used to be the case split in `c/proto.c` are that
function:

- talk (`send` pick 0) or an idle timeout (`send` pick 1) on a tick
- open (`recv` pick 0) or a half-close (`recv` pick 1), from bytes
  versus EOF, and on a half-close a piece versus a bare FIN
- `piece-req`, `piece-bad`, or `piece-chunk` from the classifier tag
- an immediate `reply-bad` for an open malformed request, or for a
  half-closed need-more or malformed request
- `reply-ok` or `reply-bad` from the reply word, or no reply while
  the observe bit is set. A handler status 400..599 is `reply-bad`.
  The core host leaves the observe bit clear. The challenge observe
  loop does not use it.
- keep (`send` pick 0) or close (`send` pick 1) after an open reply,
  including a write error, and only at the keep-or-close position.
  A write end at talk is a refusal: pick 1 there is the idle timeout
- the `st.at` numbers those predicates use (1, 4, 5, 7, 10, 11, 12)

muro checks the equations in `admit.muro` by reduction. That is not a
check that an offer is a legal prefix of `conn` from `st.at`. A `run`
is not a `conform`. The checker has `prefix` for a list of directed
moves from a position, including the numeric `at` the C stepper
assigns. `admit.muro` does not declare one, so the checker does not
connect `admitChoice` to `session conn`. `test/sess_agree_test.exs`
is that check: it steps each offer on the emitted stepper and
compares the move list with the frozen scenario traces and with
`Conn.check_trace`. That is conformance. The bounded
emitter walk is not a proof. `session.muro` already records the full
traces those offers concatenate as `conform` (keep, close, timeout,
half-close, a bad request, a chunk). The checker does not connect
`admitChoice` to those conforms. Importing `session.muro` into the
emitted admit book would emit the stepper a second time into the same
C file.

The call returns unboxed data, so `admitChoice` does not call
`muro__ALLOC`. The generated file still contains unused nat helpers
that do. The counted image path does not call them.

## What stays hand-written, and why

`c/proto.c` applies the offer. It owns the session state, calls
`muro__sess_conn_step` once per move, records the trace, and rolls
the session back when the stepper refuses. It translates the host's
signed sentinels (`status < 0`, `reply < 0`) into the book's `hasCls`
and `hasRep` words, and it passes `st.at`, `st.stuck`, `st.key`, and
whether the stepper reports done. It does not choose which move to
offer. A shut connection returns before the book is called.

`c/admit_glue.c` unpacks the unboxed offer into a flat list and maps
the constructor tag to the name string the stepper compares
(`piece-chunk`, `piece-req`, `piece-bad`, `reply-ok`, `reply-bad`).
It is not the choice.

`c/shim.c` is the arena. The C test drivers call the image. Classify,
the handler phase, the write range, keep-alive, the idle timer, the
half-close flag, and the pipelined offset stay in `conn.muro`. The
image does not call `decide`. A classify step allocates 1. A reply
step allocates 0.

`c/host.c` is the socket loop behind `c/muro_http.h`. `c/host_poll.c`
waits with `poll(2)`. `examples/challenge/httpd.c` and
`examples/hello/hello.c` are each a `main`. The core owns the file
descriptors, the buffers, the connection table, and the clock. It
performs the action `step` returns. It does not choose a status, a
header, or a body. A read error closes the fd. It is not `evError`.
That event holds in the read phase. A full receive buffer that the
image did not consume is a host limit. Closing it is not a status.
A body declared longer than 16384 is streamed, so it does not sit
in that buffer until the window fills. A header block longer than
16384 is malformed in the book. An unfinished header that fills a
smaller test window still closes with no status.

SQLite, the JWT check, and `decide` live in the challenge handler.
The first `decide` passes `db-unknown` and `like-unknown`, with auth,
body, and the route id already filled. Auth is the HS256 class of
the `Authorization` value, found with `findHeader`. The body class
for `POST /posts` is the emitted `bodyClass`, after the handler has
turned a bad unescape or bad UTF-8 into `body-malformed`. `decide`
returns `act-observe` when a query is still required. The handler
then runs one SQLite statement and calls `decide` again. `id-present`
is that lookup. A failed open, or a failed statement, is `db-down`.
If the second `decide` is still observe, the handler closes. The
stepper sees one reply, with the observe bit clear.

## The response bytes are emitted

`priv/muro/http/render.muro` is `httpRender(status, reason, date,
extra, body, keep, omit)`, plus `httpHead`, `httpChunked`,
`httpDelim`, `httpChunk`, `httpLastChunk`, and the literals
`httpMalformed`, `httpInternal`, `httpNotImplemented`,
`httpTooLarge`, `httpExpect`, and `httpContinue`.
`--backend c` writes `c/generated/render.c`. Do not hand-edit that
file. The book does not import `decide.muro`. Gap 6 is why: a `def`
is a global C symbol with no book prefix. `conn.o` must not be linked
with another copy of those symbols. The challenge maps decide tags
to an HTTP status in `examples/challenge/app.c`.
`examples/challenge/decision_tags.h` copies the constructor numbers.
`c/test_tags` calls the emitted `verbFrom`, `pathFrom`, `idFrom`,
`authFrom`, `bodyFrom`, `likeFrom`, and `dbFrom` helpers and checks
each tag against that header. Status, error, and act have no `*From`
helper, so those header numbers are not checked against the emitter.
The feature that would remove the copy is still gap 6.

`httpRender` writes the status line, `Date`, `Connection`,
`Content-Length`, the handler's extra header block, a blank line,
and the body unless `omit` is 1. `keep` of 1 is `Connection: close`.
Any other keep is `Connection: keep-alive`. Common codes (200, 201,
204, 400, 401, 404, 405, 500, 503) are digit literals. Any other
code goes through `rdShow`. The handler supplies the reason. The
host drops `Connection`, `Content-Length`, `Transfer-Encoding`, and
`Date` from the extra block, and rejects a reason or an extra block
that contains a CR or LF outside a CRLF line ending. The book still
writes `Date`, `Connection`, and `Content-Length`. `httpChunked`
writes `Transfer-Encoding: chunked` instead of a length.
`httpDelim` writes neither. `httpHead` writes a declared length and
no body bytes. `omit` 1 on `httpRender` drops the body and keeps
`Content-Length`.

`render.muro` is emitted as C only, so ExUnit does not run it.
`mix muro.gen` typechecks the empty keep, empty close, 204, `HEAD`
of an empty body, the six dated literals (`httpMalformed`,
`httpInternal`, `httpNotImplemented`, `httpTooLarge`, `httpExpect`,
`httpContinue`), the last-chunk bytes `0\r\n\r\n`, the chunked
header block, the close-delimited header block, and a declared
length of 0. Evidence is not a run. `rdShow` of 0 is the literal
`0`, so an empty body is an equation. A positive length does not
finish, so a ten-byte body is not an equation, and neither is `HEAD`
omit of a non-empty body or a positive chunk. `c/test_render` checks
those in the emitted C, including the fixed date
`Sun, 06 Nov 1994 08:49:37 GMT`. The host
formats the live clock in `c/host_date.c` with `gmtime_r` and
English weekday and month names. That formatter is not a book.

`c/host_render.c` includes `render.h` and copies the slice into the
host buffer, so the `httpRender` signature cannot drift. `conn.h` and
`render.h` both put the prelude under `MURO__PRELUDE`, so one
translation unit can include both. `c/host.c` calls
`host_copy_response`, `host_copy_head`, `host_copy_chunked`,
`host_copy_delim`, `host_copy_chunk`,
`host_copy_last`, and `host_copy_literal`. It does not include
`render.h`. The host then calls `muro_http_reset`.

muro 0.21.15 and muro 0.21.16 do not change the response bytes.
0.21.15 is a temporal check over a keyed family. 0.21.16 ranks a
recursive `State` that does not declare `all-states`. Neither emits
a socket loop. The pin is 0.22.14. The per-key family is below.

## Reply order per key

`priv/muro/http/family.muro` is a hold rule over a fixed script. It
is not `host_poll`. One key is one connection. `accept` is never
held. `reply` is held while an earlier instance of that key is
unresolved. Another key does not wait. The rule does not order
replies across different connections. `--backend fsm` emits
`MuroHttp.FamilyFsm.Conns`. `--backend c` omits the family.
`c/host.c` never calls that dispatcher.

The pin's `agda/Muro/Family.agda` proves the hold rule for any number
of keys and any number of instances. An opened `reply` has every
earlier instance of its connection, known when it opened, resolved,
when that rule is `held while earlier unresolved`. Whether an
instance is held depends only on its own key. A held instance has
some opened instance of its key. The host is not obliged to resolve.
`withdraw` drops a held instance and releases the same way `resolved`
does. This repository does not contain that proof. The program that
writes the dispatcher is not proved.

`mix muro.check` walks a fixed script and prints:

- `temporal drained: holds in 3 reachable states, 3 transitions`
- `temporal reply-order: holds in 3721 reachable states, 10521 transitions`
- `temporal accept-free: holds in 3721 reachable states, 10521 transitions`
- `temporal reply-opens: holds in 3721 reachable states, 10521 transitions`

The three family lines share one graph. 3721 states and 10521
successor-table entries are a regression pin of the script (key `a`
is accept, then reply, then reply; key `b` is accept). A different
count means that graph changed. It is not the proof above, and it
is not a bound on live sockets. `test/family_test.exs` is a second
check: two keys on the emitted dispatcher, in host order (accept
before reply), a refused step on a held reply, a withdraw that does
not open the reply, a finished reply reported until `resolved`, and
one send on the reply turn.

Not proved, and not stated as a formula, because this pin cannot
discharge them for the poll loop:

- `nconn` never exceeds `max_conn`. The family has no numeric cap.
  The host tests that cap.
- An accepted connection is eventually closed. `reply-opens` is the
  script. A keep-alive connection stays in the table.
- A closed connection never arrives another reply. A later `reply`
  of that key can still open. The per-connection refusal is
  `neg/write_closed.muro`, the stepper, `closed-refuses` in
  `admit.muro`, and `close_slot`.

An older reply that arrives late opens beside a newer one that is
already running. The host does not read the next request while a
response is in progress. The formula does not say that.

## Emitter gaps this book hit

These are missing muro emitter features. This repository does not
patch murolang/muro for them.

1. `admit.muro` does not declare `prefix`. The checker can judge a
   list of directed moves from a position. The book's evidence is
   still reduction only. It does not tie `admitChoice` to `session
   conn` at `st.at`. The prefix check for those offers is the
   conformance test above.

2. Closed on this pin. The C prelude structs (`struct muro__nat`,
   `struct muro__unit`) are under `MURO__PRELUDE`. `c/admit_glue.c`
   includes `c/generated/admit.h` and `c/generated/session.h` in one
   translation unit.

3. A book cannot name the stepper's C enum. `dir` and `tag` in
   `admit.muro` are copied literals (send 1, recv 2, pick 4, data 8).
   The feature needed is a way for a `run` to name those enums.
   `c/admit_glue.c` includes `session.h`, compares `dir` and `tag`
   with `MURO_SESS_SEND`, `MURO_SESS_RECV`, `MURO_SESS_PICK`, and
   `MURO_SESS_DATA`, and `_Static_assert`s those enums against the
   book's copied numbers. The flat constructor word is
   `muro__word_session_*`. The glue asserts piece-chunk, piece-req,
   piece-bad, reply-ok, and reply-bad as 0, 1, 2, 3, and 4. The
   per-type tag of reply-ok is 0. The name strings in the glue stay
   literals. The stepper still takes the name, not the word.

4. `muro__sess_conn_step` takes a constructor name string, not the
   data value the book built. The tag-to-string map lives in
   `c/admit_glue.c`. That is stepper ABI glue.

5. `U64` cannot represent the host's signed sentinel `-1`. The book
   takes `hasCls` and `hasRep`. The comparisons `status < 0` and
   `reply < 0` stay in `c/proto.c`. Those comparisons are glue.

6. Each `def` is emitted as a global C symbol with the source name
   and no book prefix. `conn.muro` and `admit.muro` both need the
   event names and the bytes, decide, and write handlers. Linked
   together, those symbols collide. The admit book spells its copies
   `aEv*` and `aOn*`. The feature needed is a per-book prefix, or
   `static` for a `def` the host does not call.

This session is fieldless `Piece` and `Reply`. It has no nested unboxed
payload, no direct nat `case`, no `case` that calls a run with two live
arguments, and no `down`. The emitter accepted the whole protocol.
`admit.muro` does nest unboxed data: an `Offer` holds five unboxed
`Move`s, and `--backend c` returns that offer by value.

`conn.muro` is still the HTTP image. It does not name the session.
Classify, the handler phase, the write range, keep-alive, the idle
timer, the half-close flag, and the pipelined offset stay there. A
FIN during the handler phase or write is that flag: the session's
half-close is the peer's choice before the piece. The later
keep-or-close choice is a stepper move, chosen by `admitChoice`.
`shut` is the terminal image and has no live session, so a byte on
it is not a new request. The handler reply is event tag 6. The
symbol is still `aEvDb`, and the trace word is still `db`. A 2xx is
`reply-ok`. A 4xx or 5xx is `reply-bad` and the image closes. The
observe bit stays clear. The challenge observe loop does not offer
a stepper move.

Init of this session takes no nat. A stepper that does takes `uint64_t`,
so a negative cannot be supplied. A wrong key, a bad position, or an
unknown session name returns `MURO_SESS_UNEXPECTED` from the step.
Init of an unknown name returns `MURO_SESS_INIT`.

The three books under `priv/muro/http/neg/` are still refused by
`mix muro.check`. They are not in `muro.exs`.

An unboxed field has to be a machine word, a `WBool`, a `Slice`, an
`Opt` of those, or more unboxed data. `Conn` does not store a verb
or a path. One unused id word stays 0. The image is thirteen words.
Word 6 is the origin: 0 is a handler response, 1 is the book's
malformed response.

A session payload of `Bytes` or `Slice` is refused. `Piece` and
`Reply` are fieldless data. The buffer stays on the `Ev` of the image.

`Classified` and `Decision` are ordinary data. Each constructor call
allocates one object. A control step allocates 0. A step that
classifies allocates 1. The image does not call `decide`, so a reply
step allocates 0. The challenge handler's `decide` allocates 1, and
the handler resets the arena before the next call.

ExUnit skips a closed equation when an argument's normal form is a
machine term (`argument 1 normal form is a machine term`). `step`
takes the unboxed `Conn`, so `after-close` and `after-close-tick`
are reduction evidence and skipped by `test/conn_eq_test.exs`.
The host suite runs nullary wrappers (`sawAfterDo`, `sawAfterMode`)
whose results are declared data. Those two `Conn` equations are the
ones the wrapper does not cover.
