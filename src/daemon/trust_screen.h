// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Screening the daemon's pending upgrades against the unattended trust
// policy before the upgrade pass runs, so a held-back formula also holds back
// every pending upgrade that would have to install it.

#include "../core/config.h"
#include "../index/index.h"
#include "../trust/unattended.h"
#include "daemon.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace den {

/// `to_install(name)` lists what upgrading `name` would install: `name`
/// itself plus each dependency not already installed at its index version.
/// `refusal(pkg)` says why `pkg` must not be installed unattended, or nullopt.
/// Returns pending name → reason for each upgrade to hold back; a dependent
/// gets "depends on <pkg>: <reason>". `refusal` is called once per package.
std::map<std::string, std::string> screen_pending_upgrades(
    const std::vector<PendingUpgrade>& pending,
    const std::function<std::vector<std::string>(const std::string&)>& to_install,
    const std::function<std::optional<std::string>(const std::string&)>& refusal);

/// What upgrading `name` would install: `name` plus each dependency
/// (transitively) not already installed at its index version.
std::vector<std::string> packages_to_install(const Config& config, const PackageIndex& idx,
                                             const std::string& name);

/// The unattended trust screen shared by the daemon's maintenance cycle and
/// `den --unattended upgrade`: screen_pending_upgrades() with the live bottle
/// cross-check under `policy`. A formula is held back when bottle_action()
/// says HoldBack (single source under trust.unattended=refuse) or Refuse
/// (hash mismatch, unreadable snapshot, no hash at all); every pending
/// upgrade that would install it is held back with it. Every hold goes into
/// the trust ledger with its reason (hold_reason(), or "depends on <pkg>"
/// for a dependent), so `den trust review` lists all of them, unless
/// `record_holds` is false (`den --unattended upgrade --dry-run`, which
/// changes nothing). Returns pending name → reason, as
/// screen_pending_upgrades().
std::map<std::string, std::string>
screen_unattended_upgrades(const Config& config, const PackageIndex& idx,
                           const std::vector<PendingUpgrade>& pending, const TrustPolicy& policy,
                           bool record_holds = true);

/// MaintenanceHooks::trust_screen for the live daemon: screen_unattended_upgrades()
/// under current_trust_policy(config). `config` and `idx` must outlive it.
std::function<std::map<std::string, std::string>(const std::vector<PendingUpgrade>&)>
live_trust_screen(const Config& config, const PackageIndex& idx);

/// One desktop-notification line for upgrades newly held back this pass:
/// "den held back 2 upgrades (unattended trust policy): a, b. See `den doctor`."
std::string held_back_notification(const std::vector<std::string>& names);

} // namespace den
