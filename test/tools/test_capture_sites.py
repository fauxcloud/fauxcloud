"""The quiet gates of "Listen to a new console" stay a reviewed list. Every call of otaActive(), captureQuiet() and
captureMqttOff() in firmware/fauxcloud/*.cpp / *.ino (comments ignored) is counted per file; the counts must equal the
table below, one decision per site: where the capture is quiet like an OTA, where it has its own flag, and where only
the OTA gates (the LAN face keeps working while the bridge listens). A new site fails here until a reviewer decides it
and updates the table.
Run: python -I -m unittest discover -s test/tools"""
import glob
import os
import re
import unittest

from test_heap_calls import strip_comments

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.join(HERE, "..", "..", "firmware", "fauxcloud")
CALL = re.compile(r"\b(otaActive|captureQuiet|captureMqttOff)\(\)")

# file -> {gate: calls}. otaActive alone: the OTA's own gates (sessions closed after their answer, the page and history
# refusals, the store and learner ticks, NetWd, the OTA heap feed, the bridge status's `ota`, the start press's check).
# captureQuiet: the upload skip, MQTT publish / announce / the two start gates / the diag flag, FragWatch, the heap
# context, the LED (attention and pattern), the busy table and the abort reason, the OTA route's 409, the live ETag,
# the admin's MQTT test (no client at all while listening).
EXPECTED = {
    "app_admin.cpp": {"captureQuiet": 1},
    "app_capture.cpp": {"otaActive": 2, "captureQuiet": 1, "captureMqttOff": 1},
    "app_core.cpp": {"otaActive": 4, "captureQuiet": 4},
    "app_fwd.cpp": {"otaActive": 2, "captureQuiet": 2},
    "app_http.cpp": {"otaActive": 4, "captureQuiet": 3},
    "app_learn.cpp": {"otaActive": 1},
    "app_mqtt.cpp": {"otaActive": 7, "captureQuiet": 6, "captureMqttOff": 2},
    "app_ota.cpp": {"otaActive": 1},
    "app_store.cpp": {"otaActive": 1},
    "app_sys.cpp": {"otaActive": 1},
}


def count(src):
    out = {}
    for m in CALL.finditer(strip_comments(src)):
        out[m.group(1)] = out.get(m.group(1), 0) + 1
    return out


class CaptureSites(unittest.TestCase):
    def test_sites_match_the_table(self):
        got = {}
        for f in sorted(glob.glob(os.path.join(SKETCH, "*.cpp")) + glob.glob(os.path.join(SKETCH, "*.ino"))):
            with open(f, encoding="utf-8") as fh:
                c = count(fh.read())
            if c:
                got[os.path.basename(f)] = c
        self.assertEqual(got, EXPECTED, "a quiet-gate call site was added or removed: decide it and update "
                                        "EXPECTED")

    def test_a_start_press_resets_the_last_runs_counters(self):
        # a GET while "starting" must not carry the last run's seq / lost (the page would wait for them): the press
        # clears them, not the allocation seconds later
        with open(os.path.join(SKETCH, "app_capture.cpp"), encoding="utf-8") as fh:
            src = strip_comments(fh.read())
        body = src[src.index("static esp_err_t onCaptureStart("):]
        body = body[:body.index(chr(10) + "}" + chr(10))]
        ok = body.index("if (p == CapPress::Ok)")
        self.assertLess(ok, body.index("memset(&gC, 0, sizeof gC)"))
        self.assertLess(body.index("memset(&gC, 0, sizeof gC)"), body.index("if (p != CapPress::Ok)"))
        self.assertIn("gSkipped = 0", body)

    def test_listeners_bind_with_reuse(self):
        # a Start within 120 s of a Stop meets the last peeks' TIME_WAIT on the same ports: every listener binds with
        # SOF_REUSEADDR set first (bench 2026-11-21)
        with open(os.path.join(SKETCH, "app_capture.cpp"), encoding="utf-8") as fh:
            src = strip_comments(fh.read())
        body = src[src.index("static int openListeners("):]
        body = body[:body.index(chr(10) + "}" + chr(10))]
        self.assertLess(body.index("ip_set_option(p, SOF_REUSEADDR)"), body.index("tcp_bind("))

    def test_the_gate_bites(self):
        src = "void f() { if (otaActive()) return; // captureQuiet() in a comment\n /* otaActive() */ }"
        self.assertEqual(count(src), {"otaActive": 1})
        self.assertNotEqual(count(src + "\nvoid g() { if (captureQuiet()) return; }"), {"otaActive": 1})


if __name__ == "__main__":
    unittest.main()
