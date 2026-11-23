# Bench notes

Official numbers use `bench/loadgen`. `ab` is not the table. The
server under test is `c/muro-hello` (GET `/` returns `hello\n`), not
`examples/challenge`. Build is the repo default `OPT=-O2`.

## Tool

```
make bench/loadgen
./bench/loadgen --host 127.0.0.1 --port 18180 --concurrency N --duration SEC --mode keepalive|close
```

One pthread per concurrency slot. Keep-alive is one TCP connection
per thread. Close is one connection per request, aborted with
`SO_LINGER` 0 so the tuple does not sit in `TIME_WAIT`. The ephemeral
range on this Mac is 49152–65535 and `net.inet.tcp.msl` is 15000, so
a graceful close makes the following runs fail to connect. Latency
is the time from just before the request write until the response
body has been read. Connect is outside that window. p50 and p99 are
nearest-rank (`ceil(p/100 * N)`) on 1 µs buckets. `overflow` counts
samples at or above 200 ms. `TCP_NODELAY` is set. The request is
`GET / HTTP/1.1` with `Host` and `Connection: keep-alive` or
`Connection: close`. A run stops at `duration` seconds. Threads
finish an in-flight request, so `elapsed_ms` can be a little over
the duration. `rps` is completed responses divided by that wall
clock. `errors` is the sum of `err_connect`, `err_write`,
`err_read`, and `err_parse`. `non2xx` is a completed response
outside 200–299. A server handoff that closes an accepted socket
prints `handoff drop N: reason` and, when the process exits,
`handoff drops N`.

Warmup is the same command with `--duration 2`, discarded. Then
three measured runs with `--duration 5`.

## Server

```
./c/muro-hello --bind 127.0.0.1 --port 18180 --max-connections 1024 --idle-ms 60000
```

`max_conn` is 1024 so a 128-wide run is not capped by the default
128. Idle is 60 s so the bench, not the idle tick, closes sockets.
Listen backlog is the server's 128. One process. The baseline
binary is `poll(2)`, git `9a936f6a0c6c76d9a8447836fb27a59185478481`.
The raw tool output is `bench/baseline-9a936f6.txt`.

## Machine

Apple M4 Pro, 12 cores, 24 GB, macOS. Loopback `127.0.0.1`.

```
Darwin 192.168.0.129 24.6.0 Darwin Kernel Version 24.6.0: Mon Jan 19 22:01:58 PST 2026; root:xnu-11417.140.69.708.3~1/RELEASE_ARM64_T6041 arm64
```

Baseline start: Sun Nov 22 11:28:33 CST 2026. Load averages
1.0, 1.2, 1.4. About 40432 free pages (page size 16384, about
630 MB free). A Docker Virtualization VM was running
(`com.apple.Virtualization.VirtualMachine`, about 14% RSS).
`beam.smp` 85656 was running. `ulimit -n` was 1048575.

## Baseline — poll, 1 process

Every measured run. Median is the middle rps. p50 and p99 on that
row are from the same run.

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx |
| --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 32513.00 | 31 | 37 | 0 | 0 |
| keepalive | 1 | 2 | 32492.00 | 31 | 37 | 0 | 0 |
| keepalive | 1 | 3 | 32557.40 | 31 | 37 | 0 | 0 |
| keepalive | 1 | median | 32513.00 | 31 | 37 | 0 | 0 |
| keepalive | 8 | 1 | 58679.20 | 119 | 245 | 0 | 0 |
| keepalive | 8 | 2 | 59036.40 | 119 | 244 | 0 | 0 |
| keepalive | 8 | 3 | 58924.40 | 119 | 244 | 0 | 0 |
| keepalive | 8 | median | 58924.40 | 119 | 244 | 0 | 0 |
| keepalive | 32 | 1 | 59082.60 | 524 | 1061 | 0 | 0 |
| keepalive | 32 | 2 | 59477.20 | 519 | 1054 | 0 | 0 |
| keepalive | 32 | 3 | 59439.20 | 520 | 1053 | 0 | 0 |
| keepalive | 32 | median | 59439.20 | 520 | 1053 | 0 | 0 |
| keepalive | 128 | 1 | 57467.81 | 2219 | 2327 | 0 | 0 |
| keepalive | 128 | 2 | 57214.51 | 2228 | 2337 | 0 | 0 |
| keepalive | 128 | 3 | 57578.57 | 2213 | 2329 | 0 | 0 |
| keepalive | 128 | median | 57467.81 | 2219 | 2327 | 0 | 0 |
| close | 1 | 1 | 16071.40 | 32 | 39 | 0 | 0 |
| close | 1 | 2 | 16092.60 | 33 | 39 | 0 | 0 |
| close | 1 | 3 | 16106.80 | 32 | 39 | 0 | 0 |
| close | 1 | median | 16092.60 | 33 | 39 | 0 | 0 |
| close | 8 | 1 | 42579.20 | 108 | 189 | 0 | 0 |
| close | 8 | 2 | 42347.06 | 108 | 187 | 0 | 0 |
| close | 8 | 3 | 41957.60 | 108 | 191 | 0 | 0 |
| close | 8 | median | 42347.06 | 108 | 187 | 0 | 0 |
| close | 32 | 1 | 43255.40 | 388 | 704 | 0 | 0 |
| close | 32 | 2 | 44135.80 | 385 | 643 | 0 | 0 |
| close | 32 | 3 | 44120.60 | 383 | 678 | 0 | 0 |
| close | 32 | median | 44120.60 | 383 | 678 | 0 | 0 |
| close | 128 | 1 | 41729.71 | 1463 | 2550 | 0 | 0 |
| close | 128 | 2 | 40299.81 | 1505 | 2085 | 0 | 0 |
| close | 128 | 3 | 42350.66 | 1466 | 2943 | 0 | 0 |
| close | 128 | median | 41729.71 | 1463 | 2550 | 0 | 0 |

