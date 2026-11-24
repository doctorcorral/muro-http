# Protocol step and backends

The HTTP/1.1 decisions for one connection live in `c/step.c`. A
backend reports facts and performs the actions the step returns. It
does not classify, admit, or choose a status. The reference backend
is `c/host.c` (poll, kqueue, or epoll, worker processes, the
`SCM_RIGHTS` handoff). `examples/adapter/mem.c` is a second backend
with no socket. It is the pattern, not a second server.

The books stay where they were. `priv/muro/http/request.muro`,
`conn.muro`, `session.muro`, `admit.muro`, and `render.muro` do not
name a socket. `c/proto.c` is still the hand-written admit map.
`c/host_render.c` still copies one emitted response into a caller
buffer. `host_in_policy`, `host_in_bind`, `host_in_capture`,
`host_in_after`, and `host_in_take_next` in `c/host_stream.c` still
do not call the kernel. This split does not rewrite those pieces.
What moved is the loop that used to sit in `c/host.c`: phase,
keep-alive or close, the body limit, 100 Continue, and the idle
deadline. `c/host.c` now turns an accepted fd, arrived bytes, a
finished write, a peer close, and the clock into `StepIn`, then
writes the span `StepOut` points at.

## Facts and actions

`c/step.h` is the contract. A fact is not kernel readiness. Readable
and writable are the adapter's problem. The step never sees them.

```c
typedef struct StepIn {
  int kind;
  size_t n;
  uint64_t now_ms;
  uint64_t deadline;
  const uint8_t *date;
  size_t date_n;
  int empty_before;
} StepIn;
```

`kind` is one of:

| value | meaning |
| --- | --- |
| `MURO_STEP_ACCEPTED` (1) | a connection was accepted; buffers are already bound |
| `MURO_STEP_BYTES` (2) | request bytes are already in the recv buffer |
| `MURO_STEP_PEER_CLOSED` (3) | the peer closed; bytes already read stay |
| `MURO_STEP_WROTE` (4) | the adapter accepted `n` bytes of the span |
| `MURO_STEP_WRITE_FAILED` (5) | that write failed hard |
| `MURO_STEP_NOW` (6) | monotonic time is `now_ms` and the armed deadline is due |

`n` is the byte count for `MURO_STEP_WROTE`. `now_ms` is the
adapter's monotonic clock. `deadline` is the absolute monotonic
millisecond already armed, or 0. `date` is 29 bytes of IMF-fixdate.
The step does not call `time` or `gmtime_r`. `empty_before` is 1
when the recv buffer was empty before this arrival. The reference
adapter sets it on `MURO_STEP_BYTES` and `MURO_STEP_PEER_CLOSED`.

```c
typedef struct StepOut {
  const uint8_t *bytes;
  size_t len;
  int read_more;
  int write_more;
  int interim;
  int cont;
  int close_conn;
  int status_now;
  const uint8_t *status;
  size_t status_len;
  uint64_t wake_at;
  int note;
  int note2;
  int note_phase;
  int note_ev;
  size_t note_n;
  size_t note_n2;
} StepOut;
```

`bytes` and `len` are the span to write. The pointer is into storage
the connection already owns. `read_more` 1 means the step still
needs request bytes. The adapter turns that into a read interest.
It is not a readiness event. `write_more` is set while a span is
outstanding. `interim` 1 means no byte of this `100 Continue` has
been written yet: the adapter does not read, and it may stall
before the first byte. `cont` 1 means the span is `100 Continue`
for its whole life, including after a short write. A hard error
while `cont` is set closes and is not `MURO_STEP_WRITE_FAILED`.
`wake_at` 0 disarms. Otherwise it is an
absolute monotonic millisecond. `close_conn` 1 means close.
`status_now` 1 means write `status` first, without waiting out a
full send buffer, then close. That pointer is the close area
(`HttpStep.spare`) or the send buffer.

`muro_http_step` returns 0 when `out` is filled, and -1 when the
connection must be closed (admit refused, fuel, setup). `note` may
be set either way. The reference adapter prints the same lines the
loop printed before this split:

| note | line |
| --- | --- |
| `MURO_STEP_NOTE_ADMIT` (1) | `admit refused phase %d ev %d` |
| `MURO_STEP_NOTE_TOO_BIG_COMMIT` (2) | `response is %zu bytes after commit; sending 500` |
| `MURO_STEP_NOTE_TOO_BIG` (3) | `response is %zu bytes` |
| `MURO_STEP_NOTE_HANDLER_500` (4) | `handler failed after a commit; sending 500` |
| `MURO_STEP_NOTE_FUEL` (5) | `pump fuel exhausted` |

A short write is not a fact. The adapter remembers the pointer and
the length and retries that same span. It reports `MURO_STEP_WROTE`
only for bytes the peer's window took. `EINTR` retries the write
and does not call the step again. `EAGAIN` returns to the wait.
One successful `write` is one `MURO_STEP_WROTE`, then one
`evProgress`, matching the old pump.

