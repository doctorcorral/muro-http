#!/usr/bin/env python3
"""HTTP/1.1 conformance against c/muro-hello on Linux epoll.

Each case names the expected status or the close. The server is the
real hello binary: GET and POST / answer hello\\n. POST /echo echoes
the body, which is what the chunk, expect, and pipeline cases use.
One worker, --wait epoll. No extra packages.

Limits this server already has, and this file checks on the wire:
  request line longer than 16384 bytes -> 414, close
  any other header block past 16384     -> 431, close
  Content-Length or chunk size past --max-body -> 413, close
  idle silence longer than --idle-ms    -> close, no status
"""

import os
import select
import socket
import subprocess
import sys
import time

HOST = "127.0.0.1"
HELLO = b"hello\n"
# Book cap. A request line with no LF in this many bytes is 414.
# A header block longer than this is 431.
HDR_CAP = 16384

results = []


class Fail(Exception):
    pass


def record(name, expected, ok, detail=""):
    results.append((name, "pass" if ok else "fail", expected, detail))
    if ok:
        print("PASS %s  expected %s" % (name, expected))
    else:
        print("FAIL %s  expected %s: %s" % (name, expected, detail))


def check(name, expected, fn):
    try:
        fn()
    except Fail as e:
        record(name, expected, False, str(e))
        return 1
    except Exception as e:
        record(name, expected, False, "%s: %s" % (type(e).__name__, e))
        return 1
    record(name, expected, True)
    return 0


class Server:
    def __init__(self, bin_path, idle_ms=4000, max_body=None, max_conn=48):
        self.bin_path = bin_path
        self.idle_ms = idle_ms
        self.max_body = max_body
        self.max_conn = max_conn
        self.proc = None
        self.port = None
        self.err = []
        self._err_thread = None

    def start(self):
        argv = [
            self.bin_path,
            "--bind",
            HOST,
            "--port",
            "0",
            "--wait",
            "epoll",
            "--workers",
            "1",
            "--idle-ms",
            str(self.idle_ms),
            "--max-connections",
            str(self.max_conn),
            "--drain-ms",
            "200",
        ]
        if self.max_body is not None:
            argv += ["--max-body", str(self.max_body)]
        env = {k: v for k, v in os.environ.items() if not k.startswith("MURO_")}
        env["MURO_HTTP_WAIT"] = "epoll"
        self.proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        line = self._readline(self.proc.stdout, 3)
        if not line.startswith("listening %s " % HOST):
            raise Fail("banner %r" % line)
        self.port = int(line.split()[-1])
        err = self._read_ready(self.proc.stderr, 0.5)
        self.err.append(err)
        if b"wait epoll" not in err:
            raise Fail("wait backend %r" % err)
        import threading

        def drain():
            try:
                while True:
                    chunk = self.proc.stderr.read(4096)
                    if not chunk:
                        break
                    self.err.append(chunk)
            except Exception:
                pass

        self._err_thread = threading.Thread(target=drain, daemon=True)
        self._err_thread.start()

    def _readline(self, pipe, timeout):
        fd = pipe.fileno()
        acc = b""
        end = time.monotonic() + timeout
        while b"\n" not in acc:
            left = end - time.monotonic()
            if left <= 0:
                raise Fail("no banner")
            r, _, _ = select.select([fd], [], [], left)
            if not r:
                raise Fail("no banner")
            b = os.read(fd, 1)
            if not b:
                raise Fail("banner eof")
            acc += b
        return acc.decode("ascii", "replace").strip()

    def _read_ready(self, pipe, timeout):
        fd = pipe.fileno()
        acc = b""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            left = end - time.monotonic()
            r, _, _ = select.select([fd], [], [], max(0, left))
            if not r:
                break
            acc += os.read(fd, 4096)
        return acc

    def stop(self):
        if not self.proc:
            return
        if self.proc.poll() is None:
            self.proc.send_signal(15)
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)
        if self.proc.stdout:
            self.proc.stdout.close()
        if self.proc.stderr:
            self.proc.stderr.close()


