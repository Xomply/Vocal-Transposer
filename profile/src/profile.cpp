// profile/src/profile.cpp — the flat `key = value` reader/writer behind vh/profile.hpp.
//
// GRAMMAR: one `key = value` pair per non-blank, non-comment line. `#` at the start of a
// (trimmed) line marks a whole-line comment; blank lines are ignored. Whitespace around the
// FIRST `=` on a line is trimmed from both the key and the value. A line with no `=` at all
// is a hard parse error — see "WHY MALFORMED LINES ARE A HARD ERROR" below.
//
// KEY NAMES are dotted to mirror the struct path they came from (e.g.
// "preservation.voicing.mu_strength") purely for a human reading the file next to
// tuning.hpp/shifter.hpp/analysis.hpp — the format has no nesting semantics of its own, and
// a dot is just a character the '#'/'='-based grammar above does not treat specially.
//
// FLOAT/DOUBLE FORMATTING: std::to_chars in its default (no precision argument) mode, which
// the standard defines to produce the SHORTEST decimal string that round-trips to the exact
// same value on read-back via std::from_chars. This was chosen over a fixed precision
// (`%.9g`-style) deliberately: a fixed precision either wastes bytes on values that need
// fewer digits (most of this struct's defaults are values like 0.5, 8, 120 — `%.9g` would
// write "0.500000000") or, worse, is a precision chosen by guesswork that might not be
// enough for some value that happens to need more digits to round-trip exactly. to_chars
// sidesteps the guess entirely: it is provably exact for every representable float and
// double, it needs no locale (unlike sscanf/strtof, which are locale-sensitive on the
// decimal point in ways that would make a profile saved on one machine unreadable on
// another with a different locale set), and it is also used for every integer field here so
// int/size_t/float all go through one code path rather than two.
//
// WHY MALFORMED LINES ARE A HARD ERROR, not a skip-with-warning like an unknown KEY: a line
// with no `=` at all, a duplicate key, an unparsable number, an unrecognized enum name, or a
// non-finite float is evidence the file was corrupted or hand-edited incorrectly, not
// evidence of an old/new schema mismatch (which unknown *keys* already cover generically).
// Skipping such a line silently would be building exactly the "half-loaded" hazard
// vh/profile.hpp's load() contract promises not to have. See the "hard error" comment next
// to Consume::Error below.
//
// SCHEMA VERSION MISMATCH POLICY: warn, do not refuse. Because keys are matched by NAME
// (see vh/profile.hpp's header comment), a version bump that only adds or removes fields
// already migrates correctly with zero special-casing — an added field takes its compiled
// default, a removed one is silently ignored as an "unknown key". The only case a version
// bump can introduce that this loader cannot detect AT ALL is a field that kept its name
// while its MEANING changed (units, sign convention, an enum's semantics) — and refusing to
// load on ANY version mismatch would block that hazard by also blocking the overwhelmingly
// common, harmless case (an old profile predating an additive change), which defeats
// app-design.md §7's entire payoff: a profile from an older app build feeding a newer
// vh_render. So: load anyway, attach one warning naming both versions, and trust the
// operator to check a changelog if a REINTERPRETED field is a live possibility for their
// build gap. See the report handed back to the caller for how this was reasoned through.

#include "vh/profile.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vh::profile {