Keep-alive at 8, 32, and 128 sits near 57–59k req/s. One process
saturates below the core count. Close is lower and still flat from
8 to 128. Close c=128 p99 moves between runs (2085, 2550, 2943 µs).
The median row keeps that run's own p99.

## After — same tool, git 4b9be61

Same host, same port, same `bench/loadgen` flags, same
`--max-connections 1024` and `--idle-ms 60000`. The binary is
the `-O2` `c/muro-hello` built after the wait and worker
commits. Start: Sun Nov 22 11:52:18 CST 2026. End:
Sun Nov 22 12:01:25 CST 2026. `hw.model` Mac16,11, 12 cores,
25769803776 bytes. Load average at start 1.19, 1.11, 1.23.
Free pages at the start of each config (page size 16384):
poll 8901, kqueue/1 8381, kqueue/4 10172, kqueue/12 11376.
About 140–180 MB free. The Docker Virtualization VM was still
running. This is loopback. 12 workers did not exhaust memory.
The raw tool output is `bench/after-4b9be61.txt`.

```
MURO_HTTP_WAIT=poll ./c/muro-hello --bind 127.0.0.1 --port 18180 --max-connections 1024 --idle-ms 60000 --workers 1
env -u MURO_HTTP_WAIT ./c/muro-hello --bind 127.0.0.1 --port 18180 --max-connections 1024 --idle-ms 60000 --workers N
./bench/loadgen --host 127.0.0.1 --port 18180 --concurrency N --duration SEC --mode keepalive|close
```

`N` for workers is 1, then 4, then 12. Warmup `--duration 2`
is discarded. Three runs use `--duration 5`. stderr showed
`wait poll` once; `wait kqueue` once; `workers 4 handoff`
and four `wait kqueue` lines; `workers 12 handoff` and twelve
`wait kqueue` lines. Each config printed one
`listening 127.0.0.1 18180`. On this Mac the parent owns that
socket and passes accepted fds. A full handoff socket drops
the accepted connection after one 20 ms retry. Those drops
are the non-zero `errors` below. `non2xx` was 0 on every
measured run. `overflow` was 0 on every measured run.

### poll, 1 worker

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx |
| --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 32555.40 | 31 | 37 | 0 | 0 |
| keepalive | 1 | 2 | 32327.00 | 31 | 37 | 0 | 0 |
| keepalive | 1 | 3 | 32411.40 | 31 | 37 | 0 | 0 |
| keepalive | 1 | median | 32411.40 | 31 | 37 | 0 | 0 |
| keepalive | 8 | 1 | 57567.20 | 122 | 248 | 0 | 0 |
| keepalive | 8 | 2 | 58008.00 | 121 | 247 | 0 | 0 |
| keepalive | 8 | 3 | 58081.00 | 121 | 247 | 0 | 0 |
| keepalive | 8 | median | 58008.00 | 121 | 247 | 0 | 0 |
| keepalive | 32 | 1 | 58082.60 | 534 | 1072 | 0 | 0 |
| keepalive | 32 | 2 | 58068.20 | 534 | 1072 | 0 | 0 |
| keepalive | 32 | 3 | 58248.40 | 532 | 1066 | 0 | 0 |
| keepalive | 32 | median | 58082.60 | 534 | 1072 | 0 | 0 |
| keepalive | 128 | 1 | 56532.39 | 2265 | 2354 | 0 | 0 |
| keepalive | 128 | 2 | 56342.66 | 2277 | 2363 | 0 | 0 |
| keepalive | 128 | 3 | 56525.19 | 2263 | 2356 | 0 | 0 |
| keepalive | 128 | median | 56525.19 | 2263 | 2356 | 0 | 0 |
| close | 1 | 1 | 16117.20 | 33 | 39 | 0 | 0 |
| close | 1 | 2 | 16127.80 | 32 | 39 | 0 | 0 |
| close | 1 | 3 | 16192.80 | 32 | 39 | 0 | 0 |
| close | 1 | median | 16127.80 | 32 | 39 | 0 | 0 |
| close | 8 | 1 | 41575.20 | 103 | 190 | 0 | 0 |
| close | 8 | 2 | 43021.35 | 104 | 189 | 0 | 0 |
| close | 8 | 3 | 43644.40 | 105 | 192 | 0 | 0 |
| close | 8 | median | 43021.35 | 104 | 189 | 0 | 0 |
| close | 32 | 1 | 43201.20 | 385 | 681 | 0 | 0 |
| close | 32 | 2 | 45152.60 | 382 | 681 | 0 | 0 |
| close | 32 | 3 | 42310.80 | 391 | 667 | 0 | 0 |
| close | 32 | median | 43201.20 | 385 | 681 | 0 | 0 |
| close | 128 | 1 | 40020.99 | 1503 | 2116 | 0 | 0 |
| close | 128 | 2 | 41142.83 | 1505 | 2482 | 0 | 0 |
| close | 128 | 3 | 39480.21 | 1583 | 3086 | 0 | 0 |
| close | 128 | median | 40020.99 | 1503 | 2116 | 0 | 0 |