class Resp:
    def __init__(self, status, reason, headers, body):
        self.status = status
        self.reason = reason
        self.headers = headers
        self.body = body

    def values(self, name):
        name = name.lower()
        return [v for k, v in self.headers if k == name]

    def tokens(self, name):
        out = []
        for v in self.values(name):
            for part in v.split(","):
                part = part.strip().lower()
                if part:
                    out.append(part)
        return out


class Conn:
    def __init__(self, port, timeout=3):
        self.s = socket.create_connection((HOST, port), timeout=timeout)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def send(self, data):
        if isinstance(data, str):
            data = data.encode("latin1")
        self.s.sendall(data)

    def shutdown_write(self):
        try:
            self.s.shutdown(socket.SHUT_WR)
        except OSError:
            pass

    def _pull(self, deadline):
        left = deadline - time.monotonic()
        if left <= 0:
            raise Fail("timeout")
        self.s.settimeout(min(0.4, left))
        try:
            b = self.s.recv(8192)
        except socket.timeout:
            return False
        if not b:
            return True
        self.buf += b
        if len(self.buf) > 2 * 1024 * 1024:
            raise Fail("response too large")
        return False

    def _until(self, pred, deadline):
        while not pred():
            if self._pull(deadline):
                return True
        return False

    def _line(self, deadline):
        while b"\r\n" not in self.buf:
            if self._pull(deadline):
                raise Fail("eof in line")
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line

    def read_response(self, deadline=None):
        if deadline is None:
            deadline = time.monotonic() + 3
        eof = self._until(lambda: b"\r\n\r\n" in self.buf, deadline)
        if b"\r\n\r\n" not in self.buf:
            if eof:
                return None
            raise Fail("timeout before headers")
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.split(b"\r\n")
        if not lines or not lines[0]:
            raise Fail("empty status")
        parts = lines[0].split(b" ", 2)
        if len(parts) < 2:
            raise Fail("bad status %r" % lines[0])
        try:
            status = int(parts[1])
        except ValueError:
            raise Fail("bad status %r" % lines[0])
        reason = parts[2].decode("latin1") if len(parts) > 2 else ""
        headers = []
        for line in lines[1:]:
            if b":" not in line:
                raise Fail("bad header %r" % line)
            k, v = line.split(b":", 1)
            headers.append((k.decode("latin1").lower(), v.strip().decode("latin1")))
        resp = Resp(status, reason, headers, b"")
        if 100 <= status <= 199:
            return resp
        if "chunked" in resp.tokens("transfer-encoding"):
            resp.body = self._chunks(deadline)
            return resp
        cls = resp.values("content-length")
        if len(cls) == 1:
            try:
                need = int(cls[0])
            except ValueError:
                raise Fail("bad content-length")
            eof = self._until(lambda: len(self.buf) >= need, deadline)
            if len(self.buf) < need:
                raise Fail("short body eof" if eof else "short body")
            resp.body = self.buf[:need]
            self.buf = self.buf[need:]
            return resp
        if "close" in resp.tokens("connection"):
            while not self._pull(deadline):
                pass
            resp.body = self.buf
            self.buf = b""
            return resp
        raise Fail("no body length")

    def _chunks(self, deadline):
        body = b""
        while True:
            line = self._line(deadline)
            size_txt = line.split(b";", 1)[0].strip()
            try:
                size = int(size_txt, 16)
            except ValueError:
                raise Fail("bad chunk size")
            if size == 0:
                while True:
                    t = self._line(deadline)
                    if t == b"":
                        break
                return body
            eof = self._until(lambda: len(self.buf) >= size + 2, deadline)
            if len(self.buf) < size + 2:
                raise Fail("short chunk" if eof else "chunk timeout")
            body += self.buf[:size]
            if self.buf[size : size + 2] != b"\r\n":
                raise Fail("chunk crlf")
            self.buf = self.buf[size + 2 :]

    def expect_eof(self, deadline=None):
        if deadline is None:
            deadline = time.monotonic() + 1.5
        if self.buf:
            raise Fail("bytes after response %r" % self.buf[:80])
        while True:
            if self._pull(deadline):
                return
            if self.buf:
                raise Fail("extra bytes %r" % self.buf[:80])


