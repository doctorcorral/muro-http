# Compiler gaps

muro 0.20.40. Both books check, and both emit. The generated C is not
edited. The three limits below were real on muro 0.20.36. They are
closed as of muro 0.20.39. What still allocates is the header record.

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

## What classify still allocates

The scan returns the same `HSt` on an ordinary byte. It allocates a
new record at the start, and again only when a colon or a line end
changes phase, flags, or a mark. `classify` reads that one scan.
Comparing a header name, counting leading space, finding the trailing
trim, and reading Content-Length are `sliceFold`s of a machine word.
`muro__ALLOC` is a real bump arena. Those folds do not call it.

The C driver reports `view-allocs 0`. `classify` of `GET /health`
allocates 4 objects. `classify` of `POST /posts` with a two-byte body
allocates 6. On the same muro 0.20.40, before this cut, those counts
were 201 and 685. On muro 0.20.36 they were 130676 and 519011.

`GET /health` is the initial record, the request-line record, the
blank-line record that finishes the headers, and one `Classified`.
The POST adds the colon that opens `Content-Length` and the record
that closes that line. Body bytes after the headers return the record
already built. The 201 was 200 copies of the nine-field record, one
per byte of each of eight scans, plus one `Classified`. The 685 was
396 header records, 135 from `toNat` of a header-name length inside
`nameEq`, 108 from `leadCount`, 18 from `trailEnd`, 18 `cl-ok`,
9 `cl-out`, and one `Classified`.

A `data` record is one heap object per constructor call. A pair is
also a heap cell. Words and `Slice` are passed by value, and the
`sliceFold` step binders are affine, so a step that needs the
accumulator twice calls a helper. The helper does not allocate. An
unboxed product of machine words, or a `sliceFold` that threads
several words, would drop the per-line records to zero. That is a
muro language limit. No muro issue was opened.

JSON body class is not this step. That is scope. `classify` returns
`body-none` and the raw body `Slice`.
