cmake_minimum_required(VERSION 3.20)
project(vocalharm VERSION 0.1.0 LANGUAGES CXX)

# WHY C++20: designated initialisers, <bit>, std::span-adjacent conveniences, and
# constexpr maths. Nothing here needs 23, and 17 would cost readability for no gain.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(VH_BUILD_TESTS   "Build unit tests"        ON)
option(VH_BUILD_OFFLINE "Build offline harness"   ON)

# OFF by default so the headless CI/test build stays the default build — core being
# testable without a framework is the load-bearing build decision this file opens with.
# Turning this on pulls in JUCE via FetchContent (see app/CMakeLists.txt) and builds the
# standalone instrument (docs/app-design.md, Track F).
option(VH_BUILD_APP     "Build the JUCE standalone app" OFF)

# ---------------------------------------------------------------------------
# core — the whole DSP system, with NO framework dependency.
#
# THE MOST IMPORTANT BUILD DECISION IN THIS FILE: core does not link JUCE, does not
# know about plugins, and does not touch audio hardware. It is plain C++ operating on
# buffers.
#
# The payoff is that every part of this project can be tested and benchmarked headlessly,
# deterministically, in CI, with no audio device present. An audio codebase where the DSP
# can only be exercised through a plugin host is one where the DSP is effectively
# untested, because nobody will spin up a host to check a windowing change. Keeping the
# framework at arm's length is what makes the test suite below possible at all.
# ---------------------------------------------------------------------------
add_library(vh_core STATIC
  core/src/rt.cpp
  core/src/yin.cpp
  core/src/granular_shifter.cpp
  core/src/psola_shifter.cpp
  core/src/engine.cpp
)
target_include_directories(vh_core PUBLIC core/include)

target_compile_options(vh_core PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# WHY -ffast-math IS NOT SET, since someone will eventually ask:
# it permits reassociation and flushes denormals in ways that break the numerical
# assumptions in phase accumulation, and it makes NaN checks unreliable — which matters,
# because a NaN in an audio path is a loud noise in someone's ears. Denormal handling
# belongs in an explicit FTZ/DAZ setting at stream start, not in a blanket compiler flag.

# ---------------------------------------------------------------------------
# vh_profile — serializes vh::Tuning to/from a flat, hand-editable .vhprofile file
# (profile/include/vh/profile.hpp). Its own top-level directory, NOT inside core/: per
# docs/app-design.md §7, core stays dependency-free and Tuning stays a POD that knows
# nothing about files (see the vh_core comment above for why that is load-bearing).
# Standard library only. Linked by the offline tools (vh_render, vh_sweep, below) and,
# later, the app (Track F) — one parser shared, not two silently diverging ones, which is
# the entire payoff §7 describes: a profile tuned by ear becomes the literal input to the
# offline measurement tools.
# ---------------------------------------------------------------------------
add_library(vh_profile STATIC
  profile/src/profile.cpp
)
target_include_directories(vh_profile PUBLIC profile/include)
target_link_libraries(vh_profile PUBLIC vh_core)

target_compile_options(vh_profile PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Wshadow -Wconversion>
  $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
if(VH_BUILD_TESTS)
  include(FetchContent)

  # CMake 4.0 removed compatibility with cmake_minimum_required(<3.5), which doctest's
  # released CMakeLists still declares. Scoped to the fetch so it does not relax policy
  # checking for our own code. Remove when doctest ships a newer minimum.
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
  endif()

  FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(doctest)

  add_executable(vh_tests
    tests/main.cpp
    tests/test_audio_ring.cpp
    tests/test_rt_safety.cpp
    tests/test_yin.cpp
    tests/test_engine.cpp
    tests/test_shifters.cpp
    tests/test_continuity.cpp
    tests/test_tuning.cpp
    tests/test_profile.cpp
    tests/test_humanization.cpp
    tests/test_output_stage.cpp
  )
  target_link_libraries(vh_tests PRIVATE vh_core vh_profile doctest::doctest)

  enable_testing()
  add_test(NAME unit COMMAND vh_tests)
endif()

# ---------------------------------------------------------------------------
# offline — WAV in, WAV out. No audio hardware.
#
# This is how you iterate on DSP: render a file, listen, diff against a golden reference.
# Every algorithm change gets heard and regression-checked without a single callback.
# ---------------------------------------------------------------------------
if(VH_BUILD_OFFLINE)
  add_executable(vh_offline offline/main.cpp)
  target_link_libraries(vh_offline PRIVATE vh_core)
  target_include_directories(vh_offline PRIVATE tools)

  # Renders the dry/wet listening material. Uses the same Engine as the audio callback —
  # there is no separate "offline mode", which is what makes the demo evidence rather than
  # illustration.
  add_executable(vh_demo tools/demo.cpp)
  target_link_libraries(vh_demo PRIVATE vh_core)
  target_include_directories(vh_demo PRIVATE tools)

  # Runs a REAL recording through the engine. The most important tool going forward:
  # everything else is validated on a synthetic singer.
  # Links vh_profile (which itself links vh_core) rather than vh_core directly: an optional
  # `--profile <path>` argument (app-design.md §7/Track E) lets this tool load the same
  # Tuning the app would apply, on top of every existing invocation working unchanged.
  add_executable(vh_render tools/render.cpp)
  target_link_libraries(vh_render PRIVATE vh_core vh_profile)
  target_include_directories(vh_render PRIVATE tools)

  add_executable(vh_bench tools/bench.cpp)
  target_link_libraries(vh_bench PRIVATE vh_core)
  target_include_directories(vh_bench PRIVATE tools)

  # Renders ONE engine at ONE constant interval (Mode B), nothing else. Exists so that a
  # control render can be produced at the SAME RATIO as the thing under test, which
  # vh_render structurally cannot do — its variant table fixes RatioSource per variant.
  # See the header comment in tools/sweep.cpp.
  # Also gains an optional `--profile <path>` argument (see vh_render above) — vh_profile
  # links vh_core, so this is the only line that needs to change to reach it.
  add_executable(vh_sweep tools/sweep.cpp)
  target_link_libraries(vh_sweep PRIVATE vh_core vh_profile)
  target_include_directories(vh_sweep PRIVATE tools)

  # vh_sweep plus a per-block CSV of what the analyser believed. Exists because BUGS.md
  # VH-002 names an experiment — correlate click timestamps against the epoch series and
  # the per-block period — that could not be run without a log. See tools/trace.cpp.
  add_executable(vh_trace tools/trace.cpp)
  target_link_libraries(vh_trace PRIVATE vh_core)
  target_include_directories(vh_trace PRIVATE tools)
endif()

# ---------------------------------------------------------------------------
# app — the JUCE standalone instrument. See app/CMakeLists.txt and app/README.md.
# ---------------------------------------------------------------------------
if(VH_BUILD_APP)
  add_subdirectory(app)
endif()
