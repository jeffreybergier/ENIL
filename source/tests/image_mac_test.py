#!/usr/bin/env python3
"""Build an offline ImageIO harness. No deployment or production data access.

Set ARCHS to select architectures. Stage the resulting app and synthetic or
copied fixtures below ~/Desktop on an explicitly authorized test Mac. Run:
  ENIL.app/Contents/MacOS/ENIL output.jpg valid.jpg valid.png --reject bad.jpg
The output path must be disposable; successful thumbnail tests replace it.
Retain stdout/stderr as the native test log. A nonzero exit means failure.
"""
import pathlib
import re
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
makefile = (root / "source/macOS/Makefile").read_text().replace("\\\n", " ")
sources = re.search(r"^GROUP_ui_SOURCES = (.*)$", makefile, re.M).group(1).split()
sources.remove("main.m")
sources.append(str(root / "source/tests/image_mac_test.m"))
subprocess.run([
    "make", "-C", str(root / "source/macOS"), "release",
    "BUILD_ROOT=" + str(root / "build/tests/image-native"),
    "GROUP_ui_SOURCES=" + " ".join(sources),
    "ENIL_CORE=static", "ENIL_COCOA=static",
], check=True)
