#include <doctest/doctest.h>

#include "vh/audio_ring.hpp"
#include "vh/rt.hpp"

#include <vector>

using namespace vh;

TEST_CASE("the guard detects an allocation inside a real-time section") {
    // Meta-test. A safety net that does not actually catch anything is worse than none,
    // because it produces confidence without evidence. This proves the net has holes of
    // the right size before any other test relies on it.
    rt::resetViolationCount();
    {
        VH_RT_SECTION();
        std::vector<float> naughty(64);   // exactly the sort of thing we are hunting
        (void)naughty;
    }
    CHECK(rt::violationCount() > 0);
    rt::resetViolationCount();
}

TEST_CASE("the guard is silent outside a real-time section") {
    rt::resetViolationCount();
    std::vector<float> fine(1024);
    (void)fine;
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("the suspend hatch works and restores") {
    rt::resetViolationCount();
    {
        VH_RT_SECTION();
        {
            rt::RtScopeSuspend allowThisOne;
            std::vector<float> deliberate(16);
            (void)deliberate;
        }
        CHECK(rt::violationCount() == 0);
        std::vector<float> accidental(16);   // outside the hatch — should be caught
        (void)accidental;
    }
    CHECK(rt::violationCount() > 0);
    rt::resetViolationCount();
}

TEST_CASE("AudioRing read and write allocate nothing") {
    AudioRing r(4096);
    std::vector<Sample> in(128, 0.5f), out(128);
    rt::resetViolationCount();
    for (int i = 0; i < 100; ++i) {
        r.write(in.data(), 128);
        r.read(r.writePos() - 128, out.data(), 128);
    }
    CHECK(rt::violationCount() == 0);
}
