// vh/passthrough_shifter.hpp — ratio 1.0, always. Copies input to output.
//
// WHY A DELIBERATELY USELESS SHIFTER IS THE FIRST ONE WRITTEN:
//
// 1. It is the Phase 0 deliverable. Phase 0 is "measure before you build": get audio in
//    and out through the real pipeline and loopback-measure the actual round trip. That
//    needs a shifter that provably adds nothing, so the measurement isolates the I/O and
//    host path.
//
// 2. It is the identity element, which makes it a test oracle. Any real shifter driven at
//    ratio == 1.0 should approximate this output. That is a property test you can point
//    at every future engine, and it catches a whole family of bugs (window
//    normalisation, gain staging, off-by-one in grain placement) that are otherwise only
//    audible as "hmm, something's slightly off".
//
// 3. It is the safe fallback. If the quality engine misses its deadline or the analyser
//    loses confidence entirely, a voice can degrade to this rather than to a glitch.
//    Degrading toward the dry voice reads as a mix decision; degrading toward garbage
//    reads as broken gear.

#pragma once

#include "vh/shifter.hpp"

namespace vh {

class PassthroughShifter final : public IPitchShifter {
public:
    void prepare(double, FrameCount) override {}
    void reset() noexcept override {}

    void process(const ShiftRequest& req) noexcept override {
        VH_RT_SECTION();
        req.ring->read(req.cursor->next, req.out, req.numFrames);
        // Advance by exactly the frames consumed. A frozen cursor is handled by the voice
        // layer before we get here; this shifter has no opinion about freeze.
        if (!req.cursor->frozen) req.cursor->next += req.numFrames;
    }

    FrameCount latencySamples() const noexcept override { return 0; }
    const char* name() const noexcept override { return "passthrough"; }
};

} // namespace vh
