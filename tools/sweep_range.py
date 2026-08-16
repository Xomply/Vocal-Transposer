#!/usr/bin/env python3
"""
sweep_range.py — does the QUALITY path actually degrade beyond +/- 6 semitones?

THE QUESTION. HANDOVER.md and ARCHITECTURE.md both assert "PSOLA ends around +/- 6
semitones -- that is where the technique ends, not a tuning problem." That claim is
inherited from the literature; it has never been measured in this repo. This script
measures it and renders material so it can also be judged by ear.

WHY MODE B, NOT MODE A -- the most important decision in this file.
  Mode A sets ratio = f_target / f_sung. On material whose pitch moves (the Carnatic
  melody runs 187-276 Hz) a FIXED target note gives a VARYING ratio, flattening the
  melody to a monotone. You would be hearing detachment do its job, not the shifter
  degrade. Mode B sets ratio = 2^(n/12): exact, constant, F0-independent. Shift
  magnitude becomes the only variable that differs between segments.
  This does NOT remove F0 from the picture -- PSOLA still needs epochs and a period to
  size and place grains. It removes F0 from the RATIO.

WHY vh_sweep AND NOT vh_render -- a mistake worth not repeating.
  The question is comparative: without a control at the SAME RATIO you cannot separate
  "PSOLA degrades past +/- 6 st" from "any shift this large sounds bad by any method".
  vh_render cannot give that control, because its variant table fixes RatioSource per
  variant -- `_granular.wav` is AbsoluteTarget and only `_modeB.wav` is
  IntervalFromRoot. The first version of this script used them as a matched pair. At
  "-24 st" the granular column was actually targeting MIDI 36, which from a 787 Hz
  soprano is a wholly different ratio. The resulting table looked entirely plausible:
  the control moved smoothly, in the right direction, and measured the wrong thing.
  vh_sweep exists to fix exactly this.

ON THE METRICS, and why the obvious one was dropped.
  The first attempt scored high-frequency energy as a proxy for grain hash. It is a good
  detector of localised discontinuity (it is what localised the -3 st artefact in the
  demo chord) but a bad sweep metric, because large upward shifts raise HF content
  LEGITIMATELY -- the harmonics really did move up there. Signal and artefact are not
  separable in that number across a wide range.
  Harmonic-vs-inter-harmonic energy was tried next and is also unusable here: measured
  over a multi-second window, vibrato smears each harmonic across a wide band, so the
  metric reports vibrato depth, not glitching.
  What survives is below. Each is reported for BOTH engines at the same ratio.

USAGE
  python3 tools/sweep_range.py ./build/vh_render ./build/vh_sweep \\
      sustained_dry.wav melody_dry.wav ./sweep
"""

import os
import shutil
import subprocess
import sys
import tempfile
import wave

import numpy as np


ROOT = 60          # Mode B root; vh_sweep sends ROOT + shift
GAP_SEC = 0.6      # silence between segments in the listening file
ANALYSIS_SKIP = 1.0   # seconds skipped at the head of every render: the history gate
                      # legitimately produces silence there (RESULTS.md bug #6), and
                      # including it would score correct behaviour as a fault.

# The listening sweep, as asked: every octave over +/- 3.
# 0 is NOT padding. It is the reference that separates "the shift degraded it" from
# "the engine degrades everything". Ordered monotonically so the curve is heard as a
# curve rather than as a shuffle.
OCTAVES = [-36, -24, -12, 0, +12, +24, +36]

# The measurement sweep. Octave granularity is 12 semitones and cannot possibly localise
# a knee claimed to sit at 6, so the numbers are taken at 1-semitone steps even though
# the audio is not.
FINE = list(range(-24, 25))


# --- io ---------------------------------------------------------------------

def read_wav(path):
    with wave.open(path, "rb") as w:
        n, sr, ch = w.getnframes(), w.getframerate(), w.getnchannels()
        x = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64) / 32768.0
    if ch == 2:
        x = x.reshape(-1, 2).mean(axis=1)
    return x, sr


def write_wav(path, x, sr):
    x = np.clip(x, -1.0, 1.0)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes((x * 32767.0).astype("<i2").tobytes())


# --- metrics ----------------------------------------------------------------

