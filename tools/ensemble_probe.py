#!/usr/bin/env python3
"""
ensemble_probe.py — does the harmony sound like N singers, or like N copies of one?

WHY THIS EXISTS. BUGS.md VH-005 ("sibilant polyphonic collapse") is measured with a single
number — the fricative region reads 1.7x the dry energy with four voices — and that number
cannot tell a fix from a level change. Turn the voices down and it improves; delete the
harmony and it improves. It is the same trap as duty cycle in VH-008 and "no gaps" in
VH-006: a metric that a no-op satisfies.

So this measures the property the defect is actually about — COHERENCE — and prints the
level beside it so neither can be read alone.

  gain_vs_1     energy of N voices over energy of ONE voice, in dB.
                N COHERENT copies give 20*log10(N): +12.0 dB for four.
                N INCOHERENT copies give 10*log10(N): +6.0 dB for four.
                This is the whole measurement. Where it lands between those two numbers
                says how much of the ensemble is really an ensemble. It is a RATIO between
                two renders of the same material, so a level change moves both and cancels.

  fricative     the same figure over the unvoiced window only. This is where the defect
                lives: voiced frames are decorrelated for free because every voice is at a
                different pitch, and only the passthrough path emits identical samples.

  voiced        the control window. It should NOT move much between configurations. If it
                does, whatever changed is not specific to the fricative and the fricative
                number cannot be attributed.

READ FRICATIVE AND VOICED TOGETHER. A fix that lowers both has changed the mix, not the
collapse.

The windows default to the synthetic demo phrase in tools/voice.hpp, whose fricative sits
at 2.10-2.45 s by construction — the one piece of test material where the unvoiced region
is known exactly rather than detected.

usage:
  python3 tools/ensemble_probe.py one_voice.wav four_voices.wav
  python3 tools/ensemble_probe.py one.wav four.wav --n 4 --fricative 2.10 2.45
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
    return np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0, float(sr)


def energy(x, sr, t0, t1):
    a, b = int(t0 * sr), int(t1 * sr)
    seg = x[max(0, a): min(len(x), b)]
    return float(np.mean(seg ** 2) + 1e-18)


def classify_frames(x, sr, frame_ms=32.0):
    """Split a signal into voiced / unvoiced frames by autocorrelation peak height.

    WHY AUTO-CLASSIFY RATHER THAN NAME WINDOWS BY HAND. Fixed windows only work on the
    synthetic demo phrase, where the fricative's location is known by construction. On any
    real recording the unvoiced regions are scattered through every syllable, and hand-
    picking them is both laborious and an invitation to pick the flattering ones. This is a
    crude detector — it does not have to be good, because it is applied identically to both
    renders being compared and any misclassification therefore cancels."""
    n = int(frame_ms * 0.001 * sr)
    voiced, unvoiced = [], []
    for i in range(0, len(x) - n, n):
        seg = x[i:i + n] - np.mean(x[i:i + n])
        rms = np.sqrt(np.mean(seg ** 2))
        if rms < 1e-4:
            continue                       # silence belongs to neither
        ac = np.correlate(seg, seg, "full")[n - 1:]
        ac = ac / (ac[0] + 1e-12)
        lo, hi = int(sr / 500.0), min(len(ac) - 1, int(sr / 70.0))
        peak = np.max(ac[lo:hi]) if hi > lo else 0.0
        (voiced if peak > 0.35 else unvoiced).append(i)
    return voiced, unvoiced


def auto_report(one, many, sr, n_voices, comp):
    """Energy ratio over auto-detected voiced and unvoiced frames."""
    _, unv = classify_frames(one, sr)
    voi, _ = classify_frames(one, sr)
    fl = int(32.0 * 0.001 * sr)

    def ratio(idx):
        if not idx:
            return float("nan")
        eo = sum(float(np.sum(one[i:i + fl] ** 2)) for i in idx) + 1e-18
        em = sum(float(np.sum(many[i:i + fl] ** 2)) for i in idx) + 1e-18
        return 10.0 * np.log10(em / eo) + comp

    return ratio(unv), ratio(voi), len(unv), len(voi)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("one", help="render with a SINGLE voice")
    ap.add_argument("many", help="render with N voices, same material and same per-voice gain")
    ap.add_argument("--n", type=int, default=4)
    ap.add_argument("--fricative", type=float, nargs=2, default=[2.10, 2.45])
    ap.add_argument("--voiced", type=float, nargs=2, default=[0.30, 1.10])
    ap.add_argument("--auto", action="store_true",
                    help="detect voiced/unvoiced frames instead of using fixed windows. "
                         "Required for real recordings, where the unvoiced regions are "
                         "scattered through every syllable.")
    ap.add_argument("--render-normalised", action="store_true",
                    help="the renders came from vh_render, which applies 1/sqrt(n) per "
                         "voice. That normalisation is exactly what this probe is trying "
                         "to see through, so add it back: 10*log10(n).")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    one, sr = read_wav(args.one)
    many, sr2 = read_wav(args.many)
    if sr != sr2:
        raise SystemExit("sample rate mismatch")

    # vh_render scales each voice by 1/sqrt(n) so a chord does not clip. That is correct
    # for a listening mix and fatal for this measurement, because it removes precisely the
    # N-vs-sqrt(N) difference being measured. Undo it explicitly rather than quietly.
    comp = 10.0 * np.log10(args.n) if args.render_normalised else 0.0

    def db(t0, t1):
        return 10.0 * np.log10(energy(many, sr, t0, t1) / energy(one, sr, t0, t1)) + comp

    if args.auto:
        comp = 10.0 * np.log10(args.n) if args.render_normalised else 0.0
        u, v, nu, nv = auto_report(one, many, sr, args.n, comp)
        coh, inc = 20.0 * np.log10(args.n), 10.0 * np.log10(args.n)
        collapse = (u - inc) / (coh - inc)
        if args.json:
            json.dump({"n": args.n, "unvoiced_db": u, "voiced_db": v,
                       "collapse": collapse, "frames_unvoiced": nu,
                       "frames_voiced": nv}, sys.stdout)
            print()
            return
        print("%d voices vs 1 voice, frames classified automatically" % args.n)
        print("  fully coherent would be %+.1f dB, fully incoherent %+.1f dB" % (coh, inc))
        print("  unvoiced (%4d frames)  %+6.2f dB    collapse %.2f  (0 = ensemble, 1 = one voice)"
              % (nu, u, collapse))
        print("  voiced   (%4d frames)  %+6.2f dB    (control: should not move between configs)"
              % (nv, v))
        return

    coherent = 20.0 * np.log10(args.n)
    incoherent = 10.0 * np.log10(args.n)
    fric = db(*args.fricative)
    voi = db(*args.voiced)

    # 0 = fully incoherent (what we want), 1 = fully coherent (the defect).
    span = coherent - incoherent
    collapse = (fric - incoherent) / span if span > 1e-9 else float("nan")

    out = {"n": args.n, "coherent_db": coherent, "incoherent_db": incoherent,
           "fricative_db": fric, "voiced_db": voi, "collapse": collapse}
    if args.json:
        json.dump(out, sys.stdout)
        print()
        return

    print("%d voices vs 1 voice" % args.n)
    print("  fully coherent would be %+.1f dB, fully incoherent %+.1f dB"
          % (coherent, incoherent))
    print("  fricative window %.2f-%.2f s   %+6.2f dB    collapse %.2f  (0 = ensemble, 1 = one voice)"
          % (args.fricative[0], args.fricative[1], fric, collapse))
    print("  voiced window    %.2f-%.2f s   %+6.2f dB    (control: should not move between configs)"
          % (args.voiced[0], args.voiced[1], voi))


if __name__ == "__main__":
    main()
