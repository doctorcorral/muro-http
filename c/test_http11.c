/* HTTP/1.1 completeness against c/muro-stream. Sockets, same shape as
   test_hello. Covers methods, Date, HEAD, chunked bodies, chunked and
   close-delimited responses, bodies past 64 KiB, 413, 100-continue,
   and a SIGTERM drain with one request still in flight. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static const char *bin_path = "c/muro-stream";
static const char *host_s = "127.0.0.1";

enum { BLOB = 1024 * 1024 + 1 };

static void fail(const char *name, const char *what) {
  fprintf(stderr, "FAIL %s: %s\n", name, what);
  failures += 1;
}

static int parse_port(const char *line) {
  char prefix[80];
  int port = -1;
  int n = snprintf(prefix, sizeof prefix, "listening %s ", host_s);
  if (n < 0 || (size_t)n >= sizeof prefix) return -1;
  if (strncmp(line, prefix, (size_t)n) != 0) return -1;
  if (sscanf(line + n, "%d", &port) != 1) return -1;
  if (port <= 0 || port > 65535) return -1;
  return port;
}

static pid_t start_stream(int *port, const char *max_body, const char *drain_ms) {
  int sp[2];
  pid_t pid;
  char buf[128];
  size_t n = 0;
  if (pipe(sp) < 0) return -1;
  pid = fork();
  if (pid < 0) {
    close(sp[0]);
    close(sp[1]);
    return -1;
  }
  if (pid == 0) {
    char *av[16];
    int argc = 0;
    alarm(0);
    if (dup2(sp[1], STDOUT_FILENO) < 0) _exit(127);
    close(sp[0]);
    close(sp[1]);
    unsetenv("MURO_HTTP_BIND");
    unsetenv("MURO_HTTP_PORT");
    unsetenv("MURO_HTTP_IDLE_MS");
    unsetenv("MURO_HTTP_MAX_CONNECTIONS");
    unsetenv("MURO_HTTP_MAX_BODY");
    unsetenv("MURO_HTTP_DRAIN_MS");
    av[argc++] = "muro-stream";
    av[argc++] = "--port";
    av[argc++] = "0";
    av[argc++] = "--bind";
    av[argc++] = (char *)host_s;
    av[argc++] = "--idle-ms";
    av[argc++] = "8000";
    if (max_body) {
      av[argc++] = "--max-body";
      av[argc++] = (char *)max_body;
    }
    if (drain_ms) {
      av[argc++] = "--drain-ms";
      av[argc++] = (char *)drain_ms;
    }
    av[argc] = NULL;
    execv(bin_path, av);
    perror("exec");
    _exit(127);
  }
  close(sp[1]);
  while (n + 1 < sizeof buf) {
    struct pollfd pfd;
    ssize_t r;
    pfd.fd = sp[0];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 3000) <= 0) break;
    r = read(sp[0], buf + n, 1);
    if (r <= 0) break;
    if (buf[n] == '\n') {
      buf[n] = 0;
      break;
    }
    n += (size_t)r;
  }
  close(sp[0]);
  buf[n < sizeof buf ? n : sizeof buf - 1] = 0;
  *port = parse_port(buf);
  if (*port < 0) {
    fprintf(stderr, "FAIL start: banner '%s'\n", buf);
    failures += 1;
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
  }
  return pid;
}

static void stop_server(pid_t pid) {
  int status = 0;
  int i;
  if (pid <= 0) return;
  kill(pid, SIGTERM);
  for (i = 0; i < 80; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) fail("shutdown", "status");
      return;
    }
    if (got < 0) return;
    usleep(50000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, NULL, 0);
  fail("shutdown", "did not stop");
}

static int dial_tv(int port, int sec) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr;
  struct timeval tv;
  if (fd < 0) return -1;
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, host_s, &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }
  if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
    close(fd);
    return -1;
  }
  tv.tv_sec = sec;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  return fd;
}

static int dial(int port) { return dial_tv(port, 8); }

static int send_all(int fd, const void *p, size_t n) {
  const char *s = p;
  size_t off = 0;
  while (off < n) {
    ssize_t w = send(fd, s + off, n - off, 0);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) return -1;
    off += (size_t)w;
  }
  return 0;
}

static int send_str(int fd, const char *s) { return send_all(fd, s, strlen(s)); }

typedef struct {
  int fd;
  char buf[8192];
  size_t n;
  size_t off;
} Conn;

typedef struct {
  int status;
  int close;
  int has_cl;
  long cl;
  int chunked;
  int te;
  int date_ok;
  int dates;
} Head;

static void compact(Conn *c) {
  if (c->off == 0) return;
  memmove(c->buf, c->buf + c->off, c->n - c->off);
  c->n -= c->off;
  c->off = 0;
}

static int read_more(Conn *c) {
  ssize_t r;
  if (c->n >= sizeof c->buf) return -1;
  r = read(c->fd, c->buf + c->n, sizeof c->buf - c->n);
  if (r < 0 && errno == EINTR) return read_more(c);
  if (r <= 0) return -1;
  c->n += (size_t)r;
  return 0;
}

static int week_ok(const char *w) {
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  int i;
  for (i = 0; i < 7; i++)
    if (strcmp(w, days[i]) == 0) return 1;
  return 0;
}

static int mon_index(const char *m) {
  static const char *mons[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                               "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int i;
  for (i = 0; i < 12; i++)
    if (strcmp(m, mons[i]) == 0) return i + 1;
  return 0;
}

static int wday_index(const char *w) {
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  int i;
  for (i = 0; i < 7; i++)
    if (strcmp(w, days[i]) == 0) return i;
  return -1;
}

/* Sakamoto. 0 is Sunday. 1994-11-06 is Sunday. */
static int civil_wday(int year, int month, int day) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  int w;
  if (month < 3) y -= 1;
  w = (y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7;
  if (w < 0) w += 7;
  return w;
}

