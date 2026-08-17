# Proof-of-concept demos — measurements and listening guide

Four real solo voices × four different MIDI progressions, rendered through every engine
configuration. Source material and why it was built rather than found:
`demo_new/ATTRIBUTION.md`. The chord progressions and why each is shaped the way it is:
`demo_new/midi_progressions.md`.

**The point of this document is to be cross-referenced against your ears.** Each section
says what the numbers claim and what that should sound like. Where they disagree, the ears
win and the measurement is the thing that needs fixing — that has already happened once in
this batch (see "The measurement that was wrong" at the end).

## Reproduce

```bash
python3 tools/build_voice_clips.py fetch /tmp/voicework      # clone the note bank
python3 tools/make_voice_clips.py /tmp/voicework audio/      # build the 4 dry clips
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build -j

./build/vh_render audio/vh_demo1_sustain.wav  demo_new/vh_demo1_sustain  demo1 \
  "0:64; 2.2:52; 4.4:71; 6.6:57; 8.8:67; 11:50; 13.2:74; 15.4:55; 17.6:62; 19.8:79; 22:48; 24.2:69; 26.4:59; 28.6:65" 60
./build/vh_render audio/vh_demo2_melody.wav   demo_new/vh_demo2_melody   demo2 \
  "0:48,55,64,72; 4:45,52,64,76; 8:43,55,62,79; 12:48,60,67,79; 16:41,53,60,77; 20:48,55,64,72; 24:43,58,65,74; 27.5:45,52,64,76" 48
./build/vh_render audio/vh_demo3_arpeggio.wav demo_new/vh_demo3_arpeggio demo3 \
  "0:60,63,67; 2:62,65,69; 4:60,64,67; 6:59,62,65; 8:60,63,67,70; 10:57,60,64; 12:60,63,67; 14:62,65,68; 16:60,64,67; 18:58,62,65; 20:60,63,67,70; 22:60,64,67; 24:57,60,64; 26:60,63,67" 60
./build/vh_render audio/vh_demo4_lowvowel.wav demo_new/vh_demo4_lowvowel demo4 \
  "0:36,64; 2.5:36,67; 5:36,71; 7.5:36,69; 10:41,72; 12.5:41,76; 15:41,74; 17.5:41,71; 20:38,67; 22.5:38,71; 25:38,74; 27.5:38,69" 36

python3 tools/demo_battery.py demo_new audio demo_new/one_voice
```

---

## 1. The suite still passes on new material

| | result |
|---|---|
| `vh_tests` | **48/48 pass**, 2,107,452 assertions |
| `vh_bench` worst-case pitch error | **2.1 cents** (granular +5.5 at 110 Hz −5 st) |
| `vh_bench` latency | YIN 28.6 ms · granular 12.1 ms · PSOLA 28.6 ms — unchanged |
| `vh_bench` CPU, 16 voices both engines @ 64 | **10.8%** of budget |
| all 28 rendered outputs | **finite**, no `inf`, no NaN, no 220 dB dropouts |
| all 4 dry sources | **0 clicks** — the material itself is clean going in |

The `finite` column matters more than it looks. `RESULTS.md` bugs #5 and #6 were both
found only because real recordings *start with silence*, and these four clips are new
material that starts with silence and had never been through the engine before. Nothing
reappeared.

## 2. Clicks per render

`tools/click_probe.py`'s detector, dry file as the control. **Dry scores 0 on all four**,
so unlike the flamenco/speech material there is no source-transient floor to subtract here —
every count below is the engine's own.

| demo | dry | granular | psola | blend | blend_aligned | mix | modeB |
|---|---|---|---|---|---|---|---|
| 1 sustain (melodic jumps) | 0 | **27** | 1 | 7 | 19 | 0 | 0 |
| 2 melody (wide chords) | 0 | **48** | 0 | 3 | 6 | 0 | 0 |
| 3 arpeggio (close chords) | 0 | **178** | 2 | 7 | 10 | 1 | 2 |
| 4 lowvowel (bass+melody) | 0 | **40** | 3 | 8 | 18 | 0 | 2 |

