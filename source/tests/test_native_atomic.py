from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]

class AtomicTests(unittest.TestCase):
    def test_flag_handoff_and_concurrent_health_changes(self):
        with tempfile.TemporaryDirectory() as td:
            binary = str(Path(td) / "atomic")
            subprocess.run(["cc", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-I" + str(REPO / "source/shared"),
                str(REPO / "source/tests/native_atomic.c"),
                str(REPO / "source/shared/enil_health.c"), "-o", binary], check=True)
            subprocess.run([binary], check=True, timeout=10)
