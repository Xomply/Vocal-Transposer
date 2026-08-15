# Real-Time Vocal Transposer — Design Notes

*A consolidated record of the research report and the design conversation that followed.*

**Project framing:** hobby research project, not academic. Learning DSP is a primary goal, but the target outcome is an *optimally working system*, not a toy. Priorities: formant preservation / natural timbre, large shift range (octaves), polyphonic harmony generation, with eventual interest in timbre manipulation. Original latency ambition: <10 ms for live self-monitoring — treated throughout as a constraint to confront, not to assume away.

---

## 0. The one-paragraph version

High-quality, formant-preserving pitch shifting has a latency floor set by physics (you must observe roughly one pitch period; 12.5 ms for an 80 Hz bass) and by the time–frequency uncertainty principle. Self-monitored singing has a latency ceiling around 5–10 ms, below which it feels natural and above which it disrupts. **These two ranges do not overlap.** The resolution is architectural rather than algorithmic: the singer monitors the *dry* voice for pitch reference, and the shifted/harmonized signal goes to a bus where 20–40 ms reads as ensemble looseness rather than broken feedback. Within that budget, a staged build — TD-PSOLA → phase vocoder → source-filter/WORLD — delivers the stated priorities. A speculative predictive stack (exogenous MIDI prior + contact sensing + waveform extrapolation) can push apparent latency lower on steady vowels, degrading gracefully rather than breaking.

---

## 1. The latency problem, stated honestly

### Three latencies that add

| Source | Typical | Notes |
|---|---|---|
| I/O (ADC + DAC + driver buffers) | 3–6 ms round trip | 64 samples @ 48 kHz = 1.33 ms per buffer; you pay for in and out |
| Block/buffer | = block size | Under your control |
| Algorithmic | 5–100+ ms | The lookahead the method fundamentally needs |

### The ceiling (perception)

- Pro-audio consensus: ≤5 ms for in-ear monitors, 5–10 ms tolerable on wedges. (ProSoundWeb, Shure, Church Production — engineering guidance, convergent but not peer-reviewed.)
- Peer-reviewed anchor: Lester & Boley, *The Effects of Latency on Live Sound Monitoring*, AES 123rd Convention (2007), paper 7198. Acceptable latency ranges ~1.4–42 ms depending on instrument and monitor type; **vocalists on IEMs are the most sensitive case**.
- Mechanism: the singer hears their own voice via bone conduction within a fraction of a millisecond. The delayed electronic copy sums with it, producing comb filtering. Per QSC's analysis, this is mild below ~2 ms, worst as delay approaches ~10 ms (notches move into the fundamental range), and separates into a distinct echo beyond ~35 ms.
- Wessel & Wright's often-cited bound for tight musical interaction: 10 ms latency, ±1 ms jitter.

### The floor (physics)

- One period of an 80 Hz fundamental is 12.5 ms. Robust estimation wants 2–3 periods → 25–40 ms.
- Gabor's uncertainty principle (Δt·Δf ≥ 1/4π) means frequency resolution is bought with window length. Resolving low fundamentals forces long windows.
- Real libraries confirm it rather than contradicting it: Rubber Band's `LiveShifter` documents a 50 ms+ floor; SoundTouch's README cites ~100 ms for time-stretching.

### Conclusion

**A full-quality, formant-preserving, octave-range shift of the singer's own voice returned to their ears in <10 ms does not exist in the public literature and is not achievable with current methods.** Even with free DSP, comb filtering against bone conduction would still degrade it. The design responses:

1. **Dry-path monitoring** — the singer monitors unshifted voice; shifted signal goes only to FOH/recording. Dissolves the problem entirely.
2. **Harmony-bus tolerance** — generated voices are perceived as *other singers*, so 20–40 ms reads as ensemble looseness. Only the lead's own returned voice is latency-critical, and you don't return it shifted.
3. **Blended IEM mix** — dominant dry, low wet.
4. **Accept bone conduction** — you cannot beat it; design so the electronic path never has to.

---

## 2. Technique inventory, with verdicts

### 2.1 Time-domain

**Variable-speed resampling.** Trivial, near-zero latency. Moves formants with pitch (chipmunk effect). *Not usable alone; correct as the resampling stage after time-scale modification, and as a teaching baseline.*

**Delay-line / rotating-tape-head crossfade (classic hardware harmonizer).** Read pointer traverses a delay line at ratio r; two taps crossfaded on a sawtooth ramp to hide the jump. Eventide H910/H949 lineage. Low latency (few–20 ms), cheap, no pitch detection required. Suffers comb coloration and glitch (see §3.2 for the fixes). Resamples, so no formant preservation. *Viable with caveats — the genuine low-latency option, but always sounds "effected."*

