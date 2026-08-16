# Demo — Spoken digits (male, ~110-190 Hz) — the consonant stress case

Source: `speech_dry.wav`, 30.0 s, 48 kHz mono. Chords: `0:48,52,55; 7.5:45,48,52; 15:50,53,57; 22.5:48,55,60,64`, Mode A (detachment) for the harmony variants.

`speech_reel.wav` is the whole comparison in one file, 214 s, with 0.6 s gaps between segments. Individual segments are in `wav/`.

## Segment order

- **  0.0 s — dry** — the source, unprocessed
- ** 30.6 s — quality path (TD-PSOLA) — AFTER** — this change
- ** 61.2 s — quality path (TD-PSOLA) — BEFORE** — the previous build, same material
- ** 91.8 s — fast path (delay-line) — AFTER** — this change
- **122.4 s — fast path (delay-line) — BEFORE** — the previous build, same material
- **153.0 s — both engines, blended — AFTER** — this change
- **183.6 s — both engines, blended — BEFORE** — the previous build, same material

Each AFTER is immediately followed by the same configuration BEFORE, on the same passage. The older build is placed second on purpose: the second of a pair is heard most critically, which is the harder test for the change rather than the flattering one.

## Measured

Isolated clicks per render, by `tools/click_probe.py`. The dry source scores **11** — that is the floor, not zero, because real material contains real transients.

| variant | build | clicks | peak | rms |
|---|---|---|---|---|
| psola | after | 7 | 0.948 | 0.0862 |
| psola | before | 160 | 0.999 | 0.1153 |
| granular | after | 3 | 0.823 | 0.0929 |
| granular | before | 10 | 0.857 | 0.0927 |
| blend | after | 7 | 0.948 | 0.0861 |
| blend | before | 159 | 0.999 | 0.1149 |

### Ensemble coherence

Four voices against one, same material, PSOLA. Four COHERENT copies give +12.0 dB; four INCOHERENT copies give +6.0 dB. `collapse` places the result between them: 0 is an ensemble, 1 is four copies of one voice.

| build | unvoiced dB | voiced dB | collapse |
|---|---|---|---|
| before | +10.01 | +8.56 | **0.66** |
| after | +6.19 | +7.02 | **0.03** |

The voiced row is the control and it moves too, by less. That is honest rather than ideal: the per-voice delay is applied to the WHOLE voice, not only to unvoiced frames, so it decorrelates everything a little and the fricatives a lot. Confining it to unvoiced frames would mean switching it in and out with voicing, which is the class of defect the handover crossfade exists to remove.