/* IMF-fixdate is 29 characters: "Wdy, DD Mon YYYY HH:MM:SS GMT". */
static int imf_ok(const char *s) {
  char wday[4];
  char mon[4];
  char gmt[4];
  int day = 0;
  int year = 0;
  int hh = 0;
  int mm = 0;
  int ss = 0;
  if (s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' || s[19] != ':' ||
      s[22] != ':' || s[25] != ' ')
    return 0;
  if (sscanf(s, "%3s, %2d %3s %4d %2d:%2d:%2d %3s", wday, &day, mon, &year, &hh, &mm, &ss, gmt) !=
      8)
    return 0;
  if (!week_ok(wday) || mon_index(mon) == 0 || strcmp(gmt, "GMT") != 0) return 0;
  if (day < 1 || day > 31 || year < 1970 || hh > 23 || mm > 59 || ss > 60) return 0;
  if (wday_index(wday) != civil_wday(year, mon_index(mon), day)) return 0;
  return 1;
}

static int parse_head(const char *buf, size_t eoh, Head *h) {
  size_t i = 0;
  /* eoh is the first byte of the blank line, which is also the CR that
     ends the last header. Include that CRLF so the last line is whole. */
  size_t hend = eoh + 2;
  int first = 1;
  int minor = 0;
  memset(h, 0, sizeof *h);
  h->cl = -1;
  while (i + 1 < hend) {
    size_t line = i;
    size_t len;
    while (i + 1 < hend && !(buf[i] == '\r' && buf[i + 1] == '\n')) i++;
    len = i - line;
    if (i + 1 >= hend || len == 0) break;
    i += 2;
    /* The receive buffer is not NUL-terminated. sscanf and strtol
       walk until a NUL, which ASan rejects. Copy one line first. */
    {
      char tmp[256];
      if (len >= sizeof tmp) {
        if (first) return -1;
        first = 0;
        continue;
      }
      memcpy(tmp, buf + line, len);
      tmp[len] = 0;
      if (first) {
        if (sscanf(tmp, "HTTP/1.%d %d", &minor, &h->status) != 2) return -1;
        first = 0;
        continue;
      }
      if (len >= 6 && strncmp(tmp, "Date: ", 6) == 0) {
        h->dates += 1;
        if (len == 6 + 29 && imf_ok(tmp + 6)) h->date_ok = 1;
      } else if (len == 17 && strcmp(tmp, "Connection: close") == 0) {
        h->close = 1;
      } else if (len >= 16 && strncmp(tmp, "Content-Length: ", 16) == 0) {
        char *end = NULL;
        long v = strtol(tmp + 16, &end, 10);
        if (end != tmp + len || v < 0) return -1;
        h->has_cl = 1;
        h->cl = v;
      } else if (len >= 19 && strncmp(tmp, "Transfer-Encoding: ", 19) == 0) {
        h->te = 1;
        if (len >= 26 && memcmp(tmp + 19, "chunked", 7) == 0) h->chunked = 1;
      }
    }
  }
  if (first || h->dates != 1) h->date_ok = 0;
  return h->status > 0 ? 0 : -1;
}

static int next_head(Conn *c, Head *h) {
  compact(c);
  for (;;) {
    size_t i;
    for (i = 0; i + 3 < c->n; i++) {
      if (c->buf[i] == '\r' && c->buf[i + 1] == '\n' && c->buf[i + 2] == '\r' &&
          c->buf[i + 3] == '\n') {
        if (parse_head(c->buf, i, h) < 0) return -1;
        c->off = i + 4;
        return 0;
      }
    }
    if (read_more(c) < 0) return -1;
  }
}

static int take_bytes(Conn *c, char *dst, size_t need) {
  size_t got = 0;
  while (got < need) {
    if (c->off < c->n) {
      size_t k = c->n - c->off;
      if (k > need - got) k = need - got;
      memcpy(dst + got, c->buf + c->off, k);
      c->off += k;
      got += k;
      continue;
    }
    {
      ssize_t r = read(c->fd, dst + got, need - got);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) return -1;
      got += (size_t)r;
    }
  }
  return 0;
}

static int take_cl(Conn *c, const Head *h, char *dst, size_t cap, size_t *nout) {
  if (!h->has_cl || h->cl < 0 || (size_t)h->cl >= cap) return -1;
  if (take_bytes(c, dst, (size_t)h->cl) < 0) return -1;
  dst[h->cl] = 0;
  *nout = (size_t)h->cl;
  return 0;
}

static int hex_val(int ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
  if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
  return -1;
}

/* Byte at off+extra must be in the window. The cursor is c->off, so
   a full window can drop the consumed prefix without a second index. */
static int need_off(Conn *c, size_t extra) {
  while (c->n <= c->off + extra) {
    if (c->n >= sizeof c->buf) {
      if (c->off == 0) return -1;
      compact(c);
    }
    if (read_more(c) < 0) return -1;
  }
  return 0;
}

static int take_chunked(Conn *c, char *dst, size_t cap, size_t *nout) {
  size_t out = 0;
  compact(c);
  for (;;) {
    unsigned long sz = 0;
    int digits = 0;
    int hv;
    if (need_off(c, 0) < 0) return -1;
    while ((hv = hex_val((unsigned char)c->buf[c->off])) >= 0) {
      if (digits > 6) return -1;
      sz = (sz << 4) + (unsigned)hv;
      digits += 1;
      c->off += 1;
      if (need_off(c, 0) < 0) return -1;
    }
    if (digits == 0) return -1;
    for (;;) {
      if (need_off(c, 1) < 0) return -1;
      if (c->buf[c->off] == '\r' && c->buf[c->off + 1] == '\n') {
        c->off += 2;
        break;
      }
      if (c->buf[c->off] == '\n' || c->buf[c->off] == '\r') return -1;
      c->off += 1;
    }
    if (sz == 0) {
      for (;;) {
        if (need_off(c, 1) < 0) return -1;
        if (c->buf[c->off] == '\r' && c->buf[c->off + 1] == '\n') {
          c->off += 2;
          *nout = out;
          if (out < cap) dst[out] = 0;
          return 0;
        }
        while (c->buf[c->off] != '\r') {
          c->off += 1;
          if (need_off(c, 1) < 0) return -1;
        }
        if (c->buf[c->off + 1] != '\n') return -1;
        c->off += 2;
      }
    }
    if (out + sz >= cap) return -1;
    if (take_bytes(c, dst + out, sz) < 0) return -1;
    out += sz;
    if (need_off(c, 1) < 0) return -1;
    if (c->buf[c->off] != '\r' || c->buf[c->off + 1] != '\n') return -1;
    c->off += 2;
  }
}

