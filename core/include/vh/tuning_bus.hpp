// vh/tuning_bus.hpp — how ~60 live parameters reach the audio thread without a lock.
//
// SEED, and the problem this replaces: today Engine::setPreservation (engine.hpp:87)
// copies a struct into cfg_ with NO synchronisation at all, and the audio thread reads
// blend-policy float fields directly out of whatever object setBlendPolicy() last pointed
// at. That is "probably fine on x86" — a torn read of a float that is about to be
// overwritten with a similar float rarely produces an audible glitch — and app-design.md
// §4.3 is explicit that "probably fine" is not the bar once Tuning carries ~60 fields
// instead of one policy's few floats: a UI thread mid-write to a HumanizationSpec while
// the audio thread reads it is a genuine data race (UB, not just a click), and unlike a
// single float, a partially-updated multi-field struct can be read in a state that never
// existed at any instant on the writer side — e.g. a new detuneCents paired with an old
// vibratoDepthCents, a combination nobody published.
//
// TripleBuffer — the classic design (attributed variously to game-engine "double/triple
// buffering" and to Erik Rigtorp's SPSC writeups; there is no dependency here, just the
// well-known algorithm reimplemented): three Tuning-sized slots. At every instant exactly
// one slot is privately owned by the producer (about to be overwritten), one is privately
// owned by the consumer (being read from right now), and one sits in a shared "middle"
// slot holding whatever was most recently finished. publish() and poll() each hand their
// own private slot to the middle and take back whatever was there, via a single atomic
// RMW. Ownership never overlaps, so there is no data race on any individual Tuning's
// storage even though there is no lock anywhere in either call.
//
// WHY A SNAPSHOT CHANNEL AND NOT A QUEUE — app-design.md §11 records this as the
// PROVISIONAL choice in the whole design, so the trade is worth stating plainly: a queue
// PRESERVES every published value and delivers them in order, which is correct for events
// (a note-on must not be dropped) but wrong for a continuously-adjusted VALUE. A knob
// moved from A to B to C during one audio block does not want the audio thread to render
// A, then B, then C in sequence on some later block, arriving three UI-thread-widths of
// latency behind and glitching through two states nobody's ear ever heard in isolation.
// It wants the audio thread to see C — the LATEST state — the next time it looks, with A
// and B silently discarded because they are already obsolete the instant C exists. That
// is what a knob is: TripleBuffer implements exactly that, and drops stale intermediate
// values on purpose. If a future parameter needs every value delivered (an automation
// recorder is the one app-design.md §12 names), it needs a queue instead of this bus —
// they are not the same problem wearing two hats.
//
// RT-SAFETY: poll() is meant to be called once per audio block, inside VH_RT_SECTION(),
// per app-design.md §6.1 ("tuningBus.poll() -> applyTuning" as the second step of the
// audio callback). No allocation, no lock, no blocking wait anywhere in either method —
// see tests/test_tuning.cpp for the mechanical proof, which follows the same allocation-
// detector approach as tests/test_rt_safety.cpp.
//
// THREADING CONTRACT: single producer, single consumer. publish() must not be called
// concurrently with itself — if more than one thread can edit Tuning, serialise THOSE
// writers before they reach this class; TripleBuffer does not arbitrate between multiple
// producers, only between one producer and one consumer. poll() likewise assumes a single
// caller (the audio thread); app-design.md's threading model (§6.1) has exactly one of
// each, which is what this class is built for.

#pragma once

#include "vh/rt.hpp"
#include "vh/tuning.hpp"

#include <atomic>
#include <cstdint>

namespace vh {

class TuningBus {
public:
    TuningBus() noexcept = default;
    TuningBus(const TuningBus&) = delete;
    TuningBus& operator=(const TuningBus&) = delete;

