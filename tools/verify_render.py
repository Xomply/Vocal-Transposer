#!/usr/bin/env python3
"""
verify_render.py — independent spectral checks on vh_demo / vh_render output.

WHY THIS EXISTS: vh_bench measures the engine with the engine's own YIN. That is a
useful self-consistency check but it is not independent evidence. This script reads
the rendered WAVs with nothing but numpy and asks the three questions that actually
decide whether the thing works:

  1. HARMONY  — do notes that are NOT in the sung input appear in the output, and do
                notes that are in neither input nor chord stay at the noise floor?
                (The second half is the control. RESULTS.md has the first half only.)
  2. FORMANTS — does the PSOLA path hold the spectral envelope while the granular
                path drags it up with pitch? This is the entire argument for the
                second engine, so it should be measured, not asserted.
  3. HASH     — does any single voice inject broadband high-frequency energy that
                its siblings do not? Grain discontinuities show up here long before
                they show up in a pitch-error number.

USAGE
    ./build/vh_demo ./audio
    python3 tools/verify_render.py ./audio

    # isolate one voice at a time (this is how finding 3 below was localised):
    ./build/vh_render audio/dry.wav out2/ n38 "3.6:38" 60
    python3 tools/verify_render.py ./audio --isolate out2 38 45 50 57 --window 3.9 4.6

NOTE ON WINDOWS: every measurement below is windowed by hand. Pick voiced windows.
The demo phrase has a fricative at roughly 2.0-2.5 s; including it contaminates the
centroid measurement with unvoiced passthrough and makes PSOLA look like it is
moving the envelope when it is not. That mistake cost me a wrong reading once
already, which is why it is written down here.
"""

import sys
import wave
import numpy as np


# --- io ---------------------------------------------------------------------

def read_wav(path):
    with wave.open(path, "rb") as w:
        assert w.getsampwidth() == 2, f"{path}: expected 16-bit"
        n, sr = w.getnframes(), w.getframerate()
        raw = w.readframes(n)
    x = np.frombuffer(raw, dtype=np.int16).astype(np.float64) / 32768.0
    if w.getnchannels() == 2:
        x = x.reshape(-1, 2).mean(axis=1)
    return x, sr


def window(x, sr, t0, t1):
    """Hann-windowed slice. Length is taken from the slice, never recomputed from
    the times — an off-by-one there is a silent broadcast error."""
    s = x[int(t0 * sr):int(t1 * sr)]
    return s * np.hanning(len(s))


# --- measurements -----------------------------------------------------------

def tone_magnitude(x, sr, t0, t1, freq):
    """Amplitude at one exact frequency, by direct DFT rather than an FFT bin.

    WHY NOT AN FFT BIN: musical frequencies do not land on bin centres, and the
    resulting scalloping loss is several dB — enough to turn a real 150x harmony
    ratio into an unconvincing one. A single-frequency DFT costs nothing here.
    """
    s = window(x, sr, t0, t1)
    k = np.arange(len(s))
    return np.abs(np.sum(s * np.exp(-2j * np.pi * freq * k / sr))) / len(s) * 2


def centroid(x, sr, t0, t1, lo=60.0, hi=8000.0):
    """Spectral centroid as a cheap proxy for where the formants sit.

    CAVEAT, and it matters: this is a proxy, not a formant tracker. It answers
    "did the envelope move?" and not "where is F1?". It is used here because the
    question at hand is comparative — PSOLA against granular from identical input —
    and for that a proxy with a consistent bias is sufficient. Do not quote these
    numbers as formant frequencies.
    """
    s = window(x, sr, t0, t1)
    mag = np.abs(np.fft.rfft(s))
    f = np.fft.rfftfreq(len(s), 1.0 / sr)
    m = (f > lo) & (f < hi)
    return float(np.sum(f[m] * mag[m]) / np.sum(mag[m]))


def band_energy(x, sr, t0, t1, edges=((60, 300), (300, 1000), (1000, 3000), (3000, 8000))):
    s = window(x, sr, t0, t1)
    p = np.abs(np.fft.rfft(s)) ** 2
    f = np.fft.rfftfreq(len(s), 1.0 / sr)
    return np.array([p[(f >= a) & (f < b)].sum() for a, b in edges])


def estimate_f0(x, sr, t0, t1, lo=70.0, hi=500.0):
    """Plain autocorrelation. Deliberately NOT the project's YIN: using the engine's
    own estimator to grade the engine's own output is circular."""
    s = x[int(t0 * sr):int(t1 * sr)]
    s = s - s.mean()
    ac = np.correlate(s, s, "full")[len(s) - 1:]
    a, b = int(sr / hi), int(sr / lo)
    return sr / (a + int(np.argmax(ac[a:b])))


def midi_hz(n):
    return 440.0 * 2.0 ** ((n - 69) / 12.0)


# --- reports ----------------------------------------------------------------

# The demo's chord progression, mirrored from tools/demo.cpp. If that file changes
# this table goes stale silently, which is the usual fate of duplicated constants —
# it is duplicated anyway because the alternative is parsing C++.
DEMO_REGIONS = [
    # label, voiced window (fricative and release tails excluded), chord notes
    ("Am",  0.40, 1.20, [45, 52, 57, 60]),
    ("F",   1.50, 1.95, [41, 48, 53, 57]),
    ("C/G", 2.70, 3.40, [43, 52, 55, 60]),
    ("D",   3.90, 4.60, [38, 45, 50, 57]),
]


