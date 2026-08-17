// tools/profile_cli.hpp — shared "--profile <path>" handling for the offline tools.
//
// WHY SHARED RATHER THAN COPY-PASTED INTO EACH TOOL: vh_render and vh_sweep both gain the
// same additive flag (app-design.md §7/Track E), and the two copies drifting apart (one
// accepting `--profile=path`, the other not; one printing warnings, the other silent) is
// exactly the kind of thing nobody notices until a user hits it. Header-only, like wav.hpp
// next to it, so neither tool's CMake target needs a second .cpp to track.
//
// THIS is the ONLY place these tools talk to the console about a profile. vh_profile
// itself never does — see vh/profile.hpp's own header comment on why a serialization
// library must not own stdout.

#pragma once

#include "vh/profile.hpp"
#include "vh/tuning.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace vhtools {

// Pulls "--profile <path>" (or "--profile=<path>") out of `args` IN PLACE, leaving every
// OTHER argument at its original relative position. This is what keeps every existing
// invocation in README.md/HANDOVER.md working unchanged: the flag is additive rather than
// positional, so it can be dropped in anywhere on the command line without shifting the
// indices the rest of main() still parses by position.
//
// Returns nullopt if "--profile" was not found. If "--profile" was found with nothing
// after it, it is left in `args` untouched (still findable by name) so the caller's own
// argument-count check reports a normal usage error rather than this function inventing a
// second error-reporting path.
inline std::optional<std::string> extractProfileFlag(std::vector<std::string>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--profile") {
            if (i + 1 >= args.size()) return std::nullopt;
            std::string path = args[i + 1];
            args.erase(args.begin() + static_cast<long>(i), args.begin() + static_cast<long>(i) + 2);
            return path;
        }
        constexpr const char* kPrefix = "--profile=";
        constexpr size_t kPrefixLen = 10;   // strlen("--profile=")
        if (args[i].compare(0, kPrefixLen, kPrefix) == 0) {
            std::string path = args[i].substr(kPrefixLen);
            args.erase(args.begin() + static_cast<long>(i));
            return path;
        }
    }
    return std::nullopt;
}

// Loads `path` into `tuning`, printing which profile was loaded (name, schema version) and
// any warnings to stderr. Returns false on a hard load failure, having already printed the
// error — the caller just needs to propagate the failure (return 1 from main()).
inline bool loadProfileForTool(const std::string& path, vh::Tuning& tuning) {
    vh::profile::ProfileMeta meta;
    vh::profile::LoadReport report;
    if (!vh::profile::load(path, tuning, meta, report)) {
        std::fprintf(stderr, "error: could not load profile '%s': %s\n",
                     path.c_str(), report.error.c_str());
        return false;
    }
    std::fprintf(stderr, "profile: '%s' (schema %u) from %s\n",
                 meta.name.empty() ? "(unnamed)" : meta.name.c_str(),
                 static_cast<unsigned>(report.fileSchemaVersion), path.c_str());
    for (const auto& w : report.warnings) {
        std::fprintf(stderr, "  warning: %s\n", w.c_str());
    }
    return true;
}

} // namespace vhtools
