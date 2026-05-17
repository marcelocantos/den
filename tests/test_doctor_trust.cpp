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

#include "doctor/doctor.h"
#include "core/config.h"

#include <filesystem>
#include <fstream>
#include <optional>
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
        if (!result) throw std::runtime_error("mkdtemp failed");
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
    cfg.store    = home / "Cellar";
    cfg.cache    = home / "cache";
    cfg.homebrew_prefix = home / "brew";
    cfg.homebrew_cellar = home / "brew" / "Cellar";
    cfg.arch = Arch::Arm64;
    cfg.macos_version = std::nullopt;
    return cfg;
}

// ---------------------------------------------------------------------------
// Mock trust-status report (mirrors the future interface from 🎯T42 / T44).
// Replace with real includes once the implementation lands.
// ---------------------------------------------------------------------------

enum class ReplicaStatus {
    Active,     // replica CDN reachable and in sync
    Degraded,   // replica unreachable; falling back to Homebrew-only
    Unknown,    // never synced
};

struct TrustReport {
    bool homebrew_source_active = true;
    bool replica_source_active  = false; // becomes true once 🎯T42 ships
    ReplicaStatus replica_status = ReplicaStatus::Unknown;
    std::optional<std::string> last_replica_sync_iso8601; // nullopt = never synced
    std::vector<std::string> active_advanced_layers;      // empty until 🎯T44
};

/// Summarise the trust report into doctor-style Finding messages.
static std::vector<Finding> findings_from_trust_report(const TrustReport& report) {
    std::vector<Finding> findings;

    if (!report.homebrew_source_active) {
        findings.push_back({Severity::Error,
                            "trust: Homebrew hash source (formulae.brew.sh) is inactive"});
    }

    if (!report.replica_source_active) {
        findings.push_back({Severity::Warning,
                            "trust: den replica hash source is inactive — "
                            "operating in degraded trust mode (single source only); "
                            "await 🎯T42 to enable full independent verification"});
    }

    if (report.replica_source_active) {
        switch (report.replica_status) {
        case ReplicaStatus::Active:
            // No finding — healthy state.
            break;
        case ReplicaStatus::Degraded:
            findings.push_back({Severity::Warning,
                                "trust: replica CDN unreachable — last sync: " +
                                    report.last_replica_sync_iso8601.value_or("never")});
            break;
        case ReplicaStatus::Unknown:
            findings.push_back({Severity::Warning,
                                "trust: replica has never synced"});
            break;
        }
    }

    if (report.active_advanced_layers.empty() && report.replica_source_active) {
        findings.push_back({Severity::Warning,
                            "trust: no advanced trust layers active (🎯T44 not yet wired)"});
    }

    return findings;
}

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
        report.replica_source_active  = false;
        report.replica_status         = ReplicaStatus::Unknown;

        auto findings = findings_from_trust_report(report);
        // Must emit at least one warning about the inactive replica.
        bool found_replica_warning = false;
        for (const auto& f : findings) {
            if (f.severity == Severity::Warning &&
                f.message.find("replica") != std::string::npos) {
                found_replica_warning = true;
            }
        }
        CHECK(found_replica_warning);
    }

    TEST_CASE("full-strength: both sources active and replica healthy → no trust findings") {
        TrustReport report;
        report.homebrew_source_active = true;
        report.replica_source_active  = true;
        report.replica_status         = ReplicaStatus::Active;
        report.last_replica_sync_iso8601 = "2026-05-17T00:00:00Z";
        report.active_advanced_layers    = {"sigstore"};

        auto findings = findings_from_trust_report(report);
        for (const auto& f : findings) {
            // Any finding for trust would indicate a configuration problem.
            INFO("unexpected trust finding: " << f.message);
            CHECK(f.message.find("trust:") == std::string::npos);
        }
    }

    TEST_CASE("degraded replica: active but CDN unreachable → warning with last-sync timestamp") {
        TrustReport report;
        report.homebrew_source_active    = true;
        report.replica_source_active     = true;
        report.replica_status            = ReplicaStatus::Degraded;
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
        report.replica_source_active  = true;
        report.replica_status         = ReplicaStatus::Active;
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
        report.homebrew_source_active    = true;
        report.replica_source_active     = true;
        report.replica_status            = ReplicaStatus::Active;
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
    // Upstream gap guards (skipped via doctest::skip() decorator).
    // Remove the * doctest::skip() once the upstream target is implemented.
    // -----------------------------------------------------------------------

    // 🎯T42/T44: doctor() does not yet call check_trust_model().
    TEST_CASE("doctor() emits trust section in output (🎯T42 + T44 gap)" *
              doctest::skip(true)) {
        // When T42+T44 are implemented, doctor() must:
        //   - Call a check_trust_model(config, findings) helper.
        //   - Report which hash sources are active.
        //   - Report the last replica sync timestamp.
        //   - Report which advanced trust layers are active.
        CHECK(false); // placeholder — must be replaced with real assertions
    }

    // 🎯T42: CLI integration test requires real replica state.
    TEST_CASE("den doctor CLI output contains trust section (🎯T42 gap)" *
              doctest::skip(true)) {
        // When T42 is implemented, running `den doctor` should print a block like:
        //   [trust] replica: active, last sync: 2026-05-17T00:00:00Z
        //   [trust] sources: formulae.brew.sh + den-replica-cdn
        CHECK(false); // placeholder — must be replaced with real assertions
    }

} // TEST_SUITE

} // namespace doctor_trust_test
} // namespace den
