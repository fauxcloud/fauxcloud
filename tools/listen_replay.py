"""listen_replay.py -- the bench script of "Listen to a new console": from a laptop joined to the bridge's own
Wi-Fi (fauxcloud-XXXX, the AP side), it sends what a strange console might send, one scenario after another, and checks
the answers the bridge gives (nothing may change: `success` on port 80, a closed connection on the other ports). The
admin page is on the home network, so the report is read there: start listening on the admin page first, run this
script, then Copy (or Download) the report and pass it to --compare, which checks every expected line of
tools/listen_replay_golden.txt ({c} = this laptop's last address byte on the bridge's Wi-Fi, * = anything).

Usage (stdlib only; no secret, no session needed):
    python -I tools/listen_replay.py                         # every scenario against 192.168.77.1
    python -I tools/listen_replay.py --only wu,tls           # a few
    python -I tools/listen_replay.py --list                  # the scenarios and what each expects
    python -I tools/listen_replay.py --compare report.txt    # after the run: the report against the golden lines
"""
import argparse
import os
import random
import re
import socket
import ssl
import struct
import sys
import time

AP = "192.168.77.1"
HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(HERE, "listen_replay_golden.txt")
WU_PATH = "/weatherstation/updateweatherstation.php"
WU_QUERY = ("ID=CAPT1&PASSWORD=x&dateutc=now&tempf=51.2&humidity=60&dewptf=38.0&baromin=29.92&rainin=0.00"
            "&dailyrainin=0.12&winddir=180&windspeedmph=3.0&windgustmph=5.0&UV=1&solarRadiation=12.5")


def http(path, method="GET", host="rtupdate.wunderground.com", body=b"", ctype=None, close_early=False, port=80):
    """One request on its own connection; the answer's body (or None when the script closed first)."""
    head = "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: listen_replay\r\n" % (method, path, host)
    if body:
        head += "Content-Type: %s\r\nContent-Length: %d\r\n" % (ctype, len(body))
    head += "Connection: close\r\n\r\n"
    with socket.create_connection((AP, port), timeout=5) as s:
        s.sendall(head.encode() + body)
        if close_early:  # write the request, then the FIN at once: never read the answer
            s.shutdown(socket.SHUT_WR)
            return None
        data = b""
        while True:
            chunk = s.recv(1024)
            if not chunk:
                break
            data += chunk
    return data.split(b"\r\n\r\n", 1)[-1].decode("latin-1")


def scen_dns():
    q = struct.pack(">HHHHHH", random.randint(1, 65535), 0x0100, 1, 0, 0, 0)
    for label in "console.example.net".split("."):
        q += bytes([len(label)]) + label.encode()
    q += b"\x00" + struct.pack(">HH", 1, 1)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(2)
        s.sendto(q, (AP, 53))
        a, _ = s.recvfrom(512)
    ok = a[-4:] == socket.inet_aton(AP)
    return ok, "answered %s" % socket.inet_ntoa(a[-4:])


def scen_ntp():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(2)
        s.sendto(b"\x23" + b"\x00" * 47, (AP, 123))
        try:
            s.recvfrom(64)
            return True, "answered (the bridge has the time)"
        except socket.timeout:
            return True, "not answered (the bridge has no real time; the report's header says ntp:0/1)"


def scen_wu():
    b = http(WU_PATH + "?" + WU_QUERY)
    return b == "success", "answer %r" % b


def scen_wu_close():
    http(WU_PATH + "?" + WU_QUERY, close_early=True)
    return True, "sent and closed without reading (the report should say closed-before-reply: bench verdict)"


def scen_post():
    b = http("/api/v1/up", "POST", body=b'{"token":"t","t":1}', ctype="application/json")
    return b == "success", "answer %r" % b


def scen_post_wu():
    b = http(WU_PATH, "POST", body=b"ID=CAPT1&temp=1", ctype="application/x-www-form-urlencoded")
    return b == "success", "answer %r" % b


PEEK_BODY = b"PASSKEY=0123456789abcdef&tempf=51.2&name=Z\xc3\xbcrich"  # a key to hide, UTF-8 that stays text


def scen_peek_http():
    b = http("/data/report/?PASSKEY=0123456789abcdef&tempf=51.2", "POST", host=AP + ":8080", body=PEEK_BODY,
             ctype="application/x-www-form-urlencoded", port=8080)
    return b == "", "closed without an answer" if b == "" else "answer %r" % b


def scen_probe():
    with socket.create_connection((AP, 80), timeout=5) as s:
        s.sendall(b"GET /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\nUser-Agent: listen_replay\r\n"
                  b"Connection: close\r\n\r\n")
        status = s.recv(64).split(b"\r\n")[0].decode()
    return "204" in status, status


def scen_tls():
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    with socket.create_connection((AP, 443), timeout=5) as raw:
        try:
            with ctx.wrap_socket(raw, server_hostname="api.example.net") as s:
                s.do_handshake()
            return False, "a TLS handshake completed: nothing may answer TLS"
        except (ssl.SSLError, OSError) as e:
            return True, "closed without an answer (%s)" % type(e).__name__


