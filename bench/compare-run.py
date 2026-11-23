#!/usr/bin/env python3
"""Compare muro-http, nginx, h2o, and Go net/http with bench/loadgen.

Warmup is --duration 2, discarded. Three measured runs use --duration 5.
Median is the middle req/s. p50, p99, errors, and non2xx on that row
are from the same run. Servers are pinned with taskset: 1 worker on
CPU 0, 4 workers on CPUs 0-3, loadgen on CPUs 4-7.
"""

import json
import os
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RAW = os.path.join(ROOT, "bench", "compare-2026-11-23-raw")
LOADGEN = os.path.join(ROOT, "bench", "loadgen")
NGINX = os.path.expanduser("~/opt/nginx/sbin/nginx")
GO = os.path.expanduser("~/opt/go/bin/go")
GO_BIN = os.path.join(ROOT, "bench", "go-hello", "go-hello")
H2O_BIN = os.path.join(ROOT, "bench", "h2o-hello", "h2o-hello")
POST_BODY = b"0123456789abcdef0123456789abcdef"

PORTS = {"muro": 18180, "nginx": 18181, "h2o": 18182, "go": 18183}
SERVERS = ["muro", "nginx", "h2o", "go"]
WORKERS = [1, 4]
MODES = ["keepalive", "close"]
METHODS = ["GET", "POST"]
CONCS = [32, 128, 512]


def cpus_for(workers):
    return "0" if workers == 1 else "0-3"


def loadavg():
    with open("/proc/loadavg") as f:
        return f.read().strip()


def port_open(port):
    s = socket.socket()
    s.settimeout(0.2)
    try:
        r = s.connect_ex(("127.0.0.1", port))
        return r == 0
    finally:
        s.close()


def stop(proc):
    if proc is None or proc.poll() is not None:
        return
    try:
        os.killpg(proc.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        proc.wait(timeout=3)


def wait_listen(proc, port, timeout=8):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if proc.poll() is not None:
            raise RuntimeError("server exited before listen, code %s" % proc.returncode)
        if port_open(port):
            return
        time.sleep(0.05)
    raise RuntimeError("port %d did not open" % port)


def http_check(port, method):
    url = "http://127.0.0.1:%d/" % port
    data = POST_BODY if method == "POST" else None
    req = urllib.request.Request(url, data=data, method=method)
    try:
        with urllib.request.urlopen(req, timeout=2) as resp:
            body = resp.read()
            ctype = resp.headers.get("Content-Type", "")
            code = resp.status
    except urllib.error.HTTPError as e:
        body = e.read()
        ctype = e.headers.get("Content-Type", "")
        code = e.code
    return code, ctype, body


def preflight(name, port, logf):
    for method in ("GET", "POST"):
        code, ctype, body = http_check(port, method)
        line = "preflight %s %s %d %r %r\n" % (name, method, code, ctype, body)
        logf.write(line)
        logf.flush()
        if code != 200 or body != b"hello\n":
            raise RuntimeError(line.strip())
        if "text/plain" not in ctype.lower():
            raise RuntimeError("content-type %s" % ctype)


def nginx_conf(workers, conf_path, pid_path):
    text = open(os.path.join(ROOT, "bench", "nginx", "hello.conf.template")).read()
    text = text.replace("WORKERS", str(workers)).replace("PID_PATH", pid_path)
    with open(conf_path, "w") as f:
        f.write(text)


def start_server(name, workers, logf):
    port = PORTS[name]
    if port_open(port):
        raise RuntimeError("port %d already in use" % port)
    cpus = cpus_for(workers)
    env = os.environ.copy()
    if name == "muro":
        cmd = [
            "taskset", "-c", cpus,
            os.path.join(ROOT, "c", "muro-hello"),
            "--bind", "127.0.0.1",
            "--port", str(port),
            "--max-connections", "1024",
            "--idle-ms", "60000",
            "--workers", str(workers),
            "--wait", "epoll",
        ]
    elif name == "nginx":
        conf = os.path.join(RAW, "nginx-w%d.conf" % workers)
        pid = os.path.join(RAW, "nginx-w%d.pid" % workers)
        nginx_conf(workers, conf, pid)
        # Prefix stays out of the repo. Generated conf and pid stay in RAW.
        prefix = os.path.join("/tmp", "muro-http-bench-nginx-w%d" % workers)
        os.makedirs(os.path.join(prefix, "logs"), exist_ok=True)
        os.makedirs(os.path.join(prefix, "temp"), exist_ok=True)
        cmd = ["taskset", "-c", cpus, NGINX, "-c", conf, "-p", prefix]
    elif name == "h2o":
        cmd = [
            "taskset", "-c", cpus, H2O_BIN,
            "--port", str(port),
            "--threads", str(workers),
        ]
    elif name == "go":
        env["GOMAXPROCS"] = str(workers)
        env["PORT"] = str(port)
        cmd = ["taskset", "-c", cpus, GO_BIN]
    else:
        raise RuntimeError(name)
    logf.write("start %s\n" % " ".join(cmd))
    if name == "go":
        logf.write("GOMAXPROCS=%d\n" % workers)
    logf.flush()
    proc = subprocess.Popen(
        cmd,
        stdout=logf,
        stderr=subprocess.STDOUT,
        env=env,
        cwd=ROOT,
        start_new_session=True,
    )
    try:
        wait_listen(proc, port)
        time.sleep(0.1)
        preflight(name, port, logf)
    except Exception:
        stop(proc)
        raise
    return proc


def parse_loadgen(text):
    out = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] in {
            "requests", "errors", "err_connect", "err_write", "err_read",
            "err_parse", "non2xx", "overflow", "elapsed_ms", "rps",
            "p50_us", "p99_us",
        }:
            out[parts[0]] = parts[1]
    if "rps" not in out:
        raise RuntimeError("loadgen output missing rps:\n" + text)
    return out


