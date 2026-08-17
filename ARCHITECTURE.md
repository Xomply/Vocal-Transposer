# Architecture & Handover

*Read this before changing anything. It exists so that someone with none of the
originating context — a future you, a collaborator, or an AI assistant arriving empty on
every session — can reconstruct not just what was decided but why, how sure we were, and
what we'd have built with more room.*

---

## The seed

Sing live into a microphone while playing chords on a MIDI keyboard, and hear your own
voice come back as those chords. Live, with a band, monitored in real time. The
harmonized output **is** the instrument — not an effect applied afterwards.

Two modes, deliberately different instruments rather than two settings:

- **Mode A — detachment.** Sing anything; the played notes come out. `ratio = f_target /
  f_sung`. Needs F0. Chosen explicitly over *correction* (nudging a note you're already
  aiming at), which would have meant sub-50-cent shifts and a much smaller error budget.
- **Mode B — interval.** Transpose by the played intervals from a declared root.
  `ratio = 2^(n/12)`, known the instant the key goes down. Needs no pitch detection.

Full design rationale lives in `midi-vocal-harmonizer-plan.md` and `latency-budget.md`.

---

## The reasoning that produced this shape

**1. Latency is the problem, and it cannot be sidestepped.** The usual escape for vocal
harmonizers — monitor the dry voice, send the wet elsewhere — does not apply, because
here the wet signal is what you're playing. So the architecture attacks latency directly:
two engines in parallel, a fast imperfect one for playability and a slow good one for
sound, blended. Taken from Ben Bloomberg's rig for Jacob Collier, which ran four Antares
Harmony Engine instances alongside a TC-Helicon unit specifically to give the impression
of lower latency.

**2. Analysis is shared; synthesis is per-voice.** One analyser feeds all voices. This is
why **voice count costs CPU but zero latency**, and it is the most important structural
fact in the system. Do not economise on voices hoping to reduce latency.

**3. Estimation latency is a convergence cost, not a per-block tax.** YIN needs 18–27 ms
to lock at a low male fundamental. It does **not** need that again every block. Cashing
this in is what `AnalysisFrame::f0IsHeld` exists for — see below.

**4. Absolute sample positions, everywhere.** Voices read the input at independent lags;
frozen voices don't advance at all; two engines read the same span for blending. All of
that is trivial with absolute addressing and impossible to keep straight without it.

---

## Module map

```
core/include/vh/
  types.hpp          Sample, Pos, constants. The vocabulary.
  rt.hpp             Real-time safety guard. Makes "no allocation" testable.
  audio_ring.hpp     Shared input history. One writer, many readers, no reader state.
  analysis.hpp       AnalysisFrame + IAnalyzer. The narrow waist.
  epoch.hpp          Glottal phase reference. Consistency, not anatomical truth.
  yin.hpp            F0 estimation, two-stage. The F0-hold policy lives here.
  preservation.hpp   "Same voice" as seven independent decisions.
  voicing.hpp        Those decisions as CURVES of the shift ratio, not constants.
  shifter.hpp        IPitchShifter + ReadCursor. Freeze lives in the cursor.
  granular_shifter.hpp  FAST path. Resamples; pitch-synchronous jumps kill the comb.
  psola_shifter.hpp     QUALITY path. Formants preserved by construction.
                        `mix_` is the grain/passthrough handover: a crossfade, not a branch.
  passthrough_shifter.hpp   Identity shifter: test oracle and safe fallback.
  voice.hpp          Voice state, RatioSource (the mode switch), Humanization.
  blend.hpp          IBlendPolicy. THE dial.
  engine.hpp         Orchestrator. Owns nothing conceptual.

core/src/            rt.cpp, yin.cpp, granular_shifter.cpp, psola_shifter.cpp, engine.cpp
offline/             WAV in -> WAV out. No audio hardware.
tools/               voice.hpp (synthetic singer), demo.cpp (audio), bench.cpp (perf+quality)
tests/               doctest + the allocation detector. 48 cases, six of them
                     pinning that no path switch is a branch (test_continuity.cpp).
app/                 JUCE shell. NOT YET WRITTEN — see "what's open".
```