### kqueue, 1 worker

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx |
| --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 32840.00 | 31 | 36 | 0 | 0 |
| keepalive | 1 | 2 | 32847.00 | 31 | 36 | 0 | 0 |
| keepalive | 1 | 3 | 33040.40 | 30 | 36 | 0 | 0 |
| keepalive | 1 | median | 32847.00 | 31 | 36 | 0 | 0 |
| keepalive | 8 | 1 | 57936.40 | 121 | 246 | 0 | 0 |
| keepalive | 8 | 2 | 58407.40 | 120 | 246 | 0 | 0 |
| keepalive | 8 | 3 | 58131.60 | 121 | 246 | 0 | 0 |
| keepalive | 8 | median | 58131.60 | 121 | 246 | 0 | 0 |
| keepalive | 32 | 1 | 58455.20 | 532 | 1068 | 0 | 0 |
| keepalive | 32 | 2 | 59114.20 | 526 | 1060 | 0 | 0 |
| keepalive | 32 | 3 | 59066.60 | 525 | 1066 | 0 | 0 |
| keepalive | 32 | median | 59066.60 | 525 | 1066 | 0 | 0 |
| keepalive | 128 | 1 | 58370.25 | 2203 | 2294 | 0 | 0 |
| keepalive | 128 | 2 | 58409.84 | 2193 | 2279 | 0 | 0 |
| keepalive | 128 | 3 | 58728.91 | 2175 | 2285 | 0 | 0 |
| keepalive | 128 | median | 58409.84 | 2193 | 2279 | 0 | 0 |
| close | 1 | 1 | 15968.20 | 33 | 44 | 0 | 0 |
| close | 1 | 2 | 15711.20 | 33 | 44 | 0 | 0 |
| close | 1 | 3 | 15948.60 | 33 | 44 | 0 | 0 |
| close | 1 | median | 15948.60 | 33 | 44 | 0 | 0 |
| close | 8 | 1 | 41812.60 | 107 | 181 | 0 | 0 |
| close | 8 | 2 | 42551.20 | 105 | 177 | 0 | 0 |
| close | 8 | 3 | 43255.00 | 106 | 177 | 0 | 0 |
| close | 8 | median | 42551.20 | 105 | 177 | 0 | 0 |
| close | 32 | 1 | 42174.00 | 386 | 637 | 0 | 0 |
| close | 32 | 2 | 46630.80 | 364 | 646 | 0 | 0 |
| close | 32 | 3 | 45458.00 | 371 | 657 | 0 | 0 |
| close | 32 | median | 45458.00 | 371 | 657 | 0 | 0 |
| close | 128 | 1 | 40694.18 | 1484 | 2870 | 0 | 0 |
| close | 128 | 2 | 42117.35 | 1493 | 3100 | 0 | 0 |
| close | 128 | 3 | 40814.07 | 1521 | 3047 | 0 | 0 |
| close | 128 | median | 40814.07 | 1521 | 3047 | 0 | 0 |

### kqueue, 4 workers

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx |
| --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 32856.40 | 31 | 36 | 0 | 0 |
| keepalive | 1 | 2 | 33169.00 | 30 | 36 | 0 | 0 |
| keepalive | 1 | 3 | 32987.40 | 31 | 36 | 0 | 0 |
| keepalive | 1 | median | 32987.40 | 31 | 36 | 0 | 0 |
| keepalive | 8 | 1 | 163453.80 | 48 | 68 | 0 | 0 |
| keepalive | 8 | 2 | 163964.20 | 48 | 68 | 0 | 0 |
| keepalive | 8 | 3 | 163901.40 | 48 | 68 | 0 | 0 |
| keepalive | 8 | median | 163901.40 | 48 | 68 | 0 | 0 |
| keepalive | 32 | 1 | 186137.80 | 167 | 305 | 0 | 0 |
| keepalive | 32 | 2 | 186240.80 | 166 | 305 | 0 | 0 |
| keepalive | 32 | 3 | 186118.80 | 166 | 305 | 0 | 0 |
| keepalive | 32 | median | 186137.80 | 167 | 305 | 0 | 0 |
| keepalive | 128 | 1 | 186640.47 | 678 | 1214 | 0 | 0 |
| keepalive | 128 | 2 | 186230.55 | 678 | 1232 | 0 | 0 |
| keepalive | 128 | 3 | 185974.61 | 681 | 1223 | 0 | 0 |
| keepalive | 128 | median | 186230.55 | 678 | 1232 | 0 | 0 |
| close | 1 | 1 | 15222.80 | 36 | 42 | 0 | 0 |
| close | 1 | 2 | 15237.60 | 36 | 43 | 1 | 0 |
| close | 1 | 3 | 15194.80 | 36 | 42 | 0 | 0 |
| close | 1 | median | 15222.80 | 36 | 42 | 0 | 0 |
| close | 8 | 1 | 38542.80 | 79 | 121 | 1 | 0 |
| close | 8 | 2 | 38314.20 | 79 | 120 | 0 | 0 |
| close | 8 | 3 | 38145.20 | 79 | 120 | 0 | 0 |
| close | 8 | median | 38314.20 | 79 | 120 | 0 | 0 |
| close | 32 | 1 | 38980.00 | 337 | 485 | 0 | 0 |
| close | 32 | 2 | 38739.60 | 335 | 476 | 0 | 0 |
| close | 32 | 3 | 38565.40 | 337 | 481 | 0 | 0 |
| close | 32 | median | 38739.60 | 335 | 476 | 0 | 0 |
| close | 128 | 1 | 35431.63 | 1458 | 2268 | 0 | 0 |
| close | 128 | 2 | 35965.41 | 1434 | 2145 | 2 | 0 |
| close | 128 | 3 | 37273.95 | 1427 | 2048 | 16 | 0 |
| close | 128 | median | 35965.41 | 1434 | 2145 | 2 | 0 |

