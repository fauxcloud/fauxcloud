"""tools/burst_repro.py -- a reproduction we control of a short heap dip caused by many
page connections at once (a phone resuming an open tab, a laptop re-joining). For each N it opens N TCP connections to
the board at the same moment, then every connection sends its GET request(s) at once -- the dashboard's page-load set
spread over the connections -- and reads the answers. Before the first run and after each one it reads the board's own
figures from /api/live (diag): heap_min_10m, heap_min, heap_free, heap_largest, uptime, restarts, http_shed /
http_purged, and the heap_min8_* context where the firmware has it.

GET only, no login, no cookies, nothing written on the board. A dip that runs the board out of memory can restart it:
use it only on a board that may restart. It stops climbing N at the first restart or when heap_min_10m < --stop-below.

Usage (LAN side, Python 3, stdlib only):
  python -I tools/burst_repro.py <board-address> [--n 4,8,12] [--pipeline 1] [--gap 15] [--stop-below 8192]

  --n          connection counts to try, in order
  --pipeline   requests sent back to back on each connection before reading (1 = like a browser)
  --gap        seconds to wait after a run before reading the board (the window takes the dip within a second)

The station id the page-load paths need is read from the board; nothing printed identifies the board or its user.
"""
import argparse
import json
import socket
import threading
import time
import functools
print = functools.partial(print, flush=True)  # each line as it happens (runs take minutes)

UA = ("Mozilla/5.0 (Linux; Android 14; Pixel 7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Mobile "
      "Safari/537.36")
DIAG_KEYS = ("fw_version", "heap_min_10m", "heap_min", "heap_free", "heap_largest", "http_shed", "http_paced", "http_purged",
             "http_aborted",
             "restarts", "heap_min8_t", "heap_min8_ctx", "heap_min8_socks")


def request_bytes(host, path):
    # what a phone browser sends for a fetch(): about 500 B of headers
    return ("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: keep-alive\r\nUser-Agent: %s\r\nAccept: */*\r\n"
            "Accept-Encoding: gzip, deflate\r\nAccept-Language: en-GB,en;q=0.9,en-US;q=0.8\r\nReferer: http://%s/\r\n"
            "sec-ch-ua: \"Chromium\";v=\"130\", \"Google Chrome\";v=\"130\", \"Not?A_Brand\";v=\"99\"\r\n"
            "sec-ch-ua-mobile: ?1\r\nsec-ch-ua-platform: \"Android\"\r\n\r\n" % (path, host, UA, host)).encode()


class Conn:
    """One socket's HTTP/1.1 responses, read by Content-Length or chunked framing (the board keeps sockets open)."""

    def __init__(self, s):
        self.s, self.buf = s, b""

    def _need(self, pred):
        while not pred():
            chunk = self.s.recv(4096)
            if not chunk:
                raise EOFError("closed by the board")
            self.buf += chunk

    def response(self):
        self._need(lambda: b"\r\n\r\n" in self.buf)
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        lines = head.split(b"\r\n")
        status = int(lines[0].split()[1])
        hdr = {l.split(b":", 1)[0].strip().lower(): l.split(b":", 1)[1].strip() for l in lines[1:] if b":" in l}
        body = b""
        if b"content-length" in hdr:
            n = int(hdr[b"content-length"])
            self._need(lambda: len(self.buf) >= n)
            body, self.buf = self.buf[:n], self.buf[n:]
        elif hdr.get(b"transfer-encoding", b"").lower() == b"chunked":
            while True:
                self._need(lambda: b"\r\n" in self.buf)
                size_line, self.buf = self.buf.split(b"\r\n", 1)
                size = int(size_line.split(b";")[0], 16)
                self._need(lambda: len(self.buf) >= size + 2)
                body, self.buf = body + self.buf[:size], self.buf[size + 2:]
                if size == 0:
                    break
        return status, body


def read_responses(s, want, timeout):
    """Reads `want` responses from a socket; returns their status codes (fewer if the board closed it)."""
    s.settimeout(timeout)
    c, codes = Conn(s), []
    try:
        while len(codes) < want:
            codes.append(c.response()[0])
    except EOFError:
        pass
    return codes


def get_json(host, path, timeout=10):
    s = socket.create_connection((host, 80), timeout=timeout)
    try:
        s.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\n\r\n" % (path, host)).encode())
        _, body = Conn(s).response()
    finally:
        s.close()
    return json.loads(body)


