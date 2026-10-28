// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// 🎯T64 / 🎯T42 / 🎯T44: independently-verifiable trust model.
//
// den consults TWO hash sources on every install:
//
//   Source A: formulae.brew.sh / Homebrew's API   (the bottle metadata index)
//   Source B: den's replica snapshot (data/known_hashes.json), shipped with
//             the den release (share/den/known_hashes.json or the copy
//             compiled into the binary), or a user override in
//             <den_home>/trust/known_hashes.json.
//
// Nothing is fetched at install time: B is the LOCAL snapshot. Its entries
// are seeded by CI (replica-verify.yml) from formulae.brew.sh's own hashes,
// each confirmed by pulling that bottle from GHCR, so B is an earlier,
// GHCR-confirmed copy of A rather than an independent root: it catches A
// changing a hash after the snapshot was taken, not a bottle that was bad
// when it was seeded.
//
// The cross-check decision table:
//   A == B                     → Proceed     (both sources agree)
//   A != B                     → Refuse      (mismatch — hard error)
//   A present, no replica entry → WarnProceed (degraded: bottle not covered)
//   A present, no snapshot      → WarnProceed (degraded: replica inactive)
//   snapshot unreadable/corrupt → Refuse      (fail closed)
//   A absent, B present         → WarnProceed (degraded trust)
//   neither                     → Refuse      (cannot verify integrity)

#include "../core/config.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace den {

namespace fs = std::filesystem;

struct ReplicaSource;

/// Result of a cross-source hash check.
enum class TrustDecision {
    Proceed,     // both sources agree
    Refuse,      // sources disagree (or neither reachable) — hard error
    WarnProceed, // one source unreachable — degraded trust, proceed with warning
};

struct TrustCheckResult {
    TrustDecision decision;
    std::string message; // human-readable reason
};

/// Source of a bottle hash.  nullopt means "unreachable / unavailable".
using MaybeHash = std::optional<std::string>;

/// Why the replica did (or did not) yield a hash for a bottle.
enum class ReplicaLookupStatus {
    Found,      ///< the snapshot has an entry for this bottle
    NoEntry,    ///< the snapshot loaded but does not cover this bottle
    Inactive,   ///< no snapshot at all (none on disk, none embedded)
    Unreadable, ///< a snapshot exists but cannot be read or parsed
};

/// The replica's answer for one bottle.
struct ReplicaAnswer {
    ReplicaLookupStatus status = ReplicaLookupStatus::Inactive;
    MaybeHash hash;     ///< set iff status == Found
    std::string source; ///< ReplicaSource::describe() of the snapshot consulted
    std::string error;  ///< set iff status == Unreadable
    size_t entries = 0; ///< entries in the snapshot (0 unless it loaded)
};

/// Short machine-readable tag for a degraded reason ("no-replica-entry",
/// "replica-inactive", "replica-unreadable", "homebrew-hash-missing").
const char* replica_status_tag(ReplicaLookupStatus status);

/// Pure cross-check logic, dependency-injected so it is testable without
/// network access.  Callers supply the Homebrew-API hash (already known from
/// the index) and the replica's answer.  See the decision table above. An
/// Unreadable replica refuses (fail closed): a corrupted snapshot must not
/// quietly turn every install into a single-source one.
TrustCheckResult cross_check_hashes(const MaybeHash& homebrew_hash, const ReplicaAnswer& replica);

/// Two-hash form: a missing replica hash is treated as NoEntry.
TrustCheckResult cross_check_hashes(const MaybeHash& homebrew_hash, const MaybeHash& replica_hash);

/// Look a bottle up in the local replica snapshot, reporting why there is no
/// hash when there isn't one. Never throws: a corrupt snapshot comes back as
/// Unreadable with the error text.
ReplicaAnswer lookup_local_replica(const Config& config, const std::string& formula,
                                   const std::string& version, const std::string& platform_tag);

/// lookup_local_replica against an explicit source (testable).
ReplicaAnswer lookup_replica(const ReplicaSource& source, std::string_view embedded,
                             const std::string& formula, const std::string& version,
                             const std::string& platform_tag);

