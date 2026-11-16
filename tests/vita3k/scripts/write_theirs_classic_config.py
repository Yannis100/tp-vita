#!/usr/bin/env python3
"""Write capitanbarbacoa's "Dusklight PSVita" config.json as the Classic preset, non-interactively.

WHY THIS EXISTS (2026-11-11)
The preset modal ("Welcome to Dusklight" / Classic / Dusklight) would not take a key press in
Vita3K: `hold Left` moved the selection, but every `X` press left the modal pixel-identical, across
two runs and both tap and 1 s hold. Rather than keep guessing at RmlUi's navigation, this writes the
same state the modal's Confirm handler writes. That handler is `PresetWindow` in
`dusklight/src/dusk/ui/preset.cpp`: on `NavCommand::Confirm` it calls `applyPresetClassic()` and then
`getSettings().backend.wasPresetChosen.setValue(true); config::save();`.

`config.json` is a flat dump of every registered ConfigVar (`config::save()`,
`dusklight/src/dusk/config.cpp:538-567`), so writing the same keys produces the same result. The
values below are a transcription of `applyPresetClassic()` (preset.cpp:13-26), not a guess:

    lockAspectRatio              true      bloomMode                    Classic = 1
    depthOfFieldMode             Classic = 1
    enableAchievementToasts      false     enableControllerToasts       false
    internalResolutionScale      1         shadowResolutionMultiplier   1
    hideTvSettingsScreen         false     menuScalingMode              GameCube = 0
    enableMenuPointer            false

`AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT)` is the remaining line of applyPresetClassic(); it is
runtime-only and not a config var. The viewport it requests is the one our port already uses
(HANDOVER §3.5 / §5.2), so the two builds are compared at the same geometry.

Enums are written as integers because that is how `dumpToJson` serialises them
(`dusk/settings.h:13-23`, `:64-68`).

`backend.wasPresetChosen: true` suppresses the modal (`m_Do_main.cpp:1036`).
`backend.cardFileType: 0` is CARD_GCIFOLDER, the default, i.e. the same GCI-folder card our port
reads, so both builds see `EUR/Card A/01-GZ2P-gczelda2.gci`.

Usage:
    write_theirs_classic_config.py <config.json> [--skip-prelaunch]

Prints the resulting JSON. Exit status 1 if an existing file cannot be parsed.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

# applyPresetClassic(), preset.cpp:13-26. Enums as ints, see the module docstring.
CLASSIC_PRESET = {
    "video.lockAspectRatio": True,
    "game.bloomMode": 1,                 # BloomMode::Classic
    "game.depthOfFieldMode": 1,          # DepthOfFieldMode::Classic
    "game.enableAchievementToasts": False,
    "game.enableControllerToasts": False,
    "game.internalResolutionScale": 1,
    "game.shadowResolutionMultiplier": 1,
    "game.hideTvSettingsScreen": False,
    "game.menuScalingMode": 0,           # MenuScaling::GameCube
    "game.enableMenuPointer": False,
}

# Not part of the preset. These make the run reproducible.
#
# backend.cardFileType: the enum is
#     typedef enum { CARD_RAWIMAGE, CARD_GCIFOLDER } CARDFileType;   (dolphin/card.h:19-22)
# so GCI folder is **1**, not 0. The default in settings.cpp:187 is CARD_GCIFOLDER, but the key is
# only written to config.json once something saves the config, and a missing key falls back to the
# *int zero*, i.e. CARD_RAWIMAGE. That is what happened on 2026-11-11: their build wrote
# MemoryCardA.EUR.raw (16 MiB) and never saw our EUR/Card A/*.gci. Setting 1 explicitly makes them read
# the same GCI our port reads.
BACKEND = {
    "backend.wasPresetChosen": True,     # m_Do_main.cpp:1036 -- no preset modal
    "backend.cardFileType": 1,           # CARD_GCIFOLDER -- the GCI folder our port also reads
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("config", type=Path, help="path to their ux0:data/Dusklight config.json")
    parser.add_argument(
        "--skip-prelaunch",
        action="store_true",
        help="also set backend.skipPreLaunchUI=true (boot straight into the game)",
    )
    parser.add_argument(
        "--no-iso-path",
        action="store_true",
        help="leave backend.isoPath alone instead of pointing it at ux0:data/dusklight/Zelda.iso",
    )
    args = parser.parse_args()

    config: dict = {}
    if args.config.exists():
        text = args.config.read_text(encoding="utf-8")
        if text.strip():
            try:
                config = json.loads(text)
            except json.JSONDecodeError as exc:
                print(f"{args.config}: not valid JSON: {exc}", file=sys.stderr)
                return 1
        if not isinstance(config, dict):
            print(f"{args.config}: top level is not an object", file=sys.stderr)
            return 1

    # Their Vita build's disc path lives under its own data dir (HANDOVER §7.4b). Keep an existing
    # value: it may already point somewhere that works.
    if not args.no_iso_path and not config.get("backend.isoPath"):
        config["backend.isoPath"] = "ux0:data/dusklight/Zelda.iso"

    config.update(CLASSIC_PRESET)
    config.update(BACKEND)
    if args.skip_prelaunch:
        config["backend.skipPreLaunchUI"] = True

    args.config.parent.mkdir(parents=True, exist_ok=True)
    args.config.write_text(json.dumps(config, indent=4), encoding="utf-8")
    print(json.dumps(config, indent=4))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