namespace {

// --- tiny string utilities ------------------------------------------------------------

bool isSpaceChar(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isSpaceChar(s[b])) ++b;
    while (e > b && isSpaceChar(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// String VALUES (name, notes) are written on one line each even though `notes` is
// documented as free text that may contain newlines — this escaping is what keeps "one
// parameter per line" true for every field, including the two string ones. Backslash and
// newline/carriage-return are the only characters that need escaping for THIS grammar
// (nothing else is syntactically special mid-value once the key/`=` split has already
// happened), so the scheme stays minimal on purpose.
std::string escapeString(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

bool unescapeString(const std::string& s, std::string& out, std::string& err) {
    out.clear();
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') { out += s[i]; continue; }
        if (i + 1 >= s.size()) { err = "trailing backslash"; return false; }
        const char n = s[++i];
        switch (n) {
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case '\\': out += '\\'; break;
            default:
                err = std::string("unknown escape sequence '\\") + n + "'";
                return false;
        }
    }
    return true;
}

// --- line parsing ------------------------------------------------------------------------

struct KV {
    int line = 0;
    std::string key;
    std::string value;   // raw, trimmed, NOT yet type-parsed or unescaped
    bool claimed = false;
};

// Splits `text` into KV entries, skipping blank lines and `#` comments. Returns false with
// `error` set on the FIRST syntactically malformed line (no `=`, or an empty key) — see the
// file header on why this is a hard stop rather than a skip.
bool parseLines(const std::string& text, std::vector<KV>& out, std::string& error) {
    out.clear();
    size_t pos = 0;
    int lineNo = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        std::string rawLine = (nl == std::string::npos) ? text.substr(pos)
                                                          : text.substr(pos, nl - pos);
        ++lineNo;
        if (!rawLine.empty() && rawLine.back() == '\r') rawLine.pop_back();   // CRLF files

        const std::string line = trim(rawLine);
        if (!line.empty() && line[0] != '#') {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) {
                std::ostringstream os;
                os << "line " << lineNo << ": expected 'key = value', got '" << line << "'";
                error = os.str();
                return false;
            }
            std::string key = trim(line.substr(0, eq));
            std::string value = trim(line.substr(eq + 1));
            if (key.empty()) {
                std::ostringstream os;
                os << "line " << lineNo << ": empty key before '='";
                error = os.str();
                return false;
            }
            out.push_back(KV{lineNo, std::move(key), std::move(value), false});
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return true;
}

// A key defined twice is treated as malformed input (same bucket as a bad line), not as an
// "unknown key" warning: silently picking first-wins or last-wins would let a hand-edit
// mistake load "successfully" with whichever value nobody meant to keep.
bool checkDuplicates(const std::vector<KV>& kvs, std::string& error) {
    std::unordered_map<std::string, int> firstLine;
    for (const auto& kv : kvs) {
        auto [it, inserted] = firstLine.try_emplace(kv.key, kv.line);
        if (!inserted) {
            std::ostringstream os;
            os << "key '" << kv.key << "' defined more than once (line " << it->second
               << " and line " << kv.line << ")";
            error = os.str();
            return false;
        }
    }
    return true;
}

class Reader {
public:
    explicit Reader(std::vector<KV>& kvs) : kvs_(kvs) {}

    // True and fills `value` (and marks the entry claimed) if `key` is present; false
    // (value untouched) if absent. Absence is not an error — see Consume::Absent below.
    bool take(const std::string& key, std::string& value) {
        for (auto& kv : kvs_) {
            if (kv.key == key) {
                kv.claimed = true;
                value = kv.value;
                return true;
            }
        }
        return false;
    }

private:
    std::vector<KV>& kvs_;
};

// --- typed field consumption / production -------------------------------------------------

enum class Consume { Absent, Ok, Error };

template <typename T>
Consume consumeArith(Reader& r, const char* key, T& out, std::string& err) {
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>);
    std::string raw;
    if (!r.take(key, raw)) return Consume::Absent;

    if constexpr (std::is_unsigned_v<T>) {
        // std::from_chars for an unsigned type is not guaranteed by every implementation to
        // reject a leading '-' the way strtoul's callers expect; explicitly rejecting it
        // here turns "a negative value wrapped into a huge unsigned one" (which clamp()
        // would then NOT catch, since the clamp on hop_samples only floors, never ceilings)
        // into a clean parse error instead.
        if (!raw.empty() && raw[0] == '-') {
            err = std::string("key '") + key + "': expected a non-negative integer, got '" + raw + "'";
            return Consume::Error;
        }
    }

    T v{};
    const auto res = std::from_chars(raw.data(), raw.data() + raw.size(), v);
    if (res.ec != std::errc() || res.ptr != raw.data() + raw.size()) {
        err = std::string("key '") + key + "': invalid number '" + raw + "'";
        return Consume::Error;
    }
    if constexpr (std::is_floating_point_v<T>) {
        // Defence in depth: std::from_chars for floating-point types does not recognize
        // "nan"/"inf" tokens per the standard's grammar (unlike strtod), so this branch
        // should be unreachable in practice, but a value that somehow parsed to a
        // non-finite float is exactly the kind of thing clamp() cannot be trusted to fix —
        // std::clamp on a NaN is a coin flip, not a bound.
        if (!std::isfinite(v)) {
            err = std::string("key '") + key + "': value must be finite, got '" + raw + "'";
            return Consume::Error;
        }
    }
    out = v;
    return Consume::Ok;
}

Consume consumeBool(Reader& r, const char* key, bool& out, std::string& err) {
    std::string raw;
    if (!r.take(key, raw)) return Consume::Absent;
    if (raw == "true")  { out = true;  return Consume::Ok; }
    if (raw == "false") { out = false; return Consume::Ok; }
    err = std::string("key '") + key + "': expected 'true' or 'false', got '" + raw + "'";
    return Consume::Error;
}

Consume consumeString(Reader& r, const char* key, std::string& out, std::string& err) {
    std::string raw;
    if (!r.take(key, raw)) return Consume::Absent;
    std::string unescErr;
    if (!unescapeString(raw, out, unescErr)) {
        err = std::string("key '") + key + "': " + unescErr;
        return Consume::Error;
    }
    return Consume::Ok;
}

template <typename E, size_t N>
Consume consumeEnum(Reader& r, const char* key, const std::pair<const char*, E> (&table)[N],
                     E& out, std::string& err) {
    std::string raw;
    if (!r.take(key, raw)) return Consume::Absent;
    for (const auto& [name, val] : table) {
        if (raw == name) { out = val; return Consume::Ok; }
    }
    err = std::string("key '") + key + "': unrecognized value '" + raw + "'";
    return Consume::Error;
}

template <typename T>
void writeArith(std::string& out, const char* key, T v) {
    static_assert(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>);
    out.append(key).append(" = ");
    if constexpr (std::is_floating_point_v<T>) {
        // See file header: save() does not clamp, so a caller-constructed non-finite value
        // (a test deliberately round-tripping one, say) must not crash to_chars, which does
        // not document NaN/Inf handling for its no-precision "shortest" mode. Writing the
        // literal token means load()'s from_chars-based reader will (correctly) refuse it
        // as an invalid number rather than silently accepting "nan" as if it were a name.
        if (std::isnan(v)) { out.append("nan"); out.append("\n"); return; }
        if (std::isinf(v)) { out.append(v > 0 ? "inf" : "-inf"); out.append("\n"); return; }
    }
    char buf[64];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, res.ptr);
    out.append("\n");
}

void writeBool(std::string& out, const char* key, bool v) {
    out.append(key).append(" = ").append(v ? "true" : "false").append("\n");
}

void writeString(std::string& out, const char* key, const std::string& v) {
    out.append(key).append(" = ").append(escapeString(v)).append("\n");
}

template <typename E, size_t N>
void writeEnum(std::string& out, const char* key, E v, const std::pair<const char*, E> (&table)[N]) {
    for (const auto& [name, val] : table) {
        if (val == v) { out.append(key).append(" = ").append(name).append("\n"); return; }
    }
    // Unreachable for any enumerator actually declared in voice.hpp/tuning.hpp/
    // preservation.hpp — every one of them appears in its table below. Falling back to the
    // integer rather than asserting keeps save() abort-free even if a future enumerator is
    // added to core without this file being updated in the same patch; a round-trip through
    // that fallback would then correctly fail on LOAD (consumeEnum has no name to match an
    // integer against), which is a louder, more diagnosable failure than a crash on save.
    out.append(key).append(" = ").append(std::to_string(static_cast<int>(v))).append("\n");
}

// --- enum name tables: the single source of truth for both directions --------------------
//
// Stable NAMES, not integers, per app-design.md §7: "an integer enum in a hand-editable
// file is a trap — reordering the enum silently changes the meaning of every existing
// profile." Each table is used by both writeEnum (save) and consumeEnum (load), so the
// name<->value mapping cannot drift between the two directions independently.

constexpr std::pair<const char*, RatioSource> kRatioSourceTable[] = {
    {"AbsoluteTarget", RatioSource::AbsoluteTarget},
    {"IntervalFromRoot", RatioSource::IntervalFromRoot},
};

constexpr std::pair<const char*, BlendKind> kBlendKindTable[] = {
    {"FastOnly", BlendKind::FastOnly},
    {"QualityOnly", BlendKind::QualityOnly},
    {"RegisterSplit", BlendKind::RegisterSplit},
    {"OnsetHandover", BlendKind::OnsetHandover},
};

constexpr std::pair<const char*, UnvoicedPolicy> kUnvoicedPolicyTable[] = {
    {"PassThrough", UnvoicedPolicy::PassThrough},
    {"PassThroughDecorrelated", UnvoicedPolicy::PassThroughDecorrelated},
    {"Shift", UnvoicedPolicy::Shift},
};

} // namespace