`100 Continue` that fails hard closes without `MURO_STEP_WRITE_FAILED`.
That is the whole span: `cont` stays 1 after a short write, and the
adapter closes instead of reporting the failure. A response-body
write that fails hard is `MURO_STEP_WRITE_FAILED`,
which the step feeds as `evError`. A read error, a watch failure,
or a full recv buffer with no progress closes without a fact. If
the step still owes a status, `muro_http_step_close_status` returns
the rendered buffer or the book's 500. The adapter writes that
best-effort and does not wait for the peer.

`HttpStep` is the C state for one connection. A new backend uses
`StepIn` and `StepOut`. It does not read the image fields.
`read_more` and `write_more` on `HttpStep` are pointers at the
adapter's own ints. The step writes them while a handler or a
response pull is still on the stack, so a nested
`muro_http_body_read` sees the interest the image has reached.
`park_interim` does not write those live ints. It only fills
`StepOut.read_more` and `StepOut.write_more`. The adapter applies
them when it parks (a short write, or the test stall). A 100 that
the socket takes in one write leaves the previous read interest
until the handler runs.

Phases are the image phases: read 0, decide 1, write 2, idle 3,
closed 4.

## What moved, and what stayed

Moved into `c/step.c`: the per-connection pump. That is classify
through `conn.step`, `proto_admit`, the handler call, keep-alive
consume, pipelined `more`, 100 Continue bytes, 400 / 413 / 414 /
431 / 417 / 501 while still reading, the idle-deadline decision,
and the choice of which buffer a close still owes.

Stayed in the adapter (`c/host.c`, `c/host_poll.c`):

- `poll`, kqueue, epoll, and the nested `poll(2)` inside
  `host_io_once` after eight body reads
- `read`, `write`, `accept`, `close`, `listen`, `bind`, `fork`
- `sendmsg` / `recvmsg` and the `SCM_RIGHTS` handoff
- the listen backlog (`SOMAXCONN`), `SO_REUSEPORT` on Linux
- signals, drain, and the connection table. SIGINT and SIGTERM
  also write a self-pipe, so a wait that already passed the flag
  check still wakes. The parent reaps on that pipe and on
  SIGCHLD.
- logging (`fprintf`). The step returns a note code.
- `MURO_TEST_WRITE_LIMIT`, `MURO_TEST_WRITE_FAIL_AFTER`,
  `MURO_TEST_100_STALL`, `note_write`
- the clock. `drive` calls `host_date_fill` and `host_poll_now_ms`
  once per turn, not once per partial write.

Stayed, and not rewritten:

- `c/generated/*` from the books
- `c/proto.c` and `c/admit_glue.c` (the admit wall)
- `c/host_render.c` (response bytes)
- `host_in_policy`, `host_in_bind`, `host_in_capture`,
  `host_in_after`, `host_in_take_next`
- `c/host_date.c` (the adapter calls it; the step receives the
  29 bytes)

`c/step.c`, `c/step.h`, and the quoted headers they include
(followed transitively) must not include `unistd.h`,
`sys/socket.h`, `poll.h`, `sys/event.h`, `sys/epoll.h`, `fcntl.h`,
`time.h`, `sys/time.h`, `sys/mman.h`, `sys/select.h`, `sys/uio.h`,
or `sys/ioctl.h`. `make c-check` runs `sh c/check_step_pure.sh`.
The script rejects those includes and a call, with whitespace
allowed before `(`, to `read`, `write`, `pread`, `pwrite`,
`readv`, `writev`, `send`, `recv`, `accept`, `kqueue`,
`epoll_wait`, `poll`, `ppoll`, `select`, `fork`, `mmap`, `ioctl`,
`syscall`, `gettimeofday`, `usleep`, `sleep`, and the same family.
It also rejects an undefined symbol in `c/step.o` that is not
`memcpy`, `memmove`, `memset`, `memcmp`, `malloc`, `free`,
`realloc`, `calloc`, `strlen`, or `bzero`, a sanitizer or stack-check
symbol, or a symbol defined by the books, the admit wall, or the
render and stream helpers. `inject_next` frees a pipelined side
buffer after a streamed body. That body can keep the connection,
so `free` is on that path. A keep-alive `GET /` whose response is
already in the send buffer does not call `free`.

## Keep-alive, no second copy

One keep-alive `GET /` on the reference adapter:

1. `read` into the connection's recv buffer (`HttpStep.in`,
   65536 bytes, allocated once in `adopt_fd`).
2. `muro_http_step` with `MURO_STEP_BYTES`. The step classifies,
   calls the handler, and `host_copy_response` writes the status
   line into `HttpStep.out` (69632 bytes, also allocated once).
   That copy from the bump arena is the same copy the host made
   before this split. The arena resets after it.
3. `StepOut.bytes` points at `out + out_off`. `len` is the bytes
   not yet accepted. The step does not allocate a second buffer
   and does not `memcpy` those bytes.
4. The adapter `write`s from that pointer.
5. `MURO_STEP_WROTE` with the count the socket took. The step
   reports `evProgress`, then `evDone` when the span is finished,
   consumes the request, and returns `read_more` with no bytes.

