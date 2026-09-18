"""Production pending-login lifecycle; local files and synthetic credentials only."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class LoginRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="enil-recovery-build-")
        cls.addClassCleanup(cls.build.cleanup)
        cls.binary = str(Path(cls.build.name) / "recovery")
        shared = REPO / "source/shared"
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libcjson", "openssl"], text=True))
        subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                        "-ffunction-sections", "-fdata-sections", "-I" + str(shared),
                        str(REPO / "source/tests/login_recovery.c"),
                        *[str(shared / name) for name in ["enil_login_store.c", "enil_session.c",
                                                         "enil_identity.c", "enil_api_json.c"]],
                        "-Wl,--gc-sections", "-Wl,--wrap=fsync", "-pthread",
                        *flags, "-o", cls.binary], check=True)

    def run_case(self, mode):
        with tempfile.TemporaryDirectory(prefix="enil-recovery-") as root:
            subprocess.run([self.binary, root, mode], check=True, timeout=10)

    def test_repeated_reauthentication_never_restores_older_tokens(self):
        self.run_case("cycles")

    def test_directory_flush_failures_preserve_recoverable_credentials(self):
        self.run_case("durability")

    def test_explicit_restart_preserves_uncertain_reply_and_starts_fresh(self):
        self.run_case("restart")

    def test_saved_import_is_retired_and_activation_retires_refresh_journal(self):
        self.run_case("import")

    def test_superseded_saves_cannot_change_new_or_removed_accounts(self):
        for mode in ["stale", "stale-legacy"]:
            with self.subTest(mode=mode):
                self.run_case(mode)
