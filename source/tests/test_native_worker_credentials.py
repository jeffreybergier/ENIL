from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]

class WorkerCredentialsTests(unittest.TestCase):
    def test_snapshot_lifetime_pair_consistency_and_long_secrets(self):
        with tempfile.TemporaryDirectory() as td:
            binary = str(Path(td) / "credentials")
            flags = shlex.split(subprocess.check_output(
                ["pkg-config", "--cflags", "--libs", "libcjson", "libcurl"], text=True))
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-I" + str(REPO / "source/shared"),
                str(REPO / "source/tests/native_worker_credentials.c"), *flags, "-o", binary], check=True)
            subprocess.run([binary], check=True, timeout=10)
