# Results — Milestone 1: the principles work

*Updated after testing on REAL recordings. Three further bugs surfaced within minutes of
using real audio that 36 synthetic tests had missed entirely — see "Bugs found on real
audio" below. Test count is now 40.*

*Everything here is measured on the container that built the code (g++ 13.3, Linux, x86-64).
Absolute CPU figures will differ on the target Windows machine; the RATIOS between
configurations should hold. Re-run `vh_bench` there before trusting any number.*

---

## Is it real?

Yes. The clearest single piece of evidence: the sung phrase contains an A2 with its
harmonics. During the A-minor chord the output contains strong energy at **E3 (164.8 Hz)
and C4 (261.6 Hz) — notes that are not in the input at all**, at 112× and 156× their dry
levels. The keyboard is supplying harmony that the voice never sang.

| Partial | Dry | Wet (PSOLA) | Ratio |
|---|---|---|---|
| A2 110.0 Hz (sung) | 0.00796 | 0.00352 | 0.4× |
| **E3 164.8 Hz** | 0.00003 | 0.00300 | **112×** |
| A3 220.0 Hz | 0.00563 | 0.00589 | 1.0× |
| **C4 261.6 Hz** | 0.00002 | 0.00253 | **157×** |

---

## Pitch accuracy

Steady vowel, output F0 measured against target. Under ~10 cents is inaudible inside a
chord; under ~25 is finer than real singers tune.

| Source F0 | Interval | Granular | PSOLA |
|---|---|---|---|
| 110 Hz | −12 st | n/a | n/a |
| 110 Hz | −5 st | +0.2 | −1.7 |
| 110 Hz | +3 st | +0.3 | −0.6 |
| 110 Hz | +7 st | −0.2 | +1.7 |
| 110 Hz | +12 st | +1.1 | +2.0 |
| 150 Hz | −7 st | −4.2 | −2.3 |
| 150 Hz | +4 st | +1.1 | +0.4 |
| 150 Hz | +7 st | +0.8 | +0.8 |
| 220 Hz | −5 st | +0.7 | +0.6 |
| 220 Hz | +5 st | +0.8 | +1.6 |
| 220 Hz | +12 st | +1.0 | +0.6 |

Worst case **4.2 cents**. The `n/a` row is a limit of the ruler, not the shifter: 110 Hz
down an octave is 55 Hz, below the 70 Hz floor of the YIN instance doing the measuring.

---

## CPU

48 kHz. "% budget" is p99 processing time against the wall clock available before the
callback is late. Under 50% is the honest headroom target, because the machine will also
be running a DAW and Windows.

| Configuration | mean µs | p99 µs | % budget |
|---|---|---|---|
| 1 voice, PSOLA, 128 | 85 | 258 | 9.7% |
| 8 voices, PSOLA, 128 | 93 | 337 | 12.6% |
| 16 voices, PSOLA, 128 | 101 | 334 | 12.5% |
| 8 voices, both engines, 128 | 120 | 262 | 9.8% |
| 16 voices, both engines, 128 | 137 | 317 | 11.9% |
| 8 voices, PSOLA, 64 | 47 | 128 | 9.6% |
| **16 voices, both engines, 64** | 71 | 163 | **12.2%** |

**The headline: 16 voices through both engines at a 64-sample buffer uses 12% of budget.**
Voice count barely moves the number — 1 voice and 16 cost nearly the same — because
analysis is shared. That is the architectural claim, confirmed by measurement rather than
argument.

The `max µs` column (~2 ms) is container scheduler noise, not algorithmic. It is exactly
what LatencyMon exists to characterise properly on the target machine.

---

## Latency

| Stage | Samples | ms |
|---|---|---|
| YIN acquisition (worst case, 70 Hz) | 1372 | 28.6 |
| Granular shifter (at rest) | 580 | 12.1 |
| PSOLA (worst case) | 1372 | 28.6 |

These match the predictions in `latency-budget.md` closely enough to be reassuring, and
the granular path's 12.1 ms lands inside the range that document identified as reachable.

Remember the two-clock distinction: YIN's 28.6 ms is a **convergence** cost paid at a
vocal phrase entry, not per block. Mode B does not pay it at all.

