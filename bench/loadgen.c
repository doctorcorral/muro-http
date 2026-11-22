#define _DEFAULT_SOURCE

/* HTTP/1.1 load generator for this repo. No third-party libraries.
   One pthread per concurrency slot. Keep-alive reuses one TCP
   connection per thread. Close opens a new connection per request
   and aborts it with SO_LINGER 0 so the tuple does not sit in
   TIME_WAIT. Latency is the time from just before the request write
   until the response headers and body have been read. Connect is
   outside that window. A histogram of 1 us buckets up to 200 ms
   records it. The atomic add is after that window. p50 and p99 are
   nearest-rank: rank = ceil(p/100 * N), reported as the 1 us bucket
   index. A sample at or above 200 ms lands in the last bucket and
   is counted in overflow. TCP_NODELAY is set. The request is
   GET / HTTP/1.1. errors is the sum of err_connect, err_write,
   err_read, and err_parse. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

enum { HIST = 200000 };

static uint32_t *hist;
static uint64_t requests;
static uint64_t errors;
static uint64_t err_connect;
static uint64_t err_write;
static uint64_t err_read;
static uint64_t err_parse;
static uint64_t non2xx;
static uint64_t overflow;
static int opt_port = 18180;
static int opt_conc = 1;
static int opt_duration = 5;
static int opt_requests = 0;
static int opt_close = 0;
static char opt_host[64] = "127.0.0.1";
static uint64_t deadline_ms;
static uint64_t req_left;

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static uint64_t now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void add_u64(uint64_t *p, uint64_t n) {
  __atomic_fetch_add(p, n, __ATOMIC_RELAXED);
}

static void add_err(uint64_t *part) {
  add_u64(part, 1);
  add_u64(&errors, 1);
}

static void sleep_ms(unsigned ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
  while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {
  }
}

/* Close mode aborts with SO_LINGER 0. A graceful FIN parks the tuple
   in TIME_WAIT and the next runs on this Mac stop connecting. The
   measured work is still one TCP connection per request. */