def board(host):
    live = get_json(host, "/api/live")
    d = live.get("diag", {})
    out = {k: d.get(k) for k in DIAG_KEYS}
    out["uptime_s"] = live.get("uptime_s")
    sid = (live.get("stations") or [{}])[0].get("id") if "stations" in live else (live.get("station") or {}).get("id")
    return out, sid


def connect(host, timeout, rcvbuf):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if rcvbuf:  # before connect: a small window, like a phone that is slow to take data in
        s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    s.settimeout(timeout)
    s.connect((socket.gethostbyname(host), 80))
    return s


def run(host, n, pipeline, paths, timeout, rcvbuf=0, slow=0.0):
    socks, errors, codes = [], [], []
    for _ in range(n):  # connect them all first: N sockets open at the same moment
        try:
            socks.append(connect(host, timeout, rcvbuf))
        except OSError as e:
            errors.append("connect: %s" % e)
    go = threading.Barrier(len(socks) + 1) if socks else None
    lock = threading.Lock()

    def worker(i, s):
        reqs = b"".join(request_bytes(host, paths[(i * pipeline + k) % len(paths)]) for k in range(pipeline))
        try:
            go.wait()
            s.sendall(reqs)
            if slow:
                time.sleep(slow)  # the answers wait on the board's side while this client does not read
            got = read_responses(s, pipeline, timeout)
            with lock:
                codes.extend(got)
                if len(got) < pipeline:
                    errors.append("closed after %d of %d" % (len(got), pipeline))
        except OSError as e:
            with lock:
                errors.append(str(e))
        finally:
            s.close()

    threads = [threading.Thread(target=worker, args=(i, s)) for i, s in enumerate(socks)]
    for t in threads:
        t.start()
    t0 = time.monotonic()
    if go:
        go.wait()
    for t in threads:
        t.join()
    return time.monotonic() - t0, codes, errors


def main():
    ap = argparse.ArgumentParser(description="fauxcloud page-connection burst reproduction (GET only)")
    ap.add_argument("host")
    ap.add_argument("--n", default="4,8,12")
    ap.add_argument("--pipeline", type=int, default=1)
    ap.add_argument("--gap", type=float, default=15.0)
    ap.add_argument("--stop-below", type=int, default=8192)
    ap.add_argument("--timeout", type=float, default=20.0)
    ap.add_argument("--rcvbuf", type=int, default=0, help="client receive buffer in bytes (0 = the OS default)")
    ap.add_argument("--slow", type=float, default=0.0, help="seconds each connection waits before reading its answers")
    ap.add_argument("--settle-above", type=int, default=0,
                    help="before each run, wait (<= 13 min) until heap_min_10m is at least this (0 = do not wait)")
    a = ap.parse_args()

    base, sid = board(a.host)
    print("baseline:", json.dumps(base))
    q = "?station=" + sid if sid else ""
    paths = ["/api/live" + q, "/api/bridge/status", "/api/bridge", "/api/hourly" + q, "/api/recent" + q]
    up0, restarts0 = base.get("uptime_s") or 0, base.get("restarts")
    for n in [int(x) for x in a.n.split(",") if x.strip()]:
        # heap_min_10m is a 10-minute window: wait until the last dip has left it, so each run reads its own
        t_end = time.monotonic() + 13 * 60
        while a.settle_above and time.monotonic() < t_end:
            cur, _ = board(a.host)
            if (cur.get("heap_min_10m") or 0) >= a.settle_above:
                break
            time.sleep(30)
        took, codes, errors = run(a.host, n, a.pipeline, paths, a.timeout, a.rcvbuf, a.slow)
        summary = {c: codes.count(c) for c in sorted(set(codes))}
        print("N=%d pipeline=%d rcvbuf=%d slow=%.1f: %.1f s, answers %s, errors %d%s" % (
            n, a.pipeline, a.rcvbuf, a.slow, took, summary, len(errors),
                                                                   (" (" + "; ".join(errors[:3]) + ")") if errors else ""))
        time.sleep(a.gap)
        try:
            now, _ = board(a.host)
        except (OSError, ValueError) as e:
            print("  board did not answer: %s -- stop" % e)
            return
        print("  board:", json.dumps(now))
        if (now.get("uptime_s") or 0) < up0 or now.get("restarts") != restarts0:
            print("  the board restarted -- stop")
            return
        up0 = now.get("uptime_s") or 0
        if (now.get("heap_min_10m") or 1 << 30) < a.stop_below:
            print("  heap_min_10m below %d -- stop" % a.stop_below)
            return


if __name__ == "__main__":
    main()
