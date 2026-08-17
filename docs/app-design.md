# App Design — the standalone instrument

*How `vh::Engine` becomes a thing you can play. Companion to `HANDOVER.md` (which says what
the core is), `VOICE-MODEL.md` (what the DSP must get right) and `latency-budget.md` (where
the milliseconds go). Read those first if you have not.*

**Status: design, not yet built.** `app/` is still empty. Nothing in this repository has
ever made a sound through a soundcard; every measurement in `RESULTS.md` is file-in,
file-out. This document is the spec for changing that.

---

## 1. The seed

A Windows 11 laptop, a MIDI keyboard, a microphone, and speakers or headphones. You sing;
you play chords; you hear your voice as those chords, live, through the same `vh::Engine`
that the offline tools already render with.

Around that instrument, three things the core deliberately does not have:

1. **Device and signal selection of the kind a DAW gives you** — audio backend, device,
   sample rate, buffer size, which input channel is the mic, which outputs to use, which
   MIDI devices are live.
2. **A parameter surface for the harmonization engine that is editable while playing**, and
   savable as named profiles you can switch between.
3. **Testing conveniences** that make the thing usable without a full rig: the laptop
   keyboard as a MIDI source, the built-in mic as an input, and a WAV file as an
   alternative to a live microphone.

The user's framing, which governs the scope: *"basically whatever FL Studio or Ableton does
here I need to be able to do too."*

---

## 2. The reasoning — five constraints that shaped every decision below

**(a) The parameter surface is the product, not a convenience.** This project's method is
A/B listening: `voicing.enabled` exists purely as an A/B control, `UnvoicedPolicy::PassThrough`
is retained purely as the VH-005 A/B, `RESULTS.md` is built from before/after reels, and
`HANDOVER.md` §8 lists two dozen provisional constants explicitly wanting to be tuned by
ear. An app that cannot change those constants while sound is coming out does not serve the
project. Hence §4, which is most of the engineering.

