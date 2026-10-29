// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Trust policy for installs nobody is watching.
//
// An interactive install that only one hash source vouches for pours with a
// warning: the person who asked for it sees the warning. The daemon's upgrade
// pass (and `den --unattended`) has nobody to warn, so trust.unattended
// decides what happens instead:
//
//                       trust.unattended=warn        trust.unattended=refuse
//   hashes disagree     refuse, ledger (always)      refuse, ledger (always)
//   no replica entry    pour, ledger, notify         hold back, ledger, notify
//   replica inactive    pour, ledger                 hold back, ledger
//   replica unreadable  refuse, ledger (fail closed) refuse, ledger (fail closed)
//
// An unknown trust.unattended value counts as refuse; `den doctor` warns.
//
// Unattended source builds are off unless trust.unattended_source_builds is
// "allowlist" and the formula is in trust.source_build_allowlist; even then a
// build whose source hash only one source vouches for is deferred and
// reported rather than built. A version accepted with `den trust accept` is
// let through once.

#include "../core/config.h"
#include "../core/error.h"
#include "trust_model.h"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace den {

struct TrustSettings; // settings/settings.h

/// Launchd label of den's daemon; XPC_SERVICE_NAME carries it in the job.
inline constexpr const char* kDaemonLaunchdLabel = "dev.den.daemon";

enum class UnattendedMode { Warn, Refuse };

/// "warn" → Warn, "refuse" → Refuse; anything else fails closed to Refuse.
UnattendedMode parse_unattended_mode(const std::string& value);

struct TrustPolicy {
    /// Why this run is unattended ("daemon", "--unattended", "launchd job
    /// dev.den.daemon"), or nullopt for an interactive run.
    std::optional<std::string> unattended;
    UnattendedMode mode = UnattendedMode::Warn;
    bool notify = true;
    bool source_builds = false; // trust.unattended_source_builds == "allowlist"
    std::set<std::string> source_build_allowlist;
};

/// What is wrong with the trust settings, one line each: a trust.unattended
/// that is not "warn" or "refuse" (treated as refuse), a
/// trust.unattended_source_builds that is not "off" or "allowlist" (treated as
/// off). Empty when they are fine. `den doctor` warns with these.
std::vector<std::string> trust_settings_problems(const TrustSettings& settings);

/// Build the policy from settings plus the unattended reason.
TrustPolicy make_trust_policy(const TrustSettings& settings, std::optional<std::string> unattended);

/// Pure unattended detection: an explicit override (the daemon, the
/// `--unattended` flag) wins; otherwise a process launched as den's launchd
/// job (XPC_SERVICE_NAME == dev.den.daemon) is unattended. A missing TTY on
/// its own is not: CI and scripted `den install` runs keep their interactive
/// behaviour, and `--yes` never counts.
std::optional<std::string> detect_unattended(const std::optional<std::string>& override_reason,
                                             const char* xpc_service_name);

/// Mark this process unattended (or clear it with nullopt). The daemon calls
/// this at start-up; the CLI calls it for `--unattended`.
void set_unattended(std::optional<std::string> reason);

/// detect_unattended() for this process.
std::optional<std::string> unattended_reason();

/// The policy for this process: settings from config.den_home plus
/// unattended_reason().
TrustPolicy current_trust_policy(const Config& config);

enum class BottleAction {
    Install,         // cross-checked: pour
    InstallDegraded, // single source: pour, record in the ledger
    HoldBack,        // single source, unattended + refuse: skip, record, keep pending
    Refuse,          // hashes disagree or the replica is unreadable: hard error
};

/// What to do with a bottle given its cross-check decision. `accepted` is
/// true when the exact version was accepted with `den trust accept`.
BottleAction bottle_action(const TrustPolicy& policy, TrustDecision decision, bool accepted);

/// The ledger reason for a bottle an unattended run holds back or refuses,
/// from its cross-check decision and status tag (BottleTrust::reason_tag):
///   "single-source"      one hash source only (WarnProceed under refuse)
///   "hash-mismatch"      Homebrew and the replica disagree
///   "replica-unreadable" the replica snapshot exists but cannot be read
///   "unverifiable"       neither source has a hash
/// Only a "single-source" hold can be let through with `den trust accept`.
std::string hold_reason(TrustDecision decision, const std::string& status_tag);

/// True for ledger reasons `den trust accept` may let through: a
/// single-source bottle or a deferred source build.
bool hold_is_acceptable(const std::string& reason);

enum class SourceBuildAction { Build, Defer, Refuse };

/// What to do with a source build of `name` given the cross-check of its
/// source hash. Interactive runs always build (the warning is shown, as
/// before) unless the hashes disagree.
SourceBuildAction source_build_action(const TrustPolicy& policy, const std::string& name,
                                      TrustDecision source_decision, bool accepted);

/// Thrown when an unattended install holds a package back. The package stays
/// pending; the reason is already in the ledger.
class TrustHeldBack : public UserError {
  public:
    TrustHeldBack(std::string name, std::string version, const std::string& message)
        : UserError(message), name_(std::move(name)), version_(std::move(version)) {}
    const std::string& name() const { return name_; }
    const std::string& version() const { return version_; }

  private:
    std::string name_;
    std::string version_;
};

} // namespace den