**Milestone 3 is reached.** The principles work AND the output no longer steps. Isolated
clicks across the regression grid 99 → 19; sibilant ensemble collapse 0.82 → 0.13 on real
singing; the formant warp confirmed applied to within 2.4%. Four real recordings, 48 tests,
+5% CPU. See `RESULTS.md`.

**Milestone 1 was reached earlier: the principles work.** Measured evidence in `RESULTS.md` —
worst-case pitch error 4.2 cents, 16 voices through both engines at 12% of a 64-sample
callback budget, and harmony notes absent from the input appearing at 112-157x their dry
level. Four significant bugs were found and fixed by measurement; all four are written up
in `RESULTS.md` because each had a symptom that pointed away from its cause.

**`core` links no framework.** Not JUCE, not a plugin SDK, not an audio driver. This is
the single most consequential build decision: it means every part of the DSP can be
tested, benchmarked and bisected headlessly. An audio codebase whose DSP can only be run
inside a plugin host is one whose DSP is effectively untested, because nobody spins up a
host to check a windowing change.

---

## Decisions, and how sure we are

### CERTAIN — load-bearing walls. Things rest on these.

| Decision | What rests on it |
|---|---|
| Absolute `Pos` addressing, never block-relative | Freeze, dual-engine alignment, all multi-lag reads |
| Shared analysis, per-voice synthesis | The entire latency argument; voice count being free |
| `core` has no framework dependency | The whole test suite; CI; offline iteration |
| No allocation on the audio thread, **enforced by test** | Dropout-free operation under load |
| Freeze = a cursor that stops advancing | Freeze needing no support in ring or shifters |
| Blend is a continuous C¹ crossfade, never a switch | Inaudible handover; the user's stated requirement |
| **No path switch anywhere is a branch** | Every click found in milestone 3. A mode flag in an audio path is a click waiting for a consonant |
| Grain/passthrough handover is a crossfade (`mix_`) | Clean consonants; the single largest click source |
| Voicing is gated, asymmetrically | No mode chatter; a lyric no longer toggles the handover every few ms |
| F0 momentum: leaps need corroboration | Every quantity derived from the period staying stable through one bad estimate |
| Granular fade length snapshotted at fade start | The crossfade envelope staying monotonic while pitch moves |
| Per-voice static delay on the whole voice | The ensemble not collapsing to mono at every fricative |
| Equal-gain (not equal-power) blend normalisation | No +3 dB pumping between correlated engines |
| Hold F0 through unvoiced frames | Mode A staying responsive through sung lyrics |
| One shifter instance per voice, not shared | Voices not trampling each other's grain state |
| Modes A and B are different instruments | Not letting detachment drift into correction |

### PROVISIONAL — meant to move. Change these freely.

| Decision | If it's wrong |
|---|---|
| `kMaxVoices = 16` | Array sizes only. Raise freely. |
| 2 s input history | Memory only (384 kB). Raise if freeze sounds looped. |
| YIN `minHz = 70`, `threshold = 0.15` | Every Hz of lower bound costs window length = latency |
| Analysis hop 128 samples | Deliberately decoupled from block size. Tune independently. |
| `maxHoldMs = 200` | Too long leaves stale pitch after a phrase; too short reintroduces the stutter |
| `envelopeWarp = 0.8` | Character of large downward shifts. Nothing structural. |
| `RegisterSplitPolicy` crossover at 220 Hz | Pure guess. The *ordering* (fast=high) has weak evidence; the number has none. |
| `OnsetHandoverPolicy` 25→70 ms | The parameters most worth tuning by ear first. |
| Oldest-first voice stealing | Revisit if sustain-pedal chord stacking steals notes you wanted |
| Declared root for Mode B | If the root must come from the voice, Mode B loses its latency advantage entirely |

---

## The seams — where to cut

**Dials deliberately built** (high confidence these dimensions will move):