---

## Listening guide

All files 48 kHz mono. Listen in this order:

1. **`dry.wav`** — the synthetic input. A sung phrase: held vowel with vibrato → vowel
   change at constant pitch → fricative → legato note change → low sustained note. It
   sounds like a synthesiser singing, because it is one. Judge the processing by comparing
   dry against wet, not by whether the dry sounds human.

2. **`wet_granular.wav`** — fast path only. Listen for: it *works*, it's cheap, and it has
   the chipmunk character on upward voices because it resamples. This is the sound of
   latency being bought with quality.

3. **`wet_psola.wav`** — quality path only. The same chords with formants held. The
   difference from the previous file is the entire argument for the second engine.

4. **`wet_blend.wav`** vs **`wet_blend_aligned.wav`** — both engines, handover policy,
   alignment 0 and 1. The unaligned one is what Bloomberg's rig does; the aligned one is
   phase-coherent and gives up the latency advantage. This is the dial.

5. **`mix_psola.wav`** — harmony plus dry voice, i.e. what the room actually hears.

6. **`wet_modeB_interval.wav`** — Mode B. Intervals from a declared root; the singer's own
   intonation and vibrato ride through untouched.

**What you should hear as flaws, all known and documented:** the fricative is where it
sounds worst (four voices passing the same /s/ through, summing to one mono burst — the
polyphonic collapse issue, measured at 1.7× the dry energy); note onsets are softer than
they should be; and there is no release envelope shaping beyond a linear 8 ms ramp.

---

## Bugs found and fixed during this milestone

Recorded because each was found by measurement rather than by reading, and each had a
symptom that pointed somewhere other than its cause.

**1. PSOLA emitted an octave below target.** Grains centred at the emit position wrote
their first half into audio that had already been output, so half of every grain was
silently discarded and the overlap-add sum was broken. Every grain looked correct in
isolation. *Fix:* run the synthesis clock a grain half-length ahead of the output, with an
explicit "cleared up to" watermark so the accumulator is zeroed **ahead** of where grains
land, never behind.

**2. The blend was 6 dB quieter than either engine alone.** Note-on sized its lookback
from the fast engine's latency, which starves the slower engine — PSOLA's residency checks
failed and it dropped grains. The symptom reads as a mixing fault and sends you to inspect
the blend weights, which are fine. *Fix:* lookback must satisfy the slowest engine.
Regression test added. **This is the exact failure the architecture doc predicted would
appear "when a real quality engine lands".**

**3. PSOLA collapsed at 220 Hz** (errors of +493, −694, −2401 cents) while being perfect at
110 Hz. The epoch tracker's lowpass sat at a fixed 900 Hz, which isolates a 110 Hz voice
nicely but leaves a 220 Hz voice's first formant (~700 Hz) passing almost unattenuated. The
peak picker then sometimes locked onto an F1 crest, so the phase reference **jittered**
between cycles, grains stopped being coherent, and the overlap-add partially cancelled.
*Fix:* cutoff tracks the fundamental at ~1.8×, retuned with 5% hysteresis.

**4. YIN consumed the entire CPU budget.** 103% at a 128-sample block — and identically for
1 voice and 16, which was the clue: the cost was shared analysis, never the voices. *Fix:*
two-stage estimation — coarse search on a 4× decimated copy (16× less work), then full-rate
refinement in a narrow band around the winner. **13× faster, no measurable accuracy loss.**
Box-average decimation rather than plain subsampling, because aliasing would reintroduce
exactly the octave errors the design works hardest to avoid.

**Two test bugs, both worth recording** because the next person will have the same
instincts. The F0-hold test asserted bit-exact equality when the held value is legitimately
the last *confident* estimate, which drifts a few cents as the window slides into the
fricative. And the formant tests probed single frequencies — but a formant only shows
energy where a harmonic happens to land inside it, so with a 300 Hz fundamental a 700 Hz
probe measures nothing but window leakage. Band energy, compared *between* the two engines,
is the measurement that works.


---

## Real-voice validation