static int take_eof(Conn *c, char *dst, size_t cap, size_t *nout) {
  size_t got = 0;
  while (c->off < c->n) {
    if (got + 1 >= cap) return -1;
    dst[got++] = c->buf[c->off++];
  }
  for (;;) {
    ssize_t r;
    if (got + 1 >= cap) return -1;
    r = read(c->fd, dst + got, cap - 1 - got);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return -1;
    if (r == 0) break;
    got += (size_t)r;
  }
  dst[got] = 0;
  *nout = got;
  return 0;
}

static int open_conn(int port, Conn *c) {
  memset(c, 0, sizeof *c);
  c->fd = dial(port);
  return c->fd < 0 ? -1 : 0;
}

static void close_conn(Conn *c) {
  if (c->fd >= 0) close(c->fd);
  c->fd = -1;
}

static int check_fill(Conn *c, size_t n, int ch) {
  size_t got = 0;
  while (got < n) {
    unsigned char b;
    if (c->off < c->n) {
      b = (unsigned char)c->buf[c->off++];
    } else {
      char tmp[4096];
      size_t want = n - got;
      ssize_t r;
      size_t i;
      if (want > sizeof tmp) want = sizeof tmp;
      r = read(c->fd, tmp, want);
      if (r < 0 && errno == EINTR) continue;
      if (r <= 0) return -1;
      for (i = 0; i < (size_t)r; i++) {
        if ((unsigned char)tmp[i] != (unsigned char)ch) return -1;
      }
      got += (size_t)r;
      continue;
    }
    if (b != (unsigned char)ch) return -1;
    got += 1;
  }
  return 0;
}

static void expect_cl(int port, const char *name, const char *req, int want_status,
                      const char *want_body, int want_close) {
  Conn c;
  Head h;
  char body[256];
  size_t body_n = 0;
  size_t want_n = strlen(want_body);
  if (open_conn(port, &c) < 0) {
    fail(name, "connect");
    return;
  }
  if (send_str(c.fd, req) < 0 || next_head(&c, &h) < 0) {
    fail(name, "exchange");
    close_conn(&c);
    return;
  }
  if (!h.date_ok) fail(name, "date");
  if (h.status != want_status) {
    fprintf(stderr, "FAIL %s: status %d\n", name, h.status);
    failures += 1;
  }
  if (h.close != want_close) fail(name, want_close ? "wanted close" : "wanted keep-alive");
  if (h.chunked) fail(name, "unexpected chunked");
  if (!h.has_cl || take_cl(&c, &h, body, sizeof body, &body_n) < 0) {
    fail(name, "body");
    close_conn(&c);
    return;
  }
  if (body_n != want_n || memcmp(body, want_body, want_n) != 0) {
    fprintf(stderr, "FAIL %s: body '%s'\n", name, body);
    failures += 1;
  }
  close_conn(&c);
}

static void expect_head_cl(int port, const char *name, const char *req, long want_cl) {
  Conn c;
  Head h;
  struct pollfd pfd;
  int pr;
  if (open_conn(port, &c) < 0) {
    fail(name, "connect");
    return;
  }
  if (send_str(c.fd, req) < 0 || next_head(&c, &h) < 0) {
    fail(name, "exchange");
    close_conn(&c);
    return;
  }
  if (!h.date_ok) fail(name, "date");
  if (h.status != 200) {
    fprintf(stderr, "FAIL %s: status %d\n", name, h.status);
    failures += 1;
  }
  if (h.chunked || h.te) fail(name, "transfer-encoding");
  if (!h.has_cl || h.cl != want_cl) {
    fprintf(stderr, "FAIL %s: cl %ld\n", name, h.cl);
    failures += 1;
  }
  if (h.close) fail(name, "closed");
  if (c.off != c.n) fail(name, "body bytes");
  pfd.fd = c.fd;
  pfd.events = POLLIN;
  pr = poll(&pfd, 1, 300);
  if (pr != 0) fail(name, "body arrived");
  close_conn(&c);
}

static void expect_chunked(int port, const char *name, const char *req, const char *want) {
  Conn c;
  Head h;
  char body[256];
  size_t body_n = 0;
  size_t want_n = strlen(want);
  if (open_conn(port, &c) < 0) {
    fail(name, "connect");
    return;
  }
  if (send_str(c.fd, req) < 0 || next_head(&c, &h) < 0 || !h.chunked || h.has_cl) {
    fail(name, "headers");
    close_conn(&c);
    return;
  }
  if (!h.date_ok) fail(name, "date");
  if (h.status != 200) fail(name, "status");
  if (h.close) fail(name, "closed");
  if (take_chunked(&c, body, sizeof body, &body_n) < 0 || body_n != want_n ||
      memcmp(body, want, want_n) != 0) {
    fprintf(stderr, "FAIL %s: chunk body '%s'\n", name, body);
    failures += 1;
  }
  close_conn(&c);
}