// ============================================================================
// save()
// ============================================================================

bool save(const Tuning& tuning, const ProfileMeta& meta, const std::string& path,
          std::string& error) {
    std::string out;
    out.reserve(4096);

    // schema_version FIRST, per §7's file-format rule — see profile.hpp's header comment
    // for why a keyed format does not strictly NEED this for correctness the way a
    // positional blob would, but a human opening the file benefits from seeing it first.
    writeArith(out, "schema_version", tuning.schemaVersion);

    writeEnum(out, "mode", tuning.mode, kRatioSourceTable);
    writeArith(out, "root_midi_note", tuning.rootMidiNote);
    writeArith(out, "voice_cap", tuning.voiceCap);
    writeArith(out, "attack_ms", tuning.attackMs);
    writeArith(out, "release_ms", tuning.releaseMs);
    writeArith(out, "glide_semis_per_sec", tuning.glideSemisPerSec);
    writeArith(out, "freeze_window_ms", tuning.freezeWindowMs);
    writeArith(out, "velocity_to_gain_min", tuning.velocityToGainMin);
    writeArith(out, "velocity_curve", tuning.velocityCurve);

    const auto& p = tuning.preservation;
    writeBool(out, "preservation.preserve_envelope", p.preserveEnvelope);
    writeArith(out, "preservation.envelope_warp", p.envelopeWarp);
    writeBool(out, "preservation.scale_warp_with_shift", p.scaleWarpWithShift);
    writeBool(out, "preservation.preserve_aperiodicity", p.preserveAperiodicity);
    writeBool(out, "preservation.vibrato_constant_in_cents", p.vibratoConstantInCents);
    writeEnum(out, "preservation.unvoiced", p.unvoiced, kUnvoicedPolicyTable);
    writeBool(out, "preservation.preserve_timing", p.preserveTiming);

    const auto& vp = p.voicing;
    writeBool(out, "preservation.voicing.enabled", vp.enabled);
    writeArith(out, "preservation.voicing.k_mu", vp.kMu);
    writeArith(out, "preservation.voicing.mu_strength", vp.muStrength);
    writeArith(out, "preservation.voicing.mu_min", vp.muMin);
    writeArith(out, "preservation.voicing.mu_max", vp.muMax);
    writeArith(out, "preservation.voicing.k_tilt", vp.kTilt);
    writeArith(out, "preservation.voicing.tilt_strength", vp.tiltStrength);
    writeArith(out, "preservation.voicing.tilt_gain_min", vp.tiltGainMin);
    writeArith(out, "preservation.voicing.tilt_gain_max", vp.tiltGainMax);
    writeArith(out, "preservation.voicing.tilt_corner_in_f0", vp.tiltCornerInF0);

    writeEnum(out, "blend.kind", tuning.kind, kBlendKindTable);
    writeArith(out, "blend.rs_crossover_hz", tuning.rsCrossoverHz);
    writeArith(out, "blend.rs_width_octaves", tuning.rsWidthOctaves);
    writeArith(out, "blend.oh_start_ms", tuning.ohStartMs);
    writeArith(out, "blend.oh_end_ms", tuning.ohEndMs);
    writeArith(out, "blend.alignment", tuning.alignment);

    const auto& h = tuning.humanize;
    writeArith(out, "humanize.detune_cents", h.detuneCents);
    writeArith(out, "humanize.detune_drift_cents", h.detuneDriftCents);
    writeArith(out, "humanize.onset_jitter_ms", h.onsetJitterMs);
    writeArith(out, "humanize.vibrato_rate_hz", h.vibratoRateHz);
    writeArith(out, "humanize.vibrato_depth_cents", h.vibratoDepthCents);
    writeArith(out, "humanize.envelope_warp_spread", h.envelopeWarpSpread);
    writeArith(out, "humanize.pan_spread", h.panSpread);

    writeArith(out, "decor_min_ms", tuning.decorMinMs);
    writeArith(out, "decor_max_ms", tuning.decorMaxMs);

    const auto& g = tuning.shifters.granular;
    writeArith(out, "shifters.granular.jump_periods", g.jumpPeriods);
    writeArith(out, "shifters.granular.fade_fraction", g.fadeFraction);
    writeArith(out, "shifters.granular.unvoiced_grain_ms", g.unvoicedGrainMs);
    writeArith(out, "shifters.granular.geometry_smooth_ms", g.geometrySmoothMs);

    writeArith(out, "shifters.psola.handover_ms", tuning.shifters.psola.handoverMs);

    const auto& a = tuning.analyzer;
    writeArith(out, "analyzer.threshold", a.threshold);
    writeArith(out, "analyzer.hop_samples", a.hopSamples);
    writeBool(out, "analyzer.hold_through_unvoiced", a.holdThroughUnvoiced);
    writeArith(out, "analyzer.max_hold_ms", a.maxHoldMs);
    writeArith(out, "analyzer.release_hops", a.releaseHops);
    writeArith(out, "analyzer.max_step_cents", a.maxStepCents);
    writeArith(out, "analyzer.jump_confirm_hops", a.jumpConfirmHops);
    writeArith(out, "analyzer.epoch_cutoff_ratio", a.epochCutoffRatio);
    writeArith(out, "analyzer.epoch_loudness_factor", a.epochLoudnessFactor);
    writeArith(out, "analyzer.epoch_refractory_factor", a.epochRefractoryFactor);
    writeArith(out, "analyzer.epoch_envelope_release", a.epochEnvelopeRelease);
    writeArith(out, "analyzer.epoch_cutoff_min_hz", a.epochCutoffMinHz);
    writeArith(out, "analyzer.epoch_cutoff_max_hz", a.epochCutoffMaxHz);

    writeArith(out, "input_trim_db", tuning.inputTrimDb);
    writeBool(out, "input_gate_on", tuning.inputGateOn);
    writeArith(out, "input_gate_db", tuning.inputGateDb);
    writeArith(out, "dry_gain", tuning.dryGain);
    writeArith(out, "wet_gain", tuning.wetGain);
    writeArith(out, "out_ceiling_db", tuning.outCeilingDb);
    writeBool(out, "limiter_on", tuning.limiterOn);

    writeString(out, "name", meta.name);
    writeString(out, "notes", meta.notes);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        error = "could not open '" + path + "' for writing";
        return false;
    }
    f << out;
    f.flush();
    if (!f) {
        error = "write error while saving '" + path + "'";
        return false;
    }
    return true;
}

