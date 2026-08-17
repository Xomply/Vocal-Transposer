# MIDI progressions for the four proof-of-concept demos

Each voice clip is paired with a distinct chord progression, chosen to exercise a different
part of the engine's design space rather than just repeat the same texture four times.
Chord-spec format matches `vh_render`'s: `t:n,n,n; t:n,n,n; ...` (seconds, MIDI note
numbers, notes held from that time until the next entry).

| Demo | Voice character | MIDI character | Root (Mode B) |
|---|---|---|---|
| 1 | slow scalar, long holds | **melodic jumps** — single moving voice, sparse, wide leaps (up to 2 octaves), uneven timing | 60 |
| 2 | wide leaps, uneven rhythm | **wide-spread chords** — 4-note voicings stretched across 2–3+ octaves | 48 |
| 3 | fast, high-register arpeggio | **close chords** — tight triad/tetrad clusters within an octave, changing every ~2 s | 60 |
| 4 | slow, vowel-alternating, low register | **dual-voice bass + melody** — a held low bass note under a moving upper line | 36 |

## The four specs

```
demo1 (melodic jumps):
0:64; 2.2:52; 4.4:71; 6.6:57; 8.8:67; 11:50; 13.2:74; 15.4:55;
17.6:62; 19.8:79; 22:48; 24.2:69; 26.4:59; 28.6:65

demo2 (wide-spread chords, 2-3+ octaves):
0:48,55,64,72; 4:45,52,64,76; 8:43,55,62,79; 12:48,60,67,79;
16:41,53,60,77; 20:48,55,64,72; 24:43,58,65,74; 27.5:45,52,64,76

demo3 (close chords, tight clusters, fast changes):
0:60,63,67; 2:62,65,69; 4:60,64,67; 6:59,62,65; 8:60,63,67,70;
10:57,60,64; 12:60,63,67; 14:62,65,68; 16:60,64,67; 18:58,62,65;
20:60,63,67,70; 22:60,64,67; 24:57,60,64; 26:60,63,67

demo4 (dual voice: held bass + moving melody):
0:36,64; 2.5:36,67; 5:36,71; 7.5:36,69;
10:41,72; 12.5:41,76; 15:41,74; 17.5:41,71;
20:38,67; 22.5:38,71; 25:38,74; 27.5:38,69
```

## Why each one is shaped the way it is

**demo1 — melodic jumps.** Never more than one note held at a time, so there is no chord
math to confound the read: this isolates how the shifter itself handles a single voice
leaping around (52→71 is a 19-semitone jump, well past PSOLA's documented ±6 st comfort
zone at points). Timing is deliberately uneven (2.2 s, then 2.2 s, then 2.2 s, then
alternating) rather than a metronomic grid, closer to how a keyboard player actually plays.

**demo2 — wide-spread chords.** Four-note voicings spanning up to 38 semitones
(41→79, roughly 3.2 octaves) top to bottom. This is the case `ARCHITECTURE.md` and
`HANDOVER.md` both flag as the biggest known gap: "PSOLA ends around ±6 semitones... that
is where the technique ends, not a tuning problem." The widest voices in this progression
are exactly the ones expected to show PSOLA degrading and the source-filter engine's
absence hurting.

**demo3 — close chords.** Triads and tetrads within a single octave, changing roughly every
2 seconds — the opposite texture from demo2. Tight voicings stress polyphony differently:
near-unison partials beat against each other, and the fast chord-change rate stresses
the epoch tracker's relock behaviour more than the register split does.

**demo4 — dual voice.** A genuinely different *shape* of chord: not a block voicing but two
independent lines an octave-plus apart (a bass note held under a moving upper voice, which
is how a keyboardist would actually play a bass-and-melody texture with two hands). This
tests whether the shared-analysis/per-voice-synthesis architecture (`ARCHITECTURE.md`
decision #2) genuinely keeps the two lines independent, since one voice's F0 estimate must
not leak into the other's synthesis when they move independently.
