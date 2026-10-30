// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "daemon_status.h"

namespace den {

void format_daemon_status(std::ostream& out, const DaemonState& state) {
    if (state.last_check) {
        auto ago = now_secs() - *state.last_check;
        out << "Last check: " << ago << "s ago\n";
    } else {
        out << "Last check: never\n";
    }

    if (state.pending.empty()) {
        out << "Pending upgrades: none\n";
    } else {
        out << "Pending upgrades:\n";
        for (const auto& p : state.pending)
            out << "  " << p.name << " " << p.installed << " -> " << p.available << "\n";
    }

    if (!state.index_older.empty()) {
        out << "Not downgrading (index older than installed):\n";
        for (const auto& p : state.index_older)
            out << "  " << p.name << " " << p.installed << " (index has " << p.available
                << "; to downgrade: den use " << p.name << " " << p.available << ")\n";
    }

    if (!state.version_held.empty()) {
        out << "Held (index version below installed):\n";
        for (const auto& h : state.version_held)
            out << "  " << h.name << " " << h.installed << " (" << h.reason
                << "; to downgrade: den use " << h.name << " " << h.available << ")\n";
    }

    // Only surface a deferral section when there is something to report, so
    // the common case stays quiet.
    if (!state.deferred.empty()) {
        out << "Deferred upgrades (managed binary in use):\n";
        for (const auto& d : state.deferred) {
            out << "  " << d.name << " " << d.installed << " -> " << d.available << "  ["
                << (d.reason.empty() ? "in use" : d.reason) << "]\n";
        }
    }

    format_disabled_blocked_section(out, state);

    if (!state.trust_held.empty()) {
        out << "Held back by the unattended trust policy (see `den trust review`):\n";
        for (const auto& h : state.trust_held)
            out << "  " << h.name << " " << h.installed << " -> " << h.available << "  ["
                << h.reason << "]\n";
    }
    if (state.last_trust_notification) {
        out << "Last held-back notification: " << (now_secs() - *state.last_trust_notification)
            << "s ago\n";
    }
}

void format_disabled_blocked_section(std::ostream& out, const DaemonState& state) {
    if (state.disabled_blocked.empty())
        return;
    out << "Not upgrading (would pour a package disabled upstream):\n";
    for (const auto& b : state.disabled_blocked)
        out << "  " << disabled_blocked_report(b) << "\n";
}

void format_degraded_trust(std::ostream& out, const DegradedInstallSummary& summary) {
    out << "Degraded-trust installs: " << format_degraded_counts(summary) << "\n";
    constexpr size_t kMaxListed = 5;
    for (size_t i = 0; i < summary.installs.size() && i < kMaxListed; ++i) {
        const auto& d = summary.installs[i];
        out << "  " << d.name << " " << d.version << "  [" << d.reason << "]\n";
    }
    if (summary.installs.size() > kMaxListed) {
        out << "  … " << (summary.installs.size() - kMaxListed) << " more (see `den doctor`)\n";
    }
}

} // namespace den
