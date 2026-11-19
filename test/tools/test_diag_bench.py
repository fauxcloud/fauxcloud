"""The CI checker of the bench diagnostics (tools/check_diag_bench.py) on fake inputs, and the
static half of the log places (the rate-limited AP / SETUP / FILT lines, the head's Log::Boot sites).
Run: python -I -m unittest discover -s test/tools"""
import glob
import os
import re
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(ROOT, "tools"))
import check_diag_bench as cdb  # noqa: E402

SKETCH = os.path.join(ROOT, "firmware", "fauxcloud")


def read(name):
    return open(os.path.join(SKETCH, name), encoding="utf-8").read()


class Checker(unittest.TestCase):
    def test_symbols(self):
        nm = ["3ffc0000 b app::gBusyWin", "400d0000 t app::taskSnapshot()", "3ffc1000 b gTickPartMax",
              "         U uxTaskGetSystemState", "3ffc2000 b app::gBusyWindow", "3ffc3000 d app::gHeapWin"]
        self.assertEqual(cdb.symbol_hits(nm), {"gBusyWin", "taskSnapshot", "gTickPartMax"})  # not the U line, not gBusyWindow

    def test_keys_and_refs(self):
        self.assertEqual(cdb.key_hits(b'..."tick_parts":{"slow"...,"tasks":{}'), {'"tick_parts"', '"tasks":{'})
        self.assertEqual(cdb.key_hits(b'"tasks_done":1,"heap":{"tick_us_max":3}'), set())  # the release's keep keys
        self.assertEqual(cdb.obj_hits(["         U uxTaskGetSystemState", "         U malloc"]), {"uxTaskGetSystemState"})

    def test_a_missing_file_never_passes(self):
        with tempfile.TemporaryDirectory() as d:
            b = os.path.join(d, "x.bin")
            with self.assertRaises(SystemExit):
                cdb.main(["--nm", "nm", "--elf", os.path.join(d, "none.elf"), "--bin", b, "--expect", "absent"])


