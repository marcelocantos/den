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

    // Only surface a deferral section when there is something to report, so
    // the common case stays quiet.
    if (!state.deferred.empty()) {
        out << "Deferred upgrades (managed binary in use):\n";
        for (const auto& d : state.deferred) {
            out << "  " << d.name << " " << d.installed << " -> " << d.available << "  ["
                << (d.reason.empty() ? "in use" : d.reason) << "]\n";
        }
    }
}

} // namespace den
