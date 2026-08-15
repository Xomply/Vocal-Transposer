# Latency Budget — MIDI Vocal Harmonizer

*Companion to `midi-vocal-harmonizer-plan.md`. Decomposes where every millisecond goes,
which contributions add and which don't.*

**Reference conditions (all figures below assume these):** 48 kHz, 64-sample buffer,
F0 = 110 Hz (A2, low male — period **9.09 ms**). Scale pitch-dependent rows by 110/F0:
at 220 Hz they halve, at 80 Hz they multiply by 1.375.

**Every number here is PROVISIONAL until Phase 0 measures your machine.** Converter and
driver figures are class-typical for Focusrite-tier hardware, not measured from your 2i4.

---

## 1. The three rules that make this readable

1. **Serial = adds.** Sequential stages in one signal path sum.
2. **Parallel = max, not sum.** Two engines processing the same input concurrently cost
   `max(a, b)` in latency, not `a + b`. They cost `a + b` in *CPU*. Different budget.
3. **Onset ≠ steady state.** Estimation latency is a *convergence* cost paid once when
   the tracker locks, not a tax on every buffer. This distinction is the single most
   important thing in this document and the one most often got wrong.

---

## 2. The serial spine — paid by every path, always

| # | Stage | System | ms | Serial/Parallel | Notes |
|---|---|---|---|---|---|
| 1 | Acoustic path, mouth→capsule | Air | ~0.1 | Serial | 3 cm. Ignorable. |
| 2 | Preamp + anti-alias + ADC | Scarlett 2i4 | ~0.9 | Serial | Converter group delay |
| 3 | Input buffer fill | ASIO driver | 1.33 | Serial | = buffer size. Your main dial. |
| 4 | Callback dispatch / scheduling | Windows + ASIO | 1.33 | Serial | Typically one buffer of slack |
| — | **— DSP happens here (§3) —** | | | | |
| 5 | Output buffer drain | ASIO driver | 1.33 | Serial | = buffer size |
| 6 | DAC + reconstruction filter | Scarlett 2i4 | ~0.9 | Serial | |
| 7 | Monitoring path | Headphones / wedge | 0 / 3 per m | Serial | Sealed IEM ≈ 0; wedge at 2 m ≈ 5.8 |
| | **Spine subtotal (headphones)** | | **~5.8** | | Irreducible without a smaller buffer |

At 128 samples the spine becomes ~8.5 ms; at 256, ~13.8 ms. **This is the floor your DSP
budget sits on top of.**

---

## 3. DSP stages — what each function costs

| Function | System | Steady-state ms | Onset/acquisition ms | Serial w.r.t. | Required by |
|---|---|---|---|---|---|
| MIDI note → ratio | MIDI in + allocator | **~1** | ~1 | **Parallel to audio** | Both modes |
| F0 estimation | YIN | **~0** (locked, incremental) | **18–27** (2–3 periods) | Before ratio | **Mode A only** |
| Voicing detection | YIN aperiodicity / ZCR | ~0 | 5–10 | Parallel to F0 | Unvoiced path |
| Epoch (GCI) detection | Peak-picking / SEDREAMS | 9.1 (1 period) | +9.1 | After F0 | PSOLA paths |
| Grain windowing + OLA | PSOLA | 9.1–18.2 (1–2 periods) | same | After epochs | Fast path (PSOLA) |
| Delay-line crossfade | Granular shifter | 5–15 (= ramp length) | same | Nothing upstream | Fast path (cheap) |
| STFT analysis window | Phase vocoder | **42.7** (2048 samples) | same | After nothing | Quality path (PV) |
| True-envelope correction | Cepstral liftering | ~0 (within STFT frame) | same | Inside PV frame | Quality path (PV) |
| Spectral envelope | CheapTrick | **13.6** (1.5 periods lookahead) | same | **After F0** | Quality path (WORLD) |
| Aperiodicity | D4C | ~0 (shares window) | same | Parallel to CheapTrick | Quality path (WORLD) |
| Source-filter synthesis | WORLD generator | 9.1 (1 period) | same | After envelope | Quality path (WORLD) |
| Per-voice synthesis ×N | Any engine | **+0** | +0 | **Parallel across voices** | All |
| Blend / crossfade | Blender | 0 (alignment only) | 0 | After both engines | Dual-engine |
| Freeze / hold | Ring buffer read | **0** (or negative) | 0 | Detached from input | Freeze |

**Two rows deserve emphasis.**

*MIDI is parallel and early.* The key-down arrives ~1 ms after you press, while the audio
it will apply to is still crawling through the spine. Relative to the audio path MIDI has
**effectively negative latency** — you know the target pitch before you have the samples
to shift. This is the whole reason the keyboard reframing helps.