**(b) A mono laptop and a stereo world.** The engine is mono in, mono out. Speakers and
headphones are stereo, and `Humanization::pan` has been specified-but-unread since the
beginning. `types.hpp` already predicts this ("output may become stereo for per-voice
panning; input will not become stereo") and `FrameCount` exists specifically so the
off-by-two does not happen when it does.

**(c) Mic plus speakers on one laptop fails in a specific, non-obvious way.** Not howl
first — in Mode A the ratio is `f_target / f_sung`, so YIN begins tracking the *harmony*
leaking back into the capsule and the pitch tracker corrupts before feedback is audible.
Mode B is structurally immune to that particular failure, because its ratio needs no F0 at
all. CERTAIN. Consequences: headphones for anything real, and file-as-input (§9) is not a
nicety but the thing that makes speakers usable at all.

**(d) The real-time rules are not negotiable and they are test-enforced.** All allocation in
`prepare()`, none in `process()`; no locks, no I/O on the audio thread (`tests/test_rt_safety.cpp`
enforces this via `VH_RT_SECTION`). Every mechanism in this document has to obey that, which
is why the parameter transport is a lock-free snapshot and not a mutex around a settings
object.

**(e) The existing evidence base must stay valid.** `RESULTS.md`'s measurements and the
`click_sweep.py` regression are rendered audio compared against thresholds. Any change that
alters mono output alters the baseline. So: **the mono render path must remain bit-identical**
(§5.4). This is a hard constraint, and it is the reason stereo is an added path rather than a
replacement.

---

## 3. Scope

### In

Standalone JUCE app, Windows only. Audio backend + device selection. Multi-device MIDI input
with channel/range/transpose filtering. The full live parameter surface with macro dials and
an expert panel. Named profiles with A/B slots. Stereo output with per-voice pan.
Humanization (detune, drift, vibrato, timing jitter, pan). Output stage (dry/wet, gain,
limiter) and input stage (trim, gate). Metering. Computer-keyboard MIDI. File-as-input with
looping. Recording of dry and wet. Round-trip loopback latency measurement.

### Out, for now

VST3 or any plugin format. macOS or Linux. More than one microphone. Any note-guessing
machinery — **the keyboard is the note source**; `HANDOVER.md` §4 is explicit that scale
quantization, chord recognition and voice-leading were deleted deliberately and must not
come back. MIDI output. Automation recording. Undo.

### Deliberately not, with reasons

- **No effect graph, node system or plugin host.** One analyser, two engines, N voices, one
  mixer, one output stage. `engine.hpp` names this as the standard way a project like this
  drowns.
- **No feedback suppression DSP.** The answer to feedback is headphones or a file input, not
  an adaptive notch filter we would then have to debug inside the instrument.
- **No parameter automation or LFOs beyond per-voice vibrato.** Everything else is a value
  set by hand.
- **No skinning, theming, or animation.** Meters and curve plots update; nothing else moves.

---

## 4. The parameter model

### 4.1 There are only five restart-only parameters

The intuitive split is "live params vs. config params". That is wrong. The real split is
**whether the value sizes a buffer** — and because this codebase separates `prepare()` from
`process()` with unusual rigour, almost nothing does.

| Restart-only | What it sizes | Notes |
|---|---|---|
| `sampleRate` | everything derived from it | |
| `maxBlock` | `fastBuf_`, `qualityBuf_` | `Engine::process` returns **silence** above it (`engine.cpp:230`) |
| `YinConfig::minHz` / `maxHz` | `minTau_`/`maxTau_` → `windowSamples_` → `window_`, `decimated_`, `diff_`, `cmnd_` (`yin.cpp:14-25`) | changing the field alone is **inert**, not crashing — `process()` never re-reads it |
| PSOLA `kLowestF0`, `kWindowTableSize` | `ola_`, `window_` (`psola_shifter.cpp:12-13,30,38-44`) | genuine reallocation |
| decorrelation bound `0.010` (`engine.cpp:57`) | `decorBuf_` | sets the ceiling the live spread must clamp under — see §4.5 |
| alignment bound `sampleRate/60.0*3` (`engine.cpp:48`) | `alignBuf_` | a *second* "lowest pitch" number — see §4.5 |
| `kMaxVoices` | `voices_`, both shifter vectors, `alignBuf_`, `decorBuf_` | runtime **cap ≤ 16** instead; raising the ceiling stays a rebuild, which `types.hpp:50` says is free |
| `kInputHistoryFrames` | the `AudioRing` | **needs `Engine` *reconstruction***, not just re-`prepare()` — the ring is built in the constructor's initialiser list (`engine.cpp:17`) |

Everything else is live: all of `YinConfig`'s behavioural fields, all of `GranularConfig`,
the epoch tracker's per-sample constants, PSOLA's `kHandoverMs`, the envelope times, the
freeze window, the decorrelation spread and `alignAmount_` — i.e. essentially all of
`HANDOVER.md` §8's provisional list, which is the point.

Two items need a derived value recomputed in `setTuning()` rather than just a field copy,
because `prepare()` bakes them: **`YinConfig::maxHoldMs`** → `maxHoldSamples_` (`yin.cpp:27`),
and **`kHandoverMs`** → `mixStep_` (`psola_shifter.cpp:51`). Both are one line and neither has
a mid-flight failure mode. Without that recompute they are silently *inert*, which is worse
than an error because the knob appears to work.

**Recommended small refactor:** move the `AudioRing`'s construction from the `Engine`
constructor into `prepare()`, so input history length joins the ordinary restart tier
instead of needing object reconstruction. It also removes the last piece of sizing that
happens before `prepare()` gets to see a config.

### 4.5 Five hazards the audit found, and how this design handles each

These came out of a per-item runtime-safety audit of every tunable. Each is a case where the
obvious implementation is quietly wrong.

1. **`hopSamples == 0` hangs the audio thread.** `yin.cpp:209` is
   `while (nextAnalysisPos_ + cfg_.hopSamples <= upTo)` — at zero it never advances. Not a
   click: an infinite loop inside the audio callback. **Every live parameter needs a clamp
   applied at `applyTuning()` time, in core, not in the UI widget's range.** A profile is a
   hand-editable JSON file, so the UI is not the only thing that can deliver a zero. Floor
   `hopSamples` at 32.
2. **The decorrelation spread is coupled to a buffer-sizing constant that looks unrelated.**
   The 0.5–4 ms spread (`engine.cpp:164-165`) is bounded by the 10 ms sizing literal at
   `engine.cpp:57`; push a live spread control past it and `dline[(dw - decorDelay) & dmask]`
   wraps and reads the wrong-aged sample — no crash, just quietly wrong audio. **Raise the
   prepare-time bound to 50 ms and hard-clamp the live control to it**, which also leaves
   room to use the spread as a real width control.
3. **There are two independently maintained "lowest pitch" numbers.** `engine.cpp:48` sizes
   the alignment buffer assuming a 60 Hz floor; `psola_shifter.cpp:12` sets `kLowestF0 = 70`,
   and that is what actually drives the quality path's `latencySamples()`. They are
   consistent today only because 60 < 70. Lower `kLowestF0` and the alignment buffer becomes
   silently undersized with nothing linking the two. **Make it one named constant with a
   `static_assert`.** Latent bug, unrelated to this app work — log it.
4. **The epoch tracker's 1.8× cutoff ratio is safe to read per sample but unsafe to *drag*.**
   `setCutoff()` calls `sin`/`cos` and is deliberately gated by 5% hysteresis
   (`epoch.hpp:90-93`) to rate-limit that *and* to avoid rewriting a running biquad's
   coefficients while `z1_`/`z2_` still reflect the old ones. A slider that publishes every
   frame during a drag defeats the gate and reintroduces coefficient discontinuities at
   drag-rate. **This parameter commits on drag-release, not continuously** — the only
   parameter in the set with that rule, so it needs to be marked as such in the UI code.
5. **`alignAmount_` is live but must be ramped, not stepped.** It is already correctly
   designed for live change (`engine.cpp:332-334` keeps the line primed for exactly this
   reason), but a large instantaneous step still jumps the delay tap. **Smooth it in the
   engine over ~50 ms**, rather than relying on every caller to ramp it.

Item 1 generalises into a rule: **`applyTuning()` clamps everything it receives.** The
transport carries values from a file as readily as from a knob.

### 4.2 `vh::Tuning` — one POD, versioned

Every live parameter, flat, trivially copyable, no pointers, no strings:

```
struct Tuning {
    std::uint32_t schemaVersion;

    // instrument
    RatioSource mode;  int rootMidiNote;  int voiceCap;
    float attackMs, releaseMs;            // was the hardcoded 8 ms
    float glideSemisPerSec;
    float freezeWindowMs;                 // was the hardcoded 500 ms

    // timbre
    PreservationSpec preservation;        // carries VoicingProfile

    // blend
    BlendKind kind;                       // fast | quality | register | onset
    float rsCrossoverHz, rsWidthOctaves;
    float ohStartMs, ohEndMs;
    float alignment;                      // 0..1

    // ensemble
    HumanizationSpec humanize;            // §5.2 — the *spread*, not per-voice values
    float decorMinMs, decorMaxMs;         // was the hardcoded 0.5–4 ms

    // engines + analyser (non-allocating fields only)
    ShifterTuning shifters;               // granular section + psola section
    AnalyzerTuning analyzer;              // YIN behavioural + epoch constants

    // signal path
    float inputTrimDb, inputGateDb;
    float dryGain, wetGain, outCeilingDb;
    bool  limiterOn;
};
```

`schemaVersion` is first and is checked on load. A profile file is exactly a serialized
`Tuning` plus a name — see §7, and note the payoff there.

### 4.3 `vh::TuningBus` — how it reaches the audio thread

Today [`Engine::setPreservation`](../core/include/vh/engine.hpp#L87) copies a struct into
`cfg_` with no synchronisation, and the audio thread reads blend-policy float fields
directly. With ~60 live parameters that is the central mechanism, and it needs to be
correct rather than probably-fine-on-x86.

- **Triple-buffered publish/poll**, single writer (UI thread), single reader (audio thread).
- `publish(const Tuning&)` off-thread; `poll()` at block start returns `nullptr` when
  nothing changed, so the steady state is one acquire-load and no copy.

  **Hazard, confirmed by a real bug during Track D: `applyTuning()` is a complete snapshot,
  never a partial override.** It copies every field of the `Tuning` it is given into `cfg_`
  — there is no merge, no "only touch what changed". A caller that constructs a fresh
  `Tuning{}` to change one field and publishes *that* silently resets every other live
  parameter to its struct default: root note, blend kind, humanization, the lot. This is
  exactly what happened in `tests/test_output_stage.cpp`'s stereo bit-identity test —
  `rootMidiNote` was set to 48 at `prepare()` and silently reset to `Tuning{}`'s own default
  of 60 by the first `applyTuning(Tuning{})` call, and the mispitched output only reached
  the render once PSOLA's ~28.6 ms algorithmic latency had elapsed, which is why the failure
  surfaced 1433 samples in rather than at sample 0. The fix was in the test's setup, not in
  `Engine` — this behaviour is correct and intentional, matching §4.3's own design.

  **Consequence for the UI (Track G): it must hold one authoritative `Tuning` in memory,
  mutate it field-by-field as the user turns knobs, and republish the WHOLE struct on every
  change — never construct a fresh partial `Tuning` as a "delta".** The same applies to
  loading a profile: `vh_profile::load()` already returns a complete struct (missing keys
  keep their compiled default, per §7), so loading a profile and publishing it wholesale is
  correct; publishing anything less than the full current state is not.
- `Engine::applyTuning(const Tuning&) noexcept` fans out: `analyzer_->setTuning()`, each
  shifter's `setTuning()`, its own fields. Non-allocating throughout, called from
  `process()`.
- New virtual `IPitchShifter::setTuning(const ShifterTuning&) noexcept {}`, defaulted to a
  no-op so `PassthroughShifter` is untouched. `ShifterTuning` lives in `shifter.hpp` with a
  granular section and a psola section; each shifter reads only its own.

  **Layering, resolved the hard way.** These structs were first built in `tuning.hpp`,
  because the track that built them was forbidden from touching `shifter.hpp`. That placement
  cannot stand: `tuning.hpp` includes `voice.hpp`, `voice.hpp` includes `shifter.hpp`, so
  `shifter.hpp` needing `ShifterTuning` from `tuning.hpp` closes a **circular include**.
  `shifter.hpp` is therefore the correct home — it is *why* §4.3 said so originally — and the
  dependency runs one way: `shifter.hpp` defines the tuning structs, `tuning.hpp` includes
  `shifter.hpp` and aggregates them.

  Related: `GranularTuning` and the existing `GranularConfig` must not both exist holding the
  same fields. One struct, with an alias if the old constructor API needs preserving for tools
  and tests.

**Rejected alternatives, recorded so they are not retried.** Routing tuning through
`ShiftRequest`: adds a pointer chase per voice per block and hands every shifter parameters
that are not its own. A `variant` or `any`: type erasure buys nothing with two concrete
shifters and risks allocation. Writing UI-thread floats straight into policy objects: that
is the data race this section exists to remove.

`setPreservation` / `setRatioSource` / `setBlendAlignment` stay as they are, for tests and
the offline tools. `rootMidiNote` gains the setter it currently lacks despite being read
every block (`engine.cpp:212`).

### 4.4 One documented decision deviated from, on purpose

[`engine.hpp:79-84`](../core/include/vh/engine.hpp#L79) makes the blend policy a
caller-owned raw pointer, deliberately, to keep refcount traffic off the audio thread. That
reasoning holds — but policy *parameters* must now be read by the audio thread, so they have
to live in `Tuning`.

Therefore: **`Engine` owns one instance of each of the four concrete policies** and selects
by enum from `Tuning`; `applyTuning` writes each policy's parameters into the engine-owned
instance. `setBlendPolicy()` survives as an override for custom experimental policies from
tools, and a custom policy takes precedence over the enum.

No refcounting, no lifetime hazard, and the load-bearing parts — runtime-swappable, and a
continuous C¹ crossfade rather than a branch — are untouched.

---

## 5. Core changes required

Each item: what, why, and how sure. These are the "everything that needs to be adjusted".

### 5.1 Sustain becomes sustain; freeze becomes its own control

**Today:** `setSustain(true)` plus a note-off means the voice detaches from live input and
loops a 500 ms captured window (`engine.cpp:180-185`). That is a genuine feature and it is
*not* what a keyboard player's hands expect from CC64.

**Change:**
- `Engine::setHold(bool)` — conventional sustain. While engaged, note-offs are deferred and
  the voice keeps tracking live input. On release, deferred voices go to `Releasing`.
- `Engine::setFreeze(bool)` — its own control, its own assignable CC. On engage, every
  *sounding* voice latches (stops responding to note-off) **and** stalls its cursor into a
  loop window of `freezeWindowMs` ending at its current position. On release, latched voices
  go to `Releasing`. Both can be engaged at once; freeze wins on cursor behaviour.
- `setSustain` is removed rather than kept as an alias, because two names for divergent
  behaviours is how this becomes a bug in six months.

**Confidence:** CERTAIN that separating them is right. **PROVISIONAL** that freeze should
latch *and* stall rather than only stall — latching is what makes it a pad-maker you can
sing over, but it is untested. If it feels wrong, the two halves separate cleanly.

**Migration:** exactly one test, `tests/test_engine.cpp:189` ("sustain freezes a voice by
stalling its cursor"), splits into two — one for hold, one for freeze. Nothing else in the
repo calls it.

### 5.2 Humanization gets applied

`Humanization` has been fully specified and almost entirely unread since the beginning; the
Engine reads one row (`staticDelayMs`). `HANDOVER.md` §7 ranks this #2 by value per unit
effort and calls it the difference between "chorus of robots" and "backing vocalists".

`Tuning` carries a `HumanizationSpec` describing the **spread**, not per-voice values;
per-voice values are derived at `noteOn` from the voice index, **deterministically** — the
same choice already made for the decorrelation delay at `engine.cpp:164`, and for the same
reason: a given chord must humanize identically on every render or the measurement is not
reproducible.

| Row | Application |
|---|---|
| `detuneCents` | multiplies the ratio: `r *= 2^(cents/1200)`. Set at `noteOn` from index. |
| `detuneDriftCents` | slow random walk, one step per block, one-pole smoothed, clamped to ±amplitude |
| `vibratoRateHz`, `vibratoDepthCents`, `vibratoPhase` | per-voice LFO advanced once per block; `r *= 2^(depth·sin(phase)/1200)`. Phase randomised per voice — `voice.hpp:56` argues this is the entire point. |
| `onsetJitterMs` | `noteOn` records `startAtPos = noteOnPos + jitter`; the voice is skipped until reached |
| `envelopeWarpOffset` | new `ShiftRequest::muScale`, applied by PSOLA after `muFor()` — a slight per-voice tract difference |
| `pan` | §5.3 |

**Two things worth recording.** First: a per-block ratio update is already normal — Mode A
recomputes from F0 every block — so the shifters tolerate it (PSOLA recomputes synthesis
spacing; granular has its 20 ms geometry smoother). **PROVISIONAL:** if the block-rate
quantisation of a 6 Hz vibrato is audible, interpolate the ratio within the block. At 128
samples that is ~60 steps per cycle, so it probably is not.

Second: **`PreservationSpec::vibratoConstantInCents` should be deleted, not implemented.**
The ratio is multiplicative, so the singer's own vibrato passes through in cents
automatically and unconditionally. The flag is dead because the architecture makes it always
true — which is a better outcome than a flag, and worth saying out loud so nobody "fixes" it
later.

**Defaults:** modest, not zero. A few cents of detune and ~15–25 cents at 5–6.5 Hz of
vibrato is the effect being bought. PROVISIONAL — tune by ear, it is the first thing to
sweep.

### 5.3 Stereo output with per-voice pan

- Add `Engine::process(const Sample* in, Sample* outL, Sample* outR, FrameCount)`.
- **Pan law: equal-power (sin/cos).** Note this is the *opposite* of the blend
  normalisation decision in `blend.hpp:58-65`, and for a consistent reason — that argument is
  that equal-power is wrong for *correlated* sources, because it bumps +3 dB mid-fade. Voices
  panned across the field are at different pitches, with different decorrelation delays, and
  are genuinely uncorrelated. Equal-power is correct here and equal-gain would be wrong.
- Pan positions spread deterministically by voice index, as with the delay spread.

### 5.4 The mono path stays bit-identical

The mono overload `process(in, out, n)` is kept, and it **must not route through the pan
stage**. Every number in `RESULTS.md` and every threshold in `click_sweep.py` compares
rendered mono audio; if the mono path changes, the entire evidence base needs re-baselining
for no benefit.

So the mix loop is templated on channel count: the mono instantiation is byte-for-byte the
current code path, and stereo is an added instantiation. This also keeps all 26 existing
`process()` call sites in tests and tools untouched. CERTAIN — this is a hard constraint.

### 5.5 Output stage and input stage

Belongs in core, per `app/README.md`'s rule that anything that feels like it belongs in the
shell belongs in core. Also: `tools/render.cpp` currently applies a `1/sqrt(N) · 0.9` fudge
in the *tool*, which means the tools and the app would otherwise gain-stage differently.

- **Input:** trim, then a gate. A laptop mic in a quiet room still gives YIN room tone to
  chew on.
- **Output:** per-voice `gain · envGain · velocityGain`, summed through pan, then dry/wet,
  master gain, and a limiter.
- **The dry signal is NOT delay-matched to the wet.** You hear yourself acoustically at zero
  latency anyway; delaying the dry to line up with the wet would be aligning a copy against
  something the ear is not using as its reference.
- **The limiter is a nonlinearity in the signal path.** It goes last, it is defeatable, and
  it defaults **off in the offline tools** so that measurement renders are unaffected. VH-009
  (upward shifts clip) is the reason it exists; a measurement that silently limits is a
  measurement of the limiter.

**PROVISIONAL:** a feedforward peak limiter, ~1 ms attack / ~50 ms release. Not lookahead —
lookahead is latency, and latency is the thing this project spends everything else to avoid.

### 5.6 Velocity, voice cap, panic

`Voice::velocity` is stored and never used — map it to voice gain through a curve in
`Tuning`. Runtime voice cap ≤ `kMaxVoices`. `Engine::allNotesOff()`.

### 5.7 Promoting the Tier-3 constants

`kHandoverMs` (psola, "tune on /t/ and /k/ before anything else in that file" — currently
impossible without a rebuild), the 8 ms envelope, the 500 ms freeze window, the 0.5–4 ms
decorrelation spread, the granular geometry smoother, the epoch tracker's 1.8× cutoff ratio
/ 0.25 loudness threshold / 0.6 refractory factor. All into `Tuning` via §4.3.

**Hazard, now measured rather than suspected:** reading the epoch constants per sample is
safe — the cost is a comparison, not a transcendental. What is *not* safe is dragging the
cutoff ratio, which defeats the hysteresis that protects a running biquad's coefficients.
See §4.5 item 4: that one parameter commits on drag-release. `kHandoverMs` needs `mixStep_`
recomputed in `setTuning()` or the knob is inert (§4.1).

### 5.8 What must NOT be added

The Tier-0 dials — `preserveEnvelope`, `envelopeWarp`, `scaleWarpWithShift`,
`preserveAperiodicity`, `vibratoConstantInCents`, `preserveTiming` — have no readers.
`envelopeWarp` and `scaleWarpWithShift` are documented as superseded by `voicing` and kept
as the A/B control; the rest are vestigial. **None of them goes in `Tuning`, the UI, or the
profile schema.** A knob that does nothing is worse than a missing knob, and a file format
that serializes fiction is worse still. Delete the vestigial ones; keep the two documented
A/B controls where they are, unexposed.

---

## 6. The app

### 6.1 Threads

| Thread | Does |
|---|---|
| Audio callback | `ScopedNoDenormals`; drain MIDI FIFO into `noteOn`/`noteOff`/`setHold`/`setFreeze`; `tuningBus.poll()` → `applyTuning`; `process`; write meters to atomics; push to recorder FIFO. Nothing else, per `app/README.md`. |
| MIDI callback | timestamp, push to a lock-free SPSC FIFO. No allocation. |
| UI (~60 Hz) | read meter atomics, redraw, `tuningBus.publish()` on edit |
| Disk | drain recorder FIFO to WAV |

FTZ/DAZ at stream start — the build deliberately refuses `-ffast-math`
(`CMakeLists.txt:44-48`) and requires this instead.

### 6.2 Panels

- **Devices** *(machine config, not part of a profile)* — backend, device, sample rate,
  buffer; which input channel is the mic; output pair. MIDI device multi-select with
  per-device channel filter, note range and transpose. Test tone. Input meter with peak hold
  and clip.

  **One backend dropdown, not two.** JUCE registers WASAPI as *three separate*
  `AudioIODeviceType` objects — "Windows Audio", "Windows Audio (Exclusive Mode)" and
  "Windows Audio (Low Latency Mode)" — alongside ASIO and DirectSound, and all of them
  enumerate together with one active at a time. So exclusive mode is a backend choice in the
  same list, not a checkbox, and **"Low Latency Mode" is the first thing to try on built-in
  laptop audio** — it did not exist in this design until recon turned it up.

  **Round-trip loopback measurement** — the Phase 0 measurement `latency-budget.md` §6 has
  been asking for since before any code existed. It is not a cross-check on JUCE's reported
  figures; it is **the authority**. `getInputLatencyInSamples`/`getOutputLatencyInSamples`
  are not trustworthy on Windows, per JUCE's own developers — DirectSound's figures are
  explicitly an averaged guess rather than a measurement. Display both, and label which one
  was measured.
- **Perform** *(always visible)* — input meter; F0, detected note, voicing lamp; active
  voices; latency split into algorithmic / I-O / total; CPU; wet/dry; output gain; limiter
  lamp; panic; A/B slot toggle; profile name.
- **Voice** — **Timbre Follow** (`muStrength`: 0 = "same body, new pitch" → 1 = derived
  → 3.33 = "body follows pitch"); **Source Brightness** (`tiltStrength`); unvoiced policy;
  attack/release; glide; ensemble spread (detune, vibrato, jitter, width).
- **Blend** — policy picker; alignment, labelled with its actual trade (playability ↔
  coherence, and there is no correct value); fast/quality solo.
- **Expert** *(collapsed)* — `kMu`, `kTilt`, mu and tilt clamps, corner multiple; YIN
  behavioural fields; epoch constants; granular geometry; PSOLA handover; decorrelation
  spread; freeze window; voice cap. Restart-only parameters visibly marked as such.
- **Bench** — §9.

### 6.3 The three curve displays

**This is the answer to "adjustable in a nice way", and it is the part of the UI that earns
its keep.** The voice model is not a set of constants; `voicing.hpp` is explicit that every
parameter is a *curve of the shift ratio*, and that the exponent is a single dial
interpolating between two engines. A slider alone hides that completely.

So: three plots, each with a **live dot showing where you are right now**.

1. **mu against interval**, −24…+24 st, with the endpoints labelled by what they *are*
   (`k=0` = plain PSOLA, holds formants; `k=1/kMu` = the granular engine, formants track
   pitch). The dot sits at the interval currently being played.
2. **Blend weight against the active policy's axis** — target pitch for register-split, ms
   since note-on for onset-handover. Shows the crossfade *shape*, which is the thing
   `blend.hpp` says is being tuned, using the user's own word ("waveform").
3. **The tilt shelf** — gain and corner, with a live dB readout.

A numeric live readout of achieved mu also answers the open question in `BUGS.md` VH-010 by
inspection instead of by re-rendering.

---

## 7. Profiles and machine config

**Two files, both versioned, never mixed.**

- `%APPDATA%\vocalharm\devices.json` — machine state: backend, device, rates, channel map,
  MIDI devices and filters, monitor mode. Never travels between machines.
- `%APPDATA%\vocalharm\profiles\*.json` — a serialized `Tuning` plus a name and free-text
  notes. Musical, portable, hand-editable.

**The payoff, and it is worth designing deliberately for: a profile is exactly the engine's
live parameter set, so the offline tools can load one.** The profile you tuned by ear becomes
the literal input to `vh_render`, `vh_sweep` and `click_sweep.py`. Measurement and listening
stop being two configurations that silently drift apart — which, given that this project's
entire evidence base is rendered audio compared against thresholds, is the difference between
a measurement that means something and one that describes a configuration nobody plays.

To keep that: serialization lives in a small `vh_profile` library shared by the app and the
tools. **`core` stays dependency-free** and `Tuning` stays a POD that knows nothing about
files. CERTAIN — `CMakeLists.txt:18-28` explains why core's freedom from dependencies is the
load-bearing build decision.

**Format: a flat, line-oriented `key = value` text file, extension `.vhprofile`, with
`schema_version` as its first key.** This supersedes the "JSON" this document originally
specified, and the reason is worth recording because JSON is the reflexive choice:

- `Tuning` is a **flat POD of scalars**. JSON's structure buys nothing here, and its cost is
  real — either vendoring a ~25k-line single-header parser into a project whose entire build
  is currently dependency-free, or hand-rolling a JSON parser, which is strictly more code and
  more failure modes than a `key = value` reader (~80 lines) for identical capability.
- `juce::JSON` cannot be the answer: `vh_profile` is linked by the **tools**, which must not
  depend on JUCE, so the parser has to exist independently of the app regardless.
- A flat text file is at least as hand-editable — the stated requirement — and diffs one
  parameter per line in git, which a nested JSON object does not.
- Unknown keys are ignored with a warning and missing keys keep their struct default, which is
  the whole migration story: an old profile loaded by a newer build gains the new defaults.

PROVISIONAL only in the extension name. If profiles ever need nesting — per-voice overrides,
say — this decision should be revisited rather than bent, because a flat format that grows
prefixes to fake nesting is worse than either honest option.

**Details this section originally left open, now settled by implementation.** Recorded here
because each is the kind of thing that silently ships a mistake if left to taste:

- **Duplicate keys are a hard error**, not first-wins or last-wins. Silently picking one is
  precisely how a hand-edited profile does something other than what it appears to say.
- **`schema_version` must be present and is read logically first — not required to be at byte
  position 1.** A name-keyed format has no positional semantics to protect, unlike the POD
  layout hazard `tuning.hpp` guards against.
- **A schema mismatch warns; it does not refuse to load.** Because keys match by *name*, an
  additive or removal-only version bump migrates for free — a new field takes its compiled
  default, a removed field reports as an unknown key. Refusing on any mismatch would block
  that harmless common case while still failing to catch the one hazard a keyed format
  genuinely cannot detect: a field that keeps its name and changes meaning (units, sign, enum
  semantics). That case needs a rename, not a version check. This was validated live —
  the schema went 1 → 2 mid-implementation and an existing profile loaded correctly with
  exactly the predicted warning.
- **Comments are whole-line only.** No trailing `#`, because `notes` free text may legitimately
  contain one.
- **Enum names are case-sensitive**, and serialize as names rather than integers — an integer
  enum in a hand-editable file means reordering the enum silently changes the meaning of every
  existing profile.
- **Numbers use `std::to_chars`/`from_chars` in shortest-round-trip mode**, which is provably
  exact for every representable value and, unlike `strtof`/`sscanf`, is not locale-sensitive
  about the decimal separator. Accepted limitation: leading and trailing whitespace inside
  `name`/`notes` is trimmed, as there is no escape for a boundary space.
- **Unsigned fields explicitly reject a leading `-`.** Not all `from_chars` implementations are
  guaranteed to, and a wrapped negative becomes a huge positive that `clamp()`'s floor-only
  rule would not catch.

Profile switching: instant, plus **MIDI program change → profile**, plus two A/B slots with
copy-between. A/B is how this project is developed; making it a first-class control rather
than a file-open dialog is most of the value.

---

## 8. MIDI

| Input | Mapping |
|---|---|
| Note on/off | `Engine::noteOn` / `noteOff`; velocity → gain curve |
| CC64 | `setHold` — conventional sustain (§5.1) |
| Assignable CC | `setFreeze` |
| CC1 (mod wheel) | Timbre Follow by default, reassignable |
| Program change | profile switch |
| Channel / note range / transpose | per device, in `devices.json` |

Multiple MIDI devices open simultaneously and merged, so the laptop keyboard and a hardware
keyboard are both live without a mode switch.

**Computer keyboard as MIDI.** Tracker layout (`ZSXDCVGBHNJM` lower octave, `Q2W3E4R…`
upper), octave shift keys, fixed-velocity slider.

JUCE's `MidiKeyboardComponent` already does computer-keyboard input, and it does it by
**polling live key state on a timer** rather than reacting to key events. Two consequences,
both found by recon rather than assumed:

- **OS auto-repeat is a non-issue by construction**, and JUCE imposes no simultaneous-key cap
  of its own — only real hardware rollover limits apply. An earlier draft of this document
  specified auto-repeat suppression; it is not needed.
- **But polling quantises note-on timing to the timer period.** For a testing convenience
  that is acceptable; it is not a substitute for a keyboard, and it should not be used to
  judge anything about the instrument's timing or latency. PROVISIONAL: if the quantisation
  is annoying in use, replace it with event-driven handling and filter auto-repeat manually —
  which is the cost of the polling approach, paid back.

**Honest limitation, PROVISIONAL until tested on the target laptop:** laptop key matrices
ghost or block above roughly 3–6 simultaneous keys, and which keys collide is
model-specific. Chords are the entire point of this instrument, so this may turn out to be a
two-or-three-note convenience rather than a substitute for the keyboard. Test it early; if
it is bad, a small on-screen chord-pad grid is the cheaper fallback.

---

## 9. The bench

Ranked by value, and the first item is the highest-value feature in this whole document:

1. **WAV file as the input source, looping**, replacing the live mic. Makes speakers usable
   (no feedback path at all), makes every session reproducible, and lets you tweak
   parameters live against a fixed take. The engine takes a buffer rather than a device, so
   this costs almost nothing structurally.
2. **A/B profile slots** with instant toggle.
3. **Record dry and wet to disk**, so any take can be re-rendered offline through
   `vh_render` afterwards.
4. **Loopback latency measurement** (§6.2).

---

## 10. Build

`app/` becomes a JUCE standalone target via `juce_add_gui_app`. `core` gains nothing: it
still links no framework, which is what keeps the test suite possible. New CMake option so a
headless CI build stays the default:

```
option(VH_BUILD_APP "Build the JUCE standalone app" OFF)
```

`vh_profile` is a new small static library, linked by both `app` and the `tools`.

**Version: JUCE 9.0.1** (released 2026-08-10). Note this is the 9.x line — most tutorials
and forum answers still target 8.x, so treat found examples with suspicion.

**Facts established by recon that the build depends on:**

- **`JUCE_ASIO` defaults OFF** and must be explicitly enabled. `JUCE_WASAPI` and
  `JUCE_DIRECTSOUND` default on.
- **JUCE now bundles a minimal ASIO SDK in-repo.** The old "download Steinberg's SDK and
  point CMake at it" workflow no longer applies — there is no `juce_set_asio_sdk_path`. The
  bundled SDK is dual-licensed Steinberg-proprietary **or GPLv3**, which is distinct from
  JUCE's own licence.
- **JUCE's free tier is AGPLv3, not GPLv3** (since JUCE 8, unchanged in 9.0.1). For a
  personal, non-distributed app this does not bite — the obligation attaches on
  propagating or conveying the software outside your organisation. It would bite if this is
  ever distributed or served over a network.
- **`juce_audio_utils` transitively pulls in `juce_audio_processors` and
  `juce_audio_formats`.** So the plugin-hosting module gets linked even though this app hosts
  no plugins. Accepted rather than worked around: hand-building the device selector to avoid
  it would cost more than the binary weight does.
- `ScopedNoDenormals` lives in `juce_audio_basics`, not `juce_dsp`. One per audio callback
  (§6.1).

---

## 11. Confidence register

**CERTAIN — things rest on these.** The five restart-only parameters being the only ones
that resize buffers (pending audit). The mono path staying bit-identical. Parameters reaching
the audio thread by lock-free snapshot rather than by direct write. Separating hold from
freeze. Equal-power pan for uncorrelated voices, against equal-gain for the correlated
engine blend. `core` staying dependency-free. Per-voice values derived deterministically from
voice index, not randomised. The limiter defaulting off in offline tools.

**PROVISIONAL — meant to move.** Every humanization default. Freeze latching as well as
stalling. The limiter's topology and timings. The computer-keyboard layout and whether it is
usable for chords at all. Block-rate vibrato quantisation being inaudible. Modest vibrato
defaults being better than zero. The panel grouping. Triple-buffering rather than an SPSC
queue for `Tuning` (a queue would preserve every intermediate value; a snapshot deliberately
drops stale ones, which is what a knob wants).

---

## 12. The better version I could not reach

**A parameter surface that knows what it changed.** Every profile edit could be timestamped
against the recorded audio, so an A/B could be *replayed* — "what did I have set 40 seconds
ago when it sounded right". That is the tool this project actually wants, given that its
whole method is A/B listening and that its hardest bugs (VH-002, VH-008) were found by
correlating audio against per-block state. It needs a design for parameter history and a
timeline UI, which is a project of its own, so it is deliberately declined here rather than
half-built. `tools/trace.cpp` already writes per-block CSV; that is the seam it would grow
from.

**A profile that interpolates.** Morphing between two profiles rather than switching would
make the A/B a continuous control. Declined because interpolating `RatioSource` or a blend
policy enum is meaningless, so it would need a per-field interpolability marker, and that is
schema complexity bought before anyone has asked for it.

---

## 13. What is still open

**Needs testing:**

1. **Felt onset latency of the fast path, which is not the number the engine reports.**

   *An earlier draft of this document claimed that instantiating the quality engine sets the
   read lag for the fast path too, and therefore that the blend's playability premise might
   be unavailable. That was wrong, and the correction is worth recording because the cursor's
   role is easy to misread.* The two engines position themselves **differently**: PSOLA emits
   at `cursor->next` (`psola_shifter.cpp:248`), which is exactly why the cursor must lag
   `writePos` by its lookahead — whereas granular anchors its read pointer to
   `writePos - baseDelay_` (`granular_shifter.cpp:121`) and advances the cursor only as
   bookkeeping, as its own comment says (`granular_shifter.cpp:218-221`: "the cursor tracks
   wall-clock input consumption, not the read pointer... The blender aligns on the cursor").

   So the architecture behaves as designed: granular output sits ~13 ms behind the live edge,
   PSOLA ~28.6 ms, they are already misaligned by ~15 ms with the **fast engine earlier**, and
   `alignAmount_` closes precisely that gap when coherence is wanted. Felt onset should be
   granular plus the I/O spine — roughly 21–23 ms at a 128-sample buffer. **Measure it; do
   not infer it.** That is what the app's loopback tool is for.

   **One real effect of the shared cursor does survive the retraction**, found while writing
   up VH-012: `noteOn`'s lookback and `process`'s history gate both use `max(fast, quality)`
   (`engine.cpp:148-153`, `engine.cpp:271-274`), so a blended voice's **onset is gated
   slightly later** than the fast engine alone would need. That delays *when a voice starts*,
   not *how stale its samples are* once running. The two are easy to conflate — that
   conflation is exactly what the retracted claim was — so the distinction matters when
   interpreting what the loopback tool measures.

   **Caution when reading `latency-budget.md` on this:** its fast/quality taxonomy predates
   the code. It labels **PSOLA as the fast path** and reserves "quality path" for a
   WORLD/phase-vocoder engine that was never built, whereas the engine as it exists uses
   granular = fast and PSOLA = quality. Its §1 rules and its serial/parallel decomposition are
   still sound and still authoritative; its per-engine assignments are not.

2. **The reported latency figure is not the felt one, and the UI must not present it as
   such.** `Engine::latencySamples()` adds YIN's analysis window to the shifters' transport
   delay for Mode A (`engine.cpp:77-98`). The window makes the F0 estimate *stale*; it does
   not hold samples up. Summing them conflates a convergence cost with a delay — the exact
   error `latency-budget.md` §1 rule 3 calls "the single most important thing in this document
   and the one most often got wrong". Consequence for §6.2: the Perform strip shows **felt
   fast-path latency** and **full-blend latency** as two numbers, never one sum. **Logged as
   VH-012**, together with the observation that PSOLA's latency is a fixed worst case rather
   than the pitch-dependent figure `shifter.hpp:85-92` requires of it.
3. Round-trip latency at 64/128/256 on both the Scarlett 2i4 (ASIO) and built-in audio
   (WASAPI exclusive). Every I/O figure in the docs is still borrowed.
4. LatencyMon DPC spikes, which decide whether 64 samples is real.
5. Computer-keyboard polyphony on the actual laptop (§8).
6. Whether block-rate vibrato is audible (§5.2).

**Needs information:**

7. ~~Whether WASAPI shared and exclusive enumerate as separate backends in JUCE.~~ **Answered:**
   three separate WASAPI device types, enumerating alongside ASIO and DirectSound in one list.
   One backend dropdown — see §6.2.
8. **Unconfirmed negatives, held as unproven rather than settled.** Recon could not
   exhaustively prove that JUCE ships *no* snapshot-value utility (§4.3) and *no* loopback
   latency tool (§6.2); both are best-effort absences. If either exists, prefer it to a
   hand-rolled version. Also unverified: the fine-grained feature differences between JUCE 9's
   licence tiers, only the financial terms were confirmed.

**Found while designing this, unrelated to the app, to be logged rather than dropped:**

9. **Two independently maintained "lowest pitch" constants** — `engine.cpp:48` sizes the
   alignment buffer from a 60 Hz floor, `psola_shifter.cpp:12` sets `kLowestF0 = 70`, and
   nothing links them. Consistent today only because 60 < 70; lower `kLowestF0` and the
   alignment buffer is silently undersized. One named constant plus a `static_assert`.
   **Log as VH-013.**

---

## 14. Delivery plan

Tracks are ordered by dependency, not by value. Each is a `git format-patch` series per
`HANDOVER.md` §9, and each commit must build and pass tests on its own.

| Track | Work | Depends on |
|---|---|---|
| ~~**A**~~ | ~~Runtime-safety audit of every tunable~~ — **done**; §4.1 corrected and §4.5 is its output | — |
| **B** | `Tuning`, `TuningBus`, `setTuning` fan-out, blend-policy ownership, `applyTuning` clamps, ring-into-`prepare()`; tests | — |
| **C** | Hold/freeze split; Humanization applied; stereo + pan; velocity, cap, panic | B |
| **D** | Input and output stages; limiter defeated in tools | B |
| **E** | `vh_profile` serialization; tools accept a profile file | B |
| **F** | JUCE app skeleton: devices, MIDI, audio path, meters, latency readout | B |
| **G** | Parameter UI: panels, macro dials, expert panel, the three curve displays | B, F |
| **H** | Bench: file input, A/B slots, recording, loopback measurement | F |
| **I** | Computer-keyboard MIDI | F |

**A–E are headless and test-covered**, which is deliberate: the majority of this work can be
verified by `vh_tests` and `click_sweep.py` before any GUI exists, and should be.
