# MIDI-Driven Vocal Harmonizer — Design & Plan of Attack

*Supersedes the framing in `realtime-vocal-transposer-design-notes.md`. That document
researched a different instrument (a backing-vocal harmonizer with a latency-critical
self-monitoring path). This one targets the instrument actually wanted. Where the two
conflict, this document wins — but the old one is still the reference for DSP technique
detail, and §9 of it remains the reading list.*

---

## 1. The seed

Sing live into a mic while playing chords on a MIDI keyboard, and hear your own voice
come back as those chords. Jamming with a band, in a room, monitored live. The
instrument is the harmonized output — not a studio effect applied afterwards.

**Why this changes everything relative to the old notes.** The old document's central
finding was that a formant-preserving shift of your own voice returned to your ears in
<10 ms is impossible, and its recommended escape was: monitor dry, send wet elsewhere.
That escape does not apply here, because there is no separate audience-facing version —
the wet signal *is* the instrument, and you must hear it to play it. So the latency
problem has to be attacked rather than sidestepped.

**Environment (CERTAIN):** Windows 11, RTX 3060, 16 GB RAM, Focusrite Scarlett 2i4
(native ASIO), MIDI keyboard. Software-only deliverable. CPU model unknown — see §9.

---

## 2. Two modes

| | Mode A — *Detachment* | Mode B — *Interval* |
|---|---|---|
| Behaviour | Sing anything; output lands on the played notes | Output = your pitch transposed by the played intervals |
| Ratio source | `f_target / f_sung` | `2^(n/12)` from declared root |
| Needs F0? | **Yes, critically** | No, for the ratio |
| Needs epochs? | Yes (if PSOLA) | Yes (if PSOLA) |
| F0 error | Passes straight to output pitch | Harmless — cancels out |
| Expression | Discarded (that's the point) | Preserved |

Mode A was chosen explicitly over "correction" (a small nudge toward a note you're
already aiming at). **This matters:** detachment means arbitrary shift ratios and total
dependence on F0 accuracy, where correction would have meant sub-50-cent shifts and a
much smaller error budget. Do not silently drift toward correction semantics later
without revisiting this row.

Mode B's ratio needs no pitch detection *only* because the root is declared (a key you
set, or the lowest held MIDI note). If the root is ever derived from the voice, Mode B
collapses into Mode A's latency profile. **PROVISIONAL: declared root.** If it turns
out to be musically unusable in practice, Mode B loses its latency advantage entirely
and the dual-engine split (§4) becomes the only latency lever.

---

## 3. What "same voice" means — the preservation spec

"Transpose the voice but keep it the same" is not one decision. It is this list, and
each line is independently settable. This list is the actual functional specification.

| Component | Policy | Why |
|---|---|---|
| Spectral envelope | **Preserve** | This is what makes it your voice, not a chipmunk |
| Envelope, at large shifts | **Warp, μ ≈ 0.75–1.0** | Preserving exactly under an octave drop implies an impossibly small tract producing an impossibly low F0 — reads as buzzy and synthetic |
| Aperiodicity / breath | **Preserve, do not shift** | Shifting it with the harmonics tonalizes breath noise |
| Unvoiced consonants | **Pass through untouched** | Fricatives are shaped by front-cavity geometry, not larynx rate. A real singer's /s/ doesn't move when they sing a fifth up. *But see §7 open issue.* |
| Timing | **Preserve** | Nothing time-stretches. Ever. |
| Vibrato | **Preserve in cents, not Hz** | Preserved in Hz, an octave-up halves the musical depth |
| Intonation deviation | **Mode A: discard. Mode B: preserve.** | This is the actual mode switch |

Note this list *is* a source-filter decomposition (F0 / envelope / aperiodicity) plus an
expression layer. That is an argument for source-filter methods on the quality path: its
parameters are literally these rows. In a delay-line or PSOLA shifter you get whatever
the algorithm happens to preserve.

---

## 4. Architecture: the dual-engine split

Adopted from Bloomberg's rig for Jacob Collier, which ran four instances of Antares
Harmony Engine (16 voices, better low notes, high latency) blended in parallel with a
TC-Helicon VoiceLive Touch 2 (lower latency, better high voices). Bloomberg states the
TC was run in parallel specifically to give the impression of lower latency.

**The principle: don't choose a latency, run two engines and blend.**

