// vh/profile.hpp — a Tuning, on disk.
//
// SEED: docs/app-design.md §7's payoff, stated plainly there and worth repeating here
// because it is the entire reason this library exists: "a profile is exactly the engine's
// live parameter set, so the offline tools can load one." A vh::Tuning tuned by ear in the
// (future) app becomes the literal input to vh_render and vh_sweep — measurement and
// listening stop being two configurations that can silently drift apart.
//
// WHY THIS IS ITS OWN LIBRARY, NOT PART OF core: CMakeLists.txt's header comment on
// vh_core calls core's freedom from any dependency "the most important build decision in
// this file" — core is tested headlessly, with no framework and, by extension, no
// assumption that a filesystem call is even a good idea to make from inside it. Tuning
// (core/include/vh/tuning.hpp) is a POD that "knows nothing about files" on purpose, per
// that header's own comment. Serialization is file I/O, which is exactly the kind of
// concern core keeps at arm's length — so it lives here, in a small library that links
// vh_core and nothing else, shared by the offline tools (this track) and, later, the app
// (Track F) so there is one parser instead of two silently diverging ones.
//
// FORMAT: flat, line-oriented `key = value` text, extension `.vhprofile`, `schema_version`
// first. See docs/app-design.md §7 for the full case against JSON — short version: Tuning
// is a flat POD of scalars, so a hand-rolled reader is strictly less code and fewer failure
// modes than vendoring or hand-rolling a JSON parser, for identical capability. See
// profile.cpp for the exact grammar and the reasoning behind each parsing choice.
//
// KEYS ARE MATCHED BY NAME, not by position or byte offset. That is what makes "missing
// keys keep their struct default" and "unknown keys warn and are ignored" fall out of the
// format for free rather than needing special-case logic: a key absent from the file is a
// key no `consume` call in profile.cpp ever claims, so the field it would have written
// simply keeps whatever Tuning{} already put there; a key present in the file that no
// `consume` call asks for is never claimed either, and survives to become a warning. This
// is also why a schema_version MISMATCH does not, by itself, refuse to load — see
// profile.cpp's load() for the policy and the reasoning.
//
// NO EXCEPTIONS ACROSS THIS BOUNDARY, and no console I/O. Every failure path is a return
// value; see LoadReport below. A library that writes to stdout is a library nobody can call
// from inside a GUI's file-open handler, which is the exact wording app-design.md §7 uses
// for the requirement.

#pragma once

#include "vh/tuning.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vh::profile {

// A profile's non-engine metadata. Deliberately NOT folded into vh::Tuning: Tuning is the
// engine's live parameter set and nothing in core reads a name or notes — keeping them
// separate here is the same "core does not know about files, and does not know about
// naming things either" boundary, one level further out.
struct ProfileMeta {
    std::string name;
    std::string notes;
};

// Everything load() has to say about what happened, returned BY VALUE rather than printed.
// The caller — a tool's main(), or one day the app's profile-open handler — decides what to
// do with warnings; this library never assumes a console exists on the other end of the
// call.
struct LoadReport {
    bool ok = false;

    // Set only when ok == false. Meant to be shown or logged verbatim: includes the file
    // path, the offending line number where one exists, and what was expected, because the
    // caller has no other way to explain a bad hand-edited file to whoever wrote it.
    std::string error;

    // Non-fatal notices: unknown keys (one entry per unrecognized key, naming its line
    // number) and a schema-version-mismatch notice when the file's schema_version differs
    // from vh::kTuningSchemaVersion. The file still loaded successfully; these are worth
    // surfacing to a human, not reasons the load failed.
    std::vector<std::string> warnings;

    // The schema_version key's value as read from the file, whether or not it matched this
    // build's vh::kTuningSchemaVersion. Left at 0 when load() returns false before reaching
    // that key (an unreadable file, a syntax error before it) — 0 should never coexist with
    // ok == true, because a missing schema_version key is itself always a hard error (see
    // profile.cpp).
    std::uint32_t fileSchemaVersion = 0;
};

// Serializes `tuning` and `meta` to `path` as a .vhprofile file, overwriting any existing
// file there. Returns false and fills `error` on any I/O failure; never throws.
//
// Deliberately does NOT call tuning.clamp() first. save() persists exactly the struct the
// caller has — typically an already-live, already-clamped Engine's Tuning, but also
// possibly a test harness's deliberately out-of-range value being round-tripped on purpose.
// Laundering it through clamp() on the way out would make save() lossy in a way "write then
// read reproduces the struct" (this library's own exactness guarantee) does not allow for.
// clamp() belongs to load(), because load()'s input is untrusted and save()'s is not.
bool save(const Tuning& tuning, const ProfileMeta& meta, const std::string& path,
          std::string& error);

// Reads `path` as a .vhprofile file. On success, fills `tuning` and `meta` and returns
// true with report.ok == true. On failure — unreadable file, a syntax error, a value that
// fails to parse as its field's type, a missing schema_version key — returns false with
// report.ok == false and report.error set, and leaves `tuning`/`meta` COMPLETELY UNTOUCHED:
// every field is built into a local temporary first and only copied out on success, so
// there is no such thing as a half-loaded Tuning escaping this function.
//
// MISSING KEYS keep vh::Tuning{}'s compiled-in default — not whatever `tuning` held on
// entry. This is the migration guarantee app-design.md §7 names as the whole reason for
// this rule: a profile saved by an older build, loaded by a newer one, gains the newer
// build's defaults for any field it does not mention.
//
// ALWAYS calls tuning.clamp() before returning on success. LOAD-BEARING, not a nicety:
// app-design.md §4.5 item 1 is a hand-edited profile with hopSamples = 0 hanging the audio
// thread forever inside the callback, and a .vhprofile file is exactly the untrusted,
// hand-editable input that hazard is about. There is no UI widget in this call path to have
// clamped the value first.
bool load(const std::string& path, Tuning& tuning, ProfileMeta& meta, LoadReport& report);

} // namespace vh::profile
