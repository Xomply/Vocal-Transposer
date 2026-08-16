# BUGS

*A ledger for defects found but not fixed. Read `HANDOVER.md` first for what the project
is; read `RESULTS.md` for bugs that were found **and fixed** during a milestone.*

---

## Why this file exists

You will find bugs while fixing a different bug. Fixing them immediately is usually the
wrong move — it inflates the diff, mixes two changes into one commit, and means neither
gets tested properly. Dropping them is worse: an undocumented defect that someone
*noticed* is more expensive than one nobody found, because the next person pays the
diagnosis cost all over again.

So: log it here, keep going, and let someone else pick it up.

**This ledger is written for a reader with none of your context** — a future you, a
collaborator, or an AI assistant arriving empty on every session. That is why entries
carry more than a symptom. An entry that says only "downward shifts sound wrong" costs
the next person a day. An entry that says which line, what was ruled out, and how sure
you are costs them an hour.

The discipline that matters most: **record what you ruled out, not just what you
suspect.** Negative results are the expensive part of debugging and the part that never
survives in anyone's head.

---

## Status values

| Status | Means | Who moves it |
|---|---|---|
| **Backlog** | Logged. Nobody is working on it. Diagnosis may be complete or absent. | Anyone, on discovery |
| **Doing** | Someone is actively on it. Put your name in Owner so two people don't collide. | The person starting work |
| **In review** | A fix exists and is awaiting listening tests, a benchmark run, or a merge. | The person who wrote the fix |
| **Done** | Landed, with evidence. Move the entry to `RESULTS.md` and leave a one-line stub here pointing at it. | The person who verified it |

`Done` requires **evidence, not assertion** — a measurement, a regression test, or a
named audio file, exactly as `RESULTS.md` does. This project's entire culture is that
claims are measured; a bug marked fixed because it "sounds better now" is not fixed.

## Severity

- **S1** — produces wrong output that a user would call broken. Ship-blocking.
- **S2** — audible degradation in normal use. Not broken, but noticeably bad.
- **S3** — audible only at extremes, or cosmetic, or a latent trap.

## Entry template

```markdown
### VH-NNN — one-line symptom
**Status:** Backlog | **Severity:** S? | **Area:** file/module | **Owner:** —
**Found:** how it surfaced, and by whom

**Symptom.** What is observed, in terms a listener would use.
**Repro.** Exact commands. Must run headlessly.
**Measurement.** Numbers, so the fix can be shown to have worked.
**Root cause.** What is actually wrong, and HOW CONFIDENT you are.
**Ruled out.** What you checked that was NOT the cause. Do not skip this.
**Candidate fix.** With its trade-off, if one is known.
**Blocks / blocked by.** Other VH- ids.
```

---

# Open

### VH-001 — PSOLA reproduces the SOURCE pitch on downward shifts beyond about −11 semitones
**Status:** In review | **Severity:** S1 | **Area:** `core/src/psola_shifter.cpp` | **Owner:** —
**Found:** listening to `sweep_*_psola.wav` (octave sweep, Mode B). The lower three octave
segments did not sound lower than the source. Confirmed by measurement immediately after.

**FIX LANDED, AWAITING LISTENING TESTS.** Three changes, because fixing the first exposed
two further defects that the original bug had been masking:

1. **Grain sizing** — the root cause. `halfLen` is now `min(period, maxHalf_)` instead of
   `min(max(period, synthSpacing), maxHalf_)`.
2. **Overlap-add gain**, now clamped to `min(synthSpacing / halfLen, 1.0)`. The `S/H`
   correction is only valid while grains still overlap; once they no longer do it reached
   4.0 at two octaves down and 8.0 at three, and the output **clipped at full scale**.
   Peaks are now 0.21–0.32.
3. **Silence backstop**, now gated on time since the last grain rather than on an empty
   block. With correct geometry, three octaves down on a 242 Hz voice places a 396-sample
   grain every 1584 samples — nine consecutive blocks of *correct* silence. The old
   backstop read that as starvation and substituted unshifted passthrough, filling every
   gap with the source and reproducing the VH-001 symptom from a completely different line.

**Evidence.** Output F0 / input F0, soprano / melody:

| requested | wanted | before | after |
|---|---|---|---|
| −12 st | 0.500 | 1.000 | **0.476 / 0.512** |
| −19 st | 0.334 | 1.000 | **0.322 / 0.345** |
| −24 st | 0.250 | 1.000 | **0.242 / 0.272** |
| −36 st | 0.125 | 1.000 | **0.122** soprano; melody is a boundary case, below |

Upward output unchanged. `vh_bench` cents error on downward intervals improved as a side
effect (−5 st at 110 Hz: −1.7 → +0.2). 40 tests pass, zero warnings, CPU unchanged.

**Residual limit — the technique, not the code.** A PSOLA grain spans two analysis periods
(±one period around the epoch), so at very large downward ratios the tapered neighbouring
pulses inside the grain still carry source periodicity. The soprano (814 Hz) at −36 st
gives a correct 99 Hz; the melody (242 Hz) at −36 st asks for 30 Hz and the grain content
dominates instead. 30 Hz is below musical use, so this is recorded as a boundary rather
than reopened; the real answer is the source-filter engine.

**Before this can move to Done:** listen. The fix reintroduces the inter-pulse gaps the
original line was written to suppress, and roughness at large downshifts is the honest cost.
Per the evidence rule above, Done needs a listening note, not just green tests.

**Root cause.** The line previously read:

```cpp
const FrameCount halfLen = static_cast<FrameCount>(
    std::min(std::max(period, synthSpacing), static_cast<double>(maxHalf_)));
```

On a downward shift `synthSpacing = period / ratio > period`, so `halfLen` grew with the
spacing and each grain spanned `2 * synthSpacing` — `2 / ratio` periods **of the original
waveform**. At ratio 0.25 one grain contains eight source glottal pulses.

PSOLA lowers pitch by emitting **one pulse** less often. A grain holding eight carries the
source's own periodicity, so re-spacing those grains cannot change the pitch; overlapping
them merely reconstructs the original. At exact integer ratios the reconstruction is
perfect, which is why 1/2, 1/3 and 1/4 landed on 1.000 while ratios between them were
merely erratic.

The comment above that line named the true reason it was written: it prevented "periodic
holes". **The holes were not a bug.** Gaps between glottal pulses at a lower rate are what
a lower-pitched voice *is*. The symptom was suppressed and the function went with it.

**Ruled out.**
- Not the ratio computation. `Engine::ratioForVoice` returns exactly `2^(n/12)` in Mode B;
  granular receives the identical ratio through the identical path and shifted correctly.
- Not `maxHalf_` clamping. At the soprano's 59-sample period `synthSpacing` never reaches
  `maxHalf_` (686) anywhere in the failing range.
- Not the unvoiced path, and not F0 estimation — Mode B needs no F0 for the ratio, and the
  same analyser feeds the working granular engine.
- Not the measurement, though it nearly was. See the lesson below.

**Lesson worth carrying.** Two independent metrics and a passing test all confirmed broken
output. A ±3-semitone banded F0 tracker reported everything healthy, because a 787 Hz
signal autocorrelates strongly at a 197 Hz lag. Spectral centroid sat at 0.99x and was read
as formant preservation when it was in fact *nothing happening*. The gap test passed
because there were no gaps. **A no-op trivially satisfies all three.** Only measuring the
property the feature exists for — output pitch, with an unconstrained estimator — could
catch it.

---

### VH-008 — large downward shifts are a PULSE TRAIN, not a lower voice ("square wave")
**Status:** Done | **Severity:** S2 | **Area:** `core/src/psola_shifter.cpp`

Fixed and confirmed by listening. `duty = 2 * ratio` was measuring the **open quotient
collapsing**, not a gap needing filling — a PSOLA grain is copied verbatim, so the
excitation keeps its absolute duration while the output period grows by `1/ratio`. The
buzz had two causes: the collapsed open quotient (source) and the truncated tract ring
(still open, see VH-010).

Two corrections in milestone 2: the mu warp (raises duty exactly as predicted, zero cents
of pitch movement — measured in `RESULTS.md`) and the source tilt shelf (approximates
holding the open quotient constant across F0).

**Listening test, real recordings (not the synthetic probes), −24 st through +19 st on
both `sustained_dry.wav` and `melody_dry.wav`:** confirmed a large, unambiguous
improvement — "mu+tilt is much much better" than the pre-profile engine at every interval
tested, most audibly at −24 and −12 st, which is exactly where this entry was opened.