static void check_get_blob(int port) {
  Conn c;
  Head h;
  const char *req = "GET /blob HTTP/1.1\r\nHost: h\r\n\r\n";
  if (open_conn(port, &c) < 0) {
    fail("blob", "connect");
    return;
  }
  setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &(struct timeval){.tv_sec = 20}, sizeof(struct timeval));
  if (send_str(c.fd, req) < 0 || next_head(&c, &h) < 0) {
    fail("blob", "headers");
    close_conn(&c);
    return;
  }
  if (!h.date_ok || h.status != 200 || h.chunked || !h.has_cl || h.cl != BLOB || h.close) {
    fprintf(stderr, "FAIL blob: status %d cl %ld te %d\n", h.status, h.cl, h.chunked);
    failures += 1;
    close_conn(&c);
    return;
  }
  if (check_fill(&c, (size_t)BLOB, 'x') < 0) fail("blob", "body");
  close_conn(&c);
}

static void check_post_mib(int port) {
  int fd = dial_tv(port, 20);
  char head[128];
  int hn;
  size_t head_off = 0;
  size_t sent = 0;
  char rbuf[4096];
  size_t rn = 0;
  int eoh = -1;
  Head h;
  size_t got = 0;
  int flags;
  const size_t total = (size_t)BLOB;
  if (fd < 0) {
    fail("post-mib", "connect");
    return;
  }
  hn = snprintf(head, sizeof head, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: %d\r\n\r\n",
                BLOB);
  if (hn < 0 || (size_t)hn >= sizeof head) {
    fail("post-mib", "head");
    close(fd);
    return;
  }
  flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    fail("post-mib", "nonblock");
    close(fd);
    return;
  }
  while (head_off < (size_t)hn || sent < total || eoh < 0 || got < total) {
    struct pollfd pfd;
    int pr;
    pfd.fd = fd;
    pfd.events = 0;
    if (head_off < (size_t)hn || sent < total) pfd.events |= POLLOUT;
    pfd.events |= POLLIN;
    pr = poll(&pfd, 1, 20000);
    if (pr <= 0) {
      fail("post-mib", "timeout");
      close(fd);
      return;
    }
    if (pfd.revents & (POLLERR | POLLNVAL)) {
      fail("post-mib", "socket");
      close(fd);
      return;
    }
    if (pfd.revents & POLLOUT) {
      if (head_off < (size_t)hn) {
        ssize_t w = write(fd, head + head_off, (size_t)hn - head_off);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          fail("post-mib", "write head");
          close(fd);
          return;
        }
        if (w > 0) head_off += (size_t)w;
      } else if (sent < total) {
        char tmp[8192];
        size_t chunk = total - sent;
        ssize_t w;
        if (chunk > sizeof tmp) chunk = sizeof tmp;
        memset(tmp, 'y', chunk);
        w = write(fd, tmp, chunk);
        if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          fail("post-mib", "write body");
          close(fd);
          return;
        }
        if (w > 0) sent += (size_t)w;
      }
    }
    if (pfd.revents & (POLLIN | POLLHUP)) {
      ssize_t r;
      if (eoh >= 0 && got >= total) break;
      if (rn == sizeof rbuf) {
        fail("post-mib", "response buffer");
        close(fd);
        return;
      }
      r = read(fd, rbuf + rn, sizeof rbuf - rn);
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
      if (r <= 0) {
        fail("post-mib", "read");
        close(fd);
        return;
      }
      rn += (size_t)r;
      if (eoh < 0) {
        size_t i;
        for (i = 0; i + 3 < rn; i++) {
          if (rbuf[i] == '\r' && rbuf[i + 1] == '\n' && rbuf[i + 2] == '\r' && rbuf[i + 3] == '\n') {
            eoh = (int)i;
            break;
          }
        }
        if (eoh >= 0) {
          size_t body = (size_t)eoh + 4;
          if (parse_head(rbuf, (size_t)eoh, &h) < 0 || !h.date_ok || h.status != 200 || h.chunked ||
              !h.has_cl || h.cl != BLOB) {
            fprintf(stderr, "FAIL post-mib: status %d cl %ld\n", h.status, h.cl);
            failures += 1;
            close(fd);
            return;
          }
          if (body < rn) {
            size_t k;
            for (k = body; k < rn; k++) {
              if (rbuf[k] != 'y') {
                fail("post-mib", "byte");
                close(fd);
                return;
              }
            }
            got += rn - body;
          }
          rn = 0;
        }
      } else {
        size_t k;
        for (k = 0; k < rn; k++) {
          if (rbuf[k] != 'y') {
            fail("post-mib", "byte");
            close(fd);
            return;
          }
        }
        got += rn;
        rn = 0;
      }
    }
  }
  if (got != total) fail("post-mib", "short");
  close(fd);
}

static void check_continue(int port) {
  Conn c;
  Head h;
  char body[8];
  size_t body_n = 0;
  const char *req = "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n"
                    "Expect: 100-continue\r\n\r\n";
  if (open_conn(port, &c) < 0) {
    fail("continue", "connect");
    return;
  }
  if (send_str(c.fd, req) < 0 || next_head(&c, &h) < 0) {
    fail("continue", "100");
    close_conn(&c);
    return;
  }
  if (h.status != 100 || !h.date_ok || h.has_cl || h.chunked) {
    fprintf(stderr, "FAIL continue: interim status %d date %d cl %d\n", h.status, h.date_ok, h.has_cl);
    failures += 1;
  }
  if (send_str(c.fd, "ping") < 0 || next_head(&c, &h) < 0 ||
      take_cl(&c, &h, body, sizeof body, &body_n) < 0) {
    fail("continue", "final");
    close_conn(&c);
    return;
  }
  if (h.status != 200 || !h.date_ok || h.close || h.cl != 4 || memcmp(body, "ping", 4) != 0)
    fail("continue", "echo");
  close_conn(&c);
}

static void check_http10(int port) {
  Conn c;
  Head h;
  char body[16];
  size_t body_n = 0;
  const char *req = "GET /live HTTP/1.0\r\nHost: h\r\n\r\n";
  if (open_conn(port, &c) < 0) {
    fail("http10", "connect");
    return;
  }
  if (send_str(c.fd, req) < 0 || next_head(&c, &h) < 0) {
    fail("http10", "headers");
    close_conn(&c);
    return;
  }
  if (h.status != 200 || !h.date_ok || !h.close || h.has_cl || h.te) fail("http10", "framing");
  if (take_eof(&c, body, sizeof body, &body_n) < 0 || body_n != 3 || memcmp(body, "abc", 3) != 0)
    fail("http10", "body");
  close_conn(&c);
}