def report_harmony(audio_dir):
    dry, sr = read_wav(f"{audio_dir}/dry.wav")
    ps, _ = read_wav(f"{audio_dir}/wet_psola.wav")
    gr, _ = read_wav(f"{audio_dir}/wet_granular.wav")

    print("\n=== 1. HARMONY: notes the singer never sang ===")
    print("Ratios are wet/dry amplitude at the exact chord frequency.")
    print("The CONTROL row is a note in neither the voice nor the chord: it must stay flat.\n")
    for label, t0, t1, notes in DEMO_REGIONS:
        sung = estimate_f0(dry, sr, t0, t1)
        print(f"  {label} chord   (sung {sung:6.1f} Hz)")
        for n in notes:
            f = midi_hz(n)
            d = tone_magnitude(dry, sr, t0, t1, f)
            p = tone_magnitude(ps, sr, t0, t1, f)
            g = tone_magnitude(gr, sr, t0, t1, f)
            st = 12 * np.log2(f / sung)
            print(f"    note {n:3d}  {f:7.1f} Hz  {st:+6.1f} st   "
                  f"dry {d:.5f}   psola {p / max(d, 1e-9):8.1f}x   gran {g / max(d, 1e-9):8.1f}x")
        # Control: a note in neither the chord nor the voice's harmonic series.
        # RANGE MATTERS. An earlier version searched from MIDI 30 (46 Hz), where the
        # dry signal is essentially silent — dividing by that noise floor produced a
        # meaningless 87x "control" that looked like a failure and was not. Keep the
        # control inside the range the material actually occupies.
        ctrl = next(c for c in range(45, 73) if c not in notes
                    and min(abs(12 * np.log2(midi_hz(c) / (sung * h))) for h in (1, 2, 3, 4)) > 0.7)
        f = midi_hz(ctrl)
        d = tone_magnitude(dry, sr, t0, t1, f)
        p = tone_magnitude(ps, sr, t0, t1, f)
        print(f"    CONTROL {ctrl:3d} {f:7.1f} Hz  (not played)  "
              f"dry {d:.5f}   psola {p / max(d, 1e-9):8.1f}x")
        print()


def report_formants(audio_dir):
    dry, sr = read_wav(f"{audio_dir}/dry.wav")
    ps, _ = read_wav(f"{audio_dir}/wet_psola.wav")
    gr, _ = read_wav(f"{audio_dir}/wet_granular.wav")

    print("=== 2. FORMANTS: envelope held vs dragged ===")
    print("Granular resamples, so its centroid MUST rise with upward shifts.")
    print("PSOLA re-emits unmodified grains, so its centroid MUST NOT.\n")
    print(f"  {'region':8s} {'dry Hz':>9s} {'psola':>9s} {'ratio':>8s} {'gran':>9s} {'ratio':>8s}")
    for label, t0, t1, _ in DEMO_REGIONS:
        d = centroid(dry, sr, t0, t1)
        p = centroid(ps, sr, t0, t1)
        g = centroid(gr, sr, t0, t1)
        flag = "   <-- PSOLA moved the envelope; it should not" if p / d > 1.15 else ""
        print(f"  {label:8s} {d:9.1f} {p:9.1f} {p / d:7.2f}x {g:9.1f} {g / d:7.2f}x{flag}")
    print()


def report_isolate(audio_dir, iso_dir, notes, t0, t1):
    """Per-voice band energies against dry. Localises which single voice is
    misbehaving in a chord — a chord sum tells you something is wrong and nothing
    about which voice did it."""
    dry, sr = read_wav(f"{audio_dir}/dry.wav")
    ref = band_energy(dry, sr, t0, t1)
    sung = estimate_f0(dry, sr, t0, t1)

    print(f"=== 3. PER-VOICE BAND ENERGY, {t0}-{t1}s (sung {sung:.1f} Hz) ===")
    print("dB relative to dry. A single voice should sit BELOW dry in every band.")
    print("A large positive number in 1k-3k or 3k-8k is grain hash, not a formant.\n")
    print(f"  {'voice':22s} {'60-300':>8s} {'300-1k':>8s} {'1k-3k':>8s} {'3k-8k':>8s}")
    for n in notes:
        path = f"{iso_dir}/n{n}_psola.wav"
        try:
            x, _ = read_wav(path)
        except FileNotFoundError:
            print(f"  note {n}: {path} missing — render it with vh_render first")
            continue
        r = 10 * np.log10(band_energy(x, sr, t0, t1) / ref)
        st = 12 * np.log2(midi_hz(n) / sung)
        flag = "   <-- broadband hash" if r[3] > 6 else ""
        print(f"  note {n:3d} ({st:+5.1f} st) alone " + "".join(f"{v:+8.1f}" for v in r) + flag)
    print()


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 1
    audio_dir = args[0]

    iso = None
    win = (3.90, 4.60)
    if "--isolate" in args:
        i = args.index("--isolate")
        iso_dir = args[i + 1]
        notes = []
        for a in args[i + 2:]:
            if a.startswith("--"):
                break
            notes.append(int(a))
        iso = (iso_dir, notes)
    if "--window" in args:
        i = args.index("--window")
        win = (float(args[i + 1]), float(args[i + 2]))

    report_harmony(audio_dir)
    report_formants(audio_dir)
    if iso:
        report_isolate(audio_dir, iso[0], iso[1], win[0], win[1])
    return 0


if __name__ == "__main__":
    sys.exit(main())
