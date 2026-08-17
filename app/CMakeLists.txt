# app/CMakeLists.txt — the JUCE standalone shell. Track F1 of docs/app-design.md.
#
# WHAT THIS FILE DOES AND DOES NOT DO, per docs/app-design.md §10 and app/README.md: it
# builds a plain executable (juce_add_gui_app, NOT juce_add_plugin — no plugin format, no
# AudioProcessor indirection) that links vh_core and wires it to real audio/MIDI hardware
# through JUCE. No DSP lives here; every line below is device selection, thread plumbing,
# and widgets. The Engine itself is untouched by this file.
#
# core/ still links no framework (see the comment at the top of the root CMakeLists.txt) —
# only this app/ subtree pulls in JUCE, and only when VH_BUILD_APP is turned on.

# JUCE's own CMakeLists hard-requires a C compiler to be enabled on the project (some
# bundled third-party sources, e.g. parts of the ASIO/WinRT MIDI glue, are plain C), even
# though every line vh_app itself contributes is C++. The root CMakeLists.txt declares
# `LANGUAGES CXX` only, deliberately, since core/tests/tools/offline are all C++-only and
# core's freedom from any other toolchain requirement is load-bearing there. Enabling C
# only inside this subtree keeps that root declaration untouched.
enable_language(C)

include(FetchContent)

# CMake 4.0 removed compatibility with cmake_minimum_required(<3.5); JUCE's own top-level
# CMakeLists still declares an old minimum. Same guard the root CMakeLists.txt applies
# around doctest, for the same reason.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

# WHY GIT_CONFIG core.longpaths=true: JUCE's repo ships iOS Xcode asset catalogs under
# examples/DemoRunner/... and extras/AudioPerformanceTest/... whose paths exceed Windows'
# legacy 260-character MAX_PATH. Without this, a plain `git checkout` of this tag fails on
# Windows with "Filename too long" before CMake ever sees the source tree — reproduced by
# hand against this exact tag while building this file. Scoped to this one FetchContent
# clone via ExternalProject's GIT_CONFIG (CMake >= 3.24), not global git config.
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        9.0.1
  GIT_SHALLOW    TRUE
  GIT_CONFIG     core.longpaths=true
)
FetchContent_MakeAvailable(JUCE)

juce_add_gui_app(vh_app
  PRODUCT_NAME "Vocal Transposer"
  COMPANY_NAME "vocalharm"
  NEEDS_WEB_BROWSER FALSE
  NEEDS_CURL FALSE
)

target_sources(vh_app
  PRIVATE
    src/Main.cpp
    src/MainComponent.cpp
    src/DevicesPanel.cpp
    src/PerformPanel.cpp
    src/MidiRouter.cpp
    src/EngineAudioCallback.cpp
    src/TuningStore.cpp
    src/CurveDisplays.cpp
    src/VoicePanel.cpp
    src/BlendPanel.cpp
    src/ExpertContent.cpp
    src/ExpertPanel.cpp
    src/BenchPanel.cpp
)

target_compile_definitions(vh_app
  PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:vh_app,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:vh_app,JUCE_VERSION>"

    # JUCE_ASIO defaults OFF in JUCE 9 and must be turned on explicitly (recon report,
    # docs/app-design.md §10). JUCE now bundles a minimal ASIO SDK in-repo
    # (modules/juce_audio_devices/native/asio/), dual-licensed Steinberg-proprietary or
    # GPLv3 — distinct from JUCE's own AGPLv3 licence. Fine for a personal, non-distributed
    # build; revisit before ever shipping a binary to anyone outside this machine.
    #
    # WHY THIS TARGET AND NOT juce_audio_devices DIRECTLY: JUCE's CMake modules are
    # INTERFACE libraries (extras/Build/CMake/JUCEModuleSupport.cmake:
    # `add_library(${target} INTERFACE)`), so a module's .cpp translation unit is compiled
    # as part of whichever concrete target links it — i.e. as part of vh_app's own
    # compilation. Setting the define here is therefore both necessary and sufficient;
    # confirmed against docs/CMake API.md's "A note about compile definitions" section.
    JUCE_ASIO=1
)

target_link_libraries(vh_app
  PRIVATE
    vh_core
    # vh_profile (profile/include/vh/profile.hpp): Track G's Bench panel loads/saves
    # .vhprofile files through the SAME serializer the offline tools use (tools/
    # profile_cli.hpp's own header explains why one parser beats two silently diverging
    # ones) -- see BenchPanel.cpp / TuningStore.cpp. vh_profile itself links vh_core only,
    # so this adds no framework dependency to that library.
    vh_profile
    juce::juce_audio_utils
    juce::juce_audio_devices
    juce::juce_gui_extra
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
