// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "upgrade_pass.h"
#include "../activity/activity.h"
#include "inuse.h"

#include <spdlog/spdlog.h>

#include <algorithm>

namespace den {

namespace {

// Drop any deferral entry for `name` from the state (an upgrade just applied,
// so its prior deferral no longer holds).
void clear_deferral(DaemonState& state, const std::string& name) {
    state.deferred.erase(std::remove_if(state.deferred.begin(), state.deferred.end(),
                                        [&](const DeferredUpgrade& d) { return d.name == name; }),
                         state.deferred.end());
}

// Record (or refresh) a deferral for `name` in the state, deduplicating so a
// package that stays in use across many passes appears only once.
void record_deferral(DaemonState& state, const DeferredUpgrade& du) {
    for (auto& existing : state.deferred) {
        if (existing.name == du.name) {
            existing = du; // refresh installed/available/reason
            return;
        }
    }
    state.deferred.push_back(du);
}

} // namespace

UpgradePassResult run_upgrade_pass(const fs::path& cellar, const fs::path& den_home,
                                   DaemonState& state) {
    UpgradePassResult result;

    // Work on a snapshot of the pending list; we rebuild state.pending from
    // the items that remain (deferred) so applied items are removed.
    std::vector<PendingUpgrade> still_pending;
    still_pending.reserve(state.pending.size());

    std::vector<ActivityEntry> activity;

    for (const auto& pu : state.pending) {
        // Validate: never let an empty/relative name escape into a path that
        // could resolve outside the Cellar.
        if (pu.name.empty() || pu.installed.empty()) {
            SPDLOG_WARN("upgrade_pass: skipping malformed pending entry (name='{}')", pu.name);
            still_pending.push_back(pu);
            continue;
        }

        fs::path installed_keg = cellar / pu.name / pu.installed;

        std::vector<fs::path> held = files_in_use_under(installed_keg);
        if (!held.empty()) {
            DeferredUpgrade du{pu.name, pu.installed, pu.available, "binary in use"};
            SPDLOG_INFO("upgrade_pass: deferring {} {} -> {} ({} file(s) in use)", pu.name,
                        pu.installed, pu.available, held.size());
            record_deferral(state, du);
            result.deferred.push_back(du);
            still_pending.push_back(pu); // re-attempt next pass
            continue;
        }

        // Not in use → apply. The physical pour/link is owned by the install
        // pipeline; here we settle daemon bookkeeping: clear the pending and
        // deferred records and log the activity.
        SPDLOG_INFO("upgrade_pass: applying {} {} -> {}", pu.name, pu.installed, pu.available);
        clear_deferral(state, pu.name);
        result.applied.push_back(pu);
        activity.push_back({now_secs(), pu.name, pu.installed, pu.available, "daemon"});
    }

    state.pending = std::move(still_pending);

    if (!result.applied.empty()) {
        state.last_upgrade = now_secs();
        record_activity(den_home, activity);
    }

    return result;
}

} // namespace den