class LogPlaces(unittest.TestCase):
    def test_rate_limited_sites(self):
        # the AP association / leave / lease lines, the setup page and probe lines, the FILT lines
        net, http, mqtt = read("app_net.cpp"), read("app_http.cpp"), read("app_mqtt.cpp")
        # AP lines through the per-client ApLine (the lease rides with "joined"; a flap's held changes end in the
        # heartbeat's settle line), one limiter each for the setup page and the probe, one FILT limiter per kind
        self.assertEqual(len(re.findall(r'if \(apEvent\((true|false), m, &rn, &rm\) != ApLine::None\)\s*\n\s*logf\("AP"', net)), 2)
        self.assertRegex(net, r'const bool lease = gApLine\.lease\(\);[^\n]*\n[^\n]*\n\s*if \(lease\) logf\("AP", "DHCP lease given')
        self.assertRegex(net, r'gApLine\.settle\(millis\(\), &n, &join, m\);[^\n]*\n[^\n]*\n\s*if \(due\)\s*\n\s*logf\("AP", "client now')
        self.assertIn("apLineSettle();", mqtt)
        self.assertRegex(http, r'if \(gSetupPageLine\.hit\(millis\(\), &rn, &rm\)\)[^\n]*\n\s*logf\("SETUP", "page')
        self.assertRegex(http, r'if \(gSetupProbeLine\.hit\(millis\(\), &rn, &rm\)\)[^\n]*\n\s*logf\("SETUP", "probe')
        self.assertRegex(mqtt, r'filtLine\[res\.dropped \? 0 : 1\]\.hit\(millis\(\), &rn, &rm\)\)\s*\n\s*logf\("FILT"')

    def test_head_sites(self):
        # the head is the explicit boot facts -- at most 20 Log::Boot sites (+ the crash line when present). The count
        # per file is exact, so a new Boot site is a decision, not a drift; it counts every "Log::Boot" token, comments
        # included. app_core.cpp: logl()'s lvl == Log::Boot + the HEAP slots + the CFG summary; app_net.cpp: STA (first
        # join only), MDNS, UDP; app_mqtt.cpp: MQTT connected and bridge online (each the first only), the boot's BEAT;
        # app_sys.cpp: image, crash line, TIME; fauxcloud.ino: the comment above the banner, the banner, the mode line with
        # PBKDF2; app_fwd.cpp: targets, METAR, net-out, an adopt journal given up; app_store.cpp: mount, restore; app_learn.cpp: load; app_http.cpp:
        # the first upload
        expect = {"app_core.cpp": 3, "app_fwd.cpp": 4, "app_http.cpp": 1, "app_learn.cpp": 1, "app_mqtt.cpp": 3,
                  "app_net.cpp": 3, "app_store.cpp": 2, "app_sys.cpp": 3, "fauxcloud.ino": 3}
        got = {}
        for f in glob.glob(os.path.join(SKETCH, "*.cpp")) + glob.glob(os.path.join(SKETCH, "*.ino")):
            n = len(re.findall(r"Log::Boot\b", open(f, encoding="utf-8").read()))
            if n:
                got[os.path.basename(f)] = n
        self.assertEqual(got, expect)

    def test_verbose_routine_lines(self):
        self.assertIn('logl(Log::Chatter, "HIST", "hour closed for %s', read("app_mqtt.cpp"))
        store = read("app_store.cpp")
        self.assertIn('logl(Log::Chatter, "STORE", "%s: hourly ring rotated"', store)
        self.assertIn('logl(Log::Chatter, "STORE", "%s: day %lu written"', store)

    def test_debug_hang_is_test_only(self):
        sys_ = read("app_sys.cpp")
        body = sys_[sys_.index("void sysLoop() {"):]
        body = body[:body.index("\n}\n")]
        self.assertLess(body.index("#ifdef FAUXCLOUD_TEST"), body.index("debug hang"))


class ApiSync(unittest.TestCase):
    def release_keys(self):
        """The top-level keys diag() sends in the release: its format strings outside the FC_DIAG_BENCH blocks."""
        src = open(os.path.join(SKETCH, "src", "web_render.cpp"), encoding="utf-8").read()
        a = src.index("static void diag(Out& o, const Diag& d) {")
        body, skip, keys = src[a:src.index("\n}\n", a)], False, []
        for line in body.split("\n"):
            t = line.strip()
            if t.startswith("#if FC_DIAG_BENCH"):
                skip = True
                continue
            if skip and (t.startswith("#endif") or t.startswith("#else")):
                skip = False
                continue
            if not skip:
                keys += re.findall(r'\\"([a-z0-9_]+)\\":', line)
        nested = {"diag", "active", "until_s", "failures", "reason", "task", "pc"}
        return [k for i, k in enumerate(keys) if k not in nested and k not in keys[:i]]

    def test_api_md_lists_the_release_keys(self):
        # docs/api.md §3.2.1 lists exactly the release keys, in their order; the bench keys only in its subsection
        doc = open(os.path.join(ROOT, "docs", "api.md"), encoding="utf-8").read()
        sec = doc[doc.index("#### 3.2.1 `diag`"):doc.index("**Bench build (`FC_DIAG_BENCH=1`")]
        listed = []
        for k in re.findall(r"`([a-z0-9_]+)`", sec[sec.index("in the\norder they are sent:"):sec.index("The heap figures (")]):
            if k not in listed:
                listed.append(k)
        code = self.release_keys()
        self.assertEqual([k for k in listed if k in code], code)  # every release key, in order
        self.assertEqual([k for k in code if k in listed], code)
        for bench in ("tasks", "ota_heap", "http_busy_ms_max", "fwd_heap_min"):
            self.assertNotIn(bench, code)


if __name__ == "__main__":
    unittest.main()
