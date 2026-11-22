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
