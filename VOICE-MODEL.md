# The voice model

*What we are actually trying to reproduce, and therefore what the DSP has to get right.
Read `HANDOVER.md` first for what the project is. This document is the reference the
other docs assume and never state: it says which properties of a voice must change when
you change its pitch, which must be protected from changing, and which cannot be
recovered from the recording at all.*

*Written for a reader with none of the originating context. Confidence is marked
throughout, because several rows here are physiology we are approximating rather than
measurements we have taken.*

---

## 1. The question this document answers

Not "how do I move the pitch of this waveform". That framing produces signal tricks and
runs out of road at large shifts, which is what `BUGS.md` VH-001 and VH-008 are.

The question is: **given a recording, what should come out if the pitch changes and
nothing else we care about does?** Pitch becomes a free parameter and everything else is
either derived from it, protected from it, or lost.

### Three readings, and the one we chose

"The same voice, lower" is ambiguous. The readings need different work and produce
different instruments:

| Reading | Physiology | Formants | Register |
|---|---|---|---|
| **1. Same singer, different note** | Folds change tension; tract unchanged | Stay put | Would change with the note |
| **2. A physically consistent lower singer** | Slacker folds **and** a longer tract | Drop ~15% for a 50% F0 drop | Chest, because that is what a bass does down there |
| **3. Voice-quality transfer** | Not a person. This singer's character on a lower-pitched instrument | **A choice** | **Held, by choice** |

**This project targets reading 3.** That is a design decision, not a physical fact, and it
is stated here so it can be revisited and eventually exposed as a toggle.

### Why that matters more than it looks

Under reading 2, a soprano's A5 shifted to A3 should acquire chest-voice character,
because a real bass at A3 has it. Under reading 3 we keep her head-voice character on
purpose — it is part of the quality being transferred. The whole point is that it is
recognisably *her*, two octaves down.

**Consequence: the hardest property to model (glottal source character, §2 A4) leaves the
compensation set entirely and becomes something to protect.** It also weakens the case for
a synthesis engine, because the source no longer needs modelling, only preserving.

**Second consequence, and the design core.** Reading 3 does not mean "hold everything".
It means the output should be *believably the same voice at a pitch that voice cannot
reach*. So the system is **physically informed, not physically restricted**: a real singer
cannot move three octaves, this one must, but the way properties shift across that range
should follow how real voices vary with F0.

That makes every parameter a **function of target F0**, not a constant. Real voices vary
nonlinearly across pitch; a fixed number is the wrong shape for the answer. See §6, which
is where the actual engine design lives.

This is what `PreservationSpec` was reaching for — seven rows, each answering *how much of
this property should follow the pitch*. §6 replaces each row's constant with a curve.

*Confidence: the framing is a decision, so it cannot be wrong, only regretted. The
physiology cited under reading 2 is well established but approximate (mu ~ 0.75-1.0,
US Patent 6,304,846).*

---

## 2. The three categories

Everything sorts into exactly one, and the category determines the work.

The discriminator is **not** "is this present in the recording" — nearly everything is.
It is: **should this property change when the pitch changes?** Under reading 3 that is
partly our choice, which is why several rows sit in A "by choice" rather than by physics.

| Category | Meaning | Work required |
|---|---|---|
| **A — Carried, and wanted as-is** | Present and valid at the target, by physics or by choice | **Protect it.** The requirement is negative: do not scale it |
| **B — Carried, but wrong for the target** | Present, belonging to a different pitch | **Compensate it** |
| **C — Not carried** | Never separately observable in the recording | **Generate it,** or accept the loss |

### Category A — protect; do not scale.

| Property | Why it is already right | How it gets broken |
|---|---|---|
| **A1. Breath noise / aperiodicity** | Physics. Glottal turbulence sits in similar frequency regions regardless of pitch | Resampling transposes it. A large part of the "underwater" character of the granular path |
| **A2. Vibrato, jitter, shimmer** | Physics. Expression, not pitch-dependent | Snapping to a target pitch instead of shifting by interval erases it |
| **A3. Vowel identity, articulation** | Physics. Tongue and jaw, independent of F0 | Any engine moving the spectral envelope with the ratio |
| **A4. Glottal source character** | **Choice** (reading 3). Register, pulse shape and source tilt are the voice quality we are transferring | Resampling stretches the glottal pulse: slower closure, duller upper harmonics. See §5's coupling warning |
| **A5. Singer's formant (F3-F5)** | Physics. A larynx-tube resonance, i.e. part of the envelope | Nothing extra needed. It moves correctly under mu because it *is* a formant |

**A4 is three properties, deliberately not collapsed into one.**

