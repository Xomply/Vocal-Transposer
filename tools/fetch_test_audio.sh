#!/usr/bin/env bash
# Fetch and prepare the two real-voice validation recordings.
#
# WHY A SCRIPT INSTEAD OF COMMITTING THE AUDIO. The material is Creative Commons from
# Freesound, redistributed through the MTG essentia-audio corpus. ATTRIBUTION.txt asks you
# to consult the original licences before redistributing it, and committing a megabyte of
# someone else's singing into this repo is redistribution. The exact recipe is small; the
# audio is not. So the recipe is what lives in git.
#
# The offsets and durations below are NOT arbitrary — they are what RESULTS.md's real-voice
# validation figures were measured on. Changing them invalidates the comparison.
#
# usage:  ./tools/fetch_test_audio.sh [destdir]      (default: repo root)

set -euo pipefail

DEST="${1:-.}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

command -v git >/dev/null || { echo "error: git not found"; exit 1; }
command -v ffmpeg >/dev/null || {
    echo "error: ffmpeg not found."
    echo "  Termux:  pkg install ffmpeg"
    echo "  Debian:  sudo apt install ffmpeg"
    exit 1
}

echo "Cloning MTG/essentia-audio (~217 MB, shallow)..."
git clone --depth 1 https://github.com/MTG/essentia-audio.git "$WORK/corpus"

REC="$WORK/corpus/recorded"
VARNAM="$REC/223582__gopalkoduri__carnatic-varnam-by-vignesh-in-abhogi-raaga.mp3"

# Common preparation: mono, 48 kHz, 5 s, loudness-normalised to -20 LUFS with -3 dBTP
# headroom. Mono because the analyser is mono; 48 kHz because every latency figure in the
# docs assumes it; -3 dBTP because a harmoniser SUMS voices and needs the headroom.
prep() { ffmpeg -v error -ss "$2" -i "$1" -ac 1 -ar 48000 -t 5 \
             -af "loudnorm=I=-20:TP=-3" -c:a pcm_s16le "$3" -y; }

echo "Preparing sustained_dry.wav (soprano, ~810 Hz, natural vibrato)..."
prep "$REC/long_voice.flac" 0 "$DEST/sustained_dry.wav"

# Offset 30 s is chosen deliberately: it is the first 5 s window whose F0 sits inside the
# 210-320 Hz band ATTRIBUTION.txt records. Earlier windows sit an octave lower, where the
# tambura drone competes with the voice and the excerpt stops being the intended test case.
echo "Preparing melody_dry.wav (Carnatic varnam, ~210-320 Hz, moving pitch)..."
prep "$VARNAM" 30 "$DEST/melody_dry.wav"

# --- demo material (milestone 3) -------------------------------------------------------
#
# Two further voices, neither of which resembles the two above. See ATTRIBUTION.txt for why
# each was chosen. These are used by tools/demo_reel.py and are not needed to run the tests.

echo "Preparing flamenco_dry.wav (male cante, ~157-390 Hz, heavy melisma)..."
ffmpeg -v error -i "$REC/flamenco.flac" -ac 1 -ar 48000 \
    -af "loudnorm=I=-20:TP=-3" -c:a pcm_s16le "$DEST/flamenco_dry.wav" -y

# The consonant case has to be ASSEMBLED: nothing in the corpus has this density of
# plosives and fricatives, and the reported clicking symptom named consonants specifically.
# Spoken digits cover /k/ and /s/ (six), /t/ (two, eight), /f/ (four, five), /v/ (seven).
echo "Preparing speech_dry.wav (spoken digits, ~110-190 Hz, consonant-dense)..."
git clone --depth 1 -q https://github.com/Jakobovski/free-spoken-digit-dataset.git "$WORK/fsdd"
python3 - "$WORK/fsdd/recordings" "$WORK/speech8k.wav" <<'PY'
import os, random, sys, wave
import numpy as np
rec, out = sys.argv[1], sys.argv[2]
random.seed(7)                      # fixed, so the file is byte-identical on every run
files = []
for rep in range(9):
    order = list(range(10)); random.shuffle(order)
    for d in order:
        cand = sorted(f for f in os.listdir(rec) if f.startswith("%d_theo_" % d))
        files.append(os.path.join(rec, cand[rep % len(cand)]))
sr = 8000
chunks = []
for f in files:
    w = wave.open(f)
    x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(float) / 32768.0
    chunks += [x / (np.max(np.abs(x)) + 1e-9) * 0.7, np.zeros(int(0.12 * sr))]
y = np.concatenate(chunks)
w = wave.open(out, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
w.writeframes((np.clip(y, -1, 1) * 32767).astype("<i2").tobytes()); w.close()
PY
ffmpeg -v error -i "$WORK/speech8k.wav" -ac 1 -ar 48000 -t 30 \
    -af "loudnorm=I=-20:TP=-3" -c:a pcm_s16le "$DEST/speech_dry.wav" -y

echo
echo "Done:"
ls -la "$DEST/sustained_dry.wav" "$DEST/melody_dry.wav" \
       "$DEST/flamenco_dry.wav" "$DEST/speech_dry.wav"
echo
echo "These two cover complementary failure modes: 'sustained' is a steady high note that"
echo "isolates grain behaviour, 'melody' has moving pitch and background accompaniment and"
echo "is where VH-002's clicking shows up. Test both; a fix that works on one often does"
echo "not work on the other."
echo
echo "Licences are those of the original Freesound uploads. See ATTRIBUTION.txt."
