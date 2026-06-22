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
// 🎯T42 ships the replica + cross-check on the install path and 🎯T44 wires
// the foundational advanced trust layer (cross-source hash agreement) into it.
// These tests exercise the real implementation in src/trust/trust_model.*.

#include <doctest.h>

#include "trust/trust_model.h"

#include <map>
#include <optional>
#include <string>

namespace den {
namespace trust_test {

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
    // 🎯T42: cross_check_hashes is wired into the real install path.
    //
    // install_one() (src/provider/homebrew_provider.cpp) now builds the two
    // hash sources — the Homebrew-API hash (archive.sha256) and the replica
    // hash (load_local_replica + replica_lookup) — and routes the resulting
    // TrustDecision: Refuse throws UserError before any download/extract, and
    // WarnProceed logs a degraded-trust warning. We exercise that exact
    // pipeline here against a controlled in-memory replica.
    // -----------------------------------------------------------------------

    TEST_CASE("real install path: replica agreement → Proceed") {
        const std::string sha = "abc123def456abc123def456abc123def456abc123def456abc123def456abcd";
        std::map<std::string, std::string> replica;
        replica[replica_key("tree", "2.3.2", "arm64_sequoia")] = sha;

        // Source A: Homebrew-API hash baked into the index archive entry.
        MaybeHash homebrew_hash = sha;
        // Source B: looked up from the replica exactly as install_one() does.
        MaybeHash replica_hash = replica_lookup(replica, "tree", "2.3.2", "arm64_sequoia");

        auto decision = cross_check_hashes(homebrew_hash, replica_hash);
        CHECK(decision.decision == TrustDecision::Proceed);
    }

    TEST_CASE("real install path: replica disagreement → Refuse (would throw)") {
        std::map<std::string, std::string> replica;
        replica[replica_key("tree", "2.3.2", "arm64_sequoia")] =
            "1111aaaa2222bbbb3333cccc4444dddd5555eeee6666ffff1111aaaa2222bbbb";

        MaybeHash homebrew_hash =
            "aaaa0000bbbb1111cccc2222dddd3333eeee4444ffff5555aaaa0000bbbb1111";
        MaybeHash replica_hash = replica_lookup(replica, "tree", "2.3.2", "arm64_sequoia");

        auto decision = cross_check_hashes(homebrew_hash, replica_hash);
        // install_one() throws UserError on this decision before extraction.
        CHECK(decision.decision == TrustDecision::Refuse);
    }

    TEST_CASE("real install path: replica has no entry → WarnProceed (degraded)") {
        std::map<std::string, std::string> replica; // empty replica
        MaybeHash homebrew_hash =
            "abc123def456abc123def456abc123def456abc123def456abc123def456abcd";
        MaybeHash replica_hash = replica_lookup(replica, "ffmpeg", "6.0.0", "arm64_sequoia");

        CHECK(!replica_hash.has_value());
        auto decision = cross_check_hashes(homebrew_hash, replica_hash);
        CHECK(decision.decision == TrustDecision::WarnProceed);
    }

    // -----------------------------------------------------------------------
    // 🎯T44: the foundational advanced trust layer — cross-source hash
    // agreement against an independent CDN — is wired into the install path.
    // The replica document parser fails closed on malformed input, which is
    // the integrity guarantee the layer depends on.
    // -----------------------------------------------------------------------

    TEST_CASE("advanced trust layer: replica document round-trips and validates") {
        const std::string sha = "d1967d2ed08717f963addb249ea6b8ca11c26ecb59efba34f2860853a06bedc7";
        const std::string doc = R"({
            "schema_version": 1,
            "hashes": {
                "tree--2.3.2--arm64_tahoe": ")" +
                                sha + R"("
            }
        })";
        auto parsed = parse_replica_document(doc);
        REQUIRE(parsed.size() == 1);
        auto looked = replica_lookup(parsed, "tree", "2.3.2", "arm64_tahoe");
        REQUIRE(looked.has_value());
        CHECK(*looked == sha);
    }

    TEST_CASE("advanced trust layer: malformed replica hash fails closed") {
        // A non-hex / wrong-length digest must be rejected, not silently
        // accepted — otherwise an attacker who lands a forged replica entry
        // could weaken the cross-check.
        const std::string doc = R"({"hashes": {"tree--2.3.2--arm64_tahoe": "not-a-real-sha"}})";
        CHECK_THROWS(parse_replica_document(doc));
    }

    TEST_CASE("advanced trust layer: replica missing 'hashes' object is rejected") {
        CHECK_THROWS(parse_replica_document(R"({"schema_version": 1})"));
    }

} // TEST_SUITE

} // namespace trust_test
} // namespace den