### kqueue, 12 workers

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx |
| --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 33956.20 | 29 | 35 | 0 | 0 |
| keepalive | 1 | 2 | 33959.00 | 29 | 35 | 0 | 0 |
| keepalive | 1 | 3 | 33927.60 | 29 | 35 | 0 | 0 |
| keepalive | 1 | median | 33956.20 | 29 | 35 | 0 | 0 |
| keepalive | 8 | 1 | 154517.40 | 51 | 73 | 0 | 0 |
| keepalive | 8 | 2 | 154386.80 | 51 | 73 | 0 | 0 |
| keepalive | 8 | 3 | 154706.60 | 51 | 73 | 0 | 0 |
| keepalive | 8 | median | 154517.40 | 51 | 73 | 0 | 0 |
| keepalive | 32 | 1 | 167088.80 | 189 | 230 | 0 | 0 |
| keepalive | 32 | 2 | 166987.40 | 190 | 230 | 0 | 0 |
| keepalive | 32 | 3 | 167180.20 | 189 | 229 | 0 | 0 |
| keepalive | 32 | median | 167088.80 | 189 | 230 | 0 | 0 |
| keepalive | 128 | 1 | 168703.66 | 757 | 816 | 0 | 0 |
| keepalive | 128 | 2 | 168643.27 | 757 | 817 | 0 | 0 |
| keepalive | 128 | 3 | 168692.86 | 757 | 817 | 0 | 0 |
| keepalive | 128 | median | 168692.86 | 757 | 817 | 0 | 0 |
| close | 1 | 1 | 15096.60 | 36 | 43 | 0 | 0 |
| close | 1 | 2 | 15048.80 | 36 | 42 | 0 | 0 |
| close | 1 | 3 | 15171.80 | 36 | 42 | 0 | 0 |
| close | 1 | median | 15096.60 | 36 | 43 | 0 | 0 |
| close | 8 | 1 | 38157.00 | 75 | 130 | 0 | 0 |
| close | 8 | 2 | 36850.79 | 75 | 131 | 0 | 0 |
| close | 8 | 3 | 37587.80 | 75 | 131 | 2 | 0 |
| close | 8 | median | 37587.80 | 75 | 131 | 2 | 0 |
| close | 32 | 1 | 36350.20 | 328 | 495 | 0 | 0 |
| close | 32 | 2 | 37038.15 | 324 | 480 | 2 | 0 |
| close | 32 | 3 | 38620.20 | 325 | 477 | 0 | 0 |
| close | 32 | median | 37038.15 | 324 | 480 | 2 | 0 |
| close | 128 | 1 | 36823.24 | 1301 | 1992 | 30 | 0 |
| close | 128 | 2 | 34169.88 | 1325 | 2105 | 38 | 0 |
| close | 128 | 3 | 34862.03 | 1332 | 2113 | 0 | 0 |
| close | 128 | median | 34862.03 | 1332 | 2113 | 0 | 0 |

Median is the middle req/s of the three runs. p50, p99, and
errors on that row are from the same run. Where another run
in the cell has errors, the per-run rows keep them.

kqueue with one worker stays near the poll numbers. Four
workers raise keep-alive from about 58k req/s to about 186k
at c=32 and c=128. Twelve workers are below four on those
points (about 167–169k). Close does not rise with workers.
The parent accept and the handoff are one thread, and close
opens a connection per request. A few of those handoffs
were dropped (errors 1, 2, 16, 30, or 38 on the runs that
failed the loadgen exit check).

## After the handoff wait — git b7d8bc2

Same host, same port, same `bench/loadgen` flags, same
`c/muro-hello` build (`OPT=-O2`). The binary is
`b7d8bc23661cec6b4fcada9d65120a24cd843582`. That parent does
not close an accepted socket because the workers are busy. It
blocks, scans the workers again, and keeps its copy of the fd
until the worker writes one ack byte. The listen call passes
`SOMAXCONN`. On this Mac `SOMAXCONN` and `kern.ipc.somaxconn`
are both 128, so the numeric backlog does not rise. The kernel
clamps it. The change that removes the drops is the wait, not
a deeper queue.

Start: Sun Nov 22 15:27:52 CST 2026. End: Sun Nov 22 15:36:59
CST 2026. `hw.model` Mac16,11, 12 cores, 25769803776 bytes.
`uname` is Darwin 24.6.0 arm64, the same kernel string as the
earlier section. Load average at start 0.96, 1.35, 1.80.
Free pages at the start of each config (page size 16384):
poll 4577, kqueue/1 11769, kqueue/4 10788, kqueue/12 12051.
About 75–197 MB free. The start of the log had 4057 free
pages, about 66 MB. Docker containers `muro-mac-1` and
`muro-mac-2` were up. This is loopback. 12 workers did not
exhaust memory. The raw tool output is
`bench/after-b7d8bc2.txt`.

```
./c/muro-hello --bind 127.0.0.1 --port 18180 --max-connections 1024 --idle-ms 60000 --workers 1 --wait poll
./c/muro-hello --bind 127.0.0.1 --port 18180 --max-connections 1024 --idle-ms 60000 --workers N --wait kqueue
./bench/loadgen --host 127.0.0.1 --port 18180 --concurrency N --duration SEC --mode keepalive|close
```

`N` for workers is 1, then 4, then 12. Warmup `--duration 2`
is in the raw log and is not in the tables. Three runs use
`--duration 5`. stderr showed `wait poll` for the poll
process; `wait kqueue` for the one kqueue process; `workers 4
handoff` and four `wait kqueue` lines; `workers 12 handoff`
and twelve `wait kqueue` lines. Each config printed one
`listening 127.0.0.1 18180`. One worker does not fork, so it
does not print `handoff drops`. The 4-worker and 12-worker
parents each printed `handoff drops 0` and no
`handoff drop N:` line.

`errors` is `err_connect + err_write + err_read + err_parse`.
Every warmup and every measured run had 0 for each of those,
and `non2xx` 0, and `overflow` 0. The median row is the
middle req/s. The other columns on that row are from the same
run. The three runs are listed, so a zero on the median row
cannot hide a spike in a sibling.

### poll, 1 worker

| mode | c | run | req/s | p50 µs | p99 µs | errors | connect | write | read | parse | non-2xx |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 32228.60 | 31 | 43 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 2 | 31816.20 | 32 | 43 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 3 | 32479.20 | 31 | 43 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | median | 32228.60 | 31 | 43 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 1 | 57443.00 | 122 | 260 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 57648.40 | 122 | 260 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 57729.40 | 121 | 258 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 57648.40 | 122 | 260 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 57857.40 | 534 | 1080 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 57518.20 | 538 | 1086 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 57864.20 | 536 | 1084 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 57857.40 | 534 | 1080 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 55787.88 | 2296 | 2514 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 56115.15 | 2284 | 2497 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 55778.49 | 2295 | 2506 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 55787.88 | 2296 | 2514 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 1 | 15453.00 | 32 | 49 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 2 | 15403.60 | 32 | 47 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 3 | 15573.60 | 32 | 48 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | median | 15453.00 | 32 | 49 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 1 | 38210.80 | 103 | 202 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 2 | 41756.00 | 101 | 195 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 3 | 40427.40 | 101 | 196 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | median | 40427.40 | 101 | 196 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 1 | 39605.20 | 390 | 740 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 2 | 41348.20 | 399 | 754 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 3 | 41944.20 | 403 | 769 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | median | 41348.20 | 399 | 754 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 1 | 39523.39 | 1624 | 3984 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 2 | 37798.68 | 1676 | 3859 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 3 | 38450.42 | 1670 | 3957 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | median | 38450.42 | 1670 | 3957 | 0 | 0 | 0 | 0 | 0 | 0 |