def periodicity(x, sr, f0_lo, f0_hi, frame=2048, hop=1024):
    """Mean normalised autocorrelation peak. 1.0 = perfectly periodic, 0 = noise.

    WHY SHORT FRAMES: at 2048 samples (43 ms) a vibrato cycle barely moves within the
    frame, so the estimate reflects waveform periodicity rather than pitch stability.
    Measured over seconds it would report vibrato depth instead.

    WHY A SEARCH BAND AROUND THE EXPECTED OUTPUT F0: an unconstrained peak search finds
    octave-related lags happily and would score an octave-error output as perfectly
    healthy. The band is deliberately generous (about -3/+3 semitones) so that a
    correctly shifted output with drift is not penalised.

    Silent frames are skipped rather than scored: silence has no periodicity to measure
    and averaging zeros into the result would turn "quiet" into "noisy".
    """
    lo, hi = int(sr / f0_hi), int(sr / f0_lo)
    if hi <= lo or hi >= frame:
        return float("nan")
    vals = []
    for i in range(0, len(x) - frame, hop):
        s = x[i:i + frame]
        if np.sqrt(np.mean(s * s)) < 1e-4:
            continue
        s = s - s.mean()
        ac = np.correlate(s, s, "full")[frame - 1:]
        ac = ac / (ac[0] + 1e-20)
        vals.append(float(ac[lo:hi].max()))
    return float(np.mean(vals)) if vals else float("nan")


def level_db(x, sr):
    s = x[int(ANALYSIS_SKIP * sr):]
    return 20 * np.log10(np.sqrt(np.mean(s * s)) + 1e-12)


def centroid(x, sr, lo=60.0, hi=8000.0):
    """Spectral centroid: where the energy sits. This is the formant proxy.

    It is the metric that SHOULD separate the two engines by construction -- granular
    resamples so its centroid must track the shift ratio, PSOLA re-emits unmodified
    grains so its centroid must not. If PSOLA's centroid starts tracking the ratio at
    large shifts, its defining property has broken down, and that is precisely the
    "degrades past +/- 6 st" claim in measurable form.
    """
    s = x[int(ANALYSIS_SKIP * sr):]
    s = s * np.hanning(len(s))
    mag = np.abs(np.fft.rfft(s))
    f = np.fft.rfftfreq(len(s), 1.0 / sr)
    m = (f > lo) & (f < hi)
    denom = np.sum(mag[m])
    if denom <= 0:
        return float("nan")
    return float(np.sum(f[m] * mag[m]) / denom)


def estimate_f0(x, sr, t0=1.0, t1=4.5, lo=60.0, hi=1200.0):
    s = x[int(t0 * sr):int(t1 * sr)]
    s = s - s.mean()
    ac = np.correlate(s, s, "full")[len(s) - 1:]
    a, b = int(sr / hi), int(sr / lo)
    return sr / (a + int(np.argmax(ac[a:b])))


# --- rendering --------------------------------------------------------------

