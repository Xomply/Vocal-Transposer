#!/usr/bin/env python3
"""
demo_reel.py — build an A/B listening reel for one voice, and measure what is in it.

WHY A REEL AND NOT A FOLDER OF FILES. Comparing two renders means switching between them
at the same instant in the material, and a folder of files does not let you do that — by
the time a player has loaded the second file the memory of the first is gone. A single WAV
with the same passage repeated under each configuration, separated by a short gap, is the
form that actually lets a difference be heard. It is also the form that survives being sent
to somebody else.

WHAT IT PRODUCES

  <out>/<name>_reel.wav   the whole comparison, in order, with 0.6 s gaps.
  <out>/wav/              every segment individually, for looping one of them.
  <out>/<name>_DEMO.md    the listening guide and the measured table.

THE SEGMENT ORDER IS DELIBERATE. Dry first, so the ear has the source in memory; then each
configuration AFTER immediately followed by the same configuration BEFORE. After-then-before
rather than before-then-after because the second of a pair is the one heard most critically,
and putting the older build there is the harder test for the change.

THE BEFORE BUILD IS A REAL BUILD OF THE OLD CODE, not a runtime flag. A flag would only
disable what somebody remembered to make switchable; a separate binary cannot flatter the
new code by construction.

usage:
  python3 tools/demo_reel.py flamenco_dry.wav demo/ flamenco \\
      --after ./build/vh_render --before /tmp/vtbase/build/vh_render \\
      --chords "0:60,64,67; 4:57,60,64; 8:59,62,67" --root 60
"""

import argparse
import os
import subprocess
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import click_probe  # noqa: E402
import ensemble_probe  # noqa: E402

# variant suffix produced by vh_render, and the label used in the guide
VARIANTS = [
    ("psola", "quality path (TD-PSOLA)"),
    ("granular", "fast path (delay-line)"),
    ("blend", "both engines, blended"),
]


def read(path):
    with wave.open(path, "rb") as w:
        n, sr = w.getnframes(), w.getframerate()
        raw = w.readframes(n)
    return np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0, float(sr)


def write(path, x, sr):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(sr))
        w.writeframes((np.clip(x, -1.0, 1.0) * 32767.0).astype("<i2").tobytes())


