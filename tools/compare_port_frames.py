#!/usr/bin/env python3
"""Compare frames from two PS Vita Twilight Princess ports, and refuse when that is not valid.

WHY THIS EXISTS (2026-11-11)

Three Twilight Princess ports to the Vita are relevant:

    ours            Yannis100/tp-vita                 frames in tests/vita3k/screenshots/
    Dusklight PSVita capitanbarbacoa, itch.io          code not shared; VPK held locally
    DuskOnVita      NathanKneT/DuskOnVita              source only, NO release and NO VPK

The obvious experiment — put frames side by side and diff them — produces a number that looks
scientific and means nothing, because the ports were driven to DIFFERENT scenes. Measured before
this tool existed:

    ours(r1488_gameplay_ordon) vs theirs(nm_12_gameplay_a) : 90.9% of pixels differ
    theirs(nm_12_gameplay_a)   vs theirs(nm_14_gameplay_c) : 21.0% of pixels differ  <- same run

A 90% figure is not evidence of a rendering defect; it is evidence that the two captures show
different places in the game. Only a same-scene pair supports a claim about rendering.

So this tool does two jobs, in this order:

1. **Decide whether the pair is comparable at all.** It locates the letterboxed content box,
   fingerprints the content on a normalised 32x32 grid, and correlates the two fingerprints.
   Below `--min-correlation` (default 0.85) it reports NOT COMPARABLE and does not print a diff
   percentage. That is the whole point: a refusal is a result.

   **The detector has a measured blind spot, and `--assert-same-screen` is the answer to it
   (2026-11-12).** The fingerprint is a 32x32 *global* luminance grid, so it is dominated by the
   letterbox-adjacent dark regions and by the shared parchment board that every 2D menu in this
   game draws. Measured on real captures from this machine:

       verified same screen  ours(file select) vs sf_5_05(file select)  +0.538
       verified same screen  ours(name screen) vs nm_5(name screen)     +0.372  (nm_5 not top-4)
       KNOWN DIFFERENT       ours(attract loop) vs sf_5_05(file select) +0.533

   A same-screen pair scoring 0.538 while a different-scene pair scores 0.533 means **no threshold
   separates them**. Lowering `--min-correlation` to admit the true pair would equally admit the
   false one, which is worse than refusing: it would publish a scene change as a rendering defect,
   the exact error this tool exists to prevent. Alternative fingerprints (gradient, high-pass,
   row-profile, coverage) were tried and each ranked a *title screen* above the true file select, so
   none was shipped.

   Therefore the detector stays strict, and a human who has actually LOOKED at both frames can
   state the identity explicitly:

       --assert-same-screen "the file select, slot 1 highlighted, no save written"

   The reason is required, is echoed with the result, and is recorded in the JSON as
   `asserted_same_screen`. A measured result under an assertion is always labelled as such: the
   scene identity is a human claim, only the pixel difference is computed.

2. **Only then** measure, and measure the right region. Each port places the game view inside the
   same 960x544 surface but not at the same rectangle:

       ours (tp-vita)      738x544 at x=111   (HANDOVER §3: EFB 608x448 * 1.2143)
       DuskOnVita          724x544 at x=118   (their tests/gxm_state_test.cpp: `viewport(118,0,724,544)`)

   Comparing full frames would count the different letterbox widths as image error. The tool
   intersects the two content boxes and compares only the overlap, and reports the geometry on its
   own so a width difference is visible as a fact rather than smeared into a percentage.

3. **Third port.** DuskOnVita ships no VPK and publishes no release, so no capture of it can be made
   without a full source build (VitaSDK + their patch stack). `--port-status` prints this, so a
   report never implies a three-way comparison that did not happen.

Usage:
    compare_port_frames.py --port-status
    compare_port_frames.py <a.png> <b.png> [--min-correlation 0.85] [--json]
    compare_port_frames.py <a.png> <b.png> --assert-same-screen "the file select, slot 1"

Exit status:
    0  the pair is comparable and was measured
    1  the pair is NOT comparable (different scene) or could not be read
    2  usage error
"""
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, asdict
from pathlib import Path

try:
    import numpy as np
    from PIL import Image
except ImportError as exc:  # pragma: no cover - environment check
    print(f"needs Pillow and numpy: {exc}", file=sys.stderr)
    raise SystemExit(2)

