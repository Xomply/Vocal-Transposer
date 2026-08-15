// vh/rt.hpp — making "no allocation on the audio thread" a testable claim.
//
// WHY THIS EXISTS AT ALL:
// The rule "no allocation, no locks, no I/O in the audio callback" is in every real-time
// audio guide, and it is violated constantly, because violating it is invisible. A
// std::vector::push_back hidden three call levels down inside an analysis routine will
// work perfectly on your development machine and produce a dropout during a gig. The
// symptom appears far from the cause, under load you cannot reproduce at a desk.
//
// So the rule is not left as documentation. It is instrumented: RtScope marks a region
// as real-time, and in test builds any allocation inside that region aborts the test.
// The guard compiles to nothing in release, so it costs zero on stage.
//
// CERTAIN: this instrumentation earns its keep. It is ~40 lines and it converts the
// single most expensive class of bug in this domain from "debug it during a performance"
// into "a red test".

#pragma once

namespace vh::rt {

// True while the calling thread is inside a real-time region.
// thread_local because the audio thread is real-time and the UI thread is not, and they
// run concurrently.
bool inRealtimeSection() noexcept;

// Called by the instrumented operator new in test builds. In release this is never
// invoked because the operator override is not linked in.
void reportViolation(const char* what) noexcept;

// How many violations have been seen. Tests assert this is zero.
long violationCount() noexcept;
void resetViolationCount() noexcept;

// RAII marker. Construct at the top of anything that runs on the audio thread.
//
// Nesting is supported (an engine's process() calls a shifter's process(), both marked)
// via a depth counter rather than a bool, so the inner scope's destructor does not
// prematurely clear the outer one's marking.
class RtScope {
public:
    RtScope() noexcept;
    ~RtScope() noexcept;
    RtScope(const RtScope&) = delete;
    RtScope& operator=(const RtScope&) = delete;
};

// Escape hatch, deliberately ugly to type.
//
// WHY IT EXISTS: some genuinely-safe operation may trip the detector — for example a
// third-party library that allocates once on first call and then never again. Rather
// than weaken the guard globally, mark that one call site.
// IF YOU FIND YOURSELF USING THIS MORE THAN ONCE OR TWICE, the design is wrong, not the
// guard.
class RtScopeSuspend {
public:
    RtScopeSuspend() noexcept;
    ~RtScopeSuspend() noexcept;
    RtScopeSuspend(const RtScopeSuspend&) = delete;
    RtScopeSuspend& operator=(const RtScopeSuspend&) = delete;
private:
    int saved_;
};

} // namespace vh::rt

// Usage: VH_RT_SECTION(); as the first line of any audio-thread function.
#define VH_RT_SECTION() ::vh::rt::RtScope vh_rt_scope_guard_
