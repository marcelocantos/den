// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "outdated.h"

#include <algorithm>
#include <string>

namespace den {

namespace {

// Find a deferral matching `name`; nullptr if none.
const DeferredUpgrade* find_deferral(const DaemonState& state, const std::string& name) {
    for (const auto& d : state.deferred) {
        if (d.name == name)
            return &d;
    }
    return nullptr;
}

} // namespace

void format_outdated(std::ostream& out, const DaemonState& state) {
    if (state.pending.empty() && state.deferred.empty()) {
        out << "All packages are up to date.\n";
        return;
    }

    // Column width across everything we will print.
    size_t max_name = 0;
    for (const auto& p : state.pending)
        max_name = std::max(max_name, p.name.size());
    for (const auto& d : state.deferred)
        max_name = std::max(max_name, d.name.size());

    auto pad = [&](const std::string& name) {
        std::string s = name;
        s.resize(std::max(s.size(), max_name + 2), ' ');
        return s;
    };

    // Pending upgrades. A pending item that is also deferred is annotated so
    // the user understands why it has not applied yet.
    for (const auto& p : state.pending) {
        out << pad(p.name) << p.installed << " -> " << p.available;
        if (const DeferredUpgrade* d = find_deferral(state, p.name)) {
            out << "  [deferred: " << (d->reason.empty() ? "in use" : d->reason) << "]";
        }
        out << "\n";
    }

    // Deferred upgrades that are not already shown in the pending list.
    for (const auto& d : state.deferred) {
        bool in_pending = std::any_of(state.pending.begin(), state.pending.end(),
                                      [&](const PendingUpgrade& p) { return p.name == d.name; });
        if (in_pending)
            continue;
        out << pad(d.name) << d.installed << " -> " << d.available
            << "  [deferred: " << (d.reason.empty() ? "in use" : d.reason) << "]\n";
    }
}

} // namespace den
