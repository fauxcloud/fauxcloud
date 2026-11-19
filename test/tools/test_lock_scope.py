"""A shared field is read, decided and written under ONE gLock hold where a second hold would let a change slip in
between. The functions here are glue (not host-compiled), so their hold structure is checked in the source: the body is
split into gLock holds (xSemaphoreTake(gLock ...) ... xSemaphoreGive(gLock)), comments ignored.
Run: python -I -m unittest discover -s test/tools"""
import os
import unittest

from test_heap_calls import body_span, strip_comments

HERE = os.path.dirname(os.path.abspath(__file__))
SKETCH = os.path.join(HERE, "..", "..", "firmware", "fauxcloud")
TAKE, GIVE = "xSemaphoreTake(gLock", "xSemaphoreGive(gLock"


def holds(path, signature):
    """The text of each gLock hold inside the function, in order."""
    with open(os.path.join(SKETCH, path), encoding="utf-8") as f:
        src = strip_comments(f.read())
    span = body_span(src, signature)
    if span is None:
        raise AssertionError("%s not found in %s" % (signature, path))
    body, out, k = src[span[0]:span[1]], [], 0
    while True:
        a = body.find(TAKE, k)
        if a < 0:
            return out
        b = body.find(GIVE, a)
        if b < 0:
            raise AssertionError("a gLock hold without its give in %s" % signature)
        out.append(body[a:b])
        k = b + len(GIVE)


class LockScope(unittest.TestCase):
    def test_learn_snapshot_metar_in_the_writing_hold(self):
        h = holds("app_learn.cpp", "void snapshot(int i, uint32_t gen)")
        writing = [x for x in h if "st->use = u" in x]
        self.assertEqual(len(writing), 1, "one hold writes st->use")
        self.assertIn("metarEligibleLocked()", writing[0], "metar is read in the hold that writes st->use")
        self.assertEqual(sum("metarEligibleLocked()" in x for x in h), 1, "no earlier metar read")

    def test_wufc_result_fenced_in_one_hold(self):
        # the save epoch is compared in the hold that applies the result to the scheduler, the store and the key
        h = holds("app_fwd.cpp", "static void finishWufc(Job& j, uint32_t now)")
        self.assertEqual(len(h), 1, "finishWufc takes gLock once")
        x = h[0]
        for part in ("gWufcEpoch == gWufcJobEpoch", "gWufc.result(", "same ? out : WufcOutcome::Off", "wufcPut(",
                     "gWufcCfg.verified = 1"):
            self.assertIn(part, x)
        self.assertLess(x.find("gWufcEpoch == gWufcJobEpoch"), x.find("gWufc.result("))

    def test_holds_helper_bites(self):
        # a function with the read in one hold and the write in another fails the rule above
        import tempfile
        src = ("void f() {\n xSemaphoreTake(gLock, portMAX_DELAY);\n m = metarEligibleLocked();\n xSemaphoreGive(gLock);\n"
               " xSemaphoreTake(gLock, portMAX_DELAY);\n st->use = u;\n xSemaphoreGive(gLock);\n}\n")
        with tempfile.TemporaryDirectory(dir=SKETCH) as d:
            p = os.path.join(d, "x.cpp")
            with open(p, "w", encoding="utf-8") as f:
                f.write(src)
            h = holds(os.path.relpath(p, SKETCH), "void f()")
        self.assertEqual(len(h), 2)
        self.assertNotIn("metarEligibleLocked()", [x for x in h if "st->use = u" in x][0])


if __name__ == "__main__":
    unittest.main()
