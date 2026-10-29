// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "install.h"

#include "../activity/activity.h"
#include "../build/source_plan.h"
#include "../cask/cask.h"
#include "../core/error.h"
#include "../core/version.h"
#include "../daemon/daemon.h"
#include "../daemon/trust_screen.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../env/version_change.h"
#include "../store/pins.h"
#include "../store/store.h"
#include "../trust/unattended.h"

#include <iostream>
#include <map>
#include <set>
#include <vector>

namespace den {

bool cask_only(const Config& config, const std::vector<std::string>& names) {
    if (names.empty())
        return false;
    for (const auto& name : names) {
        std::string_view token = name;
        if (token.starts_with("cask/"))
            token.remove_prefix(5);
        if (!is_cask_installed(config, token))
            return false;
    }
    return true;
}

bool report_link_count(uint32_t links, bool casks_only) {
    return links != 0 || !casks_only;
}

void report_version_change_failures(const Config& config, const VersionChangeResult& res,
                                    const std::string& verb) {
    std::string msg;
    for (const auto& rb : res.rolled_back) {
        if (!msg.empty())
            msg += "\n";
        msg += describe_rollback(config.store.parent_path(), rb, verb);
    }
    if (!res.conflicts.empty()) {
        if (!msg.empty())
            msg += "\n";
        msg += describe_conflicts(config.store.parent_path(), res.conflicts);
    }
    if (!msg.empty())
        throw UserError(msg);
}

void install_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names, const MaterialiseOptions& link_opts) {
    auto active = active_env_path(config.den_home);
    auto pname = std::string(provider.provider_name());

    // A package already in the manifest moves to the provider's version as a
    // unit: if the new version cannot be linked, it stays where it was.
    const auto manifest = read_manifest(config.den_home, active);
    std::vector<VersionChange> changes;
    for (const auto& name : names) {
        VersionChange c{name, "", ""};
        if (auto b = manifest.packages.find(pname); b != manifest.packages.end()) {
            if (auto it = b->second.find(name); it != b->second.end())
                c.from = it->second;
        }
        changes.push_back(std::move(c));
    }

    VersionChangeOptions opts;
    opts.link_opts = link_opts;
    opts.record_auto_deps = true;
    std::cout << "==> Materialising environment...\n";
    auto res = apply_version_changes(config, provider, &idx, changes, opts);
    for (const auto& c : res.applied) {
        std::cout << "==> Installed " << c.name << " " << c.to << "\n";
    }
    if (report_link_count(res.links, cask_only(config, names)))
        std::cout << "  " << res.links << " symlinks\n";
    report_version_change_failures(config, res, "install");
}

void install_from_source(const Config& config, const PackageIndex& idx,
                         const std::vector<std::string>& names, const MaterialiseOptions& link_opts,
                         const SourcePlanRunner& runner) {
    auto active = active_env_path(config.den_home);
    // G3: plan the whole chain first. Dependencies pour when their
    // bottle pours into den's Cellar and are built from source when
    // it can't (a fixed Cellar such as /opt/homebrew/Cellar); build
    // dependencies of every keg built are installed too.
    // Each source build goes through trust.unattended_source_builds
    // (trust/unattended.h): off, or allowlisted formulae whose source
    // hash cross-checks. Everything else is deferred and reported,
    // along with whatever needs it.
    SourcePlanOptions plan_opts;
    plan_opts.force_source.insert(names.begin(), names.end());
    auto plan = plan_source_build(config, idx, names, plan_opts);
    std::cout << "==> Source build plan:\n" << describe_source_plan(plan);
    auto res = run_source_plan(config, idx, plan, current_trust_policy(config), runner);
    const std::set<std::string> targets(names.begin(), names.end());
    size_t deferred_targets = 0;
    with_manifest(config.den_home, active, [&](Manifest& m) {
        auto& bucket = m.packages["homebrew"];
        auto& auto_bucket = m.auto_deps["homebrew"];
        for (const auto& step : plan.steps) {
            if (res.is_deferred(step.name)) {
                deferred_targets += targets.count(step.name);
                continue;
            }
            if (targets.count(step.name)) {
                bucket[step.name] = step.version;
                auto_bucket.erase(step.name);
            } else if (!step.build_only && !bucket.count(step.name)) {
                // Runtime dependencies join the environment as the
                // bottle path records them; build-only ones stay out.
                bucket[step.name] = step.version;
                auto_bucket.insert(step.name);
            }
        }
    });
    if (deferred_targets < names.size()) {
        auto links = materialise(config.den_home, config.store, active, &idx, link_opts);
        std::cout << "Materialised: " << links << " symlinks.\n";
    }
    if (!res.deferred.empty()) {
        throw UserError("deferred " + std::to_string(res.deferred.size()) +
                        " unattended source build step(s); nothing was installed for them");
    }
}

