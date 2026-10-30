// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "daemon.h"

#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace den {

namespace fs = std::filesystem;

// Result of a single upgrade pass (🎯T51 / 🎯T72).
//   - applied:  upgrades that were carried out this pass.
//   - deferred: upgrades held back because the installed keg's files are
//               currently in use by a live process.
struct UpgradePassResult {
    std::vector<PendingUpgrade> applied;
    std::vector<DeferredUpgrade> deferred;
    // Pending upgrades held back by `den pin`. They stay pending (so
    // `den outdated` still shows them) but are never applied.
    std::vector<PendingUpgrade> pinned;
    // Pending entries whose `available` is not newer than `installed` (e.g.
    // a stale state file from an older den). Dropped, never applied: an
    // upgrade never downgrades.
    std::vector<PendingUpgrade> not_newer;
    // Pending upgrades not applied because they would pour a package
    // disabled upstream (UpgradeDisabled). They stay pending.
    std::vector<DisabledBlocked> disabled_blocked;
};

// Thrown by an `apply` hook when the upgrade has to wait rather than fail:
// a dependent that would move in the same batch is in use. The pass records
// a deferral (reason = what()), keeps the package pending and logs no error.
struct UpgradeDeferred : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Thrown by an `apply` hook when the upgrade would pour a package the index
// marks disabled (apply_daemon_upgrade, via disabled_pours). Nothing was
// poured. The pass records it in state.disabled_blocked (every blocker),
// keeps the package pending, logs it, and records no error.
struct UpgradeDisabled : std::runtime_error {
    explicit UpgradeDisabled(std::vector<DisabledBlocker> in_the_way)
        : std::runtime_error(joined(in_the_way)), blockers(std::move(in_the_way)) {}
    std::vector<DisabledBlocker> blockers; // every disabled package the upgrade would pour

  private:
    static std::string joined(const std::vector<DisabledBlocker>& bs) {
        std::string s;
        for (const auto& b : bs)
            s += (s.empty() ? "" : "; ") + b.reason;
        return s;
    }
};

// Run one upgrade pass over the pending upgrades recorded in `state`.
//
// For each pending upgrade the installed keg (cellar/<name>/<installed>) is
// probed for in-use files. If any file is held open by a live process the
// upgrade is deferred (recorded in state.deferred and the result); otherwise
// it is applied — the pending entry is cleared, any matching deferral is
// dropped, the activity log is updated, and state.last_upgrade is bumped.
//
// `state` is mutated in place so a subsequent pass re-attempts deferrals once
// the holding process has exited. Returns the per-pass tally.
//
// Packages pinned in den_home/pins.json are never applied: they stay in
// state.pending and are reported in result.pinned. A corrupt pins.json holds
// every upgrade back (fail closed) and records the error in state.errors.
//
// `apply`, when set, performs the physical upgrade (pour + manifest) for a
// not-in-use package. If it throws, the package stays pending, the error is
// appended to state.errors, and nothing is logged as applied.
UpgradePassResult run_upgrade_pass(const fs::path& cellar, const fs::path& den_home,
                                   DaemonState& state,
                                   const std::function<void(const PendingUpgrade&)>& apply = {});

} // namespace den