Two real human recordings, from the MTG essentia-audio corpus (CC material from
Freesound; see `ATTRIBUTION.txt` beside the audio):

- **sustained** — a soprano holding roughly 810 Hz with natural vibrato and drift. Near
  the top of YIN's range; period only ~59 samples, where every grain calculation is at its
  smallest.
- **melody** — a Carnatic varnam sung by Vignesh, roughly 210-320 Hz with clear melodic
  movement. Deliberately the harder case: it is not a solo dry vocal, so tambura and
  accompaniment give the pitch tracker competing periodic material.

### Spectrogram anomaly sweep, before and after fixes

Frame-to-frame level change over a 2048-point STFT. A large negative jump is a hole in the
output; a large positive one is a burst.

| Render | max jump BEFORE | max jump AFTER | drops < −12 dB |
|---|---|---|---|
| sustained, PSOLA | **220.2 dB** | 9.8 dB | 8 → **0** |
| sustained, granular | inf (non-finite) | 4.0 dB | 0 |
| sustained, blend | inf | 9.2 dB | 0 |
| melody, PSOLA | **241.8 dB** | 12.1 dB | 7 → **0** |
| melody, granular | inf | 7.9 dB | 0 |
| melody, blend | inf | 9.3 dB | 0 |

Subsonic (<60 Hz) energy is ≤0.01% everywhere — no DC or rumble artefacts. Crest factors
13-18 dB, consistent with the dry material.

---

## Bugs found on real audio

**5. The granular shifter emitted `inf`.** Real recordings begin with SILENCE; every
synthetic test began with a fully primed steady tone. With no F0 the geometry falls back to
a long grain, the read pointer's delay is briefly under the minimum, and the corrective
jump subtracts more than has been written. `pos_` went negative — and casting a negative
double to unsigned `Pos` is undefined behaviour, yielding a huge value. The interpolator's
fractional part, which the cubic assumes lies in [0,1), became astronomical and the
polynomial overflowed. Infinity in an audio buffer is the loudest possible sound.
*Fix:* `std::floor` instead of a raw cast, plus bounds-checked jumps that are skipped
rather than clamped when they would land outside written history.

**6. A note pressed before any audio existed was silent FOREVER.** `noteOn` sets
`cursor.next = noteOnPos - lookback`; because `Pos` is unsigned, pressing a key before the
ring holds `lookback` samples clamps the cursor to zero. From then on cursor and write head
advance in lockstep, one block apart — **the lag stays at one block permanently and never
heals.** PSOLA, starved of lookahead, failed its residency checks and emitted silence for
the whole life of that voice. This is what produced the 220 dB drops, and it bit exactly
where you would least want it to: the first chord of a session. *Fix:* a history gate —
skip the voice until its lag is genuine, so `writePos` pulls away. Starting a few tens of
milliseconds late is not a compromise; you cannot pitch-shift audio you have not heard yet.

**7. My first fix for #6 was wrong, and the tests caught it.** I added a backstop that fell
back to passthrough when no grains were placed in a block. But for a DOWNWARD shift the
synthesis spacing exceeds a block — an octave down at 150 Hz spaces grains 640 samples
apart against a 128-sample block — so most blocks legitimately place no grains and emit the
tails of earlier ones. **Zero grains is healthy operation, not starvation.** The backstop
was firing constantly on correct behaviour. *Fix:* test whether the emitted audio is
actually empty, which is the honest condition.

The pattern across all three: **real input is not merely noisier than synthetic input, it
is structurally different.** It starts from silence, it arrives before you are ready for
it, and it has stretches where the model does not apply. None of those are captured by
"add noise to a sine".

---

# Milestone 2 — the voicing model

*Everything in this section was produced by `tools/model_sweep.py` on synthetic probe tones
from `tools/make_probe_tones.py`, measured by `tools/voice_probe.py`. Reproduce with:*

```bash
python3 tools/make_probe_tones.py probe/
python3 tools/model_sweep.py ./build/vh_sweep sweepout/ probe/*.wav
```

*The probes are synthetic and therefore flatter an engine built on the source-filter model,
exactly as `HANDOVER.md` §4 says about `tools/voice.hpp`. They isolate mechanisms. They do
not tell you whether it sounds good — the WAVs in `sweepout/wav/` do.*

