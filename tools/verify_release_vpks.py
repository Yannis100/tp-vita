#!/usr/bin/env python3
"""Verify the two release VPKs before they are attached to a release.

Separate from check_vpk.py on purpose: check_vpk.py answers "is this package
well-formed", this answers "is this the RIGHT package for the name it is about
to be published under". Both are needed, because a packaging mix-up produces a
perfectly valid VPK with the wrong title ID.

Usage: verify_release_vpks.py <release.vpk> <timing.vpk>
Exit code 0 = both correct, 1 = a mismatch (prints what was expected).
"""
from __future__ import annotations

import sys
import zipfile

# (argument index, expected title ID, what it is)
EXPECTED = [
    (1, b"TPLG00001", "release"),
    (2, b"TPLG30001", "timing-clean"),
]


def main(argv: list[str]) -> int:
    if len(argv) != 3:
        sys.exit(__doc__)
    bad: list[str] = []
    for idx, title_id, what in EXPECTED:
        path = argv[idx]
        try:
            sfo = zipfile.ZipFile(path).read("sce_sys/param.sfo")
        except (OSError, KeyError) as exc:
            bad.append(f"{path}: cannot read sce_sys/param.sfo ({exc})")
            continue
        if title_id not in sfo:
            bad.append(f"{path}: expected title ID {title_id.decode()} for the {what} build")
        else:
            print(f"ok: {path} carries {title_id.decode()} ({what})")
    if bad:
        print("error: " + "; ".join(bad), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
