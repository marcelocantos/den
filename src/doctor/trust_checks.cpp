// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "trust_checks.h"

#include "../trust/trust_model.h"

#include <spdlog/spdlog.h>

#include <iostream>

namespace den {

namespace {

void warn(std::vector<Finding>& findings, std::string msg) {
    findings.push_back({Severity::Warning, std::move(msg)});
}
void error(std::vector<Finding>& findings, std::string msg) {
    findings.push_back({Severity::Error, std::move(msg)});
}

const char* replica_status_label(ReplicaStatus s) {
    switch (s) {
    case ReplicaStatus::Active:
        return "active";
    case ReplicaStatus::Unreadable:
        return "unreadable";
    case ReplicaStatus::Unknown:
        return "none";
    }
    return "unknown";
}

} // namespace

TrustReport build_trust_report(const Config& config) {
    TrustReport report;

    // Source A — Homebrew's API. den always derives bottle hashes from the
    // index it builds from formulae.brew.sh, so this source is considered
    // active whenever den is operating normally.
    report.homebrew_source_active = true;

    // Source B — the local replica snapshot shipped with den (or the
    // <den_home>/trust override). Nothing is fetched. A corrupt snapshot
    // throws from load_replica; report it (installs refuse in that state)
    // rather than crashing doctor.
    std::map<std::string, std::string> replica;
    bool replica_ok = true;
    try {
        auto source = locate_local_replica(config);
        report.replica_source = source.describe();
        replica = load_replica(source, embedded_replica_json());
    } catch (const std::exception& e) {
        replica_ok = false;
        report.replica_error = e.what();
        SPDLOG_DEBUG("trust replica snapshot unreadable: {}", e.what());
    }

    report.replica_entry_count = replica.size();
    report.replica_source_active = replica_ok && !replica.empty();

    if (!replica_ok) {
        report.replica_status = ReplicaStatus::Unreadable;
    } else if (replica.empty()) {
        report.replica_status = ReplicaStatus::Unknown;
    } else {
        report.replica_status = ReplicaStatus::Active;
        // The snapshot is bundled with the binary, so "last sync" is the build
        // time baked into DEN_VERSION's release rather than a runtime fetch.
        report.last_replica_sync_iso8601 = std::string("bundled-with-den-") + DEN_VERSION;
    }

    // 🎯T44 advanced trust layers. The cross-source hash agreement check wired
    // into install_one() is the foundational layer that is active today; richer
    // provenance layers (Sigstore / SLSA) are future work.
    if (report.replica_source_active) {
        report.active_advanced_layers.push_back("cross-source-hash-agreement");
    }

    report.degraded_installs = summarize_degraded_installs(config);

    return report;
}

std::vector<Finding> findings_from_trust_report(const TrustReport& report) {
    std::vector<Finding> findings;

    if (!report.homebrew_source_active) {
        error(findings, "trust: Homebrew hash source (formulae.brew.sh) is inactive");
    }

    if (report.replica_status == ReplicaStatus::Unreadable) {
        error(findings, "trust: replica snapshot " + report.replica_source + " is unreadable (" +
                            report.replica_error.value_or("unknown error") +
                            ") — bottle installs are refused until it is repaired or removed");
    } else if (!report.replica_source_active) {
        warn(findings, "trust: no replica snapshot — every install is verified against "
                       "formulae.brew.sh only (degraded trust); reinstall den or seed "
                       "<den_home>/trust/known_hashes.json");
    }

    const auto& degraded = report.degraded_installs;
    if (degraded.ledger_error) {
        warn(findings, "trust: degraded-install ledger is unreadable (" + *degraded.ledger_error +
                           "); single-source installs cannot be counted");
    } else if (degraded.total > 0) {
        std::string msg = "trust: " + std::to_string(degraded.total) +
                          " installed keg(s) verified against one hash source only " +
                          format_degraded_reasons(degraded) + ":";
        constexpr size_t kMaxListed = 10;
        for (size_t i = 0; i < degraded.installs.size() && i < kMaxListed; ++i) {
            const auto& d = degraded.installs[i];
            msg += (i ? ", " : " ") + d.name + " " + d.version;
        }
        if (degraded.installs.size() > kMaxListed) {
            msg += ", … (+" + std::to_string(degraded.installs.size() - kMaxListed) + " more)";
        }
        warn(findings, std::move(msg));
    }

    if (report.active_advanced_layers.empty() && report.replica_source_active) {
        warn(findings, "trust: no advanced trust layers active (🎯T44 not yet wired)");
    }

    return findings;
}

void check_trust_model(const Config& config, std::vector<Finding>& findings) {
    SPDLOG_DEBUG("check: trust model");

    TrustReport report = build_trust_report(config);

    // Render a human-readable trust status block (consumed by the CLI and the
    // doctor-CLI test). Keep it deterministic — no wall-clock timestamps.
    std::cout << "[trust] sources: formulae.brew.sh"
              << (report.replica_source_active ? " + den-replica-snapshot"
                  : report.replica_status == ReplicaStatus::Unreadable
                      ? " (replica unreadable — installs refused)"
                      : " (replica inactive)")
              << "\n";
    std::cout << "[trust] replica: " << replica_status_label(report.replica_status)
              << ", entries: " << report.replica_entry_count
              << ", last sync: " << report.last_replica_sync_iso8601.value_or("never") << "\n";
    std::cout << "[trust] replica source: " << report.replica_source << "\n";
    std::cout << "[trust] degraded installs: " << format_degraded_counts(report.degraded_installs)
              << "\n";
    std::cout << "[trust] layers: ";
    if (report.active_advanced_layers.empty()) {
        std::cout << "none";
    } else {
        for (size_t i = 0; i < report.active_advanced_layers.size(); ++i) {
            std::cout << (i ? ", " : "") << report.active_advanced_layers[i];
        }
    }
    std::cout << "\n";

    auto trust_findings = findings_from_trust_report(report);
    findings.insert(findings.end(), trust_findings.begin(), trust_findings.end());
}

} // namespace den