**OLA / SOLA / WSOLA.** Overlap-add time-scale modification with cross-correlation alignment. Cheap, no phasiness. Smears/duplicates transients; needs ~100 ms windows for clean low-voice results; no formant preservation. *Good stepping-stone; not a vocal solution alone.*

**TD-PSOLA.** Detect pitch epochs (glottal closure instants), window ~two-period grains, overlap-add at new spacing. **Formants preserved for free** because you change grain emission rate, not grain spectrum. Latency ~1–2 pitch periods. Excellent to ~±6 semitones, degrades beyond. Cheap. Multiple grain streams → cheap harmony. Sensitive to epoch errors (glitches, not graceful degradation); undefined on unvoiced. *Viable and recommended as the first serious build.*

**Granular synthesis generally.** PSOLA is the pitch-synchronous special case. Free-form granular is inherently low-latency and excellent for texture/timbre work, but not a precise transposer.

### 2.2 Frequency-domain

**Phase vocoder.** STFT; estimate instantaneous frequency from phase differences (subtract expected bin advance, wrap to (−π,π], divide by hop); resynthesize at a different hop with accumulated phase; resample to convert time-scale to pitch-scale. Perfect reconstruction requires COLA / Princen–Bradley window conditions.

- *Artifacts:* preserves horizontal (per-bin) phase coherence but loses vertical (within-partial) coherence → **phasiness**; plus transient smearing.
- *Fixes:* identity/scaled phase locking (Laroche & Dolson, JAES 47(11), 1999) — lock bins in a peak's region of influence to the peak's phase; transient detection + phase reset; **PGHI/RTPGHI** (Průša & Holighaus, *Phase Vocoder Done Right*, EUSIPCO 2017), which integrates the 2-D phase gradient in magnitude-prioritized order and needs neither peak picking nor transient detection.
- *Formants:* not preserved by default — requires cepstral true-envelope correction bolted on.
- *Verdict:* the right vehicle for octaves and polyphony; latency rules it out for the monitored path.

**Constant-Q / wavelets / sliding DFT.** Log-frequency bins are musically natural; sliding DFT reduces block latency. *Advanced; worth study, not the first build.*

### 2.3 Source-filter / parametric

**Channel vocoder (Dudley).** Imposes voice band-energies on a carrier. This is *cross-synthesis* — the robot-voice effect — not transposition. *Instructive, not a solution.*

**LPC.** Predict x[n] from p past samples; the all-pole filter 1/A(z) models the vocal tract (poles = formants), residual = excitation. Solve via autocorrelation + Levinson–Durbin. Pitch-shift the residual, re-filter with the original A(z) → pitch changes, formants stay. Warp A(z) independently → formants change, pitch stays. Frame-based latency (~10–30 ms). Can sound buzzy without care. Cheap polyphony: excite one filter with N residuals. *Viable and strongly recommended — makes source-filter separation tangible.*

**Cepstral analysis / true envelope.** log|X(k)| → IDFT → cepstrum; low quefrencies = spectral envelope, high = harmonic fine structure. Liftering gives the envelope; Röbel & Rodet's true-envelope algorithm (DAFx 2005) iterates it to hug harmonic peaks. **This is the standard formant-preservation recipe:** divide out the envelope, shift, multiply back.

**WORLD (Morise et al., IEICE 2016).** Decomposes voice into F0 (DIO/Harvest) + spectral envelope (CheapTrick) + band aperiodicity (D4C). BSD-licensed, patent-free, with a published real-time sequential generator (APSIPA 2020). Independent, natural control of all three parameters; trivial polyphony; envelope warping gives the gender/character knob. *The highest-quality classical route to all three stated priorities at once. Capstone of the classical stack.* (STRAIGHT is higher quality, heavier, less real-time-friendly.)

**Sinusoidal / SMS / HNM.** McAulay–Quatieri sinusoidal modelling (IEEE TASSP 1986) with birth/death tracking and cubic phase interpolation; Serra & Smith's SMS (CMJ 1990) adds a stochastic residual so breath noise isn't tonalized. Rich playground for timbre morphing. More complex, higher latency. *Viable with caveats.*

**Glottal source modelling (LF model, inverse filtering).** The glottal pulse *shape*, not just its rate, carries naturalness. *Advanced polish; not a first build.*

### 2.4 Pitch detection

- **Autocorrelation / AMDF** — simple, octave-error-prone.
- **YIN** (de Cheveigné & Kawahara, JASA 2002) — difference function with cumulative mean normalization, absolute threshold, parabolic interpolation. Low latency, robust, no upper frequency limit. **The right default.**
- **pYIN** (Mauch & Dixon, ICASSP 2014) — probabilistic + HMM smoothing; more robust, slightly more latency.
- **CREPE** (Kim et al., ICASSP 2018) — CNN, very accurate, heavier.
- **The floor applies here too:** detecting an 80 Hz fundamental costs ~12.5 ms of observation before you shift anything.

### 2.5 Neural — honest assessment

