#!/usr/bin/env python3
"""
model_sweep.py — render the voicing-model grid, measure it, and produce things to LISTEN to.

WHY A DRIVER RATHER THAN A SHELL LOOP. The experiment has three axes — source pitch, shift
interval, and which corrections are enabled — and the whole point of VOICE-MODEL.md is that
the axes are independent. A grid that is assembled by hand gets pruned by hand, and the
cell that would have falsified the hypothesis is the one that gets skipped.

THE QUESTIONS THIS ANSWERS

  1. Is downward quality bound by SOURCE PITCH rather than by shift ratio?
     (VOICE-MODEL.md C1.) Run the same intervals on 90, 242 and 814 Hz probe tones. If the
     prediction holds, the 90 Hz tone survives shifts that wreck the 814 Hz one.

  2. Does mu do what the model says, and what does it cost?
     mu should raise duty by exactly 1/mu, move the envelope, and NOT move the pitch.

  3. How much of VH-008's buzz is open quotient versus truncated ring?
     The `mu` and `mu+tilt` columns separate them: run one correction at a time.

WHAT IT WRITES

  <out>/wav/    every render, named source__interval__config.wav. This is the deliverable.
                The numbers below are for deciding what to listen to, not a substitute.
  <out>/png/    spectrogram and waveform comparisons, per source and interval, all configs
                side by side. The waveform view is the one that shows the gap.
  <out>/RESULTS-sweep.md   the table.

THE METRICS ARE NEVER READ ALONE. See tools/voice_probe.py: duty is meaningless without
pitch beside it, because reintroducing VH-001 drives duty to 1.0 while destroying the
function. Every row here prints all four.

usage:
  python3 tools/model_sweep.py ./build/vh_sweep sweepout/ probe_90hz.wav probe_242hz.wav
  python3 tools/model_sweep.py ./build/vh_sweep sweepout/ *.wav --no-png
"""

import argparse
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voice_probe  # noqa: E402

# engine, muStrength, tiltStrength.
#
# `hold` is the control and it must stay first: it is the pre-profile engine, bit-identical,
# and every claim below is a claim RELATIVE to it. `granular` is the other control -- it
# resamples, so it is what mu = ratio looks like, i.e. the far end of the exponent.
CONFIGS = [
    ("hold",     "psola",    0.0, 0.0),
    ("mu",       "psola",    1.0, 0.0),
    ("mu+tilt",  "psola",    1.0, 1.0),
    ("granular", "granular", 0.0, 0.0),
]

INTERVALS = [-24, -19, -12, -7, -3, 3, 7, 12, 19]


def render(binary, src, out, engine, semis, mu, tilt):
    r = subprocess.run([binary, src, out, engine, str(semis), str(mu), str(tilt)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("  render failed: %s" % r.stderr.strip(), file=sys.stderr)
        return False
    return True


def png_compare(paths, labels, title, outpath):
    """Spectrogram row + waveform row, all configs side by side.

    The waveform row is zoomed to ~4 output periods. That is deliberate: the spectrogram
    shows WHETHER the comb is dense, and only the waveform shows the SHAPE of the gap that
    makes it dense. VH-008 was found by ear and characterised on the waveform view; a
    spectrogram alone would have shown 'lots of harmonics' and left the cause open."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np

    n = len(paths)
    fig, axes = plt.subplots(2, n, figsize=(4.0 * n, 6.0), squeeze=False)
    for i, (p, lab) in enumerate(zip(paths, labels)):
        x, sr = voice_probe.read_wav(p)
        seg = voice_probe.steady_window(x, sr, 0.5)

        axes[0][i].specgram(seg, NFFT=1024, Fs=sr, noverlap=768, cmap="magma")
        axes[0][i].set_ylim(0, 6000)
        axes[0][i].set_title(lab, fontsize=10)
        if i == 0:
            axes[0][i].set_ylabel("Hz")

        f0 = voice_probe.estimate_f0(seg, sr)
        span = int(4 * sr / f0) if f0 > 20 else int(0.02 * sr)
        span = min(span, len(seg))
        mid = len(seg) // 2
        w = seg[mid: mid + span]
        axes[1][i].plot(np.arange(len(w)) / sr * 1000.0, w, linewidth=0.7)
        axes[1][i].set_xlabel("ms")
        axes[1][i].axhline(0, color="k", linewidth=0.4)
        if i == 0:
            axes[1][i].set_ylabel("amplitude")

    fig.suptitle(title, fontsize=11)
    fig.tight_layout()
    fig.savefig(outpath, dpi=90)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary", help="path to vh_sweep")
    ap.add_argument("outdir")
    ap.add_argument("sources", nargs="+")
    ap.add_argument("--intervals", type=int, nargs="+", default=INTERVALS)
    ap.add_argument("--no-png", action="store_true")
    args = ap.parse_args()

    wavdir = os.path.join(args.outdir, "wav")
    pngdir = os.path.join(args.outdir, "png")
    os.makedirs(wavdir, exist_ok=True)
    if not args.no_png:
        os.makedirs(pngdir, exist_ok=True)

    lines = ["# Voicing-model sweep", "",
             "Generated by `tools/model_sweep.py`. Read the columns together, never alone —",
             "duty is only interpretable next to cents, because reintroducing VH-001 drives",
             "duty to 1.0 while destroying the pitch. See `tools/voice_probe.py`.", "",
             "`hold` is the pre-profile engine and is the control for every other column.", ""]

    for src in args.sources:
        base = os.path.splitext(os.path.basename(src))[0]
        lines += ["## %s" % base, "",
                  "| interval | config | cents err | duty | env dist dB | contrast dB | level dB | peak |",
                  "|---|---|---|---|---|---|---|---|"]
        print("== %s" % base)

        for semis in args.intervals:
            ratio = 2.0 ** (semis / 12.0)
            paths, labels = [], []
            for name, engine, mu, tilt in CONFIGS:
                out = os.path.join(wavdir, "%s__%+03dst__%s.wav" % (base, semis, name))
                if not render(args.binary, src, out, engine, semis, mu, tilt):
                    continue
                r = voice_probe.probe(src, out, ratio)
                lines.append("| %+d st | %s | %+.0f | %.2f | %.1f | %.1f | %+.1f | %.2f |" % (
                    semis, name, r["cents_error"], r["duty"], r["envdist_db"],
                    r["envcontrast_wet"], r["level_db"], r["peak"]))
                paths.append(out)
                labels.append("%s %+d st" % (name, semis))
            print("  %+4d st  done" % semis)

            if not args.no_png and paths:
                png_compare(paths, labels, "%s  %+d st" % (base, semis),
                            os.path.join(pngdir, "%s__%+03dst.png" % (base, semis)))
        lines.append("")

    path = os.path.join(args.outdir, "RESULTS-sweep.md")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\nwrote %s" % path)
    if not args.no_png:
        print("wrote %s/*.png" % pngdir)
    print("listen to %s/*.wav" % wavdir)


if __name__ == "__main__":
    main()