# Known geometry, from each port's own source or measured output, not guessed.
VITA_SCREEN = (960, 544)
KNOWN_GEOMETRY = {
    "ours": {
        "viewport": "738x544 at x=111",
        "source": "tp-vita HANDOVER §3: EFB 608x448 scaled 1.2143 to 738x544, glViewport(111,0,738,544)",
    },
    "dusklight_psvita": {
        "viewport": "unknown (code not shared)",
        "source": "capitanbarbacoa, itch.io; measured content box 738x544 in its captures",
    },
    "duskonvita": {
        "viewport": "724x544 at x=118",
        "source": "NathanKneT/DuskOnVita tests/gxm_state_test.cpp: viewport(118,0,724,544)",
    },
}

PORT_STATUS = {
    "ours": "runnable here (build/tpv_vita-release.vpk, Vita3K harness)",
    "dusklight_psvita": "runnable here (Dusklight-1-1-0.vpk), code not shared",
    "duskonvita": "NOT runnable here: no release and no VPK in the repository; needs a full "
                  "source build with VitaSDK and their patch stack",
}


@dataclass
class Frame:
    path: str
    size: tuple[int, int]
    content_box: tuple[int, int, int, int]   # x0, y0, x1, y1 inclusive
    content_size: tuple[int, int]
    colours: int


@dataclass
class Comparison:
    a: Frame
    b: Frame
    correlation: float
    comparable: bool
    min_correlation: float
    overlap_box: tuple[int, int, int, int] | None = None
    differing_percent: float | None = None
    mean_abs_diff: float | None = None
    max_abs_diff: int | None = None
    asserted_same_screen: str | None = None
    note: str = ""


def _load(path: Path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)


def _content_box(a: np.ndarray) -> tuple[int, int, int, int] | None:
    """Bounding box of non-black pixels. The Vita surface is letterboxed inside the capture."""
    mask = a.any(axis=2)
    ys, xs = np.where(mask)
    if xs.size == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


def _fingerprint(a: np.ndarray, box: tuple[int, int, int, int]) -> np.ndarray:
    """32x32 normalised greyscale of the content, so scale and brightness cancel out.

    Normalising (subtract mean, divide by std) is deliberate: the question is 'is this the same
    picture', not 'is the exposure identical'. Exposure is measured separately, on the overlap.

    LIMIT, measured 2026-11-12: on this game's 2D menus the global grid cannot separate two
    different menus from one menu twice, because the parchment board dominates both. See the module
    docstring for the numbers. A human-asserted identity is the documented escape hatch.
    """
    x0, y0, x1, y1 = box
    crop = a[y0:y1 + 1, x0:x1 + 1]
    grey = Image.fromarray(crop.astype(np.uint8)).convert("L").resize((32, 32), Image.BILINEAR)
    v = np.asarray(grey, dtype=np.float32)
    return (v - v.mean()) / (v.std() + 1e-6)


def _frame(path: Path) -> Frame:
    a = _load(path)
    box = _content_box(a)
    if box is None:
        raise ValueError(f"{path}: no non-black pixels (a blank capture proves nothing)")
    x0, y0, x1, y1 = box
    colours = len(np.unique(np.asarray(Image.open(path).convert("RGB")).reshape(-1, 3), axis=0))
    return Frame(
        path=str(path),
        size=(a.shape[1], a.shape[0]),
        content_box=box,
        content_size=(x1 - x0 + 1, y1 - y0 + 1),
        colours=colours,
    )


