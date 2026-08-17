#!/usr/bin/env python3
"""
build_voice_clips.py — assemble continuous sung passages from vocobox/human-voice-dataset's
single-note recordings.

WHY THIS EXISTS. See demo_new/ATTRIBUTION.md for the full story: every continuous-phrase
solo-singing corpus found (VocalSet, vocadito, the Jingju a cappella corpus) is hosted on
Zenodo or HuggingFace, both unreachable from a network-restricted environment. What IS
reachable and genuinely real, no-instrument, single-voice audio is
github.com/vocobox/human-voice-dataset — MIT licensed, one real singer, but sliced into
~1 s single-note takes rather than delivered as continuous phrases. This assembles those
takes into continuous-sounding passages: sequence distinct pitches (or distinct takes of
the same pitch, to avoid literal loops), join with short equal-power raised-cosine
crossfades so there is no click at the seam, and normalise the result.

This is the same category of move as tools/fetch_test_audio.sh's assembly of speech_dry.wav
from Free Spoken Digit Dataset recordings: build honestly from real recordings when no
single existing recording satisfies the brief, and document the assembly rather than
disguise it.

USAGE
    python3 tools/build_voice_clips.py fetch <workdir>     # clone the note bank
    python3 tools/build_voice_clips.py list <workdir>      # show available note stems
    python3 tools/make_voice_clips.py <workdir> <outdir>   # build the four demo clips
                                                            # (see that script for the
                                                            # actual note sequences)
"""

import numpy as np
import wave
import os
import sys
import subprocess

REPO_URL = "https://github.com/vocobox/human-voice-dataset.git"
TARGET_SR = 48000


def fetch(workdir):
    dest = os.path.join(workdir, "human-voice-dataset")
    if os.path.exists(dest):
        print(f"already present: {dest}")
        return dest
    subprocess.run(["git", "clone", "--depth", "1", REPO_URL, dest], check=True)
    return dest


def note_dirs(workdir):
    base = os.path.join(workdir, "human-voice-dataset", "data", "voices", "martin")
    return (
        os.path.join(base, "notes", "exports", "mono"),
        os.path.join(base, "voyels", "exports", "mono"),
    )


def list_notes(workdir):
    notes_dir, voyels_dir = note_dirs(workdir)
    for d in (notes_dir, voyels_dir):
        if not os.path.isdir(d):
            print(f"(missing: {d} -- run 'fetch' first)")
            continue
        for f in sorted(os.listdir(d)):
            if f.endswith(".wav"):
                print(os.path.join(os.path.basename(d), f))


# --- audio assembly primitives ----------------------------------------------

def load(stem, workdir):
    notes_dir, voyels_dir = note_dirs(workdir)
    p1 = os.path.join(notes_dir, stem + ".wav")
    p2 = os.path.join(voyels_dir, stem + ".wav")
    path = p1 if os.path.exists(p1) else p2
    if not os.path.exists(path):
        raise FileNotFoundError(f"note stem not found in dataset: {stem}")
    with wave.open(path, "rb") as w:
        sr = w.getframerate()
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64) / 32768.0
    return x, sr


def resample(x, sr, target_sr):
    if sr == target_sr:
        return x
    n_out = int(round(len(x) * target_sr / sr))
    t_in = np.linspace(0, 1, len(x), endpoint=False)
    t_out = np.linspace(0, 1, n_out, endpoint=False)
    return np.interp(t_out, t_in, x)


def trim_silence(x, thresh=0.02, pad=0.005, sr=TARGET_SR):
    env = np.abs(x)
    above = np.where(env > thresh)[0]
    if len(above) == 0:
        return x
    pad_n = int(pad * sr)
    a = max(0, above[0] - pad_n)
    b = min(len(x), above[-1] + pad_n)
    return x[a:b]


def fade(x, n, kind="both"):
    n = min(n, len(x) // 2)
    if n <= 0:
        return x
    win = 0.5 * (1 - np.cos(np.linspace(0, np.pi, n)))  # raised cosine, C1
    x = x.copy()
    if kind in ("in", "both"):
        x[:n] *= win
    if kind in ("out", "both"):
        x[-n:] *= win[::-1]
    return x


def crossfade_concat(a, b, n):
    n = min(n, len(a), len(b))
    if n <= 0:
        return np.concatenate([a, b])
    t = np.linspace(0, np.pi / 2, n)
    fade_out = np.cos(t) ** 2
    fade_in = np.sin(t) ** 2
    head = a[:-n] if n > 0 else a
    overlap = a[-n:] * fade_out + b[:n] * fade_in
    tail = b[n:]
    return np.concatenate([head, overlap, tail])


def write_wav(path, x, sr):
    x = np.clip(x, -1.0, 1.0)
    pcm = (x * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(pcm.tobytes())


def build_sequence(notes, out_path, workdir, sr=TARGET_SR, xfade_ms=60, gap_ms=25, seed=0):
    """notes: list of (note_stem, hold_seconds). Writes a continuous-sounding passage."""
    xfade_n = int(xfade_ms / 1000 * sr)
    gap_n = int(gap_ms / 1000 * sr)

    segments = []
    for stem, hold in notes:
        x, sr0 = load(stem, workdir)
        x = resample(x, sr0, sr)
        x = trim_silence(x, sr=sr)
        need = int(hold * sr)
        if len(x) < need:
            # held note is longer than the source take: repeat with a crossfaded seam
            reps = []
            total = 0
            first = True
            while total < need:
                seg = x if first else fade(x.copy(), xfade_n, "in")
                reps.append(seg)
                total += len(seg) - (xfade_n if not first else 0)
                first = False
            x = reps[0]
            for seg in reps[1:]:
                x = crossfade_concat(x, seg, xfade_n)
        x = x[:need] if len(x) > need else x
        x = fade(x, xfade_n, "both")
        segments.append(x)

    out = segments[0]
    for seg in segments[1:]:
        out = np.concatenate([out, np.zeros(gap_n)])
        out = crossfade_concat(out, seg, xfade_n)

    peak = np.max(np.abs(out)) + 1e-9
    target_peak = 10 ** (-3 / 20)
    out = out * (target_peak / peak)

    write_wav(out_path, out, sr)
    return out


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    cmd, workdir = sys.argv[1], sys.argv[2]
    if cmd == "fetch":
        fetch(workdir)
    elif cmd == "list":
        list_notes(workdir)
    else:
        print(f"unknown command: {cmd}")
        sys.exit(1)


if __name__ == "__main__":
    main()
