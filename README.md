# vocalharm

MIDI-driven vocal harmonizer. Sing; play chords; hear your voice as those chords.

**Start here: `HANDOVER.md`** — what this is, what it deliberately is not, how to use it,
and where to pick up.

Then:
- `ARCHITECTURE.md` — module-by-module reasoning, seams, confidence markers
- `RESULTS.md` — measurements, listening guide, and the full bug log

## Status

**Milestone 1 reached — the principles work.** Measurements and listening notes in
`RESULTS.md`: worst-case pitch error 4.2 cents, 16 voices through both engines at 12% of a
64-sample callback budget.

- [x] Real-time-safe engine, allocation-enforced by test
- [x] Shared analysis: two-stage YIN + glottal epoch tracking
- [x] Granular fast engine (~12 ms) and TD-PSOLA quality engine (formant-preserving)
- [x] Dual-engine blend with runtime-swappable policy and tunable time alignment
- [x] Mode A (detachment) and Mode B (interval)
- [x] Freeze via cursor stall, release envelopes, engine re-entry
- [x] Offline WAV harness, demo renderer, benchmark
- [ ] Source-filter engine (WORLD/LPC) — needed for octaves and timbre control
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
