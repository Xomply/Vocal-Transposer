# HANDOVER

*Read this first. `VOICE-MODEL.md` says what a voice is and therefore what the DSP must get
right; `ARCHITECTURE.md` has the module-by-module reasoning; `RESULTS.md` has the
measurements; `BUGS.md` is the ledger of open defects. This document tells you what the thing is, what it
deliberately is not, and how to work on it.*

---

## 1. What this is

A **MIDI-driven vocal harmonizer**. You sing into a microphone while playing chords on a
keyboard, and your own voice comes back as those chords. Live, in a room, monitored in
real time.

The critical framing: **the harmonized output IS the instrument.** It is not an effect
applied to a performance afterwards. You are playing it, so you must hear it, which is why
latency is attacked head-on rather than sidestepped.

Two modes, which are genuinely different instruments rather than two settings:

| | Mode A — *Detachment* | Mode B — *Interval* |
|---|---|---|
| Behaviour | Sing anything; the played notes come out | Your pitch, transposed by the played intervals |
| Ratio | `f_target / f_sung` | `2^(n/12)` from a declared root |
| Needs F0? | **Yes, critically** | No |
| Your intonation | Discarded — that is the point | Preserved, scoops and all |

**Current state: Milestone 3.** The principles work and the output no longer steps. Worst-
case pitch error 5.5 cents (PSOLA 2.1). Isolated clicks across the regression grid **99 → 19**,
with the residual being the dry files' own transients plus one logged case (VH-011). Sibilant
ensemble collapse **0.82 → 0.13** on real singing. Validated on synthetic material and on
**four** real human recordings. 48 tests, clean build, zero warnings under
`-Wall -Wextra -Wpedantic -Wshadow -Wconversion`. Full measurements in `RESULTS.md`.

---

## 2. Why it is shaped this way

Four decisions drive everything else.

**(a) Latency cannot be sidestepped here.** The standard escape for vocal harmonizers —
monitor the dry voice, send the wet elsewhere — does not apply when the wet signal is what
you are playing. So: **two engines run in parallel and are blended.** A cheap fast one
(~12 ms) gives playability; a slower good one (~29 ms) gives the sound.

**(b) Analysis is shared; synthesis is per-voice.** One analyser feeds all voices. This is
why **voice count costs CPU but zero latency** — confirmed by benchmark, where 1 voice and
16 cost nearly the same. Do not economise on voices hoping to reduce latency.

**(c) Estimation latency is a convergence cost, not a per-block tax.** YIN needs 18–27 ms
to lock at a low fundamental. It does not need that again every block. `AnalysisFrame::
f0IsHeld` cashes this in: holding F0 through unvoiced frames is what stops every consonant
in a lyric from re-paying acquisition.

**(d) Absolute sample positions everywhere.** Voices read at independent lags, frozen
voices do not advance, two engines read the same span for blending. Trivial with absolute
addressing; impossible to keep straight without it.

---

## 3. What it is based on

**The architecture** comes from Ben Bloomberg's rig for Jacob Collier (MIT dissertation,
*Making Musical Magic Live*, 2020, pp. 166–168, 181–183). That rig ran four Antares Harmony
Engine instances blended in parallel with a TC-Helicon unit, specifically to give the
impression of lower latency. Bloomberg's own retrospective — that he would write the pitch
shifter from scratch for low latency, and that doing so would unlock microtuning and
altered intonations — is effectively a description of this project.

**The algorithms** are classical DSP, all well documented:

- **YIN** — de Cheveigné & Kawahara, JASA 2002. Difference function, cumulative mean
  normalisation, absolute threshold, parabolic interpolation. Two-stage here (decimated
  coarse search, full-rate refinement) for CPU reasons.
- **TD-PSOLA** — Moulines & Charpentier; Moulines & Laroche, *Speech Communication* 1995.
  Formants preserved because grains are unmodified slices re-emitted at a new rate.
- **Delay-line/granular shifting** — the Eventide H910 lineage, with the pitch-synchronous
  jump constraint that makes the comb filter *stop existing* rather than merely smearing.