static void check_head_live(int port) {
  Conn c;
  Head h;
  struct pollfd pfd;
  char b;
  ssize_t r;
  const char *req = "HEAD /live HTTP/1.1\r\nHost: h\r\n\r\n";
  if (open_conn(port, &c) < 0) {
    fail("head-live", "connect");
    return;
  }
  if (send_str(c.fd, req) < 0 || next_head(&c, &h) < 0) {
    fail("head-live", "headers");
    close_conn(&c);
    return;
  }
  /* Length is unknown, so HEAD cannot repeat a chunked framing. The
     library closes and writes no body, and it does not call pull. */
  if (h.status != 200 || !h.date_ok || !h.close || h.has_cl || h.te) fail("head-live", "framing");
  if (c.off != c.n) fail("head-live", "buffered body");
  pfd.fd = c.fd;
  pfd.events = POLLIN;
  if (poll(&pfd, 1, 1000) <= 0) {
    fail("head-live", "no close");
    close_conn(&c);
    return;
  }
  r = read(c.fd, &b, 1);
  if (r != 0) fail("head-live", "body");
  close_conn(&c);
}

static int read_fill_n(Conn *c, size_t n, int ch) {
  size_t got = 0;
  while (c->off < c->n && got < n) {
    if ((unsigned char)c->buf[c->off] != (unsigned char)ch) return -1;
    c->off += 1;
    got += 1;
  }
  while (got < n) {
    char tmp[4096];
    size_t want = n - got;
    ssize_t r;
    size_t i;
    if (want > sizeof tmp) want = sizeof tmp;
    r = read(c->fd, tmp, want);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return -1;
    for (i = 0; i < (size_t)r; i++) {
      if ((unsigned char)tmp[i] != (unsigned char)ch) return -1;
    }
    got += (size_t)r;
  }
  return 0;
}

static void check_drain_real(void) {
  int port = -1;
  pid_t pid = start_stream(&port, NULL, "3000");
  Conn idle;
  Conn post;
  Head h;
  char small[8];
  size_t small_n = 0;
  char head[160];
  int hn;
  char hundred[100];
  int i;
  int status = 0;
  int saw_exit = 0;
  if (pid < 0) return;
  if (open_conn(port, &idle) < 0) {
    fail("drain", "idle connect");
    stop_server(pid);
    return;
  }
  if (send_str(idle.fd, "GET / HTTP/1.1\r\nHost: h\r\n\r\n") < 0 || next_head(&idle, &h) < 0 ||
      take_cl(&idle, &h, small, sizeof small, &small_n) < 0 || h.status != 200 || small_n != 3 ||
      memcmp(small, "ok\n", 3) != 0 || h.close) {
    fail("drain", "idle request");
    close_conn(&idle);
    stop_server(pid);
    return;
  }
  if (open_conn(port, &post) < 0) {
    fail("drain", "post connect");
    close_conn(&idle);
    stop_server(pid);
    return;
  }
  hn = snprintf(head, sizeof head, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 20000\r\n\r\n");
  memset(hundred, 'q', sizeof hundred);
  if (hn < 0 || send_all(post.fd, head, (size_t)hn) < 0 || send_all(post.fd, hundred, sizeof hundred) < 0 ||
      next_head(&post, &h) < 0) {
    fail("drain", "inflight");
    close_conn(&post);
    close_conn(&idle);
    stop_server(pid);
    return;
  }
  if (!h.date_ok || h.status != 200 || !h.has_cl || h.cl != 20000) {
    fprintf(stderr, "FAIL drain: status %d cl %ld\n", h.status, h.cl);
    failures += 1;
    close_conn(&post);
    close_conn(&idle);
    stop_server(pid);
    return;
  }
  kill(pid, SIGTERM);
  {
    int closed = 0;
    int j;
    for (j = 0; j < 25; j++) {
      int fd = dial_tv(port, 1);
      if (fd < 0) {
        closed = 1;
        break;
      }
      close(fd);
      usleep(20000);
    }
    if (!closed) fail("drain", "listen stayed open");
  }
  {
    char rest[19900];
    memset(rest, 'q', sizeof rest);
    if (send_all(post.fd, rest, sizeof rest) < 0 || read_fill_n(&post, 20000, 'q') < 0)
      fail("drain", "echo");
  }
  close_conn(&post);
  for (i = 0; i < 40; i++) {
    struct pollfd pfd;
    int fd;
    pfd.fd = idle.fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 50) > 0) {
      char b;
      ssize_t r = read(idle.fd, &b, 1);
      if (r == 0) break;
      if (r > 0) {
        fail("drain", "idle data");
        break;
      }
    }
    fd = dial_tv(port, 1);
    if (fd < 0 && i > 2) {
      /* Listen should be closed once the in-flight response is done.
         Keep polling the idle socket until it closes too. */
    }
    if (fd >= 0) close(fd);
  }
  {
    struct pollfd pfd;
    char b;
    pfd.fd = idle.fd;
    pfd.events = POLLIN;
    if (poll(&pfd, 1, 2000) <= 0 || read(idle.fd, &b, 1) != 0) fail("drain", "idle still open");
  }
  {
    int opened = 0;
    for (i = 0; i < 20; i++) {
      int fd = dial_tv(port, 1);
      if (fd < 0) {
        opened = 0;
        break;
      }
      opened = 1;
      close(fd);
      usleep(50000);
    }
    if (opened) fail("drain", "still accepting");
  }
  close_conn(&idle);
  for (i = 0; i < 80; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      saw_exit = 1;
      break;
    }
    usleep(50000);
  }
  if (!saw_exit) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    fail("drain", "did not exit");
    return;
  }
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) fail("drain", "status");
}