def expect_status(resp, status, body=None, close=None):
    if resp is None:
        raise Fail("closed with no status")
    if resp.status != status:
        raise Fail("status %s body %r" % (resp.status, resp.body[:80]))
    if body is not None and resp.body != body:
        raise Fail("body %r" % resp.body[:80])
    if not resp.values("date"):
        raise Fail("no date")
    if close is True and "close" not in resp.tokens("connection"):
        raise Fail("connection %s" % resp.tokens("connection"))
    if close is False and "close" in resp.tokens("connection"):
        raise Fail("unexpected close")
    if close is False and "keep-alive" not in resp.tokens("connection"):
        raise Fail("connection %s" % resp.tokens("connection"))


def one(srv, raw, status, body=None, close=None, timeout=3):
    c = Conn(srv.port)
    try:
        c.send(raw)
        resp = c.read_response(time.monotonic() + timeout)
        expect_status(resp, status, body, close)
        if close:
            c.expect_eof()
        return resp
    finally:
        c.close()


def run_exchange(srv):
    fails = 0

    def case(name, expected, raw, status, body=None, close=None):
        nonlocal fails
        fails += check(name, expected, lambda: one(srv, raw, status, body, close))

    # 1. Request line.
    case(
        "req-bad-method",
        "400 close",
        b"G(ET / HTTP/1.1\r\nHost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-empty-target",
        "400 close",
        b"GET  HTTP/1.1\r\nHost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-bad-version",
        "400 close",
        b"GET / HTTP/2.0\r\nHost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-version-extra",
        "400 close",
        b"GET / HTTP/1.11\r\nHost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-version-space",
        "400 close",
        b"GET / HTTP/1.1 \r\nHost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-bare-lf",
        "400 close (missing CRLF)",
        b"GET / HTTP/1.1\nHost: t\n\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-tab-separator",
        "400 close",
        b"GET\t/ HTTP/1.1\r\nHost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-cr-in-line",
        "400 close",
        b"GET / HTTP/1.1\rHost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-missing-version",
        "400 close",
        b"GET /\r\nHost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-nul-method",
        "400 close",
        b"GE\x00T / HTTP/1.1\r\nHost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "req-lower-method",
        "501 close",
        b"get / HTTP/1.1\r\nHost: t\r\n\r\n",
        501,
        b"unimplemented",
        True,
    )
    case(
        "req-trace",
        "501 close",
        b"TRACE / HTTP/1.1\r\nHost: t\r\n\r\n",
        501,
        b"unimplemented",
        True,
    )
    case(
        "req-long-target-ok",
        "200 keep-alive",
        b"GET /" + b"p" * 2000 + b" HTTP/1.1\r\nHost: t\r\n\r\n",
        200,
        HELLO,
        False,
    )

    # 2. obs-fold.
    case(
        "obs-fold-space",
        "400 close",
        b"GET / HTTP/1.1\r\nHost: t\r\nX-A: one\r\n two\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "obs-fold-tab",
        "400 close",
        b"GET / HTTP/1.1\r\nHost: t\r\nX-A: one\r\n\ttwo\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "obs-fold-length",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\n 2\r\n\r\nxx",
        400,
        b"malformed",
        True,
    )
    case(
        "header-space-before-colon",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length : 1\r\n\r\nx",
        400,
        b"malformed",
        True,
    )
    case(
        "header-leading-space",
        "400 close",
        b"GET / HTTP/1.1\r\n Host: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )

    # 3. Duplicate Content-Length. 4. TE + CL.
    case(
        "cl-dup-same",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\nx",
        400,
        b"malformed",
        True,
    )
    case(
        "cl-dup-conflict",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nxx",
        400,
        b"malformed",
        True,
    )
    case(
        "cl-list",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 1, 1\r\n\r\nx",
        400,
        b"malformed",
        True,
    )
    case(
        "te-then-cl",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n\r\n0\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "cl-then-te",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        400,
        b"malformed",
        True,
    )

    # 5. Chunks.
    case(
        "chunk-ok",
        "200 keep-alive body hi",
        b"POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\n\r\n",
        200,
        b"hi",
        False,
    )
    case(
        "chunk-ext",
        "200 keep-alive",
        b"POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n4;ext=1\r\nwiki\r\n0\r\n\r\n",
        200,
        b"wiki",
        False,
    )
    case(
        "chunk-bad-size",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "chunk-size-space",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n4 \r\nwiki\r\n0\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "chunk-ext-lf",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n4;bad\n\r\nwiki\r\n0\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "chunk-overflow",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n10000000000000000\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "chunk-too-big",
        "413 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n1000000\r\n",
        413,
        b"too large",
        True,
    )
    case(
        "chunk-trailer-bare",
        "400 close",
        b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nNope\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "chunk-trailer-ok",
        "200 keep-alive",
        b"POST /echo HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nhi\r\n0\r\nX-A: b\r\n\r\n",
        200,
        b"hi",
        False,
    )

    # 6. Host.
    case(
        "host-missing",
        "400 close",
        b"GET / HTTP/1.1\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "host-two",
        "400 close",
        b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "host-two-same",
        "400 close",
        b"GET / HTTP/1.1\r\nHost: t\r\nhost: t\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "host-10-omit",
        "200 close",
        b"GET / HTTP/1.0\r\n\r\n",
        200,
        HELLO,
        True,
    )
    case(
        "host-10-two",
        "400 close",
        b"GET / HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n",
        400,
        b"malformed",
        True,
    )
    case(
        "host-absolute",
        "200 keep-alive",
        b"GET http://t/index HTTP/1.1\r\nHost: t\r\n\r\n",
        200,
        HELLO,
        False,
    )
    case(
        "host-absolute-mismatch",
        "200 keep-alive (authority is not compared)",
        b"GET http://other.example/index HTTP/1.1\r\nHost: t\r\n\r\n",
        200,
        HELLO,
        False,
    )

    # 11. Sizes that are headers only. Body 413 does not send the body.
    line = b"GET /" + b"a" * 17000 + b" HTTP/1.1\r\nHost: t\r\n\r\n"
    case("line-too-long", "414 close", line, 414, b"uri too long", True)
    headers = b"GET / HTTP/1.1\r\nHost: t\r\nX-Pad: " + b"b" * 17000 + b"\r\n\r\n"
    case("headers-too-long", "431 close", headers, 431, b"header fields", True)
    pad = b"GET / HTTP/1.1\r\nHost: t\r\nX-Pad: " + b"c" * 1000 + b"\r\n\r\n"
    case("headers-under-cap", "200 keep-alive", pad, 200, HELLO, False)
    case(
        "body-too-long",
        "413 close, body not required",
        b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 8388609\r\n\r\n",
        413,
        b"too large",
        True,
    )
    case(
        "post-echo",
        "200 keep-alive",
        b"POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping",
        200,
        b"ping",
        False,
    )
    case(
        "post-slash",
        "200 keep-alive, hello not the body",
        b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\nping",
        200,
        HELLO,
        False,
    )
    return fails


def run_smuggle(srv):
    """A rejected framing must not answer the following request."""

    def go(raw):
        c = Conn(srv.port)
        try:
            c.send(raw)
            resp = c.read_response()
            expect_status(resp, 400, b"malformed", True)
            if b"hello" in resp.body:
                raise Fail("smuggled body")
            c.expect_eof()
        finally:
            c.close()

    fails = 0
    fails += check(
        "smuggle-cl-te",
        "400 close, no second response",
        lambda: go(
            b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n"
            b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n"
            b"GET / HTTP/1.1\r\nHost: t\r\n\r\n"
        ),
    )
    fails += check(
        "smuggle-te-cl",
        "400 close, no second response",
        lambda: go(
            b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n"
            b"Content-Length: 4\r\n\r\n0\r\n\r\n"
            b"GET / HTTP/1.1\r\nHost: t\r\n\r\n"
        ),
    )
    fails += check(
        "smuggle-dup-cl",
        "400 close, no second response",
        lambda: go(
            b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n"
            b"xGET / HTTP/1.1\r\nHost: t\r\n\r\n"
        ),
    )
    fails += check(
        "pipeline-bad-then-get",
        "400 close, pipelined GET dropped",
        lambda: go(b"GET / HTTP/1.1\r\n\r\nGET / HTTP/1.1\r\nHost: t\r\n\r\n"),
    )
    return fails


def run_continue(srv):
    fails = 0

    def withhold():
        c = Conn(srv.port)
        try:
            c.send(
                b"POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n"
                b"Expect: 100-continue\r\n\r\n"
            )
            first = c.read_response(time.monotonic() + 2)
            expect_status(first, 100)
            if first.body:
                raise Fail("100 has a body")
            c.send(b"ping")
            second = c.read_response()
            expect_status(second, 200, b"ping", False)
        finally:
            c.close()

    def bad_expect():
        c = Conn(srv.port)
        try:
            c.send(
                b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\nExpect: dinner\r\n\r\n"
            )
            resp = c.read_response(time.monotonic() + 2)
            expect_status(resp, 417, b"expectation", True)
            c.expect_eof()
        finally:
            c.close()

    def expect_too_big():
        c = Conn(srv.port)
        try:
            c.send(
                b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 8388609\r\n"
                b"Expect: 100-continue\r\n\r\n"
            )
            resp = c.read_response(time.monotonic() + 2)
            if resp is None:
                raise Fail("closed with no status")
            if resp.status == 100:
                raise Fail("sent 100 for a body over the cap")
            expect_status(resp, 413, b"too large", True)
            c.expect_eof()
        finally:
            c.close()

    def get_expect():
        # No body, so the request is already complete. No 100.
        one(
            srv,
            b"GET / HTTP/1.1\r\nHost: t\r\nExpect: 100-continue\r\n\r\n",
            200,
            HELLO,
            False,
        )

    fails += check("continue-withheld", "100 then 200 echo, body after 100", withhold)
    fails += check("expect-other", "417 close, body not required", bad_expect)
    fails += check("expect-too-big", "413 close, not 100", expect_too_big)
    fails += check("expect-get", "200 keep-alive, no 100", get_expect)
    return fails


def run_pipeline(srv):
    fails = 0

    def two_get():
        c = Conn(srv.port)
        try:
            c.send(b"GET / HTTP/1.1\r\nHost: t\r\n\r\nGET / HTTP/1.1\r\nHost: t\r\n\r\n")
            a = c.read_response()
            b = c.read_response()
            expect_status(a, 200, HELLO, False)
            expect_status(b, 200, HELLO, False)
        finally:
            c.close()

    def post_then_get():
        c = Conn(srv.port)
        try:
            c.send(
                b"POST /echo HTTP/1.1\r\nHost: t\r\nContent-Length: 3\r\n\r\n"
                b"abcGET / HTTP/1.1\r\nHost: t\r\n\r\n"
            )
            a = c.read_response()
            b = c.read_response()
            expect_status(a, 200, b"abc", False)
            expect_status(b, 200, HELLO, False)
        finally:
            c.close()

    def post_slash_then_get():
        c = Conn(srv.port)
        try:
            c.send(
                b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 4\r\n\r\n"
                b"pingGET / HTTP/1.1\r\nHost: t\r\n\r\n"
            )
            a = c.read_response()
            b = c.read_response()
            expect_status(a, 200, HELLO, False)
            expect_status(b, 200, HELLO, False)
        finally:
            c.close()

    def three():
        c = Conn(srv.port)
        try:
            one_req = b"GET / HTTP/1.1\r\nHost: t\r\n\r\n"
            c.send(one_req * 3)
            bodies = []
            for _ in range(3):
                resp = c.read_response()
                expect_status(resp, 200, HELLO, False)
                bodies.append(resp.body)
            if bodies != [HELLO, HELLO, HELLO]:
                raise Fail("order %r" % bodies)
        finally:
            c.close()

    def close_first():
        c = Conn(srv.port)
        try:
            c.send(
                b"GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n"
                b"GET / HTTP/1.1\r\nHost: t\r\n\r\n"
            )
            resp = c.read_response()
            expect_status(resp, 200, HELLO, True)
            c.expect_eof()
        finally:
            c.close()

    fails += check("pipeline-two-get", "200 then 200, bodies in order", two_get)
    fails += check("pipeline-post-get", "200 abc then 200 hello", post_then_get)
    fails += check("pipeline-post-slash-get", "200 hello then 200 hello", post_slash_then_get)
    fails += check("pipeline-three", "three 200s in order", three)
    fails += check("pipeline-close-first", "one 200 then close", close_first)
    return fails


def run_keepalive(srv):
    fails = 0

    def two_on(raw1, raw2, close1):
        c = Conn(srv.port)
        try:
            c.send(raw1)
            a = c.read_response()
            expect_status(a, 200, HELLO, close1)
            if close1:
                c.expect_eof()
                return
            c.send(raw2)
            b = c.read_response()
            expect_status(b, 200, HELLO, False)
        finally:
            c.close()

    def reuse():
        c = Conn(srv.port)
        try:
            c.send(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
            a = c.read_response()
            expect_status(a, 200, HELLO, False)
            time.sleep(0.25)
            c.send(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
            b = c.read_response()
            expect_status(b, 200, HELLO, False)
        finally:
            c.close()

    fails += check(
        "ka-11-default",
        "200 keep-alive, second 200",
        lambda: two_on(
            b"GET / HTTP/1.1\r\nHost: t\r\n\r\n",
            b"GET / HTTP/1.1\r\nHost: t\r\n\r\n",
            False,
        ),
    )
    fails += check(
        "ka-11-close",
        "200 close",
        lambda: two_on(
            b"GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n",
            b"",
            True,
        ),
    )
    fails += check(
        "ka-11-close-token",
        "200 close",
        lambda: two_on(
            b"GET / HTTP/1.1\r\nHost: t\r\nConnection: Close\r\n\r\n",
            b"",
            True,
        ),
    )
    fails += check(
        "ka-10-default",
        "200 close",
        lambda: two_on(b"GET / HTTP/1.0\r\n\r\n", b"", True),
    )
    fails += check(
        "ka-10-keep",
        "200 keep-alive, second 200",
        lambda: two_on(
            b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
            b"GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\n",
            False,
        ),
    )
    fails += check(
        "ka-close-wins",
        "200 close",
        lambda: two_on(
            b"GET / HTTP/1.1\r\nHost: t\r\nConnection: keep-alive\r\nConnection: close\r\n\r\n",
            b"",
            True,
        ),
    )
    fails += check(
        "ka-close-same-field",
        "200 close",
        lambda: two_on(
            b"GET / HTTP/1.1\r\nHost: t\r\nConnection: keep-alive, close\r\n\r\n",
            b"",
            True,
        ),
    )
    fails += check("ka-idle-reuse", "second 200 after a short pause", reuse)
    return fails


def run_chunk_eof(srv):
    def go():
        c = Conn(srv.port)
        try:
            c.send(
                b"POST / HTTP/1.1\r\nHost: t\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhi"
            )
            c.shutdown_write()
            resp = c.read_response(time.monotonic() + 2)
            expect_status(resp, 400, b"malformed", True)
        finally:
            c.close()

    return check("chunk-premature-end", "400 close", go)


def run_slow(srv):
    fails = 0

    def silence(send, name_hint):
        c = Conn(srv.port, timeout=3)
        try:
            t0 = time.monotonic()
            if send:
                c.send(send)
                t0 = time.monotonic()
            deadline = t0 + 2.0
            while time.monotonic() < deadline:
                if c._pull(deadline):
                    break
            else:
                raise Fail("still open")
            if c.buf:
                raise Fail("status bytes %r" % c.buf[:80])
            elapsed = time.monotonic() - t0
            if elapsed < srv.idle_ms / 1000.0 * 0.5:
                raise Fail("closed too fast %.3fs" % elapsed)
        finally:
            c.close()

    fails += check(
        "slow-headers",
        "close, no status, after idle",
        lambda: silence(b"GET / HTTP/1.1\r\nHo", "headers"),
    )
    fails += check(
        "slow-body",
        "close, no status, after idle",
        lambda: silence(
            b"POST / HTTP/1.1\r\nHost: t\r\nContent-Length: 8\r\n\r\nAB",
            "body",
        ),
    )
    fails += check(
        "idle-no-request",
        "close, no status, after idle",
        lambda: silence(b"", "idle"),
    )
    return fails


def run_many(srv):
    def go():
        n = srv.max_conn
        conns = []
        try:
            for i in range(n):
                c = Conn(srv.port, timeout=3)
                conns.append(c)
                c.send(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
            for c in conns:
                resp = c.read_response()
                expect_status(resp, 200, HELLO, False)
            # Idle reuse while the table is full.
            conns[0].send(b"GET / HTTP/1.1\r\nHost: t\r\n\r\n")
            again = conns[0].read_response()
            expect_status(again, 200, HELLO, False)
            # Free one slot. A new connection must be accepted.
            conns[1].close()
            conns[1] = None
            fresh = Conn(srv.port, timeout=3)
            conns.append(fresh)
            fresh.send(b"GET / HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
            resp = fresh.read_response(time.monotonic() + 3)
            expect_status(resp, 200, HELLO, True)
        finally:
            for c in conns:
                if c:
                    c.close()

    return check(
        "many-idle",
        "%d idle keep-alives, reuse, accept after one close" % srv.max_conn,
        go,
    )


def main():
    bin_path = sys.argv[1] if len(sys.argv) > 1 else "c/muro-hello"
    fails = 0
    main_srv = Server(bin_path, idle_ms=4000, max_conn=40)
    try:
        main_srv.start()
    except Exception as e:
        print("FAIL server-start  expected epoll listen: %s" % e)
        main_srv.stop()
        return 1
    try:
        if main_srv.proc.poll() is not None:
            print("FAIL server-start  expected running")
            return 1
        fails += run_exchange(main_srv)
        fails += run_smuggle(main_srv)
        fails += run_continue(main_srv)
        fails += run_pipeline(main_srv)
        fails += run_keepalive(main_srv)
        fails += run_chunk_eof(main_srv)
        fails += run_many(main_srv)
    finally:
        main_srv.stop()

    slow = Server(bin_path, idle_ms=400, max_conn=8)
    try:
        slow.start()
        fails += run_slow(slow)
    except Exception as e:
        print("FAIL slow-server  expected epoll listen: %s" % e)
        fails += 1
    finally:
        slow.stop()

    passed = sum(1 for r in results if r[1] == "pass")
    failed = sum(1 for r in results if r[1] == "fail")
    print("%d passed, %d failed" % (passed, failed))
    return 1 if fails or failed else 0


if __name__ == "__main__":
    sys.exit(main())
