#!/usr/bin/env python3
"""Turn the linked ELF's symbol table into a table the crash recorder can resolve against.

Why this exists. The recorder names the function that called abort(). It cannot use a backtrace
(the Vita has no backtrace()), so it needs a symbol table compiled into the image and matched by
address while the process is already dying.

Why NOT the linker map, which is what this used to read. The ld -Map file looked like the obvious
source because the build already produced it, but it was wrong twice over, and both defects were
invisible until a real crash was decoded:

  1. Half the symbols were missing. Map symbol lines carry no size column, and the old parser
     rejected any name containing a space. C++ demangled names contain spaces and commas, so 43.8%
     of symbols were silently discarded -- including every templated function and every function
     with arguments. `borealis::log::detail::write_fatal(std::basic_string_view<...>, ...)` was one
     of them, so an address inside write_fatal resolved to the nearest *surviving* symbol,
     `borealis::log::file_descriptor()`, which is a 0x10-byte atomic load that cannot abort. The
     log named innocent code, and that cost a session.
  2. Every size was 0, because the map does not record one. A size of 0 disables the "is the
     address actually inside this symbol" check, so nothing ever rejected a bad match.

nm -S --defined-only supplies both: complete symbol coverage and real sizes. Demangled (-C) is used
rather than mangled because it is what a reader needs, and it is no larger -- demangled names are
frequently shorter than their mangling.

Format. One row per symbol, TAB separated, because tab is the one byte that cannot appear in a
demangled name (commas appear in 19,226 of them, so the old comma-separated format was ambiguous):

    0x81000228	0x0	OSSwitchStack

Output is a complete C++ definition, so the consumer is a bare #include at namespace scope with no
surrounding syntax to get wrong:

    static const char kTable[] =
    "0x81000228\t0x0\tOSSwitchStack\n"
    ;
    static const unsigned kRowCount = 66975;

The total size of the table depends only on the set of (size, name) pairs, not on the addresses,
because addresses are padded to a fixed 8 hex digits. Do not "improve" that padding: it is what
stops the table's own size from perturbing the layout it is about to describe.

Usage: map_symbols.py <elf> <crash_map.inc> [nm]
"""
import os
import subprocess
import sys

# Address, size, type letter, name. nm -S -C pads the fields, and the name may contain anything.
# Splitting on whitespace with a max of 3 splits is what keeps names with spaces intact.
USABLE_TYPES = "TtWw"  # text, and weak text of either binding


def read_nm(elf, nm):
    try:
        out = subprocess.run([nm, "-S", "-C", "--defined-only", elf],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
    except OSError as exc:
        sys.stderr.write("map_symbols.py: cannot run %s: %s\n" % (nm, exc))
        return None
    except subprocess.CalledProcessError as exc:
        sys.stderr.write("map_symbols.py: %s failed: %s\n"
                         % (nm, exc.stderr.decode("utf-8", "replace")[:400]))
        return None
    return out.stdout.decode("utf-8", "replace")


def parse(text):
    rows = {}
    for line in text.splitlines():
        parts = line.split(None, 3)
        if len(parts) < 4:
            continue
        address, size, kind, name = parts
        if kind not in USABLE_TYPES:
            continue
        try:
            addr = int(address, 16)
            length = int(size, 16)
        except ValueError:
            continue
        if addr == 0 or not name:
            continue
        # One row per address. Aliases (identical address, several names) keep the first, which is
        # the one nm printed first and the one the linker considers primary.
        if addr in rows:
            continue
        rows[addr] = (length, name)
    return sorted((addr, length, name) for addr, (length, name) in rows.items())


def sanitize(name):
    # Tab is the separator and a newline would split the row, so neither may survive in a name. In
    # practice nm emits neither; this is a guard, not a transform.
    return name.replace("\t", " ").replace("\r", " ").replace("\n", " ")


def main(argv):
    if len(argv) not in (3, 4):
        sys.stderr.write("usage: map_symbols.py <elf> <crash_map.inc> [nm]\n")
        return 2
    elf = argv[1]
    if not os.path.exists(elf):
        sys.stderr.write("map_symbols.py: no such ELF: %s\n" % elf)
        return 1
    nm = argv[3] if len(argv) == 4 else os.environ.get("NM", "arm-vita-eabi-nm")

    text = read_nm(elf, nm)
    if text is None:
        return 1
    rows = parse(text)
    if not rows:
        sys.stderr.write("map_symbols.py: no text symbols found in %s\n" % elf)
        return 1

    # NOTE: this is a runtime data file, not C source. It must contain REAL tab and newline bytes.
    # Writing the two-character sequences "\t" and "\n" here (which is what the compiled-in version
    # needed) produces a table the resolver parses as one giant malformed line and matches nothing.
    with open(argv[2], "w", newline="\n") as out:
        out.write("// Generated by tools/map_symbols.py -- do not edit.\n")
        out.write("// %d symbols from %s\n" % (len(rows), os.path.basename(elf)))
        for addr, length, name in rows:
            out.write("0x%08x\t0x%x\t%s\n" % (addr, length, name))
    sys.stderr.write("map_symbols.py: %d symbols, %d bytes -> %s\n"
                     % (len(rows), os.path.getsize(argv[2]), argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
