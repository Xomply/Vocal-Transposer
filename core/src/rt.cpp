#include "vh/rt.hpp"

#include <atomic>

namespace vh::rt {
namespace {
// Depth, not bool: RtScopes nest (Engine::process -> Shifter::process).
thread_local int g_depth = 0;
std::atomic<long> g_violations{0};
} // namespace

bool inRealtimeSection() noexcept { return g_depth > 0; }

void reportViolation(const char*) noexcept {
    g_violations.fetch_add(1, std::memory_order_relaxed);
}

long violationCount() noexcept { return g_violations.load(std::memory_order_relaxed); }
void resetViolationCount() noexcept { g_violations.store(0, std::memory_order_relaxed); }

RtScope::RtScope() noexcept { ++g_depth; }
RtScope::~RtScope() noexcept { --g_depth; }

RtScopeSuspend::RtScopeSuspend() noexcept : saved_(g_depth) { g_depth = 0; }
RtScopeSuspend::~RtScopeSuspend() noexcept { g_depth = saved_; }

} // namespace vh::rt