- **Fast path** — cheap, low latency, imperfect. Delay-line/granular or epoch-locked
  PSOLA. Arrives first; gives *playability* — you hear a note when you press a key.
- **Quality path** — phase vocoder + true-envelope correction, or source-filter/WORLD.
  Arrives 30–60 ms later; gives *the sound*. Forward masking means the fast path's
  flaws are largely unregistered once the good one lands underneath.

**The split policy is a first-class dial (CERTAIN — this is a stated requirement).**
Collier's rig split by register (low→slow, high→fast). That is an assumption about how
you play, and you have explicitly said the system must not bake in assumptions about
playing style. So the split must be:

- Runtime-tweakable, not compile-time.
- Expressible as a continuous crossfade curve, not a binary switch — the "waveform" of
  the handover is the thing being tuned.
- Parameterizable on more than one axis: register, time-since-note-on, shift magnitude,
  voicing state, per-voice manual override.

Design both engines behind one `PitchShifter` interface with a common voice-parameter
struct, and make the blender a separate module that neither engine knows about. That
separation costs nothing and is the difference between "tune the split in an evening"
and "rewrite the voice allocator."

**Time alignment caveat (CERTAIN).** The two paths have different latencies. Blending
them means either padding the fast path to the slow path's delay (which throws away the
entire point) or accepting that the crossfade is also a time-smear. The smear is the
feature — it is what makes the handover inaudible — but it must be *deliberate*, so the
blender needs an explicit alignment offset per voice, exposed and tunable.

---

## 5. Functions: include, discard, defer

### Include

- **MIDI note → voice allocation**, 8+ voices, with per-voice state.
- **Sustain/hold pedal → freeze.** Each voice needs its own read policy over a *shared*
  input ring buffer, so frozen voices detach from live input while you keep singing new
  material in. **Architectural, not a post-effect — cheap to design in, expensive to
  retrofit.** Trap: looping grains at a fixed period buzzes audibly within ~1 second;
  needs randomized grain selection from the captured window plus slow per-voice detune
  drift. (Collier's freeze was, in fact, the hold function on a free reverb plugin —
  worth knowing that the mythologised feature was an off-the-shelf bodge.)
- **Glide/portamento** per voice — Collier's "glide" is just a portamento rate setting.
- **Humanization**: per-voice static and drifting detune (a few cents), timing jitter,
  independent vibrato rate/depth/phase, slight per-voice envelope warp. Without this,
  N identical shifted copies sound like one flanged voice rather than N singers.
- **Voicing detector** with hysteresis, driving the unvoiced path.
- **Latency measurement harness** — see §8, phase 0. Build it first.
- **Mode A / Mode B switch.**

### Discard

- **Dry-path-only monitoring** (old doc §1, rule 7). Rejected — you must hear the wet.
- **Bone-conduction comb-filtering concern**, downgraded to a watch item. Combing needs
  correlated copies; a third or a fifth shares almost no partials with your dry voice.
  Unisons and octaves still will. Live with a band at 25 ms you are already standing
  8 m from an amp.
- **Scale/diatonic quantization, chord recognition, voice-leading rules** (old doc §4).
  **The keyboard is the note source.** All of this machinery exists to guess notes you
  are now supplying directly. Delete it. This is the single biggest scope reduction
  the MIDI reframing buys.
- **"Shift by interval, not to a target pitch"** (old doc rule 4) — correct for a
  backing-vocal harmonizer, wrong for Mode A, where erasing your intonation is the
  intent. Retained for Mode B only.
- **Contact sensing** (EGG, throat mic, neck accelerometer). Software-only constraint.
- **Vibrato prediction.** Convergence needs 300–600 ms of F0 history to save ~10 ms.
  Synthesize plausible vibrato instead — for harmony voices, decorrelated vibrato is
  what makes them sound like people.
- **GPU/RTX 3060 for classical DSP.** Kernel launch plus two PCIe transfers is 1–5 ms
  and jittery against a 1.33 ms callback deadline. It is a latency loss, not a win.
  Only earns its place if a neural path is added later.

### Defer

- **Sibilant/unvoiced polyphonic path.** See §7. Flagged as needing its own design.
- **Waveform extrapolation / PLC-style forward prediction** (old doc §3.1). Genuine
  latency lever, but only pays on steady vowels and is the most speculative component.
  Revisit once the baseline is measured.
