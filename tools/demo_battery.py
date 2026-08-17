#!/usr/bin/env python3
"""
demo_battery.py — run the project's existing measurement tools across the 4 new
proof-of-concept demos (real solo voice x varied MIDI content), and print one
table per demo so the numbers can be cross-referenced against listening.

WHY A SEPARATE DRIVER, NOT click_sweep.py. click_sweep.py is deliberately fixed
to two files (sustained/melody) and a single-note vh_trace harness (one ratio,
no chords). These four demos are vh_render output (chord progressions, both
Mode A and Mode B, all three engine configurations) — a different shape of
material the existing sweep was never meant to cover. Rather than bend that
driver out of shape, this one reuses its actual measurement primitives
(click_probe.find_clicks, ensemble_probe's auto frame classifier) against the
vh_render output directly.

WHAT IT MEASURES, per demo:
  - click count on every rendered variant, with the DRY file as a control
    (a click count on dry material is the source's own transients, not a
    defect — see click_probe.py's module docstring).
  - ensemble coherence (psola variant vs the matching single-note render),
    auto-classified voiced/unvoiced, so the sibilant-collapse question can be
    asked on real recordings where fricative location isn't known a priori.
  - basic sanity: peak, RMS, any non-finite samples (a repeat of RESULTS.md's
    "starts with silence" and "inf" lessons, applied to genuinely new material
    these fixes were never run against before).

usage:
  python3 tools/demo_battery.py demo_new/ audio/ demo_new/one_voice/
"""

import glob
import json
import os
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import click_probe          # noqa: E402
import ensemble_probe       # noqa: E402


DEMOS = [
    ("vh_demo1_sustain", "demo1", "vh_demo1_sustain.wav", "one1"),
    ("vh_demo2_melody",  "demo2", "vh_demo2_melody.wav",  "one2"),
    ("vh_demo3_arpeggio","demo3", "vh_demo3_arpeggio.wav","one3"),
    ("vh_demo4_lowvowel","demo4", "vh_demo4_lowvowel.wav","one4"),
]

VARIANTS = ["dry", "granular", "psola", "blend", "blend_aligned", "mix", "modeB"]


def read_wav(path):
    with wave.open(path, "rb") as w:
        n, sr = w.getnframes(), w.getframerate()
        raw = w.readframes(n)
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    return x, float(sr)


def sanity(x):
    finite = bool(np.all(np.isfinite(x)))
    return {
        "peak": float(np.max(np.abs(x))) if len(x) else 0.0,
        "rms": float(np.sqrt(np.mean(x ** 2))) if len(x) else 0.0,
        "finite": finite,
        "seconds": len(x) / 48000.0,
    }


def main():
    if len(sys.argv) < 4:
        print("usage: demo_battery.py <demo_new_dir> <audio_dir> <one_voice_dir>", file=sys.stderr)
        sys.exit(1)
    demo_root, audio_dir, one_dir = sys.argv[1], sys.argv[2], sys.argv[3]

    report = {}

    for folder, prefix, dry_name, one_prefix in DEMOS:
        print("=" * 96)
        print(f"{folder}  (prefix {prefix})")
        print("=" * 96)
        d = os.path.join(demo_root, folder)
        demo_report = {"variants": {}}

        dry_path = os.path.join(d, f"{prefix}_dry.wav")
        dry_x, sr = read_wav(dry_path)
        dry_clicks = click_probe.find_clicks(dry_x, sr)

        print(f"{'variant':<16}{'sec':>7}{'peak':>8}{'rms':>8}{'finite':>8}{'clicks':>9}  (dry clicks: {len(dry_clicks)})")
        for v in VARIANTS:
            path = os.path.join(d, f"{prefix}_{v}.wav")
            if not os.path.exists(path):
                continue
            x, sr2 = read_wav(path)
            s = sanity(x)
            clicks = click_probe.find_clicks(x, sr2) if v != "dry" else dry_clicks
            print(f"{v:<16}{s['seconds']:>7.1f}{s['peak']:>8.3f}{s['rms']:>8.4f}"
                  f"{str(s['finite']):>8}{len(clicks):>9}")
            demo_report["variants"][v] = {**s, "clicks": int(len(clicks))}

        # Ensemble coherence: psola (N voices from the chord progression) vs the matching
        # single-note render.
        #
        # CLASSIFY THE FRAMES FROM THE *DRY* FILE, NOT FROM A RENDER. ensemble_probe's
        # auto_report classifies whichever signal it is handed as `one`, and that is wrong
        # here for a reason worth recording, because it produced a confident wrong number
        # once already. The demo4 single-note render targets MIDI 36 = 65.4 Hz, below BOTH
        # YIN's 70 Hz floor and classify_frames' own 70 Hz autocorrelation search bound. So
        # 621 frames of a correctly-sung low bass note scored "unvoiced", and the resulting
        # +5.24 dB read as sibilant collapse when it was nothing but the ruler being too
        # short. Same failure mode as the two test bugs in RESULTS.md: measure the thing,
        # not a proxy for it.
        #
        # The dry file is the right classifier input anyway: voicing is a property of the
        # SOURCE material, identical across every render being compared, so misclassification
        # cancels between them instead of varying with the shift ratio.
        one_path = os.path.join(one_dir, f"{one_prefix}_psola.wav")
        many_path = os.path.join(d, f"{prefix}_psola.wav")
        if os.path.exists(one_path) and os.path.exists(many_path):
            one, sr_o = read_wav(one_path)
            many, _ = read_wav(many_path)
            voi, unv = ensemble_probe.classify_frames(dry_x, sr)
            fl = int(0.032 * sr)

            def ratio(idx):
                if not idx:
                    return float("nan")
                eo = sum(float(np.sum(one[i:i + fl] ** 2)) for i in idx) + 1e-18
                em = sum(float(np.sum(many[i:i + fl] ** 2)) for i in idx) + 1e-18
                return 10.0 * np.log10(em / eo)

            u, v_ = ratio(unv), ratio(voi)
            print(f"\n  ensemble coherence (psola N-voice chord vs single note; frames classified on DRY):")
            print(f"    unvoiced ({len(unv):4d} frames)  {u:+6.2f} dB   "
                  f"(the sibilant-collapse question)")
            print(f"    voiced   ({len(voi):4d} frames)  {v_:+6.2f} dB   (control)")
            if len(unv) < 30:
                print(f"    NOTE: only {len(unv)} unvoiced frames -- this material is nearly all "
                      f"sustained vowels,\n          so the unvoiced figure is weakly supported. "
                      f"Not a clean test of VH-005.")
            demo_report["coherence"] = {"unvoiced_db": u, "voiced_db": v_,
                                         "frames_unvoiced": len(unv), "frames_voiced": len(voi)}

        print()
        report[folder] = demo_report

    with open(os.path.join(demo_root, "battery_report.json"), "w") as f:
        json.dump(report, f, indent=2)
    print(f"Full JSON written to {os.path.join(demo_root, 'battery_report.json')}")


if __name__ == "__main__":
    main()
