// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../daemon/daemon.h"
#include "../trust/degraded_installs.h"

#include <ostream>

namespace den {

// Render the daemon-status report from persisted DaemonState: last check,
// pending upgrades, and any deferred upgrades held back because a managed
// binary is in use (🎯T51 / 🎯T72). When nothing is deferred no deferral
// section is emitted.
void format_daemon_status(std::ostream& out, const DaemonState& state);

// The "Not upgrading (would pour a package disabled upstream):" section, one
// disabled_blocked_report (message and when it was recorded) per line;
// nothing when no upgrade is blocked.
// Shared by `den status`, `den outdated` and `den daemon status`.
void format_disabled_blocked_section(std::ostream& out, const DaemonState& state);

// One "Degraded-trust installs:" line (kegs poured on a single hash source,
// from the degraded-install ledger), listing up to a few of them.
void format_degraded_trust(std::ostream& out, const DegradedInstallSummary& summary);

} // namespace den