## What changed

Three changes, in `core/src/psola_shifter.cpp` and `core/include/vh/voicing.hpp`:

1. **Overlap-add gain is now 1.0**, derived rather than approximated. This is VH-003.
2. **Grain content is resampled by mu = ratio^0.3**, giving independent formant control.
3. **A source tilt shelf** approximates the open-quotient correction.

Configurations below: `hold` is the pre-profile engine (bit-identical control), `mu` adds
the formant curve, `mu+tilt` adds the open-quotient shelf, `granular` is the resampling
engine as the far end of the exponent.

## VH-003 is fixed, and it was the gain expression

Upward level, 242 Hz probe, relative to dry:

| interval | before (S/H) | after (1.0) |
|---|---|---|
| +7 st | −6.1 dB | −0.4 dB |
| +12 st | — | **+2.6 dB** |
| +19 st | — | **+2.1 dB** |

Upward output is now slightly *louder* than dry, which is the physically correct outcome:
raising pitch fires the glottis more often at the same strength. `vh_bench` cents error
improved as a side effect (PSOLA at +12 st on a 110 Hz source: +2.0 → +0.3).

## mu does exactly what the model predicts

Duty should rise by 1/mu and pitch should not move at all. Measured, 242 Hz probe:

| interval | mu | duty hold | duty with mu | ratio | predicted 1/mu | cents shift |
|---|---|---|---|---|---|---|
| −7 st | 0.90 | 0.76 | 0.87 | 1.14 | 1.11 | 0 |
| −12 st | 0.81 | 0.62 | 0.74 | 1.19 | 1.23 | 0 |
| −24 st | 0.66 | 0.31 | 0.47 | 1.52 | 1.52 | 0 |

**The −24 st row matches the prediction to two decimal places, and the pitch does not move
by a single cent.** That is the central claim of `VOICE-MODEL.md` §5 — that PSOLA already
decoupled pitch from grain content and nobody had used it — confirmed by measurement.

Envelope distance from dry grows with mu as it must (−24 st: 4.4 dB held, 9.6 dB with mu,
13.7 dB for full resampling), and mu sits strictly between the two engines everywhere.

## The C1 prediction is NOT confirmed. It is also not refuted.

`VOICE-MODEL.md` C1 predicts that downward quality is bound by SOURCE pitch rather than by
ratio, because a grain carries only `2/F0_source` seconds of tract ring. The probe tones
were built to test exactly that: 90, 242 and 814 Hz, identical in every other respect.

**No metric in the harness separates it.** At −24 st the three sources give duty 0.44 /
0.31 / 0.35 and cents error +2410 / −4 / +0 — and the 90 Hz outlier is the ruler, not the
engine, because 90 Hz down two octaves is 22 Hz and below the estimator's floor. Envelope
contrast, added specifically to catch formant smearing, *rises* with downward shift on all
three sources, because a denser harmonic comb adds ripple to the liftered envelope and
swamps the effect being looked for.

**What was ruled out:** it is not that the effect is absent — nothing here measures ring
truncation. Duty measures excitation shape, distance measures whether the envelope moved,
contrast is confounded by harmonic density. The prediction needs either a listening test on
`sweepout/wav/` or a metric that isolates the decay envelope within one output period.
Logged as VH-010.

This is recorded rather than quietly dropped because a plausible prediction with no
supporting measurement is exactly the shape of the thing that later gets cited as fact.

## Clipping: found, then fixed

The first pass at this milestone put three sweep cells at full scale, which is destroyed
audio and an S1. Two causes stacked, and only one of them was mine to fix.

**Cause 1, the tilt shelf, fixed.** It was normalised to 0 dB at the geometric mean of its
two corners, which is the more physical picture — a bass's glottal pulse is longer *and*
larger, so a real model boosts lows as much as it cuts highs — and it boosted the upper
band by up to +4.8 dB on upward shifts. The reference is now placed at whichever end would
otherwise exceed unity, so the shelf is a pure cut in both directions. **The modelled shape
is identical; only the reference level moved.** A clipped sample is destroyed audio, a
quiet one is a fader move.

