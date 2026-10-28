// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Trust honesty: the cross-check says what actually happened.
//
//   • "no replica entry for this bottle" is distinct from "no replica
//     snapshot" and from "replica snapshot unreadable".
//   • An unreadable (corrupt) snapshot refuses the install — fail closed —
//     instead of quietly degrading every install to a single source.
//   • Installs that did proceed on a single source are recorded, counted by
//     `den doctor` and `den daemon status`, and drop out of the counts once
//     re-poured with both sources agreeing or once the keg is gone.

#include "cli/daemon_status.h"
#include "core/config.h"
#include "core/error.h"
#include "doctor/trust_checks.h"
#include "download/sha256.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/exec.h"
#include "provider/homebrew_provider.h"
#include "trust/degraded_installs.h"
#include "trust/trust_model.h"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace den;

namespace {

const std::string kSha(64, 'a');
const std::string kOtherSha(64, 'b');

struct Tmp {
    fs::path path;
    explicit Tmp(const std::string& tag) {
        path = fs::temp_directory_path() /
               ("den-trust-honesty-" + tag + "-" + std::to_string(::getpid()));
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Tmp(const Tmp&) = delete;
    Tmp& operator=(const Tmp&) = delete;
};

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string replica_doc(const std::string& key, const std::string& sha) {
    return R"({"schema_version": 1, "hashes": {")" + key + R"(": ")" + sha + R"("}})";
}

Config tmp_config(const fs::path& tmp) {
    Config cfg;
    cfg.den_home = tmp / "den";
    cfg.cache = cfg.den_home / "cache";
    cfg.store = tmp / "prefix" / "Cellar";
    cfg.prefix = cfg.store.parent_path();
    cfg.homebrew_cellar = cfg.store;
    fs::create_directories(cfg.cache / "archives");
    fs::create_directories(cfg.store);
    return cfg;
}

// A cached bottle for probe 1.0 and an index entry pointing at it, so
// install() pours without touching the network.
struct Fixture {
    Tmp tmp;
    Config cfg;
    PackageIndex idx;
    std::string sha;

    explicit Fixture(const std::string& tag) : tmp(tag), cfg(tmp_config(tmp.path)) {
        auto stage = tmp.path / "stage";
        write(stage / "probe" / "1.0" / "bin" / "probe", "#!/bin/sh\necho ok\n");
        auto tarball = tmp.path / "probe.tar.gz";
        REQUIRE(
            run_tool({"tar", "-czf", tarball.string(), "-C", stage.string(), "probe"}).exit_code ==
            0);
        sha = hash_file(tarball);
        fs::copy_file(tarball, cfg.cache / "archives" / (sha + ".tar.gz"));

        Package p;
        p.name = "probe";
        p.version = "1.0";
        Package::Archive ar;
        ar.url = "https://bottles.invalid/probe.tar.gz";
        ar.sha256 = sha;
        ar.relocation = RelocationType::Portable;
        p.archives["default"] = ar;
        idx.packages["probe"] = p;
    }

    fs::path override_replica() const { return cfg.den_home / "trust" / "known_hashes.json"; }
    fs::path keg() const { return cfg.store / "probe" / "1.0"; }