void uninstall_packages(const Config& config, PackageProvider& provider,
                        const std::vector<std::string>& names) {
    auto active = active_env_path(config.den_home);
    auto pname = std::string(provider.provider_name());

    // Decide before uninstalling: afterwards the casks' stages are gone.
    const bool casks_only = cask_only(config, names);
    std::vector<std::string> removed;
    with_manifest(config.den_home, active, [&](Manifest& m) {
        auto& bucket = m.packages[pname];
        auto& auto_bucket = m.auto_deps[pname];
        for (const auto& name : names) {
            if (!bucket.count(name)) {
                std::cerr << "warning: '" << name << "' is not installed in environment '" << active
                          << "'\n";
                continue;
            }
            provider.uninstall(config, name);
            bucket.erase(name);
            auto_bucket.erase(name);
            removed.push_back(name);
            std::cout << "Removed '" << name << "' from environment '" << active << "'.\n";
            // brew parity: uninstalling a package drops its pin.
            if (unpin_package(config.den_home, name)) {
                std::cout << "Unpinned '" << name << "'.\n";
            }
        }
    });

    auto links = materialise(config.den_home, config.store, active);
    if (report_link_count(links, casks_only))
        std::cout << "Materialised: " << links << " symlinks.\n";

    // The keg stays until `den cleanup`, but <prefix>/opt/<name> should not
    // keep advertising a version no environment uses any more.
    if (pname == "homebrew") {
        for (const auto& name : removed) {
            std::string to;
            switch (release_prefix_opt_link(config.store, config.den_home, keg_name(name), &to)) {
            case OptReleaseAction::HandedOff:
                std::cout << "Pointed opt/" << name << " at " << name << " " << to << ".\n";
                break;
            case OptReleaseAction::Removed:
                std::cout << "Removed opt/" << name << " link.\n";
                break;
            default:
                break;
            }
        }
    }
}

