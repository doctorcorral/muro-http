# Changelog

## 0.1.0-dev — unreleased

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