- **Epoch tracking** — a peak tracker on a pitch-tracking lowpass. Explicitly NOT DYPSA or
  SEDREAMS (see §4).

**The project documents** `midi-vocal-harmonizer-plan.md` and `latency-budget.md` carry the
design reasoning and the serial/parallel latency decomposition. The older
`realtime-vocal-transposer-design-notes.md` researched a *different* instrument (a
backing-vocal harmonizer with a latency-critical self-monitoring path) — it remains a good
technique reference, but where it conflicts with the plan, the plan wins.

---

## 4. What this is NOT — and what that costs you

This section matters more than §3. Everything below is a deliberate absence, not an
oversight. Knowing which is which is the difference between extending the design and
fighting it.

### Not present, and you will probably want it

**No source-filter engine (WORLD, LPC, STRAIGHT).** Still the biggest gap, but it is a
SMALLER gap than this document used to claim, and the reasons matter.

- **"PSOLA ends around ±6 semitones" is not what this code does.** See `BUGS.md` VH-007.
  Three separate mechanisms bound the range, in different directions: tail cancellation
  upward, open-quotient collapse downward, and tract-ring truncation downward. One number
  concealed all three.
- **Timbre control now EXISTS.** `PreservationSpec::voicing` carries mu as a curve of the
  shift ratio, and `PsolaShifter` applies it by resampling grain content — independent
  formant control, in the time domain, with no FFT. `envelopeWarp` is superseded and kept
  only as the A/B control. See `VOICE-MODEL.md` §5.
- **What a source-filter engine is still needed for** is narrower and sharper than "quality":
  it is the only way to generate tract ring beyond one source period, and the only way to
  move formants WITHOUT also stretching the glottal pulse inside the grain. Resampling
  cannot separate source from filter. That coupling is the argument.
- Note `latency-budget.md` argues WORLD may be *faster* than a phase vocoder at steady
  state, because its F0 cost is a convergence cost while the vocoder's window is paid every
  frame. If that holds, WORLD wins on both axes.

**No phase vocoder.** No FFT anywhere in the codebase, in fact. Everything is time-domain.
That is why there is no dependency on KissFFT/pffft yet, and why the build has no numerical
library at all.

**No MIDI input.** `Engine::noteOn/noteOff/setSustain` exist and are tested, but **nothing
reads a MIDI device.** Notes currently arrive from the command line in `vh_render`. Wiring
a real keyboard is part of the JUCE shell work.

**No JUCE shell, no plugin, no UI, no audio device I/O.** `app/` is an empty directory with
a README. `core/` links no framework by design — that is what makes the test suite possible
— but it also means **nothing in this repository has ever made a sound through a
soundcard.** All validation is file-in/file-out.

**Humanization is MOSTLY not applied.** The `Humanization` struct has per-voice detune,
drift, vibrato rate/depth/phase, timing jitter and pan. The Engine now reads exactly one row
of it — `staticDelayMs`, the per-voice delay that fixes the sibilant collapse (VH-005). The
rest is still unread, so N voices remain N shifted copies that differ only in delay and
pitch. **Per-voice detune and independent vibrato are still the cheapest large improvement
available**, and the wiring for reading the struct now exists, so it is a smaller job than
it was.

### Deliberately absent, and you should NOT add it

**No scale/diatonic quantization, no chord recognition, no voice-leading rules.** The
original design notes specify all of this. **The keyboard is the note source** — that
machinery exists to *guess* notes you now supply directly. Deleting it was the single
biggest scope reduction the MIDI reframing bought. Do not reintroduce it.

**No GPU use.** Kernel launch plus two PCIe transfers is 1–5 ms and jittery against a
1.33 ms callback deadline. For classical DSP it is a latency *loss*. It only earns a place
if a neural path is added.

**No abstraction over FFT backend, sample rate, or channel count.** Mono analysis in, mixed
out. Pick one FFT when you need one and live with it. A pluggable FFT is machinery around a
dimension that will not move.

**No effect graph, node system, or plugin registry.** One analyser, two engines, N voices,
one mixer. These are the standard ways a project like this drowns.