**Cause 2, the WAV writer, fixed.** `writeWav` hard-clamped to ±1, so the sweep that found
the clipping was itself reporting distorted audio as data. It now scales on overflow and
announces the scalar on stderr. `voice_probe.py` reports level relative to dry, so a
known scalar shows up as a number rather than as harmonic distortion that looks like an
engine defect.

**Result: no cell in the 108-render sweep clips, and the writer's scaler never fired
once.** Demo peaks: PSOLA 0.487, blend 0.845–0.981, mix 0.645.

**Cause 3 is not a defect and is not fixed.** PSOLA upward at large ratios genuinely runs
hot — +6.4 dB at +19 st on the 814 Hz probe, with the profile disabled, so it is purely the
VH-003 gain correction. That is the physically correct outcome: more pulses per second is
more energy per second. `core` has no output stage by design, and headroom belongs to the
mixer — `vh_render` already applies 1/sqrt(n) and an 0.9 trim, and the JUCE shell will need
the same. Recorded in VH-009 so the JUCE work inherits it rather than rediscovering it.

## CPU

48 kHz. "% budget" is p99 processing time against the wall clock available before the
callback is late. Under 50% is the honest headroom target, because the machine will also
be running a DAW and Windows.

| Configuration | mean µs | p99 µs | % budget |
|---|---|---|---|
| 1 voice, PSOLA, 128 | 85 | 258 | 9.7% |
| 8 voices, PSOLA, 128 | 93 | 337 | 12.6% |
| 16 voices, PSOLA, 128 | 101 | 334 | 12.5% |
| 8 voices, both engines, 128 | 120 | 262 | 9.8% |
| 16 voices, both engines, 128 | 137 | 317 | 11.9% |
| 8 voices, PSOLA, 64 | 47 | 128 | 9.6% |
| **16 voices, both engines, 64** | 71 | 163 | **12.2%** |

**The headline: 16 voices through both engines at a 64-sample buffer uses 12% of budget.**
Voice count barely moves the number — 1 voice and 16 cost nearly the same — because
analysis is shared. That is the architectural claim, confirmed by measurement rather than
argument.

The `max µs` column (~2 ms) is container scheduler noise, not algorithmic. It is exactly
what LatencyMon exists to characterise properly on the target machine.

---

## Latency

| Stage | Samples | ms |
|---|---|---|
| YIN acquisition (worst case, 70 Hz) | 1372 | 28.6 |
| Granular shifter (at rest) | 580 | 12.1 |
| PSOLA (worst case) | 1372 | 28.6 |

These match the predictions in `latency-budget.md` closely enough to be reassuring, and
the granular path's 12.1 ms lands inside the range that document identified as reachable.

Remember the two-clock distinction: YIN's 28.6 ms is a **convergence** cost paid at a
vocal phrase entry, not per block. Mode B does not pay it at all.

---

## Listening guide

All files 48 kHz mono. Listen in this order:

1. **`dry.wav`** — the synthetic input. A sung phrase: held vowel with vibrato → vowel
   change at constant pitch → fricative → legato note change → low sustained note. It
   sounds like a synthesiser singing, because it is one. Judge the processing by comparing
   dry against wet, not by whether the dry sounds human.

2. **`wet_granular.wav`** — fast path only. Listen for: it *works*, it's cheap, and it has
   the chipmunk character on upward voices because it resamples. This is the sound of
   latency being bought with quality.

3. **`wet_psola.wav`** — quality path only. The same chords with formants held. The
   difference from the previous file is the entire argument for the second engine.

4. **`wet_blend.wav`** vs **`wet_blend_aligned.wav`** — both engines, handover policy,
   alignment 0 and 1. The unaligned one is what Bloomberg's rig does; the aligned one is
   phase-coherent and gives up the latency advantage. This is the dial.

5. **`mix_psola.wav`** — harmony plus dry voice, i.e. what the room actually hears.

6. **`wet_modeB_interval.wav`** — Mode B. Intervals from a declared root; the singer's own
   intonation and vibrato ride through untouched.

