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

**Milestone 2 reached — the voice model is in the engine.** `mu = ratio^0.3` gives
independent formant control from inside PSOLA, measured at -24 st to raise duty by exactly
the predicted 1/mu with zero cents of pitch movement. VH-003's 17 dB upward level loss is
fixed. Measurements in `RESULTS.md`; 41 tests; 18% of a 64-sample budget at 16 voices.

Milestone 1 (the principles work) is also in `RESULTS.md`.

- [x] Real-time-safe engine, allocation-enforced by test
- [x] Shared analysis: two-stage YIN + glottal epoch tracking
- [x] Granular fast engine (~12 ms) and TD-PSOLA quality engine (formant-preserving)
- [x] Dual-engine blend with runtime-swappable policy and tunable time alignment
- [x] Mode A (detachment) and Mode B (interval)
- [x] Freeze via cursor stall, release envelopes, engine re-entry
- [x] Offline WAV harness, demo renderer, benchmark
- [x] Voicing profile: mu and source tilt as CURVES of the shift ratio, not constants
- [x] Grain-content resampling — independent formant control with no FFT
- [ ] Source-filter engine (WORLD/LPC) — still the only route to ring past one source period
- [ ] Humanization applied (struct exists, Engine does not read it yet)
- [ ] Decorrelated unvoiced path (sibilant collapse — measured, unsolved)
- [ ] JUCE app/plugin shell

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
./build/vh_tests
./build/vh_bench
./build/vh_demo ./audio
./build/vh_offline in.wav out.wav 60 64 67
```

Needs a C++20 compiler and CMake 3.20+. Tests fetch doctest; everything else is
dependency-free.
