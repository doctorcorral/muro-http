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
(`c/generated/session.c`). That file is generated. `c/proto.c` is not.
It is a hand-written host mapping from one HTTP event to a sequence
of protocol moves. The emitted stepper enforces each move. It does
not choose the sequence.

An event the protocol does not allow is refused, and the image does
not run. The actions are the ones `MuroHttp.SessionFsm` takes. muro
checks the emitter by a walk of at most 14 states and depth at most 6,
including events the protocol does not allow. That walk is not a proof.
`test/sess_agree_test.exs` checks this book's emitted stepper against
that machine, including a two-request trace, and checks that
`proto_admit` offers the same move list as `SessionFsm.Conn.check_trace`
for each HTTP scenario. The check is conformance.

The host may offer several moves for one image event. Each move is
one call of `muro__sess_conn_step`. The map in `c/proto.c` is what
picks them:

- talk (`send` pick 0) or an idle timeout (`send` pick 1)
- open (`recv` pick 0) or a half-close (`recv` pick 1), and on a
  half-close a piece versus a bare FIN
- `piece-req`, `piece-bad`, or `piece-chunk` from the classifier tag
- an immediate `reply-bad` for an open malformed request, or for a
  half-closed need-more or malformed request
- `reply-ok` or `reply-bad` from a decide code, or no reply while the
  image is still observing
- keep (`send` pick 0) or close (`send` pick 1) after an open reply,
  including a write error

Those choices are host policy. They are not in the emitted stepper.

A follow-up could put that case split in a muro book and emit the C.
`session.muro` already records the legal sequences as `conform`
(keep, close, timeout, half-close, a bad request, a chunk). It does
not see the image event, the classifier tag, or the decide code.
`conn.muro` is the image and does not name the session. A third book
could take those host facts and name the moves. muro 0.21.14 does not
emit that book, and this repository does not contain it. `c/proto.c`
stays hand-written until that exists. Do not read it as generated.

This session is fieldless `Piece` and `Reply`. It has no nested unboxed
payload, no direct nat `case`, no `case` that calls a run with two live
arguments, and no `down`. The emitter accepted the whole protocol.

`conn.muro` is still the HTTP image. It does not name the session.
Classify, decide, the write range, keep-alive, the idle timer, the
half-close flag, and the pipelined offset stay there. A FIN during
decide or write is that flag: the session's half-close is the peer's
choice before the piece. The later keep-or-close choice is a stepper
move. `shut` is the terminal image and has no live session, so a byte
on it is not a new request.

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