void upgrade_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names) {
    auto active = active_env_path(config.den_home);
    auto resolved = resolve(config.den_home, active);

    const auto pins = read_pins(config.den_home);
    std::vector<UpgradeCandidate> candidates;
    std::vector<std::string> explicitly_pinned;
    // Index older than installed: an upgrade never downgrades (that takes an
    // explicit `den use <name> <version>`), so these are reported and skipped.
    std::vector<UpgradeCandidate> older;

    if (names.empty()) {
        for (const auto& [name, version] : resolved) {
            auto* pkg = idx.find(name);
            if (!pkg || pkg->version.empty() || pkg->version == version)
                continue;
            const int have_scheme =
                installed_version_scheme(config.store / keg_name(name) / version);
            switch (version_delta(version, have_scheme, pkg->version, pkg->version_scheme)) {
            case VersionDelta::Newer:
                candidates.push_back({name, version, pkg->version});
                break;
            case VersionDelta::Older:
                older.push_back({name, version, pkg->version});
                break;
            case VersionDelta::Same:
                break;
            }
        }
    } else {
        for (const auto& name : names) {
            auto it = resolved.find(name);
            if (it == resolved.end()) {
                std::cerr << "warning: '" << name << "' is not installed\n";
                continue;
            }
            if (auto pin = pins.find(name); pin != pins.end()) {
                // brew: "<name> is pinned. You must unpin it to upgrade it."
                std::cerr << "error: '" << name << "' is pinned at " << pin->second
                          << "; run `den unpin " << name << "` to upgrade it\n";
                explicitly_pinned.push_back(name);
                continue;
            }
            auto* pkg = idx.find(name);
            if (!pkg) {
                std::cerr << "warning: '" << name << "' not in index\n";
                continue;
            }
            const int have_scheme =
                installed_version_scheme(config.store / keg_name(name) / it->second);
            const auto delta =
                pkg->version == it->second
                    ? VersionDelta::Same
                    : version_delta(it->second, have_scheme, pkg->version, pkg->version_scheme);
            if (delta == VersionDelta::Newer) {
                candidates.push_back({name, it->second, pkg->version});
            } else if (delta == VersionDelta::Older) {
                older.push_back({name, it->second, pkg->version});
            } else {
                std::cout << name << " is already up to date (" << it->second << ").\n";
            }
        }
    }

    if (!older.empty()) {
        std::cout << "Not downgrading " << older.size()
                  << " package(s) (index older than installed):\n";
        for (const auto& o : older) {
            std::cout << "  " << o.name << " " << o.installed << " (index has " << o.available
                      << "; to downgrade: den use " << o.name << " " << o.available << ")\n";
        }
    }

    auto split = partition_pinned(candidates, pins);
    std::vector<UpgradeCandidate> upgrades = split.upgrade;
    if (!split.held.empty()) {
        std::cout << "Not upgrading " << split.held.size() << " pinned package(s):\n";
        for (const auto& h : split.held) {
            std::cout << "  " << h.name << " " << h.installed << " (pinned; " << h.available
                      << " available)\n";
        }
    }

    // `den --unattended upgrade` screens its candidates exactly as the
    // daemon's maintenance cycle does (screen_unattended_upgrades): a formula
    // the trust policy holds back sits this run out with every candidate that
    // would install it, each hold goes in the trust ledger, and the rest
    // upgrade. The held set is reported once, here, and the run exits
    // non-zero after the rest are applied.
    std::map<std::string, std::string> trust_held;
    const TrustPolicy policy = current_trust_policy(config);
    if (policy.unattended && !upgrades.empty()) {
        std::vector<PendingUpgrade> pending;
        for (const auto& u : upgrades)
            pending.push_back({u.name, u.installed, u.available});
        trust_held = screen_unattended_upgrades(config, idx, pending, policy);
        if (!trust_held.empty()) {
            std::cout << "Held back " << trust_held.size()
                      << " package(s) (unattended trust policy):\n";
            for (const auto& u : upgrades)
                if (auto it = trust_held.find(u.name); it != trust_held.end())
                    std::cout << "  " << u.name << " " << u.installed << " -> " << u.available
                              << ": " << it->second << "\n";
            std::erase_if(upgrades,
                          [&](const UpgradeCandidate& u) { return trust_held.contains(u.name); });
        }
    }
    auto fail_if_held = [&] {
        if (!explicitly_pinned.empty())
            throw UserError(std::to_string(explicitly_pinned.size()) +
                            " requested package(s) are pinned");
        if (!trust_held.empty())
            throw UserError(std::to_string(trust_held.size()) +
                            " package(s) held back by the unattended trust policy; review with "
                            "`den trust review`");
    };

    if (upgrades.empty()) {
        fail_if_held();
        if (older.empty())
            std::cout << "All packages are up to date.\n";
        else
            std::cout << "Nothing to upgrade.\n";
        return;
    }

    std::cout << upgrades.size() << " package(s) to upgrade:\n";
    for (const auto& u : upgrades) {
        std::cout << "  " << u.name << " " << u.installed << " -> " << u.available << "\n";
    }

    std::vector<VersionChange> changes;
    std::vector<std::string> upgraded_names;
    for (const auto& u : upgrades) {
        changes.push_back({u.name, u.installed, u.available});
        upgraded_names.push_back(u.name);
    }

    // Each package moves as a unit: one that cannot be linked at its new
    // version is put back (manifest, links, opt link) and its new keg removed.
    std::cout << "==> Materialising environment...\n";
    // Installed dependents whose index entry is a revision bump (homebrew-core
    // rebuilt them for the new dependency) join the batch, as with brew.
    VersionChangeOptions opts;
    opts.upgrade_dependents = true;
    if (policy.unattended) {
        // A dependent pulled into the batch is screened like any candidate.
        opts.screen_dependents = [&](const std::vector<VersionChange>& cs) {
            std::vector<PendingUpgrade> pending;
            for (const auto& c : cs)
                pending.push_back({c.name, c.from, c.to});
            return screen_unattended_upgrades(config, idx, pending, policy);
        };
    }
    auto res = apply_version_changes(config, provider, &idx, changes, opts);
    if (!res.dependents.empty()) {
        std::cout << "Also upgraded " << res.dependents.size()
                  << " dependent(s) rebuilt for the new version:\n";
        for (const auto& d : res.dependents) {
            std::cout << "  " << d.name << " " << d.from << " -> " << d.to << "\n";
        }
    }
    if (!res.pinned_dependents.empty()) {
        std::cout << "Not upgrading " << res.pinned_dependents.size()
                  << " pinned dependent(s) (they may need the old version):\n";
        for (const auto& d : res.pinned_dependents) {
            std::cout << "  " << d.name << " " << d.from << " (pinned; " << d.to << " available)\n";
        }
    }
    if (!res.held_dependents.empty()) {
        std::cout << "Held back " << res.held_dependents.size()
                  << " dependent(s) (unattended trust policy):\n";
        for (const auto& [d, why] : res.held_dependents) {
            std::cout << "  " << d.name << " " << d.from << " -> " << d.to << ": " << why << "\n";
            trust_held.emplace(d.name, why);
        }
    }

    if (!res.applied.empty()) {
        auto ts = now_secs();
        std::vector<ActivityEntry> entries;
        entries.reserve(res.applied.size());
        for (const auto& c : res.applied) {
            entries.push_back({ts, c.name, c.from, c.to, "manual"});
        }
        record_activity(config.den_home, entries);
    }

    if (report_link_count(res.links, cask_only(config, upgraded_names)))
        std::cout << "  " << res.links << " symlinks\n";
    std::cout << "==> " << res.applied.size() << " package(s) upgraded.\n";
    if (!res.rolled_back.empty()) {
        std::cout << "==> " << res.rolled_back.size() << " package(s) rolled back.\n";
    }
    report_version_change_failures(config, res, "upgrade");
    fail_if_held();
}

} // namespace den
