#!/usr/bin/env python3
"""
make_voice_clips.py — the four note sequences for the proof-of-concept demo voices.

See build_voice_clips.py for the assembly mechanics and demo_new/ATTRIBUTION.md for why
this exists. This file is just the four sequences, kept separate from the assembly code so
the musical choices are easy to read and change independently of the DSP.

Each sequence is (note_stem, hold_seconds). note_stem must exist as a file name (minus
.wav) under vocobox/human-voice-dataset's martin/notes or martin/voyels export folders —
run `python3 tools/build_voice_clips.py list <workdir>` to see what's available.

USAGE
    python3 tools/build_voice_clips.py fetch /tmp/voicework
    python3 tools/make_voice_clips.py /tmp/voicework audio/
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_voice_clips import build_sequence  # noqa: E402

# Clip 1 -- "sustain": slow scalar ascent/descent, long holds. Steady-pitch case, closest
# in spirit to the repo's existing sustained_dry.
CLIP1_SUSTAIN = [
    ("D2", 3.0), ("E2", 2.5), ("F#2", 2.5), ("G#2", 2.5),
    ("A3", 3.0), ("B3", 2.5), ("C#3", 2.5),
    ("D3-2", 3.0), ("C#3-2", 2.0), ("B3-2", 2.0),
    ("A3-2", 2.5), ("G#2-2", 2.5),
]

# Clip 2 -- "melody": more melodic jumps, uneven rhythm, wider leaps. Moving-pitch case,
# closest in spirit to the repo's existing melody_dry.
CLIP2_MELODY = [
    ("D#2", 1.5), ("A3", 1.3), ("F#2-2", 1.3), ("A#3", 1.7),
    ("D#3", 1.3), ("G#3", 1.5), ("F3", 1.2), ("A4", 1.4),
    ("D#3-2", 1.3), ("A#3-2", 1.6), ("F#2-3", 1.3), ("C#3-2", 1.5),
    ("G#3-2", 1.3), ("D#2-ko", 1.6), ("A3-2", 1.4), ("F3-2", 1.3),
    ("A#4", 1.5), ("D#3-3", 1.3), ("A3-3", 1.6), ("F#2", 1.3),
    ("G#2-3", 1.6), ("D#2", 1.5),
]

# Clip 3 -- "arpeggio": high register, faster, arpeggio-like. Exercises the top of the
# singer's range and PSOLA's epoch tracking at higher F0.
CLIP3_ARPEGGIO = [
    ("E3", 1.1), ("G3", 1.1), ("B3-3", 1.0), ("D3-2", 1.1),
    ("F3-3", 1.0), ("A3-4", 1.1), ("C3-2", 1.0), ("E3-2", 1.1),
    ("G3-2", 1.0), ("B3-5", 1.1), ("A4-2", 1.2), ("G3-3", 1.0),
    ("F3", 1.1), ("D3-3", 1.0), ("B3", 1.1), ("G3", 1.0),
    ("E3-3", 1.1), ("C3", 1.0), ("A3", 1.2), ("F3-2", 1.0),
    ("D3", 1.1), ("A#4-2", 1.2), ("G3-2", 1.1), ("E3", 1.1),
    ("C#3-3", 1.2), ("A3-2", 1.2),
]

# Clip 4 -- "lowvowel": low register, alternates sung notes with sustained vowels. A
# different timbral character, exercising vowel/formant variety at the bottom of the range.
CLIP4_LOWVOWEL = [
    ("D2-3", 2.5), ("_-a-c3", 1.8), ("D#2", 2.2), ("_-o-c3-2", 1.8),
    ("E2", 2.5), ("_-u-c3", 1.8), ("F2", 2.2), ("_-i-c3-2", 1.8),
    ("F#2-2", 2.5), ("_-e-c3-2", 1.8), ("G2-2", 2.2), ("_-ou-c3-2", 1.8),
    ("G#2-4", 2.5), ("F2", 2.0),
]

CLIPS = [
    (CLIP1_SUSTAIN, "vh_demo1_sustain.wav", 1),
    (CLIP2_MELODY, "vh_demo2_melody.wav", 2),
    (CLIP3_ARPEGGIO, "vh_demo3_arpeggio.wav", 3),
    (CLIP4_LOWVOWEL, "vh_demo4_lowvowel.wav", 4),
]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    workdir, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    for notes, name, seed in CLIPS:
        total = sum(h for _, h in notes)
        out_path = os.path.join(outdir, name)
        build_sequence(notes, out_path, workdir, seed=seed)
        print(f"{name}: {total:.1f}s of notes -> {out_path}")


if __name__ == "__main__":
    main()
