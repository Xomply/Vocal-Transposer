# Results

*Milestone 3 is at the top because it is the most recent. Milestone 1 and the bug log are
below and are unchanged; nothing in milestone 3 supersedes them.*

---

# Milestone 3: the output stops stepping, and the ensemble stops collapsing

Two defects that had been in `BUGS.md` since they were first heard — VH-002 (clicking) and
VH-005 (sibilant collapse) — are fixed, measured on four recordings, and pinned by six new
regression tests. A third deliverable, a harness for tuning `k_mu`, produced the first
direct confirmation that the engine applies the formant warp it computes.

## What was actually wrong with the clicking

**Neither of VH-002's two recorded candidates.** The entry suspected the epoch tracker and
stale synthesis spacing. It was four separate places where a decision was made with a branch
instead of a fade — and the reason it read as a pitch-shifting fault is that a sung lyric
crosses the voiced/unvoiced boundary twice per consonant, so the symptom presents as "k, t,
b, p all click".

The entry named the experiment that would have settled it — "log the epoch series and the
per-block period against click timestamps" — and it had never been run, because the log did
not exist. `vh_trace` is that log. The first run:

| melody, PSOLA | isolated clicks | at voicing edge | steady |
|---|---|---|---|
| +3 st | 14 | **14** | 0 |
| +7 st | 15 | **15** | 0 |
| +12 st | 12 | **12** | 0 |

Not "worse at edges". **Only** at edges.

The four causes and their fixes are written up in `BUGS.md` VH-002. In one line each:
PSOLA chose between grains and passthrough with an `if`; the voicing decision had no
hysteresis; a single wrong F0 estimate moved every derived quantity for one hop and back;
and the granular crossfade's length changed underneath a running fade.

## The grid

Two recordings x two engines x five intervals. The "before" column is a **real build of the
previous commit**, not a runtime flag — a flag can only disable what somebody remembered to
make switchable.

| | before | after |
|---|---|---|
| melody, PSOLA, -12 / -5 / +3 / +7 / +12 st | 16 / 13 / 14 / 15 / 12 | 0 / 0 / 1 / 0 / 1 |
| melody, granular | 2 / 0 / 2 / 1 / 10 | 1 / 1 / 0 / 0 / 7 |
| sustained, PSOLA | 2 / 2 / 2 / 1 / 2 | 1 each |
| sustained, granular | 1 each | 0-1 each |
| **total** | **99** | **19** |

The sustained recording's floor of 1 is the **dry file's own count** — real material contains
real transients, so zero is not the target and would indicate the detector was broken.

The one cell still above the floor, `melody/granular/+12` at 7, is logged as VH-011: at
ratio 2 the granular path needs a jump roughly every period, and each jump is a crossfade
seam. That is the trade that buys its latency, not a defect.

## Two recordings that had never been used

Both chosen because neither resembles the two validation files, and between them they cover
the two things milestone 3 targeted. See `ATTRIBUTION.txt`.

**Flamenco cante** — male, ~157-390 Hz, chest register, heavy melisma. The pitch almost
never holds still, which is the hardest available condition for VH-002.

**Spoken digits** — male, ~110-190 Hz, assembled rather than found, because nothing in the
corpus has this density of plosives and fricatives. Spoken digits happen to cover the exact
phonemes the reported symptom named: /k/ and /s/ in "six", /t/ in "two" and "eight", /f/ in
"four" and "five", /v/ in "seven". A voiced/unvoiced boundary every few hundred
milliseconds.

| render | before | after | dry source's own count |
|---|---|---|---|
| flamenco, PSOLA | 77 | **1** | 1 |
| flamenco, blend | 77 | **1** | 1 |
| flamenco, granular | 25 | 15 | 1 |
| speech, PSOLA | 160 | **7** | 11 |
| speech, blend | 159 | **7** | 11 |
| speech, granular | 10 | **3** | 11 |

The speech figures land *below* the dry source's own count, which is not a mistake: the
harmonised output is built from windowed grains, and grains are smoother than a plosive
release.

Listening material: `demo/flamenco_reel.wav` (104 s) and `demo/speech_reel.wav` (214 s).
Each is dry first, then every configuration AFTER immediately followed by the same
configuration BEFORE on the same passage. After-then-before rather than the reverse, because
the second of a pair is heard most critically and putting the older build there is the
harder test. Guides in `demo/*_DEMO.md`.

## The sibilant collapse

**The old number could not have shown a fix.** "The fricative region reads 1.7x the dry
energy with four voices" is a level, and a level improves if you turn the voices down or
delete the harmony — the same trap as duty cycle in VH-008 and "no gaps" in VH-006.