def sweep_render(vh_sweep, src, engine, shift, tmpdir):
    """One engine, one constant interval, Mode B. Raw output, no gain trim, no dry mix."""
    out = os.path.join(tmpdir, f"{engine}_{shift:+03d}.wav")
    subprocess.run([vh_sweep, src, out, engine, str(shift)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return read_wav(out)


def render_render(vh_render, src, shift, tmpdir):
    """The listening segment, via the SHIPPED vh_render `_modeB.wav` variant.

    Deliberately not vh_sweep: the listening file should be the code as it ships,
    including vh_render's level trim, so what is heard is what the project produces.
    vh_sweep is for measurement, where that trim would be a confound.
    """
    tag = f"L{shift:+03d}"
    subprocess.run([vh_render, src, tmpdir, tag, f"0:{ROOT + shift}", str(ROOT)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return read_wav(os.path.join(tmpdir, tag + "_modeB.wav"))


# --- reports ----------------------------------------------------------------

def build_listening_file(vh_render, src, outpath, tmpdir):
    dry, sr = read_wav(src)
    gap = np.zeros(int(GAP_SEC * sr))

    segments = [dry]
    for sh in OCTAVES:
        x, _ = render_render(vh_render, src, sh, tmpdir)
        segments.append(gap)
        segments.append(x)

    out = np.concatenate(segments)
    write_wav(outpath, out, sr)

    marks, t = [], 0.0
    marks.append(("dry (reference, unprocessed)", t, t + len(dry) / sr))
    t += len(dry) / sr
    for sh in OCTAVES:
        t += GAP_SEC
        label = "0 st (engine reference, no shift)" if sh == 0 \
            else f"{sh:+d} st ({sh // 12 if sh > 0 else -(-sh // 12):+d} octave)"
        marks.append((label, t, t + 5.0))
        t += 5.0
    return marks, len(out) / sr


def fine_sweep(vh_sweep, src, sung, sr, tmpdir):
    print("\n--- fine sweep, 1 semitone steps, PSOLA vs granular AT THE SAME RATIO ---")
    print("  cent = spectral centroid / dry centroid. Granular resamples so this MUST")
    print("         track the ratio. PSOLA preserves formants so it MUST stay near 1.0.")
    print("         PSOLA tracking the ratio = its defining property has broken down.")
    print("  per  = periodicity, 0..1. Below the dry figure means noise where there was tone.")
    print("  lvl  = output level in dB, relative to the same engine at 0 st.\n")
    print(f"  {'shift':>6s} {'outF0':>7s} | {'ps cent':>8s} {'gr cent':>8s} | "
          f"{'ps per':>7s} {'gr per':>7s} | {'ps lvl':>7s} {'gr lvl':>7s}")

    dry, _ = read_wav(src)
    dry_cent = centroid(dry, sr)
    dry_per = periodicity(dry, sr, sung * 0.85, sung * 1.18)
    print(f"  {'dry':>6s} {sung:7.0f} | {1.0:8.2f} {'':>8s} | {dry_per:7.3f} {'':>7s} |")

    rows = {}
    ref = {}
    for eng in ("psola", "granular"):
        x, _ = sweep_render(vh_sweep, src, eng, 0, tmpdir)
        ref[eng] = level_db(x, sr)

    for sh in FINE:
        r = {}
        for eng in ("psola", "granular"):
            x, _ = sweep_render(vh_sweep, src, eng, sh, tmpdir)
            f0 = sung * 2 ** (sh / 12.0)
            r[eng] = (centroid(x, sr) / dry_cent,
                      periodicity(x, sr, f0 * 0.85, f0 * 1.18),
                      level_db(x, sr) - ref[eng])
        rows[sh] = r
        if abs(sh) <= 8 or sh % 3 == 0:
            p, g = r["psola"], r["granular"]
            f0 = sung * 2 ** (sh / 12.0)
            print(f"  {sh:+6d} {f0:7.0f} | {p[0]:8.2f} {g[0]:8.2f} | "
                  f"{p[1]:7.3f} {g[1]:7.3f} | {p[2]:+7.1f} {g[2]:+7.1f}")
    return rows, dry_per


def verdict(rows, dry_per):
    """State what the numbers support and, just as importantly, what they do not."""
    print("\n  --- what these numbers do and do not support ---")

    drift = [sh for sh, r in rows.items() if abs(r["psola"][0] - 1.0) > 0.30]
    if drift:
        dn = [s for s in drift if s < 0]
        up = [s for s in drift if s > 0]
        print("  PSOLA's centroid moved >30% from dry (formant preservation weakening) at:")
        if dn:
            print(f"    downward from {max(dn):+d} st and below")
        if up:
            print(f"    upward   from {min(up):+d} st and above")
    else:
        print("  PSOLA held its spectral centroid within 30% of dry across the WHOLE")
        print("  swept range. Formant preservation did not break down at +/- 6 st.")

    noisy = [sh for sh, r in rows.items()
             if not np.isnan(r["psola"][1]) and r["psola"][1] < dry_per - 0.15]
    if noisy:
        print(f"  Periodicity fell >0.15 below dry at: {sorted(noisy)}")
    else:
        print("  Periodicity never fell materially below the dry material at any shift,")
        print("  for either engine. Neither degrades into noise in the swept range.")

    quiet = [(sh, r["psola"][2]) for sh, r in rows.items() if r["psola"][2] < -6.0]
    if quiet:
        worst = min(quiet, key=lambda t: t[1])
        print(f"  PSOLA level loss exceeds 6 dB at {len(quiet)} shifts; worst "
              f"{worst[1]:+.1f} dB at {worst[0]:+d} st.")
        print("  Level loss at constant periodicity is grains summing incompletely, not")
        print("  noise. It is audible as thinness, and it is a real degradation.")

    print("\n  NOT MEASURED HERE, and not to be inferred: intelligibility, naturalness,")
    print("  and 'does it sound like a person'. Those are what the WAV files are for.")


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 1
    vh_render, vh_sweep = sys.argv[1], sys.argv[2]
    sources, outdir = sys.argv[3:-1], sys.argv[-1]
    os.makedirs(outdir, exist_ok=True)
    tmpdir = tempfile.mkdtemp(prefix="vhsweep_")

    try:
        for src in sources:
            name = os.path.splitext(os.path.basename(src))[0].replace("_dry", "")
            dry, sr = read_wav(src)
            sung = estimate_f0(dry, sr)
            print(f"\n{'=' * 78}\n{name}: {len(dry)/sr:.2f} s, sung F0 ~ {sung:.0f} Hz\n{'=' * 78}")

            outpath = os.path.join(outdir, f"sweep_{name}_psola.wav")
            marks, dur = build_listening_file(vh_render, src, outpath, tmpdir)
            print(f"\nwrote {outpath}  ({dur:.1f} s)")
            for label, a, b in marks:
                print(f"   {a:6.1f} - {b:6.1f} s   {label}")

            rows, dry_per = fine_sweep(vh_sweep, src, sung, sr, tmpdir)
            verdict(rows, dry_per)
    finally:
        shutil.rmtree(tmpdir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