// ============================================================================
// load()
// ============================================================================

bool load(const std::string& path, Tuning& tuning, ProfileMeta& meta, LoadReport& report) {
    report = LoadReport{};

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        report.error = "could not open '" + path + "' for reading";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::vector<KV> kvs;
    std::string err;
    if (!parseLines(text, kvs, err)) {
        report.error = err;
        return false;
    }
    if (!checkDuplicates(kvs, err)) {
        report.error = err;
        return false;
    }

    Reader r(kvs);

    // MIGRATION GUARANTEE: start from the compiled-in Tuning{} defaults, never from
    // whatever `tuning` held on entry. A key the file does not mention must resolve to
    // Tuning{}'s field, not to a stale caller value that happened to be lying around.
    Tuning t{};
    ProfileMeta m{};

    Consume c = consumeArith(r, "schema_version", t.schemaVersion, err);
    if (c == Consume::Error) { report.error = err; return false; }
    if (c == Consume::Absent) {
        report.error = "missing required key 'schema_version' (must be present -- "
                        "see docs/app-design.md §7)";
        return false;
    }
    report.fileSchemaVersion = t.schemaVersion;
    if (t.schemaVersion != kTuningSchemaVersion) {
        std::ostringstream os;
        os << "schema_version " << t.schemaVersion << " in '" << path
           << "' does not match this build's schema " << kTuningSchemaVersion
           << "; fields are matched by NAME, so additions and removals migrate safely "
              "(a new field takes its compiled default, a removed one is ignored below as "
              "an unknown key), but a field that kept its name while its MEANING changed "
              "would be silently misapplied -- this loader cannot detect that case, only "
              "name it.";
        report.warnings.push_back(os.str());
    }

    // Every remaining field: absence is fine (Tuning{}'s default already sits in `t`); a
    // present-but-malformed value is a hard error, immediately, before any more of the file
    // is touched -- see the file header on why a syntax/type problem is not "an unknown key
    // dressed differently".
#define VHP_LOAD(expr)                                                                     \
    do {                                                                                   \
        Consume _c = (expr);                                                               \
        if (_c == Consume::Error) { report.error = err; return false; }                    \
    } while (0)

    VHP_LOAD(consumeEnum(r, "mode", kRatioSourceTable, t.mode, err));
    VHP_LOAD(consumeArith(r, "root_midi_note", t.rootMidiNote, err));
    VHP_LOAD(consumeArith(r, "voice_cap", t.voiceCap, err));
    VHP_LOAD(consumeArith(r, "attack_ms", t.attackMs, err));
    VHP_LOAD(consumeArith(r, "release_ms", t.releaseMs, err));
    VHP_LOAD(consumeArith(r, "glide_semis_per_sec", t.glideSemisPerSec, err));
    VHP_LOAD(consumeArith(r, "freeze_window_ms", t.freezeWindowMs, err));
    VHP_LOAD(consumeArith(r, "velocity_to_gain_min", t.velocityToGainMin, err));
    VHP_LOAD(consumeArith(r, "velocity_curve", t.velocityCurve, err));

    VHP_LOAD(consumeBool(r, "preservation.preserve_envelope", t.preservation.preserveEnvelope, err));
    VHP_LOAD(consumeArith(r, "preservation.envelope_warp", t.preservation.envelopeWarp, err));
    VHP_LOAD(consumeBool(r, "preservation.scale_warp_with_shift", t.preservation.scaleWarpWithShift, err));
    VHP_LOAD(consumeBool(r, "preservation.preserve_aperiodicity", t.preservation.preserveAperiodicity, err));
    VHP_LOAD(consumeBool(r, "preservation.vibrato_constant_in_cents", t.preservation.vibratoConstantInCents, err));
    VHP_LOAD(consumeEnum(r, "preservation.unvoiced", kUnvoicedPolicyTable, t.preservation.unvoiced, err));
    VHP_LOAD(consumeBool(r, "preservation.preserve_timing", t.preservation.preserveTiming, err));

    auto& vp = t.preservation.voicing;
    VHP_LOAD(consumeBool(r, "preservation.voicing.enabled", vp.enabled, err));
    VHP_LOAD(consumeArith(r, "preservation.voicing.k_mu", vp.kMu, err));
    VHP_LOAD(consumeArith(r, "preservation.voicing.mu_strength", vp.muStrength, err));
    VHP_LOAD(consumeArith(r, "preservation.voicing.mu_min", vp.muMin, err));
    VHP_LOAD(consumeArith(r, "preservation.voicing.mu_max", vp.muMax, err));
    VHP_LOAD(consumeArith(r, "preservation.voicing.k_tilt", vp.kTilt, err));
    VHP_LOAD(consumeArith(r, "preservation.voicing.tilt_strength", vp.tiltStrength, err));
    VHP_LOAD(consumeArith(r, "preservation.voicing.tilt_gain_min", vp.tiltGainMin, err));
    VHP_LOAD(consumeArith(r, "preservation.voicing.tilt_gain_max", vp.tiltGainMax, err));
    VHP_LOAD(consumeArith(r, "preservation.voicing.tilt_corner_in_f0", vp.tiltCornerInF0, err));

    VHP_LOAD(consumeEnum(r, "blend.kind", kBlendKindTable, t.kind, err));
    VHP_LOAD(consumeArith(r, "blend.rs_crossover_hz", t.rsCrossoverHz, err));
    VHP_LOAD(consumeArith(r, "blend.rs_width_octaves", t.rsWidthOctaves, err));
    VHP_LOAD(consumeArith(r, "blend.oh_start_ms", t.ohStartMs, err));
    VHP_LOAD(consumeArith(r, "blend.oh_end_ms", t.ohEndMs, err));
    VHP_LOAD(consumeArith(r, "blend.alignment", t.alignment, err));

    auto& h = t.humanize;
    VHP_LOAD(consumeArith(r, "humanize.detune_cents", h.detuneCents, err));
    VHP_LOAD(consumeArith(r, "humanize.detune_drift_cents", h.detuneDriftCents, err));
    VHP_LOAD(consumeArith(r, "humanize.onset_jitter_ms", h.onsetJitterMs, err));
    VHP_LOAD(consumeArith(r, "humanize.vibrato_rate_hz", h.vibratoRateHz, err));
    VHP_LOAD(consumeArith(r, "humanize.vibrato_depth_cents", h.vibratoDepthCents, err));
    VHP_LOAD(consumeArith(r, "humanize.envelope_warp_spread", h.envelopeWarpSpread, err));
    VHP_LOAD(consumeArith(r, "humanize.pan_spread", h.panSpread, err));

    VHP_LOAD(consumeArith(r, "decor_min_ms", t.decorMinMs, err));
    VHP_LOAD(consumeArith(r, "decor_max_ms", t.decorMaxMs, err));

    auto& g = t.shifters.granular;
    VHP_LOAD(consumeArith(r, "shifters.granular.jump_periods", g.jumpPeriods, err));
    VHP_LOAD(consumeArith(r, "shifters.granular.fade_fraction", g.fadeFraction, err));
    VHP_LOAD(consumeArith(r, "shifters.granular.unvoiced_grain_ms", g.unvoicedGrainMs, err));
    VHP_LOAD(consumeArith(r, "shifters.granular.geometry_smooth_ms", g.geometrySmoothMs, err));

    VHP_LOAD(consumeArith(r, "shifters.psola.handover_ms", t.shifters.psola.handoverMs, err));

    auto& a = t.analyzer;
    VHP_LOAD(consumeArith(r, "analyzer.threshold", a.threshold, err));
    VHP_LOAD(consumeArith(r, "analyzer.hop_samples", a.hopSamples, err));
    VHP_LOAD(consumeBool(r, "analyzer.hold_through_unvoiced", a.holdThroughUnvoiced, err));
    VHP_LOAD(consumeArith(r, "analyzer.max_hold_ms", a.maxHoldMs, err));
    VHP_LOAD(consumeArith(r, "analyzer.release_hops", a.releaseHops, err));
    VHP_LOAD(consumeArith(r, "analyzer.max_step_cents", a.maxStepCents, err));
    VHP_LOAD(consumeArith(r, "analyzer.jump_confirm_hops", a.jumpConfirmHops, err));
    VHP_LOAD(consumeArith(r, "analyzer.epoch_cutoff_ratio", a.epochCutoffRatio, err));
    VHP_LOAD(consumeArith(r, "analyzer.epoch_loudness_factor", a.epochLoudnessFactor, err));
    VHP_LOAD(consumeArith(r, "analyzer.epoch_refractory_factor", a.epochRefractoryFactor, err));
    VHP_LOAD(consumeArith(r, "analyzer.epoch_envelope_release", a.epochEnvelopeRelease, err));
    VHP_LOAD(consumeArith(r, "analyzer.epoch_cutoff_min_hz", a.epochCutoffMinHz, err));
    VHP_LOAD(consumeArith(r, "analyzer.epoch_cutoff_max_hz", a.epochCutoffMaxHz, err));

    VHP_LOAD(consumeArith(r, "input_trim_db", t.inputTrimDb, err));
    VHP_LOAD(consumeBool(r, "input_gate_on", t.inputGateOn, err));
    VHP_LOAD(consumeArith(r, "input_gate_db", t.inputGateDb, err));
    VHP_LOAD(consumeArith(r, "dry_gain", t.dryGain, err));
    VHP_LOAD(consumeArith(r, "wet_gain", t.wetGain, err));
    VHP_LOAD(consumeArith(r, "out_ceiling_db", t.outCeilingDb, err));
    VHP_LOAD(consumeBool(r, "limiter_on", t.limiterOn, err));

    VHP_LOAD(consumeString(r, "name", m.name, err));
    VHP_LOAD(consumeString(r, "notes", m.notes, err));

#undef VHP_LOAD

    // Everything left unclaimed is a key this build's loader never asked for -- either a
    // typo, or a genuinely newer field from a future schema. Either way: warn, don't fail.
    for (const auto& kv : kvs) {
        if (!kv.claimed) {
            std::ostringstream os;
            os << "line " << kv.line << ": unknown key '" << kv.key << "' ignored";
            report.warnings.push_back(os.str());
        }
    }

    // MANDATORY, unconditional: a .vhprofile is untrusted, hand-editable input (app-design.md
    // §4.5 item 1's own framing), and this is the one call in the whole path that stands
    // between a "hopSamples = 0" line and an audio thread that never returns from its
    // analysis hop loop.
    t.clamp();

    tuning = t;
    meta = m;
    report.ok = true;
    return true;
}

} // namespace vh::profile
