# Changelog

## 0.1.0-dev — unreleased

- SIGINT and SIGTERM write a self-pipe. A wait that already
  passed the stop-flag check still wakes, and the parent reaps
  on that pipe plus SIGCHLD instead of a blocking `waitpid`
  that can miss the signal. The library version stays
  `0.1.0-dev`. The pin stays muro `0.22.14`.
- `make c-conformance` runs `c/test_conformance.py` against
  `c/muro-hello` on epoll. The library version stays
  `0.1.0-dev`. The pin stays muro `0.22.14`.
- A header block past 16384 bytes is 414 when the request line
  itself passes that cap, and 431 otherwise. A shorter malformed
  request stays 400. `examples/adapter/mem.c` checks the two
  statuses. The library version stays `0.1.0-dev`. The pin stays
  muro `0.22.14`.
- A short write of `100 Continue` keeps a flag for the rest of
  that span. A later hard error closes the connection and does
  not report `MURO_STEP_WRITE_FAILED`. An idle tick while that
  span is still outstanding closes and drops it. `muro_http_step`
  resets its pump budget on every call, so a one-byte write
  window on a large body does not trip the fuse. A short response
  write does not keep the previous idle deadline.
  `MURO_TEST_100_STALL` still clears when the socket already has
  bytes queued. `c/check_step_pure.sh` allows whitespace before
  `(`, rejects a wider set of calls and headers, and checks
  undefined symbols in `c/step.o`. `examples/adapter/mem.c`
  covers those paths. `c/test_http11` waits up to 10 seconds for
  the server to exit, and `c/test_workers` waits up to 10 seconds
  for SIGTERM. A process that has already exited is reaped on
  the next poll. The library version stays `0.1.0-dev`. The pin
  stays muro `0.22.14`.
- HTTP/1.1 for one connection lives in `c/step.c`
  (`muro_http_step`). The socket loop in `c/host.c` is an adapter:
  it reports accepted, bytes already read, a write of `n` bytes, a
  hard write failure, peer close, and the clock, then writes the
  span the step returns. That span points at the connection's send
  buffer, the 100 Continue area, or the close area. The keep-alive
  path does not copy the response a second time. `c/step.c` does
  not call `read`, `write`, `accept`, `poll`, `kqueue`, `epoll`,
  `fork`, or the clock. `make c-check` runs `c/check_step_pure.sh`
  and `c/test_step` (`examples/adapter/mem.c`). The books, the
  admit wall, and `c/host_render.c` stay. Workers, `--wait poll`,
  and the listen backlog stay in the adapter.
  `docs/protocol-backend.md` is the contract. The library version
  stays `0.1.0-dev`. The pin stays muro `0.22.14`.
- The listen backlog is `SOMAXCONN`. On macOS the parent still
  passes accepted sockets with `SCM_RIGHTS`. It retries `EINTR`,
  and a full worker channel blocks the parent until some worker
  can take the fd. The parent scans every live channel again
  after that wait. It does not close an accepted socket because
  the workers are busy, and it keeps its copy until the worker
  acks the receipt. A channel error removes that worker from
  rotation. The parent reaps children with `waitpid(WNOHANG)`. A
  signalled or non-zero worker exit stops the pool, as on Linux.
  A real drop is logged (`handoff drop N: reason`) and the parent
  prints `handoff drops N` when it exits. `MSG_CTRUNC` on the
  worker's `recvmsg` is an error: the fd is not adopted, and a
  complete fd in that buffer is closed.
- kqueue applies one filter per `kevent` and retries `EINTR`. A
  failed second filter is rolled back. If that undo also fails,
  both filters are deleted and the watch row is dropped, so a
  leftover knote cannot outlive the row. A failed delete still
  drops the watch row, so a reused fd number gets `EV_ADD`.
  `poll`, kqueue, and epoll reserve a slot for a due idle tick
  instead of overwriting an fd event. `c/test_poll` checks that
  slot, fd reuse, an injected `EINTR`, a partial kevent change,
  and a failed rollback. `c/test_workers` runs on `poll` and on the native wait.
  It includes a close-mode flood with `workers > 1` and a
  `SIGKILL` of one worker.
- `bench/loadgen` includes `sys/time.h`, defines `_DEFAULT_SOURCE`,
  and sleeps with `nanosleep`, so `make bench/loadgen` builds with
  `-std=c11 -Werror` on Linux. `errors` is the sum of
  `err_connect`, `err_write`, `err_read`, and `err_parse`.
  `make c-check` compiles the tool and does not run it.
