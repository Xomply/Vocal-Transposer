#!/usr/bin/env python3
"""
click_probe.py — find the clicks, then say WHAT THE ENGINE WAS DOING when each one fired.

WHY THIS EXISTS. BUGS.md VH-002 counts clicks and stops there, which is why it has sat in
Backlog with two undistinguished candidate causes. A count tells you the defect is real;
it does not tell you which line to open. The entry itself names the missing experiment:
"log the epoch series and the per-block period against click timestamps".

So this tool does the join. `vh_trace` writes one CSV row per block — F0, voicing, whether
the F0 was held, confidence, the epoch interval, and the passthrough decision derived from
exactly the expression the shifters use. This reads the rendered WAV, finds the clicks, and
reports which state each click landed in.

THE DETECTOR. A click is a step discontinuity, and a step is broadband: |x[n] - x[n-1]|
jumps far above what the surrounding waveform does. So: first difference, compared against
a LOCAL median (a rolling window, not a global one — a global threshold finds only the
clicks in the loud parts and calls the quiet ones clean). VH-002 used 12x the local median
and that threshold is kept, so numbers here are comparable with the ones already in the
ledger.

THE FALSE-POSITIVE CLASS, and the two things that were tried.
At large DOWNWARD shifts the output is a train of isolated glottal pulses with real silence
between them — that is what the VH-001 fix restored, and VOICE-MODEL.md §4 argues it is
what a low voice *is*. A rolling median taken across mostly-silence is tiny, so every
pulse's leading edge clears 12x it and scores as a click. At -12 st on the melody that was
89 of 184 "clicks", every one of them the shifter working correctly: checked by hand, the
flagged instants have max|dx| over the local peak of 0.02-0.21, i.e. smooth band-limited
rises with no step in them at all.

  REJECTED: an absolute step-ness test on its own (|dx| over local peak). Dry melody scores
  479 samples above 0.4, because tambura, consonants and HF content genuinely contain fast
  slopes. As a sole criterion it does not discriminate.

  ADOPTED: the median-outlier test AND a step-ratio gate, which is the same quantity used
  as a second condition rather than a first. The gate has a derivation rather than a
  tuning: a sinusoid of amplitude A at frequency f has max|dx| = 2A sin(pi f / fs), so a
  ratio of 0.35 at 48 kHz means the slew implies dominant energy above ~2.7 kHz relative to
  the local peak. Voiced waveforms put their energy an octave and more below that; a step
  is broadband by definition. Fricatives are broadband too, which is exactly why the
  median-outlier condition is retained alongside it — the gate alone would flag them.

Validated against the dry material, which is the only control that matters here: both dry
files score 0-1 with the gate on, while the pre-fix renders still score 15 (+7 st) and 16
(-12 st). The correction removes the false positives without touching the defect.

`pulse_locked` is still reported, because it names the mechanism that caused the false
positives and is worth seeing when it fires. With the gate on it should be near zero.

WHAT THE DETECTOR CANNOT DO, stated so nobody over-reads the output:

  - It cannot tell a click from a genuinely fast attack. A plosive release IS a step in the
    source material. That is why `--dry` exists: run the detector on the dry file too as a
    control.
  - It counts SAMPLE-LEVEL steps. A discontinuity smeared over 20 samples by an
    interpolator is real and audible and will score low here. Absence of outliers is not
    proof of continuity.

READING THE ATTRIBUTION TABLE. Each click is bucketed by the state of the block it landed
in, and the buckets are chosen to separate the hypotheses:

  at_voicing_edge   the block is within +/-`edge` ms of a voiced<->unvoiced transition.
                    Points at a HARD SWITCH between the shifted path and passthrough.
  voicing_chatter   the voicing decision flipped more than twice within 30 ms around it.
                    Points at the DECISION being unstable, not the switch being abrupt.
  f0_jump           |dF0| over the surrounding blocks exceeds `--f0-jump` cents.
                    Points at the tracker, at synthesis spacing, or at the epoch relock.
  epoch_irregular   the epoch interval departs from the period by more than 25%.
                    Points at the phase reference.
  steady            none of the above. Points at something inside a shifter that this
                    tool cannot see, and means the diagnosis is not finished.

A click can be in more than one bucket; the table is not a partition, and pretending it
were would hide the correlation that matters.

usage:
  python3 tools/click_probe.py wet.wav --trace wet.csv --dry dry.wav
  python3 tools/click_probe.py wet.wav --json
"""

