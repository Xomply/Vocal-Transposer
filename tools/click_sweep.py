#!/usr/bin/env python3
"""
click_sweep.py — the click regression, as one command.

WHY A DRIVER. Continuity defects are the class of bug that comes back: every new mode,
policy or engine adds another place where the signal path can be switched instead of
faded, and each one is silent until somebody listens on headphones. So the count has to be
cheap enough to run after every change, on fixed material, at fixed intervals.

The grid is deliberately small — two recordings, two engines, five intervals — because the
point is a number you re-run, not a study. Both recordings matter and neither substitutes
for the other: `sustained` has steady pitch and few voicing transitions, `melody` has
moving pitch and many, and BUGS.md VH-002 exists precisely because the first was clean
while the second was not.

READ THE `edge` COLUMN, NOT ONLY THE TOTAL. A fix that reduces the total while leaving
`edge` untouched has changed something else. See tools/click_probe.py for what the buckets
mean and for the pulse-locked correction, which is why the totals here are lower than the
raw counts in BUGS.md VH-002.

usage:
  python3 tools/click_sweep.py ./build/vh_trace .            # writes a table to stdout
  python3 tools/click_sweep.py ./build/vh_trace . --json out.json
  python3 tools/click_sweep.py ./build/vh_trace . --compare before.json
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import click_probe  # noqa: E402

SOURCES = ["sustained", "melody"]
ENGINES = ["psola", "granular"]
INTERVALS = [-12, -5, 3, 7, 12]


def run(trace_bin, audio_dir, workdir):
    rows = []
    for src in SOURCES:
        dry = os.path.join(audio_dir, f"{src}_dry.wav")
        if not os.path.exists(dry):
            print(f"skipping {src}: {dry} not found (run tools/fetch_test_audio.sh)",
                  file=sys.stderr)
            continue
        for eng in ENGINES:
            for st in INTERVALS:
                wav = os.path.join(workdir, f"{src}_{eng}_{st}.wav")
                csv = os.path.join(workdir, f"{src}_{eng}_{st}.csv")
                subprocess.run([trace_bin, dry, wav, csv, eng, str(st)], check=True)

                x, sr = click_probe.read_wav(wav)
                clicks = click_probe.find_clicks(x, sr)
                buckets, _ = click_probe.attribute(
                    clicks, sr, click_probe.load_trace(csv), ratio=2.0 ** (st / 12.0))
                rows.append({"source": src, "engine": eng, "semitones": st, **buckets})
    return rows


def key(r):
    return f"{r['source']}/{r['engine']}/{r['semitones']:+d}"


def table(rows, before=None):
    prev = {key(r): r for r in (before or [])}
    print("%-26s %8s %8s %8s %8s %8s" %
          ("source/engine/interval", "isolated", "edge", "chatter", "f0jump", "steady"))
    print("-" * 72)
    tot = 0
    tot_prev = 0
    for r in rows:
        k = key(r)
        delta = ""
        if k in prev:
            d = r["isolated"] - prev[k]["isolated"]
            delta = "   %+d" % d
            tot_prev += prev[k]["isolated"]
        tot += r["isolated"]
        print("%-26s %8d %8d %8d %8d %8d%s" %
              (k, r["isolated"], r["at_voicing_edge"], r["voicing_chatter"],
               r["f0_jump"], r["steady"], delta))
    print("-" * 72)
    if prev:
        print("TOTAL isolated clicks: %d   (was %d)" % (tot, tot_prev))
    else:
        print("TOTAL isolated clicks: %d" % tot)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("trace_bin")
    ap.add_argument("audio_dir")
    ap.add_argument("--json", help="write results here, for use as a later --compare")
    ap.add_argument("--compare", help="a previous --json, to show deltas")
    ap.add_argument("--keep", help="keep renders in this directory instead of a temp one")
    args = ap.parse_args()

    workdir = args.keep or tempfile.mkdtemp(prefix="clicksweep-")
    os.makedirs(workdir, exist_ok=True)
    rows = run(args.trace_bin, args.audio_dir, workdir)

    before = json.load(open(args.compare)) if args.compare else None
    table(rows, before)

    if args.json:
        json.dump(rows, open(args.json, "w"), indent=1)


if __name__ == "__main__":
    main()
