#!/usr/bin/env python3
"""Read and validate a Twilight Princess GameCube save (`.gci`) without a console.

WHY THIS EXISTS (2026-11-11)

A driven Vita3K run reached the file select with a save that had been written by this
port, and every slot read "New Quest Log" instead of the player's name. Two opposite
explanations fit a screenshot: the card was never read, or the card was read and the
save is empty. Telling them apart by eye is impossible, so this tool reads the bytes.

It resolved the case: the checksums are valid, the player name really is empty, and
the slot therefore legitimately displays "New Quest Log". `dFile_info_c::setSaveData()`
(`dusklight/src/d/d_file_sel_info.cpp:121`) is the authority:

    checksum invalid  -> "This Quest Log is Corrupted", return -1
    checksum valid, name empty -> "New Quest Log", return 1
    checksum valid, name set   -> the player's name, return 0

So "New Quest Log" on every slot proves the card WAS read and the data IS valid.

ON-DISK FORMAT

A GCI file is a 64-byte directory entry followed by `m_blockCount` 8 KiB blocks:

    +0x00  gameName[4]     "GZ2P"
    +0x04  company[2]      "01"
    +0x06  _padding0
    +0x07  bannerFormat    low 2 bits = banner format, bit 2 = animation
    +0x08  fileName[32]    "gczelda2"
    +0x28  time            u32, BE seconds since 2000-01-01
    +0x2C  iconAddr        u32, BE (0xFFFFFFFF = none)
    +0x30  iconFormat      u16, BE, 2 bits per icon
    +0x32  iconSpeed       u16, BE, 2 bits per icon
    +0x34  permission
    +0x35  copyTimes
    +0x36  startBlock      u16, BE
    +0x38  blockCount      u16, BE
    +0x3A  _padding1[2]
    +0x3C  commentAddr     u32, BE

`bannerFormat` is at **0x07**, not 0x06: `CARDDir` (`dolphin/card.h`) puts a one-byte
`_padding0` between `company[2]` and the banner byte. Reading 0x06 reports `0` where the
file says `5` (C8 banner, bounce animation), which reads like a defect that is not there.

The game writes four blocks (32,832 bytes total): block 0 is the banner and icon data,
block 2 (card offset 0x4000) and block 3 (card offset 0x6000) are two copies of the
save sector. Each sector starts with `data_s`:

    +0x00  unk_0x0     int
    +0x04  dataVersion int   must be SAVEDATA_VERSION = 6
    +0x08  data[3 * 0xA94 + 0x38]
    +...   checksum    u32

and each 0xA94-byte quest log ends with a u64 running checksum.

Two endianness traps are encoded below, because both were guessed wrong once:

1. `mDoMemCdRWm_CalcCheckSum()` computes `high += word; low += ~word` in **u16**, so
   each accumulator wraps at 65536, and the result is `high << 16 | low`. Widening the
   accumulators to u32 gives a different number.
2. `BE(u16)` is a no-op byte type on a little-endian host (`dusklight/include/helpers/
   endian.h:352`), so the words are read in **host** order. On the Vita, which is
   little-endian, the game therefore writes and verifies little-endian words. The stored
   value matches the little-endian reading and not the big-endian one. That is
   self-consistent on the device and not a valid GameCube card image.

Usage:
    gci.py <file.gci> [<file.gci> ...]
    gci.py --json <file.gci>

Exit status: 0 if every file parsed and every sector validated, 1 otherwise.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path

HEADER_SIZE = 64
BLOCK_SIZE = 0x2000
SAVEDATA_SIZE = 0xA94          # d/d_save.h: one quest log
SAVEDATA_NUM = 3               # m_Do_MemCard.h: three quest logs
SAVEDATA_VERSION = 6           # m_Do_MemCard.h
SAVE_SECTOR_OFFSETS = (0x4000, 0x6000)   # primary, backup
# dSv_save_c layout (d/d_save.h:922): mPlayer @0x000, mPlayerInfo @0x1A0,
# mPlayerName @0x1A0 + 0x14, mHorseName follows. Sizes: dSv_save_c = 0x1EC.
PLAYER_INFO_OFFSET = 0x1A0
PLAYER_NAME_OFFSET = PLAYER_INFO_OFFSET + 0x14
PLAYER_NAME_LEN = 17
HORSE_NAME_OFFSET = PLAYER_NAME_OFFSET + PLAYER_NAME_LEN


@dataclass
class QuestLog:
    index: int
    checksum_stored: str
    checksum_computed: str
    checksum_valid: bool
    player_name: str
    horse_name: str
    total_time_ticks: int
    stage_hint: str


@dataclass
class Sector:
    offset: int
    name: str
    data_version_raw: int
    data_version_le: int
    data_version_be: int
    data_version_valid: bool
    outer_checksum_valid: bool
    outer_word_order: str
    quest_logs: list[QuestLog] = field(default_factory=list)


@dataclass
class GciFile:
    path: str
    size: int
    size_valid: bool
    game_code: str
    maker_code: str
    filename: str
    block_count: int
    expected_size: int
    banner_format: int
    icon_format: int
    icon_speed: int
    icon_addr: int
    comment_addr: int
    sectors: list[Sector] = field(default_factory=list)


def _banner_format(byte: int) -> int:
    return byte & 0x03


def _icon_anim(byte: int) -> int:
    return byte & 0x04


def _icon_field(value: int, index: int) -> int:
    return (value >> (2 * index)) & 0x03


def checksum_quest_log(data: bytes) -> int:
    """`mDoMemCdRWm_CalcCheckSumGameData()`, byte-wise, u32 accumulators.

    `low += ~x` with `x` a promoted unsigned char: the complement is the 32-bit
    `~x`, not `0xFF - x`. Those differ, and only this one reproduces the value the
    game stores.
    """
    high = 0
    low = 0
    for x in data:
        high = (high + x) & 0xFFFFFFFF
        low = (low + (~x & 0xFFFFFFFF)) & 0xFFFFFFFF
    return (high << 32) | low


def checksum_sector(data: bytes, word_order: str = "little") -> int:
    """`mDoMemCdRWm_CalcCheckSum()`, word-wise, **u16** accumulators.

    The C code declares `u16 high, low`, so both wrap at 65536 before being packed as
    `high << 16 | low`. Using u32 accumulators yields a different value; measured on a
    real file rather than assumed.
    """
    high = 0
    low = 0
    for i in range(len(data) // 2):
        word = int.from_bytes(data[2 * i:2 * i + 2], word_order)
        high = (high + word) & 0xFFFF
        low = (low + ((~word) & 0xFFFF)) & 0xFFFF
    return (high << 16) | low


def _parse_c_string(raw: bytes) -> str:
    return raw.split(b"\x00", 1)[0].decode("ascii", "replace")


def parse_sector(blob: bytes, offset: int, name: str) -> Sector:
    data_version_raw = struct.unpack_from(">I", blob, 4)[0]
    payload = blob[8:8 + SAVEDATA_SIZE * SAVEDATA_NUM]

    # Two INDEPENDENT conventions, and conflating them reports a false mismatch (it did,
    # once). The C code reads its words through `BE(u16)*`, so on a little-endian host it
    # byte-swaps each word: words are effectively big-endian. The u32 result is then
    # assigned to a plain `u32 checksum`, so it lands in **host** byte order. Search both
    # axes and report the pair that reproduces the stored bytes.
    #
    # This value is informational only: `mDoMemCdRWm_Restore()` never reads it. Only the
    # per-quest-log u64 checksums gate a load, and `mDoMemCdRWm_Store()` compares this one
    # against a read-back of what it just wrote.
    stored_outer_bytes = blob[len(blob) - 4:]
    matching = []
    for word_order in ("big", "little"):
        computed = checksum_sector(blob[:len(blob) - 4], word_order)
        for byte_order in ("little", "big"):
            if computed.to_bytes(4, byte_order) == stored_outer_bytes:
                matching.append(f"{word_order}-word/{byte_order}-store")
    outer_valid = bool(matching)
    outer_order = matching[0] if matching else "none"

    logs = []
    for i in range(SAVEDATA_NUM):
        seg = payload[i * SAVEDATA_SIZE:(i + 1) * SAVEDATA_SIZE]
        stored = struct.unpack_from(">Q", seg, SAVEDATA_SIZE - 8)[0]
        computed = checksum_quest_log(seg[:SAVEDATA_SIZE - 8])
        stage = seg.find(b"F_SP")
        logs.append(
            QuestLog(
                index=i + 1,
                checksum_stored=f"{stored:016x}",
                checksum_computed=f"{computed:016x}",
                checksum_valid=stored == computed,
                player_name=_parse_c_string(seg[PLAYER_NAME_OFFSET:PLAYER_NAME_OFFSET + PLAYER_NAME_LEN]),
                horse_name=_parse_c_string(seg[HORSE_NAME_OFFSET:HORSE_NAME_OFFSET + PLAYER_NAME_LEN]),
                total_time_ticks=struct.unpack_from(">q", seg, PLAYER_INFO_OFFSET + 0x08)[0],
                stage_hint=_parse_c_string(seg[stage:stage + 8]) if stage >= 0 else "",
            )
        )

    return Sector(
        offset=offset,
        name=name,
        data_version_raw=data_version_raw,
        data_version_le=int.from_bytes(blob[4:8], "little"),
        data_version_be=int.from_bytes(blob[4:8], "big"),
        data_version_valid=SAVEDATA_VERSION in (
            int.from_bytes(blob[4:8], "little"), int.from_bytes(blob[4:8], "big")
        ),
        outer_checksum_valid=outer_valid,
        outer_word_order=outer_order,
        quest_logs=logs,
    )


def parse_gci(path: Path) -> GciFile:
    raw = path.read_bytes()
    if len(raw) < HEADER_SIZE:
        raise ValueError(f"{path}: {len(raw)} bytes is smaller than the 64-byte header")

    block_count = struct.unpack_from(">H", raw, 0x38)[0]
    expected = HEADER_SIZE + block_count * BLOCK_SIZE
    banner = raw[0x07]

    result = GciFile(
        path=str(path),
        size=len(raw),
        size_valid=len(raw) == expected,
        game_code=raw[0x00:0x04].decode("ascii", "replace"),
        maker_code=raw[0x04:0x06].decode("ascii", "replace"),
        filename=_parse_c_string(raw[0x08:0x28]),
        block_count=block_count,
        expected_size=expected,
        banner_format=banner,
        icon_format=struct.unpack_from(">H", raw, 0x30)[0],
        icon_speed=struct.unpack_from(">H", raw, 0x32)[0],
        icon_addr=struct.unpack_from(">I", raw, 0x2C)[0],
        comment_addr=struct.unpack_from(">I", raw, 0x3C)[0],
    )

    for offset in SAVE_SECTOR_OFFSETS:
        start = HEADER_SIZE + offset
        end = start + BLOCK_SIZE
        if end > len(raw):
            continue
        name = "primary" if offset == 0x4000 else "backup"
        result.sectors.append(parse_sector(raw[start:end], offset, name))

    return result


def describe(gci: GciFile) -> list[str]:
    lines = []
    lines.append(f"{gci.path}")
    lines.append(
        f"  header: {gci.game_code}-{gci.maker_code} name={gci.filename!r} "
        f"blocks={gci.block_count}"
    )
    lines.append(
        f"  size:   {gci.size} bytes, expected {gci.expected_size} "
        f"({'ok' if gci.size_valid else 'MISMATCH'})"
    )
    lines.append(
        f"  status: banner={_banner_format(gci.banner_format)} "
        f"anim={_icon_anim(gci.banner_format)} iconFormat={gci.icon_format:04x} "
        f"iconSpeed={gci.icon_speed:04x} iconAddr={gci.icon_addr:#x} "
        f"commentAddr={gci.comment_addr:#x}"
    )

    if not gci.sectors:
        lines.append("  no save sectors present")
        return lines

    for sector in gci.sectors:
        version = "ok" if sector.data_version_valid else "MISMATCH"
        lines.append(
            f"  {sector.name} @{sector.offset:#06x}: "
            f"dataVersion le={sector.data_version_le} be={sector.data_version_be} ({version})"
        )
        outer = "ok" if sector.outer_checksum_valid else "not reproduced"
        order = f"({sector.outer_word_order})" if sector.outer_checksum_valid else "(informational, not load-gating)"
        lines.append(f"    sector checksum: {outer} {order}")
        for log in sector.quest_logs:
            state = "valid" if log.checksum_valid else "CORRUPT"
            # The label the game shows is decided by dFile_info_c::setSaveData():
            # corrupt -> "This Quest Log is Corrupted"; valid but unnamed -> "New Quest Log".
            if not log.checksum_valid:
                shown = "This Quest Log is Corrupted"
            elif not log.player_name:
                shown = "New Quest Log"
            else:
                shown = log.player_name
            lines.append(
                f"    slot {log.index}: checksum {state}  name={log.player_name!r} "
                f"horse={log.horse_name!r} time={log.total_time_ticks} "
                f"stage={log.stage_hint!r}  displays={shown!r}"
            )
    return lines


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Validate Twilight Princess .gci saves")
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args(argv)

    failures = 0
    parsed = []
    for path in args.files:
        try:
            gci = parse_gci(path)
        except ValueError as exc:
            print(str(exc), file=sys.stderr)   # parse_gci already names the path
            failures += 1
            continue
        except OSError as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            failures += 1
            continue
        parsed.append(gci)
        if not gci.size_valid:
            failures += 1
        # Only the data version and the per-quest-log checksums gate a load. The sector's
        # own u32 is not consulted by `mDoMemCdRWm_Restore()`, so a mismatch there is
        # reported but does not fail the file.
        for sector in gci.sectors:
            if not sector.data_version_valid:
                failures += 1
            failures += sum(1 for log in sector.quest_logs if not log.checksum_valid)

    if args.json:
        print(json.dumps([asdict(g) for g in parsed], indent=2))
    else:
        for gci in parsed:
            print("\n".join(describe(gci)))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
