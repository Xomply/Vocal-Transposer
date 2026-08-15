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
  )
  target_link_libraries(vh_tests PRIVATE vh_core doctest::doctest)

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
  add_executable(vh_render tools/render.cpp)
  target_link_libraries(vh_render PRIVATE vh_core)
  target_include_directories(vh_render PRIVATE tools)

  add_executable(vh_bench tools/bench.cpp)
  target_link_libraries(vh_bench PRIVATE vh_core)
  target_include_directories(vh_bench PRIVATE tools)
endif()
