# Changelog

## 0.1.0-dev — unreleased

- Pin muro 0.20.42. The header scan is an unboxed product of machine
  words, passed by value. `GET /health` is 1 allocation and a two-byte
  `POST /posts` is 1, down from 4 and 6. The leftover is `Classified`.
  Views stay at 0.
- Pin muro 0.20.40. The classifier matches `WBool` and `Opt`, and
  `dropCR`, `authValue`, and `bodyView` return a `Slice` from the match.
- `mix muro.gen --check` again. It type-checks each book once and
  compares the committed C, Elixir, and ExUnit files. The one-shot
  check script is gone.
- Generated C compiles with `-Wall -Wextra -Werror -std=c11` and
  without the three `-Wno` suppressions.
- On muro 0.20.40 the header scan allocated a record only when a colon
  or a line end changed state, and `classify` read that one scan.
  `GET /health` was 4 allocations and a two-byte `POST /posts` was 6,
  down from 201 and 685. Views stayed at 0.
- First-order request classifier over a `Slice` of the receive buffer.
  Method, path shape, id token, keepalive, Content-Length, and a raw
  Authorization view. Malformed and need-more are constructors.
- The challenge `decide` book, unchanged, with its 46 evidence equations.
- C emit of both books and a small driver. Slice views allocate nothing.
  `classify` itself allocates; see `GAPS.md`.
- No Hex publish.
