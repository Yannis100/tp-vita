#!/usr/bin/env python3
"""Fail the link step early if vita-elf-create will not find room for its SCE data.

Usage: check_text_gap.py <elf> [min_gap_bytes]

vita-elf-create appends its SCE metadata to the end of the first (R+E) PT_LOAD segment and fails with
"Cannot allocate N bytes for SCE data at end of segment 0; segment 1 overlaps" when the next segment
starts too close. The linker script rounds the data segment up to a 64 KiB boundary, so the gap
depends on the text size modulo 64 KiB. Remedy: change TPV_TEXT_PAD_KIB (vita/src/text_pad.cpp).
"""
import struct
import sys

PT_LOAD = 1
DEFAULT_MIN_GAP = 8 * 1024  # SCE data is ~5 KiB today; leave room for growth


def load_segments(path):
    with open(path, "rb") as f:
        ehdr = f.read(52)
        if ehdr[:4] != b"\x7fELF" or ehdr[4] != 1 or ehdr[5] != 1:
            sys.exit(f"{path}: not a 32-bit little-endian ELF")
        phoff, = struct.unpack_from("<I", ehdr, 28)
        phentsize, phnum = struct.unpack_from("<HH", ehdr, 42)
        f.seek(phoff)
        segs = []
        for _ in range(phnum):
            ph = f.read(phentsize)
            p_type, p_off, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from("<8I", ph)
            if p_type == PT_LOAD:
                segs.append((p_vaddr, p_filesz, p_memsz, p_flags))
    return segs


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    need = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_MIN_GAP
    segs = sorted(load_segments(path))
    if len(segs) < 2:
        sys.exit(f"{path}: expected at least 2 PT_LOAD segments, found {len(segs)}")
    text_end = segs[0][0] + segs[0][2]
    gap = segs[1][0] - text_end
    print(f"text segment ends 0x{text_end:08x}, data segment starts 0x{segs[1][0]:08x}: gap {gap} bytes")
    if gap < need:
        sys.exit(f"error: gap {gap} < {need} bytes; vita-elf-create will fail to place its SCE data.\n"
                 f"Change -DTPV_TEXT_PAD_KIB=<n> (currently padding vita/src/text_pad.cpp) by a few tens of KiB "
                 f"so the text segment ends well below a 64 KiB boundary, then rebuild.")


main()
