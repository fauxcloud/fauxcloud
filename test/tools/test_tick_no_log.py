"""The forwarding tick writes no log line on a send. A logged send put 11.4 ms of the loop tick in finishResult()
(measured): one ~250-B logf at 115 200 Bd with no UART TX buffer blocks the caller until the FIFO takes
it. The send's line is written by the net-out task (logJob, before J_DONE). This check keeps logf / logJob / Serial out
of fwdTick() and finishResult() in app_fwd.cpp (comments ignored). Run: python -I -m unittest discover -s test/tools"""
import os
import re
import unittest

from test_heap_calls import strip_comments

HERE = os.path.dirname(os.path.abspath(__file__))
APP_FWD = os.path.join(HERE, "..", "..", "firmware", "fauxcloud", "app_fwd.cpp")
LOGGING = re.compile(r"\b(logf|logJob|Serial)\b")


def body(src, head):
    """The brace-balanced body of the function whose definition starts with `head`."""
    at = src.find(head)
    if at < 0:
        raise AssertionError("not found: " + head)
    i = src.index("{", at)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise AssertionError("unbalanced: " + head)


class TickNoLog(unittest.TestCase):
    def test_tick_and_finish_write_no_log(self):
        with open(APP_FWD, encoding="utf-8") as f:
            src = strip_comments(f.read())
        for head in ("void fwdTick()", "static void finishResult()"):
            m = LOGGING.search(body(src, head))
            self.assertIsNone(m, "%s: %s on the loop tick" % (head, m.group(0) if m else ""))

    def test_net_out_writes_the_send_line(self):
        with open(APP_FWD, encoding="utf-8") as f:
            src = strip_comments(f.read())
        self.assertIn("logJob(gB->job);", body(src, "static void netTask(void*)"))


if __name__ == "__main__":
    unittest.main()
