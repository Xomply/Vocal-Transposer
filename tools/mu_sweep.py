#!/usr/bin/env python3
"""
mu_sweep.py — the k_mu tuning harness. Requested mu against ACHIEVED mu, plus the renders
to decide between them by ear.

WHAT THIS IS FOR. VOICE-MODEL.md §7 Q3 asks "is k_mu = 0.3 right by ear?" and notes that
the value is a fit to two population datapoints across data that has register breaks in it.
Answering that needs two things this repo did not have:

  1. Confirmation that the engine APPLIES the mu it computes. Until that is established,
     tuning k by ear is tuning a number whose effect is unverified — and the two are
     separable failures: `muFor()` could be right while `placeGrain`'s resampling is not.
     Column `mu_meas` against `mu_want` answers this, using tools/mu_probe.py.
  2. Renders across a k grid at fixed intervals, so the ear compares one variable.

HOW k IS SET FROM THE SHELL. vh_sweep exposes `muStrength`, which MULTIPLIES the profile's
kMu (0.30). So an effective exponent k is requested as muStrength = k / 0.30, and this
driver does that conversion. The endpoints are worth knowing by heart because they are the
two engines this project already has:

    k = 0.0   mu = 1          formants held         == plain PSOLA
    k = 0.3   mu = ratio^0.3  the derived default
    k = 1.0   mu = ratio      formants track pitch  == the granular engine

THE TILT IS OFF FOR MEASUREMENT AND ON FOR LISTENING, and they are separate renders.
A broadband tilt correlates with a frequency-axis translation, so leaving it on biases the
measured mu — the number would be reporting the tilt as if it were the warp. The listening
renders keep it on because that is what the instrument actually does.

READ `cents` BESIDE `mu_meas`, ALWAYS. mu must move the envelope and NOT move the pitch;
that independence is the entire claim of the grain-resampling approach (VOICE-MODEL.md §5).
A k that improves timbre while dragging the pitch has not improved anything. This is the
same discipline as tools/voice_probe.py and it exists for the same reason.

CHOOSE LOW-PITCHED MATERIAL. mu_probe cannot measure a soprano — too few harmonics sample
the envelope. It will warn; believe it.

usage:
  python3 tools/mu_sweep.py ./build/vh_sweep melody_dry.wav muout/
  python3 tools/mu_sweep.py ./build/vh_sweep melody_dry.wav muout/ --k 0 0.15 0.3 0.5 1.0
"""

import argparse
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mu_probe  # noqa: E402
import voice_probe  # noqa: E402

KMU_DEFAULT = 0.30          # VoicingProfile::kMu, the value muStrength multiplies
INTERVALS = [-24, -12, -7, 7, 12]
KS = [0.0, 0.15, 0.30, 0.50, 1.00]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("sweep_bin")
    ap.add_argument("source")
    ap.add_argument("outdir")
    ap.add_argument("--k", type=float, nargs="+", default=KS)
    ap.add_argument("--intervals", type=int, nargs="+", default=INTERVALS)
    args = ap.parse_args()

    wavdir = os.path.join(args.outdir, "wav")
    os.makedirs(wavdir, exist_ok=True)

    dry, sr = mu_probe.read_wav(args.source)
    d = mu_probe.steady_window(dry, sr)
    f0 = mu_probe.source_f0(d, sr)
    harmonics = 6000.0 / f0 if f0 > 0 else 0.0

    print("source %s   F0 ~%.0f Hz   %.1f harmonics below 6 kHz"
          % (os.path.basename(args.source), f0, harmonics))
    if harmonics < 8.0:
        print("WARNING: too few harmonics to measure mu on this source. The mu_meas column")
        print("         below is not trustworthy — use it for listening renders only.")
    print()
    print("%6s %5s %8s %8s %8s %8s %8s" %
          ("st", "k", "mu_want", "mu_meas", "err_%", "cents", "level_dB"))
    print("-" * 60)

    rows = []
    for semis in args.intervals:
        ratio = 2.0 ** (semis / 12.0)
        for k in args.k:
            mu_strength = k / KMU_DEFAULT
            mu_want = float(np.clip(ratio ** k, 0.45, 2.20))

            # Measurement render: tilt OFF, so the measured translation is the warp alone.
            meas = os.path.join(wavdir, "meas_%+03d_k%.2f.wav" % (semis, k))
            subprocess.run([args.sweep_bin, args.source, meas, "psola", str(semis),
                            "%.4f" % mu_strength, "0"], check=True)
            wet, _ = mu_probe.read_wav(meas)
            w = mu_probe.steady_window(wet, sr)
            mu_meas, corr = mu_probe.measure_mu(d, w, sr)

            # Pitch and level come from the existing probe, unchanged, so these numbers are
            # directly comparable with everything already in RESULTS.md.
            f0d = voice_probe.estimate_f0(d, sr)
            f0w = voice_probe.estimate_f0(w, sr)
            measured_ratio = (f0w / f0d) if f0d > 0 else float("nan")
            cents = 1200.0 * np.log2(measured_ratio / ratio) if measured_ratio > 0 else float("nan")
            level = 20.0 * np.log10((np.sqrt(np.mean(w ** 2)) + 1e-12) /
                                    (np.sqrt(np.mean(d ** 2)) + 1e-12))

            # Listening render: tilt ON, because that is the instrument.
            listen = os.path.join(wavdir, "listen_%+03d_k%.2f.wav" % (semis, k))
            subprocess.run([args.sweep_bin, args.source, listen, "psola", str(semis),
                            "%.4f" % mu_strength, "1"], check=True)

            err = 100.0 * (mu_meas / mu_want - 1.0) if mu_want > 0 else float("nan")
            print("%+6d %5.2f %8.3f %8.3f %+8.1f %+8.0f %+8.1f"
                  % (semis, k, mu_want, mu_meas, err, cents, level))
            rows.append((semis, k, mu_want, mu_meas, cents, level, corr))
        print()

    good = [r for r in rows if abs(r[4]) < 100.0]
    if good:
        errs = [abs(100.0 * (r[3] / r[2] - 1.0)) for r in good]
        print("mean |mu error| over cells with pitch within 100 cents: %.1f%%" % np.mean(errs))
    print("listening renders (tilt on) in %s" % wavdir)


if __name__ == "__main__":
    main()