Close errors per run, poll / 1: c=1 0/0/0, c=8 0/0/0, c=32 0/0/0, c=128 0/0/0. Each run's connect/write/read/parse is 0/0/0/0.

### kqueue, 1 worker

| mode | c | run | req/s | p50 µs | p99 µs | errors | connect | write | read | parse | non-2xx |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 32363.00 | 30 | 42 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 2 | 32255.40 | 31 | 42 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 3 | 32345.40 | 31 | 42 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | median | 32345.40 | 31 | 42 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 1 | 59558.60 | 117 | 249 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 59630.00 | 117 | 247 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 59616.60 | 117 | 249 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 59616.60 | 117 | 249 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 60130.60 | 513 | 1039 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 60122.40 | 513 | 1037 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 60235.80 | 512 | 1034 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 60130.60 | 513 | 1039 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 59737.90 | 2134 | 2309 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 59693.32 | 2132 | 2307 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 59808.88 | 2132 | 2286 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 59737.90 | 2134 | 2309 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 1 | 14569.20 | 34 | 47 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 2 | 14609.20 | 34 | 48 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 3 | 14754.40 | 34 | 46 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | median | 14609.20 | 34 | 48 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 1 | 38447.20 | 111 | 235 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 2 | 39547.80 | 115 | 243 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 3 | 39725.20 | 113 | 245 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | median | 39547.80 | 115 | 243 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 1 | 40071.40 | 469 | 1055 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 2 | 40203.80 | 457 | 1021 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 3 | 40660.80 | 463 | 1050 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | median | 40203.80 | 457 | 1021 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 1 | 37168.81 | 1754 | 4434 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 2 | 37920.23 | 1737 | 3801 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 3 | 36994.20 | 1776 | 4786 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | median | 37168.81 | 1754 | 4434 | 0 | 0 | 0 | 0 | 0 | 0 |

Close errors per run, kqueue / 1: c=1 0/0/0, c=8 0/0/0, c=32 0/0/0, c=128 0/0/0. Each run's connect/write/read/parse is 0/0/0/0. This process does not hand off.

### kqueue, 4 workers

| mode | c | run | req/s | p50 µs | p99 µs | errors | connect | write | read | parse | non-2xx |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 33220.60 | 30 | 40 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 2 | 33193.00 | 30 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 3 | 33189.20 | 30 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | median | 33193.00 | 30 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 1 | 162616.40 | 48 | 73 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 162227.80 | 48 | 73 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 161931.00 | 48 | 75 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 162227.80 | 48 | 73 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 183690.80 | 165 | 314 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 184329.60 | 165 | 311 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 183632.00 | 166 | 313 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 183690.80 | 165 | 314 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 184524.70 | 674 | 1311 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 184704.06 | 676 | 1298 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 184158.17 | 674 | 1314 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 184524.70 | 674 | 1311 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 1 | 14416.20 | 36 | 52 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 2 | 14323.40 | 36 | 54 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 3 | 14414.00 | 36 | 52 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | median | 14414.00 | 36 | 52 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 1 | 35545.80 | 84 | 141 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 2 | 37268.20 | 83 | 139 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 3 | 35951.60 | 83 | 141 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | median | 35951.60 | 83 | 141 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 1 | 36603.60 | 355 | 529 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 2 | 36112.80 | 356 | 534 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 3 | 36533.80 | 354 | 533 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | median | 36533.80 | 354 | 533 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 1 | 33429.03 | 1549 | 2485 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 2 | 36709.26 | 1479 | 2116 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 3 | 36563.17 | 1467 | 2139 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | median | 36563.17 | 1467 | 2139 | 0 | 0 | 0 | 0 | 0 | 0 |

Close errors per run, kqueue / 4: c=1 0/0/0, c=8 0/0/0, c=32 0/0/0, c=128 0/0/0. Each run's connect/write/read/parse is 0/0/0/0. Server exit: `handoff drops 0`.

### kqueue, 12 workers

| mode | c | run | req/s | p50 µs | p99 µs | errors | connect | write | read | parse | non-2xx |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 33849.60 | 29 | 35 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 2 | 33891.40 | 29 | 35 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 3 | 34072.80 | 29 | 35 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | median | 33891.40 | 29 | 35 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 1 | 154911.20 | 51 | 73 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 154861.20 | 51 | 72 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 154938.40 | 51 | 73 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 154911.20 | 51 | 73 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 166244.00 | 190 | 231 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 165374.00 | 191 | 233 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 164793.60 | 192 | 233 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 165374.00 | 191 | 233 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 166089.98 | 768 | 840 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 162415.72 | 783 | 879 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 165492.50 | 770 | 859 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 165492.50 | 770 | 859 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 1 | 14191.60 | 40 | 47 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 2 | 14194.20 | 40 | 47 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 3 | 14190.20 | 40 | 47 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | median | 14191.60 | 40 | 47 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 1 | 35589.80 | 94 | 151 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 2 | 34451.40 | 94 | 158 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 3 | 36322.20 | 93 | 146 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | median | 35589.80 | 94 | 151 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 1 | 37997.20 | 364 | 512 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 2 | 35357.60 | 375 | 568 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 3 | 37278.60 | 369 | 546 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | median | 37278.60 | 369 | 546 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 1 | 34768.09 | 1547 | 2390 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 2 | 35716.91 | 1527 | 2164 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 3 | 34774.89 | 1558 | 2379 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | median | 34774.89 | 1558 | 2379 | 0 | 0 | 0 | 0 | 0 | 0 |