static void check_drain_cut(void) {
  int port = -1;
  pid_t pid = start_stream(&port, NULL, "400");
  Conn post;
  Head h;
  char head[160];
  char partial[64];
  int hn;
  int status = 0;
  int saw = 0;
  int i;
  int closed = 0;
  memset(&post, 0, sizeof post);
  post.fd = -1;
  if (pid < 0) return;
  hn = snprintf(head, sizeof head, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 20000\r\n\r\n");
  memset(partial, 'q', sizeof partial);
  if (hn < 0 || open_conn(port, &post) < 0 || send_all(post.fd, head, (size_t)hn) < 0 ||
      send_all(post.fd, partial, sizeof partial) < 0 || next_head(&post, &h) < 0 || h.status != 200) {
    fail("drain-cut", "inflight");
    close_conn(&post);
    stop_server(pid);
    return;
  }
  kill(pid, SIGTERM);
  for (i = 0; i < 25; i++) {
    int fd = dial_tv(port, 1);
    if (fd < 0) {
      closed = 1;
      break;
    }
    close(fd);
    usleep(20000);
  }
  if (!closed) fail("drain-cut", "listen stayed open");
  close_conn(&post);
  for (i = 0; i < 40; i++) {
    pid_t got = waitpid(pid, &status, WNOHANG);
    if (got == pid) {
      saw = 1;
      break;
    }
    usleep(50000);
  }
  if (!saw) {
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    fail("drain-cut", "did not exit");
    return;
  }
  if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) fail("drain-cut", "status");
}