What this listening test does NOT establish: how much of the improvement is the open
quotient versus the still-truncated ring (VH-010 remains open, because no metric here
separates them), and it was A/B against `hold`, not against option 2's blend-tilt
fallback or option 1's source-filter engine — so it says the correction *works*, not that
it is the *best available* fix. Recorded as the honest scope of the result.

---

### VH-002 — clicks on the quality path when the source pitch moves quickly
**Status:** Backlog | **Severity:** S2 | **Area:** `core/src/psola_shifter.cpp`, `core/include/vh/epoch.hpp` | **Owner:** —
**Found:** reported by ear while auditioning the octave sweep; reproduced by measurement.

**Symptom.** Audible clicking during melodic movement. Absent on sustained notes. Present
across shift amounts, so it is not a large-shift artefact.

**Repro.**
```bash
./build/vh_sweep melody_dry.wav /tmp/a.wav psola 7        # pitch moves -> clicks
./build/vh_sweep sustained_dry.wav /tmp/b.wav psola 7     # steady pitch -> none
```

**Measurement.** First-difference outliers (>12x the local median jump). Zero in the dry
material of both files.

| material | +5 st | +7 st | +12 st |
|---|---|---|---|
| melody (moving pitch), PSOLA | 14 | 11 | 14 |
| melody, granular | 0 | 0 | 16 |
| sustained (steady pitch), PSOLA | 0 | 0 | 0 |

Clicks coincide with pitch movement: median |ΔF0| at click instants is 90–145 cents/frame
against a 65 cents/frame overall median. PSOLA-specific at +5 and +7; both engines at +12.

**Root cause.** Not diagnosed. Two candidates, untested:
1. **Epoch tracker instability during glides.** `epoch.hpp` is a peak tracker on a lowpass
   whose cutoff follows the fundamental at ~1.8x with 5% hysteresis. A fast glide moves the
   cutoff; if the peak picker relocks to a different crest the phase reference jumps and
   grains stop being coherent. **This mechanism has caused a bug here before** — RESULTS.md
   #3, where a fixed 900 Hz cutoff let F1 through and the reference jittered.
2. **Stale `synthSpacing` within a block.** `period` is read once per block from the shared
   analysis frame. During a fast glide the true period changes within the block, so grains
   placed late in it are spaced for a pitch that has moved on.

Distinguish them by logging the epoch series and the per-block period against click
timestamps. If clicks align with epoch-interval discontinuities it is (1); if they are
spread evenly through glides it is (2).

**Ruled out.** Not the source material — the dry files contain zero click-class
discontinuities. Not a large-shift effect — present at +5 st. Not exclusively PSOLA — the
granular path shows it at +12 st, which suggests a shared upstream cause (the analyser or
the epoch tracker) rather than something inside either shifter.

---

### VH-003 — PSOLA loses up to 17 dB of level on large upward shifts
**Status:** Done | **Severity:** S2 | **Area:** `core/src/psola_shifter.cpp`

Fixed. The overlap-add gain `min(S/H, 1.0)` came from a window-envelope argument; PSOLA is
not an envelope problem. What must be preserved is the glottal PULSE amplitude, and the
pulse sits at the window peak, so the correct gain is **1.0**. The clamp meant downward
already got 1.0, which is why the bug looked upward-only. Measurements and the residual
tail-cancellation component in `RESULTS.md`, milestone 2.

---

### VH-004 — broadband hash from small DOWNWARD shifts at low fundamentals
**Status:** Backlog | **Severity:** S2 | **Area:** `core/src/psola_shifter.cpp`, `core/include/vh/epoch.hpp` | **Owner:** —
**Found:** isolating voices in the demo's D chord after the chord's spectral centroid moved
when PSOLA's should not.

**Symptom.** A −3 st voice on an 87 Hz sung note injects roughly 30 dB more 3–8 kHz energy
than its siblings in the same chord. Not an envelope shift — a discontinuity signature.

**Repro.**
```bash
./build/vh_demo ./audio
./build/vh_render audio/dry.wav out2/ n38 "3.6:38" 60
./build/vh_render audio/dry.wav out2/ n45 "3.6:45" 60
python3 tools/verify_render.py ./audio --isolate out2 38 45 50 57 --window 3.9 4.6
```

**Measurement.** Band energy vs dry, 3.9–4.6 s, sung F0 87.4 Hz:

| voice | 60–300 | 300–1k | 1k–3k | 3k–8k |
|---|---|---|---|---|
| note 38 (−3 st) | −2.9 | −1.7 | **+15.4** | **+29.8** |
| note 45 (+4 st) | −2.9 | −0.7 | −2.6 | −2.0 |
| note 57 (+16 st) | −7.9 | −9.5 | −6.3 | −4.5 |

The +16 st voice is *clean*. The −3 st voice is the problem, which is the opposite of what
the "PSOLA ends at ±6 st" framing predicts.

**Root cause.** Not diagnosed. Localised to a voice, not to a line. 87.4 Hz sits close to
YIN's 70 Hz floor and gives a 549-sample period, so both the epoch tracker's adaptive
cutoff and the analyser's window are near their limits. Note this is a *different*
condition from VH-001: −3 st tracks pitch correctly on both test recordings.

**Ruled out.** Not shift magnitude (+16 st in the same chord is clean). Not the chord sum
(reproduces with the single voice alone). Not the release envelope (the window sits well
inside the note).

---

### VH-005 — sibilant polyphonic collapse
**Status:** Backlog | **Severity:** S2 | **Area:** `core/include/vh/preservation.hpp`, `core/src/engine.cpp` | **Owner:** —
**Found:** pre-existing; documented in `HANDOVER.md` §7 and `RESULTS.md`. Logged here so it
lives in one ledger with everything else.

**Symptom.** Passing fricatives through unshifted is correct per voice, but N voices emit N
*identical* copies which sum to one mono burst. The ensemble collapses to unison at every
sibilant and re-diverges when voicing resumes, heard as pumping and a width artefact.

**Measurement.** The fricative region reads 1.7x the dry energy with four voices in
`vh_demo` output; it is audibly the worst part of `wet_psola.wav`.

**Root cause.** Understood, not a mystery. `UnvoicedPolicy::PassThroughDecorrelated` is a
named placeholder with no implementation.

**Candidate fix.** Per-voice allpass, per-voice micro-delay, or mild per-voice spectral
warp on unvoiced frames. Real backing singers sibilate decorrelated. Untried.

---

### VH-006 — a test asserted the VH-001 defect as intended behaviour
**Status:** In review | **Severity:** S3 | **Area:** `tests/test_shifters.cpp` | **Owner:** —
**Found:** while diagnosing VH-001.

**Symptom.** `"psola survives a downward shift past half-pitch without gapping"` rendered at
ratio 0.45 and checked only that the output contained no long runs of near-silence. It never
checked the output **pitch**, and its comment named the `max(period, synthSpacing)` line as
the thing preventing holes — so the test actively defended the VH-001 root cause.

`HANDOVER.md` §6's own lesson recurring: *measure the thing, not a proxy for it.* Absence of
gaps was used as a proxy for a working downward shift, and the two came apart.

**Fixed alongside VH-001.** Renamed to `"psola actually LOWERS pitch past half-pitch, not
merely avoids gapping"`. The gap check is retained but loosened to one synthesis spacing
plus slack — gaps are now correct output, so the old 200-sample bound was itself asserting
the bug. The missing pitch assertion is added using the existing `measureF0`, which is YIN
and so picks the first dip below threshold rather than the global minimum; that is the
property stopping it from reporting a subharmonic, and the comment says so.

---

### VH-007 — the documented "PSOLA ends around ±6 semitones" limit is not what the code does
**Status:** Backlog | **Severity:** S3 | **Area:** `HANDOVER.md`, `ARCHITECTURE.md` | **Owner:** —
**Found:** measurement sweep across ±24 st.

**Symptom.** Both documents state the limit as a property of the technique. Measured, the
code does not behave that way in either direction:
- **Formant preservation does not break at ±6 st.** PSOLA's spectral centroid stays within
  ~5% of dry out to ±24 st, while granular's tracks the ratio as it must (0.29x at −24 st,
  2.07x at +24 st). The defining property holds far past the stated limit.
- **What actually limits the range is different in each direction** — level loss upward
  (VH-003) and VH-001 downward — and is asymmetric, which "±6" conceals. Downward is far
  more forgiving than upward once VH-001 is fixed.

**Why this is logged rather than just edited.** The claim is inherited from the literature
and is probably right *about TD-PSOLA in general*. Rewriting the docs before VH-001 and
VH-003 are fixed would record the limits of a buggy implementation as the limits of the
method. Re-measure after those land, then rewrite.

