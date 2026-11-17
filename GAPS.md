# Compiler gaps

muro 0.21.14. The books check, and the runs emit. The generated C is
not edited. The three limits below were real on muro 0.20.36. They
are closed as of muro 0.20.39. The header record is unboxed as of
muro 0.20.42. On 0.21.14, `classify` still allocates the one
`Classified` result, and the C driver still prints `view-allocs 0`,
`health-allocs 1`, `post-allocs 1`.

## Closed on muro 0.20.39

`match` eliminates `WBool` and `Opt`. A `match` whose result is a
`Slice` initializes that struct, so `dropCR`, `authValue`, and
`bodyView` return the view from the match. Fieldless constructors
initialize the tag and the unused union member. A binder the arm does
not read is not declared. A static function the book does not call is
marked unused.

`c/generated/request.c` and `c/generated/decide.c` compile with
`-Wall -Wextra -Werror -std=c11`. They do not need
`-Wno-missing-field-initializers`, `-Wno-unused-variable`, or
`-Wno-unused-function`.

Word comparisons and byte reads allocate nothing. `methodView`,
`targetView`, `pathView`, and `versionView` are only `sliceUntil`,
`sliceAfter`, and `slice`.

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

`authAt` and `authLen` each call `scan` twice, once for the mark and
once for the offset or the length. `authValue` calls `scan` once.
Those helpers are off the counted `classify` path.

C, Rust, and Zig refuse `Opt` of unboxed data (`c:unboxed`). This book
does not build that option. An `Opt` of a byte or a word is a value.

`idNumber` still matches `id-missing` and `id-present`. `idClassOf`
returns only `id-none`, `id-bad`, or `id-unknown`, so compiling the
generated Elixir warns that those two clauses never match. They stay.
Those constructors are host facts for `decide`.

`mix compile --warnings-as-errors` fails on that generated code.
`MuroHttp.Request.idNumber/1` (`lib/muro_http/request.ex`) and
`MuroHttp.Conn.idNumber/1` (`lib/muro_http/conn.ex`) each warn that
the `:id_missing` and `:id_present` clauses will never match, because
`idClassOf` has type `id-bad or id-none or id-unknown`. This is
pre-existing. It is a muro codegen follow-up: the emitter should not
write a clause for a constructor the function cannot return, or the
book should stop matching constructors `idClassOf` never produces.
This PR does not edit those generated clauses. CI does not pass
`--warnings-as-errors`. `mix test` and `mix muro.gen --check` do not
either.

JSON body class is not this step. That is scope. `classify` returns
`body-none` and the raw body `Slice`. No muro issue was opened.

## The connection stepper, on muro 0.21.14

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
host event (phase, event tag, buffer length, classifier tag, decide
code, observe bit, close-after bit) and the current session position
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
- `reply-ok` or `reply-bad` from a decide code, or no reply while the
  image is still observing
- keep (`send` pick 0) or close (`send` pick 1) after an open reply,
  including a write error
- the `st.at` numbers those predicates use (1, 4, 5, 7, 10, 11, 12)

muro checks the equations in `admit.muro` by reduction. That is not a
check that an offer is a legal prefix of `conn` from `st.at`. A `run`
is not a `conform`. The grammar has no judgment that a list of
directed moves is legal from a position, where the position is a named
continuation or the numeric `at` the C stepper assigns. Until that
judgment exists, `test/sess_agree_test.exs` is the prefix check: it
steps each offer on the emitted stepper and compares the move list
with the previous hand-written map and with
`SessionFsm.Conn.check_trace`. That is conformance. The bounded
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
decide, the write range, keep-alive, the idle timer, the half-close
flag, and the pipelined offset stay in `conn.muro`. The socket and
the event loop are not in this step.

## Emitter gaps this book hit

These are missing muro emitter features. This repository does not
patch murolang/muro for them.

1. No checker judgment that a list of directed moves is a legal prefix
   of a session from a position (a named continuation, or the numeric
   `at` the C stepper assigns). The book's evidence is reduction only.
   The prefix check is the conformance test above.

2. The C prelude structs (`struct muro__nat`, `struct muro__unit`)
   are not under a shared include guard. One translation unit cannot
   include both `c/generated/admit.h` and `c/generated/session.h`.
   The host uses a second translation unit for the unpack. The feature
   needed is one include guard around that prelude.

3. A book cannot name the stepper's C enum. `dir` and `tag` in
   `admit.muro` are copied literals (send 1, recv 2, pick 4, data 8).
   The feature needed is a way for a `run` to name those enums, or for
   the emitter to share them.

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
Classify, decide, the write range, keep-alive, the idle timer, the
half-close flag, and the pipelined offset stay there. A FIN during
decide or write is that flag: the session's half-close is the peer's
choice before the piece. The later keep-or-close choice is a stepper
move, chosen by `admitChoice`. `shut` is the terminal image and has
no live session, so a byte on it is not a new request.

Init of this session takes no nat. A stepper that does takes `uint64_t`,
so a negative cannot be supplied. A wrong key, a bad position, or an
unknown session name returns `MURO_SESS_UNEXPECTED` from the step.
Init of an unknown name returns `MURO_SESS_INIT`.

The three books under `priv/muro/http/neg/` are still refused by
`mix muro.check`. They are not in `muro.exs`.

An unboxed field has to be a machine word, a `WBool`, a `Slice`, an
`Opt` of those, or more unboxed data. `Verb`, `PathShape`, and
`IdClass` are ordinary data, so `Conn` stores their tags as `U64`.

A session payload of `Bytes` or `Slice` is refused. `Piece` and
`Reply` are fieldless data. The buffer stays on the `Ev` of the image.

`Classified` and `Decision` are ordinary data. Each constructor call
allocates one object. A control step allocates 0. A step that
classifies allocates 1. A step that calls `decide` allocates 1.

ExUnit skips a closed equation when an argument's normal form is a
machine term (`argument 1 normal form is a machine term`). `step`
takes the unboxed `Conn`, so `after-close` and `after-close-tick`
are proved by the checker and skipped by `test/conn_eq_test.exs`.
The host suite runs nullary wrappers (`sawAfterDo`, `sawAfterMode`)
whose results are declared data. Those two `Conn` equations are the
ones the wrapper does not cover.
