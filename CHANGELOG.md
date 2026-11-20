# Changelog

## 0.1.0-dev — unreleased

- Pin muro 0.22.14. The library version stays `0.1.0-dev`.
  `mix muro.gen` rewrites the committed C and Elixir. The C prelude
  is under `MURO__PRELUDE`. `c/admit_glue.c` includes `session.h` and
  `admit.h` together and asserts the flat constructor words
  (`piece-chunk` through `reply-bad` are 0 through 4).
- `priv/muro/http/family.muro` is one key per connection in the poll
  loop. `accept` is never held. `reply` is held while an earlier
  instance of that connection is unresolved. `--backend fsm` emits
  `MuroHttp.FamilyFsm`. `--backend c` omits the family, so `c/host.c`
  is not the dispatcher. `mix muro.check` reports `drained` in 3
  states and 3 transitions, and `reply-order`, `accept-free`, and
  `reply-opens` each in 3721 states and 10521 transitions. That walk
  is the fixed script. The pin proves the hold rule for any number of
  keys and instances. This repository does not contain that proof.
  `test/family_test.exs` checks the emitted dispatcher on two
  connections. Not proved: `nconn` at most `max_conn`, an accepted
  connection eventually closing, and a closed connection never
  arriving another reply. An older reply that arrives late can run
  beside a newer one. The host does not read the next request while
  a response is in progress.
- HTTP/1.1 with the headers finished and not exactly one `Host` is
  400. The previous challenge server accepted a request that omitted
  `Host`. HTTP/1.0 may still omit it. A second `Host` is malformed
  on both.
- A field name with whitespace before the colon is 400. `classify`
  and `findHeader` both reject it, so `Content-Length : 2` is not a
  body length on one path and a header on the other. Space after the
  colon is still legal.
- A handler status from 400 through 599 is `reply-bad` and closes
  the connection. A 2xx or 3xx stays `reply-ok` and can keep it.
- The host rejects a reason or an extra header block that would
  break CRLF framing, and drops handler `Connection`,
  `Content-Length`, and `Transfer-Encoding`. The book still writes
  `Connection` and `Content-Length`. Response pointers must stay
  valid until that copy, which is after the handler returns.
- `muro_http_serve` refuses `max_conn` outside 1..1024. Every
  successful listen prints `listening <host> <port>`, including
  port 0.
- The HTTP/1.1 core is the books under `priv/muro/http/` and the C
  host behind `c/muro_http.h`. `classify` returns status and
  keep-alive. Method, target, path, query, version, headers, and body
  are zero-copy slices. HTTP/1.1 with the headers finished requires
  exactly one `Host`. The image calls a handler (`Request -> Response`)
  instead of `decide`. The host owns sockets, buffers, the connection
  table, the clock, and `poll`. It does not choose a status, a header,
  or a body. `httpRender` takes a status, a reason, extra headers, a
  body, and keep. `--bind` and `MURO_HTTP_BIND` select the IPv4 listen
  address. The default remains `127.0.0.1`.
- The challenge server is `examples/challenge/` (`decide.muro`,
  `route.muro`, `body.muro`, SQLite, JWT, JSON). `c/muro-httpd` is
  still that binary and still serves `/health`, `/feed`, `/posts`, and
  `/posts/:id/like`. `examples/hello/hello.c` is a second server on
  the same header: GET answers `hello\n`, POST echoes the body, and
  there is no SQLite. `c/test_hello`, `c/test_route`, and `c/test_tags`
  run from `make c-check` and `make c-sanitize`. The pin stays
  0.21.14. The library version stays `0.1.0-dev`.
- A write-only `POLLHUP` without `POLLOUT` closes that slot instead
  of spinning the poll loop (the macOS case when a peer aborts
  mid-response). A peer FIN while the host is still reading is still
  `evEof`, so a request that already arrived can be answered.
  `host_poll_wait` scans ready fds from a rotating cursor so a full
  event cap does not starve high-index sockets. `write_status`
  stays best-effort and non-blocking: `EAGAIN` returns immediately.
  `c/test_poll` checks the absolute timer, an empty wait, hangup
  and error flags (including write-only interest), and scan fairness.
  `c/test_e2e` aborts a client mid-response and still serves the next
  socket, and it finishes the paused POST in the interleaved case.
- `c/host_poll.c` waits with `poll(2)` on macOS and on Linux. The idle
  tick is that poll timeout, armed for the soonest connection
  deadline. One socket that has sent only part of a header, or only
  part of a body, does not stall the others. A close that still owes
  a status writes it without waiting on that socket. `c/test_e2e`
  keeps the earlier cases and adds the interleaved clients (`/health`,
  authenticated `/feed`, `/posts`, `/posts/:id/like`, a malformed
  request) plus SIGINT and SIGTERM while a partial header is open.
  The macOS CI job runs the same `make c-check`. The pin stays
  0.21.14. The library version stays `0.1.0-dev`.