**What to listen for.** The granular column is the story: the fast path is 10–90× clickier
than the quality path on every single demo. That is the delay-line engine being what it is
— `ARCHITECTURE.md` says it "never sounds transparent" and this is that, quantified. In
`_granular.wav` you should hear a grainy, slightly buzzy edge that is absent from
`_psola.wav`, and on demo3 it should be clearly objectionable.

**demo3 is the standout (178).** The clicks are not spread evenly — they cluster:

```
 2-4s:11   4-6s:32   6-8s:8   8-10s:5  10-12s:3  12-14s:9
14-16s:38  16-18s:16  18-20s:23  20-22s:3  22-24s:2  24-26s:2  26-28s:26
```

Those clusters land on chord changes. demo3 changes chord every 2 s — faster than any other
demo — so this is the granular engine's re-lock behaviour under rapid retargeting, not a
steady-state defect. **Listen at 4–6 s, 14–16 s and 26–28 s specifically.** If it sounds
like a click *at the moment the chord moves*, the measurement and your ears agree and the
diagnosis is retargeting. If it sounds continuously grainy throughout instead, the
clustering is telling you something the detector cannot see and it is worth re-opening.

**`blend_aligned` is consistently worse than `blend`** (19 vs 7, 6 vs 3, 10 vs 7, 18 vs 8).
That is the alignment dial doing exactly what `RESULTS.md`'s listening guide says it does:
alignment 1 is phase-coherent and gives up the latency advantage, alignment 0 is Bloomberg's
choice. Worth confirming by ear which trade you actually prefer — this is the one dial the
whole architecture was shaped around.

## 3. Formant preservation — the argument for the second engine

Median spectral centroid over voiced frames (frames classified on the dry file, so the same
windows are used for every render).

| demo | dry | granular | psola | granular − psola |
|---|---|---|---|---|
| 1 sustain | 577 Hz | 1040 Hz | 835 Hz | **+204 Hz** |
| 2 melody | 634 Hz | 1212 Hz | 844 Hz | **+368 Hz** |
| 3 arpeggio | 795 Hz | 1075 Hz | 954 Hz | **+121 Hz** |
| 4 lowvowel | 538 Hz | 1138 Hz | 735 Hz | **+403 Hz** |

**This is the cleanest result in the batch.** Granular sits above PSOLA on all four, by
121–403 Hz. Resampling drags the spectral envelope up with pitch; PSOLA re-emits unmodified
grains at a new rate and does not. This is the test `ARCHITECTURE.md` describes as "the test
that would catch either engine quietly becoming the other," and both engines are behaving
as their algorithms predict.

**What to listen for.** A/B `_granular.wav` against `_psola.wav` on **demo4**, which has the
largest gap (+403 Hz) and the lowest source voice. The granular version should sound
chipmunked/thin on the upward voices — the vowel identity shifting with the pitch. The PSOLA
version should keep the vowel sounding like the same singer's mouth. If you cannot hear a
difference on demo4, something is wrong that these numbers are not catching.

Both engines sit above dry because these progressions are predominantly upward shifts. That
is expected and is not evidence about formants either way.

## 4. Where the known gaps show up

**PSOLA past ±6 semitones (demo2).** The wide-chord progression spans up to 38 semitones
(41→79). `HANDOVER.md` §4 is explicit that "PSOLA ends around ±6 semitones — that is where
the technique ends, not a tuning problem," and demo2 exists to put that on the record
audibly. The spectrogram at 8–16 s shows dense, coherent harmonic content with no breakage
stripes, so it does not *fail*; the question is whether the extreme voices sound like a
singer or like an artifact. **Listen to demo2's lowest and highest voices specifically.**
This is the case the missing source-filter engine (WORLD/LPC) would fix, and it is
`HANDOVER.md`'s #4 pick-up item.