def scen_mqtt():
    cid = b"abcdefghijkl"
    var = b"\x00\x04MQTT\x04\xc2\x00\x3c" + struct.pack(">H", len(cid)) + cid + b"\x00\x04user\x00\x04pass"
    pkt = b"\x10" + bytes([len(var)]) + var
    with socket.create_connection((AP, 1883), timeout=5) as s:
        s.sendall(pkt)
        s.settimeout(2)
        try:
            got = s.recv(16)
        except socket.timeout:
            got = b"(timeout)"
    return got in (b"", b"(timeout)"), "no CONNACK: %r" % got


def scen_bare():
    with socket.create_connection((AP, 8080), timeout=5) as s:
        time.sleep(0.6)
        s.settimeout(1)
        try:
            got = s.recv(16)
        except socket.timeout:
            got = b"(open)"
    return got == b"", "the bridge closed it: %r" % got


def scen_raw():
    with socket.create_connection((AP, 5000), timeout=5) as s:
        s.sendall(bytes(range(0x80, 0x94)))
        s.settimeout(2)
        try:
            got = s.recv(16)
        except socket.timeout:
            got = b"(timeout)"
    return got in (b"", b"(timeout)"), "nothing answered"


SCENARIOS = [
    ("dns", scen_dns, "a DNS A query for console.example.net: the bridge's address comes back"),
    ("ntp", scen_ntp, "an NTP request: answered only with a real clock (counted either way)"),
    ("wu", scen_wu, "the WU upload with 14 fields: `success`, the query whole in the report (two records joined)"),
    ("wu_close", scen_wu_close, "the same upload, closed at once without reading the answer"),
    ("post", scen_post, "POST /api/v1/up with JSON (an unknown path, 404 inside): the token hidden"),
    ("post_wu", scen_post_wu, "POST to the WU path (405 inside): a form body, the ID hidden"),
    ("probe", scen_probe, "a phone's connectivity probe: 204 as always"),
    ("tls", scen_tls, "a TLS ClientHello with SNI api.example.net to 443: closed, the hello's facts recorded"),
    ("mqtt", scen_mqtt, "an MQTT CONNECT with user and password to 1883: a summary, never the credentials"),
    ("peek_http", scen_peek_http, "a form POST to 8080 (a peek port): the request line, headers and body in the report, "
                                  "the key hidden in the query and the body, UTF-8 shown as ?"),
    ("bare", scen_bare, "a bare connection to 8080 that sends nothing"),
    ("raw", scen_raw, "20 unknown bytes to 5000: 16 as hex"),
]


def my_octet():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.connect((AP, 53))
        return s.getsockname()[0].split(".")[-1]


def golden_lines(octet):
    out = []
    with open(GOLDEN, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            rx = re.escape(line.replace("{c}", octet)).replace(r"\*", ".*")
            out.append((line, re.compile(rx)))
    return out


def compare(path, octet):
    with open(path, encoding="utf-8") as f:
        report = f.read()
    lines = report.splitlines()
    missing = [g for g, rx in golden_lines(octet) if not any(rx.search(l) for l in lines)]
    bad = [l for l in lines if re.search(r"(PASSWORD|ID|token|passkey|key)=(?!\*\*\*)[^&\s]", l) or "abcdefghijkl" in l or "0123456789abcdef" in l]
    for g in missing:
        print("MISSING  " + g)
    for b in bad:
        print("LEAK?    " + b)
    print("%s: %d expected lines, %d missing, %d possible leaks" % ("OK" if not missing and not bad else "FAILED",
                                                                    len(golden_lines(octet)), len(missing), len(bad)))
    return 0 if not missing and not bad else 1


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--only", default="", help="comma-separated scenario names")
    ap.add_argument("--list", action="store_true", help="list the scenarios")
    ap.add_argument("--compare", metavar="REPORT", help="check a copied report against the golden lines")
    ap.add_argument("--octet", help="this laptop's last address byte (default: read from the route to the bridge)")
    a = ap.parse_args(argv)
    if a.list:
        for n, _, what in SCENARIOS:
            print("%-9s %s" % (n, what))
        return 0
    octet = a.octet or my_octet()
    if a.compare:
        return compare(a.compare, octet)
    want = set(filter(None, a.only.split(",")))
    fails = 0
    for n, fn, what in SCENARIOS:
        if want and n not in want:
            continue
        for attempt in range(3):
            try:
                ok, note = fn()
                break
            except (ConnectionResetError, ConnectionRefusedError) as e:
                # the bridge reads one extra-port connection at a time: one held by this laptop's own traffic (a
                # Windows or browser HTTPS call to 443) turns ours away as "busy, closed"; try again shortly
                ok, note = False, "%s: %s (after %d tries)" % (type(e).__name__, e, attempt + 1)
                time.sleep(1.5)
            except Exception as e:  # a scenario that cannot reach the bridge says so and the rest go on
                ok, note = False, "%s: %s" % (type(e).__name__, e)
                break
        fails += not ok
        print("%-4s %-9s %s" % ("ok" if ok else "FAIL", n, note))
        time.sleep(1.5)  # one peek at a time on the bridge, and time for the admin page to fetch the records
    print("\nThis laptop is .%s on the bridge's Wi-Fi. Copy the report from the admin page into a file, then:" % octet)
    print("  python -I tools/listen_replay.py --compare <file> --octet %s" % octet)
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
