// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Screening the daemon's pending upgrades against the unattended trust
// policy before the upgrade pass runs, so a held-back formula also holds back
// every pending upgrade that would have to install it.

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

/// One desktop-notification line for upgrades newly held back this pass:
/// "den held back 2 upgrades (unattended trust policy): a, b. See `den doctor`."
std::string held_back_notification(const std::vector<std::string>& names);

} // namespace den
