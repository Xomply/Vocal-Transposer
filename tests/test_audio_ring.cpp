#include <doctest/doctest.h>

#include "vh/audio_ring.hpp"

#include <numeric>
#include <vector>

using namespace vh;

namespace {
std::vector<Sample> ramp(FrameCount n, Sample start = 0) {
    std::vector<Sample> v(n);
    std::iota(v.begin(), v.end(), start);
    return v;
}
} // namespace

TEST_CASE("capacity rounds up to a power of two") {
    AudioRing r(1000);
    CHECK(r.capacity() == 1024);
}

TEST_CASE("absolute positions survive wraparound") {
    // The whole addressing scheme rests on this. If positions were block-relative or the
    // wrap were mishandled, a reader lagging across the wrap boundary would silently get
    // the wrong audio — which is audible as a click every capacity/samplerate seconds,
    // i.e. rare enough to be blamed on the driver.
    AudioRing r(16);
    const auto a = ramp(16, 0);
    const auto b = ramp(16, 100);
    r.write(a.data(), 16);
    r.write(b.data(), 16);

    CHECK(r.writePos() == 32);
    CHECK(r.oldestPos() == 16);

    std::vector<Sample> got(16);
    REQUIRE(r.read(16, got.data(), 16));
    CHECK(got[0] == doctest::Approx(100));
    CHECK(got[15] == doctest::Approx(115));
}

TEST_CASE("reads of overwritten material fail loudly rather than returning garbage") {
    AudioRing r(16);
    const auto a = ramp(16, 0);
    r.write(a.data(), 16);
    r.write(a.data(), 16);

    std::vector<Sample> got(4, 999.0f);
    CHECK_FALSE(r.read(0, got.data(), 4));       // position 0 is long gone
    // Silence, not stale audio: a caller that ignores the return value gets a gap, which
    // is recoverable, rather than a burst of unrelated audio, which is not.
    CHECK(got[0] == doctest::Approx(0.0f));
}

TEST_CASE("reads of not-yet-written material fail") {
    AudioRing r(64);
    const auto a = ramp(8);
    r.write(a.data(), 8);
    std::vector<Sample> got(8);
    CHECK_FALSE(r.read(4, got.data(), 8));       // would run past the write head
}

TEST_CASE("freeze needs no support from the ring") {
    // This test exists to pin the architectural claim in audio_ring.hpp: a frozen voice
    // is nothing but a cursor that stopped advancing. If someone later adds freeze
    // handling *into* the ring, this test still passes — but the comment it defends will
    // have become a lie, so the test name is the documentation.
    AudioRing r(1024);
    const auto a = ramp(256, 0);
    r.write(a.data(), 256);

    const Pos frozenAt = 100;
    std::vector<Sample> first(8), second(8);
    REQUIRE(r.read(frozenAt, first.data(), 8));

    const auto more = ramp(256, 1000);
    r.write(more.data(), 256);

    REQUIRE(r.read(frozenAt, second.data(), 8));
    for (int i = 0; i < 8; ++i) CHECK(first[i] == doctest::Approx(second[i]));
}