- A 500 sent because a connection closes after a committed create or
  like, before any status byte, says `Connection: close`.
- `priv/muro/http/body.muro` is `bodyClass`, `textOff`, and
  `textLen`. `--backend c` emits them. Four equations reduce: the
  empty body, and a body past 1024 bytes, all of which return before
  the scan. A refl that walks the scanner does not finish.
  `c/test_body` checks the scanner, and checks that those calls
  allocate 0. The host unescapes a `body-ok` value and rejects
  ill-formed UTF-8 before `decide`.
- `priv/muro/http/render.muro` is the HTTP/1.1 response bytes.
  `--backend c` emits `httpRender`, now with the row bytes. The host
  copies that slice and writes it. It does not choose the status or
  the body. Eight equations are reduction evidence (`refl`), including
  an empty row (`application/json`, `Content-Length: 0`). A positive
  length walks the decimal printer, and that reduction does not
  finish. `c/test_render` checks the eight strings, `[]`, a ten-byte
  row, the emitted `*From` tag helpers, and `Content-Length` for 1664
  tag combinations. That sweep is a test. The `_Static_assert`s in
  that driver only pin `c/decision_tags.h` to itself. The book does
  not import `decide.muro` (see `GAPS.md`, gap 6). Health render
  allocates 2. An empty row allocates 5. A two-byte row allocates 8.
  A ten-byte row allocates 10.
- `c/muro-httpd` is a hand-written loop. It binds `127.0.0.1`, waits
  with kqueue or epoll, and feeds bytes, peer eof, write progress,
  write done, write error, an idle tick, and db facts through
  `proto_admit` and `step`. A refusal closes the socket. Facts are
  SQLite rows, an HS256 check of the `Authorization` value (key from
  `--jwt-key` or `MURO_HTTP_JWT_KEY`, constant-time MAC compare), and
  the emitted body class. Feed, create, and like rows come from the
  store. The host builds read JSON only. `c/test_e2e`
  checks health, a malformed request, keep-alive, a pipeline,
  `Connection: close`, the idle timeout, a peer FIN mid-request, a
  split body, a short write, the connection cap, a full receive
  buffer, and every challenge route against a live database,
  including a path that cannot be opened. `c/test_jwt`, `c/test_json`,
  and `c/test_store` check the hand-written pieces. Those are tests.
  A failed connection watch closes that socket. A failed listen watch
  stops the process. muro 0.21.15 and 0.21.16 are not useful here.
  The pin stays 0.21.14. The library version stays `0.1.0-dev`.
- `priv/muro/http/admit.muro` is the event-to-move map. `--backend c`
  emits it as `admitChoice`. `c/proto.c` applies that offer and calls
  the stepper. It does not choose the moves. `c/admit_glue.c` unpacks
  the offer. muro checks the map by reduction. It does not check that
  an offer is a legal prefix of `conn` from `st.at`.
  `test/sess_agree_test.exs` does that check by conformance: frozen
  scenario traces and `Conn.check_trace`.
- Pin muro 0.21.14. `--backend c` emits `priv/muro/http/session.muro`
  as a stepper. Each move is one `muro__sess_conn_step` call. The HTTP image in
  `conn.muro` stays:
  classify, decide, write ranges, keep-alive, the idle timer, the
  half-close flag, and a pipelined offset. That image does not name
  the session. A FIN while a reply is already in progress is the
  image's half flag. The session chooses half-close before the piece.
  The later keep-or-close choice is a move on the stepper.
- The stepper's actions are the ones the state machine takes. muro
  checks the emitter by a walk of at most 14 states and depth at most
  6, including events the protocol does not allow. That walk is not a
  proof. `test/sess_agree_test.exs` checks the emitted C stepper
  against `MuroHttp.SessionFsm`, including legal traces and refused
  off-protocol events. A two-request keep-alive is deeper than that
  walk. There is no `down` step. This session's init takes no nat. A
  wrong key, a bad position, or an unknown session name is
  `MURO_SESS_UNEXPECTED` on the step. Init of an unknown name is
  `MURO_SESS_INIT`. Classifier counts stay at views 0, `GET /health`
  1, and a two-byte `POST /posts` 1. The library version stays
  `0.1.0-dev`. There is no Hex publish.
- Pin muro 0.21.6. The classifier counts stay where 0.20.42 left them:
  views 0, `GET /health` 1, a two-byte `POST /posts` 1.
- One finite step of an HTTP/1 connection. `Conn` and `Ev` are unboxed.
  `step` classifies, asks the shim to call `decide`, writes a fixed
  range, and then keeps the connection, closes it, or arms the idle
  timer. Half-close is a flag. Pipelined bytes are an offset the shim
  re-steps. On that pin, C emitted no session stepper, so `session.muro`
  was evidence: conforms, meets, an fsm trace beside `step`, and three
  negative books that `mix muro.check` refuses. A control step
  allocates 0. Classify allocates the one `Classified`. Decide
  allocates the one `Decision`.
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