Close errors per run, kqueue / 12: c=1 0/0/0, c=8 0/0/0, c=32 0/0/0, c=128 0/0/0. Each run's connect/write/read/parse is 0/0/0/0. Server exit: `handoff drops 0`. The earlier 12-worker close c=128 runs were 30, 38, and 0. The median row of that table showed 0.

Keep-alive with four workers is about 162k req/s at c=8 and about
184k at c=32 and c=128. That is within about 2% of the 4b9be61
medians (about 164k and 186k). Twelve workers stay below four
(about 155k and 165k). One kqueue worker stays near poll, and
its keep-alive at c=32 and c=128 is a little above the earlier
one-worker run. Close still does not rise with workers. The
4-worker close c=32 median is 36534 req/s, under the earlier
38740. The 12-worker close c=128 median is 34775, next to the
earlier 34862, and those earlier runs had errors. One kqueue
worker, which does not hand off, is also a few percent under
its earlier close c=1 number (14609 vs 15949), so the small-c
gap is not only the ack byte. The ack is one extra write on
the worker before the request is adopted, and the parent closes
its copy when that byte arrives.

## After the protocol split — git 3c74b00

Same host, same port, same `bench/loadgen` flags, same
`c/muro-hello` build (`OPT=-O2`). The binary is
`3c74b00333262a63cc1758fbbbbf3c8613796e84`. HTTP/1.1 decisions
are `c/step.c`. The adapter writes the pointer the step
returns. It does not copy that span again.

Start: Sun Nov 22 20:20:43 CST 2026. End: Sun Nov 22 20:29:50
CST 2026. `hw.model` Mac16,11, 12 cores, 25769803776 bytes.
`uname` is the same Darwin 24.6.0 arm64 string as the earlier
sections. Load average at start 1.37, 1.38, 1.34. At the end
5.99, 4.77, 3.05. Free pages at the start of each config
(page size 16384): poll 4897 (about 80 MB), kqueue/1 10634
(about 174 MB), kqueue/4 11977 (about 196 MB), kqueue/12
12109 (about 198 MB). The start of the log had 5910 free
pages, about 97 MB. Docker containers `muro-mac-1` and
`muro-mac-2` were up. `beam.smp` 85656 was running. This is
loopback. The raw tool output is `bench/after-3c74b00.txt`.

```
./c/muro-hello --bind 127.0.0.1 --port 18180 --max-connections 1024 --idle-ms 60000 --workers 1 --wait poll
./c/muro-hello --bind 127.0.0.1 --port 18180 --max-connections 1024 --idle-ms 60000 --workers N --wait kqueue
./bench/loadgen --host 127.0.0.1 --port 18180 --concurrency N --duration SEC --mode keepalive|close
```

`N` for workers is 1, then 4, then 12. Warmup `--duration 2`
is in the raw log and is not in the tables. Three runs use
`--duration 5`. stderr showed `wait poll` for the poll
process; `wait kqueue` for the one kqueue process; `workers 4
handoff` and four `wait kqueue` lines; `workers 12 handoff`
and twelve `wait kqueue` lines. Each config printed one
`listening 127.0.0.1 18180`. One worker does not print
`handoff drops`. The 4-worker and 12-worker parents each
printed `handoff drops 0` and no `handoff drop N:` line.

`errors` is `err_connect + err_write + err_read + err_parse`.
Every warmup and every measured run had 0 for each of those,
and `non2xx` 0, and `overflow` 0. The median row is the
middle req/s. The other columns on that row are from the same
run.

Compared with the handoff section (`b7d8bc2`, the #16 numbers
on this Mac): keep-alive, kqueue, 4 workers is 162334 at c=8
(was 162228), 183156 at c=32 (was 183691), and 183893 at
c=128 (was 184525). That is under 1%. Twelve workers stay
below four: 152938, 165134, and 166710, next to 154911,
165374, and 165493. One kqueue worker at c=8, c=32, and
c=128 is about 5% under that section (56589 vs 59617, 56869
vs 60131, 56766 vs 59738). Poll is about 2 to 3.5% under
(56167 vs 57648 at c=8). The machine was busier at the start
of this matrix than at the start of the handoff matrix
(load 1.37 versus 0.96, and the load climbed while this
matrix ran). The 4-worker keep-alive path is the one a second
copy of the response would have dropped. It did not drop.
Close-mode errors are 0/0/0 on every config. The 4-worker
close c=128 runs were 32672, 36415, and 36181. The low run
is in the table. Its error count is still 0.

### poll, 1 worker

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx | connect | write | read | parse |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 31479.40 | 31 | 43 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 2 | 31109.00 | 32 | 42 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 3 | 31278.40 | 32 | 43 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | median | 31278.40 | 32 | 43 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 1 | 56166.60 | 124 | 263 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 56080.80 | 125 | 263 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 56634.80 | 123 | 262 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 56166.60 | 124 | 263 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 56357.60 | 549 | 1108 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 56193.20 | 551 | 1112 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 56460.40 | 550 | 1109 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 56357.60 | 549 | 1108 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 54396.04 | 2353 | 2515 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 54530.79 | 2344 | 2516 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 53978.21 | 2367 | 2568 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 54396.04 | 2353 | 2515 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 1 | 15261.20 | 33 | 57 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 2 | 15181.20 | 34 | 57 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 3 | 15320.00 | 33 | 53 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | median | 15261.20 | 33 | 57 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 1 | 39120.40 | 109 | 213 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 2 | 39095.60 | 108 | 209 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 3 | 40682.00 | 112 | 203 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | median | 39120.40 | 109 | 213 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 1 | 40386.80 | 420 | 794 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 2 | 41350.80 | 411 | 793 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 3 | 41876.80 | 419 | 802 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | median | 41350.80 | 411 | 793 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 1 | 37179.33 | 1684 | 3856 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 2 | 39116.35 | 1670 | 3851 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 3 | 38391.64 | 1680 | 4130 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | median | 38391.64 | 1680 | 4130 | 0 | 0 | 0 | 0 | 0 | 0 |