- `IBlendPolicy` — the split policy. Swappable at runtime, mid-performance. A new policy
  is ~20 lines and touches nothing else. This is the dial the whole architecture was
  shaped around, because it was an explicit requirement that the split's behaviour be
  tunable *after* the system exists.
- `IPitchShifter` — we know of four implementations we intend to write and at least two
  that must run simultaneously. Not speculative.
- `IAnalyzer` — YIN now; pYIN, CREPE, and a MIDI-informed analyser are all on the map.
- `PreservationSpec` — seven rows we expect to tune by ear, per voice.

**Deliberately NOT abstracted** (so nobody adds these thinking they were forgotten):

- **FFT backend.** Pick one, wrap thinly, live with it. A pluggable FFT is machinery
  around a dimension that won't move.
- **Sample rate / channel count.** Mono analysis in, mixed out. Generalising this buys
  nothing and costs clarity everywhere.
- **No effect graph, no node system, no plugin registry.** These are the standard ways a
  project like this drowns. One analyser, two engines, N voices, one mixer.
- **Voice count** is a constant, not an architecture.

---

## The better version we couldn't reach

- **No source-filter engine.** Timbre control landed without it (see below), so the
  remaining case is narrower and sharper: it is the only way to generate tract ring beyond
  one source period, and the only way to move formants WITHOUT stretching the glottal pulse
  inside the grain, because resampling scales the whole grain and cannot separate source
  from filter. This is the single biggest remaining gap, and
  `latency-budget.md` argues WORLD may also be FASTER than a phase vocoder at steady state.