| Method | Verdict |
|---|---|
| WaveNet / WaveRNN | **Not usable.** Autoregressive; far too slow and latent for causal real-time. |
| LPCNet / CLPCNet | Viable with caveats; built on LPC, so it rewards the DSP you'll already know. |
| HiFi-GAN | Fast on GPU, but mel→wave; needs a front end; streaming latency non-trivial. |
| DDSP (Engel et al., ICLR 2020) | Viable with caveats; literally SMS made differentiable. Pedagogically ideal bridge. |
| RAVE (Caillon & Esling, 2021) | ~20–80× real time on CPU, but with buffering latency. |
| **BRAVE** (Caspe et al., JAES 73(5), 2025) | **The only credible <10 ms neural option** — <10 ms latency, ~3 ms jitter, via removing RAVE's noise generator, smaller encoder compression, PQMF attenuation, causal training. |

*Overall: legitimate as a phase 4. For a DSP-learning project, come to it after the classical stack, because DDSP and LPCNet only make sense once you know the DSP they differentiate.*

---

## 3. New ideas from the discussion

### 3.1 Beating the latency floor with side information

**The key distinction — three latencies, not one:**

| Kind | What it is | Killed by a prior? |
|---|---|---|
| Estimation | Observing enough signal to measure F0 | **Yes, outright** |
| Representation | Window needed to resolve harmonics | **Partly** |
| Reconstruction | Samples the resynthesis physically needs | **No** |

The non-obvious part is representation latency. **The Gabor limit governs *blind* time–frequency resolution; it does not bound *parametric* estimation with a correct model.** If you know harmonics sit at k·F0, you don't need to resolve peaks — a bank of heterodyne demodulators or resonators locked to k·F0 reads amplitude and phase from a much shorter window. "Where are the partials?" becomes "what are the amplitudes of partials I already know about," a far lower-variance problem. Same reason ESPRIT/MUSIC beat the Rayleigh limit. Adaptive comb filtering locked to a known F0 is the practical version.

**Three gotchas, one fatal:**

1. **Pitch-to-MIDI from the voice is circular.** A vocal pitch tracker has exactly the latency you're escaping. The prior must be *exogenous*: keyboard doubling the line, guitar chord input (what TC-Helicon and DigiTech actually do), a sequencer, or score-following.
2. **The prior will be wrong.** Treat it as a Bayesian prior that shrinks the search interval, not as ground truth.
3. **F0 ≠ epoch phase.** MIDI gives the rate of glottal pulses, not when they occur. Pitch-synchronous methods need the phase.

**Contact sensing.** Electroglottography gives glottal closure instants directly and — because the acoustic wave still has to travel up the tract and across the air gap — **the EGG signal precedes the microphone signal.** Free negative latency. Lab-priced and ergonomically awkward (electrodes, gel, movement artifacts). *In-scope substitutes that keep the useful property:* a **throat mic** (~$20–50) or a **neck accelerometer** over the suprasternal notch (as used in ambulatory voice monitoring). Neither gives clean GCI, but both are dominated by glottal excitation with less tract filtering, and both still arrive early. Worth a $30 experiment before dismissing.

**Waveform extrapolation.** A steady vowel is quasi-periodic, therefore predictable. Packet-loss concealment (G.711 Appendix I, Opus PLC) routinely synthesizes 10–30 ms of plausible speech by repeating and cross-fading pitch periods. Run it forward: feed a high-latency shifter *extrapolated* samples so its output lands time-aligned with the present. You trade latency for prediction error. Holds on sustained vowels; collapses at onsets, plosives, sibilants — which is where forward masking is strongest and pitch judgment weakest.

**The prediction residual as control signal.** Large innovation means "something unmodeled is happening": widen the prior, switch modes, duck the wet signal. You get an onset and anomaly detector free from machinery you already built.

**The safety property that makes the whole scheme worth building:** the extrapolation horizon should be a *continuous function of predicted error*, not a mode flag. Extrapolate as far as predicted deviation stays under a perceptual threshold (~10–20 cents in musical context), no further. Erratic input → residual grows → horizon collapses toward zero → you degrade smoothly to baseline latency. **Uncertainty costs latency, not correctness.**

Equally: a prior fails gracefully where a constraint does not. If MIDI is ground truth, a wrong note is catastrophic. If MIDI is a prior weighted against acoustic likelihood, a wrong prior just means slower, noisier convergence — you degrade *toward the blind estimator*, i.e. toward where you'd have been anyway. **The prior can only help, provided acoustic evidence always has the final say.** Make its variance adaptive.

**What the prior buys that's easy to undervalue: octave errors become structurally impossible.** That's the catastrophic failure mode of every blind tracker — not a slight detune but a harmony a full octave adrift. Constraining the search to ±X cents around a known note eliminates the entire class. That alone may justify the machinery.