### kqueue, 1 worker

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx | connect | write | read | parse |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 32384.00 | 30 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 2 | 32083.00 | 31 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 3 | 32170.00 | 31 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | median | 32170.00 | 31 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 1 | 56446.20 | 124 | 260 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 56589.00 | 123 | 260 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 56769.80 | 123 | 260 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 56589.00 | 123 | 260 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 56847.20 | 544 | 1102 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 56868.60 | 545 | 1100 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 56955.80 | 542 | 1098 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 56868.60 | 545 | 1100 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 56766.29 | 2251 | 2438 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 56894.24 | 2253 | 2411 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 56668.93 | 2254 | 2439 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 56766.29 | 2251 | 2438 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 1 | 14641.80 | 35 | 46 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 2 | 14570.37 | 35 | 48 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 3 | 14722.20 | 35 | 46 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | median | 14641.80 | 35 | 46 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 1 | 38313.60 | 116 | 238 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 2 | 38513.20 | 116 | 243 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 3 | 40401.80 | 118 | 238 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | median | 38513.20 | 116 | 243 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 1 | 41035.80 | 485 | 1054 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 2 | 39653.34 | 487 | 1089 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 3 | 41408.40 | 505 | 1081 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | median | 41035.80 | 485 | 1054 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 1 | 38739.10 | 1814 | 5353 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 2 | 36979.36 | 1758 | 4309 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 3 | 38141.34 | 1833 | 4848 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | median | 38141.34 | 1833 | 4848 | 0 | 0 | 0 | 0 | 0 | 0 |

### kqueue, 4 workers

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx | connect | write | read | parse |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 32441.60 | 30 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 2 | 31938.00 | 31 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 3 | 32483.40 | 30 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | median | 32441.60 | 30 | 41 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 1 | 162334.00 | 48 | 72 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 162080.00 | 48 | 72 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 162433.80 | 48 | 72 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 162334.00 | 48 | 72 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 183141.20 | 168 | 314 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 183590.60 | 167 | 313 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 183155.80 | 167 | 314 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 183155.80 | 167 | 314 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 184178.96 | 677 | 1308 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 183666.87 | 677 | 1314 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 183893.02 | 676 | 1315 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 183893.02 | 676 | 1315 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 1 | 14318.00 | 38 | 52 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 2 | 14321.40 | 38 | 51 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 3 | 14304.40 | 38 | 51 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | median | 14318.00 | 38 | 52 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 1 | 37076.00 | 84 | 140 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 2 | 37345.80 | 84 | 138 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 3 | 37638.00 | 84 | 136 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | median | 37345.80 | 84 | 138 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 1 | 37182.00 | 354 | 532 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 2 | 35749.40 | 360 | 552 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 3 | 36218.20 | 354 | 531 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | median | 36218.20 | 354 | 531 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 1 | 32672.07 | 1545 | 2465 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 2 | 36414.92 | 1514 | 2169 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 3 | 36180.73 | 1491 | 2170 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | median | 36180.73 | 1491 | 2170 | 0 | 0 | 0 | 0 | 0 | 0 |

### kqueue, 12 workers

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx | connect | write | read | parse |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 1 | 1 | 32868.80 | 30 | 39 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 2 | 32842.20 | 30 | 40 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | 3 | 32905.60 | 30 | 39 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 1 | median | 32868.80 | 30 | 39 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 1 | 152937.80 | 51 | 78 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 153249.80 | 51 | 77 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 152565.00 | 51 | 78 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 152937.80 | 51 | 78 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 165133.60 | 191 | 242 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 165228.00 | 191 | 241 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 165018.20 | 191 | 243 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 165133.60 | 191 | 242 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 166801.04 | 764 | 850 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 166709.86 | 763 | 857 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 166250.35 | 765 | 858 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 166709.86 | 763 | 857 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 1 | 13917.40 | 40 | 53 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 2 | 13768.60 | 40 | 55 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 3 | 13887.60 | 40 | 53 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | median | 13887.60 | 40 | 53 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 1 | 34631.40 | 95 | 157 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 2 | 34632.57 | 95 | 157 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 3 | 34991.40 | 95 | 159 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | median | 34632.57 | 95 | 157 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 1 | 35866.20 | 375 | 567 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 2 | 36127.60 | 378 | 561 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 3 | 34183.40 | 388 | 599 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | median | 35866.20 | 375 | 567 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 1 | 35116.95 | 1553 | 2240 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 2 | 33669.53 | 1604 | 2432 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 3 | 35479.01 | 1562 | 2242 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | median | 35116.95 | 1553 | 2240 | 0 | 0 | 0 | 0 | 0 | 0 |

## After the review fixes — git 09d3493

Same host, same port, same `bench/loadgen` flags, same
`c/muro-hello` build (`OPT=-O2`). The binary is
`09d3493c810a42aea330fb208423bc60c842a50b`. The step still
returns a pointer into the connection send buffer. This pass
does not add a second copy.

Start: Sun Nov 22 23:44:47 CST 2026. End: Sun Nov 22 23:48:47
CST 2026. `hw.model` Mac16,11, 12 cores, 25769803776 bytes.
`uname` is the same Darwin 24.6.0 arm64 string as the earlier
sections. Load average at start 1.48, 3.48, 3.25. At the end
8.27, 5.31, 4.03. The 5-minute load was still coming down
from an earlier busy period. The 1-minute load at the start
was 1.48. Free pages at the start of each config (page size
16384): poll 3638 (about 60 MB), kqueue/1 3643 (about 60 MB),
kqueue/4 4128 (about 68 MB), kqueue/12 7044 (about 115 MB).
The start of the log had 4047 free pages, about 66 MB.
Docker containers `muro-mac-1` and `muro-mac-2` were up and
idle for this pass. `beam.smp` 85656 was running. This is
loopback. The raw tool output is `bench/after-09d3493.txt`.