`100 Continue` points at `HttpStep.cont` (160 bytes inside the
step). A close that still owes 500 points at `HttpStep.spare`
(512 bytes) or at `out` when the response was already rendered.
`EAGAIN` retries the same pointer. The test stall
(`MURO_TEST_100_STALL`) sleeps in the adapter and does not call
the step.

The deadline lives on the slot. The step returns `wake_at`. The
adapter stores it when it parks back to the wait, not after a
write that completed inside the same turn. `on_readable` clears
the slot deadline before the step. A one-shot 100 that the socket
takes immediately does not arm a deadline until the following read
park, which is what the old `hold_100` did. A short response write
returns `wake_at` 0. The deadline armed for the previous idle or
`100 Continue` is not copied onto that write. The body-wait
deadline is the adapter's, armed while a pull is on the stack, and
the park runs only after that wait has returned.

## Writing a backend

Bind the two buffers, then loop:

1. Fill a `StepIn`. On accept, `kind` is `MURO_STEP_ACCEPTED`,
   `now_ms` is the monotonic clock, and `date` is 29 bytes. The
   buffers are already the ones passed to `muro_http_step_bind`.
2. Call `muro_http_step`. On -1, close.
3. If `close_conn` is set, write `status` when `status_now` is set,
   then close. Do not wait for a full send buffer.
4. If `bytes` is set, write that span. On a short write, remember
   the pointer and call again later with `MURO_STEP_WROTE` and the
   count actually taken. On a hard error while `cont` is set, close
   and do not report a fact. On any other hard error, call again
   with `MURO_STEP_WRITE_FAILED`.
5. Otherwise arm `wake_at` and wait until bytes arrive, the peer
   closes, or the deadline is due. Report `MURO_STEP_BYTES`,
   `MURO_STEP_PEER_CLOSED`, or `MURO_STEP_NOW`. Put new request
   bytes in the recv buffer before `MURO_STEP_BYTES`. Set
   `empty_before` from the length before that read.

`:gen_tcp` and a muro-beam process can use the same six facts.
Neither one has kqueue readiness. The step does not grow an
`EvWritable` input for them.

`examples/adapter/mem.c` does this in one process. `make c/test_step`
links it. The keep-alive check requires `StepOut.bytes` to lie
inside the caller-owned send buffer. A `100 Continue` span points
at `HttpStep.cont`, including after a short write. The fixed date
`Sun, 06 Nov 1994 08:49:37 GMT` is injected and must appear in the
response. A partial request plus `MURO_STEP_PEER_CLOSED` must close
with 400 and `malformed`. The same driver checks a one-byte accept,
`MURO_STEP_WRITE_FAILED` on a response, a hard error after a short
`100 Continue`, `MURO_STEP_NOW` while that span is pending,
`status_now` for 413, 417, and 501, two pipelined GETs,
`empty_before` 0, and a body of 100001 bytes under a one-byte
window.

The reference server is still `c/muro-hello` through `c/host.c`.
The memory adapter is not on that path.

Workers stay a host concern. The default is 1: no fork, no
`SO_REUSEPORT`. `--workers N` (1 through 64) and `MURO_HTTP_WORKERS`
set the count. Above 1, Linux binds each worker with
`SO_REUSEPORT`. Other systems bind once and pass fds with
`SCM_RIGHTS`. `--wait poll` or `MURO_HTTP_WAIT=poll` forces
`poll(2)`. The default wait is kqueue on macOS and epoll on Linux.

## Step 2

`priv/muro/http/step.muro` is a checked slice of `muro_http_step`.
`mix muro.gen` emits it as `c/generated/step_slice.c` with
`c_prefix` `http_step_`. The host still calls the handwritten
function in `c/step.c`. `c/host.c` is unchanged. The slice does not
call the kernel or the clock.

The book covers one read: the request line, the headers, and a
Content-Length body. While that read is still open it closes with
`status_now` on 413, 417, and 501. It owes a write span for 400,
for an unknown transfer coding (501), and for 413, 417, or 501
after classify has already finished. A finished request the policy
accepts is `framed`. The host still runs the handler and renders
the response into the caller send buffer. The book does not render
those bytes.

Once the book has closed, every later fact is ignored. No second
span. `priv/muro/http/neg/step_span.muro` claims a span after the
413 close. It is not in `muro.exs`. `mix muro.check --fuel 5000000`
on that file refuses `span-after-close`. `mix test` does not load
it: the load checks `step.muro`, then checks the flattened book
again, and that second pass does not finish inside the suite.
`c/test_step_slice` feeds the same buffer to both functions
on that slice. After a `status_now` close, a later fact on
`c/step.c` used to emit another span. The handwritten step now
marks the connection shut, and the driver requires both the book
and `muro_http_step` to stay quiet.

Not in this slice: 414 and 431, 100 Continue, chunked bodies,
pipelining, a pulled body, an idle `MURO_STEP_NOW`, a short write,
`WRITE_FAILED` of a handler response, keep-alive consume, fuel,
and a handler 500. Chunked is outside the book even when the
method would otherwise be 501.

The admit wall, the request views, and `host_copy_response` stay
in C. There is no HTTP/2 backend and no Elixir or muro-beam
backend. A keep-alive span from the handwritten step still points
into the send buffer bound by `muro_http_step_bind`.
