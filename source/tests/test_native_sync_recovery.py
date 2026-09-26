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
            "-Wl,--wrap=sleep", "-Wl,--wrap=fsync",
            "-Wl,--wrap=enil_line_post", "-Wl,--wrap=enil_line_post_ex",
            "-Wl,--wrap=enil_line_acquire_obs_token", "-pthread", *flags, "-lz", "-o", str(binary),
        ], check=True)

    def run_recovery(self, *args):
        with tempfile.TemporaryDirectory(dir=self.temp.name) as temp:
            subprocess.run([str(self.binary), str(Path(temp) / "session.json"),
                            str(Path(temp) / "enil.sqlite"), *args], check=True, timeout=15)

    def test_full_sync_preserves_cursor_until_successful_recovery(self):
        self.run_recovery()

    def test_replayed_messages_preserve_synced_unread_count_but_new_events_update_it(self):
        self.run_recovery("replay-synced")

    def test_failed_message_write_redelivers_before_advancing_cursors(self):
        self.run_recovery("message")

    def test_failed_cursor_write_keeps_in_memory_revision_for_retry(self):
        self.run_recovery("cursor")

    def test_uncached_reactions_do_not_block_later_messages(self):
        self.run_recovery("reactions")

    def test_inaccessible_chat_updates_do_not_block_later_messages(self):
        self.run_recovery("chat-update-0")

    def test_chat_update_failures_retain_cursors_until_retry_succeeds(self):
        for mode, failure in enumerate([
            "HTTP", "transport", "missing chats", "invalid chat", "database",
            "invalid chats array", "invalid body",
        ], start=1):
            with self.subTest(failure=failure):
                self.run_recovery(f"chat-update-{mode}")

    def test_legacy_startup_allows_inflight_saves_during_first_refresh(self):
        self.run_recovery("legacy")

    def test_obsolete_cached_chats_do_not_block_history_or_read_receipt_sync(self):
        self.run_recovery("obsolete")

    def test_empty_current_chat_list_preserves_local_history(self):
        self.run_recovery("empty")

    def test_incomplete_current_chat_list_does_not_advance_cursor(self):
        for mode in ["partial-boxes", "invalid-boxes"]:
            with self.subTest(mode=mode):
                self.run_recovery(mode)

    def test_idle_polls_reset_failures_but_consecutive_errors_still_stop(self):
        for mode in ["idle-reset", "consecutive-failures"]:
            with self.subTest(mode=mode):
                self.run_recovery(mode)

    def test_partial_sync_requests_are_acknowledged_only_after_success(self):
        for mode in ["partial-native", "partial-chrome", "partial-newer"]:
            with self.subTest(mode=mode):
                self.run_recovery(mode)