Register, glottal pulse shape (open quotient, closure abruptness) and source spectral tilt
do co-vary strongly in natural singing along the chest-head continuum, and the LF model
parameterises them with a small coupled set. It is tempting to call them one variable.

**We treat them as independent, on engineering grounds:**

1. **Knowing one does not give you the others.** Vocal effort moves them along a different
   axis from register — loud-high and soft-high differ in closure abruptness and tilt at
   the same register. At least two axes are needed and we measure neither.
2. **Each has a distinct, separately measurable signal-domain effect.** So corrections can
   be tested one at a time and compounded.
3. **A wrong joint model is wrong everywhere at once** and gives nothing to bisect. Three
   independent approximations degrade gracefully; one incorrect mechanism does not.

*If a future source-filter engine lands, the physical correlation becomes exploitable and
this decision is worth revisiting.*

**The design consequence of Category A generally:** fricatives must not be pitch-shifted
at all. Their identity is broadband noise shaped by front-cavity geometry — tongue and
teeth — not larynx rate. A real singer's /s/ does not transpose when they sing a fifth
higher. This is already `UnvoicedPolicy` and it is correct behaviour, not a compromise.
(The polyphonic consequence is `BUGS.md` VH-005, out of scope here.)

### Category B — compensate.

**B1. Vocal tract length — the mu warp.**
Under reading 3 this is a *voicing choice*, not a requirement: how much larger should the
instrument sound? `mu ~ 0.8` gives an octave-down that reads as a bass rather than as
slowed tape; `mu = 1` keeps the singer's own tract.

*Status: specified, unimplemented.* `PreservationSpec::envelopeWarp` carries the value and
nothing reads it, because neither current engine can move the envelope independently of
pitch. §5 has the cheap route.

**B2. Formant bandwidth.**
Within a speaker this is Category A. Under a mu warp it is not: a longer tract has lower
formants **and** narrower bandwidths, therefore a longer ring. The patent notes this
narrowing and links it to mitigating the buzzy character of pitch-lowered sound.

Comes free with a correct implementation of B1 — see §5.

### Category C — generate, or accept the loss.

**C1. Vocal tract ringing beyond one source period.**

The tract rings for 5-15 ms depending on formant bandwidth. That ring exists in the
recording, but past `T_source` after a pulse it is **superimposed on the next pulse's
excitation** and cannot be separated without a model.

A PSOLA grain is `2 * T_source` wide and cannot be wider — widening drags in neighbouring
glottal pulses and therefore the source's own periodicity, which is exactly VH-001. So
available ring is capped by **source pitch**, not by shift ratio:

| Source | T | Grain = 2T | Ring wanted | Consequence |
|---|---|---|---|---|
| Soprano 814 Hz | 1.23 ms | 2.5 ms | 5-15 ms | Badly truncated. Each pulse is a clipped burst |
| Melody 242 Hz | 4.1 ms | 8.2 ms | 5-15 ms | Mostly intact |
| Bass 90 Hz | 11 ms | 22 ms | 5-15 ms | Complete |

**This predicts downward shift quality is bound by SOURCE PITCH, not by ratio** — which
contradicts the `duty = 2 * ratio` framing in VH-008 and the "+/- 6 semitones" of VH-007.
*Unverified.* The sweep that tests it must disaggregate the two test recordings rather than
pooling them.

Recovering true ring requires modelling the filter: LPC or WORLD. Nothing else will,
because no rearrangement of copied samples produces audio that was never separately
observed.

---

## 3. What no model here captures

Recorded so nobody assumes the list above is complete.

- **The mu ~ 0.75-1.0 range is approximate** and vowel-dependent.
- **Register transitions are nonlinear.** Irrelevant under reading 3, where register is
  held — but it becomes relevant the moment the toggle in §1 is built, and a continuous
  parameter will be a smooth approximation of something that is not smooth.
- **Fold mass and tension are not independent of effort.** Loudness changes source
  character on its own, and we do not measure loudness for this purpose.
- **Coupling.** Source and filter are treated as independent. They are not entirely —
  there is acoustic loading of the folds by the tract, most audible exactly where a
  harmonic approaches a formant.

---

## 4. Why this reframing dissolves the duty-cycle argument

`BUGS.md` VH-008 reports that below -12 st the output is a burst followed by flat silence,
and derives `duty = grain_length / spacing = 2 * ratio`.

The formula is correct. The framing is not. **A real low voice does not have a 100% duty
cycle either.** Ring time is set by formant bandwidth and is independent of F0, so a bass
at 60 Hz has a 16.7 ms period in which his upper formants have long since decayed. Silence
between pulses is what a bass *is*. The measured 0.27 duty at -36 st is not far off
physically correct.

In a source-filter model you never set duty. You excite the filter at rate F0 and the
filter's decay decides how full the period looks — **with the correct formant spectrum in
the tail**, because the filter generated it rather than a copy running out of material.