- **Microtuning, hermode tuning, altered intonations.** Bloomberg names these as
  features he wanted and couldn't have without writing the shifter from scratch —
  which is exactly what you are doing, so they become nearly free later. Not phase 1.
- **Neural (DDSP, RAVE, BRAVE).** Only place the GPU would matter.

---

## 6. Stack

| Layer | Choice | Notes |
|---|---|---|
| Language | C++ | Real-time audio is CPU single-thread and cache latency |
| Framework | JUCE — standalone **and** VST3 from one codebase | Standalone for jamming (no host buffer games); VST3 for recording into a DAW |
| Driver | ASIO via Scarlett 2i4 native driver | Not ASIO4ALL, not WASAPI shared |
| Buffer | 64–128 samples target | 1.33–2.67 ms per buffer @ 48 kHz |
| FFT | KissFFT (BSD, readable) first; pffft later | Learning value first, speed second |
| Pitch detection | YIN | Low latency, robust, no upper frequency limit |
| Prototyping | Pure Data or Faust for algorithm sketches | Before committing to C++ |

**Real-time discipline (CERTAIN, non-negotiable):** no allocation, no locks, no I/O in
the audio callback. Lock-free SPSC ring buffers to UI and analysis threads.

**Windows-specific, do this before writing DSP:** run LatencyMon. WiFi and GPU drivers
routinely cause DPC spikes that produce dropouts no amount of clean DSP survives. Set
high-performance power plan, disable core parking, exclude the build directory from
Defender. This is unglamorous and it is usually the difference between "works at 64
samples" and "doesn't."

---

## 7. Open issues

- **Sibilant polyphonic collapse (needs design).** Passing fricatives through unshifted
  is correct per voice, but N voices then pass N *identical* copies of the same /s/,
  summing to one mono burst — the ensemble collapses to unison at every sibilant and
  re-widens when voicing resumes. Audible as a pumping width artifact. Real backing
  singers sibilate decorrelated. Needs its own path: likely per-voice decorrelation
  (allpass, micro-delay, spectral warp) on unvoiced frames.
- **Mic choice may dominate DSP (needs testing, cheap to test).** Practitioner consensus
  is that an SM58 sounds markedly better through any harmonizer than a large-diaphragm
  condenser like a C414, because the sibilant range gets ugly. A darker capsule may
  solve more of the sibilance problem than code will.
- **Acceptable latency (needs testing).** Deliberately unspecified. Build the
  measurement harness first, keep latency as low as possible, and find the threshold
  empirically rather than assuming one.
- **Whether the fast path is good enough alone (needs testing).** If it is, the quality
  path becomes optional and the whole architecture simplifies.

**Non-issue, recorded so it isn't re-raised:** your acoustic voice being audible in the
room alongside the processed output. This is a production concern, not a design one —
in jamming it is fine, and when recording you keep whichever signals you need.

---

## 8. Plan of attack

**Phase 0 — Measure before you build.**
*See `latency-budget.md` for the full serial/parallel decomposition and the four
measurements this phase exists to take.*
JUCE skeleton, ASIO in/out, straight passthrough. Loopback-measure actual round-trip
latency at 64/128/256 samples. Run LatencyMon. Establish the floor you are working
against. *Benchmark: known, reproducible round-trip number in ms.*

**Phase 1 — Foundations.**
COLA-compliant STFT with verified perfect reconstruction (no modification). Variable-speed
resampling, to hear the chipmunk effect first-hand. YIN, validated on your own recorded
voice across your full range. *Benchmark: bit-accurate round trip; YIN within a few
cents on sustained vowels; measured YIN convergence time at your lowest note.*

**Phase 2 — Fast path.**
Delay-line/granular shifter with the fixes: pitch-synchronous jump distance (Δ = k·T₀,
which makes the comb *stop existing* rather than merely smearing it), C¹-continuous
crossfade envelopes, equal-gain rather than equal-power law on correlated taps, jitter
the jump timing but **never** the read rate (the read rate *is* the pitch ratio;
modulating it FMs the output). Mode B first — no F0 needed for the ratio.
*Benchmark: playable 8-voice chord, latency measured, no audible comb on sustained vowels.*

**Phase 3 — Mode A.**
Add F0-driven ratio. Epoch-locked PSOLA for formant preservation.
*Benchmark: clean ±5 semitones on sustained vowels; measured added latency.*

