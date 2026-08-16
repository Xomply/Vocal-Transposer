# Demo — Flamenco cante (male, ~157-390 Hz)

Source: `flamenco_dry.wav`, 14.2 s, 48 kHz mono. Chords: `0:57,60,64; 3.5:55,59,62; 7:53,57,60,65; 10.5:57,60,64,69`, Mode A (detachment) for the harmony variants.

`flamenco_reel.wav` is the whole comparison in one file, 104 s, with 0.6 s gaps between segments. Individual segments are in `wav/`.

## Segment order

- **  0.0 s — dry** — the source, unprocessed
- ** 14.8 s — quality path (TD-PSOLA) — AFTER** — this change
- ** 29.7 s — quality path (TD-PSOLA) — BEFORE** — the previous build, same material
- ** 44.5 s — fast path (delay-line) — AFTER** — this change
- ** 59.3 s — fast path (delay-line) — BEFORE** — the previous build, same material
- ** 74.1 s — both engines, blended — AFTER** — this change
- ** 89.0 s — both engines, blended — BEFORE** — the previous build, same material

Each AFTER is immediately followed by the same configuration BEFORE, on the same passage. The older build is placed second on purpose: the second of a pair is heard most critically, which is the harder test for the change rather than the flattering one.

## Measured

Isolated clicks per render, by `tools/click_probe.py`. The dry source scores **1** — that is the floor, not zero, because real material contains real transients.

| variant | build | clicks | peak | rms |
|---|---|---|---|---|
| psola | after | 1 | 0.470 | 0.0442 |
| psola | before | 77 | 0.612 | 0.0606 |
| granular | after | 15 | 0.471 | 0.0545 |
| granular | before | 25 | 0.476 | 0.0567 |
| blend | after | 1 | 0.470 | 0.0442 |
| blend | before | 77 | 0.612 | 0.0605 |

### Ensemble coherence

Four voices against one, same material, PSOLA. Four COHERENT copies give +12.0 dB; four INCOHERENT copies give +6.0 dB. `collapse` places the result between them: 0 is an ensemble, 1 is four copies of one voice.

| build | unvoiced dB | voiced dB | collapse |
|---|---|---|---|
| before | +10.98 | +10.21 | **0.82** |
| after | +6.82 | +7.62 | **0.13** |

The voiced row is the control and it moves too, by less. That is honest rather than ideal: the per-voice delay is applied to the WHOLE voice, not only to unvoiced frames, so it decorrelates everything a little and the fricatives a lot. Confining it to unvoiced frames would mean switching it in and out with voicing, which is the class of defect the handover crossfade exists to remove.

