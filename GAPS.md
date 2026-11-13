# Compiler gaps

muro 0.20.42. Both books check, and both emit. The generated C is not
edited. The three limits below were real on muro 0.20.36. They are
closed as of muro 0.20.39. The header record is unboxed as of muro
0.20.42. `classify` allocates the one `Classified` result.

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

JSON body class is not this step. That is scope. `classify` returns
`body-none` and the raw body `Slice`. No muro issue was opened.