**What you should hear as flaws, all known and documented:** the fricative is where it
sounds worst (four voices passing the same /s/ through, summing to one mono burst — the
polyphonic collapse issue, measured at 1.7× the dry energy); note onsets are softer than
they should be; and there is no release envelope shaping beyond a linear 8 ms ramp.

---

## Bugs found and fixed during this milestone

Recorded because each was found by measurement rather than by reading, and each had a
symptom that pointed somewhere other than its cause.

**1. PSOLA emitted an octave below target.** Grains centred at the emit position wrote
their first half into audio that had already been output, so half of every grain was
silently discarded and the overlap-add sum was broken. Every grain looked correct in
isolation. *Fix:* run the synthesis clock a grain half-length ahead of the output, with an
explicit "cleared up to" watermark so the accumulator is zeroed **ahead** of where grains
land, never behind.

**2. The blend was 6 dB quieter than either engine alone.** Note-on sized its lookback
from the fast engine's latency, which starves the slower engine — PSOLA's residency checks
failed and it dropped grains. The symptom reads as a mixing fault and sends you to inspect
the blend weights, which are fine. *Fix:* lookback must satisfy the slowest engine.
Regression test added. **This is the exact failure the architecture doc predicted would
appear "when a real quality engine lands".**

**3. PSOLA collapsed at 220 Hz** (errors of +493, −694, −2401 cents) while being perfect at
110 Hz. The epoch tracker's lowpass sat at a fixed 900 Hz, which isolates a 110 Hz voice
nicely but leaves a 220 Hz voice's first formant (~700 Hz) passing almost unattenuated. The
peak picker then sometimes locked onto an F1 crest, so the phase reference **jittered**
between cycles, grains stopped being coherent, and the overlap-add partially cancelled.
*Fix:* cutoff tracks the fundamental at ~1.8×, retuned with 5% hysteresis.

**4. YIN consumed the entire CPU budget.** 103% at a 128-sample block — and identically for
1 voice and 16, which was the clue: the cost was shared analysis, never the voices. *Fix:*
two-stage estimation — coarse search on a 4× decimated copy (16× less work), then full-rate
refinement in a narrow band around the winner. **13× faster, no measurable accuracy loss.**
Box-average decimation rather than plain subsampling, because aliasing would reintroduce
exactly the octave errors the design works hardest to avoid.

**Two test bugs, both worth recording** because the next person will have the same
instincts. The F0-hold test asserted bit-exact equality when the held value is legitimately
the last *confident* estimate, which drifts a few cents as the window slides into the
fricative. And the formant tests probed single frequencies — but a formant only shows
energy where a harmonic happens to land inside it, so with a 300 Hz fundamental a 700 Hz
probe measures nothing but window leakage. Band energy, compared *between* the two engines,
is the measurement that works.


---

## Real-voice validation

Two real human recordings, from the MTG essentia-audio corpus (CC material from
Freesound; see `ATTRIBUTION.txt` beside the audio):

- **sustained** — a soprano holding roughly 810 Hz with natural vibrato and drift. Near
  the top of YIN's range; period only ~59 samples, where every grain calculation is at its
  smallest.
- **melody** — a Carnatic varnam sung by Vignesh, roughly 210-320 Hz with clear melodic
  movement. Deliberately the harder case: it is not a solo dry vocal, so tambura and
  accompaniment give the pitch tracker competing periodic material.

### Spectrogram anomaly sweep, before and after fixes

Frame-to-frame level change over a 2048-point STFT. A large negative jump is a hole in the
output; a large positive one is a burst.

| Render | max jump BEFORE | max jump AFTER | drops < −12 dB |
|---|---|---|---|
| sustained, PSOLA | **220.2 dB** | 9.8 dB | 8 → **0** |
| sustained, granular | inf (non-finite) | 4.0 dB | 0 |
| sustained, blend | inf | 9.2 dB | 0 |
| melody, PSOLA | **241.8 dB** | 12.1 dB | 7 → **0** |
| melody, granular | inf | 7.9 dB | 0 |
| melody, blend | inf | 9.3 dB | 0 |

