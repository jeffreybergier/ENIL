"""Event and full-sync recovery through the production account/SQLite pipeline."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class NativeSyncRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="enil-sync-recovery-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / "recovery"
        shared = REPO / "source/shared"
        binary = cls.binary
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libcjson", "libcurl", "openssl", "sqlite3"], text=True))
        # GCC diagnoses pre-existing fixed-buffer formatting in the HTML
        # and sticker helpers pulled in by the complete portable core.
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations",
            "-Wno-format-truncation", "-ffunction-sections", "-fdata-sections", "-I" + str(shared),
            "-I" + str(REPO / "source/deps/qrcodegen/c"),
            str(REPO / "source/tests/native_sync_recovery.c"),
            *[str(p) for p in sorted(shared.glob("*.c"))],
            "-Wl,--gc-sections", "-Wl,--wrap=curl_easy_perform",
            "-Wl,--wrap=enil_line_post", "-Wl,--wrap=enil_line_post_ex",
            "-Wl,--wrap=enil_line_acquire_obs_token", "-pthread", *flags, "-lz", "-o", str(binary),
        ], check=True)

    def run_recovery(self, *args):
        with tempfile.TemporaryDirectory(dir=self.temp.name) as temp:
            subprocess.run([str(self.binary), str(Path(temp) / "session.json"),
                            str(Path(temp) / "enil.sqlite"), *args], check=True, timeout=15)

    def test_full_sync_preserves_cursor_until_successful_recovery(self):
        self.run_recovery()

    def test_failed_message_write_redelivers_before_advancing_cursors(self):
        self.run_recovery("message")

    def test_failed_cursor_write_keeps_in_memory_revision_for_retry(self):
        self.run_recovery("cursor")

    def test_uncached_reactions_do_not_block_later_messages(self):
        self.run_recovery("reactions")

    def test_legacy_startup_allows_inflight_saves_during_first_refresh(self):
        self.run_recovery("legacy")
