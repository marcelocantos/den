// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// A ledger of kegs den poured on a single hash source (the trust cross-check
// returned WarnProceed). `den doctor` and `den daemon status` read it so a
// degraded install stays visible after its one-off warning has scrolled away.
//
// Stored at <den_home>/trust/degraded_installs.json:
//   { "schema_version": 1,
//     "installs": { "<name>@<version>": { "reason": "no-replica-entry",
//                                         "keg": "/path/to/keg",
//                                         "bottle_tag": "arm64_sequoia",
//                                         "recorded_at": 1730000000 } } }
//
// Only entries whose keg still exists are counted, so `den cleanup` removing a
// keg drops it from the counts without having to edit the ledger.

#include "../core/config.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace den {

struct DegradedInstall {
    std::string name;
    std::string version;
    std::string reason; // replica_status_tag(): "no-replica-entry", "replica-inactive", ...
    std::string bottle_tag;
    std::filesystem::path keg;
};

struct DegradedInstallSummary {
    size_t total = 0;                        // degraded kegs still on disk
    std::map<std::string, size_t> by_reason; // reason tag → count
    std::vector<DegradedInstall> installs;   // the kegs counted, sorted by name
    std::optional<std::string> ledger_error; // set when the ledger is unreadable
};

std::filesystem::path degraded_installs_path(const Config& config);

/// Record that `install` was poured on a single hash source. Best effort: a
/// failure to write is logged, never fatal (the install already happened).
void record_degraded_install(const Config& config, const DegradedInstall& install);

/// Forget name@version (it was re-poured with both sources agreeing).
void clear_degraded_install(const Config& config, const std::string& name,
                            const std::string& version);

/// Count the ledger's entries whose keg still exists.
DegradedInstallSummary summarize_degraded_installs(const Config& config);

/// "(no-replica-entry: a, replica-inactive: b)", or "" when there are none.
std::string format_degraded_reasons(const DegradedInstallSummary& summary);

/// "N (no-replica-entry: a, replica-inactive: b)" or "0".
std::string format_degraded_counts(const DegradedInstallSummary& summary);

} // namespace den
