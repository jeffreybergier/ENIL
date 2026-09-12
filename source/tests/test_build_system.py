"""Exercise root/child forwarding and cleanup without requiring Apple toolchains."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[2]


class BuildSystemTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="enil-build-tests-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "project"
        self.root.mkdir()
        shutil.copy(REPO / "Makefile", self.root / "Makefile")
        shutil.copytree(REPO / "source/make", self.root / "source/make")
        for platform in ("macOS", "iOS"):
            child = self.root / "source" / platform
            child.mkdir()
            shutil.copy(REPO / "source" / platform / "Makefile", child / "Makefile")
            # Stand in for the installed compiler engine, keeping file-target
            # and validation behavior observable without running a compiler.
            (child / "build.mk").write_text(
                '.PHONY: validate probe fail analyze\n'
                'validate:\n\t@echo validate >> "$(BUILD_ROOT)/calls"\n'
                'probe:\n\t@echo "$(BUILD_DIR)|$(OPT_FLAGS)|$(ANALYZE_OUTPUT_DIR)"\n'
                'fail:\n\t@exit 7\n'
                'analyze:\n\t@mkdir -p "$(ANALYZE_OUTPUT_DIR)"\n'
                '\t@touch "$(ANALYZE_OUTPUT_DIR)/analyze.txt"\n'
                '$(BUILD_DIR)/ENIL.zip:\n\t@mkdir -p "$(BUILD_DIR)/Intermediates"\n'
                '\t@echo zip > "$@"\n'
                '$(BUILD_DIR)/ENIL.app:\n\t@mkdir -p "$@" "$(BUILD_DIR)/Intermediates"\n'
                '\t@echo binary > "$@/ENIL"\n'
            )
        (self.root / "build").mkdir()

    def make(self, *args, child=None, ok=True):
        cwd = self.root / "source" / child if child else Path(self.tmp.name)
        cmd = ["make", "--no-print-directory"]
        if not child:
            cmd += ["-f", str(self.root / "Makefile")]
        env = os.environ.copy()
        for key in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKEOVERRIDES"):
            env.pop(key, None)
        result = subprocess.run(cmd + list(args), cwd=cwd, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=30)
        if ok:
            self.assertEqual(result.returncode, 0, result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result.stdout

    def test_root_and_child_resolve_the_same_output_from_another_cwd(self):
        root = self.make("iOS-probe", "CONFIG=debug")
        child = self.make("probe", "CONFIG=debug", child="iOS")
        self.assertEqual(root, child)
        self.assertIn(str(self.root / "build/iOS/debug") + "|-O0|", root)

    def test_relative_and_absolute_output_overrides(self):
        self.assertIn(str(self.root / "custom/macOS/release"),
                      self.make("macOS-probe", "BUILD_ROOT=custom"))
        absolute = Path(self.tmp.name) / "external"
        self.assertIn(str(absolute / "iOS/release"),
                      self.make("probe", f"BUILD_ROOT={absolute}", child="iOS"))

    def test_debug_release_and_ipa_packaging(self):
        self.make("debug", "release", "-j2")
        import zipfile
        for config in ("debug", "release"):
            self.assertTrue((self.root / "build/macOS" / config / "ENIL.zip").is_file())
            ipa = self.root / "build/iOS" / config / "ENIL.ipa"
            with zipfile.ZipFile(ipa) as archive:
                self.assertEqual(archive.read("Payload/ENIL.app/ENIL"), b"binary\n")
            before = ipa.stat().st_mtime_ns
            self.make(f"iOS-{config}")
            self.assertEqual(ipa.stat().st_mtime_ns, before, "no-op build repackaged IPA")
            self.assertFalse((ipa.parent / "Intermediates/Payload").exists())

    def test_analyze_and_arbitrary_child_targets_forward(self):
        self.make("analyze")
        for platform in ("macOS", "iOS"):
            self.assertTrue((self.root / "build" / platform / "analyze/analyze.txt").exists())
        self.make("macOS-fail", ok=False)
        self.make("iOS-no-such-target", ok=False)

    def test_platform_clean_preserves_other_platform_and_unrelated_files(self):
        for platform in ("macOS", "iOS"):
            d = self.root / "build" / platform / "debug"
            d.mkdir(parents=True)
            (d / "keep").touch()
        sentinel = self.root / "build/unrelated"
        sentinel.touch()
        self.make("iOS-clean")
        self.assertFalse((self.root / "build/iOS").exists())
        self.assertTrue((self.root / "build/macOS/debug/keep").exists())
        self.make("clean")
        self.assertFalse((self.root / "build/macOS").exists())
        self.assertTrue(sentinel.exists())

    def test_unsafe_clean_roots_are_rejected(self):
        for path in ("/", str(self.root), str(self.root / "source"),
                     str(self.root / "source/shared"), str(self.root / ".git")):
            self.make("clean", f"BUILD_ROOT={path}", ok=False)
        self.assertTrue((self.root / "source/iOS/Makefile").is_file())

    def test_clean_does_not_follow_platform_symlinks(self):
        external = Path(self.tmp.name) / "external"
        external.mkdir()
        (external / "keep").touch()
        (self.root / "build/iOS").symlink_to(external, target_is_directory=True)
        self.make("iOS-clean")
        self.assertTrue((external / "keep").exists())
        self.assertFalse((self.root / "build/iOS").is_symlink())

    def test_invalid_configuration_and_empty_build_root_fail(self):
        self.make("iOS-probe", "CONFIG=typo", ok=False)
        self.make("iOS-probe", "CONFIG=debug release", ok=False)
        self.make("macOS-probe", "BUILD_ROOT=", ok=False)


if __name__ == "__main__":
    unittest.main()
