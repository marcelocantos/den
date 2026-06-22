// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// 🎯T64 verification harness: `den doctor` trust model reporting.
//
// `den doctor` must report the state of the trust model so users can
// reason about their security posture:
//
//   • Which hash sources are active (Homebrew API, den replica, both).
//   • The timestamp of the last successful replica sync.
//   • Whether the trust model is operating at full strength (both sources
//     available) or in degraded mode (only one source).
//
// Until 🎯T42 ships the replica and 🎯T44 wires advanced trust layers,
// these tests verify:
//
//   1. The existing `doctor()` function is callable and returns a
//      vector<Finding> — establishing the interface is stable.
//   2. A thin trust-status reporting layer (inline mock matching the future
//      interface) behaves correctly given different replica states.
//   3. Skipped sentinels record what must be true once 🎯T42 / 🎯T44 land.

#include <doctest.h>

#include "core/config.h"
#include "doctor/doctor.h"
#include "doctor/trust_checks.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace den {
namespace doctor_trust_test {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// RAII temp directory (mirrors pattern used in other test files).
// ---------------------------------------------------------------------------
struct TmpDir {
    fs::path path;

    TmpDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_dt_XXXXXX").string();
        char* result = ::mkdtemp(tmpl.data());
        if (!result)
            throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~TmpDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TmpDir(const TmpDir&) = delete;
    TmpDir& operator=(const TmpDir&) = delete;
};

/// Build a minimal Config pointing at an isolated temp directory.
static Config make_isolated_config(const fs::path& home) {
    Config cfg;
    cfg.den_home = home;
    cfg.store = home / "Cellar";
    cfg.cache = home / "cache";
    cfg.homebrew_prefix = home / "brew";
    cfg.homebrew_cellar = home / "brew" / "Cellar";
    cfg.arch = Arch::Arm64;
    cfg.macos_version = std::nullopt;
    return cfg;
}

// ReplicaStatus / TrustReport / findings_from_trust_report now come from the
// real implementation in src/doctor/trust_checks.* (included above).

// ---------------------------------------------------------------------------
// Tests — existing doctor() interface
// ---------------------------------------------------------------------------

TEST_SUITE("doctor_trust::existing_doctor_interface") {

    TEST_CASE("doctor() is callable and returns a vector<Finding>") {
        TmpDir tmp;
        auto cfg = make_isolated_config(tmp.path);
        auto findings = doctor(cfg);
        // The return type is correct; any number of findings is acceptable for
        // a fresh isolated home.  We just assert the call doesn't crash.
        CHECK(true); // reached without exception
    }

    TEST_CASE("doctor() Finding struct has severity and message") {
        TmpDir tmp;
        auto cfg = make_isolated_config(tmp.path);
        // Create den_home so at least the structure check fires.
        fs::create_directories(tmp.path / "envs");
        auto findings = doctor(cfg);
        for (const auto& f : findings) {
            CHECK(!f.message.empty());
            CHECK((f.severity == Severity::Warning || f.severity == Severity::Error));
        }
    }

} // TEST_SUITE

// ---------------------------------------------------------------------------
// Tests — trust report → Finding translation
// ---------------------------------------------------------------------------

TEST_SUITE("doctor_trust::trust_report_to_findings") {

    TEST_CASE("pre-T42 baseline: replica inactive → warning finding emitted") {
        TrustReport report;
        report.homebrew_source_active = true;
        report.replica_source_active = false;
        report.replica_status = ReplicaStatus::Unknown;

        auto findings = findings_from_trust_report(report);
        // Must emit at least one warning about the inactive replica.
        bool found_replica_warning = false;
        for (const auto& f : findings) {
            if (f.severity == Severity::Warning && f.message.find("replica") != std::string::npos) {
                found_replica_warning = true;
            }
        }
        CHECK(found_replica_warning);
    }

    TEST_CASE("full-strength: both sources active and replica healthy → no trust findings") {
        TrustReport report;
        report.homebrew_source_active = true;
        report.replica_source_active = true;
        report.replica_status = ReplicaStatus::Active;
        report.last_replica_sync_iso8601 = "2026-05-17T00:00:00Z";
        report.active_advanced_layers = {"sigstore"};

        auto findings = findings_from_trust_report(report);
        for (const auto& f : findings) {
            // Any finding for trust would indicate a configuration problem.
            INFO("unexpected trust finding: " << f.message);
            CHECK(f.message.find("trust:") == std::string::npos);
        }
    }

    TEST_CASE("degraded replica: active but CDN unreachable → warning with last-sync timestamp") {
        TrustReport report;
        report.homebrew_source_active = true;
        report.replica_source_active = true;
        report.replica_status = ReplicaStatus::Degraded;
        report.last_replica_sync_iso8601 = "2026-05-10T12:00:00Z";

        auto findings = findings_from_trust_report(report);
        bool found = false;
        for (const auto& f : findings) {
            if (f.severity == Severity::Warning &&
                f.message.find("2026-05-10") != std::string::npos) {
                found = true;
            }
        }
        CHECK(found);
    }

    TEST_CASE("homebrew source inactive → error finding") {
        TrustReport report;
        report.homebrew_source_active = false;
        report.replica_source_active = true;
        report.replica_status = ReplicaStatus::Active;
        report.last_replica_sync_iso8601 = "2026-05-17T00:00:00Z";

        auto findings = findings_from_trust_report(report);
        bool found_error = false;
        for (const auto& f : findings) {
            if (f.severity == Severity::Error &&
                f.message.find("formulae.brew.sh") != std::string::npos) {
                found_error = true;
            }
        }
        CHECK(found_error);
    }

    TEST_CASE("replica active but no advanced layers → T44 gap warning") {
        TrustReport report;
        report.homebrew_source_active = true;
        report.replica_source_active = true;
        report.replica_status = ReplicaStatus::Active;
        report.last_replica_sync_iso8601 = "2026-05-17T00:00:00Z";
        // active_advanced_layers intentionally left empty

        auto findings = findings_from_trust_report(report);
        bool found_t44_gap = false;
        for (const auto& f : findings) {
            if (f.message.find("T44") != std::string::npos) {
                found_t44_gap = true;
            }
        }
        CHECK(found_t44_gap);
    }

    // -----------------------------------------------------------------------
    // 🎯T42/T44: doctor() calls check_trust_model() and prints a trust block.
    // -----------------------------------------------------------------------

    /// Capture everything written to std::cout while `fn` runs.
    static std::string capture_stdout(const std::function<void()>& fn) {
        std::ostringstream buf;
        std::streambuf* old = std::cout.rdbuf(buf.rdbuf());
        try {
            fn();
        } catch (...) {
            std::cout.rdbuf(old);
            throw;
        }
        std::cout.rdbuf(old);
        return buf.str();
    }

    // doctor() must emit a [trust] status block describing the active sources.
    TEST_CASE("doctor() emits trust section in output (🎯T42 + T44)") {
        TmpDir tmp;
        auto cfg = make_isolated_config(tmp.path);
        std::string out = capture_stdout([&] { (void)doctor(cfg); });
        CHECK(out.find("[trust]") != std::string::npos);
        CHECK(out.find("formulae.brew.sh") != std::string::npos);
        // Sources line names both the Homebrew API and the den replica posture.
        CHECK(out.find("replica") != std::string::npos);
    }

    // check_trust_model() drives the CLI-visible block. With a real replica
    // snapshot present it must report active sources and a sync line.
    TEST_CASE("trust block reports replica state (🎯T42)") {
        TmpDir tmp;
        auto cfg = make_isolated_config(tmp.path);

        // Seed an isolated replica snapshot so the block reports it as active.
        fs::create_directories(tmp.path / "trust");
        std::ofstream f(tmp.path / "trust" / "known_hashes.json");
        f << R"({"schema_version":1,"hashes":{)"
          << R"("tree--2.3.2--arm64_sequoia":")"
          << "ef367d0a5e74970e2f5042479fe4000a8b324ac075520c66f8457f1cb06ca668"
          << R"("}})";
        f.close();

        std::vector<Finding> findings;
        std::string out = capture_stdout([&] { check_trust_model(cfg, findings); });
        CHECK(out.find("[trust] sources:") != std::string::npos);
        CHECK(out.find("den-replica-cdn") != std::string::npos);
        CHECK(out.find("[trust] replica: active") != std::string::npos);
        CHECK(out.find("entries: 1") != std::string::npos);
    }

} // TEST_SUITE

} // namespace doctor_trust_test
} // namespace den
