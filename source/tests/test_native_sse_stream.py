"""Chrome SSE framing and cursor regressions, with no external network calls."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class SSEStreamTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="enil-sse-stream-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = str(Path(cls.temp.name) / "sse-stream")
        shared = REPO / "source/shared"
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libcjson", "libcurl"], text=True))
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
            "-Wno-deprecated-declarations", "-ffunction-sections", "-fdata-sections",
            "-I" + str(shared), str(REPO / "source/tests/sse_stream.c"),
            str(shared / "enil_api_json.c"), "-Wl,--gc-sections", "-pthread",
            *flags, "-o", cls.binary,
        ], check=True)

    def check_stream(self, mode, chunks=(1, 7, 16384)):
        for chunk in chunks:
            with self.subTest(mode=mode, chunk=chunk):
                subprocess.run([self.binary, mode, str(chunk)], check=True, timeout=10)

    def test_messages_exceeding_old_line_and_event_buffers(self):
        self.check_stream("message")
        self.check_stream("large")

    def test_multiline_crlf_event_and_following_message(self):
        self.check_stream("multiline")
        self.check_stream("coalesced")

    def test_event_at_size_limit_including_crlf(self):
        self.check_stream("limit", (16384,))

    def test_oversized_lines_and_events_stop_without_acknowledgement(self):
        self.check_stream("oversize-line", (16384,))
        self.check_stream("oversize-event", (16384,))

    def test_partial_event_is_discarded_on_reconnect(self):
        self.check_stream("interrupted")

    def test_failed_delivery_retries_without_advancing_cursor(self):
        self.check_stream("retry")
