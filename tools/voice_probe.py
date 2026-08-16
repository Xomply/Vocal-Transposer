#!/usr/bin/env python3
"""
voice_probe.py — the four measurements that must be read TOGETHER, in one place.

WHY THIS EXISTS. BUGS.md VH-008 ends with a warning that duty cycle cannot be a standalone
pass/fail, because driving duty to 1.0 is trivially achieved by reintroducing VH-001. That
warning generalises, and the repo has been bitten by it twice already:

  VH-001  a broken shifter scored healthy on THREE metrics at once (banded F0, spectral
          centroid, gap check) because a no-op satisfies all of them.
  VH-006  a test used "no gaps" as a proxy for "shifts downward", and the two came apart.

So this tool refuses to print one number. Every run reports:

  pitch     output F0 over input F0, from an UNCONSTRAINED estimator. The property the
            feature exists for. Without this, everything below is decoration.
  duty      fraction of each output period carrying energy. VOICE-MODEL.md §5 argues this
            is really the OPEN QUOTIENT, and that a real bass does not score 1.0 either —
            so read it as "is the excitation the right shape", not "is there a hole".
  envdist   spectral envelope distance from dry, in dB, cepstrally liftered so it measures
            the ENVELOPE and not the harmonics moving. This is the formant axis: 0 means
            the envelope did not move (mu = 1), and it grows as mu departs from 1.
  level     RMS relative to dry, in dB. VH-003 was invisible to every other metric here.

READING THEM TOGETHER is the whole point:

  pitch right + duty low + envdist 0     truncated ring / collapsed open quotient (VH-008)
  pitch WRONG + duty high                VH-001 reintroduced
  pitch right + envdist tracks the ratio the engine has become the granular engine
  pitch right + level falling with shift VH-003

usage:
  python3 tools/voice_probe.py dry.wav wet.wav --ratio 0.25
  python3 tools/voice_probe.py dry.wav wet.wav --ratio 0.25 --json
"""

import argparse
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


