#include "vh/engine.hpp"
#include "vh/rt.hpp"

#include <cmath>

namespace vh {
namespace {

double midiToHz(int note) noexcept {
    return 440.0 * std::pow(2.0, (note - 69) / 12.0);
}

const AnalysisFrame kEmptyFrame{};

} // namespace

Engine::Engine() : ring_(kInputHistoryFrames) { blend_ = &defaultBlend_; }
Engine::~Engine() = default;

void Engine::prepare(const EngineConfig& cfg,
                     std::unique_ptr<IAnalyzer> analyzer,
                     ShifterFactory makeFast,
                     ShifterFactory makeQuality) {
    cfg_ = cfg;
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
    FrameCount dneed = static_cast<FrameCount>(cfg_.sampleRate * 0.010) + cfg_.maxBlock;
    FrameCount dcap = 1;
    while (dcap < dneed) dcap <<= 1;
    decorStride_ = dcap;
    decorBuf_.assign(decorStride_ * static_cast<FrameCount>(kMaxVoices), Sample{0});
    decorWrite_.assign(static_cast<size_t>(kMaxVoices), 0);

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

FrameCount Engine::latencySamples() const noexcept {
    FrameCount worst = 0;
    if (analyzer_ && cfg_.ratioSource == RatioSource::AbsoluteTarget) {
        // Mode A needs F0 before it can compute a ratio; Mode B does not.
        // This asymmetry is the whole latency argument for Mode B.
        worst = analyzer_->latencySamples();
    }
    const FrameCount lf = (!fast_.empty() && fast_[0]) ? fast_[0]->latencySamples() : 0;
    const FrameCount lq = (!quality_.empty() && quality_[0]) ? quality_[0]->latencySamples() : 0;

    // The per-voice decorrelation delay is real output latency and is included, because a
    // host doing delay compensation needs the worst case, not the typical one. It is the
    // largest value noteOn can assign (see the golden-ratio spread there).
    const FrameCount decor =
        (cfg_.preservation.unvoiced == UnvoicedPolicy::PassThroughDecorrelated)
            ? static_cast<FrameCount>(cfg_.sampleRate * 0.004) : 0;

    // Parallel engines: max, not sum. The listener hears the fast one first, so the
    // FELT latency is lf; the figure reported here is when the full blend is available,
    // which is what a host needs for delay compensation.
    return worst + std::max(lf, lq) + decor;
}

int Engine::allocateVoice(int midiNote) noexcept {
    // Retriggering a note that is already sounding reuses its voice rather than starting
    // a second one. Without this, holding the sustain pedal and repeatedly striking the
    // same key stacks identical voices, which sums to something progressively louder and
    // more phase-cancelled rather than to a chord.
    for (int i = 0; i < static_cast<int>(voices_.size()); ++i) {
        if (voices_[i].active() && voices_[i].midiNote == midiNote) return i;
    }
    for (int i = 0; i < static_cast<int>(voices_.size()); ++i) {
        if (!voices_[i].active()) return i;
    }
    // Steal the oldest. PROVISIONAL: oldest-first is the conventional choice and is
    // musically defensible (the newest note is the one you just asked for). If chord
    // stacking under the sustain pedal turns out to steal notes you wanted kept, revisit
    // — a "steal the quietest" or "steal the most recently released" policy may suit
    // better.
    int oldest = 0;
    Pos oldestPos = voices_[0].noteOnPos;
    for (int i = 1; i < static_cast<int>(voices_.size()); ++i) {
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
    v.noteOnPos = ring_.writePos();

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

    // Spread the voices over 0.5-4 ms using the golden ratio, so no two delays are a
    // simple multiple of one another. Equal spacing would put every voice's comb notches
    // on the same frequencies and colour the ensemble instead of diffusing it. Derived
    // from the voice INDEX rather than randomised, so a given chord decorrelates the same
    // way every render and the measurement is reproducible.
    const double frac = std::fmod(static_cast<double>(idx) * 0.6180339887498949, 1.0);
    v.hum.staticDelayMs = static_cast<float>(0.5 + 3.5 * frac);
    v.fastRan = false;
    v.qualityRan = false;

    if (fast_[idx]) fast_[idx]->reset();
    if (quality_[idx]) quality_[idx]->reset();
}

void Engine::noteOff(int midiNote) noexcept {
    VH_RT_SECTION();
    for (auto& v : voices_) {
        if (v.active() && v.midiNote == midiNote) {
            if (sustain_) {
                // FREEZE. The voice detaches from the live input and loops the window it
                // has been reading. Note how little happens here: freeze is a cursor
                // policy, not a mode the buffer or the shifters implement.
                v.cursor.frozen = true;
                v.cursor.freezeEnd = v.cursor.next;
                const Pos span = static_cast<Pos>(cfg_.sampleRate * 0.5);  // 500 ms window
                v.cursor.freezeBegin = v.cursor.freezeEnd > span ? v.cursor.freezeEnd - span : 0;
                v.state = VoiceState::Sustaining;
            } else {
                v.state = VoiceState::Releasing;
                v.envTarget = 0.0f;
            }
        }
    }
}

void Engine::setSustain(bool held) noexcept {
    VH_RT_SECTION();
    sustain_ = held;
    if (!held) {
        for (auto& v : voices_) {
            if (v.cursor.frozen) {
                v.cursor.frozen = false;
                v.state = VoiceState::Releasing;
                v.envTarget = 0.0f;
            }
        }
    }
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

void Engine::process(const Sample* in, Sample* out, FrameCount numFrames) noexcept {
    VH_RT_SECTION();

    for (FrameCount i = 0; i < numFrames; ++i) out[i] = 0.0f;
    if (!prepared_ || numFrames > cfg_.maxBlock) return;

    ring_.write(in, numFrames);
    if (analyzer_) analyzer_->process(ring_, ring_.writePos());
    const AnalysisFrame& a = analysis();

    const IBlendPolicy* policy = blend_ ? blend_ : &defaultBlend_;

    // Envelope slew per sample. PROVISIONAL 8 ms: long enough to remove the click,
    // short enough that a staccato chord still sounds staccato.
    const float envStep = static_cast<float>(1.0 / (0.008 * cfg_.sampleRate));

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
        if (!v.cursor.frozen && ring_.writePos() - v.cursor.next < need) continue;

        v.ratio = ratioForVoice(v, a);

        BlendContext ctx{};
        ctx.ratio = v.ratio;
        ctx.sourceF0Hz = a.f0Hz;
        ctx.targetF0Hz = static_cast<float>(midiToHz(v.midiNote));
        ctx.shiftSemitones = static_cast<float>(12.0 * std::log2(v.ratio > 1e-9 ? v.ratio : 1e-9));
        ctx.samplesSinceNoteOn = static_cast<float>(ring_.writePos() - v.noteOnPos);
        ctx.voicing = a.voicing;
        ctx.confidence = a.confidence;
        ctx.sampleRate = cfg_.sampleRate;

        BlendWeights w = policy->weightsFor(ctx);
        w.normalize();

        ShiftRequest req{};
        req.ring = &ring_;
        req.analysis = &a;
        req.cursor = &v.cursor;
        req.preservation = &cfg_.preservation;
        req.ratio = v.ratio;
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
                if (v.envPhase < v.envTarget) v.envPhase = std::min(v.envTarget, v.envPhase + envStep);
                else                          v.envPhase = std::max(v.envTarget, v.envPhase - envStep);
                v.envGain = static_cast<float>(0.5 - 0.5 * std::cos(M_PI * v.envPhase));
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

            out[k] += mixed * v.gain * v.envGain;
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
            if (v.cursor.next < ring_.oldestPos()) {
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
}

} // namespace vh