**So the defect is not the gap. The defect is C1 — ring truncated at `T_source`.** "Fill
the gap" is a symptom-level goal, and VH-001 satisfies it perfectly while being completely
wrong.

This is why `duty_cycle()` in `tools/inspect_audio.py` must never be a standalone
pass/fail. Pair it with an output-pitch assertion, and preferably with spectral envelope
distance from dry.

---

## 5. What this implies for the engines

### The decoupling that already exists and is unused

PSOLA sets **pitch** by *when* grains are emitted. Grain **content** is currently copied
verbatim — but nothing requires that.

**Resample the grain content by mu as you copy it, and formants move by mu while pitch
stays where the synthesis spacing put it.** Independent formant control, time domain, no
FFT. In this codebase it is the read index in `PsolaShifter::placeGrain`: `ring.at(epoch +
k)` becomes an interpolated read at `epoch + k / mu`.

That implements **B1** and gives `envelopeWarp` its first consumer.

**It also partially addresses C1, and not by luck.** Resampling a grain by `mu = 0.8`
stretches it in time by 1.25x, so `duty = 2 * ratio / mu`. A longer tract genuinely has
lower formants *and* narrower bandwidths *and* therefore a longer ring — **B2 arrives
free**, because the thing we would otherwise be faking is the thing a longer tract
actually does.

### The coupling warning, and the open quotient — the real limit of the cheap route

**Grain resampling cannot separate source from filter, because it scales the whole grain.**
The question is whether it needs to. It does, and the reason is stronger than timbre.

A grain is the glottal pulse convolved with the tract response. The pulse occupies the
**open phase** of the fold cycle, and the meaningful quantity is the **open quotient** —
open phase over period — which is roughly scale-free at 0.4-0.6 across real voices. A bass
has an absolutely longer open phase than a soprano, in proportion to his period.

**Plain PSOLA copies the grain verbatim, so the excitation keeps its ABSOLUTE duration
while the output period grows by `1 / ratio`. The open quotient collapses by `ratio`.**
At -24 st a soprano's ~0.6 ms open phase sits inside a 20 ms period: open quotient ~0.03.
That is not a voice, it is an impulse train — and an impulse train has energy at every
multiple of its rate.

**This is VH-008's symptom and VH-008's own measurement.** `duty = 2 * ratio` *is* the
open quotient collapsing. The ledger measured it correctly and read it as a gap-filling
problem; it is a source problem. **The buzz has two causes, not one:** truncated ring (C1)
and an excitation that stayed soprano-sized in a bass-sized period.

*Confidence: high on the mechanism, and it is consistent with an already-published
measurement rather than a new prediction. Unmeasured: the relative contribution of the two
causes.*

So the excitation wants stretching by `1 / ratio` while the tract wants stretching by
`1 / mu`, and those differ by construction. One resampling factor cannot serve both.

**The cheap approximation.** The glottal pulse's spectral contribution is approximately a
**tilt** with a corner near `1 / (open phase)`. Correcting the open quotient is therefore
approximately a one-pole tilt filter whose corner scales with target F0, applied to the
OUTPUT and never touching the grain. First order, no filter model, no FFT. It also
retroactively justifies keeping source tilt as its own row in A4 rather than collapsing it
into register.

**The exact answer is still LPC**, which splits residual from filter so the two can be
scaled independently. That remains the sharpest argument for it.

### Coverage, current and proposed

| Row | Granular (resampling) | PSOLA (copy) | PSOLA + grain resampling | LPC / WORLD |
|---|---|---|---|---|
| A1: breath noise not transposed | ✗ | ~ | ~ | ✓ explicit |
| A2/A3: vibrato, articulation | ✓ | ✓ | ✓ | ✓ |
| A4: source character held | ✗ scaled by ratio | **✓** | ~ scaled by mu only | ✓ fully separable |
| A5: singer's formant | ✗ | ✓ | ✓ | ✓ |
| B1: mu warp | ✗ tied to ratio | ✗ | **✓** | ✓ |
| B2: bandwidth narrowing | ✗ | ✗ | **✓ free** | ✓ |
| C1: ring past `T_source` | ✗ stretched, wrong | ✗ truncated | ~ partly | **✓ generated** |

Note PSOLA's column is *better* than the proposed one on A4. That is the coupling above,
made visible: grain resampling trades a held source character for formant control.
Whether that trade is worth it at a given mu is a listening question, not an argument.

### Why LPC is still on the roadmap, and why it is not the prerequisite

LPC or WORLD is the only thing that answers **C1**, and the only thing that decouples
**A4 from B1**. Polyphony is nearly free: one filter, N excitations.

