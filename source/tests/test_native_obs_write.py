"""OBS publication must preserve the previous file on delayed write errors."""
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]

class ObsWriteTests(unittest.TestCase):
    def test_close_failure_preserves_old_file_and_cleans_temporary(self):
        with tempfile.TemporaryDirectory(prefix="enil-obs-write-") as temp:
            binary = Path(temp) / "test"
            flags = shlex.split(subprocess.check_output(
                ["pkg-config", "--cflags", "libcjson", "libcurl", "openssl"], text=True))
            subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                "-Wno-deprecated-declarations", "-ffunction-sections", "-fdata-sections",
                "-I" + str(REPO / "source/shared"), *flags,
                str(REPO / "source/tests/native_obs_write.c"),
                "-Wl,--gc-sections", "-Wl,--wrap=fclose", "-o", str(binary)], check=True)
            subprocess.run([str(binary), temp], check=True)