    // UI thread (or any single non-audio thread; see the threading contract above). Not
    // marked VH_RT_SECTION() — it is explicitly the OFF-thread half of this channel, and
    // is allowed to do things the audio thread cannot (though today it does nothing more
    // than a struct copy and one atomic RMW).
    void publish(const Tuning& t) noexcept {
        buf_[writeIdx_] = t;

        // RELEASE: publishes the struct copy above together with the index that now names
        // it. See "why not memory_order_relaxed" below for why this specific pairing is
        // load-bearing rather than a style choice.
        const std::uint8_t next = static_cast<std::uint8_t>(writeIdx_) | kDirty;
        const std::uint8_t prev = state_.exchange(next, std::memory_order_acq_rel);

        // Reclaim whichever slot was sitting in the middle before this call. It cannot be
        // the slot poll() currently holds (readIdx_) — that one is, by the invariant this
        // whole class maintains, never anything but privately owned by the consumer or by
        // the shared middle, and this exchange only ever hands the MIDDLE slot back to us.
        // So overwriting it next call is always safe, with no read in flight on it.
        writeIdx_ = prev & kIndexMask;
    }

    // Audio thread, once per block. Returns nullptr when nothing has been published since
    // the last poll() — that is the STEADY STATE (most blocks, most of the time nobody is
    // touching a knob), and it costs exactly one atomic load with acquire ordering and no
    // copy: the fast path this class exists to guarantee.
    const Tuning* poll() noexcept {
        VH_RT_SECTION();

        if (!(state_.load(std::memory_order_acquire) & kDirty)) return nullptr;

        // Something is new. Claim the middle slot with an RMW rather than a plain load,
        // because a plain load would hand back an index without also creating a
        // happens-before edge for what that index NOW points at — see the memory-ordering
        // note below. The value we publish back (readIdx_, no dirty bit) is our own old
        // slot, which we are done with; the producer may start overwriting it the instant
        // this exchange lands.
        const std::uint8_t next = static_cast<std::uint8_t>(readIdx_);
        const std::uint8_t prev = state_.exchange(next, std::memory_order_acq_rel);
        readIdx_ = prev & kIndexMask;
        return &buf_[readIdx_];
    }

private:
    // 2 bits name one of 3 slots; the 3rd bit says "the middle slot holds something poll()
    // has not claimed yet". 3 slots, not 2, is what makes this wait-free in both
    // directions: with 2 slots, publish() and poll() would contend for the same "the other
    // one" slot and one side would have to block or spin. With 3, there is always a slot
    // that belongs to nobody's in-flight operation, so both sides complete in one RMW,
    // always, regardless of what the other thread is doing at that instant.
    static constexpr std::uint8_t kIndexMask = 0b011;
    static constexpr std::uint8_t kDirty     = 0b100;

    // WHY NOT std::memory_order_relaxed ON THE EXCHANGES — the sequence handoff itself
    // (which integer names which slot) is not the only thing this atomic protects; it is
    // the ONLY synchronisation edge, anywhere, between the UI thread's writes into a
    // Tuning and the audio thread's reads of that same Tuning. relaxed ordering guarantees
    // the INDEX is transferred atomically (no torn read of the 3-bit state byte) but
    // creates no happens-before relationship, so the compiler and the CPU remain free to
    // reorder the audio thread's read of buf_[readIdx_]'s fields ahead of its own exchange
    // that just claimed that index. On x86's comparatively strong memory model this bug is
    // very likely to be silently absent in testing and present on hardware with a weaker
    // model (ARM, which is exactly the class of machine a laptop's audio stack may end up
    // running on) — it would be a real, reproducible bug that happens not to reproduce on
    // whatever machine wrote the code. acquire/release is not defence in depth here; it is
    // the entire mechanism by which a struct written on one thread becomes safe to read on
    // another without a mutex, and weakening it is not a style preference.
    std::atomic<std::uint8_t> state_{1};   // slot 1 starts in the middle, unclaimed, clean.

    Tuning buf_[3]{};

    std::uint8_t writeIdx_ = 0;   // producer-private: never touched by poll().
    std::uint8_t readIdx_  = 2;   // consumer-private: never touched by publish().
};

} // namespace vh
