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
clock. `errors` are connect, write, read, or parse failures.
`non2xx` is a completed response outside 200–299.

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