def run_loadgen(port, mode, method, conc, duration, dest):
    cmd = [
        "taskset", "-c", "4-7", LOADGEN,
        "--host", "127.0.0.1",
        "--port", str(port),
        "--concurrency", str(conc),
        "--duration", str(duration),
        "--mode", mode,
        "--method", method,
    ]
    with open(dest, "w") as f:
        f.write("cmd %s\n" % " ".join(cmd))
        f.flush()
        proc = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=duration + 20,
            cwd=ROOT,
        )
        text = proc.stdout.decode()
        f.write(text)
        if proc.returncode not in (0, 1):
            raise RuntimeError("loadgen exit %d\n%s" % (proc.returncode, text))
    parsed = parse_loadgen(text)
    parsed["exit"] = proc.returncode
    return parsed


def one_cell(name, workers, mode, method, conc):
    port = PORTS[name]
    tag = "%s-w%d-%s-%s-c%d" % (name, workers, mode, method, conc)
    warm = os.path.join(RAW, tag + "-warmup.txt")
    run_loadgen(port, mode, method, conc, 2, warm)
    runs = []
    for i in (1, 2, 3):
        dest = os.path.join(RAW, "%s-run%d.txt" % (tag, i))
        parsed = run_loadgen(port, mode, method, conc, 5, dest)
        parsed["run"] = i
        runs.append(parsed)
        print("%s run%d rps=%s p50=%s p99=%s err=%s non2xx=%s" % (
            tag, i, parsed["rps"], parsed["p50_us"], parsed["p99_us"],
            parsed["errors"], parsed["non2xx"]), flush=True)
    ordered = sorted(runs, key=lambda r: float(r["rps"]))
    median = ordered[1]
    rec = {
        "server": name,
        "workers": workers,
        "mode": mode,
        "method": method,
        "c": conc,
        "runs": runs,
        "median_run": median["run"],
        "loadavg_after": loadavg(),
    }
    return rec


def cell_key(rec):
    return (rec["server"], rec["workers"], rec["mode"], rec["method"], rec["c"])


def load_done(path):
    done = {}
    if not os.path.exists(path):
        return done
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            done[cell_key(rec)] = rec
    return done


def main():
    os.makedirs(RAW, exist_ok=True)
    meta = {
        "start": time.strftime("%Y-%m-%d %H:%M:%S %Z"),
        "loadavg": loadavg(),
        "uname": list(os.uname()),
    }
    with open("/proc/meminfo") as f:
        meta["meminfo_head"] = "".join(f.readlines()[:8])
    with open(os.path.join(RAW, "start.json"), "w") as f:
        json.dump(meta, f, indent=2)
        f.write("\n")
    out_path = os.path.join(RAW, "results.jsonl")
    done = load_done(out_path)
    results = list(done.values())
    for name in SERVERS:
        for workers in WORKERS:
            pending = []
            for mode in MODES:
                for method in METHODS:
                    for conc in CONCS:
                        key = (name, workers, mode, method, conc)
                        if key not in done:
                            pending.append(key)
            if not pending:
                print("=== %s workers %d already done ===" % (name, workers), flush=True)
                continue
            print("=== %s workers %d pending %d load %s ===" % (
                name, workers, len(pending), loadavg()), flush=True)
            log_path = os.path.join(RAW, "%s-w%d.server.log" % (name, workers))
            with open(log_path, "a") as logf:
                logf.write("loadavg %s\n" % loadavg())
                proc = start_server(name, workers, logf)
                logf.flush()
                try:
                    for mode in MODES:
                        for method in METHODS:
                            for conc in CONCS:
                                key = (name, workers, mode, method, conc)
                                if key in done:
                                    print("skip %s" % (key,), flush=True)
                                    continue
                                rec = one_cell(name, workers, mode, method, conc)
                                results.append(rec)
                                done[key] = rec
                                with open(out_path, "a") as rf:
                                    rf.write(json.dumps(rec) + "\n")
                                    rf.flush()
                finally:
                    stop(proc)
                    logf.write("stopped loadavg %s\n" % loadavg())
            time.sleep(2)
            if port_open(PORTS[name]):
                raise RuntimeError("%s port %d still open" % (name, PORTS[name]))
    print("DONE cells %d" % len(done), flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as e:
        print("FAILED %s" % e, file=sys.stderr, flush=True)
        sys.exit(1)
