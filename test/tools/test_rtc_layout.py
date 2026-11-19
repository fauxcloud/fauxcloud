"""tools/check_rtc_layout.py: an RTC_NOINIT word that moves, shrinks, grows without its line updated or is overtaken by
a new word inside the listed span fails the build; recorded growth at the end and new words after it pass. Synthetic nm
output, no ELF."""
import importlib.util
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("check_rtc_layout", os.path.join(HERE, "..", "..", "tools", "check_rtc_layout.py"))
rl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rl)

LAYOUT = "start 0x50000200\napp::gRtc 0 36\n"


def nm(*syms, start=0x50000200, end=None):
    lines = ["%08x A _rtc_noinit_start" % start]
    top = start
    for name, off, size in syms:
        lines.append("%08x %08x b %s" % (start + off, size, name))
        top = max(top, start + off + size)
    lines.append("%08x A _rtc_noinit_end" % (end or top))
    lines.append("%08x 00000020 d s_sleep_sub_mode_ref_cnt" % (end or top))  # the next section: ignored
    return "\n".join(lines)


class RtcLayout(unittest.TestCase):
    def test_holds(self):
        self.assertEqual(rl.check(nm(("app::gRtc", 0, 36)), LAYOUT), [])

    def test_grown_at_the_end_and_appended(self):
        # growth at the end passes once its line holds the new size; a new word after the listed span passes
        self.assertEqual(rl.check(nm(("app::gRtc", 0, 52)), "start 0x50000200\napp::gRtc 0 52\n"), [])
        self.assertEqual(rl.check(nm(("app::gRtc", 0, 36), ("app::gBootLog", 36, 64)), LAYOUT), [])

    def test_unrecorded_growth_fails(self):
        # the committed list is the truth: a word that grew (the BootPrev restart word) without its line updated fails
        bad = rl.check(nm(("app::gRtc", 0, 52)), LAYOUT)
        self.assertTrue(any("grew" in b and "record the new size" in b for b in bad), bad)
        old = "start 0x50000200\napp::gRtc 0 64\napp::gBootLog 64 4116\napp::gPrevTail 4180 2056\n"
        built = nm(("app::gRtc", 0, 64), ("app::gBootLog", 64, 4116), ("app::gPrevTail", 4180, 2060))
        self.assertTrue(any("app::gPrevTail grew" in b for b in rl.check(built, old)))
        with open(os.path.join(HERE, "..", "..", "tools", "rtc_layout.txt"), encoding="utf-8") as f:
            self.assertEqual(rl.check(built, f.read()), [])

    def test_b12_5_shift_fails(self):
        # a build with a 16-B record linked first and the listed words 16 B later
        bad = rl.check(nm(("app::gOtaHeapRtc", 0, 16), ("app::gRtc", 16, 36)), LAYOUT)
        self.assertTrue(any("moved" in b for b in bad) and any("inside the listed words" in b for b in bad))

    def test_missing_shrunk_start(self):
        self.assertTrue(any("missing" in b for b in rl.check(nm(("app::gOther", 0, 36)), LAYOUT)))
        self.assertTrue(any("shrank" in b for b in rl.check(nm(("app::gRtc", 0, 20)), LAYOUT)))
        self.assertTrue(any("section start" in b for b in rl.check(nm(("app::gRtc", 0, 36), start=0x50000210), LAYOUT)))

    def test_committed_layout_parses(self):
        with open(os.path.join(HERE, "..", "..", "tools", "rtc_layout.txt"), encoding="utf-8") as f:
            start, words = rl.parse_layout(f.read())
        # the boot log after gRtc, the previous boot's tail after the boot log (append-only)
        self.assertEqual((start, words), (0x50000200, {"app::gRtc": (0, 64), "app::gBootLog": (64, 4116),
                                                       "app::gPrevTail": (4180, 2060)}))


if __name__ == "__main__":
    unittest.main()