def render(binary, source, outdir, tag, chords, root):
    os.makedirs(outdir, exist_ok=True)
    subprocess.run([binary, source, outdir + "/", tag, chords, str(root)],
                   check=True, stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("source")
    ap.add_argument("outdir")
    ap.add_argument("name")
    ap.add_argument("--after", required=True)
    ap.add_argument("--before", required=True)
    ap.add_argument("--chords", required=True)
    ap.add_argument("--root", type=int, default=60)
    ap.add_argument("--title", default="")
    args = ap.parse_args()

    wavdir = os.path.join(args.outdir, "wav")
    os.makedirs(wavdir, exist_ok=True)
    adir = os.path.join(wavdir, "after")
    bdir = os.path.join(wavdir, "before")

    render(args.after, args.source, adir, args.name, args.chords, args.root)
    render(args.before, args.source, bdir, args.name, args.chords, args.root)

    dry, sr = read(args.source)
    gap = np.zeros(int(0.6 * sr))

    reel = [dry, gap]
    guide = [("dry", "the source, unprocessed", args.source)]
    rows = []

    for suffix, label in VARIANTS:
        a = os.path.join(adir, "%s_%s.wav" % (args.name, suffix))
        b = os.path.join(bdir, "%s_%s.wav" % (args.name, suffix))
        if not (os.path.exists(a) and os.path.exists(b)):
            continue
        xa, _ = read(a)
        xb, _ = read(b)
        reel += [xa, gap, xb, gap]
        guide.append(("%s — AFTER" % label, "this change", a))
        guide.append(("%s — BEFORE" % label, "the previous build, same material", b))

        for tag, x, path in (("after", xa, a), ("before", xb, b)):
            clicks = click_probe.find_clicks(x, sr)
            rows.append({"variant": suffix, "build": tag, "clicks": len(clicks),
                         "peak": float(np.max(np.abs(x))),
                         "rms": float(np.sqrt(np.mean(x ** 2)))})

    reelpath = os.path.join(args.outdir, "%s_reel.wav" % args.name)
    write(reelpath, np.concatenate(reel), sr)

    dryclicks = len(click_probe.find_clicks(dry, sr))

    # --- ensemble coherence, the other half of the story ----------------------------
    # One voice against four, on the same material, in both builds. See
    # tools/ensemble_probe.py for why this and not a bare energy figure.
    ens = {}
    for tag, binary in (("after", args.after), ("before", args.before)):
        d1 = os.path.join(wavdir, "ens_%s_1" % tag)
        d4 = os.path.join(wavdir, "ens_%s_4" % tag)
        render(binary, args.source, d1, "e", "0:60", 60)
        render(binary, args.source, d4, "e", "0:60,64,67,72", 60)
        x1, _ = read(os.path.join(d1, "e_psola.wav"))
        x4, _ = read(os.path.join(d4, "e_psola.wav"))
        comp = 10.0 * np.log10(4)
        u, v, nu, nv = ensemble_probe.auto_report(x1, x4, sr, 4, comp)
        coh, inc = 20.0 * np.log10(4), 10.0 * np.log10(4)
        ens[tag] = {"unvoiced": u, "voiced": v, "collapse": (u - inc) / (coh - inc),
                    "nu": nu, "nv": nv}

    # --- the guide -----------------------------------------------------------------
    lines = []
    lines.append("# Demo — %s" % (args.title or args.name))
    lines.append("")
    lines.append("Source: `%s`, %.1f s, %.0f kHz mono. Chords: `%s`, Mode A (detachment) "
                 "for the harmony variants." % (os.path.basename(args.source),
                                                len(dry) / sr, sr / 1000.0, args.chords))
    lines.append("")
    lines.append("`%s_reel.wav` is the whole comparison in one file, %.0f s, with 0.6 s "
                 "gaps between segments. Individual segments are in `wav/`."
                 % (args.name, len(np.concatenate(reel)) / sr))
    lines.append("")
    lines.append("## Segment order")
    lines.append("")
    t = 0.0
    for label, note, path in guide:
        x, _ = read(path)
        lines.append("- **%5.1f s — %s** — %s" % (t, label, note))
        t += len(x) / sr + 0.6
    lines.append("")
    lines.append("Each AFTER is immediately followed by the same configuration BEFORE, on "
                 "the same passage. The older build is placed second on purpose: the second "
                 "of a pair is heard most critically, which is the harder test for the "
                 "change rather than the flattering one.")
    lines.append("")
    lines.append("## Measured")
    lines.append("")
    lines.append("Isolated clicks per render, by `tools/click_probe.py`. The dry source "
                 "scores **%d** — that is the floor, not zero, because real material "
                 "contains real transients." % dryclicks)
    lines.append("")
    lines.append("| variant | build | clicks | peak | rms |")
    lines.append("|---|---|---|---|---|")
    for r in rows:
        lines.append("| %s | %s | %d | %.3f | %.4f |"
                     % (r["variant"], r["build"], r["clicks"], r["peak"], r["rms"]))
    lines.append("")
    lines.append("### Ensemble coherence")
    lines.append("")
    lines.append("Four voices against one, same material, PSOLA. Four COHERENT copies give "
                 "+12.0 dB; four INCOHERENT copies give +6.0 dB. `collapse` places the "
                 "result between them: 0 is an ensemble, 1 is four copies of one voice.")
    lines.append("")
    lines.append("| build | unvoiced dB | voiced dB | collapse |")
    lines.append("|---|---|---|---|")
    for tag in ("before", "after"):
        e = ens[tag]
        lines.append("| %s | %+.2f | %+.2f | **%.2f** |"
                     % (tag, e["unvoiced"], e["voiced"], e["collapse"]))
    lines.append("")
    lines.append("The voiced row is the control and it moves too, by less. That is honest "
                 "rather than ideal: the per-voice delay is applied to the WHOLE voice, not "
                 "only to unvoiced frames, so it decorrelates everything a little and the "
                 "fricatives a lot. Confining it to unvoiced frames would mean switching it "
                 "in and out with voicing, which is the class of defect the handover "
                 "crossfade exists to remove.")
    lines.append("")

    path = os.path.join(args.outdir, "%s_DEMO.md" % args.name)
    open(path, "w").write("\n".join(lines) + "\n")

    print("reel  %s  (%.0f s)" % (reelpath, len(np.concatenate(reel)) / sr))
    print("guide %s" % path)
    print("dry source scores %d clicks" % dryclicks)
    for r in rows:
        print("  %-9s %-6s clicks %3d  peak %.3f" % (r["variant"], r["build"], r["clicks"], r["peak"]))


if __name__ == "__main__":
    main()