Subsonic (<60 Hz) energy is ≤0.01% everywhere — no DC or rumble artefacts. Crest factors
13-18 dB, consistent with the dry material.

---

## Bugs found on real audio

**5. The granular shifter emitted `inf`.** Real recordings begin with SILENCE; every
synthetic test began with a fully primed steady tone. With no F0 the geometry falls back to
a long grain, the read pointer's delay is briefly under the minimum, and the corrective
jump subtracts more than has been written. `pos_` went negative — and casting a negative
double to unsigned `Pos` is undefined behaviour, yielding a huge value. The interpolator's
fractional part, which the cubic assumes lies in [0,1), became astronomical and the
polynomial overflowed. Infinity in an audio buffer is the loudest possible sound.
*Fix:* `std::floor` instead of a raw cast, plus bounds-checked jumps that are skipped
rather than clamped when they would land outside written history.

**6. A note pressed before any audio existed was silent FOREVER.** `noteOn` sets
`cursor.next = noteOnPos - lookback`; because `Pos` is unsigned, pressing a key before the
ring holds `lookback` samples clamps the cursor to zero. From then on cursor and write head
advance in lockstep, one block apart — **the lag stays at one block permanently and never
heals.** PSOLA, starved of lookahead, failed its residency checks and emitted silence for
the whole life of that voice. This is what produced the 220 dB drops, and it bit exactly
where you would least want it to: the first chord of a session. *Fix:* a history gate —
skip the voice until its lag is genuine, so `writePos` pulls away. Starting a few tens of
milliseconds late is not a compromise; you cannot pitch-shift audio you have not heard yet.

**7. My first fix for #6 was wrong, and the tests caught it.** I added a backstop that fell
back to passthrough when no grains were placed in a block. But for a DOWNWARD shift the
synthesis spacing exceeds a block — an octave down at 150 Hz spaces grains 640 samples
apart against a 128-sample block — so most blocks legitimately place no grains and emit the
tails of earlier ones. **Zero grains is healthy operation, not starvation.** The backstop
was firing constantly on correct behaviour. *Fix:* test whether the emitted audio is
actually empty, which is the honest condition.

The pattern across all three: **real input is not merely noisier than synthetic input, it
is structurally different.** It starts from silence, it arrives before you are ready for
it, and it has stretches where the model does not apply. None of those are captured by
"add noise to a sine".

---

# Milestone 2 — the voicing model

*Everything in this section was produced by `tools/model_sweep.py` on synthetic probe tones
from `tools/make_probe_tones.py`, measured by `tools/voice_probe.py`. Reproduce with:*

```bash
python3 tools/make_probe_tones.py probe/
python3 tools/model_sweep.py ./build/vh_sweep sweepout/ probe/*.wav
```

*The probes are synthetic and therefore flatter an engine built on the source-filter model,
exactly as `HANDOVER.md` §4 says about `tools/voice.hpp`. They isolate mechanisms. They do
not tell you whether it sounds good — the WAVs in `sweepout/wav/` do.*

## What changed

Three changes, in `core/src/psola_shifter.cpp` and `core/include/vh/voicing.hpp`:

1. **Overlap-add gain is now 1.0**, derived rather than approximated. This is VH-003.
2. **Grain content is resampled by mu = ratio^0.3**, giving independent formant control.
3. **A source tilt shelf** approximates the open-quotient correction.

Configurations below: `hold` is the pre-profile engine (bit-identical control), `mu` adds
the formant curve, `mu+tilt` adds the open-quotient shelf, `granular` is the resampling
engine as the far end of the exponent.

## VH-003 is fixed, and it was the gain expression

Upward level, 242 Hz probe, relative to dry:

| interval | before (S/H) | after (1.0) |
|---|---|---|
| +7 st | −6.1 dB | −0.4 dB |
| +12 st | — | **+2.6 dB** |
| +19 st | — | **+2.1 dB** |

Upward output is now slightly *louder* than dry, which is the physically correct outcome:
raising pitch fires the glottis more often at the same strength. `vh_bench` cents error
improved as a side effect (PSOLA at +12 st on a 110 Hz source: +2.0 → +0.3).

## mu does exactly what the model predicts

