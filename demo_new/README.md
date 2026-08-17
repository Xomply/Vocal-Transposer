# demo_new — proof-of-concept demo set

Four real solo voices × four different MIDI progressions, through every engine
configuration, with the full test suite run across all of it.

**Start with `RESULTS.md`** — measurements paired with what each one should sound like, so
the numbers can be checked against your ears.

- `ATTRIBUTION.md` — where the voices came from, why they were assembled rather than found,
  and the licence
- `midi_progressions.md` — the four chord progressions and why each is shaped that way
- `RESULTS.md` — measurements, listening guide, and the one measurement that was wrong
- `battery_report.json` — the raw numbers, machine-readable

## Layout

```
vh_demo1_sustain/    demo1_{dry,granular,psola,blend,blend_aligned,mix,modeB}.wav
vh_demo2_melody/     demo2_{...}.wav
vh_demo3_arpeggio/   demo3_{...}.wav
vh_demo4_lowvowel/   demo4_{...}.wav
one_voice/           one{1..4}_*.wav   single-note renders, for the coherence comparison
```

The four dry sources live in `audio/` and, like the repo's other voice material, are not
committed — regenerate with `tools/build_voice_clips.py` and `tools/make_voice_clips.py`.

## Pairings

| Demo | Voice | MIDI | Exercises |
|---|---|---|---|
| 1 | slow scalar, long holds, D2–D3 | melodic jumps, one note at a time | shifter behaviour on a single leaping line |
| 2 | wide leaps, uneven rhythm, D#2–A4 | 4-note chords spread 2–3+ octaves | PSOLA's ±6 st limit; the missing source-filter engine |
| 3 | fast, high register, C3–A#4 | close chords, changing every 2 s | granular re-lock under fast retargeting; ensemble quality on tight voicings |
| 4 | slow, vowel-alternating, D2–G#2 | held bass under a moving melody | formant preservation; independence of two lines; the unused `envelopeWarp` |

## Headline

48/48 tests pass. All 28 renders finite, no dropouts. All four dry sources score 0 clicks,
so every click counted is the engine's own. Granular runs 27–178 clicks against PSOLA's
0–3, and sits 121–403 Hz higher in spectral centroid on every demo — both engines behaving
exactly as their algorithms predict, which is the whole argument for having two.