**`envelopeWarp` still unused.** The μ≈0.8 vocal-tract warp that makes an octave-down read
as a bass rather than as slowed tape is carried in `PreservationSpec` and applied by
nothing, because neither engine can move the envelope independently of pitch. demo4's bass
line (MIDI 36 = 65.4 Hz) is where you would hear its absence most.

**Humanization still unapplied.** N voices are N identical shifted copies. On demo3's close
chords — tight clusters, near-unison partials — this should be most audible as a flanged,
single-source quality rather than several singers. `HANDOVER.md` calls this "the cheapest
large improvement available" and demo3 is the material that makes the case.

**Sibilant collapse (VH-005) is NOT tested by this material.** All four clips are sung
vowels with essentially no fricatives — 10 to 23 unvoiced frames each, out of ~900. The
coherence figures below are reported for completeness and are weakly supported; do not read
them as evidence either way. The existing `speech_dry.wav` remains the right material for
that question.

| demo | unvoiced frames | unvoiced dB | voiced dB (control) |
|---|---|---|---|
| 1 sustain | 20 | −0.79 | +0.05 |
| 2 melody | 20 | +1.71 | +0.23 |
| 3 arpeggio | 10 | +0.64 | +0.37 |
| 4 lowvowel | 23 | +1.39 | +2.87 |

## 5. Listening order

Per demo, in this order:

1. **`*_dry.wav`** — the source. One real singer, no instruments. It is assembled from
   single-note takes (see `ATTRIBUTION.md`), so it sounds like a careful vocal exercise
   rather than a song; judge the processing by dry-against-wet, not by whether the dry
   sounds like a performance.
2. **`*_psola.wav`** — quality path. This is the one that should sound like harmony.
3. **`*_granular.wav`** — fast path. Cheaper, ~12 ms instead of ~29 ms, and audibly so.
   The entire argument for having two engines is the difference between this and the
   previous file.
4. **`*_blend.wav`** vs **`*_blend_aligned.wav`** — the dial. Unaligned is Bloomberg's
   choice; aligned is phase-coherent and surrenders the latency win.
5. **`*_mix.wav`** — harmony plus dry voice, i.e. what a room would actually hear.
6. **`*_modeB.wav`** — intervals from a declared root, singer's own intonation riding
   through untouched. On demo1 (single moving line) the difference from Mode A is clearest.

Then across demos: **demo2** for the wide-interval limit, **demo3** for the granular click
clusters and for close-voicing ensemble quality, **demo4** for formant preservation and for
the low bass.

## 6. The measurement that was wrong

Worth recording, because it is the third instance of the same pattern in this project and
it was caught only by disbelieving a number.

The first battery run reported demo4's unvoiced coherence at **+5.24 dB** against
under 1 dB for the other three, with **621 unvoiced frames** against 15–91. Read naively
that is a dramatic sibilant collapse on the low-voice material.

It was not. `ensemble_probe.auto_report` classifies voiced/unvoiced on whichever signal it
is handed as `one` — and `one` was the single-note *render*, not the dry file. demo4's
single-note render targets MIDI 36 = **65.4 Hz**, below both YIN's 70 Hz floor and
`classify_frames`' own 70 Hz autocorrelation search bound. So 621 frames of a correctly-sung
low bass note were scored "unvoiced," and the ratio computed over them measured nothing but
the classifier running out of range. The dry file has 23 unvoiced frames, not 621.

*Fix:* classify frames on the **dry** file. Voicing is a property of the source material,
identical across every render being compared, so misclassification cancels between them
instead of varying with the shift ratio. `tools/demo_battery.py` now does this and prints a
warning when the unvoiced frame count is too small to support a conclusion.

The lesson is the one `RESULTS.md` already states twice: *measure the thing, not a proxy for
it*, and a metric whose value depends on the configuration under test cannot compare
configurations.
