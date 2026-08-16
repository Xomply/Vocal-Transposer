# vocalharm

MIDI-driven vocal harmonizer. Sing; play chords; hear your voice as those chords.

**Start here: `HANDOVER.md`** — what this is, what it deliberately is not, how to use it,
and where to pick up.

Then:
- `VOICE-MODEL.md` — what a voice actually is, and therefore what the DSP has to get right.
  Which properties must change with pitch, which must be protected from changing, and which
  cannot be recovered from the recording at all. Read this before touching an engine.
- `ARCHITECTURE.md` — module-by-module reasoning, seams, confidence markers
- `RESULTS.md` — measurements and listening guide
- `BUGS.md` — the ledger of defects found but not yet fixed
- `BUGS.md` — open defects with status (Backlog / Doing / In review / Done). Log
  anything you find and cannot fix in scope; do not drop it and do not detour.

## Status

**Milestone 3 reached — the principles work, and the output no longer steps.** Every path
switch in the engine (grain/passthrough, voiced/unvoiced, fast/quality) is now a continuous
crossfade rather than a branch; isolated clicks across the regression grid went **99 → 19**,
and on two real voices never used before, flamenco cante **77 → 1** and consonant-dense
speech **160 → 7**. The sibilant ensemble collapse is fixed — coherence measured 0.82 → 0.13
on real singing, 0 being an ensemble and 1 being N copies of one voice. The formant warp from
milestone 2 is now confirmed to be *applied*, not just computed: measured within 2.4% of
requested across a 5×5 grid, independent of pitch. All for +5% CPU. Measurements, the bug
log, and mistakes caught along the way are in `RESULTS.md`; 48 tests; 25–37% of a 64-sample
budget at 16 voices depending on configuration.

Listening material: `demo/flamenco_reel.wav` and `demo/speech_reel.wav` — before/after A/B
reels with guides beside them (`demo/*_DEMO.md`), built by `tools/demo_reel.py`.

Milestones 1 and 2 (the principles work; the voice model is in the engine) are also in
`RESULTS.md`.

- [x] Real-time-safe engine, allocation-enforced by test
- [x] Shared analysis: two-stage YIN + glottal epoch tracking
- [x] Granular fast engine (~12 ms) and TD-PSOLA quality engine (formant-preserving)
- [x] Dual-engine blend with runtime-swappable policy and tunable time alignment
- [x] Mode A (detachment) and Mode B (interval)
- [x] Freeze via cursor stall, release envelopes, engine re-entry
- [x] Offline WAV harness, demo renderer, benchmark
- [x] Voicing profile: mu and source tilt as CURVES of the shift ratio, not constants
- [x] Grain-content resampling — independent formant control with no FFT, confirmed applied
- [x] Every path switch is a continuous crossfade — no branch anywhere in the signal path
- [x] Voicing gate + F0 momentum — no mode chatter, no single-hop estimate moves the output
- [x] Decorrelated unvoiced path (sibilant collapse — measured fixed, not just measured)
- [ ] Source-filter engine (WORLD/LPC) — still the only route to ring past one source period
- [ ] Humanization beyond the per-voice delay (detune, vibrato, timing jitter, pan)
- [ ] JUCE app/plugin shell

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/vh_tests
./build/vh_bench
./build/vh_demo ./audio
./build/vh_offline in.wav out.wav 60 64 67
python3 tools/click_sweep.py ./build/vh_trace .    # the continuity regression
```

Run `click_sweep.py` after any change to a shifter, the analyser or the Engine's mix loop.

Needs a C++20 compiler and CMake 3.20+. Tests fetch doctest; everything else is
dependency-free.

## Delivering a change

Package a change as a `git format-patch` series (commits, `git am`-able) of
source/tests/tools/docs — never with rendered audio inside it, which `.gitignore` already
excludes and which is regenerable from the tools in the series. Full procedure, including
why each commit should build and pass tests on its own and how to verify a series against a
throwaway clone before it ships: `HANDOVER.md` §9.
