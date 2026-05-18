// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T64 verification harness: cross-source hash agreement checks.
//
// The trust model (🎯T42 + 🎯T44) requires that den consult TWO independent
// hash sources on every install:
//
//   Source A: formulae.brew.sh / Homebrew's API   (homebrew_hash)
//   Source B: den's own replica on a separate CDN  (replica_hash)
//
// Decision table:
//   A == B                → install proceeds
//   A != B (both reachable)→ install refused (mismatch error)
//   A reachable, B absent  → warn and proceed (degraded trust)
//   A absent, B reachable  → warn and proceed (degraded trust)
//
// Until 🎯T42 ships a real replica CDN and 🎯T44 wires an advanced trust
// layer into the install path, these tests use a minimal inline mock that
// captures the *interface contract* the real implementation must satisfy.
// Tests that require absent upstream code skip via DOCTEST_SKIP.

#include <doctest.h>

#include <optional>
#include <string>

namespace den {
namespace trust_test {

// ---------------------------------------------------------------------------
// Minimal trust model interface (mirrors what 🎯T42/T44 must expose).
// Replace with real includes once the implementation lands.
// ---------------------------------------------------------------------------

/// Result of a cross-source hash check.
enum class TrustDecision {
    Proceed,     // both sources agree
    Refuse,      // sources disagree — hard error
    WarnProceed, // one source unreachable — degraded trust, proceed with warning
};

struct TrustCheckResult {
    TrustDecision decision;
    std::string message; // human-readable reason
};

/// Source of a bottle hash.  nullopt means "unreachable / unavailable".
using MaybeHash = std::optional<std::string>;

/// Pure cross-check logic, dependency-injected so it is testable without
/// network access.  The real implementation calls formulae.brew.sh and the
/// den replica CDN; tests supply controlled mock values.
TrustCheckResult cross_check_hashes(const MaybeHash& homebrew_hash, const MaybeHash& replica_hash) {
    // Neither source reachable: cannot verify anything.
    if (!homebrew_hash && !replica_hash) {
        return {TrustDecision::Refuse,
                "both hash sources unreachable — cannot verify bottle integrity"};
    }

    // Both reachable: require agreement.
    if (homebrew_hash && replica_hash) {
        if (*homebrew_hash == *replica_hash) {
            return {TrustDecision::Proceed, "hashes agree"};
        }
        return {TrustDecision::Refuse,
                "hash mismatch: formulae.brew.sh=" + *homebrew_hash + " replica=" + *replica_hash};
    }

    // Exactly one reachable: degraded-trust fallback.
    return {TrustDecision::WarnProceed,
            homebrew_hash ? "replica unreachable — using Homebrew hash only"
                          : "formulae.brew.sh unreachable — using replica hash only"};
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_SUITE("trust_model::cross_check_hashes") {

    TEST_CASE("agreement: both sources return same hash → proceed") {
        const std::string sha = "abc123def456abc123def456abc123def456abc123def456abc123def456abcd";
        auto result = cross_check_hashes(sha, sha);
        CHECK(result.decision == TrustDecision::Proceed);
        CHECK(result.message.find("agree") != std::string::npos);
    }

    TEST_CASE("disagreement: hashes differ → refuse install") {
        const MaybeHash homebrew_hash =
            "aaaa0000bbbb1111cccc2222dddd3333eeee4444ffff5555aaaa0000bbbb1111";
        const MaybeHash replica_hash =
            "1111aaaa2222bbbb3333cccc4444dddd5555eeee6666ffff1111aaaa2222bbbb";
        auto result = cross_check_hashes(homebrew_hash, replica_hash);
        CHECK(result.decision == TrustDecision::Refuse);
        CHECK(result.message.find("mismatch") != std::string::npos);
        // Both hashes should appear in the error for auditability.
        CHECK(result.message.find(*homebrew_hash) != std::string::npos);
        CHECK(result.message.find(*replica_hash) != std::string::npos);
    }

    TEST_CASE("fallback: replica unreachable → warn and proceed") {
        const MaybeHash homebrew_hash =
            "abc123def456abc123def456abc123def456abc123def456abc123def456abcd";
        const MaybeHash replica_hash = std::nullopt;
        auto result = cross_check_hashes(homebrew_hash, replica_hash);
        CHECK(result.decision == TrustDecision::WarnProceed);
        CHECK(result.message.find("replica") != std::string::npos);
    }

    TEST_CASE("fallback: formulae.brew.sh unreachable → warn and proceed") {
        const MaybeHash homebrew_hash = std::nullopt;
        const MaybeHash replica_hash =
            "abc123def456abc123def456abc123def456abc123def456abc123def456abcd";
        auto result = cross_check_hashes(homebrew_hash, replica_hash);
        CHECK(result.decision == TrustDecision::WarnProceed);
        CHECK(result.message.find("formulae.brew.sh") != std::string::npos);
    }

    TEST_CASE("neither source reachable → refuse (cannot verify)") {
        auto result = cross_check_hashes(std::nullopt, std::nullopt);
        CHECK(result.decision == TrustDecision::Refuse);
        CHECK(result.message.find("unreachable") != std::string::npos);
    }

    // -----------------------------------------------------------------------
    // Upstream gap guards (skipped via doctest::skip() decorator).
    // Remove the * doctest::skip() once the upstream target is implemented.
    // -----------------------------------------------------------------------

    // 🎯T42: cross_check_hashes not yet wired into install_one().
    TEST_CASE("real install path invokes cross_check_hashes (🎯T42 gap)" * doctest::skip(true)) {
        // When T42 is implemented:
        //   - install_one() must call cross_check_hashes() before extracting.
        //   - A TrustDecision::Refuse must throw or return early.
        //   - A TrustDecision::WarnProceed must emit a visible warning.
        CHECK(false); // placeholder — must be replaced with real assertions
    }

    // 🎯T44: advanced trust layers not yet wired into install path.
    TEST_CASE("advanced trust layers wired into install path (🎯T44 gap)" * doctest::skip(true)) {
        // When T44 is implemented:
        //   - Sigstore / SLSA provenance attestation should be verified.
        //   - Trust policy should be configurable in config.json.
        CHECK(false); // placeholder — must be replaced with real assertions
    }

} // TEST_SUITE

} // namespace trust_test
} // namespace den