- The wait is kqueue on macOS and epoll on Linux. `poll(2)` stays
  compiled and is selected with `--wait poll` or `MURO_HTTP_WAIT=poll`.
  The default is the native backend. The contract stays
  level-triggered: a one-shot deadline, `ms == 0` disarms, a ready
  socket does not move the deadline, and a wait returns at most 64
  events. `poll(2)` still scans from a rotating cursor. kqueue and
  epoll return kernel order and do not invent that cursor.
  `c/test_poll` runs the timer, empty-set, hangup, and scan checks
  on the default backend and on `poll`. Linux `check` and `asan`
  exercise epoll. The macOS job exercises kqueue. Past eight nested
  `muro_http_body_read` calls the wait on that socket is still
  `poll(2)`.
- `MuroHttpConfig.workers` is 1 through 64. The default is 1.
  `--workers` and `MURO_HTTP_WORKERS` set it. `max_conn` stays per
  worker. One worker is the single process and does not set
  `SO_REUSEPORT`. Above that, Linux workers each bind with
  `SO_REUSEPORT`. Other systems bind once in the parent and pass
  accepted sockets with `SCM_RIGHTS`. macOS `SO_REUSEPORT` does not
  load-balance TCP. SIGINT and SIGTERM stop the workers. Each
  worker drains itself. `c/test_workers` checks one worker, two
  workers, the poll fallback, a refused count, and SIGTERM. The
  pin stays 0.22.14. The library version stays `0.1.0-dev`.
- HTTP/1.1 methods. `GET`, `HEAD`, `POST`, `PUT`, `DELETE`, `OPTIONS`,
  and `PATCH` reach the handler. `OPTIONS *` is in that set. Any other
  method with target `*` is 400. A well-formed method outside the set
  (`TRACE`, `CONNECT`) is 501 and does not reach the handler. A token
  that is not a method is 400. `HEAD` keeps the headers the handler
  built, including `Content-Length`, and the host writes no body.
- Every response carries `Date` in IMF-fixdate. The host caches one
  rendering per second. A handler `Date` is dropped. The library still
  owns `Connection`, `Content-Length`, and `Transfer-Encoding`.
- `Transfer-Encoding: chunked` is a request body. Extensions on a
  chunk size are ignored. Trailers need a colon and are dropped. A
  transfer-coding with a parameter is 400. RFC 9112 7.1 says
  chunked defines no parameters and their presence SHOULD be an
  error. RFC 9112 7.2 says the same SHOULD for compression codings.
  `chunked;q=1`, `chunked ; q=1`, `gzip;q=1`, `chu;nked`, and
  `chu;n=1` are all 400. Accepting the parameter, or stripping it,
  would let a proxy disagree about where the body ends. A bare
  `chunked;` (`te-param-empty`) or `chunked;@@@` (`te-param-junk`)
  is the same 400. `chunked;q=1, chunked` (`te-param-twice`) and
  `gzip;q=1, chunked` (`te-gzip-param-final`) are
  `status-malformed` as well. The parameter is bad framing, so a
  later `chunked` does not make the coding unimplemented. `mix test`
  reduces those `statusOf` equations. `c/test_classify` checks
  `teCode` 3 for the same buffers. `c/test_http11` checks the 400.
  `te-param-quote` (`chunked;q="a,b"`) and `te-param-empty-q`
  (`chunked;q=""`) stay C checks in `c/test_classify` and
  `c/test_http11`. They need a `"` inside the header, and the book
  byte strings have none.
  A chunk size that
  does not fit in a `U64` is 400. `Transfer-Encoding`
  together with `Content-Length` is 400 (RFC 9112 6.3). `chunked`
  has to be the final coding: `chunked, gzip` is 400 (RFC 9112 6.1).
  `chunked;q=1, gzip` is 400 because of the parameter.
  Any other coding, including `gzip` and `gzip, chunked`, is
  `status-unimplemented` in `classify`. `gzip;q=1` is malformed,
  not unimplemented. The image stores origin 2
  and the host copies `httpNotImplemented`. `host_in_policy` does
  not return 501 for that coding. It returns 501 only for method
  code 1.
  Chunked on HTTP/1.0 is 400. A handler that does not
  know the length sends chunked to an HTTP/1.1 client and
  close-delimited bytes to an HTTP/1.0 client. `HEAD` of an unknown
  length does not call the pull and does not send
  `Transfer-Encoding`.