- **Epoch detection is a peak tracker, not a GCI detector.** It gives a CONSISTENT phase
  reference, which is what PSOLA actually needs, but it is not DYPSA or SEDREAMS and it has
  not been validated against electroglottography. It was already the cause of one
  hard-to-diagnose bug (see `RESULTS.md` #3).
- **Humanization is mostly not applied.** The Engine now reads exactly one row —
  `staticDelayMs`, which fixes the sibilant collapse — and ignores per-voice detune,
  vibrato, timing jitter and pan. N voices still differ only in pitch and delay. The wiring
  to read the struct exists now, so adding the rest is smaller than it was.
- **`envelopeWarp` is unused** — resolved in milestone 2, and the fix was smaller than the
  wait implied. `VoicingProfile` replaces the constant with `mu = ratio^0.3` (derived from
  population data, and reproducing the literature's 0.8 at an octave), and `PsolaShifter`
  applies it by resampling grain CONTENT. PSOLA sets pitch by WHEN grains are emitted, so
  the content was always free; nobody had used it. No FFT, no source-filter engine, no new
  dependency. See `VOICE-MODEL.md` §5.
- **Onsets are the weak spot**, as predicted. A voice entering mid-vowel starts from an
  8 ms linear ramp; there is no attack shaping and no ducking on high analysis residual.
- **YIN's refinement stage recomputes the difference function** in a narrow band rather
  than reusing the coarse result. Correct but slightly wasteful; irrelevant at 12% budget.

## What's still open

### Needs testing (run the experiment)

1. **YOUR OWN VOICE, on your own hardware.** The engine has now been validated on two real
   recordings (a soprano sustain and a Carnatic melody, see `RESULTS.md`), which
   immediately exposed three bugs that 36 synthetic tests had missed. But both are studio
   material at comfortable levels. Your microphone, your room, your consonants and your
   register are still untested. Use `vh_render`:
   `vh_render take.wav out/ mytake "0:60,64,67; 2:59,62,67" 60`
2. **Round-trip latency on the target machine** at 64/128/256 samples. Every I/O latency
   figure in the docs is still borrowed.
3. **LatencyMon DPC spikes on Windows.** Determines whether 64 samples is real. Note the
   benchmark's ~2 ms `max` outliers are container scheduler noise of exactly this kind.
4. **Whether the fast path alone is good enough.** If yes, the quality path becomes
   optional and the architecture simplifies. Listen to `wet_granular.wav` against
   `wet_psola.wav` and decide.
5. **Mic choice.** Practitioner consensus is that a dark capsule (SM58) beats a
   large-diaphragm condenser through any harmonizer, because sibilance is where these
   things fall apart. May solve more than code will. Cheap to test.

### Needs design (think before coding)

6. **Sibilant polyphonic collapse — FIXED in milestone 3.** A per-voice static delay,
   golden-ratio spread, applied to the whole voice after the blend. Collapse 0.82 → 0.13 on
   real singing. `BUGS.md` VH-005 has the measurement and the reason the allpass this was
   originally going to be was rejected.
7. **Frozen-voice looping.** A fixed-period loop buzzes audibly within ~1 s. Needs
   randomised grain selection plus slow per-voice detune drift. `ReadCursor::rngState`
   is seeded per voice for this and otherwise unused.
8. **Engine re-entry after being skipped.** The Engine skips engines below −80 dB blend
   weight as a free CPU governor. A stateful shifter coming back after being skipped has
   stale internal state and must cross-fade in or reset. Passthrough doesn't care; a
   phase vocoder will.

### Needs a decision

9. **JUCE shell.** `app/` is empty. Standalone and VST3 from one codebase is the plan;
   nothing has been written, and `core` is deliberately unaware of it.

---

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/vh_tests                              # 48 cases
./build/vh_bench                              # CPU, pitch accuracy, latency
./build/vh_demo ./audio                       # render dry + wet listening material
./build/vh_render take.wav out/ mytake "0:60,64,67; 2:59,62,67" 60   # your own recording
```

Verified: g++ 13.3, CMake 4.4, Linux — **and MSVC / Windows 11**, which is the actual target.
`M_PI` was the blocker and it was total, not minor: being POSIX rather than ISO C++, it meant
nothing compiled under MSVC at all. It is now `vh::kPi` in `core/include/vh/types.hpp`; see
`HANDOVER.md` §5 for why a `_USE_MATH_DEFINES` compile definition was rejected. `#pragma pack`
turned out fine as written.

### What the tests actually protect

The suite is not there for coverage. Each test pins a specific claim that would otherwise
silently rot:

- *"freeze needs no support from the ring"* — pins the architectural claim, so that if
  someone adds freeze handling into `AudioRing`, the comment defending it has a test
  attached.
- *"Mode B computes its ratio without any pitch detection"* — feeds **silence**, so the
  analyser can never lock, and asserts the ratio is still exact. If Mode B ever acquires
  an F0 dependency, this fails.
- *"holds F0 through an unvoiced gap"* — if this fails, the responsiveness claim in
  `latency-budget.md` is void.
- *"does not make octave errors"* — pins the *first dip below threshold, not global
  minimum* choice in `yin.cpp`. Someone "optimising" that into a global-minimum search
  breaks the harmony by an octave; this catches it.
- *"the guard detects an allocation"* — a meta-test. A safety net that catches nothing is
  worse than none, because it produces confidence without evidence.
- *"a full block with many voices allocates nothing"* — the end-to-end real-time claim.
- *"note-on lookback satisfies the SLOWEST engine"* — regression test for the 6 dB blend
  bug. Two spy shifters with deliberately mismatched latencies; asserts the slow one is
  handed enough history.
- *"the two engines differ exactly as their algorithms predict"* — measures spectral tilt
  on both engines from identical input. Resampling must push the envelope up; PSOLA must
  not. This is the test that would catch either engine quietly becoming the other.
- *"signals that START WITH SILENCE stay finite"* — every synthetic test began with a
  primed steady tone, which is why the `inf` bug survived 36 of them. Real recordings begin
  with silence. **Run any new shifter through this case before trusting it.**
- *"a note pressed on an EMPTY ring still produces sound"* — the history gate. Pins a bug
  that never healed itself and silenced the first chord of a session.

**One test was wrong on first run and the fix is recorded in the source**: the F0-hold
test asserted bit-exact equality, but the held value is the last *confident* estimate, and
the window legitimately produces slightly different estimates as it slides into the
fricative. A few cents of drift is correct behaviour. Left documented in
`test_yin.cpp` because the next person will have the same instinct.
