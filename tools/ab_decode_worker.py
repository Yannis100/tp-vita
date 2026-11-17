#!/usr/bin/env python3
"""Compare two Vita3K runs of the SAME route where one had the decode worker on and one had it off.

    ab_decode_worker.py <log-with-worker> <log-without-worker>

Why this is a separate tool rather than a snippet: the obvious comparison is wrong, and it is wrong in
a way that produced a 70 % "improvement" here before it was caught.

  Matching on FRAME NUMBER is invalid. Game time in this port is wall-clock (dusk::game_clock samples
  the native clock), so the same frame number lands in a different scene in the two runs, and the
  windows differ in draw count by 2.6x. That comparison measured scene content, not the change.

  Matching on WORKLOAD is the valid one: pair each window with the window from the other run whose
  draw count is closest, and only if they are within a few percent. Draw count is the load signal this
  port already uses for exactly this reason (see the batching-survey comments in vita_gl_replay.cpp).

What it reports, and what it refuses to:
  * the decode share of the frame in each run (the thing the change is supposed to move), and
  * the frame-time delta on the matched pairs, with the spread, so a noisy result reads as noisy.

It does not claim a console number. Both runs are Vita3K, whose absolute times are not a Vita's.
"""
import glob
import os
import re
import statistics as st
import sys

WINDOW = re.compile(r"window @frame (\d+): 120 frames, (\d+) ms wall = ([\d.]+) ms/frame")
PHASES = re.compile(
    r"phases: decode ([\d.]+)% \| submit ([\d.]+)% \| pass ([\d.]+)% \| resolve ([\d.]+)% "
    r"\| other ([\d.]+)%; (\d+) draws"
)
OFFTHREAD = re.compile(r"decode worker: ([\d.]+)% of frame decoded off-thread")
DISABLED = re.compile(r"decode worker: DISABLED")


def parse(path):
    """-> list of windows, each a dict. `publish`-order: the phases line follows its window line."""
    windows = []
    cur = None
    for line in open(path, errors="replace"):
        m = WINDOW.search(line)
        if m:
            cur = {"frame": int(m.group(1)), "ms": float(m.group(3)), "draws": 0}
            windows.append(cur)
            continue
        if cur is None:
            continue
        m = PHASES.search(line)
        if m:
            cur.update(
                decode=float(m.group(1)),
                submit=float(m.group(2)),
                passing=float(m.group(3)),
                resolve=float(m.group(4)),
                other=float(m.group(5)),
                draws=int(m.group(6)),
            )
            continue
        m = OFFTHREAD.search(line)
        if m:
            cur["offthread"] = float(m.group(1))
    return [w for w in windows if w.get("draws")]


def newest_logs(directory, count):
    logs = sorted(glob.glob(os.path.join(directory, "dusklight-*.log")), key=os.path.getmtime)
    return logs[-count:]


def main(argv):
    if len(argv) == 3:
        on_path, off_path = argv[1], argv[2]
    elif len(argv) == 1:
        logs = newest_logs(
            os.path.expanduser(
                "~/psvita/tooling/vita3k-work/home/.local/share/Vita3K/Vita3K/ux0/data/"
                "TwilitRealm/Dusklight/logs"
            ),
            2,
        )
        if len(logs) < 2:
            print("need two runs; pass them explicitly", file=sys.stderr)
            return 2
        on_path, off_path = logs
    else:
        print(__doc__, file=sys.stderr)
        return 2

    on, off = parse(on_path), parse(off_path)
    if not on or not off:
        print("one of the logs has no phase windows; nothing to compare", file=sys.stderr)
        return 1

    print(f"worker ON : {os.path.basename(on_path)}  {len(on)} windows, frames {on[0]['frame']}..{on[-1]['frame']}")
    print(f"worker OFF: {os.path.basename(off_path)}  {len(off)} windows, frames {off[0]['frame']}..{off[-1]['frame']}")
    print()
    print(
        "draws/window  ON  {}..{}   OFF {}..{}".format(
            min(w["draws"] for w in on),
            max(w["draws"] for w in on),
            min(w["draws"] for w in off),
            max(w["draws"] for w in off),
        )
    )

    # Pair on workload, never on frame number: see the module docstring.
    pairs = []
    for a in on:
        near = [b for b in off if abs(b["draws"] - a["draws"]) <= 0.10 * max(a["draws"], 1) and a["ms"] > 0]
        if not near:
            continue
        b = min(near, key=lambda w: abs(w["draws"] - a["draws"]))
        pairs.append((a, b, 100.0 * (b["ms"] - a["ms"]) / b["ms"]))

    print()
    if not pairs:
        print("no window in the two runs has a comparable draw count; the runs did not reach the same")
        print("scene, so there is nothing to compare. Do not substitute a frame-number match.")
        return 1

    print(f"{'drawsON':>8} {'msON':>7} {'drawsOFF':>9} {'msOFF':>7} {'delta':>7} {'decON':>7} {'decOFF':>7} {'offthr':>7}")
    for a, b, delta in pairs:
        print(
            f"{a['draws']:>8} {a['ms']:>7.1f} {b['draws']:>9} {b['ms']:>7.1f} {delta:>+6.1f}% "
            f"{a['decode']:>7.1f} {b['decode']:>7.1f} {a.get('offthread', 0):>7.1f}"
        )

    deltas = [p[2] for p in pairs]
    print()
    print(f"n={len(deltas)}  median {st.median(deltas):+.1f}%  mean {st.mean(deltas):+.1f}%  "
          f"range {min(deltas):+.1f}..{max(deltas):+.1f}%")
    # The phase shares are the robust result: they are ratios within one run, so scene composition
    # cancels. The frame-time delta is not robust and is reported with its spread for that reason.
    print("decode share of frame: ON {:.1f}%  OFF {:.1f}%".format(
        st.mean([p[0]["decode"] for p in pairs]), st.mean([p[1]["decode"] for p in pairs])))
    print("submit share of frame: ON {:.1f}%  OFF {:.1f}%".format(
        st.mean([p[0]["submit"] for p in pairs]), st.mean([p[1]["submit"] for p in pairs])))
    print("pass   share of frame: ON {:.1f}%  OFF {:.1f}%".format(
        st.mean([p[0]["passing"] for p in pairs]), st.mean([p[1]["passing"] for p in pairs])))
    if max(deltas) - min(deltas) > abs(st.median(deltas)):
        print()
        print("NOTE: the frame-time spread is wider than the median, so the timing result is noisy and")
        print("should not be quoted as a speedup. The phase shares above are the stable measurement.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