- A request line or header line that ends in a bare LF is 400
  (RFC 9112 2.2), including a buffer that has not reached a blank
  line. `headerEnd` is the CRLF block end only, so `streamTake`
  stays 0 and the host does not copy a prefix of that buffer.
  An empty request-target is 400. `OPTIONS *` stays a target.
  `Content-Length` is `1*DIGIT` and must be below 4294967296.
  `4294967295` is legal. A leading `+`, `4294967296`, `8589934592`,
  and a trimmed digit string of 32 bytes or more are 400.
  `Connection` matches whole comma-separated tokens, without regard
  to ASCII case. `close` in any field closes the connection, in
  either order with `keep-alive` (RFC 9110). `enclose` and
  `closeable` are not `close`.
- A `Content-Length` above 16384 that has not all arrived, and a
  chunked body that does not finish inside 16384 bytes, is streamed.
  `MuroHttpResponse.pull` writes a response the same way.
  `muro_http_body_read` is the request side. The default max body is
  8 MiB (`--max-body`, `MURO_HTTP_MAX_BODY`). Above that is 413 at
  the headers, including `Content-Length: 4294967295`, and the host
  does not read that body to the end. `0` rejects every positive
  length. A length equal to the max is accepted. The receive buffer
  stays 65536. A header block past 16384 is still 400.
  A buffer that fills while the image still needs bytes closes with
  no status. A chunk size that shows up only after the 200 has been
  written cannot be a 413. The host closes, and the chunked response
  has no last-chunk.
- `Expect: 100-continue` on HTTP/1.1 writes `100 Continue` before the
  body is read, when the request is otherwise acceptable and the body
  is not already in the buffer. A short write or `EAGAIN` leaves the
  interim unsent. `poll` retries it and does not watch that socket
  for read until the last byte is written, so a pipelined request
  stays in the kernel instead of being held in `HANDLE`. `sent_100`
  is set only after the last byte. Any other Expect value is 417.
  HTTP/1.0 does not send 100 and does not send 417.
- SIGINT and SIGTERM stop accepting, finish the request that is
  already in the handler, close idle keep-alive connections, and exit
  0 when the table is empty or `--drain-ms` has passed (default 1000,
  `MURO_HTTP_DRAIN_MS`). The same flag arms that drain inside
  `muro_http_body_read`: the listen socket closes immediately, idle
  connections close on that same turn, and the body read continues
  until the body ends, the drain deadline passes, or the idle
  deadline passes with no further body byte. A stalled streamed
  body closes after the idle timeout. That deadline is armed while
  the image is in decide or write, where a book tick would hold.
  A tick for a socket already inside that read does not step the
  image. In read and idle, as in decide and write, it sets
  `body_idle` and clears the deadline, so the next wait cannot arm
  a fresh window. The echo pull reads during write, after the 200
  headers are out. `c/test_http11` `check_body_idle` sends headers
  with `Content-Length: 20000`, then silence, and sees 200 and a
  close. A handler that reads during decide has not sent a status
  yet. `examples/hello` returns 400 with body `bad\n` when
  `muro_http_body_read` fails. `host_in_after` sees that read error
  and copies `httpMalformed` instead of the handler body, then
  closes. `c/test_hello` `check_decide_idle` uses the same stalled
  `Content-Length: 20000` and sees 400, body `malformed`, and a
  close. These servers do not read a body while the image is in
  read or idle. `c/test_serve` still runs that skipped tick.
  `host_test_skipped_tick` marks the fd skipped, sets the phase
  to read and to idle, and calls `on_tick`. It checks that
  `body_idle` is set, the deadline is cleared, and the slot stays
  open. That call is not a wire exchange.
  While that read waits, the server poll accepts and serves other
  connections. A nested read does not watch a socket already inside
  a read. Past eight nested reads, the wait is that socket alone.
- `examples/stream` serves a known-length pull, a chunked pull, and
  an echo of the request body. `c/test_http11` drives it.
