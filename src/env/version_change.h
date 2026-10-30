// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"
#include "../provider/package_provider.h"
#include "environment.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// One package moving to another version in the active environment.
struct VersionChange {
    std::string name;
    /// The version the active env's manifest records now; empty when the
    /// package is not in it yet (a fresh install, which is not rolled back).
    std::string from;
    /// The version to move to; empty means whatever the provider resolves.
    std::string to;
};

/// A change that was undone because the new version could not be linked.
struct RolledBackChange {
    VersionChange change; // `to` is the version that was attempted
    std::vector<fs::path> conflicts;
    bool keg_removed = false; // the new keg was poured by this attempt and is gone again
    /// Set for a dependent that joined the batch only because `with` (one of
    /// its runtime dependencies) moved; it is undone when `with` is.
    std::string with;
    /// Set when the new keg fails linkage verification: libraries installed
    /// kegs load through `<prefix>/opt/<name>` that the old keg had and the
    /// new one does not, one "app 1.0: bin/app loads <path>" line each. The
    /// change is undone before anything switches.
    std::vector<std::string> broken_loads;
};

struct VersionChangeResult {
    uint32_t links = 0;
    std::vector<VersionChange> applied; // `to` is the version now recorded
    std::vector<RolledBackChange> rolled_back;
    /// Conflicts nothing was rolled back for (fresh installs, which keep the
    /// manifest entry so a later materialise retries, as before).
    std::vector<PrefixConflict> conflicts;
    /// Dependents added to the batch by `upgrade_dependents` (also listed in
    /// `applied` or `rolled_back`, like any other change).
    std::vector<VersionChange> dependents;
    /// Dependents with a revision bump that were left alone because they are
    /// pinned (`to` is the index version they would have moved to).
    std::vector<VersionChange> pinned_dependents;
    /// Dependents that joined the batch whose installed keg was in use
    /// (VersionChangeOptions::keg_in_use). When set, nothing was poured or
    /// changed: the whole batch waits, so the dependency chain moves together.
    std::vector<VersionChange> deferred_in_use;
    /// Dependents with a revision bump that `screen_dependents` held back
    /// (`to` is the index version), with the screen's reason.
    std::vector<std::pair<VersionChange, std::string>> held_dependents;
    /// Dependents with a revision bump that `refuse_dependent` refused
    /// (`to` is the index version), with its reason.
    std::vector<std::pair<VersionChange, std::string>> refused_dependents;
};

struct VersionChangeOptions {
    MaterialiseOptions link_opts;
    /// Call provider.install for each change. Off for `den use`, which only
    /// switches between versions already in the store.
    bool install = true;
    /// Add the provider's auto deps to the manifest (`den install`).
    bool record_auto_deps = false;
    /// Also move installed runtime dependents of the batch whose index entry
    /// is a revision bump of the installed version (same upstream version,
    /// higher `_<revision>`), as `brew upgrade` does: homebrew-core bumps a
    /// dependent's revision when a dependency changes ABI, so the two have to
    /// move together. Needs an index. Pinned dependents are left alone.
    bool upgrade_dependents = false;
    /// When set, called for the installed keg of each dependent that joins
    /// the batch through `upgrade_dependents`, before anything is poured. If
    /// any is in use, the batch is not applied at all and those dependents
    /// are listed in VersionChangeResult::deferred_in_use. The daemon passes
    /// its in-use probe, so a dependent it would otherwise move under a
    /// running process defers its whole dependency chain.
    std::function<bool(const fs::path& keg)> keg_in_use;
    /// Before the manifest switches, check that every keg active after the
    /// batch still finds the libraries it loads through `<prefix>/opt/<dep>`
    /// for each dependency the batch moves (a library the old keg had and the
    /// new keg lacks, such as a soname bump with no rebuilt dependent, fails).
    /// A change that fails is rolled back with its brought-in dependents
    /// (RolledBackChange::broken_loads). Needs an index.
    bool verify_linkage = true;
    /// Optional screen for each dependent `upgrade_dependents` would add
    /// (the unattended trust policy): returns name -> reason for one to hold
    /// back. A held dependent stays at its version, like a pinned one, and
    /// is listed in `held_dependents`.
    std::function<std::map<std::string, std::string>(const std::vector<VersionChange>&)>
        screen_dependents;
    /// Optional check for each dependent `upgrade_dependents` would add:
    /// returns a reason to refuse it (`den upgrade` refuses one disabled
    /// upstream, or one that would pour a disabled dependency). A refused
    /// dependent stays at its version, like a pinned one, and is listed in
    /// `refused_dependents`.
    std::function<std::optional<std::string>(const VersionChange&)> refuse_dependent;
};

/// Move packages in the active environment to new versions as one unit per
/// package: install, record in the manifest, materialise. When a package that
/// was already in the manifest cannot be linked at its new version (paths in
/// the prefix den does not manage), that package is put back exactly as it
/// was: its manifest entry returns to `from`, the old version stays linked
/// and recorded, the new keg is removed from the store if this attempt poured
/// it, and `<prefix>/opt/<name>` is pointed back where it was if the pour
/// moved it. Opt links are snapshotted for the whole batch: a dependency's
/// `<prefix>/opt/<dep>` that the batch moved forward is pointed back too when
/// no change that stays applied needs it, so existing dependents keep loading
/// the dependency they had. A dependent added by `upgrade_dependents` is
/// rolled back with the dependency that brought it in. An install that throws
/// undoes every pour and every opt link the batch moved before the error
/// propagates; nothing reaches the manifest.
VersionChangeResult apply_version_changes(const Config& config, PackageProvider& provider,
                                          const PackageIndex* idx,
                                          std::vector<VersionChange> changes,
                                          const VersionChangeOptions& opts = {});

/// "upgrade of <name> <from> -> <to> was rolled back: ..." with up to
/// `max_paths` of the conflicting paths.
std::string describe_rollback(const fs::path& prefix, const RolledBackChange& rb,
                              const std::string& verb = "upgrade", size_t max_paths = 10);

} // namespace den
