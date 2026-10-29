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
//
// Unattended installs (the daemon, `den --unattended`) also record what they
// held back under trust.unattended=refuse or deferred as a source build:
//   "<name>@<version>": { "state": "refused", "reason": "unattended-refuse",
//                         "status": "no-replica-entry", "bottle_sha": "…",
//                         "bottle_tag": "…", "recorded_at": … }
// `den trust accept <name>@<version>` turns a refused entry into
// "state": "accepted", which lets that exact version through unattended once;
// the install that follows overwrites the entry with a degraded one. Entries
// without a "state" are degraded installs (the original schema).

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
    std::string state = "degraded"; // "degraded", "refused" or "accepted"
    std::string status;             // refused: replica_status_tag() of the lookup
    std::string bottle_sha;         // refused: the hash Homebrew's API gave
    long long recorded_at = 0;      // seconds since the epoch (set when read)
};

struct DegradedInstallSummary {
    size_t total = 0;                              // degraded kegs still on disk
    std::map<std::string, size_t> by_reason;       // reason tag → count
    std::vector<DegradedInstall> installs;         // the kegs counted, sorted by name
    std::optional<std::string> ledger_error;       // set when the ledger is unreadable
    size_t refused = 0;                            // held back or deferred, not installed
    std::vector<DegradedInstall> refused_installs; // sorted by name
    std::vector<DegradedInstall> accepted;         // accepted, not yet installed
    std::optional<long long> since;                // earliest recorded_at among counted entries
};

std::filesystem::path degraded_installs_path(const Config& config);

/// Record that `install` was poured on a single hash source. Best effort: a
/// failure to write is logged, never fatal (the install already happened).
void record_degraded_install(const Config& config, const DegradedInstall& install);

/// Forget name@version (it was re-poured with both sources agreeing).
void clear_degraded_install(const Config& config, const std::string& name,
                            const std::string& version);

/// Record that an unattended install held name@version back (or deferred its
/// source build). `install.reason` says why ("unattended-refuse",
/// "unattended-source-build", "depends on …"). Best effort, like
/// record_degraded_install. An "accepted" entry is left alone.
void record_refused_install(const Config& config, const DegradedInstall& install);

/// Mark a refused name@version accepted. Returns false (changing nothing)
/// when the ledger holds no refused entry for it.
bool accept_refused_install(const Config& config, const std::string& name,
                            const std::string& version);

/// Forget a refused or accepted name@version once it has been installed by
/// a path that does not record a degraded entry (a source build). Degraded
/// entries are left alone.
void clear_refused_install(const Config& config, const std::string& name,
                           const std::string& version);
/// True when name@version has been accepted with `den trust accept`.
bool is_install_accepted(const Config& config, const std::string& name, const std::string& version);

/// Count the ledger's entries whose keg still exists.
DegradedInstallSummary summarize_degraded_installs(const Config& config);

/// "(no-replica-entry: a, replica-inactive: b)", or "" when there are none.
std::string format_degraded_reasons(const DegradedInstallSummary& summary);

/// "N (no-replica-entry: a, replica-inactive: b)" or "0".
std::string format_degraded_counts(const DegradedInstallSummary& summary);

/// "N refused, M degraded since 2026-10-29 14:05" (local time), or
/// "0 refused, 0 degraded" when the ledger is empty.
std::string format_trust_ledger(const DegradedInstallSummary& summary);

} // namespace den
