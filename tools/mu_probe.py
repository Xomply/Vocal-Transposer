#!/usr/bin/env python3
"""
mu_probe.py — measure the formant scale factor the engine ACTUALLY applied.

WHY THIS EXISTS, and it is the gap that stops `k_mu` from being tunable. VOICE-MODEL.md §6
derives mu = ratio^k and voicing.hpp implements it, but every existing metric stops one
step short of checking it:

  envdist_db      says the envelope MOVED. It does not say by how much, and it grows with
                  any change of shape at all — including the source tilt, which is a
                  different row of the model entirely.
  envcontrast     says the envelope is still peaky. Nothing about position.
  centroid        a single scalar over the whole spectrum, moved by level and by tilt as
                  much as by formants.

So `k_mu` has been tuned against metrics that cannot tell 0.81 from 0.7, which means it has
not really been tuned at all. This measures the thing.

THE MEASUREMENT. A formant scale factor is a MULTIPLICATION on the frequency axis, and a
multiplication is a TRANSLATION on a log-frequency axis. So: take the cepstrally liftered
log-magnitude envelope of dry and of wet, resample both onto a log-frequency grid, and
cross-correlate. The lag that maximises correlation is log(mu). Parabolic interpolation on
the correlation peak gives sub-bin resolution, which matters because one bin is already a
few percent of mu.

Liftering is what makes this possible at all: the harmonic comb moves with the PITCH, and
if it were still in the spectrum the correlation would lock onto the comb and return the
pitch ratio for every configuration. Low quefrencies are the tract; high quefrencies are
the comb. Same recipe as voice_probe.envelope_db, same reason.

THE TWO CALIBRATION CONTROLS, which are the reason this can be trusted. Both have known
answers that come from the algorithms rather than from this file:

  granular, any ratio     mu MUST equal the ratio. The granular engine resamples, so it
                          drags the whole spectrum with the pitch by construction. This is
                          the far end of the exponent (k = 1/k_mu in profile terms).
  psola, muStrength = 0   mu MUST equal 1.0. Grain content is copied verbatim, so the
                          envelope cannot move. This is the near end (k = 0).

Run `--selftest` to check both before believing any number in between. If the controls do
not come back at ratio and 1.0, the probe is wrong and nothing else it says means anything.

THE LIMIT, AND IT IS A REAL ONE RATHER THAN AN IMPLEMENTATION DETAIL. A cepstral envelope
is only informative where harmonics actually sample it, so this measurement needs roughly
eight or more harmonics inside the band. At 242 Hz (the melody) that is comfortable and the
controls land within 32 cents. At 814 Hz (the soprano) there are barely seven harmonics
below 6 kHz, the envelope between them is interpolation, and the controls come back 170-200
cents out — the probe cannot measure that source and says so rather than returning a
confident wrong number.

That is the same condition VOICE-MODEL.md §5 records for LPC ("the all-pole fit is
worst-conditioned at high F0, where widely spaced harmonics under-sample the envelope, the
same reason the soprano is already the hard case"). It is not a coincidence and it is not
fixable here: TUNE `k_mu` ON LOW-PITCHED MATERIAL. The soprano is for listening, not for
measuring.

MEASURE WITH THE TILT DISABLED. The source tilt (voicing.hpp A4) deliberately reshapes the
spectrum, and a broadband tilt correlates with a translation — it will bias mu. The sweep
driver passes tiltStrength = 0 for exactly this reason and turns it back on only for the
listening renders.

usage:
  python3 tools/mu_probe.py dry.wav wet.wav
  python3 tools/mu_probe.py dry.wav wet.wav --json
  python3 tools/mu_probe.py --selftest ./build/vh_sweep sustained_dry.wav
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np


def read_wav(path):
    with wave.open(path, "rb") as w:
        n, sr = w.getnframes(), w.getframerate()
        raw = w.readframes(n)
    return np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0, float(sr)


def source_f0(x, sr):
    """Crude autocorrelation F0, used only to decide whether the envelope is sampled
    densely enough for measure_mu to mean anything. Precision is irrelevant here."""
    s = x - np.mean(x)
    if np.sqrt(np.mean(s ** 2)) < 1e-5:
        return 0.0
    n = min(len(s), 1 << 15)
    s = s[:n]
    ac = np.correlate(s, s, "full")[n - 1:]
    ac /= ac[0] + 1e-12
    lo, hi = int(sr / 1200.0), int(sr / 70.0)
    if hi <= lo + 2 or hi >= len(ac):
        return 0.0
    return float(sr / (int(np.argmax(ac[lo:hi])) + lo))


def steady_window(x, sr, seconds=1.5):
    """A window from the middle, away from attack and release. Everything here is a
    steady-state statement; measuring across an onset measures the onset."""
    n = int(seconds * sr)
    if len(x) <= n:
        return x
    mid = len(x) // 2
    return x[mid - n // 2: mid + n // 2]


def envelope_db(x, sr, nfft=8192, lifter=40):
    """Cepstrally liftered log-magnitude spectrum, averaged over several frames.

    AVERAGED, unlike voice_probe's single-frame version: a lone frame's envelope wobbles
    with whichever harmonics happen to land where, and that wobble is the noise floor of
    the correlation below. Averaging log-magnitude over ~10 frames costs nothing and makes
    the peak sharp enough to interpolate."""
    hop = nfft // 2
    frames = []
    for i in range(0, max(1, len(x) - nfft), hop):
        seg = x[i:i + nfft]
        if len(seg) < nfft or np.sqrt(np.mean(seg ** 2)) < 1e-5:
            continue
        spec = np.abs(np.fft.rfft(seg * np.hanning(nfft))) + 1e-12
        cep = np.fft.irfft(np.log(spec))
        cep[lifter:-lifter] = 0.0
        frames.append(np.fft.rfft(cep).real)
    if not frames:
        return None
    return np.mean(frames, axis=0) * (20.0 / np.log(10.0))


def to_log_axis(env, sr, f_lo=600.0, f_hi=6000.0, bins=600):
    """Resample an envelope onto a uniform log-frequency grid.

    The band matters, and 600-6000 Hz was chosen by MEASUREMENT rather than by taste: the
    two calibration controls were run at five candidate bands and this one minimises the
    error against their known answers (31.8 cents mean on the melody, against 48-85 for the
    alternatives). Wider is not better — below ~600 Hz the envelope is dominated by the
    fundamental's own region and by whatever the shift did there, and above ~6 kHz there is
    little formant structure and a great deal of noise."""
    freqs = np.linspace(0.0, sr / 2.0, len(env))
    grid = np.exp(np.linspace(np.log(f_lo), np.log(f_hi), bins))
    return np.interp(grid, freqs, env), np.log(f_hi / f_lo) / (bins - 1)


def measure_mu(dry, wet, sr, f_lo=600.0, f_hi=6000.0):
    """Return the formant scale factor that best maps the dry envelope onto the wet one."""
    ed, ew = envelope_db(dry, sr), envelope_db(wet, sr)
    if ed is None or ew is None:
        return float("nan"), 0.0

    a, dlog = to_log_axis(ed, sr, f_lo, f_hi)
    b, _ = to_log_axis(ew, sr, f_lo, f_hi)

    # Search only physically reachable factors. The profile clamps mu to [0.45, 2.20], so
    # anything outside that is a correlation artefact rather than a candidate.
    span = int(np.log(2.4) / dlog)

    # PER-LAG NORMALISED CORRELATION, OVER THE OVERLAP ONLY.
    #
    # THE BUG THIS FIXES, and it is worth recording because the first version looked
    # plausible and was wrong in a direction that flattered the engine. np.correlate divides
    # every lag by the same length, so a large lag — where the two envelopes overlap over
    # fewer bins — scores systematically lower than a small one. The peak is therefore
    # pulled toward zero lag, i.e. toward mu = 1. The granular control, whose answer is
    # known to be the ratio, came back at 1.243 when it had to be 1.498: a 33% underestimate
    # that would have been read as "the warp is gentler than requested".
    #
    # Correlating only the overlapping bins, and standardising each side WITHIN that
    # overlap, removes the bias. Lags are then comparable to one another, which is the only
    # property the argmax actually needs.
    best_lag, best_corr = 0, -2.0
    n = len(a)
    for lag in range(-span, span + 1):
        if lag >= 0:
            u, v = a[:n - lag], b[lag:]
        else:
            u, v = a[-lag:], b[:n + lag]
        if len(u) < 0.55 * n:          # too little overlap to mean anything
            continue
        u = u - u.mean()
        v = v - v.mean()
        d = (np.sqrt(np.sum(u * u) * np.sum(v * v)) + 1e-12)
        c = float(np.sum(u * v) / d)
        if c > best_corr:
            best_corr, best_lag = c, lag

    # Parabolic interpolation on the correlation peak, recomputed at the neighbouring lags.
    # One bin is ~0.5% of mu, so this is not decoration: without it the reported mu
    # quantises visibly across a k sweep.
    def corr_at(lag):
        if abs(lag) > span:
            return None
        if lag >= 0:
            u, v = a[:n - lag], b[lag:]
        else:
            u, v = a[-lag:], b[:n + lag]
        if len(u) < 0.55 * n:
            return None
        u = u - u.mean()
        v = v - v.mean()
        return float(np.sum(u * v) / (np.sqrt(np.sum(u * u) * np.sum(v * v)) + 1e-12))

    frac = 0.0
    y0, y1, y2 = corr_at(best_lag - 1), best_corr, corr_at(best_lag + 1)
    if y0 is not None and y2 is not None:
        denom = 2.0 * (2.0 * y1 - y0 - y2)
        if abs(denom) > 1e-12:
            frac = (y2 - y0) / denom

    return float(np.exp((best_lag + frac) * dlog)), best_corr


def selftest(sweep_bin, source):
    """The two known-answer controls. See the module docstring."""
    ok = True
    with tempfile.TemporaryDirectory() as td:
        dry, sr = read_wav(source)
        d = steady_window(dry, sr)

        print("calibration controls (the probe is only meaningful if these pass)")
        print("%-34s %10s %10s %8s" % ("case", "expected", "measured", "corr"))
        for semis in (-12, -5, 7):
            ratio = 2.0 ** (semis / 12.0)

            # Control 1: granular resamples, so mu is the ratio by construction.
            g = os.path.join(td, "g.wav")
            subprocess.run([sweep_bin, source, g, "granular", str(semis), "0", "0"], check=True)
            wg, _ = read_wav(g)
            mg, cg = measure_mu(d, steady_window(wg, sr), sr)

            # Control 2: PSOLA copies grains verbatim, so mu is 1.0 by construction.
            p = os.path.join(td, "p.wav")
            subprocess.run([sweep_bin, source, p, "psola", str(semis), "0", "0"], check=True)
            wp, _ = read_wav(p)
            mp, cp = measure_mu(d, steady_window(wp, sr), sr)

            for label, exp, got, c in (("granular %+d st" % semis, ratio, mg, cg),
                                       ("psola muStrength=0 %+d st" % semis, 1.0, mp, cp)):
                err = abs(np.log2(got / exp)) * 1200.0 if got > 0 else 9e9
                flag = "ok" if err < 120.0 else "FAIL"
                if flag == "FAIL":
                    ok = False
                print("%-34s %10.3f %10.3f %8.2f  %s" % (label, exp, got, c, flag))
    print()
    print("PASS — measured mu can be trusted" if ok else
          "FAIL — do not trust any mu figure until this passes")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", nargs=2, metavar=("SWEEP_BIN", "SOURCE"))
    ap.add_argument("dry", nargs="?")
    ap.add_argument("wet", nargs="?")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        sys.exit(selftest(*args.selftest))
    if not args.dry or not args.wet:
        ap.error("give a dry and a wet file, or --selftest")

    dry, sr = read_wav(args.dry)
    wet, sr2 = read_wav(args.wet)
    if sr != sr2:
        raise SystemExit("sample rate mismatch")

    d = steady_window(dry, sr)
    mu, corr = measure_mu(d, steady_window(wet, sr), sr)
    f0 = source_f0(d, sr)
    out = {"mu_measured": mu, "correlation": corr, "source_f0": f0,
           "harmonics_in_band": (6000.0 / f0) if f0 > 0 else 0.0}
    if args.json:
        json.dump(out, sys.stdout)
        print()
    else:
        print("measured mu %.3f   (peak correlation %.2f — below ~0.5 the envelope match "
              "is poor and mu is not meaningful)" % (mu, corr))
        if out["harmonics_in_band"] < 8.0:
            print("  WARNING: source F0 is %.0f Hz, giving only %.1f harmonics below 6 kHz."
                  % (f0, out["harmonics_in_band"]))
            print("  The envelope is under-sampled and this figure is not trustworthy. "
                  "Tune k_mu on lower-pitched material; see the module docstring.")


if __name__ == "__main__":
    main()