def steady_window(x, sr, seconds=1.0):
    """Take a window from the middle, away from attack and release. Every metric below is a
    steady-state statement and measuring it across an onset measures the onset."""
    n = int(seconds * sr)
    if len(x) <= n:
        return x
    mid = len(x) // 2
    return x[mid - n // 2: mid + n // 2]


def estimate_f0(x, sr, fmin=40.0, fmax=1400.0):
    """YIN, properly: cumulative-mean-normalised difference function over a FIXED window,
    with an absolute threshold and the FIRST dip below it.

    WHY NOT THE GLOBAL MINIMUM: a signal at 787 Hz autocorrelates strongly at a 197 Hz lag,
    which is how BUGS.md VH-001 survived a banded tracker reporting everything healthy. The
    first dip below threshold picks the true period; the global minimum picks a subharmonic
    whenever the harmonics line up. Same choice as yin.cpp, same reason.

    WRITTEN WRONG THE FIRST TIME, and worth recording because the failure was silent and
    plausible: the difference function summed energy over the WHOLE signal instead of over
    a fixed window, so d(tau) grew with tau, the normalisation was meaningless, and the
    estimator returned a formant (855 Hz) for a 60 Hz output. It agreed with the requested
    ratio on the control render, which made it look correct. Cross-checking against
    inspect_audio.py's independent tracker is what caught it -- two estimators, not one."""
    x = x - np.mean(x)
    if np.max(np.abs(x)) < 1e-6:
        return 0.0
    n = len(x)
    lo, hi = max(2, int(sr / fmax)), int(sr / fmin)
    W = n // 2
    if hi >= W or hi <= lo + 2:
        return 0.0

    fsize = 1 << (n + W).bit_length()
    ac = np.fft.irfft(np.fft.rfft(x, fsize) * np.conj(np.fft.rfft(x[:W], fsize)))[: hi + 1]

    cs = np.concatenate([[0.0], np.cumsum(x ** 2)])
    e0 = cs[W]
    taus = np.arange(hi + 1)
    e_tau = cs[taus + W] - cs[taus]
    d = e0 + e_tau - 2.0 * ac

    cum = np.ones_like(d)
    run = 0.0
    for tau in range(1, hi + 1):
        run += d[tau]
        cum[tau] = d[tau] * tau / (run + 1e-18)

    tau = -1
    for t in range(lo, hi - 1):
        if cum[t] < 0.15 and cum[t] <= cum[t + 1] and cum[t] <= cum[t - 1]:
            tau = t
            break
    if tau < 0:
        tau = int(np.argmin(cum[lo:hi])) + lo
    if tau <= 0:
        return 0.0
    if 0 < tau < hi - 1:
        a, b, c = cum[tau - 1], cum[tau], cum[tau + 1]
        denom = 2 * (2 * b - a - c)
        if abs(denom) > 1e-18:
            tau = tau + (a - c) / denom
    return sr / tau


def duty(x, sr, f0):
    """Fraction of each output period carrying energy, above -25 dB of the period's peak.

    NOT A PASS/FAIL. See the module docstring and VOICE-MODEL.md §4: a real low voice does
    not score 1.0 either, because tract ring time is independent of F0. This measures the
    shape of the excitation, and it is only interpretable next to `pitch`."""
    if f0 <= 0:
        return float("nan")
    period = sr / f0
    if period < 8 or len(x) < 4 * period:
        return float("nan")
    nper = int(len(x) // period)
    vals = []
    for k in range(1, nper - 1):
        seg = x[int(k * period): int((k + 1) * period)]
        if len(seg) < 8:
            continue
        pk = np.max(np.abs(seg))
        if pk < 1e-5:
            continue
        vals.append(np.mean(np.abs(seg) > pk * 0.056))   # -25 dB
    return float(np.median(vals)) if vals else float("nan")


def envelope_db(x, sr, nfft=4096, lifter=32):
    """Cepstrally liftered log-magnitude spectrum: the spectral ENVELOPE.

    WHY LIFTERING AND NOT A BAND-ENERGY RATIO: band energy moves when the HARMONICS move,
    which they always do here, so it cannot separate 'the formants shifted' from 'the pitch
    shifted'. Low quefrencies are the envelope; high quefrencies are the harmonic comb.
    Keeping the first `lifter` cepstral coefficients discards the comb and keeps the tract.
    This is the standard recipe and the reason RESULTS.md's single-frequency formant probes
    measured nothing but window leakage."""
    w = np.hanning(min(nfft, len(x)))
    seg = x[:len(w)] * w
    spec = np.abs(np.fft.rfft(seg, nfft)) + 1e-12
    logspec = np.log(spec)
    cep = np.fft.irfft(logspec)
    cep[lifter:-lifter] = 0.0
    return np.fft.rfft(cep).real * (20.0 / np.log(10.0))


def envelope_distance(dry, wet, sr, fmax=6000.0):
    """RMS difference between two envelopes, in dB, over the band where formants live."""
    a, b = envelope_db(dry, sr), envelope_db(wet, sr)
    n = min(len(a), len(b))
    freqs = np.linspace(0, sr / 2, n)
    m = freqs <= fmax
    a, b = a[:n][m], b[:n][m]
    a = a - np.mean(a)
    b = b - np.mean(b)          # remove level; this is a SHAPE comparison
    return float(np.sqrt(np.mean((a - b) ** 2)))


def envelope_contrast(x, sr, fmax=6000.0):
    """Standard deviation of the liftered envelope, in dB — how PEAKY the spectrum is.

    WHY THIS AND NOT envelope_distance: distance measures whether the envelope MOVED, which
    mu does on purpose. Contrast measures whether it is still an envelope at all. The C1
    prediction in VOICE-MODEL.md is that a grain truncated at 2/F0_source cannot carry the
    tract's full ring, so the formants smear and the spectrum FLATTENS. Flattening is a
    contrast collapse; it is invisible to distance, which would score a smeared envelope and
    a shifted one the same."""
    e = envelope_db(x, sr)
    freqs = np.linspace(0, sr / 2, len(e))
    e = e[freqs <= fmax]
    return float(np.std(e))


def probe(dry_path, wet_path, ratio):
    dry, sr = read_wav(dry_path)
    wet, sr2 = read_wav(wet_path)
    if sr != sr2:
        raise SystemExit("sample rate mismatch")

    d = steady_window(dry, sr)
    w = steady_window(wet, sr)

    f0d = estimate_f0(d, sr)
    f0w = estimate_f0(w, sr)

    rms_d = float(np.sqrt(np.mean(d ** 2)) + 1e-12)
    rms_w = float(np.sqrt(np.mean(w ** 2)) + 1e-12)

    measured = (f0w / f0d) if f0d > 0 else float("nan")
    cents = 1200.0 * np.log2(measured / ratio) if (measured > 0 and ratio > 0) else float("nan")

    return {
        "f0_dry": f0d,
        "f0_wet": f0w,
        "ratio_requested": ratio,
        "ratio_measured": measured,
        "cents_error": cents,
        "duty": duty(w, sr, f0w),
        "duty_dry": duty(d, sr, f0d),
        "envdist_db": envelope_distance(d, w, sr),
        "envcontrast_dry": envelope_contrast(d, sr),
        "envcontrast_wet": envelope_contrast(w, sr),
        "level_db": 20.0 * np.log10(rms_w / rms_d),
        "peak": float(np.max(np.abs(wet))),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dry")
    ap.add_argument("wet")
    ap.add_argument("--ratio", type=float, required=True)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    r = probe(args.dry, args.wet, args.ratio)
    if args.json:
        json.dump(r, sys.stdout)
        print()
        return

    print("dry F0        %8.1f Hz   duty %.2f" % (r["f0_dry"], r["duty_dry"]))
    print("wet F0        %8.1f Hz   duty %.2f" % (r["f0_wet"], r["duty"]))
    print("ratio         %8.3f      requested %.3f   (%+.0f cents)"
          % (r["ratio_measured"], r["ratio_requested"], r["cents_error"]))
    print("envelope dist %8.2f dB   0 = envelope held, grows with mu" % r["envdist_db"])
    print("env contrast  %8.2f dB   dry %.2f dB — a collapse means the formants smeared"
          % (r["envcontrast_wet"], r["envcontrast_dry"]))
    print("level         %+8.2f dB   peak %.3f" % (r["level_db"], r["peak"]))


if __name__ == "__main__":
    main()