import argparse
import csv
import json
import sys
import wave

import numpy as np


def read_wav(path):
    with wave.open(path, "rb") as w:
        n, sr = w.getnframes(), w.getframerate()
        raw = w.readframes(n)
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    return x, float(sr)


def rolling_median(v, win):
    """Median in a sliding window, decimated then held. Exact per-sample rolling median of
    a 240k-sample array is slow and the extra resolution buys nothing: the threshold only
    has to follow the ENVELOPE of the waveform, which moves on a syllable timescale."""
    step = max(1, win // 4)
    idx = np.arange(0, len(v), step)
    med = np.array([np.median(v[max(0, i - win): i + win]) + 1e-12 for i in idx])
    return np.interp(np.arange(len(v)), idx, med)


def rolling_peak(x, win):
    """Peak |x| in a sliding window, decimated then interpolated. Same shape of
    approximation as rolling_median and for the same reason."""
    step = max(1, win // 4)
    idx = np.arange(0, len(x), step)
    pk = np.array([np.max(np.abs(x[max(0, i - win): i + win])) + 1e-9 for i in idx])
    return np.interp(np.arange(len(x)), idx, pk)


def find_clicks(x, sr, factor=12.0, win_ms=20.0, min_gap_ms=3.0, step_ratio=0.35):
    """Return sample indices of events that are BOTH first-difference outliers AND fast
    enough to imply a step. See the module docstring for why it takes both.

    min_gap collapses a single click into one event. A step usually produces two or three
    consecutive large differences (the step itself plus the interpolator's ringing), and
    counting those separately inflates every number by a factor that varies with the
    interpolator rather than with the defect."""
    d = np.abs(np.diff(x))
    if len(d) == 0:
        return np.array([], dtype=int)
    win = max(64, int(win_ms * 0.001 * sr))
    thresh = rolling_median(d, win) * factor
    peak = rolling_peak(x, max(64, int(15.0 * 0.001 * sr)))[: len(d)]

    # An absolute floor, so digital silence does not make every dither-level wobble an
    # outlier: below -60 dBFS a step is not audible as a click anyway.
    hits = np.where((d > thresh) & (d > 1e-3) & (d > step_ratio * peak))[0]
    if len(hits) == 0:
        return hits

    gap = int(min_gap_ms * 0.001 * sr)
    keep = [hits[0]]
    for h in hits[1:]:
        if h - keep[-1] > gap:
            keep.append(h)
    return np.array(keep, dtype=int)


def load_trace(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            rows.append({k: float(v) for k, v in r.items()})
    return rows


def pulse_locked(clicks, sr, trace, ratio, tol=0.22):
    """True for events that have a neighbour one OUTPUT PERIOD away.

    The output period is the traced SOURCE period divided by the shift ratio, which is the
    spacing PSOLA emits grains at. An isolated discontinuity has no such neighbour; a train
    of correctly-emitted glottal pulses is nothing but such neighbours."""
    out = np.zeros(len(clicks), dtype=bool)
    if not trace or ratio is None or ratio <= 0 or len(clicks) < 2:
        return out
    t = np.array([r["time_s"] for r in trace])
    period = np.array([r["period_samples"] for r in trace])
    ct = clicks / sr
    for i, c in enumerate(ct):
        j = int(np.clip(np.searchsorted(t, c), 0, len(t) - 1))
        if period[j] < 8.0:
            continue
        outp = (period[j] / ratio) / sr          # seconds between output pulses
        gaps = np.abs(np.abs(ct - c) - outp)
        gaps[i] = 1e9
        if np.min(gaps) < tol * outp:
            out[i] = True
    return out


def attribute(clicks, sr, trace, edge_ms=12.0, f0_jump_cents=60.0, ratio=None):
    """Bucket every click by the analyser state around the block it landed in."""
    if not trace:
        return None
    locked = pulse_locked(clicks, sr, trace, ratio)

    t = np.array([r["time_s"] for r in trace])
    voiced = np.array([r["voiced"] for r in trace])
    passthru = np.array([r["passthrough"] for r in trace])
    f0 = np.array([r["f0_hz"] for r in trace])
    epint = np.array([r["epoch_interval"] for r in trace])
    period = np.array([r["period_samples"] for r in trace])

    hop = float(np.median(np.diff(t))) if len(t) > 1 else 1.0
    edge_blocks = max(1, int(edge_ms * 0.001 / hop))
    chat_blocks = max(2, int(0.030 / hop))

    # Blocks where the passthrough decision changes. This is the switch the shifters
    # actually make, so it is the one to correlate against — `voiced` alone would miss the
    # period<8 and epochCount==0 arms of the same expression.
    edges = np.zeros(len(t), dtype=bool)
    flip = np.where(np.diff(passthru) != 0)[0]
    for i in flip:
        edges[max(0, i - edge_blocks): i + edge_blocks + 1] = True

    buckets = {"isolated": 0, "pulse_locked": 0, "at_voicing_edge": 0, "voicing_chatter": 0,
               "f0_jump": 0, "epoch_irregular": 0, "steady": 0}
    detail = []

    for n, c in enumerate(clicks):
        ct = c / sr
        i = int(np.clip(np.searchsorted(t, ct), 0, len(t) - 1))
        lo, hi = max(0, i - chat_blocks), min(len(t), i + chat_blocks)

        if locked[n]:
            buckets["pulse_locked"] += 1
            detail.append({"time_s": round(ct, 4), "tags": ["pulse_locked"],
                           "f0": round(float(f0[i]), 1), "voiced": bool(voiced[i])})
            continue
        buckets["isolated"] += 1

        tags = []
        if edges[i]:
            tags.append("at_voicing_edge")
        if np.count_nonzero(np.diff(passthru[lo:hi]) != 0) > 2:
            tags.append("voicing_chatter")

        seg = f0[lo:hi]
        seg = seg[seg > 1.0]
        if len(seg) > 1:
            cents = 1200.0 * np.abs(np.log2(seg.max() / seg.min()))
            if cents > f0_jump_cents:
                tags.append("f0_jump")

        if period[i] > 8.0 and epint[i] > 0.0:
            if abs(epint[i] - period[i]) / period[i] > 0.25:
                tags.append("epoch_irregular")

        if not tags:
            tags.append("steady")
        for tag in tags:
            buckets[tag] += 1
        detail.append({"time_s": round(ct, 4), "tags": tags,
                       "f0": round(float(f0[i]), 1), "voiced": bool(voiced[i])})

    return buckets, detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wet")
    ap.add_argument("--trace", help="CSV from vh_trace")
    ap.add_argument("--dry", help="dry file, scored the same way as a control")
    ap.add_argument("--factor", type=float, default=12.0)
    ap.add_argument("--ratio", type=float,
                    help="shift ratio, so the output period is known and pulse edges "
                         "can be told from isolated discontinuities")
    ap.add_argument("--f0-jump", type=float, default=60.0)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--list", action="store_true", help="print every click")
    args = ap.parse_args()

    x, sr = read_wav(args.wet)
    clicks = find_clicks(x, sr, args.factor)

    out = {"file": args.wet, "clicks": len(clicks), "seconds": len(x) / sr}

    if args.dry:
        d, sr2 = read_wav(args.dry)
        if sr2 != sr:
            raise SystemExit("sample rate mismatch between dry and wet")
        out["clicks_dry"] = len(find_clicks(d, sr, args.factor))

    detail = None
    if args.trace:
        res = attribute(clicks, sr, load_trace(args.trace),
                        f0_jump_cents=args.f0_jump, ratio=args.ratio)
        if res:
            out["buckets"], detail = res

    if args.json:
        json.dump(out, sys.stdout)
        print()
        return

    head = out["clicks"]
    if "buckets" in out:
        head = out["buckets"]["isolated"]
        print("%-28s %4d isolated clicks in %.1f s  (%d pulse-locked, not counted)"
              % (args.wet.split("/")[-1], head, out["seconds"], out["buckets"]["pulse_locked"]))
    else:
        print("%-28s %4d clicks in %.1f s" % (args.wet.split("/")[-1], head, out["seconds"]))
    if "clicks_dry" in out:
        print("%-28s %4d clicks  (control: the source material's own steps)"
              % ("dry", out["clicks_dry"]))
    if "buckets" in out:
        print("  attribution of the isolated ones (a click may carry more than one tag):")
        for k, v in out["buckets"].items():
            if k in ("isolated", "pulse_locked"):
                continue
            print("    %-18s %4d" % (k, v))
    if args.list and detail:
        for d in detail:
            print("    %7.3f s  f0 %6.1f  %s" % (d["time_s"], d["f0"], ",".join(d["tags"])))


if __name__ == "__main__":
    main()