### 3.2 Fixing the delay-line crossfade

**Diagnosis.** Two taps read at the same rate, offset by half the ramp, so their delay difference Δ is **constant**. Summing a signal with a constant-delay copy is by definition a comb filter: notches every 1/Δ Hz. Δ = 10 ms puts notches every 100 Hz, straight through the vowel range. Static and pitched, so the ear attributes it to a filter — hence "metallic tube."

**The biggest win is not tap count or jitter — it's making Δ pitch-synchronous.** Constrain jump distance to an integer number of pitch periods, Δ = k·T₀, and the taps read phase-aligned copies. They sum constructively across the spectrum. The comb doesn't get smeared; **it stops existing.** This is exactly the "synchronized" in SOLA and the "pitch-synchronous" in PSOLA. A delay-line shifter is just PSOLA with an arbitrary jump instead of a pitch-locked one. Which requires knowing T₀ and its phase — so §3.1 and §3.2 are the same problem.

**Multi-tap / Shepard-style (the idea, assessed).** Right instinct, real mechanism: N taps give pairwise differences Δ, 2Δ, 3Δ…, so one deep comb becomes several shallower ones whose notches partially fill each other in. Diffusion, as in multi-tap reverb. Cost: more taps sound at once, trading pitched coloration for temporal smearing and chorused blur — usually a good trade on voice, since the ear forgives blur more readily than resonance.

The deeper lesson from Shepard tones is different, though. That illusion works because partials are born and die under a bell-shaped envelope at amplitudes where the transition is inaudible. The structural analogy is exact — each tap is continuously Doppler-shifted, and the only discontinuity is *which tap you're listening to*. **So the payoff is in window shape, not tap count:** make the crossfade envelope have zero slope at birth and death (raised-cosine or better, C¹-continuous) and the glitch disappears without shortening the fade.

**Jittered / modulated ramp period (the idea, assessed).** Also right, for a perceptual reason worth naming: a static comb is heard as *timbre* (property of the filter); a time-varying comb is heard as *movement* (property of the source) — that's chorus and flanging, which the ear finds far less objectionable. Randomizing Δ converts one into the other.

> **Gotcha, easy to trip over:** do **not** jitter the read *rate*. The read pointer's rate *is* the pitch ratio, so modulating it FMs the output — uncontrolled vibrato and detuning on top of your transposition. Jitter *when you jump* and *which tap you fade to*, holding every tap's rate at exactly r. Keep modulation slow, shallow, band-limited (1/f-ish, not white), or you get warble instead of chorus.

**Two more cheap wins:**
- **Crossfade law.** Equal-power (sin/cos) fades assume uncorrelated sources; on correlated ones they give a +3 dB mid-fade bump — amplitude pumping at the ramp rate, a chunk of the perceived warble in naive implementations. Once pitch-synchronous, the taps *are* correlated: use equal-gain (linear, summing to 1). Adapting the law to measured inter-tap correlation is a few lines.
- **Allpass decorrelation on alternate taps.** Frequency-dependent delay scrambles phase so taps sum in power rather than combing. Costs some transient definition. Borrowed from stereo wideners.

**Mode switching.** Unvoiced sounds have no T₀, so pitch-synchronous alignment is undefined — and noise is where comb coloration is *most* audible. Correct structure: pitch-synchronous jumps on voiced frames, randomized/decorrelated jumps on unvoiced, driven by a voicing detector. **The jitter idea isn't a competitor to the pitch-synchronous fix; it's the correct fallback for exactly the case where the fix doesn't apply.**

*Reminder: every variant here still resamples, so it still drags formants along with pitch. The comb problem and the chipmunk problem are independent; you want envelope correction on the output regardless.*

### 3.3 Where the predictive ideas fail — the honest accounting