def compare(a_path: Path, b_path: Path, min_correlation: float,
            assert_same_screen: str | None = None) -> Comparison:
    fa, fb = _frame(a_path), _frame(b_path)
    aa, ab = _load(a_path), _load(b_path)

    corr = float((_fingerprint(aa, fa.content_box) * _fingerprint(ab, fb.content_box)).mean())

    # A human assertion overrides the detector's verdict, but never silently: the reason is
    # required by the CLI and is echoed into the result and the JSON.
    asserted = bool(assert_same_screen)
    comparable = asserted or corr >= min_correlation

    result = Comparison(a=fa, b=fb, correlation=corr, comparable=comparable,
                        min_correlation=min_correlation,
                        asserted_same_screen=assert_same_screen)

    if not comparable:
        result.note = (
            f"NOT COMPARABLE: fingerprint correlation {corr:+.3f} is below {min_correlation:.2f}. "
            "The two captures do not show the same scene, so any pixel difference between them "
            "measures the scene change, not the renderer. Drive both ports to the same screen "
            "before comparing. If you have LOOKED at both frames and know they are the same screen, "
            're-run with --assert-same-screen "<what you see>" to measure anyway; see the module '
            "docstring for why the detector under-scores this game's menus."
        )
        return result

    # Same scene (detected, or asserted by a human): compare only where both content boxes
    # overlap, never the letterbox.
    x0 = max(fa.content_box[0], fb.content_box[0])
    y0 = max(fa.content_box[1], fb.content_box[1])
    x1 = min(fa.content_box[2], fb.content_box[2])
    y1 = min(fa.content_box[3], fb.content_box[3])
    if x1 <= x0 or y1 <= y0:
        result.comparable = False
        result.note = "content boxes do not overlap"
        return result

    d = np.abs(aa[y0:y1 + 1, x0:x1 + 1] - ab[y0:y1 + 1, x0:x1 + 1])
    result.overlap_box = (x0, y0, x1, y1)
    result.differing_percent = float(100.0 * (d.max(axis=2) > 8).mean())
    result.mean_abs_diff = float(d.mean())
    result.max_abs_diff = int(d.max())

    notes = []
    if asserted:
        notes.append(
            f"SCENE IDENTITY ASSERTED BY THE OPERATOR, NOT DETECTED: {assert_same_screen!r}. The "
            f"detector scored this pair {corr:+.3f}, below its {min_correlation:.2f} bar; the pixel "
            "difference below is computed, the 'same screen' claim is not. The button glyphs are "
            "expected to differ (GameCube B/A vs Vita X/Circle) and count as image error."
        )
    if fa.content_size != fb.content_size:
        notes.append(
            f"content sizes differ: {fa.content_size[0]}x{fa.content_size[1]} vs "
            f"{fb.content_size[0]}x{fb.content_size[1]}; only the "
            f"{x1 - x0 + 1}x{y1 - y0 + 1} overlap was measured. A viewport difference is geometry, "
            "not image error."
        )
    result.note = " ".join(notes)
    return result


def _describe(c: Comparison) -> list[str]:
    lines = []
    lines.append(f"  A: {c.a.path}")
    lines.append(f"     size={c.a.size[0]}x{c.a.size[1]} content={c.a.content_size[0]}x{c.a.content_size[1]}"
                 f" at {c.a.content_box[:2]} colours={c.a.colours}")
    lines.append(f"  B: {c.b.path}")
    lines.append(f"     size={c.b.size[0]}x{c.b.size[1]} content={c.b.content_size[0]}x{c.b.content_size[1]}"
                 f" at {c.b.content_box[:2]} colours={c.b.colours}")
    verdict = "asserted" if c.asserted_same_screen else "detected"
    lines.append(f"  fingerprint correlation: {c.correlation:+.3f} (threshold {c.min_correlation:.2f}, "
                 f"scene identity {verdict})")
    if c.comparable and c.overlap_box is not None:
        x0, y0, x1, y1 = c.overlap_box
        lines.append(f"  measured region: {x1 - x0 + 1}x{y1 - y0 + 1} at ({x0},{y0})")
        lines.append(f"  differing pixels: {c.differing_percent:.1f}%   "
                     f"mean|diff|={c.mean_abs_diff:.1f}   max={c.max_abs_diff}")
    if c.note:
        lines.append(f"  {c.note}")
    return lines


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description="Compare two Vita TP port frames, or refuse to")
    ap.add_argument("frames", nargs="*", type=Path)
    ap.add_argument("--min-correlation", type=float, default=0.85,
                    help="fingerprint correlation required to treat a pair as the same scene")
    ap.add_argument("--assert-same-screen", metavar="REASON", default=None,
                    help="measure even if the detector refuses, because you have looked at both "
                         "frames and know they show the same screen. The reason is required and is "
                         "printed with the result. Only the pixel difference is computed; the "
                         "'same screen' claim becomes yours.")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--port-status", action="store_true",
                    help="print which ports can actually be captured here")
    args = ap.parse_args(argv)

    if args.port_status:
        for name in ("ours", "dusklight_psvita", "duskonvita"):
            geo = KNOWN_GEOMETRY[name]
            print(f"{name}:")
            print(f"  capture:  {PORT_STATUS[name]}")
            print(f"  viewport: {geo['viewport']}")
            print(f"  basis:    {geo['source']}")
        return 0

    if len(args.frames) != 2:
        ap.error("give exactly two frames, or --port-status")

    if args.assert_same_screen is not None and not args.assert_same_screen.strip():
        ap.error("--assert-same-screen needs a reason saying what you see, not an empty string")

    try:
        c = compare(args.frames[0], args.frames[1], args.min_correlation,
                    args.assert_same_screen)
    except (OSError, ValueError) as exc:
        print(str(exc), file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(asdict(c), indent=2))
    else:
        print("\n".join(_describe(c)))
    return 0 if c.comparable else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
