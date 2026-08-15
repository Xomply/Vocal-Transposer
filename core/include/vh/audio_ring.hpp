// vh/audio_ring.hpp — one writer (the audio callback), many readers at independent lags.
//
// SEED: every voice needs to read the singer's recent audio, but not the same part of it.
// The fast engine reads material ~10 ms old; the quality engine reads ~30 ms old; a
// frozen voice reads a fixed window captured seconds ago and never advances. All from
// one input stream that is being written continuously.
//
// THE DECISION THAT SHAPES EVERYTHING ELSE HERE: the ring holds *no reader state*.
//
// The obvious design gives each reader a cursor owned by the ring. That forces the ring
// to know how many readers exist, to police the slowest one, and to grow special cases
// for a reader that has stopped advancing. Instead, readers address samples by absolute
// position (vh::Pos) and keep their own cursor. The ring only answers "do you still have
// the samples from position P onward?".
//
// The payoff is that FREEZE NEEDS NO SUPPORT HERE AT ALL. A frozen voice is simply a
// voice that stops incrementing its own cursor. The most-requested feature in the design
// falls out of the addressing scheme rather than being a mode the buffer has to
// implement. If you ever find yourself adding a "frozen" concept to this file, something
// has gone wrong upstream.
//
// CONCURRENCY: one writer thread, any number of reader threads. Readers never mutate.
// Correctness rests on a single release/acquire pair on writePos_.

#pragma once

#include "vh/types.hpp"
#include "vh/rt.hpp"

#include <atomic>
#include <cassert>
#include <cstring>
#include <vector>

namespace vh {

class AudioRing {
public:
    // Capacity is rounded up to a power of two so index wrapping is a mask, not a modulo.
    // (Division on the audio thread for every sample is exactly the sort of thing that
    // turns a comfortable CPU budget into a marginal one.)
    explicit AudioRing(FrameCount minCapacity = kInputHistoryFrames) {
        FrameCount cap = 1;
        while (cap < minCapacity) cap <<= 1;
        buf_.assign(cap, Sample{0});
        mask_ = cap - 1;
    }

    FrameCount capacity() const noexcept { return buf_.size(); }

    // One past the newest sample written. Readers use this as "now".
    Pos writePos() const noexcept { return writePos_.load(std::memory_order_acquire); }

    // Oldest position still resident. Anything below this has been overwritten.
    Pos oldestPos() const noexcept {
        const Pos w = writePos();
        return w > capacity() ? w - capacity() : 0;
    }

    // --- writer side: audio thread only ---
    void write(const Sample* src, FrameCount n) noexcept {
        VH_RT_SECTION();
        assert(n <= capacity());
        const Pos w = writePos_.load(std::memory_order_relaxed);
        const FrameCount start = static_cast<FrameCount>(w & mask_);
        const FrameCount first = std::min(n, capacity() - start);
        std::memcpy(buf_.data() + start, src, first * sizeof(Sample));
        if (n > first) {
            std::memcpy(buf_.data(), src + first, (n - first) * sizeof(Sample));
        }
        // Release: everything written above is visible to any reader that acquires this.
        writePos_.store(w + n, std::memory_order_release);
    }

    // --- reader side: any thread ---

    // True if [from, from+n) is entirely resident right now.
    //
    // NOTE THE RACE THIS DOES NOT PROTECT AGAINST: the writer may overwrite `from`
    // between this check and a subsequent read. That is by design — guaranteeing
    // otherwise would need reader registration, which is the reader-state design this
    // file exists to avoid. The safety margin comes from capacity: at 2 s of history and
    // readers lagging by tens of milliseconds, the writer is ~2 s of wall-clock behind
    // catching up to any live reader. A *frozen* reader can legitimately fall off the
    // back, and that is a musical fact (you cannot hold a note forever), reported via
    // read() returning false so the voice can fade out rather than click.
    bool contains(Pos from, FrameCount n) const noexcept {
        const Pos w = writePos();
        if (from + n > w) return false;          // not written yet
        return from >= oldestPos();              // not yet overwritten
    }

    // Copy n samples starting at absolute position `from`. Returns false and writes
    // silence if the range is unavailable, so a caller that ignores the return value
    // gets a gap rather than garbage.
    bool read(Pos from, Sample* dst, FrameCount n) const noexcept {
        VH_RT_SECTION();
        if (!contains(from, n)) {
            std::memset(dst, 0, n * sizeof(Sample));
            return false;
        }
        const FrameCount start = static_cast<FrameCount>(from & mask_);
        const FrameCount first = std::min(n, capacity() - start);
        std::memcpy(dst, buf_.data() + start, first * sizeof(Sample));
        if (n > first) {
            std::memcpy(dst + first, buf_.data(), (n - first) * sizeof(Sample));
        }
        return true;
    }

    // Single-sample access, for the grain-scatter inner loops where copying a block
    // first would be wasteful. No bounds reporting: callers in that path have already
    // established residency for the whole grain.
    Sample at(Pos p) const noexcept { return buf_[static_cast<FrameCount>(p & mask_)]; }

private:
    std::vector<Sample> buf_;   // allocated once at construction, never resized
    FrameCount mask_ = 0;
    std::atomic<Pos> writePos_{0};
};

} // namespace vh