- `mix test` reduces these `statusOf` equations in `request.muro`.
  `opt-star` (`OPTIONS *`) is `status-ok`. `c/test_http11` `options`
  sees 200. `meth-star` is the skipped `methodCode` equation (0),
  and `c/test_classify` checks that code. `get-star` (`GET *`) is
  `status-malformed`. `c/test_http11` `get-star` sees 400.
  `meth-get-star` is the skipped `methodCode` equation (2), checked
  by `c/test_classify`. `te-ok` (an empty chunked body, `0\r\n\r\n`)
  is `status-ok`. `ck-empty` is the skipped `ckDone` equation for
  that body. `mix test` does not reduce it. `c/test_classify`
  checks `ck-empty`. `te-cl-bad` (`Transfer-Encoding` with
  `Content-Length`) is `status-malformed`. `c/test_http11` `te-cl`
  and `cl-te` see 400. `te-http10` (chunked on HTTP/1.0) is
  `status-malformed`. `c/test_http11` `te-http10` sees 400.
  `te-param-st` (`chunked;q=1`), `te-gzip-param-st` (`gzip;q=1`),
  `te-param-empty` (a bare `chunked;`), `te-cut-st` (`chu;nked`),
  `te-unknown-st` (`chu;n=1`), `te-param-junk` (`chunked;@@@`),
  `te-param-twice` (`chunked;q=1, chunked`), and
  `te-gzip-param-final` (`gzip;q=1, chunked`) are
  `status-malformed`. `c/test_http11` sees 400 for those buffers
  (`te-param`, `te-param-gzip`, `te-param-empty`, `te-param-cut`,
  `te-param-unknown`, `te-param-junk`, `te-param-twice`,
  `te-gzip-param-final`). The `teCode` equations `te-chunk-param`,
  `te-chunk-ows`, `te-gzip-param`, and `te-param-not-final` are
  skipped. `c/test_classify` checks those codes. ExUnit skips a
  closed equation whose result or argument normal form is a machine
  term. `test/request_test.exs` skips these 34: `crlf-end`,
  `lf-end`, `hdr-prefers-crlf`, `meth-head`, `meth-put`,
  `meth-trace`, `meth-bad`, `meth-star`, `meth-get-star`,
  `te-chunk`, `te-gzip`, `exp-100`, `exp-two`, `ck-empty`, `ck-hi`,
  `header-count`, `header-name-0`, `te-not-final`, `te-gzip-code`,
  `te-chunk-param`, `te-chunk-ows`, `te-gzip-param`,
  `te-param-not-final`,
  `post-framed`, `post-framed-short`, `post-take-short`,
  `ck-framed`, `ck-framed-hdr`, `ck-take-hdr`, `stream-take-hi`,
  `stream-end-hi`, `stream-take-edge`, `stream-end-edge`, and
  `stream-take-lf`. `test/route_test.exs` skips sixteen
  `route.muro` equations: `route-verb-get`, `route-verb-post`,
  `route-verb-other`, `route-health`, `route-feed`, `route-posts`,
  `route-id`, `route-like`, `route-tail`, `route-id-42`,
  `route-idn-42`, `route-id-zero`, `route-id-abc`, `route-id-lead`,
  `route-like-id`, and `route-health-id`. `c/test_route` checks
  those results. Its names drop the `route-` prefix (`verb-get`
  through `health-id`). `c/test_classify` checks the 34 skipped
  `request.muro` results, and the failure name is the equation
  name. `stream-take-hi` and
  `stream-take-edge` are header-only buffers. `stream-cl` and
  `cap-cl` are C checks in `c/test_classify`, not book equations.
  They name the classify status of those buffers, not a body of
  that size. A body past 16384 bytes is `c/test_http11` and
  `c/test_hello`. The checks also include `streamTake` of a short
  `Content-Length`, a chunked header with no chunk bytes, and a
  bare LF. `test/conn_eq_test.exs` skips `unimpl-code`,
  `after-close`, and `after-close-tick`. `unimpl-origin` is the
  flag ExUnit runs for origin 2. `c/test_conn` wants that field to
  be 2. ExUnit runs the wrappers `sawAfterDo` (`after-close-do`),
  `sawAfterTick` (`after-close-tick-do`), and `sawAfterMode`
  (`after-close-mode`).
  `render.muro` is emitted as C only, same as before. Its `refl`s
  typecheck under `mix muro.gen`, which reduces each `refl`
  under the fuel bound: `HEAD` of an empty body, the
  100/400/413/417/500/501 literals, the chunked and close-delimited
  header blocks, a declared length of 0, and the last-chunk bytes
  `0\r\n\r\n` from `httpChunk` of an empty body (`chunk-last`).
  The fixed date `Sun, 06 Nov 1994 08:49:37 GMT` is an argument of
  those refls. There is no separate Date equation.
  `last-chunk` reduces `httpLastChunk` to that `httpChunk`. The host
  calls `httpLastChunk`. The reduction is the evidence term. It is not a
  separate program, and it is not the poll loop. ExUnit does not run
  that book. A positive `Content-Length` or chunk size walks
  `rdShow` or `hxShow` and is not an equation. `c/test_render`
  calls `httpRender`, `httpHead`, `httpChunked`, `httpDelim`,
  `httpChunk`, `httpLastChunk`, and the six literals. It checks
  those bytes, including `HEAD` of `ok` and one chunk of `hi`.
  The clock, the poll loop, the drain, and a body past the window
  are hand-written. `c/test_http11` and `make c-sanitize` check
  those. `c/test_hello` checks a body past the window: it sends
  the headers, waits until `muro_http_body_read` is entered, then
  sends the rest.

