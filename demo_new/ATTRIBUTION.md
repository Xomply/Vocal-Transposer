# Attribution — proof-of-concept demo voice material

## Why this material, and not more of the essentia-audio corpus

The repo's existing validation voices (`sustained_dry`, `melody_dry`, `flamenco_dry`,
`speech_dry`) all come from `MTG/essentia-audio` on GitHub. That corpus was re-checked for
this batch and does not contain four *further* clips that are simultaneously: real solo
singing, single voice, no other instruments, and 30 s long.

- `long_voice.flac` (the soprano used for `sustained_dry`) is only **12 s long in total** —
  not enough for even one clean 30 s excerpt, let alone four non-overlapping ones.
- The Carnatic varnam (`melody_dry`'s source) has a **tambura drone under the voice for its
  entire length** — ATTRIBUTION.txt already says so. It fails "no other instruments."
- `flamenco.flac` has **guitar behind the voice** throughout. Same problem.

Every other real solo-singing dataset found in research (VocalSet, vocadito, the Jingju a
cappella corpus, Choral Singing Dataset) is hosted on Zenodo or HuggingFace, both of which
are outside this environment's network allowlist (`host_not_allowed`). Only `github.com` and
its content-delivery subdomains (`raw.githubusercontent.com`, `codeload.github.com`,
`release-assets.githubusercontent.com`) are reachable.

## Source: vocobox/human-voice-dataset

<https://github.com/vocobox/human-voice-dataset>, MIT licensed, real recordings of a single
singer ("Martin") committed directly to the repository as individual WAV files — one file
per held note or vowel, roughly 1 second each, spanning D2–A4 plus a small vowel set at C3.
No instruments, no other voices, no synthesis: it is real human singing, just sliced into
single-note takes rather than delivered as continuous phrases.

**This is genuinely different from the repo's other four voices**, which are continuous
recordings. The four demo clips here are *assembled* from that note bank — sequencing
different pitches (picking distinct takes among the dataset's repeats so no two adjacent
notes are literally the same audio) and joining them with 60 ms equal-power raised-cosine
crossfades so the result reads as one continuous sung passage rather than a stack of
clicks. `tools/build_voice_clips.py` has the assembly code; `tools/make_clips.py` has the
four note sequences.

This is the same category of move the repo already made once: `speech_dry.wav` is
assembled from Free Spoken Digit Dataset recordings rather than found as one continuous
recording, because "no recording in the corpus has this density of plosives and
fricatives" (see `ATTRIBUTION.txt`). Here the constraint was availability under the network
allowlist rather than phoneme density, but the resolution is the same: build the material
honestly from real recordings, document how, rather than force-fit an existing corpus that
doesn't actually satisfy the brief.

## The four clips

| Clip | Character | Register | Assembly |
|---|---|---|---|
| `vh_demo1_sustain.wav` | slow scalar ascent/descent, long holds (2–3 s each) | D2–D3 | the steady-pitch case, closest in spirit to `sustained_dry` |
| `vh_demo2_melody.wav` | uneven rhythm, wide melodic leaps (up to ~2 octaves note-to-note) | D#2–A4 | the moving-pitch case, closest in spirit to `melody_dry` |
| `vh_demo3_arpeggio.wav` | faster, arpeggio-like, mostly the top of the singer's range | C3–A#4 | exercises PSOLA's upper range and epoch-tracking at higher F0 |
| `vh_demo4_lowvowel.wav` | slow, alternates sung notes with sustained vowels (a/e/i/o/u/ou) | D2–G#2 | timbral/vowel variety at the bottom of the range |

All four: mono, 48 kHz, 16-bit PCM, ~27–31 s, peak-normalised to −3 dBFS, silent start (no
priming tone) so the "starts with silence" lesson from `RESULTS.md` bug #5/#6 applies to
this material too.

## Licence

`vocobox/human-voice-dataset` is MIT licensed (see the `LICENSE` file in that repository).
The four assembled clips inherit that licence. They are not committed to this repository for
the same reason the other four voices aren't (see `ATTRIBUTION.txt` and
`tools/fetch_test_audio.sh`) — regenerate them with `tools/build_voice_clips.py` and
`tools/make_clips.py`, which first `git clone --depth 1
https://github.com/vocobox/human-voice-dataset.git`.