*Voice count is free in latency.* Eight voices cost eight times the CPU and zero extra
milliseconds, because analysis is shared and synthesis is parallel. Latency is set by the
slowest *stage*, never by the number of voices.

---

## 4. Critical paths — what actually lands when

| Configuration | Spine | DSP (serial chain) | **Total, steady state** | **Total, at onset** |
|---|---|---|---|---|
| **Mode B, delay-line fast path** | 5.8 | 7 (ramp only) | **~12.8** | ~12.8 |
| **Mode B, PSOLA fast path** | 5.8 | 9.1 + 9.1 (epoch + grain) | **~24** | ~33 |
| **Mode A, PSOLA fast path** | 5.8 | 0 + 9.1 + 9.1 (F0 locked) | **~24** | **~51** (F0 acquisition) |
| **Mode A, WORLD quality path** | 5.8 | 0 + 13.6 + 9.1 | **~28.5** | ~55 |
| **Mode A, phase-vocoder quality path** | 5.8 | 42.7 | **~48.5** | ~48.5 |
| *Reference: Collier's Antares path* | ~13.8 | ~30–55 (inferred) | *~45–70* | — |
| *Reference: Collier's TC hardware path* | ~2 | ~10–18 (inferred) | *~12–20* | — |

**Three findings that fall out of this table.**

**(a) Mode B's delay-line path lands at ~13 ms.** That is inside the range the old design
document called impossible — because it *is* a different problem. No F0, no epochs, no
formant preservation. Whether it sounds acceptable is the open question, not whether it
is fast enough.

**(b) WORLD beats the phase vocoder on latency at steady state (~28 vs ~48 ms).** This
inverts the old document's stack matrix, which put source-filter at 30–60 ms and PV at
40–100 ms without noting that WORLD's F0 cost is a convergence cost, not a per-frame one.
The PV's window length, by contrast, is paid on every single frame forever. **If this
holds under measurement, WORLD is the better quality path on both axes** — it was already
the better one for the preservation spec.

**(c) Mode A's onset cost is driven by your *singing*, not your *playing*.** ~51 ms at a
vocal note entry against ~24 ms sustained — but a MIDI chord change costs neither. The
key-down arrives ~1 ms after you press it, the ring buffer is already full of analysed
material, and a new voice starts within one grain period. **Hold a vowel and play as fast
as you like; nothing re-acquires.** Full acquisition is paid once per *vocal* phrase
entry and on genuine pitch changes.

**Implementation rule that follows (CERTAIN, cheap, high-value):** on unvoiced or
low-confidence frames, **hold** the last F0 rather than re-acquiring. Otherwise every
consonant in a lyric reads as lost lock and re-pays 27 ms. Let the confidence/residual
signal decide when a real re-acquisition is warranted. With this rule, singing words over
fast chords stays responsive; without it, it stutters.

---

## 5. Where the levers actually are

Ranked by milliseconds returned per unit of effort:

| Lever | Saves | Cost | Confidence |
|---|---|---|---|
| Buffer 256 → 64 | **~8 ms** | Driver tuning, LatencyMon work | CERTAIN |
| Fast path instead of quality path | ~24–36 ms | Sounds worse — hence the blend | CERTAIN |
| WORLD instead of phase vocoder | ~20 ms | Must make analysis causal/streaming | PROVISIONAL |
| MIDI prior narrows YIN's search | ~10–20 ms **at onset only** | Moderate; see below | PROVISIONAL |
| Headphones instead of wedge | ~6 ms | Free | CERTAIN |
| Sing higher | Everything pitch-dependent halves per octave | Not a design lever | CERTAIN |

**On the MIDI prior.** In Mode A the keyboard tells you the *target*, not your *source* —
so it can't replace F0 estimation. What it can do is constrain the search: you are
plausibly singing something harmonically related to what you're playing, which narrows
YIN's candidate range and, more importantly, makes octave errors structurally impossible.
Octave errors are the catastrophic failure mode of every blind tracker — not a slight
detune but a voice a full octave adrift. Eliminating that class may justify the machinery
on correctness grounds alone, with the latency saving as a bonus.

---

## 6. What to measure in Phase 0

Replace every provisional number above with a measured one:

1. **Round-trip loopback** at 64 / 128 / 256 samples — validates rows 2–6 as one figure.
2. **LatencyMon** DPC spikes — determines whether 64 is actually attainable.
3. **YIN convergence time** on your own voice at your lowest comfortable note — this is
   the single number that decides how long a *vocal* phrase entry takes to lock. Measure
   it both cold and after an unvoiced gap, to validate the F0-hold rule in §4(c).
4. **MIDI-to-audio-callback jitter** — confirms the ~1 ms parallel assumption.

Until (3) is measured, the onset column in §4 is the least trustworthy part of this
document and the most consequential.
