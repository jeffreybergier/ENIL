"""Consumer declarations; the engine owns compilation/dependency regression tests."""
from pathlib import Path
import os
import subprocess
import unittest

REPO = Path(__file__).resolve().parents[2]
ENGINE = Path(os.environ.get("ALTIVEC_ROOT", "/altivec"))
if not ENGINE.exists():
    ENGINE = REPO.parent / "AltivecIntelligence"

class BuildSystemTests(unittest.TestCase):
    def config(self, directory, target, *options):
        result = subprocess.run(["make", "--no-print-directory", "-C", str(directory),
            target, f"ALTIVEC_ROOT={ENGINE}", *options], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return next(line for line in result.stdout.splitlines() if line.startswith("project="))

    def test_root_child_paths(self):
        for platform in ("macOS", "iOS"):
            for override in ("custom", "/tmp/enil-output"):
                args = ("BUILD_ROOT=" + override, "CONFIG=debug")
                self.assertEqual(self.config(REPO, platform + "-print-config", *args),
                                 self.config(REPO / "source" / platform, "print-config", *args))

    def test_foreign_workdir(self):
        for entry in (REPO / 'Makefile', REPO / 'source/macOS/Makefile'):
            target = 'macOS-print-config' if entry.parent == REPO else 'print-config'
            result = subprocess.run(['make', '--no-print-directory', '-C', str(REPO.parent),
                '-f', str(entry), target, f'ALTIVEC_ROOT={ENGINE}', 'BUILD_ROOT=custom'],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn(f'project={REPO}', result.stdout)
            self.assertIn(f'output={REPO}/custom/macOS/release', result.stdout)

    def test_no_private_engine_recipes(self):
        for platform in ("macOS", "iOS"):
            text = (REPO / "source" / platform / "Makefile").read_text()
            self.assertNotIn("\n\t", text)
            self.assertNotIn("$(INT_DIR)", text)
            self.assertIn("ENIL_TYPE = app", text)

if __name__ == "__main__":
    unittest.main()
