"""Clean only ENIL native output trees, including with a custom BUILD_ROOT."""
import argparse
from pathlib import Path
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("project_root", type=Path)
parser.add_argument("build_root", type=Path)
parser.add_argument("platform", nargs="?", choices=("macOS", "iOS"))
args = parser.parse_args()
project = args.project_root.resolve()
root = args.build_root.resolve()
# Never let a mistaken override turn clean into deletion of source or Git data.
protected = [project / name for name in ("source", ".git", ".github", "docs")]
if (root == project or root in project.parents or
        any(root == p or root in p.parents or p in root.parents for p in protected)):
    parser.error("refusing unsafe BUILD_ROOT")

for platform in ([args.platform] if args.platform else ["macOS", "iOS"]):
    target = root / platform
    if target.is_symlink():
        target.unlink()
    elif target.exists():
        shutil.rmtree(target)
    print(f"Cleaned {target}")
# A shared/custom output directory may contain unrelated files. Leave those alone.
try:
    root.rmdir()
except (FileNotFoundError, OSError):
    pass
