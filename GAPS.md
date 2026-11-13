# Compiler gaps

muro 0.20.36. Both books check, and both emit. The generated C is not
edited. Three emitter limits show up when that C is compiled, and one
missing eliminator is why `classify` allocates.

## No match on WBool or Opt

`bytesEq`, `sliceEq`, `eqi`, `lti`, and `lei` return `WBool`.
`sliceGet` and `bytesGet` return `Opt U8`. `match` accepts a declared
`data` type or `Nat` only. There is no `wcond`.

This file is the whole repro:

```
def pick : run Π (b : WBool) → Nat :=
  λ (b : WBool) →
    match b motive (λ _ → Nat)
      | wtrue => 0
      | wfalse => (suc 0)

def unwrap : run Π (o : Opt U64) → U64 :=
  λ (o : Opt U64) →
    match o motive (λ _ → U64)
      | wsome n => n
      | wnone => (toU64 0)
```

`mix muro.check` on muro 0.20.36 prints:

```
gap.muro:1:1: pick body: expected data type, got WBool
gap.muro:7:1: unwrap body: expected data type, got Opt U64
```

What the classifier does instead:

- `u8eq` is `toNat (subi x y)` and a `Nat` match. Equal bytes allocate
  one `muro__zero()`. Unequal bytes allocate at most 256 successors,
  because `U8` subtraction wraps modulo 256.
- `U64` equality and order walk eight bytes (`eqGo`, `ltGo`). `toNat`
  of a `U64` difference would be a chain near 2^64. A closed
  `toNat (toU64 19)` does emit, as 19 `muro__suc` allocations rather
  than a C integer.
- Header state is a `data` record (`HSt`). A product cannot be matched.
  `sliceFold` of that record allocates one object per byte. `classify`
  also allocates one `Classified`, and several projections each rescan.
- `methodView`, `targetView`, `pathView`, and `versionView` are only
  `sliceUntil`, `sliceAfter`, and `slice`. Those allocate nothing.

The C driver reports `view-allocs 0`. On the same run, `classify` of
`GET /health` allocated 130676 objects and `classify` of
`POST /posts` with a two-byte body allocated 519011. `muro__ALLOC` is
a real bump arena. Method, path, authorization, and body still point
into the original buffer.

## A Slice match emits `= 0`

A `match` whose result is a `Slice` is emitted as a temporary
initialized with `0`. `muro__slice` is a struct, so that is a type
error. Pointer and word results accept `0`. This was the body of
`dropCR` before the workaround:

```
def dropCR : run Π (+ s : Slice) → Slice :=
  λ (+ s : Slice) →
    match (isEmpty s) motive (λ _ → Slice)
      | true => s
      | false => s
```

`cc -Wall -Wextra -Werror -std=c11` on the generated file reports:

```
error: initializing 'muro__slice' with an expression of incompatible type 'int'
  muro__slice muro__t_0 = 0;
```

The book matches a `U64` instead (`crFlag`, `authAt`, `authLen`,
`bodyAt`, `bodyLen`) and calls `slice` outside the match.

## -Wextra on fieldless constructors and unused binders

Fieldless constructors are emitted as `{tag}`:

```
static Status muro__obj_Status_statuszdmalformed = {1};
```

`-Wmissing-field-initializers` (part of `-Wextra`) rejects the missing
union field.

A binder written `_` is encoded `zu` and then unused
(`-Wunused-variable`). Matching one field of `HSt` binds the other
eight, which are also unused. Constructors the classifier never calls
(`id-missing`, `id-present`, every `AuthClass`, the unused `BodyClass`
tags, `LikeFact`, `DbFact`) are `static` and trip `-Wunused-function`.

`c/test_classify.c` and `c/shim.c` compile with `-Wall -Wextra -Werror`.
`c/generated/request.c` adds `-Wno-missing-field-initializers`,
`-Wno-unused-variable`, and `-Wno-unused-function`. See `README.md`.

JSON body class is not this step. That is scope. `classify` returns
`body-none` and the raw body `Slice`. No muro issue was opened.
