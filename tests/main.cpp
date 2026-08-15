#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "vh/rt.hpp"

#include <cstdlib>
#include <new>

// THE ALLOCATION DETECTOR.
//
// Overriding global operator new in the TEST BINARY ONLY lets us assert, mechanically,
// that no code path marked VH_RT_SECTION() ever allocates. This is the enforcement half
// of the rule that rt.hpp only declares.
//
// WHY GLOBAL AND NOT A CUSTOM ALLOCATOR: the allocations we are hunting are the ones we
// did not know about — a std::function copy, a vector growing inside a library call, a
// string built for a log line. A custom allocator only catches allocations you already
// routed through it, which are precisely the ones you were already thinking about. The
// global override catches the ones you weren't.
//
// It reports rather than aborts, so a test can assert on the count and give a useful
// message instead of dying with a stack trace.

void* operator new(std::size_t sz) {
    if (vh::rt::inRealtimeSection()) {
        vh::rt::reportViolation("operator new inside RtScope");
    }
    if (sz == 0) sz = 1;
    if (void* p = std::malloc(sz)) return p;
    throw std::bad_alloc{};
}

void* operator new[](std::size_t sz) { return operator new(sz); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