An earlier pass the same evening started while those two
containers were each near 100% CPU (load 3.53, 2.31, 1.83).
Its 4-worker keep-alive medians were 103868, 156762, and
157344 at c=8, c=32, and c=128, and the three c=8 runs were
93866, 103868, and 141907. Errors on that pass were still 0.
That pass is not this table. The runners were at 0% CPU when
this one started.

This pass measured the cells the review asked for. It did
not repeat poll at c=1, c=32, and c=128, and it did not
repeat close mode for 1 worker or 12 workers. Those rows
stay in the 3c74b00 section.

```
./c/muro-hello --bind 127.0.0.1 --port 18180 --max-connections 1024 --idle-ms 60000 --workers 1 --wait poll
./c/muro-hello --bind 127.0.0.1 --port 18180 --max-connections 1024 --idle-ms 60000 --workers N --wait kqueue
./bench/loadgen --host 127.0.0.1 --port 18180 --concurrency N --duration SEC --mode keepalive|close
```

`N` for workers is 1, then 4, then 12. Warmup `--duration 2`
is in the raw log and is not in the tables. Three runs use
`--duration 5`. stderr showed `wait poll` for the poll
process and `wait kqueue` for the kqueue processes. The
4-worker parent printed `workers 4 handoff` and
`handoff drops 0`. The 12-worker parent printed
`workers 12 handoff` and `handoff drops 0`. No
`handoff drop N:` line. One worker does not print
`handoff drops`.

`errors` is `err_connect + err_write + err_read + err_parse`.
Every warmup and every measured run had 0 for each of those,
and `non2xx` 0, and `overflow` 0. The median row is the
middle req/s. The other columns on that row are from the same
run.

Compared with the protocol-split section (`3c74b00`, the
numbers on `eef1293`): keep-alive, kqueue, 4 workers is
163016 at c=8 (was 162334), 186070 at c=32 (was 183156),
and 186194 at c=128 (was 183893). That is +0.4%, +1.6%,
and +1.3%. p50 at c=8 is still 48 µs. Twelve workers stay
within 1% of that section (154255, 164198, 165943 versus
152938, 165134, 166710). One kqueue worker is about 5%
above it (59355, 59904, 59602 versus 56589, 56869, 56766).
Poll at c=8 is 58868 versus 56167, about 4.8% above.
Close-mode errors on the 4-worker config are 0/0/0 at
c=1, c=8, c=32, and c=128. Each run's connect/write/read/parse
is 0/0/0/0. The 4-worker close c=128 runs were 34918,
33467, and 35489. The low run is in the table. Its error
count is 0. That median is about 3.5% under 36181.

### poll, 1 worker

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx | connect | write | read | parse |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 8 | 1 | 58781.20 | 119 | 244 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 58873.80 | 119 | 245 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 58867.60 | 119 | 243 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 58867.60 | 119 | 243 | 0 | 0 | 0 | 0 | 0 | 0 |

### kqueue, 1 worker

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx | connect | write | read | parse |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 8 | 1 | 59355.20 | 118 | 241 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 59384.00 | 118 | 242 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 59129.40 | 118 | 242 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 59355.20 | 118 | 241 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 59835.00 | 518 | 1040 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 59904.00 | 518 | 1039 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 59929.80 | 518 | 1039 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 59904.00 | 518 | 1039 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 59602.36 | 2146 | 2245 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 59522.99 | 2148 | 2268 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 59602.76 | 2146 | 2255 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 59602.36 | 2146 | 2245 | 0 | 0 | 0 | 0 | 0 | 0 |

### kqueue, 4 workers

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx | connect | write | read | parse |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 8 | 1 | 162736.80 | 48 | 69 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 163231.80 | 48 | 69 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 163016.20 | 48 | 69 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 163016.20 | 48 | 69 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 185960.00 | 168 | 305 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 186130.20 | 167 | 306 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 186070.40 | 168 | 304 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 186070.40 | 168 | 304 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 186211.56 | 677 | 1227 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 186194.36 | 678 | 1230 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 185125.37 | 676 | 1272 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 186194.36 | 678 | 1230 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 1 | 14566.60 | 38 | 44 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 2 | 14570.60 | 38 | 44 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | 3 | 14534.60 | 38 | 45 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 1 | median | 14566.60 | 38 | 44 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 1 | 37830.20 | 83 | 134 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 2 | 38831.20 | 83 | 132 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | 3 | 38351.40 | 83 | 134 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 8 | median | 38351.40 | 83 | 134 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 1 | 37912.60 | 347 | 505 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 2 | 39175.40 | 342 | 478 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | 3 | 39656.20 | 342 | 480 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 32 | median | 39175.40 | 342 | 478 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 1 | 34917.62 | 1494 | 2317 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 2 | 33467.11 | 1515 | 2395 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | 3 | 35488.80 | 1505 | 2343 | 0 | 0 | 0 | 0 | 0 | 0 |
| close | 128 | median | 34917.62 | 1494 | 2317 | 0 | 0 | 0 | 0 | 0 | 0 |

Close errors per run, kqueue / 4: c=1 0/0/0, c=8 0/0/0, c=32 0/0/0, c=128 0/0/0. Each run's connect/write/read/parse is 0/0/0/0. Server exit: `handoff drops 0`.

### kqueue, 12 workers

| mode | c | run | req/s | p50 µs | p99 µs | errors | non-2xx | connect | write | read | parse |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| keepalive | 8 | 1 | 154187.80 | 51 | 74 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 2 | 154254.80 | 51 | 74 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | 3 | 154261.40 | 51 | 73 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 8 | median | 154254.80 | 51 | 74 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 1 | 164557.20 | 192 | 235 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 2 | 164198.40 | 193 | 235 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | 3 | 164181.20 | 193 | 236 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 32 | median | 164198.40 | 193 | 235 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 1 | 164366.73 | 777 | 848 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 2 | 165943.21 | 769 | 835 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | 3 | 165953.41 | 769 | 840 | 0 | 0 | 0 | 0 | 0 | 0 |
| keepalive | 128 | median | 165943.21 | 769 | 835 | 0 | 0 | 0 | 0 | 0 | 0 |