The property is COHERENCE. `tools/ensemble_probe.py` measures four voices against **one
voice** on the same material: four coherent copies give +12.0 dB, four incoherent copies give
+6.0 dB. `collapse` places the result between them, 0 for an ensemble and 1 for four copies
of one voice. Being a ratio between two renders, a level change moves both and cancels.

| material | before | after |
|---|---|---|
| synthetic demo phrase (the 1.7x figure's own material) | 0.79 | **0.41** |
| flamenco cante | 0.82 | **0.13** |
| consonant-dense speech | 0.66 | **0.03** |

The fix is a fixed per-voice delay of 0.5-4 ms, golden-ratio spread so no two are a simple
multiple, applied to the whole voice after the blend. Rationale, and why the allpass this
entry originally proposed was rejected, in `BUGS.md` VH-005.

**Honest caveat.** The voiced control window moves too, by less (flamenco +10.21 -> +7.62 dB
against the fricative's +10.98 -> +6.82). The delay is applied to the whole voice by design,
so it decorrelates everything a little and fricatives a lot. A reader comparing only the
headline number would conclude the change is fricative-specific, and it is not.

## The mu harness, and what it found

`k_mu` could not be tuned, because nothing measured whether the engine applied the mu it
computed. `envdist_db` says the envelope MOVED; it cannot tell 0.81 from 0.70.

A formant scale factor is a multiplication on the frequency axis and therefore a translation
on a LOG frequency axis, so `tools/mu_probe.py` cross-correlates the cepstrally liftered
envelopes of dry and wet and reports the best translation. Two calibration controls make it
trustworthy, and both have answers that come from the algorithms rather than from the tool:

| control | must be | measured |
|---|---|---|
| granular, -12 / -5 / +7 st | the ratio | 0.498 / 0.747 / 1.421 |
| PSOLA, muStrength = 0, same | 1.000 | 0.996 / 0.997 / 0.994 |

**The finding: the engine applies its requested mu to within 2.4% mean error across a
5 x 5 grid of interval and exponent, while pitch stays within ~25 cents everywhere.** That
independence — mu moves the envelope and does NOT move the pitch — is the entire claim of the
grain-resampling approach in `VOICE-MODEL.md` §5, and until now it was an argument rather
than a measurement.

Upward shifts under-deliver mu by 3-7% and large downward exponents slightly over-deliver.
Not chased; recorded so the next person knows the residual is systematic rather than noise.

**A limit that is real rather than an implementation detail: the probe cannot measure a
soprano.** A cepstral envelope is only informative where harmonics sample it, and at 814 Hz
there are barely seven below 6 kHz, so the controls come back 170-200 cents out. At 242 Hz
they land within 32. This is the same conditioning problem `VOICE-MODEL.md` §5 records for
LPC at high F0, and it is not a coincidence. **Tune `k_mu` on low-pitched material; the
soprano is for listening.**

## Cost

| | before | after |
|---|---|---|
| 16 voices, PSOLA, 128, mean us | 222 | 234 |
| 16 voices, both engines, 64, mean us | 128 | 132 |

**+5%.** Measured against the previous build in the same container on the same run, because
the absolute figures in milestone 1 were taken under different load and are not comparable.

Two-thirds of the overhead was self-inflicted and removed: a transcendental per sample per
voice, once in the handover crossfade and once in the Engine envelope. Both are now computed
only while the thing they shape is actually moving. Before that, 16 voices at a 128-sample
block went from 12% to 37% of budget — 2048 cosines per block that almost always returned
the same number.

## Mistakes made and caught, in one place

Recorded because each is a trap the next person can walk into, and all four were caught by
measurement rather than by reading.

**A fix that was worse than the bug.** For the granular fade length, deferring
`updateGeometry` until no fade was in flight. At ratio 2 the fade occupies half the time
between jumps, so block boundaries keep landing inside one, the update starves for many
blocks and then arrives all at once — a bigger step than the one being avoided. The geometry
must keep updating; it is the FADE that must be insulated from it. Recorded in the header so
it is not retried.

**A time constant off by the block size.** The geometry smoother's one-pole coefficient was
derived from the sample rate, but `updateGeometry` runs once per BLOCK — so 20 ms became
2.5 seconds, the geometry never caught up with the singer, and the granular pitch test failed
by 1971 cents. Caught in one run by a test suite that needs no audio hardware, which is the
whole argument for `core` linking no framework.

**The same performance mistake twice.** A `cos` per sample per voice, first in the handover
and then, independently, in the Engine envelope. See Cost above.

**A test signal that contained the defect it was testing for.** The first continuity tests
concatenated a mid-cycle tone straight onto full-scale white noise, which is a step in the
SOURCE. The shifter passed it through faithfully and the test blamed the shifter — every
"failure" was one cluster of samples at the join, delayed by exactly the shifter's latency.
`tools/voice.hpp` already recorded this lesson for the synthetic singer. The joins are now
crossfaded.

**A detector that flagged correct behaviour.** The original click counts were partly false
positives: at -12 st, 89 of 184 "clicks" were the shifter's own glottal pulses, because a
rolling median taken across the inter-pulse silence the VH-001 fix correctly restored is
tiny. Verified by hand — max|dx| over local peak of 0.02-0.21, smooth band-limited rises. The
threshold was NOT lowered, because that would have broken comparability with the counts
already in the ledger; a step-ratio gate was added alongside it, derived rather than tuned.
An absolute step-ness test on its own was tried first and rejected: dry melody scores 479
samples above it, because tambura and consonants genuinely contain fast slopes.

## New tools

| tool | what it is for |
|---|---|
| `vh_trace` | `vh_sweep` plus a per-block CSV of the analyser's state. No changes to `core`; the audio path is bit-identical. |
| `tools/click_probe.py` | Finds discontinuities and attributes each to what the engine was doing. |
| `tools/click_sweep.py` | The click regression as one command, with `--compare` for deltas. |
| `tools/ensemble_probe.py` | Coherence of N voices against one. Auto-classifies voiced/unvoiced frames. |
| `tools/mu_probe.py` | Achieved formant scale factor, with `--selftest` calibration. |
| `tools/mu_sweep.py` | Requested vs achieved mu across a k grid, plus listening renders. |
| `tools/demo_reel.py` | Builds an A/B listening reel and its guide from two builds. |

## New tests

`tests/test_continuity.cpp`, six cases, all pinning claims that would otherwise rot silently:
PSOLA and granular hand over without stepping; a pitch step does not put a step in the
output; the voicing gate rides through a dropout; voicing attack stays immediate; F0 momentum
rejects an excursion but accepts a real leap; N voices do not sum coherently on unvoiced
material — that last one also asserting the control still reproduces the defect, so it cannot
pass by the fix having been quietly disabled.

One existing test was changed. `"reported latency reflects the mode"` asserted
`b->latencySamples() == 0`, which was a PROXY for "Mode B pays no analyser cost" that held
only because Mode B with no shifters had no other latency. The per-voice decorrelation delay
added a term to both modes and the proxy failed while the claim stayed true. It now asserts
the difference between the modes equals the analyser's latency, so anything common to both
cancels. `HANDOVER.md` §6's "measure the thing, not a proxy for it", recurring for the third
time in this repository.

---

# Milestone 1: the principles work

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

---

## Milestone 2, continued — listening test on real recordings, VH-008 closed

*Everything above this point (mu confirmation, C1 non-result, clipping) was measured on
the synthetic probe tones. This section is the listening test on the two real recordings
already in the repo, produced with:*

```bash
./tools/fetch_test_audio.sh .
python3 tools/model_sweep.py ./build/vh_sweep sweepout/ sustained_dry.wav melody_dry.wav \
    --intervals -24 -12 -5 5 12 19
```

**Result: confirmed, unambiguously, by ear.** `mu+tilt` (the full voicing profile) is a
large improvement over `hold` (the pre-profile engine) at every interval tested on both
recordings — the soprano (~805 Hz, sustained, natural vibrato) and the Carnatic melody
(~276 Hz, moving pitch, tambura in the background). Most audible exactly where VH-008 was
opened: −24 and −12 st.

This closes VH-008. It does **not** close VH-010 — nothing here separates how much of the
improvement is the open-quotient correction versus how much is that mu also lengthens the
grain and so partly mitigates the still-truncated ring. It also is not a comparison against
option 2 (blend-tilt toward the granular engine) or option 1 (a source-filter engine), only
against the engine as it stood before this milestone. The claim is "this is a real
improvement", not "this is the best available fix".

## A probe-tool limitation, found by this listening test

The soprano source shifted up +12 and +19 st puts the output fundamental above 1.6 kHz,
past `voice_probe.py`'s 1400 Hz ceiling (`estimate_f0`'s default `fmax`). The tool locks
onto a subharmonic and reports garbage — cents error around −1200 to −1900, which reads as
a catastrophic octave failure and is not one; the rendered audio is fine. Anyone re-running
this sweep on high sopranos should raise `fmax` or discard those two cells' pitch column
and trust their ears. Not filed as a BUGS.md entry because it is a measurement-tool ceiling
with an obvious cause, not an open question — but recorded here so the next person doesn't
re-diagnose it as an engine regression.

## Listening material

`sustained_dry_listening_demo.wav` and `melody_dry_listening_demo.wav` (not committed —
generated audio, same reasoning as `ATTRIBUTION.txt` for the source recordings): dry
reference, then six intervals from −24 to +19 st, `hold` immediately followed by `mu+tilt`
at each, so the A/B sits adjacent rather than scattered across the file. Regenerate with
the commands above; `tools/model_sweep.py`'s output naming makes the pairing mechanical.
