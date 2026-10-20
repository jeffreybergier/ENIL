#!/usr/bin/env python3
"""Build the offline recovery/UI harness; never deploys or opens account data.

Run the executable with a disposable account directory containing
.enil-offline-fixture, enil.sqlite and copied media/purchases. Do not copy a
session.json. Add --ui to display a representative chat after recovery.
--regenerate removes one thumbnail in the disposable fixture and verifies
that recovery recreates it. Never use the normal application account directory.
Every application curl transfer is replaced at compile time by a refusal.
On Leopard, additionally run under sandbox-exec with network* denied and
production account reads/writes denied. Preserve logs privately: framework
diagnostics can include message identifiers. ui.png contains private content.
"""
import pathlib
import re
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
makefile = (root / "source/macOS/Makefile").read_text().replace("\\\n", " ")
sources = re.search(r"^GROUP_ui_SOURCES = (.*)$", makefile, re.M).group(1).split()
sources.remove("main.m")
sources.append(str(root / "source/tests/attachment_mac_test.m"))
subprocess.run([
    "make", "-C", str(root / "source/macOS"), "release",
    "BUILD_ROOT=" + str(root / "build/tests/attachment-native"),
    "GROUP_ui_SOURCES=" + " ".join(sources),
    "GROUP_shared_FLAGS=-Dcurl_easy_perform=enil_test_refuse_network",
    "ENIL_CORE=static", "ENIL_COCOA=static",
], check=True)
