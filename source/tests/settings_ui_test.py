#!/usr/bin/env python3
"""Build an isolated Settings regression IPA inside the Altivec container.

Install only on an authorized test phone with appinst, launch
ENIL-settings-test://run, and require Documents/settings-result.txt to show
PASS within 15 seconds. A missing result, RUNNING, crash or FAIL is a failure.
The fixture replaces the account coordinator and keychain with synthetic data.
"""
import pathlib
import plistlib
import re
import subprocess

root = pathlib.Path(__file__).resolve().parents[2]
out = root / "build/tests/settings-ui"
out.mkdir(parents=True, exist_ok=True)
identity = "com.altivecintelligence.ENILSettingsTest"
plist = plistlib.loads((root / "source/iOS/Info.plist").read_bytes())
plist.update(CFBundleIdentifier=identity, CFBundleDisplayName="ENIL Settings Test",
             CFBundleURLTypes=[{"CFBundleURLSchemes": ["enil-settings-test"]}])
(out / "Info.plist").write_bytes(plistlib.dumps(plist))
(out / "Entitlements.plist").write_bytes(plistlib.dumps({
    "application-identifier": identity, "get-task-allow": True}))
makefile = (root / "source/iOS/Makefile").read_text().replace("\\\n", " ")
sources = re.search(r"^GROUP_ui_SOURCES = (.*)$", makefile, re.M).group(1).split()
sources.remove("main.m")
sources.append(str(root / "source/tests/settings_ui_test.m"))
subprocess.run([
    "make", "-C", str(root / "source/iOS"), "release",
    "BUILD_ROOT=" + str(out / "output"),
    "GROUP_ui_SOURCES=" + " ".join(sources),
    "INFO_PLIST=" + str(out / "Info.plist"),
    "ENIL_ENTITLEMENTS=" + str(out / "Entitlements.plist"),
], check=True)
print(out / "output/iOS/release/ENIL/ENIL.ipa")
