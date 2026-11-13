# muro-http

`muro-http` is a separate library. It is not part of muro's standard
library and it is not a book inside murolang/muro. A muro web server
will live here. This repository is version `0.1.0-dev`.

It pins the released Hex package muro `0.20.40` (`{:muro, "~> 0.20.40"}`
in `mix.exs`, `muro: "0.20.40"` in `muro.exs`). It does not depend on a
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
like, and db facts from the host. This step does not call it.

## What this step does not do

- JWT HMAC verification. The classifier records the raw `Authorization`
  value. `AuthClass` is a host input to `decide`.
- JSON body class. A POST body is the raw `Slice` plus the length.
  `BodyClass` from `classify` is `body-none`. That is deferred scope,
  not a failed JSON walker.
- The connection session, the event loop, sockets, epoll, kqueue, and
  SQLite. `c/shim.c` is a bump arena and a slice wrap.

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
0.20.40, `classify` of `GET /health` allocates 4 and `classify` of
`POST /posts` with a two-byte body allocates 6. See `GAPS.md`.
Method, path, authorization, and body views still point at the
original buffer, or at the out-of-range empty when the span is absent.