**Vibrato: mostly doesn't pay off, for a timescale reason.** Vibrato at 5–7 Hz has a cycle of 140–200 ms. Estimating its rate and phase means analyzing the F0 *trajectory as a signal*, needing two or three cycles: **300–600 ms of history before lock.** You'd be trying to save 10 ms using an estimator with half a second of lag. On anything but a long held note, the tracker converges as the note ends. (The machinery exists — adaptive frequency oscillators, Righetti & Ijspeert's Hopf-oscillator work, an EKF with a resonator state model. The problem isn't a missing method; it's that convergence exceeds the useful window.)

*The reframe that dissolves it:* **for harmony voices, don't predict vibrato — synthesize it.** Real backing singers don't lock vibrato phase with the lead; independent, decorrelated vibrato is precisely what makes voices sound like *people* rather than one flanged voice. It's the humanization parameter every commercial harmonizer ships. A harmony voice's vibrato needs to be *plausible*, not *accurate*. Rate and depth converge far faster than phase, and phase you can randomize — beneficially. **The hard part is only hard for the case where you'd want it least.** For a shifted lead returned to the singer it does matter, and there the honest answer is: it doesn't work, and you fall back.

**Glissando: a rate problem, not a value problem.** Prior on (F0, dF0/dt), not F0 alone. A constant-velocity state model — a small Kalman filter — handles portamento natively. During a glide the prior is wrong about *now* but correct about the *destination*. Still information.

**Sibilants: a category error that fixes a bug you'd have anyway.** /s/, /ʃ/, /f/ have no F0 — the prior isn't wrong, it's inapplicable. More importantly, **fricatives shouldn't be pitch-shifted at all.** Their identity is broadband noise shaped by front-cavity geometry — tongue and teeth — not larynx rate. A real singer's /s/ doesn't transpose when they sing a fifth higher. Correct handling is voicing-detect and pass through (optionally with mild spectral warp). The worst case becomes a non-case.

**Onsets: the region that genuinely stays hard.** Note entries, plosive releases, the first 20–50 ms of a phrase. The prior may be *mistimed*; extrapolation has nothing to extrapolate from; F0 is unstable; the ear is at its most timing-sensitive. Partly rescued by forward masking, but this is where artifacts will live.

**On the MIDI-fuzziness proposal.** Correct, with a distinction that matters: **sequenced MIDI gives genuine future knowledge** (you know the whole song); **a live accompanist gives none** (they play simultaneously); **score-following gives lookahead with tempo uncertainty.** Decide which you're targeting — the sequenced case is dramatically more powerful.

The distinction that survives even in the best case: **knowing the target pitch is not the same as having the samples.** At a note onset the binding constraint isn't "which pitch?" but that you have less than one period of audio, so a grain-based shifter has no material. MIDI lookahead solves the parameter problem completely and the material problem not at all. Partial mitigation: warm-start from the previous frame's spectral envelope, since vowel identity and formants usually persist across a legato note change even as F0 jumps.

The fuzziness has a precise form: **during an expected transition the prior should go bimodal** — mass on both old and new pitch, weighted by elapsed time against expected onset — collapsing onto one when acoustic evidence arrives. And it gates the anomaly detector: near an expected transition, a large residual is *predicted*, not anomalous. That kills the false alarms.

**One caveat that cuts against snapping to a MIDI target.** Singers scoop into notes, back-phrase, and bend — that's expression, not error. Setting the shifted voice's pitch from MIDI erases it. Harmonizers shift by *interval* for exactly this reason: the lead's deviations carry through into the harmony, which is what makes it sound sung rather than sequenced.

**Which lands somewhere slightly deflating, and worth keeping in view:** MIDI tells you which harmony note to pick; it doesn't excuse you from measuring the lead. The latency win comes from making the measurement faster and better-conditioned — narrower search, no octave errors — not from replacing it.

---

## 4. Musical principles

**Interval math.** Equal temperament: n semitones = ratio **2^(n/12)**; one cent = 2^(1/1200). An octave is exactly ×2 — the "easiest" shift, harmonics align, clean resample factor. A minor third is 2^(3/12) = 1.1892…, irrational, so periods never line up: non-octave shifts stress every algorithm more. Just intonation uses small-integer ratios (fifth 3/2, major third 5/4) that beat less; TC-Helicon exposes Equal / Just / Barbershop modes, with Just tuned to the chord root and Barbershop to the input notes.

**Choosing harmony notes.**
- *Fixed-interval (chromatic)* — musically dumb, wrong notes over changing chords; useful mainly for fourths, fifths, octaves.
- *Scale/diatonic* — set key + scale, snap each voice to the nearest scale tone at the chosen diatonic interval (a "third" is sometimes major, sometimes minor, tracking the scale).
- *Chordal* — internal chord recognition or MIDI/guitar input picks root/third/fifth/seventh and assigns the closest chord tone. This is how singer-plus-guitar boxes "just work."

**Voice leading.** Prefer contrary/oblique motion; avoid parallel fifths and octaves between generated voices; keep voices in comfortable range; choose voicings minimizing leaps. A smoothing parameter controls how much of the lead's pitch fluctuation is imposed on the harmony.

**Vocal acoustics — why a naive octave shift fails.** F0 and formants are governed by different physiology and do not scale together. Female→male: F0 drops ~50%, formants only ~15%, because the difference is vocal tract length. Formant spacing ΔF = c/2·VTL; a 17.5 cm male tract puts formants near 500, 1500, 2500 Hz. Children's formants sit ~40% above adult male; adult female ~15% above (Sundberg). For a convincing octave-down, shift F0 by 2 but warp the spectral envelope by only μ ≈ 0.75–1.0 (per US Patent 6,304,846), which also slightly narrows formant bandwidths and mitigates the buzzy character of pitch-lowered sound. **This asymmetry is the crux of naturalness.** Register/passaggio behavior and the singer's-formant cluster (F3–F5) add further realism that no single scaling law captures.

**Humanization.** Identical shifted copies sound like a flanged single voice. Add per-voice static + drifting detune (a few cents), timing jitter, independent vibrato (differing rate/depth/phase), and slight per-voice formant/gender offset. This is *the* difference between "chorus of robots" and "backing vocalists."

---

## 5. Stack matrix

Add ~3–6 ms of I/O to every algorithmic figure. Numbers are for adult voice; low male fundamentals sit at the pessimistic end.

| | Stack | Algorithmic latency | Formants | Range | Polyphony | CPU | Best for |
|---|---|---|---|---|---|---|---|
| **A** | Delay-line / granular crossfade | **5–20 ms** | ✗ (resamples) | Any, but ugly at extremes | Cheap, near-N× | Very low | Stompbox, embedded, octave/fifth doubling, deliberate effect |
| **B** | TD-PSOLA, epoch-locked | **15–30 ms** | ✓ free | ±6 st | Cheap, shared analysis | Low | First serious build; low-latency naturalness |
| **C** | Phase vocoder + phase locking + cepstral formants | **40–100 ms** | ✓ (bolted on) | Octaves ✓ | Shared analysis | Moderate | Audience/recording bus, large shifts |
| **D** | Source-filter (LPC → WORLD) | **30–60 ms** | ✓✓ independent | Octaves ✓ | Very cheap (one filter, N excitations) | Moderate | All three priorities at once + timbre control |
| **E** | Predictive / feed-forward | **<10 ms apparent → degrades to B** | ✓ | ±6 st | Cheap | Low–moderate | Research branch; requires extra hardware + exogenous MIDI |
| **F** | Neural (RAVE/DDSP; BRAVE) | 20–50 ms; BRAVE <10 ms | ✓ learned | Learned | Model-dependent | High | Timbre morphing, voice conversion |

### Per-stack notes

**A** — no pitch detection needed, so nothing to get wrong; never sounds transparent. Apply the §3.2 fixes.

**B** — the sweet spot for learning. Epoch errors are audible as *glitches* rather than graceful degradation. Will **not** give you the octave range you asked for. Requires a voicing detector and passthrough path.

**C** — latency is structural, not an implementation flaw. Transient smearing needs explicit handling.

**D** — most machinery to build and understand; the only stack that gives the *timbre* knob (envelope warp = gender/character/vocal-tract-length control, including the μ≈0.8 warp for a believable bass).

**E** — the honest accounting: the prior fixes *parameter* latency, not *material* latency. Vibrato prediction mostly doesn't pay off. Onsets stay hard. Needs extra hardware and a MIDI source not derived from the voice. What redeems it is the failure mode — uncertainty costs latency, not correctness; it degrades toward B rather than breaking.

**F** — highest ceiling for timbre morphing; costs training infrastructure, hard to debug, and teaches less DSP per unit effort, which matters given the goal.

---

## 6. Use-case requirements matrix

| Use case | Latency need | Formants | Range | Polyphony | Recommended stack |
|---|---|---|---|---|---|
| Singer monitors own shifted voice | **<10 ms — unachievable at quality** | — | — | — | **Don't.** Dry-path monitoring instead |
| Live harmony to FOH, singer monitors dry | 20–40 ms fine | Critical | Octaves | 2–4 voices | **D**, or **C** |
| Studio / recorded harmony | 100 ms+ fine | Critical | Octaves | 3–6 voices | **D** (or offline C) |
| Stompbox octave/fifth doubler | 5–20 ms | Nice-to-have | ±12 st | 1–2 | **A** (with §3.2 fixes) |
| Embedded / battery / MCU | 5–20 ms | Nice-to-have | Modest | 1–2 | **A**, or **B** if CPU allows |
| Gender/character transform | Not latency-bound | **The whole point** | Small F0 change | 1 | **D** |
| Learning DSP well | n/a | n/a | n/a | n/a | **B → C → D** |
| Research / novel instrument | Aspirationally <10 ms | ✓ | ±6 st | Cheap | **E** on top of **B** |

---

## 7. Cross-cutting design rules

1. **Hybrid latency is max, not min.** Paths with different delays must be padded to a common alignment or you get a time-jump at every switch — quietly eating the low-latency win. *Escape:* permit jumps only where they're masked — onsets and unvoiced frames — which is exactly where you're switching anyway. Turn the constraint into the switching policy.
2. **Switching artifacts can be worse than either mode's steady-state flaws.** Hysteresis on the voicing detector; crossfades not hard switches; better still, run paths in parallel and blend continuously.
3. **Don't pitch-shift unvoiced audio.** Fricatives are shaped by front-cavity geometry, not larynx rate. Passthrough is correct, not a compromise.
   - *Open issue — polyphonic collapse on unvoiced frames.* Passthrough is correct per voice, but if all N harmony voices pass the same fricative through unmodified, they become N identical copies of the lead's /s/ summing to a single mono burst — then re-diverge when voicing resumes. The ensemble momentarily collapses to unison and reappears, which is audible as a pumping/width artifact at every sibilant. Real backing singers still sibilate, decorrelated. Noted, not solved.
4. **Shift by interval, not to a target pitch.** Otherwise you erase scoops, bends, and back-phrasing — the things that make it sound sung.
5. **Acoustic evidence always overrides the prior.** Adaptive prior variance: widen on large residual, narrow on small.
6. **Extrapolation horizon is continuous, not a mode.** Uncertainty must cost latency, not correctness.
7. **Self-monitoring is the constraint that doesn't bend.** Dry path + harmony bus at 20–40 ms is the architecture that actually works, because harmony voices are heard as *other singers*.
   - *Open issue — monitor bleed breaks the dry-path assumption.* The architecture assumes the singer hears only the dry path. That holds for sealed IEMs and nothing else. On wedges, with one ear out, or with any FOH spill, the singer hears the 20–40 ms harmony bus too — and that bus contains a pitch-shifted derivative of their own voice, arriving in exactly the window §1 identifies as worst for comb interaction with bone conduction. How much this actually degrades things is unmeasured; the shifted copies are at least spectrally displaced from the dry voice, which may soften it. Noted, not solved.

### Failure-mode complementarity

| Signal state | Path | Why |
|---|---|---|
| Steady voiced | Pitch-synchronous PSOLA, epoch-locked, forward-extrapolated | Near-zero apparent latency, formants free |
| Vibrato | Same; synthesize plausible vibrato on harmony voices | Prediction too slow to converge; plausibility suffices |
| Glissando | Same, velocity state in tracker | PSOLA re-spaces grains natively as period changes |
| Unvoiced | Passthrough + optional envelope warp | Shifting is the wrong operation — but see the polyphonic-collapse caveat under rule 3 |
| Large shift (octaves) | Source-filter / WORLD on harmony bus | PSOLA degrades past ~±6 st |
| Onset / high residual | Duck wet, favor dry | A late harmony reads as a human singer; a glitchy one reads as broken gear |

PSOLA fails at large shifts where source-filter is strong; the phase vocoder fails at transients where time-domain is strong; both fail on unvoiced where passthrough is correct; blind tracking fails via octave errors the prior prevents; the prior fails when the singer deviates, which the likelihood overrides; extrapolation fails at onsets, which the residual detects.

---

## 8. Build path

1. **Foundations.** COLA-compliant STFT with verified perfect reconstruction (no modification). Variable-speed resampling, to hear the chipmunk effect first-hand. YIN, validated on recorded voice. *Benchmark: bit-accurate round-trip; YIN within a few cents on sustained vowels.*
2. **TD-PSOLA.** Epoch detection → grain windowing → re-spaced overlap-add, plus a voicing detector and passthrough. *Benchmark: clean ±5 st on sustained vowels; measure algorithmic latency in periods.*
3. **Phase vocoder.** Bernsee-style baseline → identity/scaled phase locking → transient reset → cepstral true-envelope formant correction → RTPGHI, A/B'd. *Benchmark: octave up/down with formants held; measurably reduced phasiness.*
4. **Source-filter.** LPC residual-shift demo (Levinson–Durbin), then integrate WORLD for independent F0/envelope/aperiodicity and N-voice harmony with per-voice envelope warp. *Benchmark: octave-down that reads as a real bass at μ≈0.8, not slowed tape.*
5. **Musical layer.** Scale/chord quantization, voice-leading rules, per-voice detune/timing/vibrato humanization. *Benchmark: 3-part harmony heard as three singers.*
6. **Research branch (optional).** Throat-mic experiment; exogenous MIDI prior with adaptive variance; PLC-style extrapolation with residual-driven horizon.
7. **Neural (optional).** DDSP timbre transfer; BRAVE if pushing low-latency neural.

**Engineering practicalities.** Audio-callback processing (CoreAudio/JACK/ASIO/WASAPI); **no allocation, no locks, no IO in the audio thread**; lock-free SPSC ring buffers to the UI/analysis threads; smallest stable buffer (64–128 samples). FFT: KissFFT (simple, BSD) for learning, pffft or vDSP for speed. Build in JUCE (plugin/app) or Bela (ultra-low-latency embedded; its C++ course includes a three-part phase vocoder tutorial). Prototype in Pure Data/Max or Faust.

**Evaluation.** Objective: log-spectral distance, formant tracking error, F0 accuracy, PESQ/PEAQ. Subjective: MUSHRA with hidden reference and anchors. Always test across the full F0 range and on transient-heavy consonants.

---

## 9. Reading list

**Textbooks** — Zölzer (ed.), *DAFX: Digital Audio Effects* (TSM/pitch chapters); Julius O. Smith, *Spectral Audio Signal Processing* and *Mathematics of the DFT* (free, CCRMA); Rabiner & Schafer, *Digital Processing of Speech Signals*; Roads, *Microsound*; Sundberg, *The Science of the Singing Voice*.

**Phase vocoder** — Flanagan & Golden (1966); Portnoff (1976/78); Dolson, "The Phase Vocoder: A Tutorial" (CMJ 1986); Laroche & Dolson, "Phase-vocoder: about this phasiness business" (WASPAA 1997) and "New Phase-Vocoder Techniques for Real-Time Pitch Shifting" (JAES 47(11), 1999); Puckette, "Phase-locked vocoder" (1995); Průša & Holighaus, "Phase Vocoder Done Right" (EUSIPCO 2017); Bernsee's "Pitch Shifting Using the Fourier Transform".

**PSOLA / TSM** — Moulines & Charpentier (PSOLA); Moulines & Laroche, "Non-parametric techniques for pitch-scale and time-scale modification of speech" (Speech Comm. 1995); Verhelst & Roelands (WSOLA); Juillerat et al., "Low latency audio pitch shifting in the frequency domain" (ICALIP 2010).

**Source-filter / vocoders** — Makhoul, "Linear Prediction: A Tutorial Review" (Proc. IEEE 1975); Röbel & Rodet, "Efficient spectral envelope estimation… true envelope" (DAFx 2005); Kawahara (STRAIGHT); Morise et al., WORLD (IEICE 2016), CheapTrick (Speech Comm. 2015), D4C (Speech Comm. 2016), real-time generator (APSIPA 2020).

**Sinusoidal / SMS** — McAulay & Quatieri (IEEE TASSP 1986); Quatieri & McAulay, "Shape-Invariant Time-Scale and Pitch Modifications" (1992); Serra & Smith, "Spectral Modeling Synthesis" (CMJ 1990); Stylianou (HNM).

**Pitch & epoch detection** — de Cheveigné & Kawahara, "YIN" (JASA 2002); Mauch & Dixon, "pYIN" (ICASSP 2014); Kim et al., "CREPE" (ICASSP 2018); DYPSA and SEDREAMS for GCI detection (EGG-validated).

**Neural** — Engel et al., "DDSP" (ICLR 2020); Caillon & Esling, "RAVE" (arXiv:2111.05011); Caspe, Shier, Sandler, Saitis & McPherson, "Designing Neural Synthesizers for Low-Latency Interaction" / BRAVE (JAES 73(5), 2025; arXiv:2503.11562); Valin & Skoglund, "LPCNet"; CLPCNet (arXiv:2110.02360).

**Perception / latency** — Gabor, "Theory of communication" (1946); Lester & Boley, "The Effects of Latency on Live Sound Monitoring" (AES 2007, paper 7198); Wessel & Wright on interaction bounds; delayed-auditory-feedback literature.

**Prediction / concealment** — ITU-T G.711 Appendix I (packet loss concealment); Opus PLC; Righetti & Ijspeert on adaptive frequency oscillators.

**Code** — mmorise/World; jurihock/stftPitchShift; breakfastquay/rubberband; Signalsmith-Audio/signalsmith-stretch; MTG/sms-tools; sannawag/TD-PSOLA; PieterPenninckx/tdpsola; acids-ircam/RAVE; magenta/ddsp; smbPitchShift (Bernsee, as a teaching baseline with known limits); Bela phase vocoder tutorial.

---

## 10. Open caveats

- The <10 ms self-monitored, formant-preserving, large-range shifter does not exist in the public literature. Every datapoint — period-observation floor, Gabor limit, Rubber Band's 50 ms+, SoundTouch's ~100 ms — points the same way. The dry-monitoring recommendation follows from that, not from lack of effort.
- Pro-audio latency figures (5/10 ms rules, comb thresholds) are convergent engineering guidance rather than peer-reviewed; treat exact numbers as rules of thumb. The peer-reviewed anchor reports a wide instrument-dependent range (~1.4–42 ms).
- Library latency figures are configuration-dependent. Signalsmith Stretch publishes split input/output latency rather than one number.
- Neural figures (RAVE ~20–80× real time; BRAVE <10 ms) are author-reported. Validate on your target hardware before trusting them.
- The μ ≈ 0.75–1.0 envelope warp and vocal-tract/formant relations are well-established but approximate; real register and passaggio behavior exceeds any single scaling law.
- Two consequences of the §7 rules are recorded there but unresolved: unvoiced passthrough collapses N harmony voices to mono at every fricative (rule 3), and the dry-path monitoring architecture assumes zero monitor bleed, which only holds for sealed IEMs (rule 7).
- Stack E is speculative. Its components are individually well-established (Bayesian priors, contact sensing, PLC extrapolation, pitch-synchronous OLA); the assembly is not something I found described as a whole, which is either an opportunity or a warning.
