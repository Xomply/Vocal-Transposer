# app/ — JUCE shell (not yet written)

Deliberately empty. `core/` links no framework so it can be tested headlessly; this
directory is where the JUCE standalone app and VST3 plugin will live, both wrapping the
same `vh::Engine`.

When writing it:

- The audio callback does exactly three things: pump the MIDI queue into
  `Engine::noteOn/noteOff/setSustain`, call `Engine::process`, and nothing else.
- No parameter smoothing, voice logic, or DSP in the shell. If it feels like it belongs
  in the shell, it belongs in `core`.
- Buffer 64-128 samples, native ASIO driver. Not ASIO4ALL, not WASAPI shared.
- Report `Engine::latencySamples()` to the host for plugin delay compensation.
