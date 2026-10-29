#!/usr/bin/env python3
"""Make device absolute symlinks relocatable inside an exported sysroot."""
import argparse
import os
from pathlib import Path
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("sysroot", type=Path)
root = parser.parse_args().sysroot.resolve()
if root == Path("/") or not (root / "usr/include/features.h").is_file():
    raise SystemExit("Expected an exported sysroot, never the running system root")
for folder, directories, files in os.walk(root, followlinks=False):
    for name in directories + files:
        path = Path(folder) / name
        if not path.is_symlink():
            continue
        target = os.readlink(path)
        if target.startswith("/"):
            relative = os.path.relpath(root / target.lstrip("/"), path.parent)
            path.unlink()
            path.symlink_to(relative)
            print(f"{path.relative_to(root)} -> {relative}")
entry = root / "lib"
if not entry.exists() and not entry.is_symlink():
    entry.symlink_to("usr/lib")