static void check_two(int port) {
  Conn a;
  Conn b;
  Head ha;
  Head hb;
  char small[8];
  size_t small_n = 0;
  char head[160];
  char partial[32];
  char rest[19968];
  int hn;
  memset(&a, 0, sizeof a);
  memset(&b, 0, sizeof b);
  a.fd = -1;
  b.fd = -1;
  hn = snprintf(head, sizeof head, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 20000\r\n\r\n");
  memset(partial, 'm', sizeof partial);
  if (hn < 0 || open_conn(port, &a) < 0 || send_all(a.fd, head, (size_t)hn) < 0 ||
      send_all(a.fd, partial, sizeof partial) < 0 || next_head(&a, &ha) < 0 || ha.status != 200 ||
      !ha.has_cl || ha.cl != 20000) {
    fail("two", "post");
    close_conn(&a);
    return;
  }
  if (open_conn(port, &b) < 0 || send_str(b.fd, "GET / HTTP/1.1\r\nHost: h\r\n\r\n") < 0 ||
      next_head(&b, &hb) < 0 || take_cl(&b, &hb, small, sizeof small, &small_n) < 0 ||
      hb.status != 200 || small_n != 3 || memcmp(small, "ok\n", 3) != 0) {
    fail("two", "other");
    close_conn(&a);
    close_conn(&b);
    return;
  }
  memset(rest, 'm', sizeof rest);
  if (send_all(a.fd, rest, sizeof rest) < 0 || read_fill_n(&a, 20000, 'm') < 0) fail("two", "echo");
  close_conn(&a);
  close_conn(&b);
}

static void check_http10_expect(int port) {
  Conn c;
  Head h;
  char body[8];
  size_t body_n = 0;
  const char *req = "POST /echo HTTP/1.0\r\nContent-Length: 4\r\nExpect: 100-continue\r\n\r\nping";
  if (open_conn(port, &c) < 0) {
    fail("http10-expect", "connect");
    return;
  }
  if (send_str(c.fd, req) < 0 || next_head(&c, &h) < 0) {
    fail("http10-expect", "headers");
    close_conn(&c);
    return;
  }
  if (h.status != 200 || !h.date_ok || !h.close || h.chunked) {
    fprintf(stderr, "FAIL http10-expect: status %d close %d\n", h.status, h.close);
    failures += 1;
  }
  if (!h.has_cl || take_cl(&c, &h, body, sizeof body, &body_n) < 0 || body_n != 4 ||
      memcmp(body, "ping", 4) != 0)
    fail("http10-expect", "body");
  close_conn(&c);
}

static void check_continue_stall(void) {
  int port = -1;
  pid_t pid;
  Conn c;
  Head h;
  char body[8];
  size_t body_n = 0;
  const char *req = "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n"
                    "Expect: 100-continue\r\n\r\n";
  setenv("MURO_TEST_WRITE_LIMIT", "8", 1);
  setenv("MURO_TEST_100_STALL", "1", 1);
  pid = start_stream(&port, NULL, NULL);
  unsetenv("MURO_TEST_WRITE_LIMIT");
  unsetenv("MURO_TEST_100_STALL");
  if (pid < 0) return;
  if (open_conn(port, &c) < 0) {
    fail("continue-stall", "connect");
    stop_server(pid);
    return;
  }
  if (send_str(c.fd, req) < 0 || next_head(&c, &h) < 0 || h.status != 100 || !h.date_ok) {
    fprintf(stderr, "FAIL continue-stall: interim %d\n", h.status);
    failures += 1;
    close_conn(&c);
    stop_server(pid);
    return;
  }
  if (send_str(c.fd, "ping") < 0 || next_head(&c, &h) < 0 ||
      take_cl(&c, &h, body, sizeof body, &body_n) < 0 || h.status != 200 || body_n != 4 ||
      memcmp(body, "ping", 4) != 0)
    fail("continue-stall", "echo");
  close_conn(&c);
  stop_server(pid);
}

/* Expect: 100-continue, a stalled interim, the body, then a second
   request, all before the interim finishes. Both responses must
   arrive. The stall sleeps inside the server, so these waits overlap
   that window. A 3s receive timeout fails a held pipeline. */
static void check_continue_pipe(void) {
  int port = -1;
  pid_t pid;
  Conn c;
  Head h;
  char body[8];
  size_t body_n = 0;
  struct timespec ts;
  struct timeval tv;
  const char *hdr = "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n"
                    "Expect: 100-continue\r\n\r\n";
  const char *second = "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";
  setenv("MURO_TEST_WRITE_LIMIT", "8", 1);
  setenv("MURO_TEST_100_STALL", "40", 1);
  pid = start_stream(&port, NULL, NULL);
  unsetenv("MURO_TEST_WRITE_LIMIT");
  unsetenv("MURO_TEST_100_STALL");
  if (pid < 0) return;
  if (open_conn(port, &c) < 0) {
    fail("continue-pipe", "connect");
    stop_server(pid);
    return;
  }
  tv.tv_sec = 3;
  tv.tv_usec = 0;
  setsockopt(c.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ts.tv_sec = 0;
  ts.tv_nsec = 200000000L;
  if (send_str(c.fd, hdr) < 0) {
    fail("continue-pipe", "headers");
    close_conn(&c);
    stop_server(pid);
    return;
  }
  nanosleep(&ts, NULL);
  if (send_str(c.fd, "ping") < 0) {
    fail("continue-pipe", "body");
    close_conn(&c);
    stop_server(pid);
    return;
  }
  nanosleep(&ts, NULL);
  if (send_str(c.fd, second) < 0) {
    fail("continue-pipe", "pipeline");
    close_conn(&c);
    stop_server(pid);
    return;
  }
  if (next_head(&c, &h) < 0 || h.status != 100 || !h.date_ok) {
    fprintf(stderr, "FAIL continue-pipe: interim %d\n", h.status);
    failures += 1;
    close_conn(&c);
    stop_server(pid);
    return;
  }
  if (next_head(&c, &h) < 0 || take_cl(&c, &h, body, sizeof body, &body_n) < 0 || h.status != 200 ||
      body_n != 4 || memcmp(body, "ping", 4) != 0) {
    fail("continue-pipe", "echo");
    close_conn(&c);
    stop_server(pid);
    return;
  }
  body_n = 0;
  if (next_head(&c, &h) < 0 || take_cl(&c, &h, body, sizeof body, &body_n) < 0 || h.status != 200 ||
      !h.close || body_n != 3 || memcmp(body, "ok\n", 3) != 0)
    fail("continue-pipe", "next");
  close_conn(&c);
  stop_server(pid);
}

static void recv_limit(Conn *c, int sec) {
  struct timeval tv;
  tv.tv_sec = sec;
  tv.tv_usec = 0;
  setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

static void check_stream_cl(int port) {
  Conn c;
  Head h;
  char *payload;
  char head[160];
  char small[8];
  size_t small_n = 0;
  int hn;
  hn = snprintf(head, sizeof head, "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 20000\r\n\r\n");
  payload = malloc(20000);
  if (!payload || hn < 0 || open_conn(port, &c) < 0) {
    free(payload);
    fail("stream-next-cl", "connect");
    return;
  }
  recv_limit(&c, 3);
  memset(payload, 's', 20000);
  if (send_all(c.fd, head, (size_t)hn) < 0 || next_head(&c, &h) < 0 || h.status != 200 || !h.has_cl ||
      h.cl != 20000 || h.close) {
    fail("stream-next-cl", "headers");
    free(payload);
    close_conn(&c);
    return;
  }
  if (send_all(c.fd, payload, 20000) < 0 || check_fill(&c, 20000, 's') < 0) {
    fail("stream-next-cl", "echo");
    free(payload);
    close_conn(&c);
    return;
  }
  free(payload);
  if (send_str(c.fd, "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n") < 0 ||
      next_head(&c, &h) < 0 || take_cl(&c, &h, small, sizeof small, &small_n) < 0 || h.status != 200 ||
      !h.close || small_n != 3 || memcmp(small, "ok\n", 3) != 0)
    fail("stream-next-cl", "next");
  close_conn(&c);
}

static void check_stream_chunk(int port) {
  Conn c;
  Head h;
  char *partial;
  char *rest;
  char *echo;
  char head[180];
  char small[8];
  size_t small_n = 0;
  size_t echo_n = 0;
  int hn;
  hn = snprintf(head, sizeof head,
                "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n4e20\r\n");
  partial = malloc(17000);
  rest = malloc(3000);
  echo = malloc(20001);
  if (!partial || !rest || !echo || hn < 0 || open_conn(port, &c) < 0) {
    free(partial);
    free(rest);
    free(echo);
    fail("stream-next-chunk", "connect");
    return;
  }
  recv_limit(&c, 3);
  memset(partial, 'k', 17000);
  memset(rest, 'k', 3000);
  if (send_all(c.fd, head, (size_t)hn) < 0 || send_all(c.fd, partial, 17000) < 0 ||
      next_head(&c, &h) < 0 || h.status != 200 || !h.chunked || h.close) {
    fail("stream-next-chunk", "headers");
    free(partial);
    free(rest);
    free(echo);
    close_conn(&c);
    return;
  }
  free(partial);
  if (send_all(c.fd, rest, 3000) < 0 || send_str(c.fd, "\r\n0\r\n\r\n") < 0 ||
      take_chunked(&c, echo, 20001, &echo_n) < 0 || echo_n != 20000) {
    fail("stream-next-chunk", "echo");
    free(rest);
    free(echo);
    close_conn(&c);
    return;
  }
  free(rest);
  {
    size_t i;
    for (i = 0; i < echo_n; i++) {
      if (echo[i] != 'k') {
        fail("stream-next-chunk", "bytes");
        break;
      }
    }
  }
  free(echo);
  if (send_str(c.fd, "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n") < 0 ||
      next_head(&c, &h) < 0 || take_cl(&c, &h, small, sizeof small, &small_n) < 0 || h.status != 200 ||
      !h.close || small_n != 3 || memcmp(small, "ok\n", 3) != 0)
    fail("stream-next-chunk", "next");
  close_conn(&c);
}

int main(int argc, char **argv) {
  pid_t pid;
  int port = -1;
  pid_t pid_small;
  int port_small = -1;
  if (argc > 1) bin_path = argv[1];
  signal(SIGPIPE, SIG_IGN);
  alarm(120);
  pid = start_stream(&port, NULL, NULL);
  if (pid < 0) return 1;

  expect_cl(port, "get", "GET / HTTP/1.1\r\nHost: h\r\n\r\n", 200, "ok\n", 0);
  expect_head_cl(port, "head", "HEAD / HTTP/1.1\r\nHost: h\r\n\r\n", 3);
  expect_head_cl(port, "head-blob", "HEAD /blob HTTP/1.1\r\nHost: h\r\n\r\n", BLOB);
  expect_cl(port, "put", "PUT /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 2\r\n\r\nxy", 200, "xy",
            0);
  expect_cl(port, "patch", "PATCH /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 1\r\n\r\nz", 200, "z",
            0);
  expect_cl(port, "delete", "DELETE /echo HTTP/1.1\r\nHost: h\r\n\r\n", 204, "", 0);
  expect_cl(port, "options", "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n", 200, "", 0);
  expect_cl(port, "trace", "TRACE / HTTP/1.1\r\nHost: h\r\n\r\n", 501, "unimplemented", 1);
  expect_cl(port, "bad-method", "G(ET / HTTP/1.1\r\nHost: h\r\n\r\n", 400, "malformed", 1);
  expect_cl(port, "get-star", "GET * HTTP/1.1\r\nHost: h\r\n\r\n", 400, "malformed", 1);
  expect_chunked(port, "chunk",
                 "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "4\r\nwiki\r\n0\r\n\r\n",
                 "wiki");
  expect_chunked(port, "chunk-ext",
                 "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "4;ext=1\r\nwiki\r\n0\r\nX-A: b\r\n\r\n",
                 "wiki");
  expect_chunked(port, "chunk-two",
                 "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "1\r\na\r\n1\r\nb\r\n0\r\n\r\n",
                 "ab");
  expect_cl(port, "chunk-bad",
            "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n", 400,
            "malformed", 1);
  expect_cl(port, "chunk-colon",
            "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
            "4\r\nwiki\r\n0\r\nNope\r\n\r\n",
            400, "malformed", 1);
  expect_cl(port, "chunk-overflow",
            "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
            "10000000000000000\r\n",
            400, "malformed", 1);
  expect_cl(port, "chunk-huge",
            "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n1000000\r\n", 413,
            "too large", 1);
  expect_cl(port, "te-cl",
            "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
            "Content-Length: 5\r\n\r\n0\r\n\r\n",
            400, "malformed", 1);
  expect_cl(port, "cl-te",
            "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n"
            "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
            400, "malformed", 1);
  expect_cl(port, "te-gzip", "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n\r\n", 501,
            "unimplemented", 1);
  expect_cl(port, "te-list",
            "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked, gzip\r\n\r\n", 400,
            "malformed", 1);
  expect_cl(port, "te-gzip-final",
            "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip, chunked\r\n\r\n", 501,
            "unimplemented", 1);
  expect_cl(port, "enclose", "GET / HTTP/1.1\r\nHost: h\r\nConnection: enclose\r\n\r\n", 200, "ok\n",
            0);
  expect_cl(port, "close-foo", "GET / HTTP/1.1\r\nHost: h\r\nConnection: close, foo\r\n\r\n", 200,
            "ok\n", 1);
  expect_cl(port, "close-ws", "GET / HTTP/1.1\r\nHost: h\r\nConnection:  close \r\n\r\n", 200, "ok\n",
            1);
  expect_cl(port, "close-then-alive",
            "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\nConnection: keep-alive\r\n\r\n", 200,
            "ok\n", 1);
  expect_cl(port, "alive-then-close",
            "GET / HTTP/1.1\r\nHost: h\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n", 200,
            "ok\n", 1);
  expect_cl(port, "http10-keep", "GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", 200, "ok\n", 0);
  expect_cl(port, "bare-lf", "GET / HTTP/1.1\nHost: h\n\n", 400, "malformed", 1);
  expect_cl(port, "empty-target", "GET  HTTP/1.1\r\nHost: h\r\n\r\n", 400, "malformed", 1);
  expect_cl(port, "cl-plus", "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: +1\r\n\r\nx", 400,
            "malformed", 1);
  expect_cl(port, "cl-bound",
            "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4294967296\r\n\r\n", 400,
            "malformed", 1);
  expect_cl(port, "te-twice",
            "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
            "Transfer-Encoding: chunked\r\n\r\n",
            400, "malformed", 1);
  expect_cl(port, "te-http10",
            "POST /echo HTTP/1.0\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400,
            "malformed", 1);
  expect_chunked(port, "live", "GET /live HTTP/1.1\r\nHost: h\r\n\r\n", "abc");
  expect_cl(port, "too-big", "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 20000000\r\n\r\n",
            413, "too large", 1);
  expect_cl(port, "expect-other",
            "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\nExpect: nope\r\n\r\n", 417,
            "expectation", 1);
  check_continue(port);
  check_http10_expect(port);
  check_two(port);
  check_http10(port);
  check_head_live(port);
  check_get_blob(port);
  check_post_mib(port);
  check_stream_cl(port);
  check_stream_chunk(port);
  stop_server(pid);

  pid_small = start_stream(&port_small, "1000", NULL);
  if (pid_small > 0) {
    expect_cl(port_small, "max-under", "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 4\r\n\r\nping",
              200, "ping", 0);
    expect_cl(port_small, "max-cl",
              "POST /echo HTTP/1.1\r\nHost: h\r\nContent-Length: 1001\r\n\r\n", 413, "too large", 1);
    expect_cl(port_small, "max-chunk",
              "POST /echo HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
              "3e9\r\n",
              413, "too large", 1);
    stop_server(pid_small);
  }

  check_drain_real();
  check_drain_cut();
  check_continue_stall();
  check_continue_pipe();
  if (failures != 0) {
    fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  printf("OK\n");
  return 0;
}