**Phase 4 — Quality path.**
Phase vocoder → identity/scaled phase locking → transient reset → cepstral true-envelope
formant correction. Or source-filter/WORLD (note: WORLD's reference implementation is
offline-batch; the analysis side needs making causal and streaming, which is the real
work). *Benchmark: octave down that reads as a real bass at μ≈0.8, not slowed tape.*

**Phase 5 — The blender.**
Two engines behind one interface, continuous tunable crossfade, per-voice alignment
offset. *Benchmark: split policy changeable at runtime without recompiling.*

**Phase 6 — Musical layer.**
Freeze, glide, humanization, per-voice envelope warp.
*Benchmark: an 8-note held chord that sounds like eight singers, not one flanged voice.*

**Phase 7 — Deferred items.** Sibilant path, extrapolation, microtuning.

---

## 9. What I couldn't reach, and what's uncertain

- **CPU is unknown.** Everything about voice count and per-voice cost depends on it.
  Check it (`wmic cpu get name`) before committing to 8+ voices on the quality path.
  An RTX 3060 typically pairs with a Ryzen 5 5600 / i5-12400 class part, which would be
  comfortable — but that is an inference, not a fact.
- **Reference latency figures are inferred, not published.** Neither Antares nor
  TC-Helicon publishes a number. Reviewers report Harmony Engine needing 512–1024
  sample buffers to run clean, which is 11–21 ms of buffer *before* algorithmic
  latency — call the Antares path 40–70 ms and the TC hardware path 10–20 ms. The
  underlying maths of the slow path is inferred from Bloomberg's own diagnosis (that
  accurate FFT on low-frequency input is inherently slow), placing it in the phase-vocoder
  family. **PROVISIONAL throughout.** Treat as order-of-magnitude orientation only.
- **The strongest single piece of external evidence for this project.** Bloomberg, on
  what he'd do differently: he would consider low-latency DSP or FPGA platforms; their
  biggest struggle was system latency; they played tricks to reduce it, but writing the
  pitch-shifting algorithm from scratch would do it far better *and* would unlock the
  microtuning and altered intonations they had long wanted. He notes it would be a
  serious project needing real QA, and that what actually made the unit work for Collier
  was durability, remote control, and simplicity — none of which apply to you.
  **You are building the thing he described and declined to build**, minus every
  touring-hardware constraint that made it expensive for him.
- **The better version I couldn't reach here:** an honest latency budget. Without a
  measured floor on your machine and a tested tolerance threshold from your own playing,
  every latency number in this document is borrowed from elsewhere. Phase 0 exists to
  replace them.

---

## 10. Sources

- Bloomberg, B. (2020). *Making Musical Magic Live*, MIT PhD dissertation.
  Harmonizer construction pp. 166–168, 181–183.
  https://ben.ai/wp-content/uploads/2020/02/Making-Musical-Magic-Live-Dissertation-V1.pdf
- de Cheveigné & Kawahara, "YIN, a fundamental frequency estimator for speech and music",
  JASA 2002.
- Laroche & Dolson, "New Phase-Vocoder Techniques for Real-Time Pitch Shifting",
  JAES 47(11), 1999.
- Röbel & Rodet, "Efficient spectral envelope estimation… true envelope", DAFx 2005.
- Morise et al., WORLD vocoder, IEICE 2016; real-time sequential generator, APSIPA 2020.
  Code: github.com/mmorise/World
- Moulines & Laroche, "Non-parametric techniques for pitch-scale and time-scale
  modification of speech", Speech Communication 1995.
- Lester & Boley, "The Effects of Latency on Live Sound Monitoring", AES 2007, paper 7198.
- Zölzer (ed.), *DAFX: Digital Audio Effects* — TSM and pitch chapters.
- Julius O. Smith, *Spectral Audio Signal Processing* (free, CCRMA).
- Prior-art reference implementations to A/B against: BLEASS Voices (~$70), Waves
  Harmony, Antares Harmony Engine, TC-Helicon VoiceLive, Eventide H90 VocalShiftMIDI.
  The feature you are building is called **"MIDI Notes mode"** in the commercial world —
  useful search term.
- `realtime-vocal-transposer-design-notes.md` (project file) — retains the full technique
  inventory, the delay-line comb-filter fixes in its §3.2, and the reading list in §9.
