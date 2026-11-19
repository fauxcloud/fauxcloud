"""Tests for tools/fwd_fake_endpoint.py: every canned answer comes over a real socket byte
for byte as the host parser vectors have it, the behaviour cases behave, and the log line never shows a key.
Run: python -I -m unittest discover -s test/tools"""
import io
import pathlib
import socket
import sys
import time
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "tools"))
import fwd_fake_endpoint as fake  # noqa: E402


def get(port, path, timeout=20):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.sendall(("GET %s HTTP/1.1\r\nHost: fake\r\nUser-Agent: fauxcloud/test\r\nConnection: close\r\n\r\n" % path).encode())
    data = b""
    t0 = time.monotonic()
    while True:
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        data += chunk
    s.close()
    return data, time.monotonic() - t0


class FakeEndpoint(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.srv, cls.port = fake.start()

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def test_every_case_is_served_byte_for_byte(self):
        for name, _, _, body, _, _, _ in fake.CASES:
            data, _ = get(self.port, "/%s/weatherstation/updateweatherstation.php?ID=EXAMPLE1&PASSWORD=examplekey" % name)
            self.assertEqual(data, body, name)

    def test_close_and_split(self):
        data, _ = get(self.port, "/close/x")
        self.assertEqual(data, b"")
        data, secs = get(self.port, "/split/x")
        self.assertEqual(data, dict((c[0], c[3]) for c in fake.CASES)["wu-ok"])
        self.assertGreater(secs, 1.0)  # one byte every 50 ms

    def test_unknown_case_is_404(self):
        data, _ = get(self.port, "/nope")
        self.assertTrue(data.startswith(b"HTTP/1.1 404"))

    def test_the_log_line_hides_keys(self):
        self.assertEqual(fake.redact("GET /wu-ok/x?ID=EXAMPLE1&PASSWORD=examplekey&t=1 HTTP/1.1"),
                         "GET /wu-ok/x?ID=EXAMPLE1&PASSWORD=***&t=1 HTTP/1.1")
        self.assertEqual(fake.redact("GET /x/?key=abc&pass=def"), "GET /x/?key=***&pass=***")
        srv, port = fake.start(quiet=False)
        try:
            out = io.StringIO()
            with redirect_stdout(out):
                get(port, "/wu-ok/x?ID=EXAMPLE1&PASSWORD=examplekey")
                time.sleep(0.2)
            self.assertNotIn("examplekey", out.getvalue())
            self.assertIn("PASSWORD=***", out.getvalue())
        finally:
            srv.shutdown()
            srv.server_close()

    def test_the_cases_cover_every_result(self):
        results = {c[4] for c in fake.CASES}
        self.assertEqual(results, {"ok", "dup", "limited", "refused", "reject", "fail"})
        self.assertIn("stall15", fake.BEHAVIOUR)


if __name__ == "__main__":
    unittest.main()