**Blocked by.** VH-001, VH-003. **VH-003 is now Done**, so this is unblocked. Milestone 2
measured the real limits and they are in `RESULTS.md`; what is still missing before the
docs can be rewritten is a listening pass, because two of the three limits (open quotient,
truncated ring) are timbre and not level.

**Partly answered already.** `VOICE-MODEL.md` restates the limit as three separate
mechanisms rather than one number — tail cancellation upward, open-quotient collapse
downward, ring truncation downward — and `psola_shifter.hpp`'s header comment has been
rewritten to say so. What remains is propagating that into `HANDOVER.md` and
`ARCHITECTURE.md` once the listening pass confirms it.

---

### VH-009 — upward shifts run hot enough to clip
**Status:** In review | **Severity:** S1 | **Area:** `core/src/psola_shifter.cpp`, `tools/wav.hpp` | **Owner:** —
**Found:** milestone 2 sweep. Three of 108 cells wrote files at full scale.

**Symptom.** Upward shifts at large intervals reach 0 dBFS. Heard as harsh, brittle
distortion on the loudest partials rather than as a level problem, which is why it would be
blamed on the shifter's timbre.

**Fixed, in two places.** The tilt shelf no longer boosts (see `RESULTS.md`), and
`writeWav` scales rather than hard-clamping, announcing the scalar. No cell in the sweep
clips now and the scaler never fires.

**NOT fixed, and not a defect.** PSOLA upward genuinely produces more energy: +6.4 dB at
+19 st on the 814 Hz probe with the profile DISABLED, so it is purely the VH-003 gain
correction, and it is physically correct — more glottal pulses per second is more energy
per second. `core` has no output stage by design.

**Where the remaining headroom belongs.** The mixer. `vh_render` already applies
`1/sqrt(n)` and an 0.9 trim; `vh_sweep` deliberately applies neither, because it is a
measurement tool. **The JUCE shell must apply headroom, and this entry exists so that work
inherits the requirement rather than rediscovering it at a soundcheck.**

**Ruled out.** Not the mu warp — cells with `muStrength = 0` clip too. Not the blend — a
single voice clips on its own.

---

### VH-010 — the C1 source-pitch prediction cannot be measured with the current metrics
**Status:** Backlog | **Severity:** S3 | **Area:** `tools/voice_probe.py` | **Owner:** —
**Found:** milestone 2, trying to test `VOICE-MODEL.md` C1.

**The prediction.** Downward quality is bound by SOURCE pitch, not by shift ratio, because
a grain carries only `2/F0_source` seconds of vocal tract ring against a tract that rings
for 5-15 ms. A soprano should fall apart where a bass is clean.

**Symptom.** No metric separates it. `tools/make_probe_tones.py` was written specifically
for this — 90, 242 and 814 Hz, identical in every other respect — and at -24 st the three
give duty 0.44 / 0.31 / 0.35 with cents error +2410 / -4 / +0, where the 90 Hz outlier is
the ESTIMATOR's floor (22 Hz output) and not the engine.

**Ruled out.** Not that the effect is absent — nothing here measures ring truncation.
- *duty* measures excitation shape, i.e. the open quotient, which is a different mechanism.
- *envelope distance* measures whether the envelope MOVED, which mu does deliberately.
- *envelope contrast* was added specifically to catch formant smearing and is confounded:
  it RISES with downward shift on all three sources, because a denser harmonic comb adds
  ripple to the liftered envelope and swamps the effect.

**Candidate fix.** A metric that isolates the decay envelope WITHIN one output period —
fit an exponential to the Hilbert envelope between excitation instants and compare its time
constant against the dry material's. A truncated ring should show a shorter time constant
that does not scale with the output period.

**Why this is logged rather than dropped.** A plausible prediction with no supporting
measurement is exactly the shape of the thing that gets cited as established fact three
documents later. `VOICE-MODEL.md` C1 is marked unverified; this entry is why.

---

# Done

*VH-003 and VH-008 landed with measurements and a listening test in `RESULTS.md`
milestone 2; their stubs are above. VH-001, VH-006 and VH-009 are fixed and in review — they move here once someone has
listened to the regenerated sweeps and confirmed the reintroduced inter-pulse gaps are
acceptable. Bugs fixed before this ledger existed are written up in `RESULTS.md` — seven of
them, each with a symptom that pointed away from its cause.*