Its costs are real and should not be discovered late: autocorrelation plus
Levinson-Durbin, residual extraction, stability under frame interpolation, and the
buzziness that makes naive impulse excitation sound like a 1970s speech synthesiser. The
all-pole fit is **worst-conditioned at high F0**, where widely spaced harmonics
under-sample the envelope — the same reason the soprano is already the hard case.

Under reading 3, however, it is *not* needed to model the glottal source, only to avoid
scaling it. That is a much weaker requirement than full voice synthesis, and it is why
this document does not treat a synthesis engine as the goal.

---

## 6. The voicing profile — parameters as curves, not constants

**This is the design core, and it is what distinguishes this engine from a pitch shifter.**

A dial is the wrong shape. Real voices vary nonlinearly with F0, so every parameter in §2
should be a **function of the target pitch**, evaluated per voice per block.

### The form, and where the numbers come from

Take the population relations in the design notes:

| Transform | F0 ratio | Formant ratio |
|---|---|---|
| Male -> female | x1.75 | x1.167 (~15% up) |
| Male -> child | x2.5 | x1.4 (~40% up) |

Fit `mu = ratio^k`:

- male -> female: `k = ln(1.167) / ln(1.75) = 0.28`
- male -> child: `k = ln(1.4) / ln(2.5) = 0.37`

Take **`k ~ 0.3`**. Then an octave down gives `mu = 0.5^0.3 = 0.81`.

**That is the patent's `mu ~ 0.8`, derived rather than inherited** — and now it is a curve
that is correct at every ratio instead of one number that happens to suit octaves. Two
octaves down gives 0.66; an octave up gives 1.23.

### Why the power law is the right first form

The endpoints are the two engines this project already has:

| k | mu | Behaviour | Equivalent to |
|---|---|---|---|
| 0 | 1 | Formants held | **Plain PSOLA** |
| 0.3 | ratio^0.3 | Formants follow as a real body would | The proposed default |
| 1 | ratio | Formants track pitch exactly | **The granular engine** |

**The exponent is the dial, and it interpolates continuously between the two existing
engines** — not by blending their outputs, but as a single parameter with both current
behaviours as its limits. That is a strong argument that the form is right.

*Caveat: a power law is linear in log-log, so it captures the smooth trend and not
register breaks. It is a first form, not a final one. The profile should be an object that
a breakpoint table can replace later without touching callers.*

### Generalising

Every row gets its own exponent against the same F0 ratio, with a per-row multiplier for
taste:

| Parameter | Exponent | Default | Status |
|---|---|---|---|
| Formant position (B1, mu) | `k_mu` | 0.30 | Derived above |
| Formant bandwidth (B2) | `k_bw` | follows mu | Free with grain resampling, §5 |
| Source tilt corner (A4) | `k_tilt` | 1.0 | Holds open quotient. §5 |
| Aperiodicity gain (A1) | `k_ap` | 0.0 | Do not transpose noise |
| Ring extension (C1) | — | — | Not reachable without a filter model |

*All defaults except `k_mu` are guesses and are flagged as such. `k_tilt = 1.0` follows
from open quotient being scale-free; it has not been tested by ear.*

**Physically informed, not physically restricted.** A real voice cannot move three
octaves; this one must. The curve simply does not know it is supposed to stop, which is
the intended behaviour and not an oversight.

---

## 7. Open questions this document raises

1. **Is downward quality bound by source pitch rather than ratio?** (§2, C1.) Testable
   now: sweep with the two recordings disaggregated, measuring duty, output F0 and
   envelope distance separately. If true, VH-007 and VH-008 both need restating.
2. **How audible is the mu / source-character coupling?** (§5.) Decides whether grain
   resampling is a clean win or a trade, and at which mu it stops being one.
3. **Is `k_mu = 0.3` right by ear?** It is derived from two population datapoints, which
   is two more than 0.8 had, but it is still a fit to a smooth law over data that has
   register breaks in it. Sweep k and listen.
4. **How much of the VH-008 buzz is open quotient versus truncated ring?** (§5.) Decides
   whether the tilt filter is a large win or a small one, and it is testable by applying
   the tilt correction alone and re-measuring.
5. **Should reading 2 exist as a toggle?** (§1.) A physically consistent lower singer is a
   different and also musically valid instrument. It would need register compensation,
   which is the work reading 3 avoids.
6. **Does grain resampling interact badly with the epoch tracker?** Grain content and
   epoch spacing would no longer be at the same rate. Suspect it is fine — the epoch only
   sets where the grain is *centred*, and PSOLA's premise is that content and emission
   rate are independent — but it has not been reasoned through carefully, and `BUGS.md`
   VH-002 already suspects the epoch tracker of something else.
