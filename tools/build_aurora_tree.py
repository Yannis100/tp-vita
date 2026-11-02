#!/usr/bin/env python3
"""Assemble the Vita view of Aurora's lib/ directory.

    build_aurora_tree.py <upstream-lib> <overlay-dir> <patches.py> <out-dir>

out-dir = upstream lib/ + overlay files (whole-file replacements/additions, same relative
path) + string patches from patches.py (PATCHES = [(relpath, old, new), ...]; every `old`
must occur exactly once, so an upstream change fails loudly instead of silently diverging).
Files are only rewritten when their content changes, so unrelated rebuilds are not triggered.
"""
import os, runpy, shutil, sys

up, overlay, patches_py, out = sys.argv[1:5]
patches = runpy.run_path(patches_py)["PATCHES"] if os.path.exists(patches_py) else []
by_file = {}
for rel, old, new in patches:
    by_file.setdefault(rel, []).append((old, new))

def write_if_changed(dst, data):
    if os.path.exists(dst):
        with open(dst, "rb") as f:
            if f.read() == data:
                return
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "wb") as f:
        f.write(data)

want = set()
def collect(root, skip_dirs=()):
    for d, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if x not in skip_dirs]
        for fn in files:
            rel = os.path.relpath(os.path.join(d, fn), root)
            yield rel, os.path.join(d, fn)

sources = {}
for rel, path in collect(up, skip_dirs=("dawn", "rmlui")):
    sources[rel] = path
for rel, path in collect(overlay):
    sources[rel] = path          # overlay wins

for rel, path in sources.items():
    data = open(path, "rb").read()
    if rel in by_file:
        text = data.decode()
        for old, new in by_file[rel]:
            if text.count(old) != 1:
                sys.exit(f"patch for {rel}: expected exactly one match of {old[:60]!r}, found {text.count(old)}")
            text = text.replace(old, new)
        data = text.encode()
    write_if_changed(os.path.join(out, rel), data)
    want.add(rel)

# drop files that no longer exist in either tree
for rel, _ in collect(out):
    if rel not in want:
        os.remove(os.path.join(out, rel))
missing = [r for r in by_file if r not in sources]
if missing:
    sys.exit("patches for unknown files: " + ", ".join(missing))
print(f"aurora tree: {len(want)} files -> {out}")
