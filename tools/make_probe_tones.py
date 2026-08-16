#!/usr/bin/env python3
"""
make_probe_tones.py — controlled source material at CHOSEN fundamentals.

WHY THIS EXISTS, and why the two real recordings do not answer the question.

VOICE-MODEL.md C1 predicts that downward shift quality is bound by SOURCE PITCH, not by
shift ratio. A PSOLA grain is two source periods wide and cannot be wider, so the amount
of vocal-tract ringing it can carry is 2/F0_source seconds — 2.5 ms for a soprano, 22 ms
for a bass — against a tract that rings for 5-15 ms. That predicts a soprano falls apart
at a shift where a bass is clean.

`sustained_dry.wav` (814 Hz) and `melody_dry.wav` (242 Hz) differ in source pitch, but
they ALSO differ in vibrato, in vowel, in background accompaniment, and in how steady the
pitch is. Any difference between them has four candidate explanations. That is not a
controlled experiment, it is two anecdotes.

These tones vary ONE thing. Identical formants, identical amplitude envelope, identical
duration, identical noise floor — only F0 differs. If the prediction is right, the -24 st
render of the 90 Hz tone is clean and the -24 st render of the 814 Hz tone buzzes, and
nothing else in the material can be blamed.

THEY ARE NOT A SUBSTITUTE FOR REAL AUDIO. A synthetic source-filter voice is exactly the
model the engine assumes, so it will flatter any engine built on that model. HANDOVER.md
§4 already says this about tools/voice.hpp and it is just as true here. Use these to
isolate a mechanism; use real recordings to find out whether the mechanism matters.

usage:
  python3 tools/make_probe_tones.py outdir/            # 90, 242, 814 Hz
  python3 tools/make_probe_tones.py outdir/ --f0 120 300
"""

import argparse
import os
import struct
import wave

import numpy as np

SR = 48000.0
DUR = 3.0

# One vowel, held, for every tone. /a/ as in "father", adult male, per Peterson & Barney.
# Bandwidths are the ones that set RING TIME, which is the quantity the whole C1 argument
# turns on: tau = 1/(pi*B), so 60 Hz gives ~5.3 ms and 160 Hz gives ~2.0 ms.
FORMANTS = [(700.0, 60.0), (1220.0, 90.0), (2600.0, 160.0), (3400.0, 200.0)]

# Open quotient held CONSTANT across F0, which is the physical fact the tilt correction
# exists to restore. A real voice's open phase scales with its period; that is exactly what
# a copied PSOLA grain fails to do.
OPEN_QUOTIENT = 0.5


def glottal_flow(n, f0, sr):
    """Rosenberg-style glottal flow pulse train. Not the LF model — the point here is only
    that the excitation's DURATION scales with the period, so open quotient is constant."""
    out = np.zeros(n)
    period = sr / f0
    openlen = OPEN_QUOTIENT * period
    pos = 0.0
    while pos < n:
        i0 = int(pos)
        L = int(openlen)
        if L < 2:
            break
        k = np.arange(L)
        t = k / L
        # Rising then falling, with the abrupt closure at the end that gives voiced speech
        # its upper harmonics. Derivative of flow is what excites the tract, so difference.
        pulse = np.where(t < 0.6, 0.5 * (1 - np.cos(np.pi * t / 0.6)), np.cos(np.pi * (t - 0.6) / 0.8))
        d = np.diff(np.concatenate([[0.0], pulse]))
        end = min(n, i0 + L)
        out[i0:end] += d[: end - i0]
        pos += period
    return out


def resonator(x, fc, bw, sr):
    """Two-pole resonator. This IS the vocal tract's ring, and its decay time is what a
    PSOLA grain has to fit inside two source periods."""
    r = np.exp(-np.pi * bw / sr)
    theta = 2 * np.pi * fc / sr
    a1, a2 = -2 * r * np.cos(theta), r * r
    b0 = (1 - r) * np.sqrt(1 - 2 * r * np.cos(2 * theta) + r * r)
    y = np.zeros_like(x)
    y1 = y2 = 0.0
    for i in range(len(x)):
        v = b0 * x[i] - a1 * y1 - a2 * y2
        y2, y1 = y1, v
        y[i] = v
    return y


def make_tone(f0, sr=SR, dur=DUR):
    n = int(sr * dur)
    exc = glottal_flow(n, f0, sr)
    # A little breath noise, so the aperiodicity row (A1) has something to be measured on.
    exc += 0.004 * np.random.default_rng(int(f0)).standard_normal(n)

    y = np.zeros(n)
    for fc, bw in FORMANTS:
        y += resonator(exc, fc, bw, sr)

    # Soft attack and release. A step onset would make every measurement a measurement of
    # the click instead of the tone.
    env = np.ones(n)
    a = int(0.05 * sr)
    env[:a] = np.linspace(0, 1, a) ** 2
    env[-a:] = np.linspace(1, 0, a) ** 2
    y *= env

    peak = np.max(np.abs(y)) + 1e-12
    return (y / peak * 0.5).astype(np.float64)


def write_wav(path, x, sr=SR):
    d = np.clip(x, -1.0, 1.0)
    pcm = (d * 32767.0).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(sr))
        w.writeframes(pcm.tobytes())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--f0", type=float, nargs="+", default=[90.0, 242.0, 814.0],
                    help="fundamentals in Hz. Defaults span the C1 prediction: a bass whose "
                         "grain holds the whole ring, the melody recording's pitch, and the "
                         "soprano whose grain holds a fifth of it.")
    args = ap.parse_args()
    os.makedirs(args.outdir, exist_ok=True)

    for f0 in args.f0:
        path = os.path.join(args.outdir, "probe_%dhz.wav" % int(round(f0)))
        write_wav(path, make_tone(f0))
        ring_ms = 1000.0 / (np.pi * FORMANTS[0][1])
        grain_ms = 2000.0 / f0
        print("%-22s F0 %6.1f Hz   grain = %5.2f ms   F1 ring = %5.2f ms   %s"
              % (os.path.basename(path), f0, grain_ms, ring_ms,
                 "ring fits" if grain_ms >= ring_ms else "RING TRUNCATED"))


if __name__ == "__main__":
    main()
