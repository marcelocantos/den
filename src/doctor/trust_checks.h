// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// 🎯T64 / 🎯T42 / 🎯T44: `den doctor` trust-model reporting.
//
// `den doctor` surfaces the state of the independent-verification trust model
// so users can reason about their security posture:
//
//   • Which hash sources are active (Homebrew API, den replica, both).
//   • The timestamp of the last successful replica sync.
//   • Whether the model is at full strength (both sources) or degraded.
//   • Which advanced trust layers (🎯T44) are wired in.

#include "../core/config.h"
#include "doctor.h"

#include <optional>
#include <string>
#include <vector>

namespace den {

/// State of the den replica hash source.
enum class ReplicaStatus {
    Active,   // replica reachable / snapshot present and in sync
    Degraded, // replica unreachable; falling back to Homebrew-only
    Unknown,  // never synced
};

/// A snapshot of the trust model's current configuration and health.
struct TrustReport {
    bool homebrew_source_active = true;
    bool replica_source_active = false;
    ReplicaStatus replica_status = ReplicaStatus::Unknown;
    std::optional<std::string> last_replica_sync_iso8601; // nullopt = never synced
    std::vector<std::string> active_advanced_layers;      // 🎯T44 layers
    size_t replica_entry_count = 0;                       // entries in the snapshot
};

/// Inspect the live configuration and build a trust report.
TrustReport build_trust_report(const Config& config);

/// Summarise a trust report into doctor-style Finding messages.
std::vector<Finding> findings_from_trust_report(const TrustReport& report);

/// doctor() check entry point: build the report, render its status block to
/// stdout, and append any findings.
void check_trust_model(const Config& config, std::vector<Finding>& findings);

} // namespace den
