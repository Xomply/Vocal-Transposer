#include "vh/engine.hpp"
#include "vh/rt.hpp"

#include <algorithm>
#include <cmath>

namespace vh {
namespace {

double midiToHz(int note) noexcept {
    return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

const AnalysisFrame kEmptyFrame{};

} // namespace

Engine::Engine() { blend_ = &defaultBlend_; }
Engine::~Engine() = default;

void Engine::prepare(const EngineConfig& cfg,
                     std::unique_ptr<IAnalyzer> analyzer,
                     ShifterFactory makeFast,
                     ShifterFactory makeQuality) {
    cfg_ = cfg;

    // AudioRing construction moved here from the constructor's initialiser list
    // (app-design.md §4.1/§6, the "move AudioRing into prepare()" refactor): input history
    // length now joins the ordinary restart tier — re-prepare() resizes it, like every
    // other buffer below, rather than needing the Engine itself rebuilt. make_unique here
    // is the allocation this whole function exists to contain; nothing on the audio thread
    // ever constructs an AudioRing.
    ring_ = std::make_unique<AudioRing>(cfg_.inputHistoryFrames);

    analyzer_ = std::move(analyzer);
    if (analyzer_) analyzer_->prepare(cfg_.sampleRate, cfg_.maxBlock);

    voices_.assign(kMaxVoices, Voice{});

    fast_.clear();
    quality_.clear();
    fast_.reserve(kMaxVoices);
    quality_.reserve(kMaxVoices);
    for (int i = 0; i < kMaxVoices; ++i) {
        auto f = makeFast ? makeFast() : nullptr;
        auto q = makeQuality ? makeQuality() : nullptr;
        if (f) f->prepare(cfg_.sampleRate, cfg_.maxBlock);
        if (q) q->prepare(cfg_.sampleRate, cfg_.maxBlock);
        fast_.push_back(std::move(f));
        quality_.push_back(std::move(q));
    }

    fastBuf_.assign(cfg_.maxBlock, Sample{0});
    qualityBuf_.assign(cfg_.maxBlock, Sample{0});

    // Input/output stage scratch and state (app-design.md §5.5, Track D). Reset on every
    // (re-)prepare(), same as everProcessed_/alignPrimed_ below -- a fresh prepare() is a
    // fresh instrument, and stale gate/limiter envelope state from a previous sample rate
    // or block size has no meaning against the new one.
    inputStageBuf_.assign(cfg_.maxBlock, Sample{0});
    gateEnv_ = 0.0f;
    peakEnv_ = 0.0f;
    limiterGain_ = 1.0f;

    // Alignment delay: worst case is the full quality-path latency, which is two periods
    // of the lowest fundamental we track. Rounded to a power of two so the wrap is a mask.
    FrameCount need = static_cast<FrameCount>(cfg_.sampleRate / 60.0) * 3 + cfg_.maxBlock;
    FrameCount cap = 1;
    while (cap < need) cap <<= 1;
    alignStride_ = cap;
    alignBuf_.assign(alignStride_ * static_cast<FrameCount>(kMaxVoices), Sample{0});
    alignWrite_.assign(static_cast<size_t>(kMaxVoices), 0);

    // Per-voice decorrelation line. Sized for the largest static delay any voice can be
    // given (see noteOn), rounded up to a power of two so the wrap is a mask.
    //
    // RAISED from 0.010 to 0.050 (app-design.md §4.5 item 2 / BUGS.md): the old 10 ms
    // sizing bound made a live spread control above ~10 ms wrap the ring and read the
    // wrong-aged sample — quietly wrong audio, not a crash. tuning.hpp's Tuning::clamp()
    // enforces the MATCHING 50 ms bound on decorMinMs/decorMaxMs; the two literals are
    // independent copies by necessity (core stays dependency-free, so this file cannot
    // include tuning.hpp for one constant) and must be raised together, which is why both
    // changed in this same commit rather than one now and one "later".
    FrameCount dneed = static_cast<FrameCount>(cfg_.sampleRate * 0.050) + cfg_.maxBlock;
    FrameCount dcap = 1;
    while (dcap < dneed) dcap <<= 1;
    decorStride_ = dcap;
    decorBuf_.assign(decorStride_ * static_cast<FrameCount>(kMaxVoices), Sample{0});
    decorWrite_.assign(static_cast<size_t>(kMaxVoices), 0);

    // app-design.md §4.5 item 5: the NEXT call to setBlendAlignment()/applyTuning() after
    // this (re-)prepare() must snap rather than ramp, and no block has been processed yet
    // against the freshly (re)sized buffers above. See setAlignmentTarget()/mixImpl().
    everProcessed_ = false;
    alignPrimed_ = false;

    prepared_ = true;
}

const AnalysisFrame& Engine::analysis() const noexcept {
    return analyzer_ ? analyzer_->current() : kEmptyFrame;
}

int Engine::activeVoiceCount() const noexcept {
    int n = 0;
    for (const auto& v : voices_) if (v.active()) ++n;
    return n;
}

// Shared by all three public latency accessors: the transport-delay sum that is correct
// once a voice is actually running, in EITHER mode. See engine.hpp's doc comment on the
// three latency accessors for the question each one answers and why VH-012 split them.
FrameCount Engine::steadyStateLatencySamples() const noexcept {
    const FrameCount lf = (!fast_.empty() && fast_[0]) ? fast_[0]->latencySamples() : 0;
    const FrameCount lq = (!quality_.empty() && quality_[0]) ? quality_[0]->latencySamples() : 0;

    // The per-voice decorrelation delay is real output latency and is included, because a
    // host doing delay compensation needs the worst case, not the typical one. It is the
    // largest value noteOn can assign (see decorMaxMs and the golden-ratio spread in
    // noteOn() — this now tracks the LIVE bound, not a hardcoded 4 ms, so raising
    // decorMaxMs via Tuning is reflected here rather than silently under-reported).
    const FrameCount decor =
        (cfg_.preservation.unvoiced == UnvoicedPolicy::PassThroughDecorrelated)
            ? static_cast<FrameCount>(cfg_.sampleRate * cfg_.decorMaxMs * 0.001) : 0;

    // Parallel engines: max, not sum. The listener hears the fast one first, so the
    // FELT latency is lf; the figure reported here is when the full blend is available,
    // which is what a host needs for delay compensation.
    return std::max(lf, lq) + decor;
}

FrameCount Engine::acquisitionLatencySamples() const noexcept {
    FrameCount worst = 0;
    if (analyzer_ && cfg_.ratioSource == RatioSource::AbsoluteTarget) {
        // Mode A needs F0 before it can compute a ratio; Mode B does not.
        // This asymmetry is the whole latency argument for Mode B. `worst` is a
        // CONVERGENCE cost — paid once when the tracker locks onto a new pitch — not a
        // recurring per-block tax, which is exactly why it is added here and NOT inside
        // steadyStateLatencySamples() (BUGS.md VH-012).
        worst = analyzer_->latencySamples();
    }
    return worst + steadyStateLatencySamples();
}

FrameCount Engine::latencySamples() const noexcept {
    // Kept bit-identical to what this function has always returned — see engine.hpp's doc
    // comment on why this is not simply deleted in favour of the two named accessors above.
    return acquisitionLatencySamples();
}

int Engine::allocateVoice(int midiNote) noexcept {
    // Runtime voice cap (app-design.md §5.6). Defensive re-clamp here too: cfg_.voiceCap
    // normally arrives already clamped to [1, kMaxVoices] via Tuning::clamp(), but
    // EngineConfig can be handed to prepare() directly, bypassing Tuning entirely. At the
    // default (voiceCap == kMaxVoices == voices_.size()) `cap` is exactly what the loops
    // below iterated before this field existed — bit-identical allocation behaviour.
    const int cap = std::clamp(cfg_.voiceCap, 1, static_cast<int>(voices_.size()));

    // Retriggering a note that is already sounding reuses its voice rather than starting
    // a second one. Without this, holding the sustain pedal and repeatedly striking the
    // same key stacks identical voices, which sums to something progressively louder and
    // more phase-cancelled rather than to a chord.
    for (int i = 0; i < cap; ++i) {
        if (voices_[i].active() && voices_[i].midiNote == midiNote) return i;
    }
    for (int i = 0; i < cap; ++i) {
        if (!voices_[i].active()) return i;
    }
    // Steal the oldest. PROVISIONAL: oldest-first is the conventional choice and is
    // musically defensible (the newest note is the one you just asked for). If chord
    // stacking under the sustain pedal turns out to steal notes you wanted kept, revisit
    // — a "steal the quietest" or "steal the most recently released" policy may suit
    // better.
    int oldest = 0;
    Pos oldestPos = voices_[0].noteOnPos;
    for (int i = 1; i < cap; ++i) {
        if (voices_[i].noteOnPos < oldestPos) { oldestPos = voices_[i].noteOnPos; oldest = i; }
    }
    return oldest;
}

void Engine::noteOn(int midiNote, float velocity) noexcept {
    VH_RT_SECTION();
    if (!prepared_) return;
    const int idx = allocateVoice(midiNote);
    Voice& v = voices_[idx];

    v.state = VoiceState::Attacking;
    v.midiNote = midiNote;
    v.velocity = velocity;
    v.ratioSource = cfg_.ratioSource;
    v.noteOnPos = ring_->writePos();

    // Retriggering a held voice must not leave it stuck in whatever hold/freeze state its
    // previous life ended in.
    v.holdDeferred = false;
    v.freezeLatched = false;

    // Velocity -> gain curve (app-design.md §5.6, Tuning::velocityToGainMin/velocityCurve).
    // At the default velocityToGainMin == 1.0, `1.0 + (1.0 - 1.0) * x` is bit-exactly 1.0
    // for ANY velocity or curve exponent — identical to today's Engine, which stores
    // velocity and never reads it.
    {
        const float vel = std::clamp(velocity, 0.0f, 1.0f);
        v.gain = cfg_.velocityToGainMin +
                 (1.0f - cfg_.velocityToGainMin) * std::pow(vel, cfg_.velocityCurve);
    }

    // Start reading far enough back that BOTH engines have the history they need.
    //
    // THE BUG THIS FIXES, recorded because it cost real debugging time and the symptom
    // pointed nowhere near the cause: using only the fast engine's latency here starves
    // the quality engine. PSOLA needs about two pitch periods of lookahead to build a
    // grain; given only the granular shifter's ~1.5 periods, its residency checks fail and
    // it silently drops grains. The audible result is not a glitch or an error — it is a
    // blend that is roughly 6 dB quieter than either engine alone, which reads as a
    // mixing problem and sends you looking at the blend weights, which are fine.
    //
    // The lookback must satisfy the SLOWEST engine. The blender then decides how much of
    // that head start to give back as alignment.
    FrameCount lookback = 0;
    if (fast_[idx]) lookback = std::max(lookback, fast_[idx]->latencySamples());
    if (quality_[idx]) lookback = std::max(lookback, quality_[idx]->latencySamples());
    v.cursor = ReadCursor{};
    v.cursor.next = v.noteOnPos > lookback ? v.noteOnPos - lookback : 0;
    v.cursor.rngState = static_cast<std::uint32_t>(idx * 2654435761u + 1u);

    v.envGain = 0.0f;      // fade in; a voice starting mid-vowel begins at an arbitrary
    v.envPhase = 0.0f;     // waveform amplitude, and stepping to it clicks
    v.envTarget = 1.0f;

    // Spread the voices over decorMinMs..decorMaxMs (default 0.5-4 ms, live via
    // Tuning::decorMinMs/decorMaxMs -> EngineConfig -> here) using the golden ratio, so no
    // two delays are a simple multiple of one another. Equal spacing would put every
    // voice's comb notches on the same frequencies and colour the ensemble instead of
    // diffusing it. Derived from the voice INDEX rather than randomised, so a given chord
    // decorrelates the same way every render and the measurement is reproducible.
    const double frac = std::fmod(static_cast<double>(idx) * 0.6180339887498949, 1.0);
    v.hum.staticDelayMs = static_cast<float>(
        cfg_.decorMinMs + (cfg_.decorMaxMs - cfg_.decorMinMs) * frac);

    // --- Humanization: per-voice values derived DETERMINISTICALLY from voice index -------
    // (app-design.md §5.2). Same reasoning as decorMinMs/decorMaxMs above: a given chord
    // must humanize identically on every render, or a before/after comparison is comparing
    // two different PERFORMANCES rather than two engines.
    //
    // A DIFFERENT irrational multiplier per row, rather than reusing 0.6180339887498949
    // (the golden ratio conjugate) for every one of them: sharing one sequence across rows
    // would make voice N "the sharp one" AND "the far-panned one" AND "the fast-vibrato-
    // phase one" all at once, in the same relative order every time — correlated
    // deviations read as one ensemble property moving, not as N independent singers. Each
    // row below gets its own low-discrepancy sequence (fmod(idx * irrational, 1)) so the
    // rows decorrelate from EACH OTHER the same way the decorrelation delay already
    // decorrelates voices from each other. All are irrational multiples of 1 for the same
    // "no two voices land on a simple multiple" reason app-design.md gives for the
    // golden-ratio precedent.
    //
    // THE +0.5*mult OFFSET IS LOAD-BEARING, not decoration: `fmod(idx*mult, 1.0)` alone
    // is EXACTLY 0.0 whenever idx == 0, for every possible mult — voice index 0 (the FIRST
    // voice allocated, which is very often the only note of a single held chord, or its
    // lowest voice) would land on the zero corner of every single row at once: no detune,
    // vibrato phase exactly 0, no onset jitter, panned to a fixed extreme. That is the
    // opposite of "no two voices at a simple multiple of one another" applied to voice 0
    // against the origin itself. Shifting every row by half its own step keeps the sequence
    // just as deterministic and reproducible while moving voice 0 off that corner.
    auto spread01 = [idx](double mult) noexcept {
        return std::fmod(static_cast<double>(idx) * mult + 0.5 * mult, 1.0);
    };

    // detuneCents: a fixed per-voice offset within +/-humanize.detuneCents/2, set ONCE
    // here — "static, per voice" (voice.hpp). At humanize.detuneCents == 0 this is exactly
    // 0.0f (0.0 * anything == 0.0), which is what keeps the ratio multiplier below an exact
    // 1.0 when humanization is off.
    v.hum.detuneCents = static_cast<float>(
        cfg_.humanize.detuneCents * (spread01(0.4142135623730951) - 0.5));   // sqrt(2)-1

    // detuneDriftCents holds the AMPLITUDE every voice's own random walk is bounded to
    // (tuning.hpp: "every voice runs its OWN walk ... decorrelation comes from the
    // per-voice seed, not from this number differing per voice"). The WALK ITSELF is
    // humDriftValue/humDriftRng below, advanced once per block in mixImpl().
    v.hum.detuneDriftCents = cfg_.humanize.detuneDriftCents;
    v.humDriftValue = 0.0f;
    v.humDriftRng = static_cast<std::uint32_t>(idx * 2246822519u + 3266489917u) | 1u;

    // vibratoRateHz/vibratoDepthCents are shared by every voice ON PURPOSE (tuning.hpp) —
    // different RATES would beat against a held chord rather than reading as one ensemble;
    // only PHASE decorrelates the voices.
    v.hum.vibratoRateHz = cfg_.humanize.vibratoRateHz;
    v.hum.vibratoDepthCents = cfg_.humanize.vibratoDepthCents;
    // vibratoPhase: randomised per voice — "the entire point" (voice.hpp:56).
    v.hum.vibratoPhase = static_cast<float>(spread01(0.7320508075688772) * 2.0 * kPi); // sqrt(3)-1

    // onsetJitterMs: entry-timing spread within [0, onsetJitterMs]. mixImpl() skips this
    // voice (does not advance it) until the ring's write position reaches startAtPos —
    // same shape as the pre-existing history gate, so a jittered voice is simply "not
    // caught up yet" rather than a special case.
    v.hum.onsetJitterMs = cfg_.humanize.onsetJitterMs;
    {
        const double jitterFrac = spread01(0.6457513110645907);   // sqrt(7)-2
        const Pos jitterSamples = static_cast<Pos>(
            cfg_.humanize.onsetJitterMs * jitterFrac * 0.001 * cfg_.sampleRate);
        v.startAtPos = v.noteOnPos + jitterSamples;
    }

    // envelopeWarpOffset: symmetric offset on mu (ShiftRequest::muScale = 1 + this),
    // applied by PSOLA after muFor() — a slight per-voice vocal-tract difference.
    v.hum.envelopeWarpOffset = static_cast<float>(
        cfg_.humanize.envelopeWarpSpread * (spread01(0.31662479035539985) - 0.5)); // sqrt(11)-3

    // pan: -1..+1, spread by index; consumed only by the STEREO mixImpl instantiation
    // (app-design.md §5.3) — mono ignores it entirely, by construction (see mixImpl()).
    v.hum.pan = static_cast<float>(
        cfg_.humanize.panSpread * (spread01(0.6055512754639891) * 2.0 - 1.0)); // sqrt(13)-3

    v.fastRan = false;
    v.qualityRan = false;

    if (fast_[idx]) fast_[idx]->reset();
    if (quality_[idx]) quality_[idx]->reset();
}

void Engine::noteOff(int midiNote) noexcept {
    VH_RT_SECTION();
    for (auto& v : voices_) {
        if (v.active() && v.midiNote == midiNote) {
            if (v.freezeLatched) {
                // LATCHED. Freeze stops this voice responding to note-off entirely (app-
                // design.md §5.1) — not deferred like hold below, simply not listened to.
                // The voice will not release until setFreeze(false) does it.
                continue;
            }
            if (hold_) {
                // HOLD (conventional sustain). Defer the release: state, cursor and
                // everything else are untouched, so the voice keeps tracking live input
                // exactly as an unreleased note would. Released in setHold(false).
                v.holdDeferred = true;
                continue;
            }
            v.state = VoiceState::Releasing;
            v.envTarget = 0.0f;
        }
    }
}

void Engine::setHold(bool held) noexcept {
    VH_RT_SECTION();
    hold_ = held;
    if (!held) {
        for (auto& v : voices_) {
            if (v.holdDeferred) {
                v.holdDeferred = false;
                v.state = VoiceState::Releasing;
                v.envTarget = 0.0f;
            }
        }
    }
}

void Engine::setFreeze(bool engaged) noexcept {
    VH_RT_SECTION();
    if (engaged && !freeze_) {
        // ENGAGE. Latch every currently SOUNDING voice and stall its cursor into a loop
        // window ending at its position right now — the same cursor-stall mechanics the
        // old setSustain(true)+note-off used, just triggered at freeze-engage time instead
        // of at note-off time, and paired with the latch (see Voice::freezeLatched).
        for (auto& v : voices_) {
            if (!v.active()) continue;
            v.freezeLatched = true;
            v.cursor.frozen = true;
            v.cursor.freezeEnd = v.cursor.next;
            // Was a hardcoded `sampleRate * 0.5` (500 ms); live via
            // Tuning::freezeWindowMs -> EngineConfig::freezeWindowMs.
            const Pos span = static_cast<Pos>(cfg_.sampleRate * cfg_.freezeWindowMs * 0.001);
            v.cursor.freezeBegin = v.cursor.freezeEnd > span ? v.cursor.freezeEnd - span : 0;
        }
    } else if (!engaged && freeze_) {
        // RELEASE. Every latched voice fades out — freeze is a captured pad, not a hold
        // you can silently keep singing under after letting go of it (app-design.md §5.1).
        for (auto& v : voices_) {
            if (v.freezeLatched) {
                v.freezeLatched = false;
                v.cursor.frozen = false;
                v.state = VoiceState::Releasing;
                v.envTarget = 0.0f;
            }
        }
    }
    freeze_ = engaged;
}

void Engine::allNotesOff() noexcept {
    VH_RT_SECTION();
    for (auto& v : voices_) {
        if (v.active()) {
            v.holdDeferred = false;
            v.freezeLatched = false;
            v.cursor.frozen = false;
            v.state = VoiceState::Releasing;
            v.envTarget = 0.0f;
        }
    }
}

void Engine::setAlignmentTarget(float clamped01) noexcept {
    alignTarget_ = clamped01;
    // Snap if the engine has not produced a block since prepare(), or this is the first
    // time alignment has been set since prepare() — see app-design.md §4.5 item 5 and this
    // function's declaration in engine.hpp for the reasoning. Every existing offline tool
    // calls setBlendAlignment() exactly once, immediately after prepare() and before any
    // process() call, so this is the ONLY branch they ever take: bit-identical to the old
    // step-only behaviour. A live change made after audio has started falls through to the
    // ramp in mixImpl().
    if (!everProcessed_ || !alignPrimed_) {
        alignAmount_ = clamped01;
    }
    alignPrimed_ = true;
}

double Engine::ratioForVoice(const Voice& v, const AnalysisFrame& a) const noexcept {
    if (v.ratioSource == RatioSource::IntervalFromRoot) {
        // MODE B. No pitch detection anywhere in this branch — the ratio is known the
        // instant the key goes down. This is the ~11 ms that Mode B saves.
        const int semis = v.midiNote - cfg_.rootMidiNote;
        return std::pow(2.0, semis / 12.0);
    }

    // MODE A — detachment. The played note comes out regardless of what was sung.
    //
    // Note it uses a.f0Hz WITHOUT checking a.f0IsHeld. That is deliberate: a held F0 is
    // exactly what lets a voice sail through a consonant without re-paying acquisition. A
    // consumer that needs to know the estimate is fresh should check the flag; a ratio
    // computation should not, or the instrument stutters on every /t/.
    if (a.f0Hz <= 1.0f) return 1.0;
    return midiToHz(v.midiNote) / static_cast<double>(a.f0Hz);
}

// THE MIX LOOP, templated on channel count (app-design.md §5.4 — CERTAIN, a hard
// constraint). Stereo == false is byte-for-byte the pre-Track-C mix loop: every stereo-only
// addition (pan gain computation, the second output channel) lives inside `if constexpr
// (Stereo)` and is therefore not merely inactive but ABSENT from the mono instantiation's
// compiled code, not a runtime branch it pays for. Humanization is NOT gated this way — it
// applies to both instantiations equally, exactly like the rest of the mix loop — and is
// instead engineered to be an exact algebraic no-op (multiply by 1.0, add 0.0) when
// Tuning::humanize is entirely zero. See the per-field comments below for why each one is
// bit-exact at zero, which is what keeps the "mono path stays bit-identical" property (§5.4)
// true even though this function now does substantially more work than it used to.
template <bool Stereo>
void Engine::mixImpl(const Sample* in, Sample* outL, Sample* outR, FrameCount numFrames) noexcept {
    VH_RT_SECTION();

    for (FrameCount i = 0; i < numFrames; ++i) {
        outL[i] = 0.0f;
        if constexpr (Stereo) outR[i] = 0.0f;
    }
    if (!prepared_ || numFrames > cfg_.maxBlock) return;
    everProcessed_ = true;

    // --- INPUT STAGE (app-design.md §5.5): trim, then an optional gate, applied to the
    // signal BEFORE it reaches the ring or the analyser -- "a laptop mic in a quiet room
    // still gives YIN room tone to chew on". Writing the STAGED signal into the ring
    // (rather than the raw `in`) means the trim/gate affects pitch tracking too, which is
    // the point: a gated-down noise floor is a cleaner F0 estimate, not merely a quieter
    // one. The staged signal doubles as the DRY signal for the output mix at the bottom
    // of this function -- see inputStageBuf_'s own comment in engine.hpp for why one
    // buffer serves both reads.
    //
    // GATE SHAPE: a smoothstep (blend.hpp's own C1 crossfade curve, reused here for the
    // identical reason -- HANDOVER.md's rule that every crossfade in this codebase is at
    // least C1 applies to a gate exactly as it applies to a note release) of a one-pole
    // envelope follower across a soft knee straddling the threshold, NOT a binary
    // decision plus a separate ramp. A switched gate steps from full gain to zero between
    // two samples, which is broadband and clicks (voice.hpp's envPhase comment names the
    // same failure mode for a hard-cut note-off); this has no branch on the envelope's
    // position at all, only on inputGateOn itself, and that branch selects between "1.0f
    // literal" and "the smoothstep formula", never between two different signal paths.
    //
    // PROVISIONAL, like every other un-auditioned millisecond constant in this codebase:
    // the two time constants and the knee width. Not exposed via Tuning -- app-design.md
    // §4.2's sketch gives the gate exactly one live parameter (inputGateDb) plus the
    // enable flag, the same way PSOLA's kHandoverMs was a bare constant before it was
    // promoted (app-design.md §5.7 names that as a followup, not this track's job).
    {
        constexpr float kGateAttackMs = 3.0f;    // envelope rise: fast enough to open on
                                                  // a consonant onset without chopping it.
        constexpr float kGateReleaseMs = 80.0f;  // envelope fall: slow enough that a
                                                  // legato phrase's brief dips do not
                                                  // chatter the gate open and shut.
        constexpr float kGateKneeDb = 6.0f;      // width of the soft-knee transition band
                                                  // straddling inputGateDb.

        const float trimLin = std::pow(10.0f, cfg_.inputTrimDb * (1.0f / 20.0f));

        // Coefficients and knee bounds are BLOCK-CONSTANT (sampleRate and cfg_ do not
        // change mid-block), computed once here rather than cached across blocks --
        // matching this function's existing precedent of recomputing attackStep/
        // releaseStep fresh every block below, rather than introducing a second place
        // (prepare()) that must be kept in sync with a Tuning-driven live field.
        const float gateAttackCoeff = std::exp(
            -1.0f / (kGateAttackMs * 0.001f * static_cast<float>(cfg_.sampleRate)));
        const float gateReleaseCoeff = std::exp(
            -1.0f / (kGateReleaseMs * 0.001f * static_cast<float>(cfg_.sampleRate)));
        const float threshLin = std::pow(10.0f, cfg_.inputGateDb * (1.0f / 20.0f));
        const float kneeLoLin = std::pow(10.0f, (cfg_.inputGateDb - kGateKneeDb) * (1.0f / 20.0f));
        const float kneeSpan = std::max(threshLin - kneeLoLin, 1e-9f);

        for (FrameCount k = 0; k < numFrames; ++k) {
            const float trimmed = in[k] * trimLin;

            // Envelope follower, updated regardless of inputGateOn -- see gateEnv_'s
            // comment in engine.hpp for why.
            const float rectified = std::fabs(trimmed);
            const float coeff = rectified > gateEnv_ ? gateAttackCoeff : gateReleaseCoeff;
            gateEnv_ = rectified + (gateEnv_ - rectified) * coeff;

            // At inputGateOn == false this is the literal 1.0f, not merely close to it --
            // `trimmed * 1.0f` is bit-exact, which is what keeps a default Tuning a
            // provable no-op through this stage regardless of what gateEnv_ happens to be
            // doing underneath.
            float gateGain = 1.0f;
            if (cfg_.inputGateOn) {
                const float x = (gateEnv_ - kneeLoLin) / kneeSpan;
                gateGain = x <= 0.0f ? 0.0f : (x >= 1.0f ? 1.0f : x * x * (3.0f - 2.0f * x));
            }
            inputStageBuf_[k] = trimmed * gateGain;
        }
    }

    ring_->write(inputStageBuf_.data(), numFrames);
    if (analyzer_) analyzer_->process(*ring_, ring_->writePos());
    const AnalysisFrame& a = analysis();

    const IBlendPolicy* policy = blend_ ? blend_ : &defaultBlend_;

    // Envelope slew per sample. Was a single hardcoded 8 ms constant used for both attack
    // and release; live via Tuning::attackMs/releaseMs -> EngineConfig, one step each so
    // an asymmetric envelope (fast attack, slow release, or vice versa) is expressible.
    // Defaults still both 8 ms, so a default Tuning changes neither direction.
    const float attackStep = static_cast<float>(1.0 / (std::max(cfg_.attackMs, 0.001f) * 0.001 * cfg_.sampleRate));
    const float releaseStep = static_cast<float>(1.0 / (std::max(cfg_.releaseMs, 0.001f) * 0.001 * cfg_.sampleRate));

    // alignAmount_ ramp toward alignTarget_ (app-design.md §4.5 item 5 / §5's resolution).
    // A no-op for every existing offline tool: they call setBlendAlignment() exactly once,
    // before any process() call, which takes the SNAP branch in setAlignmentTarget() and
    // leaves alignAmount_ == alignTarget_ from then on — so this comparison is false on
    // every single block they ever render. Only a live change made while audio is already
    // flowing (everProcessed_ already true) leaves the two apart and takes this ramp.
    if (alignAmount_ != alignTarget_) {
        const float step = static_cast<float>(
            static_cast<double>(numFrames) / std::max(1e-6, 0.050 * cfg_.sampleRate));
        if (alignAmount_ < alignTarget_) alignAmount_ = std::min(alignTarget_, alignAmount_ + step);
        else                              alignAmount_ = std::max(alignTarget_, alignAmount_ - step);
    }

    const FrameCount lf = fast_[0] ? fast_[0]->latencySamples() : 0;
    const FrameCount lq = quality_[0] ? quality_[0]->latencySamples() : 0;
    // Delay applied to the FAST path so it lines up with the slower one. See
    // setBlendAlignment: at 0 this is zero and the engines are deliberately misaligned.
    const FrameCount alignDelay = (lq > lf)
        ? static_cast<FrameCount>(alignAmount_ * static_cast<float>(lq - lf))
        : 0;

    for (int i = 0; i < static_cast<int>(voices_.size()); ++i) {
        Voice& v = voices_[i];
        if (!v.active()) continue;

        // HISTORY GATE. A voice may not be processed until its cursor genuinely lags the
        // write head by the amount the slowest engine needs.
        //
        // THE BUG THIS FIXES, and it is a nasty one because it never heals: noteOn sets
        // cursor.next = noteOnPos - lookback, but Pos is unsigned, so pressing a key
        // before the ring holds `lookback` samples clamps the cursor to 0. From then on
        // the cursor and the write head advance in lockstep, one block apart — the lag
        // stays at one block FOREVER. PSOLA, starved of the lookahead it needs to build a
        // grain, fails its residency checks and emits SILENCE for the whole life of that
        // voice. Measured as 220 dB frame-to-frame drops in the rendered output.
        //
        // It bites exactly where you would least want it to: the first chord of a session,
        // pressed before any audio has been sung.
        //
        // Skipping the voice (rather than advancing it) lets writePos pull away until the
        // lag is correct. The voice starts a few tens of milliseconds late, which is not a
        // compromise — you cannot pitch-shift audio you have not heard yet.
        FrameCount need = 0;
        if (fast_[i]) need = std::max(need, fast_[i]->latencySamples());
        if (quality_[i]) need = std::max(need, quality_[i]->latencySamples());
        if (!v.cursor.frozen && ring_->writePos() - v.cursor.next < need) continue;

        // ONSET JITTER GATE (app-design.md §5.2, Humanization::onsetJitterMs). Same shape
        // as the history gate immediately above — skip the voice entirely, do not advance
        // its cursor — until the write head reaches the jittered start position. At the
        // default jitter of 0, startAtPos == noteOnPos, and writePos() has already moved
        // past noteOnPos by the time this runs (noteOn captures noteOnPos BEFORE that
        // block's ring_->write() call above), so this never fires — exactly today's
        // behaviour, no gate existed before this track and none is added in the default
        // case.
        if (!v.cursor.frozen && ring_->writePos() < v.startAtPos) continue;

        // --- Humanization: block-rate updates (app-design.md §5.2) -----------------------
        //
        // Vibrato and detune drift are updated once per BLOCK, not per sample — both are
        // slow relative to a 128-sample block (~2.7 ms at 48 kHz), and HANDOVER.md's rule
        // ("a per-sample transcendental needs a reason") argues against paying a sin() per
        // sample per voice for movement this gradual. PROVISIONAL: at a 128-sample block
        // this quantises a 6 Hz vibrato to ~60 steps/cycle, which is probably inaudible; if
        // it is not, interpolate the ratio within the block instead — the shifters already
        // tolerate a per-block ratio update (Mode A recomputes from F0 every block), so
        // this would not need any shifter-side change, only a finer-grained loop here.
        double humRatioCents = static_cast<double>(v.hum.detuneCents);

        // Slow random walk, one-pole smoothed, clamped to +-amplitude. Starts at exactly
        // 0.0f (set in noteOn) and the recurrence below KEEPS it exactly 0.0f, bit for bit,
        // for as long as detuneDriftCents is exactly 0.0f: `0 += k*(x*0 - 0)` is 0 for any
        // finite x, and `clamp(0, -0, 0)` is 0. That is what lets a humanization-off render
        // stay bit-identical without a branch here — the RNG still advances every block
        // (a few integer ops, not audible cost) but never influences the output.
        v.humDriftRng = v.humDriftRng * 1664525u + 1013904223u;
        const float whiteStep = static_cast<float>(v.humDriftRng >> 8) / 8388608.0f - 1.0f;  // [-1,1)
        constexpr float kDriftPole = 0.02f;   // PROVISIONAL: untested by ear.
        v.humDriftValue += kDriftPole * (whiteStep * v.hum.detuneDriftCents - v.humDriftValue);
        v.humDriftValue = std::clamp(v.humDriftValue, -v.hum.detuneDriftCents, v.hum.detuneDriftCents);
        humRatioCents += static_cast<double>(v.humDriftValue);

        // Vibrato: r *= 2^(depth * sin(phase) / 1200), phase advanced once per block.
        // Phase is randomised PER VOICE at noteOn (voice.hpp:56, "the entire point") and
        // keeps advancing even at zero rate/depth (the multiply below is by exactly 0.0f,
        // an exact no-op) so that re-engaging depth later resumes a phase that has been
        // running continuously rather than snapping back to its start.
        humRatioCents += static_cast<double>(v.hum.vibratoDepthCents) *
                         std::sin(static_cast<double>(v.hum.vibratoPhase));
        v.hum.vibratoPhase += static_cast<float>(
            2.0 * kPi * static_cast<double>(v.hum.vibratoRateHz) *
            (static_cast<double>(numFrames) / cfg_.sampleRate));
        v.hum.vibratoPhase = std::fmod(v.hum.vibratoPhase, static_cast<float>(2.0 * kPi));
        if (v.hum.vibratoPhase < 0.0f) v.hum.vibratoPhase += static_cast<float>(2.0 * kPi);

        // detuneCents | detuneDriftCents | vibrato all fold into one multiplier here.
        // pow(2, 0/1200) == 1.0 exactly (IEEE-754), so at all-zero humanization this line
        // multiplies v.ratio by exactly 1.0 — bit-identical to `v.ratio = ratioForVoice(...)`
        // alone, which is what this line replaced.
        v.ratio = ratioForVoice(v, a) * std::pow(2.0, humRatioCents / kCentsPerOctave);

        BlendContext ctx{};
        ctx.ratio = v.ratio;
        ctx.sourceF0Hz = a.f0Hz;
        ctx.targetF0Hz = static_cast<float>(midiToHz(v.midiNote));
        ctx.shiftSemitones = static_cast<float>(12.0 * std::log2(v.ratio > 1e-9 ? v.ratio : 1e-9));
        ctx.samplesSinceNoteOn = static_cast<float>(ring_->writePos() - v.noteOnPos);
        ctx.voicing = a.voicing;
        ctx.confidence = a.confidence;
        ctx.sampleRate = cfg_.sampleRate;

        BlendWeights w = policy->weightsFor(ctx);
        w.normalize();

        ShiftRequest req{};
        req.ring = ring_.get();
        req.analysis = &a;
        req.cursor = &v.cursor;
        req.preservation = &cfg_.preservation;
        req.ratio = v.ratio;
        // envelopeWarpOffset -> muScale (app-design.md §5.2), read by PSOLA only, after
        // muFor(). At the default 0.0f offset this is exactly 1.0 — see ShiftRequest's own
        // comment on muScale for why that keeps PSOLA's mu computation bit-identical.
        req.muScale = 1.0 + static_cast<double>(v.hum.envelopeWarpOffset);
        req.numFrames = numFrames;

        // Below -80 dB an engine costs full CPU and contributes nothing audible. With 16
        // voices and two engines that is 32 shifters running to produce silence, so the
        // blend policy doubles as a CPU governor.
        constexpr float kAudible = 1e-4f;
        const bool runFast = w.fast > kAudible && fast_[i] != nullptr;
        const bool runQuality = w.quality > kAudible && quality_[i] != nullptr;

        // RE-ENTRY. A stateful shifter that was skipped has stale internals — a grain
        // phase, an OLA accumulator, a crossfade position — all relating to input from
        // before the gap. Resuming from them produces a burst of unrelated audio exactly
        // at the moment the engine fades back in, which is the worst possible timing.
        if (runFast && !v.fastRan && fast_[i]) fast_[i]->reset();
        if (runQuality && !v.qualityRan && quality_[i]) quality_[i]->reset();
        v.fastRan = runFast;
        v.qualityRan = runQuality;

        // Both engines read the same span, so only one may advance the shared cursor.
        const ReadCursor cursorBefore = v.cursor;

        if (runFast) {
            req.out = fastBuf_.data();
            fast_[i]->process(req);
        }
        if (runQuality) {
            ReadCursor tmp = cursorBefore;
            ShiftRequest qreq = req;
            qreq.cursor = &tmp;
            qreq.out = qualityBuf_.data();
            quality_[i]->process(qreq);
            if (!runFast) v.cursor = tmp;
        }
        if (!runFast && !runQuality) v.cursor.next += numFrames;

        // Equal-power (sin/cos) pan gains (app-design.md §5.3), computed once per voice per
        // block — pan is static within a block, so there is no reason to pay a sin/cos per
        // sample. Exists ONLY in the Stereo instantiation: `if constexpr` means the mono
        // build does not merely skip this at runtime, the code is not there at all, which
        // is what "byte-for-byte the current code path" (§5.4) requires.
        //
        // OPPOSITE LAW from blend.hpp's equal-gain crossfade normalisation just above, and
        // deliberately so — the two arguments are consistent, not contradictory. blend.hpp
        // (BlendWeights::normalize) uses equal-GAIN because the fast and quality engines
        // process the SAME voice with the SAME ratio and are therefore strongly correlated;
        // equal-power there would bump +3 dB mid-crossfade. Voices spread across the stereo
        // field are at DIFFERENT pitches with DIFFERENT decorrelation delays — genuinely
        // uncorrelated — so equal-power is the correct law here, and equal-gain would leave
        // an audible dip at the centre of the field instead of a bump nowhere.
        float panGainL = 1.0f, panGainR = 1.0f;
        if constexpr (Stereo) {
            const float pan = std::clamp(v.hum.pan, -1.0f, 1.0f);
            const float angle = (pan + 1.0f) * 0.25f * static_cast<float>(kPi);   // 0..pi/2
            panGainL = std::cos(angle);
            panGainR = std::sin(angle);
        }

        // Alignment delay on the fast path. Always written to, even at zero delay, so the
        // line stays primed and changing alignAmount_ mid-performance does not read
        // silence.
        // Decorrelation line for this voice. Zero delay when the policy is plain
        // PassThrough, which is retained as the A/B control for VH-005.
        Sample* dline = decorBuf_.data() + static_cast<FrameCount>(i) * decorStride_;
        const FrameCount dmask = decorStride_ - 1;
        FrameCount dw = decorWrite_[static_cast<size_t>(i)];
        const FrameCount decorDelay =
            (cfg_.preservation.unvoiced == UnvoicedPolicy::PassThroughDecorrelated)
                ? static_cast<FrameCount>(v.hum.staticDelayMs * 0.001 * cfg_.sampleRate)
                : 0;

        Sample* aline = alignBuf_.data() + static_cast<FrameCount>(i) * alignStride_;
        const FrameCount amask = alignStride_ - 1;
        FrameCount aw = alignWrite_[static_cast<size_t>(i)];

        for (FrameCount k = 0; k < numFrames; ++k) {
            const Sample fastIn = runFast ? fastBuf_[k] : 0.0f;
            aline[aw & amask] = fastIn;
            const Sample fastOut = alignDelay ? aline[(aw - alignDelay) & amask] : fastIn;
            ++aw;

            const Sample q = runQuality ? qualityBuf_[k] : 0.0f;

            // Envelope slews toward target; a released voice retires only once silent.
            //
            // The RAMP is linear; the applied GAIN is a raised cosine of it, so the gain is
            // C1 at both ends. Ramping the gain itself was C0: its derivative stepped from
            // zero to full slope at the start of every attack and every release, which is a
            // kink, and a kink clicks however long the ramp is. This is the same rule the
            // rest of the codebase already follows and the one place that did not.
            // Recomputed ONLY while the envelope is actually moving. A sustained voice
            // sits at phase 1 for seconds at a time and a transcendental per sample per
            // voice is not free: 16 voices at a 128-sample block is 2048 cosines that
            // would all return the same number. Measured at 4 ms of ramp per note against
            // seconds of sustain, this branch is taken essentially never.
            if (v.envPhase != v.envTarget) {
                // Rising toward envTarget (envTarget == 1, an attack) uses attackStep;
                // falling toward it (envTarget == 0, a release) uses releaseStep.
                if (v.envPhase < v.envTarget) v.envPhase = std::min(v.envTarget, v.envPhase + attackStep);
                else                          v.envPhase = std::max(v.envTarget, v.envPhase - releaseStep);
                v.envGain = static_cast<float>(0.5 - 0.5 * std::cos(kPi * v.envPhase));
            }

            // PER-VOICE STATIC DELAY, applied AFTER the blend so both engines receive the
            // identical treatment. Applying it inside a shifter would give the fast and
            // quality paths different offsets and comb them against each other, which is
            // the opposite of the intent. See Humanization::staticDelayMs for why a delay
            // and not an allpass.
            Sample mixed = fastOut * w.fast + q * w.quality;
            if (decorDelay > 0) {
                dline[dw & dmask] = mixed;
                mixed = dline[(dw - decorDelay) & dmask];
            }
            ++dw;

            // MONO: out[k] += mixed * v.gain * v.envGain -- unchanged, bit-for-bit, from
            // the pre-Track-C engine. STEREO: the same voice gain summed through the
            // equal-power pan gains above; see app-design.md §5.3.
            if constexpr (Stereo) {
                const float g = mixed * v.gain * v.envGain;
                outL[k] += g * panGainL;
                outR[k] += g * panGainR;
            } else {
                outL[k] += mixed * v.gain * v.envGain;
            }
        }
        alignWrite_[static_cast<size_t>(i)] = aw;
        decorWrite_[static_cast<size_t>(i)] = dw;

        // FREEZE. The cursor loops within the captured window instead of chasing the write
        // head. Note there is no branch anywhere else in this function for it, and none at
        // all in AudioRing or the shifters — the whole feature is these six lines.
        if (v.cursor.frozen && v.cursor.freezeEnd > v.cursor.freezeBegin) {
            if (v.cursor.next >= v.cursor.freezeEnd) {
                // Restart at a randomised offset rather than exactly at freezeBegin.
                // Looping a fixed window at a fixed period is audible as a buzz within
                // about a second — the ear locks onto the loop rate. Jittering the restart
                // scatters it into something that reads as a sustained voice.
                const Pos span = v.cursor.freezeEnd - v.cursor.freezeBegin;
                v.cursor.rngState = v.cursor.rngState * 1664525u + 1013904223u;
                const Pos jitter = static_cast<Pos>((v.cursor.rngState >> 16) % 512u);
                v.cursor.next = v.cursor.freezeBegin + (jitter < span ? jitter : 0);
            }
            if (v.cursor.next < ring_->oldestPos()) {
                // The frozen window has fallen off the back of the history. You cannot
                // hold a note forever; fade out rather than read garbage.
                v.envTarget = 0.0f;
                v.state = VoiceState::Releasing;
            }
        }

        if (v.state == VoiceState::Attacking) v.state = VoiceState::Sustaining;
        if (v.state == VoiceState::Releasing && v.envGain <= 1e-6f) {
            v.state = VoiceState::Idle;
            v.envGain = 0.0f;
        }
    }

    // --- OUTPUT STAGE (app-design.md §5.5): dry/wet, then a feedforward peak limiter. ---
    //
    // ORDER, per §5.5: per-voice gain*envGain*velocityGain summed through pan (the loop
    // above -- Track C's, untouched) -> dry/wet -> master gain -> limiter. There is no
    // Tuning field named "master gain" distinct from wetGain (Tuning has exactly
    // inputTrimDb/inputGateOn/inputGateDb/dryGain/wetGain/outCeilingDb/limiterOn -- see
    // tuning.hpp), so this reads §5.5's three-stage description as two: wetGain IS the
    // wet bus's own level control, there being no separate overall bus trim in the struct
    // this track was handed. If a distinct master-gain field is wanted later, it slots in
    // between this comment and the limiter below without disturbing anything else here.
    //
    // outL/outR currently hold the WET sum only (voices, already through pan). `dry` is
    // this block's own staged input, from inputStageBuf_ above -- NOT delay-matched to
    // the wet signal. app-design.md §5.5 is explicit that this is deliberate: the player
    // hears themselves acoustically at zero latency regardless of anything this engine
    // does, so aligning a copy of the input against the wet signal would be aligning
    // against a reference the ear is not using. Using this block's own dry samples at
    // this block's own indices is therefore not an approximation of the "correct" delay
    // — it is the zero-latency answer the spec asks for.
    //
    // At Tuning{}'s defaults (dryGain=0, wetGain=1, limiterOn=false) every line below is
    // an algebraic no-op: `wet*1.0f` is bit-exact, `dry*0.0f` is bit-exact zero for any
    // finite dry sample, and the limiter multiply is skipped by the `if` rather than
    // applied at gain 1.0 -- three independent reasons the governing property (a default
    // Tuning changes no rendered sample) holds through this stage, not just one.
    {
        // LIMITER: feedforward peak, NOT lookahead (app-design.md §5.5 -- "lookahead is
        // latency, and latency is the thing this project spends everything else to
        // avoid"). TWO stages, matching §5.5's own wording exactly -- "track a peak
        // envelope, compute a gain reduction ..., smooth the gain reduction itself with
        // the attack/release times (not just the envelope)":
        //
        //   1. peakEnv_: a peak-HOLD detector. Rises INSTANTLY to a new true peak (no
        //      lookahead means no advance warning, so it must react the moment a peak
        //      arrives) and otherwise decays at the release time constant. See
        //      peakEnv_'s comment in engine.hpp for why this cannot be the raw
        //      instantaneous |sample| -- on periodic material the true crest is a
        //      vanishing fraction of each cycle, so an undamped detector hands the gain
        //      smoother below a target that disappears before it can converge onto it,
        //      which is a real defect this file's own test caught (a "steady-state"
        //      limited peak that stayed roughly 2x over ceiling indefinitely, not just
        //      during the attack).
        //   2. limiterGain_: a one-pole toward the gain rawGain implies, with different
        //      coefficients engaging (attack) vs releasing (release) -- the smoothing
        //      that keeps the APPLIED gain C-infinity, so the reduction itself never
        //      zippers or clicks, independent of how peakEnv_ moves.
        //
        // STEREO-LINKED: one shared gain-reduction value derived from whichever channel
        // peaks higher this sample, applied to both. An UNLINKED limiter (each channel
        // computing its own reduction) would narrow the stereo image on a transient that
        // hits only one channel -- exactly the kind of side effect a limiter protecting
        // against clipping should not itself introduce. In the mono instantiation the
        // second channel is always silent (r stays 0), so this collapses to a plain
        // single-channel peak with no special-casing needed.
        //
        // PROVISIONAL, like every other un-auditioned millisecond constant in this
        // codebase: the attack/release figures themselves. app-design.md §5.5 gives
        // "~1 ms attack / ~50 ms release" as the starting point; these are that verbatim,
        // reused for BOTH the detector's release and the gain smoother's release, so
        // there is one release figure in the whole stage rather than two that could
        // silently drift apart -- see this loop's own comment on why the detector's slow
        // release is what makes the gain smoother's fast attack able to converge at all.
        constexpr float kLimiterAttackMs = 1.0f;
        constexpr float kLimiterReleaseMs = 50.0f;

        const float ceilingLin = std::pow(10.0f, cfg_.outCeilingDb * (1.0f / 20.0f));
        const float limAttackCoeff = std::exp(
            -1.0f / (kLimiterAttackMs * 0.001f * static_cast<float>(cfg_.sampleRate)));
        const float limReleaseCoeff = std::exp(
            -1.0f / (kLimiterReleaseMs * 0.001f * static_cast<float>(cfg_.sampleRate)));

        for (FrameCount k = 0; k < numFrames; ++k) {
            const Sample dry = inputStageBuf_[k];
            Sample l = outL[k] * cfg_.wetGain + dry * cfg_.dryGain;
            Sample r = 0.0f;
            if constexpr (Stereo) r = outR[k] * cfg_.wetGain + dry * cfg_.dryGain;

            // Detector and gain tracking run UNCONDITIONALLY, same "always track, only
            // apply when enabled" reasoning as the gate above (see limiterGain_'s comment
            // in engine.hpp) -- so enabling the limiter mid-performance reacts to the
            // signal happening right now, not to whatever state was last held from
            // before it was turned off.
            const float instPeak = std::max(std::fabs(l), std::fabs(r));
            peakEnv_ = instPeak > peakEnv_ ? instPeak : instPeak + (peakEnv_ - instPeak) * limReleaseCoeff;

            const float rawGain = peakEnv_ > ceilingLin ? (ceilingLin / peakEnv_) : 1.0f;
            const float coeff = rawGain < limiterGain_ ? limAttackCoeff : limReleaseCoeff;
            limiterGain_ = rawGain + (limiterGain_ - rawGain) * coeff;

            if (cfg_.limiterOn) {
                l *= limiterGain_;
                if constexpr (Stereo) r *= limiterGain_;
            }

            outL[k] = l;
            if constexpr (Stereo) outR[k] = r;
        }
    }
}

void Engine::process(const Sample* in, Sample* out, FrameCount numFrames) noexcept {
    mixImpl<false>(in, out, nullptr, numFrames);
}

void Engine::process(const Sample* in, Sample* outL, Sample* outR, FrameCount numFrames) noexcept {
    mixImpl<true>(in, outL, outR, numFrames);
}

// Explicit instantiation: mixImpl is defined only here, and these are the only two
// instantiations that will ever exist (one bool template parameter, two call sites just
// above). Keeps the definition out of engine.hpp, matching every other method in this
// class, rather than forcing the mix loop's implementation into a public header.
template void Engine::mixImpl<false>(const Sample*, Sample*, Sample*, FrameCount) noexcept;
template void Engine::mixImpl<true>(const Sample*, Sample*, Sample*, FrameCount) noexcept;

const IBlendPolicy* Engine::policyForKind(BlendKind k) noexcept {
    switch (k) {
        case BlendKind::FastOnly:      return &tuningFastOnly_;
        case BlendKind::QualityOnly:   return &tuningQualityOnly_;
        case BlendKind::RegisterSplit: return &tuningRegisterSplit_;
        case BlendKind::OnsetHandover: return &tuningOnsetHandover_;
    }
    // Unreachable for any valid BlendKind; a hand-edited profile could still deliver an
    // out-of-range enum value through Tuning (it is a POD, nothing stops a raw integer
    // being written into the field's storage by a loader bug). Fail toward the same
    // default a fresh Engine starts with rather than reading past the switch.
    return &tuningFastOnly_;
}

void Engine::applyTuning(const Tuning& t) noexcept {
    VH_RT_SECTION();

    // Clamp a COPY, never the caller's struct — app-design.md §4.5's closing rule.
    Tuning c = t;
    c.clamp();

    if (analyzer_) analyzer_->setTuning(c.analyzer);
    for (auto& s : fast_)    if (s) s->setTuning(c.shifters);
    for (auto& s : quality_) if (s) s->setTuning(c.shifters);

    // Blend-policy parameters are kept current on the owned instances REGARDLESS of
    // whether a custom policy is active right now, so that switching back to enum-driven
    // selection later (setBlendPolicy(nullptr)) picks up whatever was most recently
    // published rather than stale construction-time defaults.
    tuningRegisterSplit_.crossoverHz = c.rsCrossoverHz;
    tuningRegisterSplit_.widthOctaves = c.rsWidthOctaves;
    tuningOnsetHandover_.handoverStartMs = c.ohStartMs;
    tuningOnsetHandover_.handoverEndMs = c.ohEndMs;
    if (!customBlendPolicy_) blend_ = policyForKind(c.kind);

    cfg_.ratioSource = c.mode;
    cfg_.rootMidiNote = c.rootMidiNote;
    cfg_.preservation = c.preservation;
    cfg_.attackMs = c.attackMs;
    cfg_.releaseMs = c.releaseMs;
    cfg_.freezeWindowMs = c.freezeWindowMs;
    cfg_.decorMinMs = c.decorMinMs;
    cfg_.decorMaxMs = c.decorMaxMs;

    // app-design.md §5.2/§5.6, Track C: the ensemble spread, the runtime voice cap, and the
    // velocity->gain curve are all consumed at noteOn (see Engine::noteOn) rather than every
    // block, so copying the struct fields here is sufficient — there is no derived,
    // prepare()-baked value to recompute the way PsolaTuning::handoverMs needs (app-design.md
    // §4.1). A voice already sounding when one of these changes keeps whatever values its
    // own noteOn derived; only the NEXT noteOn sees the new spread/cap/curve. That is a
    // deliberate reading of "per-voice values derived at noteOn" — re-deriving a running
    // voice's humanization mid-note would make its detune/vibrato/pan jump, which is a worse
    // discontinuity than a knob turned mid-chord only affecting new notes.
    cfg_.humanize = c.humanize;
    cfg_.voiceCap = c.voiceCap;
    cfg_.velocityToGainMin = c.velocityToGainMin;
    cfg_.velocityCurve = c.velocityCurve;

    // c.alignment is already clamped to [0,1] by Tuning::clamp() above, so
    // setAlignmentTarget()'s own clamp is redundant here, not incorrect. Routed through the
    // shared snap-vs-ramp helper (app-design.md §4.5 item 5) rather than writing
    // alignAmount_ directly, exactly like setBlendAlignment() above — see that function's
    // doc comment in engine.hpp for why a snap-then-ramp split is the resolution rather
    // than ramping unconditionally.
    setAlignmentTarget(c.alignment);

    // Input/output stage (app-design.md §5.5, Track D): consumed every block by mixImpl(),
    // not derived into anything prepare()-baked, so a plain field copy is sufficient --
    // same shape as attackMs/releaseMs/decorMinMs/decorMaxMs above, and unlike
    // shifters.psola.handoverMs there is no mixStep_-style cached derivative here that
    // would need recomputing for the new value to take effect (app-design.md §4.1).
    cfg_.inputTrimDb = c.inputTrimDb;
    cfg_.inputGateOn = c.inputGateOn;
    cfg_.inputGateDb = c.inputGateDb;
    cfg_.dryGain = c.dryGain;
    cfg_.wetGain = c.wetGain;
    cfg_.outCeilingDb = c.outCeilingDb;
    cfg_.limiterOn = c.limiterOn;

    // NOT applied by this track, and deliberately so -- read by nothing yet:
    //   - c.glideSemisPerSec: no portamento implementation reads Voice::
    //     glideRateSemitonesPerSec anywhere in this file today; wiring the FIELD without
    //     wiring the BEHAVIOUR would be exactly the "knob that does nothing" §5.8 warns
    //     against, so it stays unread until glide itself is built.
}

} // namespace vh