Duty should rise by 1/mu and pitch should not move at all. Measured, 242 Hz probe:

| interval | mu | duty hold | duty with mu | ratio | predicted 1/mu | cents shift |
|---|---|---|---|---|---|---|
| −7 st | 0.90 | 0.76 | 0.87 | 1.14 | 1.11 | 0 |
| −12 st | 0.81 | 0.62 | 0.74 | 1.19 | 1.23 | 0 |
| −24 st | 0.66 | 0.31 | 0.47 | 1.52 | 1.52 | 0 |

**The −24 st row matches the prediction to two decimal places, and the pitch does not move
by a single cent.** That is the central claim of `VOICE-MODEL.md` §5 — that PSOLA already
decoupled pitch from grain content and nobody had used it — confirmed by measurement.

Envelope distance from dry grows with mu as it must (−24 st: 4.4 dB held, 9.6 dB with mu,
13.7 dB for full resampling), and mu sits strictly between the two engines everywhere.

## The C1 prediction is NOT confirmed. It is also not refuted.

`VOICE-MODEL.md` C1 predicts that downward quality is bound by SOURCE pitch rather than by
ratio, because a grain carries only `2/F0_source` seconds of tract ring. The probe tones
were built to test exactly that: 90, 242 and 814 Hz, identical in every other respect.

**No metric in the harness separates it.** At −24 st the three sources give duty 0.44 /
0.31 / 0.35 and cents error +2410 / −4 / +0 — and the 90 Hz outlier is the ruler, not the
engine, because 90 Hz down two octaves is 22 Hz and below the estimator's floor. Envelope
contrast, added specifically to catch formant smearing, *rises* with downward shift on all
three sources, because a denser harmonic comb adds ripple to the liftered envelope and
swamps the effect being looked for.

**What was ruled out:** it is not that the effect is absent — nothing here measures ring
truncation. Duty measures excitation shape, distance measures whether the envelope moved,
contrast is confounded by harmonic density. The prediction needs either a listening test on
`sweepout/wav/` or a metric that isolates the decay envelope within one output period.
Logged as VH-010.

This is recorded rather than quietly dropped because a plausible prediction with no
supporting measurement is exactly the shape of the thing that later gets cited as fact.

## What got worse: peaks

| source | interval | config | peak |
|---|---|---|---|
| 90 Hz | +19 st | mu+tilt | **1.00** |
| 814 Hz | +12 st | mu+tilt | **1.00** |
| 814 Hz | +19 st | hold | **1.00** |
| demo, 4 voices | blend | — | 0.981 |

Upward shifts now clip. Two causes stack: the VH-003 fix removed an attenuation that was
masking it, and the tilt shelf boosts the upper band on upward shifts. Logged as VH-009,
severity S1, because a clipped sample is destroyed audio rather than a mix decision.

## CPU

The cubic interpolation in `placeGrain` is per output sample and is not free.

| configuration | before | after |
|---|---|---|
| 16 voices, both engines, 64 | 12.2% | **18.0%** |
| 8 voices, psola, 128 | 12.6% | 10.9% |

Still inside the 50% headroom target, and the 64-sample figure is the one that matters.
The `unity` fast path means `voicing.enabled = false` costs nothing.

## Listening material

`sweepout/wav/` holds 108 renders: three sources x nine intervals x four configs, named
`source__interval__config.wav` so any two are directly comparable. `sweepout/png/` holds
spectrogram-and-waveform grids, all four configs side by side per cell.

**Listen in this order:**

1. `probe_242hz__-24st__hold.wav` then `__mu.wav`. This is the whole change in one A/B.
2. `__mu.wav` then `__mu+tilt.wav` at the same cell — is the tilt an improvement or just
   darker? That is `VOICE-MODEL.md` §7 Q4 and it is a listening question.
3. `probe_90hz__-12st__*` against `probe_814hz__-12st__*`. Same interval, different source
   pitch. If C1 is real you will hear it here even though no metric caught it.
4. Sweep the exponent by hand where it matters:
   `./build/vh_sweep in.wav out.wav psola -12 0.5 1` — muStrength 0.5 is halfway between
   holding formants and the derived curve.