// ---------------------------------------------------------------------------
// Replica snapshot (known_hashes.json).
//
// The replica is a flat map keyed by an opaque "<formula>--<version>--<tag>"
// identifier whose value is the verified SHA256 of that bottle.  The file is
// produced by the diff-verify CI pipeline (replica-verify.yml) and bundled
// with den so an offline install still has something to cross-check against.
// ---------------------------------------------------------------------------

/// Build the canonical replica key for a bottle.
std::string replica_key(const std::string& formula, const std::string& version,
                        const std::string& platform_tag);

/// Parse a known_hashes.json document (the "hashes" object) into a flat map.
/// Throws UserError on malformed JSON or a non-conforming schema (fail closed:
/// a corrupt replica must not silently degrade to "no replica").
std::map<std::string, std::string> parse_replica_document(const std::string& json_text);

/// Where the local replica snapshot comes from.
struct ReplicaSource {
    enum class Kind {
        File,     ///< an on-disk known_hashes.json (path is set)
        Embedded, ///< the snapshot compiled into the binary
        None,     ///< nothing available (replica source inactive)
    };
    Kind kind = Kind::None;
    fs::path path; ///< set when kind == File

    /// Human-readable description for diagnostics ("embedded snapshot" or the path).
    std::string describe() const;
};

/// The replica snapshot compiled into the binary from data/known_hashes.json
/// at build time (generated source; see CMakeLists.txt).
std::string_view embedded_replica_json();

/// Locate the local replica snapshot. Search order (first existing wins):
///   1. <den_home>/trust/known_hashes.json      — user-synced override.
///   2. <exe_dir>/../share/den/known_hashes.json — packaged with the binary
///      (release tarball via install.sh, `cmake --install`, Homebrew). Tried
///      for the executable path as reported and its symlink-resolved target.
///   3. <exe_dir>/share/den/known_hashes.json   — an unpacked release tarball.
///   4. <exe_dir>/../data/known_hashes.json     — dev build at <repo>/build/den.
///   5. the embedded snapshot (when non-empty).
/// Nothing is resolved relative to the current working directory. `exe` is
/// the running executable (nullopt = unknown; only 1 and 5 apply).
ReplicaSource locate_local_replica(const Config& config, const std::optional<fs::path>& exe,
                                   std::string_view embedded);

/// locate_local_replica for the running executable and the embedded snapshot.
ReplicaSource locate_local_replica(const Config& config);

/// Load a replica from a located source. Empty map for Kind::None; throws
/// UserError (fail closed) on a corrupt file or embedded document.
std::map<std::string, std::string> load_replica(const ReplicaSource& source,
                                                std::string_view embedded);

/// Load the bundled replica snapshot shipped alongside den.  Returns an empty
/// map if the snapshot is absent (treated as "replica source inactive").
std::map<std::string, std::string> load_local_replica(const Config& config);

/// Path to the on-disk local replica snapshot, or the canonical den_home
/// location (for diagnostics) when only the embedded snapshot is available.
fs::path local_replica_path(const Config& config);

/// Look up a single bottle hash in a replica map.  nullopt = not present.
MaybeHash replica_lookup(const std::map<std::string, std::string>& replica,
                         const std::string& formula, const std::string& version,
                         const std::string& platform_tag);

// ---------------------------------------------------------------------------
// Diff-based replica verifier (used by the replica-verify CI pipeline).
// ---------------------------------------------------------------------------

/// A single entry in a replica diff.
struct ReplicaDiffEntry {
    std::string formula_name;
    std::string version;
    std::string platform_tag;
    std::string claimed_sha256; // SHA256 claimed by the replica diff record
};

/// Outcome of verifying a single diff entry against a downloaded bottle.
enum class DiffVerifyOutcome {
    Match,    // claimed SHA matches actual bottle
    Mismatch, // claimed SHA does NOT match — fabrication detected
};

/// Verify a single diff entry against a bottle file on disk.  In production
/// the file is freshly downloaded from GHCR, so a hash formulae.brew.sh claims
/// is only committed to the replica if GHCR serves a bottle with that digest.
/// A file that
/// cannot be hashed is treated as a mismatch (fail closed).
DiffVerifyOutcome verify_diff_entry(const ReplicaDiffEntry& entry, const fs::path& bottle_path);

} // namespace den