- Pin muro 0.22.14. The library version stays `0.1.0-dev`.
  `mix muro.gen` rewrites the committed C and Elixir. The C prelude
  is under `MURO__PRELUDE`. `c/admit_glue.c` includes `session.h` and
  `admit.h` together and asserts the flat constructor words
  (`piece-chunk` through `reply-bad` are 0 through 4).
- `priv/muro/http/family.muro` is a hold rule over a fixed script,
  not the poll loop. One key is one connection. `accept` is never
  held. `reply` is held while an earlier instance of that key is
  unresolved. Another key does not wait. The rule does not order
  replies across different connections. `--backend fsm` emits
  `MuroHttp.FamilyFsm`. `--backend c` omits the family, so `c/host.c`
  never calls the dispatcher. `mix muro.check` reports `drained` in 3
  states and 3 transitions, and `reply-order`, `accept-free`, and
  `reply-opens` each in 3721 states and 10521 transitions. Those
  three share one graph. The counts are a regression pin of the
  script. `test/family_test.exs` runs `Muro.CLI.Check.run` on
  `family.muro` and asserts those four lines. `mix test` runs that
  file, and the `check` job runs `mix test`, so the counts are a CI
  result. The jobs do not print the counts as their own step. The
  pin proves the hold rule for any number of keys and instances.
  This repository does not contain that proof.
  `test/family_test.exs` also checks the emitted dispatcher on two keys,
  accepting before it replies. Not proved: `nconn` at most
  `max_conn`, an accepted connection eventually closing, and a closed
  connection never arriving another reply. An older reply that
  arrives late can run beside a newer one. The host does not read
  the next request while a response is in progress.
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
  table, the clock, and `poll`. It writes `100 Continue`, 413, and
  417, and it copies the book's 400 and unimplemented 501. It does
  not invent a status the book did not name. `httpRender` takes a
  status, a reason, extra headers, a body, and keep. `--bind` and
  `MURO_HTTP_BIND` select the IPv4 listen address. The default
  remains `127.0.0.1`.
- The challenge server is `examples/challenge/` (`decide.muro`,
  `route.muro`, `body.muro`, SQLite, JWT, JSON). `c/muro-httpd` is
  still that binary and still serves `/health`, `/feed`, `/posts`, and
  `/posts/:id/like`. `examples/hello/hello.c` is a second server on
  the same header: GET answers `hello\n`, POST echoes the buffered
  or streamed body, and a body read that fails is 400 `bad\n`.
  There is no SQLite. `c/test_hello`, `c/test_route`, and `c/test_tags`
  run from `make c-check` and `make c-sanitize`. The pin is
  0.22.14. The library version stays `0.1.0-dev`.
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
  The macOS CI job runs the same `make c-check`. The pin is
  0.22.14. The library version stays `0.1.0-dev`.
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
  copies that slice and writes it. The handler supplies the status
  and the body for a request the book accepted. The host also writes
  the book's 100, 400, 413, 417, and 501 literals. The current
  `refl`s are listed at the top of this unreleased section. A positive
  length walks the decimal printer, and that reduction does not
  finish. `c/test_render` calls `httpRender`, `httpHead`,
  `httpChunked`, `httpDelim`, `httpChunk`, `httpLastChunk`, and the
  six literals. It checks a ten-byte body, `HEAD` omit of a non-empty
  body, one chunk of `hi`, the last-chunk bytes `0\r\n\r\n`, and the
  fixed date `Sun, 06 Nov 1994 08:49:37 GMT`. It does not sweep tag
  combinations and it does not call the decide `*From` helpers.
  `c/test_tags` checks those helpers against `decision_tags.h`.
  Classify allocation counts are in `c/test_classify`: one for
  `GET /health` and one for a two-byte POST. The book does not
  import `decide.muro` (see `GAPS.md`, gap 6).
- `c/muro-httpd` is a hand-written loop. It binds `127.0.0.1`, waits
  with `poll(2)`, and feeds bytes, peer eof, write progress,
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
  The pin is 0.22.14. The library version stays `0.1.0-dev`.
- `priv/muro/http/admit.muro` is the event-to-move map. `--backend c`
  emits it as `admitChoice`. `c/proto.c` applies that offer and calls
  the stepper. It does not choose the moves. `c/admit_glue.c` unpacks
  the offer. muro checks the map by reduction. It does not check that
  an offer is a legal prefix of `conn` from `st.at`.
  `test/sess_agree_test.exs` does that check by conformance: frozen
  scenario traces and `Conn.check_trace`.
- Pin muro 0.22.14. `--backend c` emits `priv/muro/http/session.muro`
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
