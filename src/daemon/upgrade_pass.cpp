// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "upgrade_pass.h"

#include "../activity/activity.h"
#include "../core/version.h"
#include "../store/pins.h"
#include "../store/store.h"
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

// Drop the disabled-package block for `name` (its upgrade just applied).
void clear_disabled_block(DaemonState& state, const std::string& name) {
    std::erase_if(state.disabled_blocked, [&](const DisabledBlocked& b) { return b.name == name; });
}

// Record (or refresh) the disabled-package block for `b.name`.
void record_disabled_block(DaemonState& state, const DisabledBlocked& b) {
    for (auto& existing : state.disabled_blocked) {
        if (existing.name == b.name) {
            existing = b;
            return;
        }
    }
    state.disabled_blocked.push_back(b);
    std::sort(state.disabled_blocked.begin(), state.disabled_blocked.end(),
              [](const DisabledBlocked& x, const DisabledBlocked& y) { return x.name < y.name; });
}

} // namespace

UpgradePassResult run_upgrade_pass(const fs::path& cellar, const fs::path& den_home,
                                   DaemonState& state,
                                   const std::function<void(const PendingUpgrade&)>& apply) {
    UpgradePassResult result;

    // Work on a snapshot of the pending list; we rebuild state.pending from
    // the items that remain (deferred) so applied items are removed.
    std::vector<PendingUpgrade> still_pending;
    still_pending.reserve(state.pending.size());

    std::vector<ActivityEntry> activity;

    PinMap pins;
    bool pins_ok = true;
    try {
        pins = read_pins(den_home);
    } catch (const std::exception& e) {
        // Fail closed: never let an unreadable pin file release pins.
        SPDLOG_WARN("upgrade_pass: {}; holding all upgrades", e.what());
        state.errors.push_back(std::string("pins: ") + e.what());
        pins_ok = false;
    }

    for (const auto& pu : state.pending) {
        if (!pins_ok || pins.count(pu.name)) {
            SPDLOG_INFO("upgrade_pass: {} is pinned; not upgrading {} -> {}", pu.name, pu.installed,
                        pu.available);
            result.pinned.push_back(pu);
            still_pending.push_back(pu);
            continue;
        }
        // Validate: never let an empty/relative name escape into a path that
        // could resolve outside the Cellar.
        if (pu.name.empty() || pu.installed.empty()) {
            SPDLOG_WARN("upgrade_pass: skipping malformed pending entry (name='{}')", pu.name);
            still_pending.push_back(pu);
            continue;
        }

        // Defence in depth: compute_pending never queues a downgrade, but
        // state.pending may come from elsewhere (an older den's state file).
        // The installed side's scheme is re-read from the keg's receipt; the
        // index side's comes from the pending entry (0 for a pre-scheme state).
        const int have_scheme = installed_version_scheme(cellar / pu.name / pu.installed);
        if (version_delta(pu.installed, have_scheme, pu.available, pu.available_scheme) !=
            VersionDelta::Newer) {
            SPDLOG_WARN("upgrade_pass: {} {} -> {} is not an upgrade; not applying", pu.name,
                        pu.installed, pu.available);
            result.not_newer.push_back(pu);
            continue; // dropped from pending
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
        if (apply) {
            try {
                apply(pu);
            } catch (const UpgradeDisabled& e) {
                DisabledBlocked b{pu.name, pu.installed, pu.available, e.blockers, now_secs()};
                SPDLOG_WARN("upgrade_pass: {}", disabled_blocked_message(b));
                log_daemon(den_home, "not upgrading " + disabled_blocked_message(b));
                record_disabled_block(state, b);
                result.disabled_blocked.push_back(std::move(b));
                still_pending.push_back(pu);
                continue;
            } catch (const UpgradeDeferred& e) {
                DeferredUpgrade du{pu.name, pu.installed, pu.available, e.what()};
                SPDLOG_INFO("upgrade_pass: deferring {} {} -> {} ({})", pu.name, pu.installed,
                            pu.available, e.what());
                record_deferral(state, du);
                result.deferred.push_back(du);
                still_pending.push_back(pu);
                continue;
            } catch (const std::exception& e) {
                SPDLOG_WARN("upgrade_pass: {} {} -> {} failed: {}", pu.name, pu.installed,
                            pu.available, e.what());
                state.errors.push_back("upgrade " + pu.name + ": " + e.what());
                still_pending.push_back(pu);
                continue;
            }
        }
        clear_deferral(state, pu.name);
        clear_disabled_block(state, pu.name);
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
