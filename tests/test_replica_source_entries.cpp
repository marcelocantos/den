// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// The shipped replica (data/known_hashes.json, also compiled into the binary)
// carries a "<name>--<version>--source" entry for every formula in the
// default trust.source_build_allowlist, so an unattended allowlisted source
// build can cross-check its source hash instead of deferring.

#include <doctest.h>

#include "settings/settings.h"
#include "trust/trust_model.h"
#include "trust/unattended.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using namespace den;

namespace {

std::string slurp_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::map<std::string, std::string> shipped_replica() {
    return parse_replica_document(
        slurp_file(fs::path(DEN_SOURCE_DIR) / "data" / "known_hashes.json"));
}

// "<formula>--<version>--source" keys by formula: version -> sha.
std::map<std::string, std::map<std::string, std::string>>
source_entries(const std::map<std::string, std::string>& replica) {
    std::map<std::string, std::map<std::string, std::string>> out;
    const std::string suffix = "--source";
    for (const auto& [key, sha] : replica) {
        if (key.size() <= suffix.size() ||
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        auto stem = key.substr(0, key.size() - suffix.size());
        auto sep = stem.find("--");
        REQUIRE_MESSAGE(sep != std::string::npos, key);
        out[stem.substr(0, sep)][stem.substr(sep + 2)] = sha;
    }
    return out;
}

} // namespace

TEST_SUITE("replica_source_entries") {

    TEST_CASE("every default allowlisted formula has a source entry") {
        auto sources = source_entries(shipped_replica());
        for (const auto& name : TrustSettings{}.source_build_allowlist) {
            INFO(name);
            CHECK(sources.count(name) == 1);
        }
    }

    TEST_CASE("source entries are well-formed SHA256s") {
        for (const auto& [name, versions] : source_entries(shipped_replica()))
            for (const auto& [version, sha] : versions) {
                INFO(name << " " << version);
                CHECK(!version.empty());
                CHECK(version.find('_') == std::string::npos); // upstream version, no revision
                CHECK(sha.size() == 64);
                CHECK(sha.find_first_not_of("0123456789abcdef") == std::string::npos);
            }
    }

    TEST_CASE("the embedded snapshot resolves source entries") {
        ReplicaSource embedded{ReplicaSource::Kind::Embedded, {}};
        for (const auto& [name, versions] : source_entries(shipped_replica()))
            for (const auto& [version, sha] : versions) {
                INFO(name << " " << version);
                auto answer =
                    lookup_replica(embedded, embedded_replica_json(), name, version, "source");
                REQUIRE(answer.status == ReplicaLookupStatus::Found);
                CHECK(answer.hash == sha);
                // A bottle tag never matches a source entry.
                CHECK(
                    lookup_replica(embedded, embedded_replica_json(), name, version, "arm64_tahoe")
                        .hash != sha);
            }
    }

    TEST_CASE("an unattended allowlisted build cross-checks and builds") {
        TrustSettings settings;
        settings.unattended = "refuse";
        settings.unattended_source_builds = "allowlist";
        auto policy = make_trust_policy(settings, std::string("daemon"));
        ReplicaSource embedded{ReplicaSource::Kind::Embedded, {}};
        auto sources = source_entries(shipped_replica());
        for (const auto& name : settings.source_build_allowlist) {
            INFO(name);
            REQUIRE(sources.count(name) == 1);
            const auto& [version, sha] = *sources[name].rbegin();
            auto replica =
                lookup_replica(embedded, embedded_replica_json(), name, version, "source");

            auto agree = cross_check_hashes(MaybeHash(sha), replica);
            CHECK(agree.decision == TrustDecision::Proceed);
            CHECK(source_build_action(policy, name, agree.decision, false) ==
                  SourceBuildAction::Build);

            // A different Homebrew hash still refuses.
            auto disagree = cross_check_hashes(MaybeHash(std::string(64, '0')), replica);
            CHECK(disagree.decision == TrustDecision::Refuse);
            CHECK(source_build_action(policy, name, disagree.decision, false) ==
                  SourceBuildAction::Refuse);

            // A version the replica does not cover still defers.
            auto missing =
                lookup_replica(embedded, embedded_replica_json(), name, version + ".999", "source");
            CHECK(missing.status == ReplicaLookupStatus::NoEntry);
            auto single = cross_check_hashes(MaybeHash(sha), missing);
            CHECK(source_build_action(policy, name, single.decision, false) ==
                  SourceBuildAction::Defer);
        }
    }
}
