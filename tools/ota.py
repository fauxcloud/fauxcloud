"""tools/ota.py -- a plain OTA through the admin API (docs/api.md §3.4), then wait for the new image to answer.

The admin password comes only from the environment variable FC_ADMIN_PW (never a file, never an argument), and the
login is the page's own challenge/proof (PBKDF2 + HMAC), so the password never crosses the network.

    PowerShell:  $env:FC_ADMIN_PW = '<password>';  python -I tools\\ota.py <bridge-ip> firmware\\fauxcloud\\build\\default\\fauxcloud.ino.bin
    sh:          FC_ADMIN_PW='<password>' python3 -I tools/ota.py <bridge-ip> firmware/fauxcloud/build/default/fauxcloud.ino.bin
"""
import hashlib
import hmac
import http.client
import json
import os
import sys
import time


def req(host, method, path, headers=None, body=None, timeout=15):
    c = http.client.HTTPConnection(host, 80, timeout=timeout)
    c.request(method, path, body=body, headers=headers or {})
    r = c.getresponse()
    d = r.read()
    hd = {k.lower(): v for k, v in r.getheaders()}
    c.close()
    return r.status, hd, d


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    host, path = sys.argv[1:3]
    pw = os.environ.get("FC_ADMIN_PW")
    if not pw:
        sys.exit("set FC_ADMIN_PW first (see the usage at the top of this file)")
    h = {"X-Fauxcloud": "1"}
    ch = json.loads(req(host, "GET", "/api/admin/challenge", h)[2])
    key = hashlib.pbkdf2_hmac("sha256", pw.encode(), bytes.fromhex(ch["salt"]), ch["iters"])
    proof = hmac.new(key, ch["nonce"].encode(), hashlib.sha256).hexdigest()
    s, hd, _ = req(host, "POST", "/api/admin/login", dict(h, **{"Content-Type": "application/json"}),
                   json.dumps({"nonce": ch["nonce"], "proof": proof}))
    if "set-cookie" not in hd:
        sys.exit("login failed: HTTP %d" % s)
    cookie = hd["set-cookie"].split(";")[0]
    with open(path, "rb") as f:
        data = f.read()
    t0 = time.time()
    c = http.client.HTTPConnection(host, 80, timeout=120)
    c.putrequest("POST", "/api/admin/ota")
    for k, v in {"X-Fauxcloud": "1", "Cookie": cookie, "Content-Type": "application/octet-stream",
                 "Content-Length": str(len(data))}.items():
        c.putheader(k, v)
    c.endheaders()
    for i in range(0, len(data), 8192):
        c.send(data[i:i + 8192])
    r = c.getresponse()
    print("OTA reply %d in %.1f s: %s" % (r.status, time.time() - t0, r.read().decode()[:200]))
    time.sleep(5)
    for _ in range(90):
        try:
            d = json.loads(req(host, "GET", "/api/live", timeout=4)[2])
            g = d["diag"]
            print("up %d s: %s, ota_state %s, last_reset %s" % (d["uptime_s"], g.get("fw_version"), g.get("ota_state"),
                                                              g.get("last_reset")))
            return
        except Exception:
            time.sleep(1)
    sys.exit("the bridge did not answer within 90 s")


if __name__ == "__main__":
    main()
