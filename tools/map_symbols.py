#!/usr/bin/env python3
"""Turn a GNU ld -Map file into a sorted symbol table the crash recorder can resolve against.

Why this exists. Naming a faulting address on this port used to mean grepping the section headers of
tpv_vita.velf to work out whether the address was even in our image, then running addr2line against
the 666 MB debug ELF, all while assuming the link-time address equals the run-time address. That
assumption is what the crash log now records instead of assuming (see vita/src/crash_record.cpp).

This script produces a compact table of (address, size, name) rows that is embedded in the ELF and
matched by binary search when the process is already dying, so there is no filesystem read and no
allocation on that path.

Output is the body of a C++ string literal (crash_map.inc), one symbol per line:
    "0x82269aec,0x22,abort\\n"
    "0x82269af0,0x10,abort_helper\\n"
so it can be pasted into a constexpr const char* with #include and lands in .rodata with no loader
support needed at crash time.

Usage: map_symbols.py <linker.map> <crash_map.inc>
"""
import re
import sys

# Map lines look like:
#                 0x0000000082269aec                abort
#   0x0000000082269aec    0x30 libc_stub.a(foo.o)   bar
# The address is the first 0x token. A symbol line has exactly one 0x token (the address);
# an input-file line has two (the section's start and, sometimes, its size).
LINE = re.compile(r"^\s+(0x[0-9a-fA-F]+)(?:\s+(0x[0-9a-fA-F]+)\s+\S+)?\s+(\S.*?)\s*$")


def parse(path):
    rows = []
    with open(path, "r", errors="replace") as handle:
        for line in handle:
            match = LINE.match(line)
            if match is None:
                continue
            address, size, name = match.groups()
            # Skip anything that is clearly not a symbol name.
            if not name or " " in name or name.startswith("0x"):
                continue
            try:
                addr = int(address, 16)
            except ValueError:
                continue
            length = int(size, 16) if size else 0
            if addr == 0:
                continue
            rows.append((addr, length, name))

    # Symbols can appear more than once in a map (section, then input file). Keep the first, which is
    # the definition, and sort so the recorder can binary search.
    seen = {}
    for addr, length, name in rows:
        if addr not in seen:
            seen[addr] = (length, name)
    return sorted((addr, length, name) for addr, (length, name) in seen.items())


def c_escape(name):
    # Only a double quote or backslash would break the literal; symbol names are otherwise ordinary
    # identifiers plus occasional C++ mangling. Newlines cannot appear inside a map symbol name.
    return name.replace("\\", "\\\\").replace('"', '\\"')


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: map_symbols.py <linker.map> <crash_map.inc>\n")
        return 2
    rows = parse(argv[1])
    if not rows:
        sys.stderr.write("map_symbols.py: no symbols found in %s\n" % argv[1])
        return 1
    with open(argv[2], "w") as out:
        for addr, length, name in rows:
            out.write('"0x%08x,0x%x,%s\\n"\n' % (addr, length, c_escape(name)))
    sys.stderr.write("map_symbols.py: %d symbols -> %s\n" % (len(rows), argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
