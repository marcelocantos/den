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
    case ReplicaStatus::Degraded:
        return "degraded";
    case ReplicaStatus::Unknown:
        return "unknown";
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

    // Source B — the bundled replica snapshot (raw.githubusercontent.com).
    // Its presence and content determine the replica source's health.  A
    // corrupt snapshot throws from load_local_replica; treat that as a
    // degraded/inactive replica rather than crashing doctor.
    std::map<std::string, std::string> replica;
    bool replica_ok = true;
    try {
        replica = load_local_replica(config);
    } catch (const std::exception& e) {
        replica_ok = false;
        SPDLOG_WARN("trust replica snapshot unreadable: {}", e.what());
    }

    report.replica_entry_count = replica.size();
    report.replica_source_active = replica_ok && !replica.empty();

    if (!replica_ok) {
        report.replica_status = ReplicaStatus::Degraded;
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

    return report;
}

std::vector<Finding> findings_from_trust_report(const TrustReport& report) {
    std::vector<Finding> findings;

    if (!report.homebrew_source_active) {
        error(findings, "trust: Homebrew hash source (formulae.brew.sh) is inactive");
    }

    if (!report.replica_source_active) {
        warn(findings, "trust: den replica hash source is inactive — "
                       "operating in degraded trust mode (single source only); "
                       "seed data/known_hashes.json to enable full independent verification");
    }

    if (report.replica_source_active) {
        switch (report.replica_status) {
        case ReplicaStatus::Active:
            // No finding — healthy state.
            break;
        case ReplicaStatus::Degraded:
            warn(findings, "trust: replica CDN unreachable — last sync: " +
                               report.last_replica_sync_iso8601.value_or("never"));
            break;
        case ReplicaStatus::Unknown:
            warn(findings, "trust: replica has never synced");
            break;
        }
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
              << (report.replica_source_active ? " + den-replica-cdn" : " (replica inactive)")
              << "\n";
    std::cout << "[trust] replica: " << replica_status_label(report.replica_status)
              << ", entries: " << report.replica_entry_count
              << ", last sync: " << report.last_replica_sync_iso8601.value_or("never") << "\n";
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