    std::string install_error() {
        HomebrewProvider provider(&idx);
        try {
            provider.install(cfg, "probe", "");
        } catch (const UserError& e) {
            return e.what();
        }
        return {};
    }
};

} // namespace

TEST_SUITE("trust_honesty::messages") {

    TEST_CASE("no replica entry is reported as such, not as 'unreachable'") {
        ReplicaAnswer r;
        r.status = ReplicaLookupStatus::NoEntry;
        r.source = "/x/known_hashes.json";
        r.entries = 12;
        auto res = cross_check_hashes(MaybeHash(kSha), r);
        CHECK(res.decision == TrustDecision::WarnProceed);
        CHECK(res.message.find("no replica entry for this bottle") != std::string::npos);
        CHECK(res.message.find("/x/known_hashes.json (12 entries)") != std::string::npos);
        CHECK(res.message.find("unreachable") == std::string::npos);
        CHECK(res.message.find("unreadable") == std::string::npos);
    }

    TEST_CASE("no snapshot at all is reported as 'no replica snapshot'") {
        ReplicaAnswer r;
        r.status = ReplicaLookupStatus::Inactive;
        auto res = cross_check_hashes(MaybeHash(kSha), r);
        CHECK(res.decision == TrustDecision::WarnProceed);
        CHECK(res.message.find("no replica snapshot") != std::string::npos);
    }

    TEST_CASE("an unreadable snapshot refuses, naming the file and the error") {
        ReplicaAnswer r;
        r.status = ReplicaLookupStatus::Unreadable;
        r.source = "/x/known_hashes.json";
        r.error = "parse error at byte 1";
        auto res = cross_check_hashes(MaybeHash(kSha), r);
        CHECK(res.decision == TrustDecision::Refuse);
        CHECK(res.message.find("unreadable") != std::string::npos);
        CHECK(res.message.find("/x/known_hashes.json") != std::string::npos);
        CHECK(res.message.find("parse error at byte 1") != std::string::npos);
    }

    TEST_CASE("an unreadable snapshot refuses even when formulae.brew.sh gave no hash") {
        ReplicaAnswer r;
        r.status = ReplicaLookupStatus::Unreadable;
        CHECK(cross_check_hashes(std::nullopt, r).decision == TrustDecision::Refuse);
    }

    TEST_CASE("two-hash form: a missing replica hash is a missing entry") {
        auto res = cross_check_hashes(MaybeHash(kSha), MaybeHash());
        CHECK(res.decision == TrustDecision::WarnProceed);
        CHECK(res.message.find("no replica entry") != std::string::npos);
    }

    TEST_CASE("reason tags") {
        CHECK(std::string(replica_status_tag(ReplicaLookupStatus::NoEntry)) == "no-replica-entry");
        CHECK(std::string(replica_status_tag(ReplicaLookupStatus::Inactive)) == "replica-inactive");
        CHECK(std::string(replica_status_tag(ReplicaLookupStatus::Unreadable)) ==
              "replica-unreadable");
    }
}

TEST_SUITE("trust_honesty::lookup") {

    TEST_CASE("lookup_replica distinguishes found, no entry, inactive and unreadable") {
        Tmp t("lookup");
        auto good = t.path / "good.json";
        write(good, replica_doc("probe--1.0--default", kSha));
        auto bad = t.path / "bad.json";
        write(bad, "{oops");

        ReplicaSource file_good{ReplicaSource::Kind::File, good};
        auto found = lookup_replica(file_good, "", "probe", "1.0", "default");
        CHECK(found.status == ReplicaLookupStatus::Found);
        CHECK(found.hash == MaybeHash(kSha));
        CHECK(found.entries == 1);

        auto missing = lookup_replica(file_good, "", "other", "1.0", "default");
        CHECK(missing.status == ReplicaLookupStatus::NoEntry);
        CHECK_FALSE(missing.hash.has_value());
        CHECK(missing.entries == 1);

        auto none = lookup_replica(ReplicaSource{}, "", "probe", "1.0", "default");
        CHECK(none.status == ReplicaLookupStatus::Inactive);

        ReplicaSource file_bad{ReplicaSource::Kind::File, bad};
        auto unreadable = lookup_replica(file_bad, "", "probe", "1.0", "default");
        CHECK(unreadable.status == ReplicaLookupStatus::Unreadable);
        CHECK_FALSE(unreadable.error.empty());
        CHECK(unreadable.source.find("bad.json") != std::string::npos);
    }
}

TEST_SUITE("trust_honesty::install") {

    TEST_CASE("a corrupt replica snapshot refuses the install before anything is poured") {
        Fixture f("corrupt");
        write(f.override_replica(), "{oops");

        auto msg = f.install_error();
        REQUIRE_FALSE(msg.empty());
        CHECK(msg.find("refusing to install 'probe 1.0'") != std::string::npos);
        CHECK(msg.find("unreadable") != std::string::npos);
        CHECK(msg.find(f.override_replica().string()) != std::string::npos);
        CHECK_FALSE(fs::exists(f.keg()));
        CHECK(summarize_degraded_installs(f.cfg).total == 0);
    }

    TEST_CASE("an install with no replica entry proceeds, is recorded and is counted") {
        Fixture f("noentry");
        write(f.override_replica(), replica_doc("unrelated--9.9--default", kOtherSha));

        CHECK(f.install_error().empty());
        REQUIRE(fs::exists(f.keg() / "bin" / "probe"));

        auto s = summarize_degraded_installs(f.cfg);
        CHECK(s.total == 1);
        CHECK(s.by_reason["no-replica-entry"] == 1);
        REQUIRE(s.installs.size() == 1);
        CHECK(s.installs[0].name == "probe");
        CHECK(s.installs[0].version == "1.0");
        CHECK(s.installs[0].bottle_tag == "default");
        CHECK(format_degraded_counts(s) == "1 (no-replica-entry: 1)");

        // doctor surfaces it as a finding and in the [trust] block.
        auto report = build_trust_report(f.cfg);
        CHECK(report.degraded_installs.total == 1);
        bool found = false;
        for (const auto& finding : findings_from_trust_report(report)) {
            if (finding.message.find("1 installed keg(s) verified against one hash source") !=
                    std::string::npos &&
                finding.message.find("probe 1.0") != std::string::npos) {
                found = true;
            }
        }
        CHECK(found);

        // daemon status prints the count and the keg.
        std::ostringstream out;
        format_degraded_trust(out, s);
        CHECK(out.str().find("Degraded-trust installs: 1 (no-replica-entry: 1)") !=
              std::string::npos);
        CHECK(out.str().find("probe 1.0  [no-replica-entry]") != std::string::npos);

        // Once the keg is gone (den cleanup), it no longer counts.
        fs::remove_all(f.keg());
        CHECK(summarize_degraded_installs(f.cfg).total == 0);
    }

    TEST_CASE("re-pouring with both sources agreeing clears the degraded record") {
        Fixture f("cleared");
        write(f.override_replica(), replica_doc("unrelated--9.9--default", kOtherSha));
        REQUIRE(f.install_error().empty());
        REQUIRE(summarize_degraded_installs(f.cfg).total == 1);

        fs::remove_all(f.keg());
        write(f.override_replica(), replica_doc("probe--1.0--default", f.sha));
        REQUIRE(f.install_error().empty());
        REQUIRE(fs::exists(f.keg()));
        CHECK(summarize_degraded_installs(f.cfg).total == 0);
    }

    TEST_CASE("a replica that disagrees still refuses") {
        Fixture f("mismatch");
        write(f.override_replica(), replica_doc("probe--1.0--default", kOtherSha));
        auto msg = f.install_error();
        CHECK(msg.find("hash mismatch") != std::string::npos);
        CHECK_FALSE(fs::exists(f.keg()));
    }
}

TEST_SUITE("trust_honesty::ledger") {

    TEST_CASE("empty when there is no ledger") {
        Tmp t("ledger-empty");
        auto cfg = tmp_config(t.path);
        auto s = summarize_degraded_installs(cfg);
        CHECK(s.total == 0);
        CHECK_FALSE(s.ledger_error.has_value());
        CHECK(format_degraded_counts(s) == "0");
    }

    TEST_CASE("an unreadable ledger is reported, then moved aside on the next write") {
        Tmp t("ledger-corrupt");
        auto cfg = tmp_config(t.path);
        write(degraded_installs_path(cfg), "not json");
        auto s = summarize_degraded_installs(cfg);
        REQUIRE(s.ledger_error.has_value());
        CHECK(format_degraded_counts(s).find("unknown") == 0);

        bool warned = false;
        TrustReport report;
        report.replica_source_active = true;
        report.replica_status = ReplicaStatus::Active;
        report.active_advanced_layers = {"cross-source-hash-agreement"};
        report.degraded_installs = s;
        for (const auto& finding : findings_from_trust_report(report)) {
            warned |= finding.message.find("ledger is unreadable") != std::string::npos;
        }
        CHECK(warned);

        auto keg = cfg.store / "x" / "1";
        fs::create_directories(keg);
        record_degraded_install(cfg, {"x", "1", "replica-inactive", "default", keg});
        auto aside = degraded_installs_path(cfg);
        aside += ".corrupt";
        CHECK(fs::exists(aside));
        auto s2 = summarize_degraded_installs(cfg);
        CHECK(s2.total == 1);
        CHECK(s2.by_reason["replica-inactive"] == 1);
    }
}

TEST_SUITE("trust_honesty::doctor") {

    TEST_CASE("an unreadable snapshot is an error that says installs are refused") {
        TrustReport report;
        report.replica_source_active = false;
        report.replica_status = ReplicaStatus::Unreadable;
        report.replica_source = "/x/known_hashes.json";
        report.replica_error = "bad digest";
        bool found = false;
        for (const auto& finding : findings_from_trust_report(report)) {
            if (finding.severity == Severity::Error &&
                finding.message.find("/x/known_hashes.json is unreadable (bad digest)") !=
                    std::string::npos &&
                finding.message.find("refused") != std::string::npos) {
                found = true;
            }
        }
        CHECK(found);
    }

    TEST_CASE("check_trust_model on a corrupt override reports it, not 'CDN unreachable'") {
        Tmp t("doctor-corrupt");
        auto cfg = tmp_config(t.path);
        write(cfg.den_home / "trust" / "known_hashes.json", "{oops");
        auto report = build_trust_report(cfg);
        CHECK(report.replica_status == ReplicaStatus::Unreadable);
        REQUIRE(report.replica_error.has_value());
        for (const auto& finding : findings_from_trust_report(report)) {
            CHECK(finding.message.find("CDN") == std::string::npos);
        }
    }
}
