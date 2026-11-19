"""Every heap figure of the sketch is read in one place. No getFreeHeap,
getMinFreeHeap, getMaxAllocHeap, esp_get_free_heap_size, esp_get_minimum_free_heap_size, MALLOC_CAP_INTERNAL or
heap_caps_get_* call in firmware/fauxcloud/*.cpp, *.ino or src/* outside heapNow() / heapSample() in app_core.cpp
(comments ignored). Run: python -I -m unittest discover -s test/tools"""
import glob
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.join(HERE, "..", "..", "firmware", "fauxcloud")
FORBIDDEN = re.compile(r"\b(getFreeHeap|getMinFreeHeap|getMaxAllocHeap|esp_get_free_heap_size|"
                       r"esp_get_minimum_free_heap_size|MALLOC_CAP_INTERNAL|heap_caps_get_\w+)\b")
ALLOWED_FUNCS = ("HeapNow heapNow()", "void heapSample()")


def strip_comments(src):
    """Drops // and /* */ comments, keeps string literals and the line count."""
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c in "\"'":  # strings and char literals such as '"' or '\''
            j = i + 1
            while j < n and src[j] != c:
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1])
            i = j + 1
        elif src.startswith("//", i):
            j = src.find("\n", i)
            i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2)
            out.append("\n" * src.count("\n", i, n if j < 0 else j))
            i = n if j < 0 else j + 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def body_span(src, signature):
    """(start, end) of the braces of the function defined by `signature`, or None."""
    k = src.find(signature)
    if k < 0:
        return None
    b = src.find("{", k)
    depth = 0
    for j in range(b, len(src)):
        depth += src[j] == "{"
        depth -= src[j] == "}"
        if depth == 0:
            return b, j
    return None


class HeapCalls(unittest.TestCase):
    def test_one_reader(self):
        files = (glob.glob(os.path.join(SKETCH, "*.cpp")) + glob.glob(os.path.join(SKETCH, "*.ino")) +
                 [f for f in glob.glob(os.path.join(SKETCH, "src", "*"))  # files only (src/bearssl/ is vendored C)
                  if os.path.isfile(f) and not f.endswith("web_assets.h")])
        bad = []
        for f in files:
            src = strip_comments(open(f, encoding="utf-8").read())
            spans = []
            if os.path.basename(f) == "app_core.cpp":
                for sig in ALLOWED_FUNCS:
                    sp = body_span(src, sig)
                    self.assertIsNotNone(sp, sig + " not found in app_core.cpp")
                    spans.append(sp)
            for m in FORBIDDEN.finditer(src):
                if not any(a <= m.start() <= b for a, b in spans):
                    bad.append("%s:%d %s" % (os.path.basename(f), src.count("\n", 0, m.start()) + 1, m.group(0)))
        self.assertEqual(bad, [], "heap reads outside heapNow()/heapSample(): " + ", ".join(bad))

    def test_the_rule_bites(self):
        src = strip_comments('int a = ESP.getFreeHeap(); // getMinFreeHeap\n/* MALLOC_CAP_INTERNAL */ const char* s = "x";')
        self.assertEqual([m.group(0) for m in FORBIDDEN.finditer(src)], ["getFreeHeap"])
        # a '"' char literal does not open a string (the call after it is still seen)
        src = strip_comments("char q = '\"'; char e = '\\''; int b = ESP.getMinFreeHeap(); // x\nconst char* u = \"http://a\";")
        self.assertEqual([m.group(0) for m in FORBIDDEN.finditer(src)], ["getMinFreeHeap"])


class DiagQuickWins(unittest.TestCase):
    """The static half of the log and heap rules (the sinks are sketch code, the host cannot run them)."""

    def read(self, name):
        return strip_comments(open(os.path.join(SKETCH, name), encoding="utf-8").read())

    def test_no_internal_heap_figure(self):
        # no INTERNAL heap figures: no MALLOC_CAP_INTERNAL read anywhere, not even in heapSample
        for f in glob.glob(os.path.join(SKETCH, "*.cpp")) + glob.glob(os.path.join(SKETCH, "src", "*.cpp")):
            self.assertNotIn("MALLOC_CAP_INTERNAL", strip_comments(open(f, encoding="utf-8").read()), f)

    def test_live_diag_after_the_304(self):
        # /api/live builds the diag (a heap read) only after the ETag check, so a 304 poll reads no heap
        src = self.read("app_http.cpp")
        a, b = body_span(src, "static esp_err_t onLive(httpd_req_t* req)")
        body = src[a:b]
        self.assertIn("notModified(", body)
        self.assertGreater(body.index("diagNow()"), body.index("notModified("))

    def test_chatter_reaches_no_sink(self):
        # logImpl returns before formatting when the line is not kept, so neither the UART nor the ring sees it;
        # logl decides with logKeeps (src/log.h, host-tested)
        src = self.read("app_core.cpp")
        a, b = body_span(src, "void logImpl(bool keep, bool boot, const char* tag, const char* fmt,")
        body = src[a:b]
        self.assertLess(body.index("if (!keep) return;"), body.index("vsnprintf"))
        self.assertLess(body.index("if (!keep) return;"), body.index("Serial.printf"))
        self.assertLess(body.index("if (!keep) return;"), body.index("bootlogCapture("))
        a, b = body_span(src, "void logl(Log lvl, const char* tag, const char* fmt, ...)")
        self.assertIn("logKeeps(lvl == Log::Chatter, gBridge.log_verbose)", src[a:b])
        self.assertIn("lvl == Log::Boot", src[a:b])  # the head flag from the level


if __name__ == "__main__":
    unittest.main()
