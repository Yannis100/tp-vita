#!/usr/bin/env python3
"""Refuse a VPK that contains something it must never contain.

`check_vpk.py` answers "will this install and appear as a bubble".  This answers a different
question, which no amount of install-testing catches: does the package ship something the project
has no right to redistribute, or something that makes the release unbuildable-by-others?

Two classes are refused:

  * Sony modules.  `libshacccg.suprx` and friends are Sony's, and a VPK that carries one is not
    distributable.  The port loads them from the user's console at run time instead.
  * Game data.  The build reads the user's own disc image from `ux0:data/tpvita/`; a packaged
    `.iso`/`.ciso`/`.gcm` is a copyright problem and also usually a leak of the tester's setup.

Usage: check_vpk_contents.py VPK [VPK ...]

Exit 0 when every VPK is clean, 1 with one `error:` line per offender.  Prints each archive's
entries so a CI log shows what shipped, not only what failed.
"""

from __future__ import annotations

import sys
import zipfile

# Extensions that must never be inside a distributed VPK.  Checked case-insensitively because the
# packager preserves whatever case the file had.
FORBIDDEN_SUFFIXES = (
    ".suprx",
    ".skprx",
    ".iso",
    ".gcm",
    ".ciso",
    ".rvz",
    ".wia",
    ".nkit.iso",
)

# A Sony module is recognisable by name even without the suffix, and these are the ones the runtime
# would actually need, so name them explicitly rather than relying on the suffix list.
FORBIDDEN_NAMES = ("libshacccg.suprx", "libatrac9.suprx", "libcuda.suprx", "libfiber.suprx")


def check(path: str) -> list[str]:
    problems: list[str] = []
    try:
        with zipfile.ZipFile(path) as zf:
            names = zf.namelist()
    except zipfile.BadZipFile:
        return [f"error: {path}: not a zip archive (a VPK is a zip)"]

    print(f"== {path} ({len(names)} entries)")
    for name in names:
        print(f"   {name}")

    for name in names:
        lowered = name.lower()
        base = lowered.rsplit("/", 1)[-1]
        if base in FORBIDDEN_NAMES or lowered.endswith(FORBIDDEN_SUFFIXES):
            problems.append(f"error: {path}: contains {name}, which must not be redistributed")

    # A VPK with no eboot is not a program; check_vpk.py catches this too, but a release gate that
    # silently accepts an empty archive is worse than a duplicated check.
    if not any(n.lower().endswith("eboot.bin") for n in names):
        problems.append(f"error: {path}: no eboot.bin")

    return problems


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__.strip().splitlines()[-1].strip(), file=sys.stderr)
        return 2

    problems: list[str] = []
    for path in argv:
        problems.extend(check(path))

    for line in problems:
        print(line, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
