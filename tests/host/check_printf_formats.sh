#!/bin/sh
# Fails if Vita-side code uses a printf length modifier that the VitaSDK's newlib does not implement.
#   tests/host/check_printf_formats.sh [dir...]      (default: vita/)
# Found by running the VPK in Vita3K: the boot log printed "zu disc-like file(s)" for  log.line("%zu", s.files.size())  - %z is
# not supported by the newlib in the VitaSDK, the host glibc (which every host test uses) accepts it, so no host test could
# see it. %ll (long long) IS supported on the Vita: the 1459978240-byte size printed correctly in the same run.
# Cast to unsigned / unsigned long long (or use PRIu32 / PRIu64) instead of %zu / %zd / %ju / %jd / %td / %hh*.
#
# COMMENTS ARE NOT CODE, AND THIS CHECKER USED TO THINK THEY WERE (2026-11-12).
# The pattern matched a comment that *documented* the "%zu" lesson, so the guard failed on the note that
# explains why the guard exists. A checker that goes red on documentation is a checker somebody deletes,
# so comment text is now stripped before matching. Line comments, trailing comments and block comments
# are handled; strings are not, because a "%zu" inside a string literal is exactly what this looks for.
root=$(cd "$(dirname "$0")/../.." && pwd)
[ $# -gt 0 ] || set -- "$root/vita"
bad=$(python3 - "$@" <<'PY'
import re, sys, pathlib

EXT = {".c", ".cc", ".cpp", ".h", ".hpp"}
# %[-+ #0-9.*]*(z|j|t|hh)[diouxX]
PAT = re.compile(r"%[-+ #0-9.*]*(?:hh|z|j|t)[diouxX]")

def strip_comments(line: str, in_block: bool) -> tuple[str, bool]:
    """Blank out comment text, keeping string literals and column positions intact."""
    out = []
    i = 0
    n = len(line)
    quote = None
    while i < n:
        c = line[i]
        if in_block:
            if line.startswith("*/", i):
                in_block = False
                out.append("  ")
                i += 2
            else:
                out.append(" ")
                i += 1
            continue
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(line[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in "\"'":
            quote = c
            out.append(c)
            i += 1
            continue
        if line.startswith("//", i):
            out.append(" " * (n - i))
            break
        if line.startswith("/*", i):
            in_block = True
            out.append("  ")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out), in_block

hits = []
for root in sys.argv[1:]:
    for path in sorted(pathlib.Path(root).rglob("*")):
        if path.suffix not in EXT or not path.is_file():
            continue
        in_block = False
        try:
            text = path.read_text(errors="replace")
        except OSError:
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            code, in_block = strip_comments(line, in_block)
            if PAT.search(code):
                hits.append(f"{path}:{lineno}:{line.strip()}")

print("\n".join(hits))
PY
)
if [ -n "$bad" ]; then
  echo "printf length modifiers the Vita's newlib does not implement (use unsigned / unsigned long long casts):"
  echo "$bad"
  exit 1
fi
echo "printf formats: no %z / %j / %t / %hh in code under $*"
