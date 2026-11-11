#!/usr/bin/env python3
"""Unpack a PS Vita SELF (eboot.bin) into its decompressed, readable form.

    unpack_self.py <eboot.bin> <out.bin> [--strings]

A Vita SELF is an SceHeader (0x20 bytes) followed by an ELF header, then the
segments. VitaSDK's `vita-make-fself -c` stores each segment as a **raw zlib
stream**, and that is the whole of the obfuscation: there is no encryption in a
default eboot, and no key is needed.

Two things that look true and are not, both of which cost time here:

  1. `SceHeader` flags 0x100c0 do NOT mean "encrypted". Our own eboot, which we
     build ourselves, carries the same value. Do not infer encryption from it.
  2. The program headers are **not** `Elf32_Phdr`. `e_phentsize` is 32 but the
     fields at `p_offset`/`p_vaddr` are SCE segment descriptors, so reading them
     as (type, offset, vaddr, ...) yields plausible-looking nonsense
     (e.g. type=0x0159a284). Do not walk the phdrs to find the segments.

What does work, and what this script does, is to scan the file for zlib stream
headers and decompress each stream in place. The streams are contiguous, so the
concatenation yields the segments in file order. Verified two ways:

  * on our own eboot, the result contains `Aurora (vitaGL) initializing`,
    `ux0:data/tpvita/shader_fail` and `input script: ACTIVE` -- three strings we
    can see in our source, so the output is provably our code;
  * on capitanbarbacoa's release, the result contains `aurora`, `dusklight`,
    `GZ2` and `Zelda`, so it is provably a Dusklight build.

The output is NOT a valid ELF file -- the ELF header is not the first thing in
the concatenation, since segment 0 is data. Treat it as an address-ordered
image blob: good for `strings`, symbol names and constant tables, not for
`objdump -d` without further work.
"""
import re
import sys
import zlib

ZLIB_HEADERS = (0x01, 0x5E, 0x9C, 0xDA)


def unpack(data: bytes) -> bytes:
    out = bytearray()
    pos = 0
    streams = 0
    end = len(data) - 2
    while pos < end:
        if data[pos] != 0x78 or data[pos + 1] not in ZLIB_HEADERS:
            pos += 1
            continue
        obj = zlib.decompressobj()
        try:
            chunk = obj.decompress(data[pos:])
        except zlib.error:
            pos += 1
            continue
        if not chunk:
            pos += 1
            continue
        out += chunk
        streams += 1
        pos += len(data) - pos - len(obj.unused_data)
    print(f"{streams} zlib stream(s), {len(out)} bytes decompressed", file=sys.stderr)
    return bytes(out)


def is_self(data: bytes) -> bool:
    return data[:4] == b"SCE\x00"


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, "rb") as handle:
        data = handle.read()
    if not is_self(data):
        print(f"error: {src} is not a SELF (magic {data[:4]!r})", file=sys.stderr)
        return 2
    image = unpack(data)
    if not image:
        print("error: no zlib streams decompressed; the file may be encrypted", file=sys.stderr)
        return 1
    with open(dst, "wb") as handle:
        handle.write(image)
    print(f"wrote {dst}: {len(image)} bytes")

    # A cheap self-check: a Dusklight build mentions its own engine somewhere.
    for needle in (b"dusklight", b"aurora", b"aurora::", b"GZ2"):
        print(f"  contains {needle.decode():12s}: {needle in image}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