static int dial(void) {
  int fd;
  int one = 1;
  int flags;
  int rc;
  struct sockaddr_in addr;
  struct timeval tv;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  if (opt_close) {
    struct linger lg;
    lg.l_onoff = 1;
    lg.l_linger = 0;
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
  }
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    close(fd);
    return -1;
  }
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)opt_port);
  if (inet_pton(AF_INET, opt_host, &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  rc = connect(fd, (struct sockaddr *)&addr, sizeof addr);
  if (rc < 0 && errno == EINPROGRESS) {
    struct pollfd pfd;
    int err = 0;
    socklen_t elen = sizeof err;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    if (poll(&pfd, 1, 1000) <= 0) {
      close(fd);
      return -1;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
      close(fd);
      return -1;
    }
  } else if (rc < 0) {
    close(fd);
    return -1;
  }
  if (fcntl(fd, F_SETFL, flags) < 0) {
    close(fd);
    return -1;
  }
  tv.tv_sec = 2;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  return fd;
}

/* Read until n bytes or a closed peer. 0 ok, -1 error. */
static int read_full(int fd, char *dst, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t r = recv(fd, dst + off, n - off, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    off += (size_t)r;
  }
  return 0;
}

static int discard_n(int fd, size_t n) {
  char tmp[512];
  while (n > 0) {
    size_t chunk = n > sizeof tmp ? sizeof tmp : n;
    if (read_full(fd, tmp, chunk) < 0) return -1;
    n -= chunk;
  }
  return 0;
}

static int write_full(int fd, const char *p, size_t n) {
  size_t off = 0;
  while (off < n) {
    ssize_t w = send(fd, p + off, n - off, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

/* One response. Status and content-length are required.
   0 ok, -1 a short read, -2 a parse error.
   *status is set when the status line parsed. */
static int read_response(int fd, int *status) {
  char buf[4096];
  size_t n = 0;
  char *end;
  char *line;
  long clen = -1;
  int code = 0;
  *status = 0;
  for (;;) {
    ssize_t r;
    if (n + 1 >= sizeof buf) return -2;
    r = recv(fd, buf + n, sizeof buf - 1 - n, 0);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    n += (size_t)r;
    buf[n] = 0;
    end = strstr(buf, "\r\n\r\n");
    if (end) break;
  }
  if (sscanf(buf, "HTTP/1.%*d %d", &code) != 1) return -2;
  *status = code;
  line = buf;
  while (line < end) {
    char *nl = strstr(line, "\r\n");
    size_t ln;
    if (!nl || nl > end) break;
    ln = (size_t)(nl - line);
    if (ln >= 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
      clen = strtol(line + 15, NULL, 10);
    }
    line = nl + 2;
  }
  if (clen < 0 || clen > 1048576L) return -2;
  {
    size_t header_n = (size_t)(end + 4 - buf);
    size_t have = n - header_n;
    if ((long)have > clen) return -2;
    if (discard_n(fd, (size_t)clen - have) < 0) return -1;
  }
  return 0;
}

static int take_request(void) {
  uint64_t cur;
  if (opt_requests <= 0) return 1;
  cur = __atomic_load_n(&req_left, __ATOMIC_RELAXED);
  for (;;) {
    if (cur == 0) return 0;
    if (__atomic_compare_exchange_n(&req_left, &cur, cur - 1, 0, __ATOMIC_RELAXED,
                                    __ATOMIC_RELAXED))
      return 1;
  }
}

static void *worker(void *arg) {
  char req[256];
  int req_n;
  int fd = -1;
  (void)arg;
  req_n = snprintf(req, sizeof req,
                   "GET / HTTP/1.1\r\n"
                   "Host: %s:%d\r\n"
                   "Connection: %s\r\n"
                   "\r\n",
                   opt_host, opt_port, opt_close ? "close" : "keep-alive");
  if (req_n < 0 || req_n >= (int)sizeof req) {
    add_err(&err_parse);
    return NULL;
  }
  while (now_ms() < deadline_ms && take_request()) {
    int status = 0;
    int rr;
    uint64_t t0;
    uint64_t dt;
    if (fd < 0) {
      fd = dial();
      if (fd < 0) {
        add_err(&err_connect);
        sleep_ms(1);
        continue;
      }
    }
    t0 = now_us();
    if (write_full(fd, req, (size_t)req_n) < 0) {
      add_err(&err_write);
      close(fd);
      fd = -1;
      continue;
    }
    rr = read_response(fd, &status);
    if (rr < 0) {
      add_err(rr == -2 ? &err_parse : &err_read);
      close(fd);
      fd = -1;
      continue;
    }
    dt = now_us() - t0;
    if (dt >= (uint64_t)HIST) {
      add_u64(&overflow, 1);
      __atomic_fetch_add(&hist[HIST - 1], 1, __ATOMIC_RELAXED);
    } else {
      __atomic_fetch_add(&hist[dt], 1, __ATOMIC_RELAXED);
    }
    add_u64(&requests, 1);
    if (status < 200 || status > 299) add_u64(&non2xx, 1);
    if (opt_close) {
      close(fd);
      fd = -1;
    }
  }
  if (fd >= 0) close(fd);
  return NULL;
}

static uint64_t percentile(uint64_t total, int pct) {
  uint64_t need;
  uint64_t acc = 0;
  int i;
  if (total == 0) return 0;
  need = (total * (uint64_t)pct + 99ull) / 100ull;
  if (need < 1) need = 1;
  for (i = 0; i < HIST; i++) {
    acc += hist[i];
    if (acc >= need) return (uint64_t)i;
  }
  return (uint64_t)HIST - 1;
}

static int parse_i(const char *s, int lo, int hi, int *out) {
  char *end = NULL;
  long v;
  if (!s || !*s) return -1;
  errno = 0;
  v = strtol(s, &end, 10);
  if (errno || !end || *end || v < lo || v > hi) return -1;
  *out = (int)v;
  return 0;
}

static void usage(void) {
  fprintf(stderr,
          "loadgen --host H --port N --concurrency N --duration S\n"
          "        [--requests N] --mode keepalive|close\n"
          "duration is seconds. requests 0 means duration only.\n"
          "When both are set, the run stops at the first limit.\n");
}

int main(int argc, char **argv) {
  pthread_t *threads = NULL;
  int i;
  int mode_set = 0;
  uint64_t t0;
  uint64_t elapsed;
  uint64_t reqs;
  uint64_t errs;
  uint64_t nconn;
  uint64_t nwrite;
  uint64_t nread;
  uint64_t nparse;
  uint64_t bad;
  uint64_t over;
  for (i = 1; i < argc; i++) {
    const char *a = argv[i];
    const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;
    if (strcmp(a, "--host") == 0 && val) {
      if (strlen(val) >= sizeof opt_host) return 2;
      memcpy(opt_host, val, strlen(val) + 1);
      i++;
    } else if (strcmp(a, "--port") == 0 && val) {
      if (parse_i(val, 1, 65535, &opt_port) < 0) return 2;
      i++;
    } else if (strcmp(a, "--concurrency") == 0 && val) {
      if (parse_i(val, 1, 1024, &opt_conc) < 0) return 2;
      i++;
    } else if (strcmp(a, "--duration") == 0 && val) {
      if (parse_i(val, 1, 3600, &opt_duration) < 0) return 2;
      i++;
    } else if (strcmp(a, "--requests") == 0 && val) {
      if (parse_i(val, 1, 2000000000, &opt_requests) < 0) return 2;
      i++;
    } else if (strcmp(a, "--mode") == 0 && val) {
      if (strcmp(val, "keepalive") == 0) opt_close = 0;
      else if (strcmp(val, "close") == 0) opt_close = 1;
      else return 2;
      mode_set = 1;
      i++;
    } else {
      usage();
      return 2;
    }
  }
  if (!mode_set || opt_duration < 1) {
    usage();
    return 2;
  }
  hist = calloc((size_t)HIST, sizeof *hist);
  threads = calloc((size_t)opt_conc, sizeof *threads);
  if (!hist || !threads) {
    fprintf(stderr, "loadgen: out of memory\n");
    return 1;
  }
  req_left = (uint64_t)opt_requests;
  deadline_ms = now_ms() + (uint64_t)opt_duration * 1000ull;
  t0 = now_ms();
  for (i = 0; i < opt_conc; i++) {
    if (pthread_create(&threads[i], NULL, worker, NULL) != 0) {
      fprintf(stderr, "loadgen: pthread_create\n");
      return 1;
    }
  }
  for (i = 0; i < opt_conc; i++) pthread_join(threads[i], NULL);
  elapsed = now_ms() - t0;
  if (elapsed == 0) elapsed = 1;
  reqs = __atomic_load_n(&requests, __ATOMIC_RELAXED);
  errs = __atomic_load_n(&errors, __ATOMIC_RELAXED);
  nconn = __atomic_load_n(&err_connect, __ATOMIC_RELAXED);
  nwrite = __atomic_load_n(&err_write, __ATOMIC_RELAXED);
  nread = __atomic_load_n(&err_read, __ATOMIC_RELAXED);
  nparse = __atomic_load_n(&err_parse, __ATOMIC_RELAXED);
  bad = __atomic_load_n(&non2xx, __ATOMIC_RELAXED);
  over = __atomic_load_n(&overflow, __ATOMIC_RELAXED);
  printf("requests %llu\n", (unsigned long long)reqs);
  printf("errors %llu\n", (unsigned long long)errs);
  printf("err_connect %llu\n", (unsigned long long)nconn);
  printf("err_write %llu\n", (unsigned long long)nwrite);
  printf("err_read %llu\n", (unsigned long long)nread);
  printf("err_parse %llu\n", (unsigned long long)nparse);
  printf("non2xx %llu\n", (unsigned long long)bad);
  printf("overflow %llu\n", (unsigned long long)over);
  printf("elapsed_ms %llu\n", (unsigned long long)elapsed);
  printf("rps %.2f\n", (double)reqs * 1000.0 / (double)elapsed);
  printf("p50_us %llu\n", (unsigned long long)percentile(reqs, 50));
  printf("p99_us %llu\n", (unsigned long long)percentile(reqs, 99));
  free(threads);
  free(hist);
  return errs ? 1 : 0;
}
