// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// 🎯T64 / 🎯T42 / 🎯T44: independently-verifiable trust model.
//
// den consults TWO independent hash sources on every install:
//
//   Source A: formulae.brew.sh / Homebrew's API   (the bottle metadata index)
//   Source B: den's own replica on a SEPARATE CDN  (raw.githubusercontent.com)
//
// raw.githubusercontent.com is a different origin and CDN from
// formulae.brew.sh, so an attacker must compromise BOTH to swap a bottle:
// they would have to forge a matching SHA256 in Homebrew's API *and* land a
// matching forged hash in den's GitHub repo (which is diff-verified against
// GHCR by a CI pipeline before it is committed — see verify_diff_entry).
//
// The cross-check decision table:
//   A == B                 → Proceed     (both sources agree)
//   A != B (both reachable) → Refuse      (mismatch — hard error)
//   A reachable, B absent   → WarnProceed (degraded trust)
//   A absent, B reachable   → WarnProceed (degraded trust)
//   neither reachable       → Refuse      (cannot verify integrity)

#include "../core/config.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>

namespace den {

namespace fs = std::filesystem;

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

/// Pure cross-check logic, dependency-injected so it is testable without
/// network access.  Callers supply the Homebrew-API hash (already known from
/// the index) and the replica hash (from fetch_replica_hash / the local
/// replica snapshot).  See the decision table above.
TrustCheckResult cross_check_hashes(const MaybeHash& homebrew_hash, const MaybeHash& replica_hash);

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

/// Load the bundled replica snapshot shipped alongside den.  Returns an empty
/// map if the snapshot is absent (treated as "replica source inactive").
std::map<std::string, std::string> load_local_replica(const Config& config);

/// Path to the local replica snapshot (known_hashes.json) shipped with den.
fs::path local_replica_path(const Config& config);

/// Look up a single bottle hash in a replica map.  nullopt = not present.
MaybeHash replica_lookup(const std::map<std::string, std::string>& replica,
                         const std::string& formula, const std::string& version,
                         const std::string& platform_tag);

/// Fetch the replica snapshot from den's separate CDN
/// (raw.githubusercontent.com).  Returns nullopt on any network/parse failure
/// so the caller can fall back to the local snapshot or degrade gracefully.
/// This performs network I/O; install_one() prefers the bundled snapshot and
/// only reaches for the network when explicitly enabled.
std::optional<std::map<std::string, std::string>> fetch_replica_hashes();

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
/// the file is freshly downloaded from GHCR, so an attacker must control both
/// formulae.brew.sh AND den's replica CDN to pass this check.  A file that
/// cannot be hashed is treated as a mismatch (fail closed).
DiffVerifyOutcome verify_diff_entry(const ReplicaDiffEntry& entry, const fs::path& bottle_path);

} // namespace den