**No contact sensing (EGG, throat mic, accelerometer), no predictive/PLC extrapolation, no
neural engines.** All researched in the design notes; all out of scope for a software-only
system at this stage.

**No `-ffast-math`.** It permits reassociation and flushes denormals in ways that break
phase accumulation and make NaN checks unreliable — and a NaN in an audio path is a loud
noise in someone's ears. Denormal handling belongs in an explicit FTZ/DAZ call at stream
start.

### Honest about what it is

**The epoch tracker is not a GCI detector.** It is a peak tracker on an adaptive lowpass.
PSOLA needs a phase reference that is *consistent* from period to period, not one that is
anatomically the moment of glottal closure — and that is what this delivers. It has not
been validated against electroglottography, and it has already caused one hard-to-diagnose
bug (RESULTS.md #3).

**The synthetic voice in `tools/voice.hpp` is not a convincing human.** It is a
source-filter model — right physics, crude realisation. It exists because it gives ground
truth for F0 *and* formants, which no recording can. Judge processing by comparing dry
against wet, never by whether the dry sounds like a person.

---

## 5. How to use it

### Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

Needs a C++20 compiler and CMake 3.20+. Tests fetch doctest over the network; everything
else is dependency-free. Verified on g++ 13.3 / CMake 4.4 / Linux. **Not yet verified on
MSVC** — expect minor fixes around `#pragma pack` and `M_PI` (define `_USE_MATH_DEFINES`).

### The tools

```bash
./build/vh_tests                    # 48 cases. Run this before and after every change.
./build/vh_bench                    # CPU per block, cents error per interval, latency
./build/vh_demo ./audio             # synthetic singer -> dry + 6 wet variants
./build/vh_render take.wav out/ mytake "0:60,64,67; 2.5:59,62,67" 60
./build/vh_sweep take.wav out.wav psola -12   # ONE engine, ONE constant interval, Mode B
./build/vh_trace take.wav out.wav out.csv psola -12   # ...plus a per-block analyser log
```

Plus Python analysis tools (numpy only; matplotlib optional, for `--png`):

```bash
python3 tools/inspect_audio.py out.wav --from 11.2 --to 16.2        # spectrogram, F0, duty, waveform
python3 tools/inspect_audio.py a.wav b.wav --png inspect/           # side-by-side images
python3 tools/sweep_range.py ./build/vh_render ./build/vh_sweep take.wav ./sweep
python3 tools/click_sweep.py ./build/vh_trace .                     # the continuity regression
python3 tools/ensemble_probe.py one.wav four.wav --n 4 --auto       # sibilant coherence
python3 tools/mu_probe.py --selftest ./build/vh_sweep melody_dry.wav # is mu being applied?
```

**`inspect_audio.py` is the one to reach for when something sounds wrong and no number
explains it.** Every other measurement in this repo is a scalar, and a scalar can only
answer the question you thought to ask — VH-001 scored healthy on three of them at once,
because a no-op preserves formants, preserves periodicity and leaves no gaps. Its
`duty` view names the VH-008 pulse-train artefact, which no existing metric would have.

**`click_sweep.py` is the one to run after any change to a shifter, the analyser, or the
Engine's mix loop.** It takes about a minute and it is the difference between knowing the
output is continuous and hoping so. Pass `--json` and a later `--compare` to see deltas
rather than only totals — see §9 for the workflow this is part of.

`vh_render` is the one you will use most: **any real recording**, plus a chord progression
as `time:note,note,note; time:...`, plus a root MIDI note for Mode B. It writes the same
six variants as the demo so they are directly comparable.

`vh_offline` is the older simple harness (one chord, held throughout). `vh_render`
supersedes it.

### The development loop

1. Change something in `core/`.
2. `vh_tests` — catches regressions in seconds, needs no audio hardware.
3. `vh_render` on a real recording — listen.
4. `vh_bench` — confirm you did not blow the CPU budget.

This loop is the whole reason `core/` links no framework. **Iterating on DSP through a
plugin host is miserable**: you cannot diff the output, cannot regression-test it, cannot
bisect, and every run is a different performance.

### Where to make common changes

| You want to… | Go to |
|---|---|
| Add a new pitch shifter | Implement `IPitchShifter` (`shifter.hpp`). Nothing else changes. |
| Change how the two engines hand over | Write an `IBlendPolicy` (`blend.hpp`). ~20 lines. Swappable at runtime. |
| Change what "same voice" means | `PreservationSpec` (`preservation.hpp`) — seven independent rows. |
| Swap the pitch tracker | Implement `IAnalyzer` (`analysis.hpp`). |
| Trade playability against coherence | `Engine::setBlendAlignment(0..1)`. 0 = fast path early (Bloomberg's choice), 1 = phase-coherent, no latency win. |
| Add voices | `kMaxVoices` in `types.hpp`. Costs CPU, not latency. |

### Rules that are not negotiable

- **No allocation, no locks, no I/O on the audio thread.** Enforced, not documented: mark
  audio-thread functions with `VH_RT_SECTION()` and the test build's `operator new`
  override turns any violation into a red test. There is a meta-test proving the detector
  actually fires, because a safety net that catches nothing is worse than none.
- **All allocation happens in `prepare()`.**
- **Every crossfade is at least C¹** (zero slope at both ends). A fade with a kink in its
  derivative clicks no matter how long you make it.
- **Never switch between two signal paths with a branch.** This is the same rule one level
  up, and milestone 3 exists because it was broken in four places at once. If the output can
  come from either of two sources, it must be able to come from a mix of them, and the mix
  must move continuously. A mode flag in an audio path is a click waiting for a consonant.
  See `BUGS.md` VH-002.
- **A per-sample transcendental needs a reason.** Both crossfade shaping and the note
  envelope compute a cosine, and both are gated on the thing they shape actually moving.
  Ungated, they cost more than the DSP: 16 voices at a 128-sample block is 2048 calls that
  almost always return the same number, and it took CPU from 12% to 37% of budget.
- **Run any new shifter through the "starts with silence" and "extreme ratios" tests**
  before trusting it. See §6.

---

## 6. What the bugs taught, in one place

Seven significant bugs were found and fixed, every one by measurement rather than reading.
Full write-ups in `RESULTS.md`. The transferable lessons:

**Real input is not merely noisier than synthetic input — it is structurally different.**
Three bugs surfaced within minutes of using real recordings that 36 synthetic tests had
missed entirely. Real audio starts from silence, arrives before you are ready for it, and
has stretches where the model does not apply. None of that is captured by adding noise to a
sine. The `inf` bug (negative read position → undefined unsigned cast → cubic overflow)
existed purely because every synthetic test began with a fully primed steady tone.

**Some bugs never heal.** The history-gate bug clamped a voice's read cursor to zero if a
key was pressed before enough audio existed, after which cursor and write head advanced in
lockstep one block apart — permanently. It silenced the first chord of every session.
Symptoms that persist rather than recover point at initialisation, not at steady state.

**The symptom usually points away from the cause.** A 6 dB quiet blend was a *lookback*
problem, not a blend-weight problem. A PSOLA octave error was an *overlap-add* problem, not
a pitch problem. A 220 dB dropout was an *unsigned underflow at note-on*. Instrument and
measure; do not reason from the symptom's category.

**Measure the thing, not a proxy for it.** Two of my own tests were wrong: one asserted
bit-exact F0 equality when the held value legitimately drifts a few cents; another probed a
single frequency to find a formant, which measures window leakage, because a formant only
shows energy where a harmonic lands inside it. Band energy compared *between* two engines
is the measurement that works.

**A fix can be wrong in the same way.** My first silence-backstop tested "were any grains
placed this block?" — but for downward shifts most blocks legitimately place none and emit
earlier grains' tails. The tests caught it immediately. Zero grains is healthy operation.

---

## 7. Where to pick up

Ranked by value per unit effort.

**0. Done — VH-008 closed by listening.** `mu+tilt` A/B'd against the pre-profile engine
on both real recordings, -24 st through +19 st: unambiguous improvement at every interval,
most audible at -24 and -12 st. See `RESULTS.md` milestone 2. **Still open:** VH-010, how
much of it is the open-quotient correction versus mu incidentally lengthening the grain —
and the comparison was only against the pre-profile engine, not against a source-filter
engine or the blend-tilt fallback.

**1. Record your own voice and run `vh_render`.** No code required. Your microphone, your
room, your consonants, your register are all untested; the two validation recordings are
studio material at comfortable levels. This will find things.

**2. Apply `Humanization` in `Engine::process`.** The struct is fully specified and unused.
Per-voice detune, independent vibrato with randomised phase, slight timing jitter. This is
the difference between "chorus of robots" and "backing vocalists", and it is a day's work.

**3. Write the JUCE shell.** Standalone plus VST3 from one codebase, real MIDI in, ASIO
out, 64–128 sample buffers. Then run LatencyMon on the target machine — the benchmark's
~2 ms `max` outliers are container scheduler noise of exactly the kind LatencyMon exists to
characterise. Keep the shell thin: pump MIDI into `Engine`, call `Engine::process`, nothing
else.

**4. Source-filter engine (WORLD or LPC).** No longer needed for the envelope warp — that
landed in milestone 2. Needed for the two things resampling cannot do: generate tract ring
beyond one source period, and move formants without stretching the glottal pulse with them.
WORLD's reference implementation is offline-batch; making the analysis causal and streaming
is the actual work.

**5. Onsets.** The weak spot, as predicted throughout the design docs. A voice entering
mid-vowel now gets a C¹ raised-cosine ramp instead of a linear one, which removed the kink,
but there is still no attack shaping and no ducking on high analysis residual.

**6. VH-011 — granular crossfade seams at large upward shifts.** The one cell of the click
grid still above the dry floor. Probably the algorithm rather than a defect; read the entry
before spending time on it, particularly the note about NOT lengthening the crossfade.

**Cheap test worth doing early:** practitioner consensus is that a dark capsule (SM58)
beats a large-diaphragm condenser through any harmonizer, because sibilance is where these
things fall apart. May solve more than code will.

---

## 8. Provisional versus load-bearing

Change the provisional freely. Think hard before touching the load-bearing.

**Load-bearing** — things rest on these: absolute `Pos` addressing; shared analysis with
per-voice synthesis; `core` having no framework dependency; the no-allocation rule being
test-enforced; freeze being nothing but a cursor that stops advancing; blend as a
continuous C¹ crossfade; equal-gain (not equal-power) blend normalisation; holding F0
through unvoiced frames; one shifter instance per voice; Modes A and B being different
instruments; **every path switch being a crossfade rather than a branch** (milestone 3 —
see BUGS.md VH-002 for what it cost to not have this the first time); **the granular
crossfade's length being snapshotted at fade start, not recomputed while a fade is running**.

**Provisional** — meant to move: the voicing profile's exponents (`kMu = 0.30` is derived
from two population datapoints and is the only one with data behind it; `kTilt = 1.0`
follows from open quotient being scale-free and is untested by ear); `kMaxVoices = 16`; 2 s input history; YIN's 70 Hz floor
and 0.15 threshold; 128-sample analysis hop; 200 ms max F0 hold; `envelopeWarp = 0.8`;
`RegisterSplitPolicy` crossover at 220 Hz (a pure guess); `OnsetHandoverPolicy` 25→70 ms
(the parameters most worth tuning by ear first); oldest-first voice stealing; the 4×
decimation factor in YIN; the epoch tracker's 1.8× cutoff ratio; Mode B's declared root;
**the 8 ms grain/passthrough handover** (`kHandoverMs` in `psola_shifter.cpp` — tune on
/t/ and /k/ before anything else in that file); **the voicing gate's `releaseHops = 4`**
and **F0 momentum's `maxStepCents = 120` / `jumpConfirmHops = 2`** (all in `YinConfig`);
**the granular geometry smoother's 20 ms time constant**; **the per-voice decorrelation
delay's 0.5–4 ms spread** (`Engine::noteOn`).

**One provisional decision with an outsized consequence:** if Mode B's root ever has to be
derived from the voice rather than declared, Mode B collapses into Mode A's latency profile
and the dual-engine blend becomes the only remaining latency lever.

---

## 9. Delivering a change

*This section is procedure, not architecture — how a change gets from a working tree into
something the next person (human or AI, in a fresh session with none of this context) can
apply, verify and trust. Follow it for anything beyond a one-line fix.*

### The shape of a delivery

A delivery is three things, and they are not interchangeable:

1. **A patch series** — commits, and only commits that belong in history: source, tests,
   tools, doc prose. Reviewable, bisectable, `git am`-able.
2. **Regenerable evidence** — renders, reels, sweep output. Never in the patch. Every byte
   of it is producible from the tools *in* the patch, run against material named in
   `ATTRIBUTION.txt` or synthesised by a documented recipe. If it cannot be regenerated this
   way, something is wrong with the tools, not with the rule.
3. **A measured claim** — a line in `RESULTS.md` or `BUGS.md` citing a number that came from
   running (2) with the code in (1). "Fixed" is not a claim; "99 → 19 clicks, `--json`
   attached" is.

Mixing (2) into (1) is the single most common way a repository like this rots: a committed
WAV file is a binary diff that means nothing on the next `git log`, balloons the repository
forever (deleting it later does not reclaim the history), and cannot be re-derived to check
whether a later change broke it. `.gitignore` already excludes `*.wav`, `audio/`, `out/`,
`sweep/` and `inspect/` for exactly this reason — the discipline predates this section, this
section just names it.

### Producing the series

**Commit as you go, in dependency order, not as one squash at the end.** Each commit should
be the smallest unit that (a) builds and (b) leaves `vh_tests` green — including any commit
that only *adds* a source file not yet referenced by `CMakeLists.txt`, which builds fine by
definition. A commit that only wires an already-added file into the build is legitimate and
often the cleanest way to split a change that touches `CMakeLists.txt` from two directions
at once (new tests and new tools both add to that file; wiring both in its own commit keeps
"the source exists" and "the source is compiled in" separately bisectable).

**A REAL ORDERING BUG THAT SHOWED UP ASSEMBLING THIS EXACT SERIES**, kept here because it is
the non-obvious failure mode: a behavioural change (adding per-voice output latency) and the
one existing test whose assumption it invalidated (`b->latencySamples() == 0`, a proxy for
"Mode B pays no analyser cost" that only happened to equal zero before the change) were
first split into separate commits. The first commit built cleanly but **failed its own test
suite** — not caught until every commit in the series was checked out and built in turn (see
Verifying, below). The fix was not to reorder commits but to recognise that a test whose
assumption a commit invalidates is *part of that commit*, not a follow-up. A test fix that
would pass identically on the code before and after a change (a genuine improvement in what
is asserted) can go in its own commit or earlier in the series; a test fix that only starts
passing again because of the change belongs with the change.

```bash
git add <files for this logical step>
git status --short          # SANITY CHECK before every commit, not just the first: confirm
                             # the staged list is exactly what you intended. `git reset
                             # --soft` does NOT clear the index — if a previous attempt at
                             # this series is being redone, use a bare `git reset <base>`
                             # (mixed, the default) or the next `git add` stages nothing new
                             # and the following commit silently sweeps up everything still
                             # sitting in the index from before. This exact mistake produced
                             # a single 21-file commit while assembling this series.
git commit                  # a real commit message: what changed, why, what was measured,
                             # what was tried and rejected. This is `BUGS.md`'s discipline
                             # ("record what you ruled out, not just what you suspect")
                             # applied to commit messages instead of a ledger entry.
```

**Do NOT commit rendered audio, ever**, even temporarily to "just get the demo working" —
`.gitignore` already stops `*.wav`, and the reason is in the paragraph above this one.

### Producing the patch file

```bash
git format-patch <base-commit> --stdout > NAME.patch
```

`--stdout` concatenates the whole series into one mbox-format file, which `git am` reads
correctly as a sequence — this is what "one patch file" means for a multi-commit delivery,
and it is not the same file format `git diff` / `git apply` produce. **Prefer this over
`git diff` + `git apply` in general**: `format-patch`/`am` preserves commit boundaries,
authorship and messages, which is what makes the series bisectable and reviewable commit by
commit on the receiving end rather than as one undifferentiated blob. Reach for
`git diff --cached` only for a single uncommitted change too small to be worth a commit
message of its own.

### Verifying the series, not just producing it

A patch that has not been applied to a clean checkout is a claim, not evidence — the exact
distinction §1 of this document draws for the code itself. And a series that has not been
checked out commit-by-commit is a claim about *bisectability* specifically, which a single
end-to-end build cannot verify — see the ordering bug above, found by exactly this step.

```bash
rm -rf /tmp/verify && git clone <this repo> /tmp/verify
cd /tmp/verify
# `git am` needs a committer identity in the receiving repo, same as any commit:
#   git config user.email "you@example.com" && git config user.name "Your Name"
git am <NAME.patch                # applies the whole series as real commits, with authors
                                   # and messages intact. Fails loudly, mid-series, on the
                                   # first patch that does not apply — there is no separate
                                   # dry-run flag for this in current git; applying to a
                                   # throwaway clone IS the dry run.
git log --oneline                 # confirm the commit count and order match what you meant
                                   # to send

# Then, for EVERY commit in the series, not just the tip — REBUILDING FROM SCRATCH at each
# one. Reusing a build directory across checkouts silently tests the wrong commit's binary,
# which is exactly the mistake made assembling this section: an early version of this loop
# skipped `rm -rf build` and reported 48 passing tests at every commit, including the ones
# before test_continuity.cpp was even wired into CMakeLists.txt.
for c in $(git rev-list --reverse <base-commit>..HEAD); do
  git checkout -q "$c"
  rm -rf build
  cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null 2>&1 || { echo "$c CONFIGURE FAILED"; break; }
  cmake --build build -j >/dev/null 2>&1 || { echo "$c BUILD FAILED"; break; }
  ./build/vh_tests >/tmp/log 2>&1 || { echo "$c TESTS FAILED"; tail -20 /tmp/log; break; }
  echo "$c ok — $(./build/vh_tests 2>&1 | grep 'test cases')"
done
git checkout -q -                 # back to the tip
```

This is the same reason `core` links no framework (§ "Build and test" above): a claim that
can only be checked by trusting the person who made it is not a claim this project accepts
about its own code, and a patch is code — the whole series, not only its final state.

### Regenerating the evidence from a patched checkout

```bash
./tools/fetch_test_audio.sh .                     # the two validation recordings + the two
                                                   # demo sources (flamenco, speech) — real,
                                                   # CC-licensed, never committed
python3 tools/click_sweep.py ./build/vh_trace .   # the continuity regression, current numbers
python3 tools/demo_reel.py flamenco_dry.wav demo/ flamenco \
    --after ./build/vh_render --before <build of the pre-series commit>/vh_render \
    --chords "0:57,60,64; 3.5:55,59,62; 7:53,57,60,65; 10.5:57,60,64,69" --root 57
```

**Getting a "before" build:** check out the commit the series applies against into a
*separate* worktree or clone and build there. Do not gate the old behaviour behind a runtime
flag for comparison purposes — a flag can only disable what somebody remembered to make
switchable, and a separate binary built from separate source cannot flatter the new code by
construction. This is the same principle as the calibration controls in `tools/mu_probe.py`:
a measurement is only trustworthy if it has a way to be wrong.

### What goes where, concretely

| Artefact | Lives in | Reason |
|---|---|---|
| Source, tests, tools, doc prose | The commit series | Reviewable, bisectable, `git am`-able |
| `RESULTS.md` / `BUGS.md` prose *citing* a number | The commit series | It is text, and the number is a claim about code that IS in the series |
| Rendered WAVs, reels, sweep grids | Delivered alongside, never committed | Regenerable; a binary diff of it is noise |
| The recipe that produces those WAVs | The commit series (a tool or a documented command) | Without this, "regenerable" is an assertion |
| Third-party source recordings | Fetched by a script in the series, per `ATTRIBUTION.txt` | Licensing; and it keeps the repository's own history free of megabytes that are not this project's work |
