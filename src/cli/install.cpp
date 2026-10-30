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
#include "../env/disabled.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../env/version_change.h"
#include "../store/pins.h"
#include "../store/store.h"
#include "../trust/unattended.h"

#include <algorithm>
#include <deque>
#include <iostream>
#include <map>
#include <set>
#include <vector>

namespace den {

std::string empty_index_message() {
    return "the package index is empty; run `den update` first";
}

void require_index(const PackageIndex& idx) {
    if (idx.empty())
        throw UserError(empty_index_message());
}

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

namespace {

std::string refused_count_message(size_t n) {
    return std::to_string(n) + " requested package(s) are disabled upstream";
}

/// The UserError for a run that refused `disabled` named packages and
/// `needs_disabled` whose install would pour a disabled dependency.
void throw_if_refused(size_t disabled, size_t needs_disabled) {
    std::string msg;
    if (disabled)
        msg = refused_count_message(disabled);
    if (needs_disabled) {
        if (!msg.empty())
            msg += "; ";
        msg += std::to_string(needs_disabled) +
               " requested package(s) need a dependency that is disabled upstream";
    }
    if (!msg.empty())
        throw UserError(msg);
}

/// --force lets a disabled formula through, as `brew install --force`
/// does; a disabled cask has no such bypass (brew has none either).
bool force_bypasses(const Package& pkg, bool force) {
    return force && !is_cask_package(pkg);
}

/// Prints the error (or, under --force for formulae, the warning) for each
/// disabled package installing `name` would pour; true when `name` is
/// refused.
bool refuse_for_disabled_dependencies(const Config& config, const PackageProvider& provider,
                                      const PackageIndex& idx, const std::string& name,
                                      bool force) {
    bool refused = false;
    for (const auto& pour : disabled_pours(config, provider, idx, name, /*include_self=*/false)) {
        const bool bypass = force_bypasses(*pour.pkg, force);
        std::cerr << (bypass ? "warning: " : "error: ") << pour.message << "\n";
        refused = refused || !bypass;
    }
    return refused;
}

/// A named package that is disabled upstream but already installed at the
/// index version: there is nothing to do, so it is reported as up to date
/// with a warning instead of being refused.
void warn_disabled_up_to_date(const Package& pkg, const std::string& name,
                              const std::string& version) {
    std::cout << name << " is already up to date (" << version << ").\n";
    std::cerr << "warning: " << disabled_package_message(pkg) << "\n";
}

} // namespace

std::vector<std::string> refuse_disabled_packages(const PackageIndex& idx,
                                                  const std::vector<std::string>& names,
                                                  bool force) {
    std::vector<std::string> refused;
    for (const auto& name : names) {
        const auto* pkg = idx.find(name);
        if (pkg && pkg->disabled) {
            const bool bypass = force_bypasses(*pkg, force);
            std::cerr << (bypass ? "warning: " : "error: ") << disabled_package_message(*pkg)
                      << "\n";
            if (!bypass)
                refused.push_back(name);
        }
    }
    return refused;
}

void install_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& requested,
                      const MaterialiseOptions& link_opts, bool force) {
    auto active = active_env_path(config.den_home);
    auto pname = std::string(provider.provider_name());

    // A package the index marks disabled (homebrew-core or homebrew-cask
    // disabled it; brew refuses to install it too) is refused unless it is
    // already installed here at the index version, when there is nothing to
    // do: it is reported up to date, with a warning. The index only
    // describes Homebrew packages, so a name another provider installs is
    // never matched against it. --force installs a disabled formula anyway,
    // with the message as a warning; a disabled cask is refused regardless.
    const auto manifest = read_manifest(config.den_home, active);
    auto recorded_at = [&](const std::string& name, const std::string& version) {
        auto b = manifest.packages.find(pname);
        if (b == manifest.packages.end())
            return false;
        auto it = b->second.find(name);
        return it != b->second.end() && it->second == version;
    };
    std::vector<std::string> refused;
    std::vector<std::string> names;
    for (const auto& name : requested) {
        const Package* pkg = pname == "homebrew" ? idx.find(name) : nullptr;
        if (!pkg || !pkg->disabled) {
            names.push_back(name);
            continue;
        }
        const bool up_to_date = !pkg->version.empty() && recorded_at(name, pkg->version) &&
                                provider.is_installed(config, name, pkg->version);
        if (up_to_date) {
            warn_disabled_up_to_date(*pkg, name, pkg->version);
        } else if (force_bypasses(*pkg, force)) {
            std::cerr << "warning: " << disabled_package_message(*pkg) << "\n";
            names.push_back(name);
        } else {
            std::cerr << "error: " << disabled_package_message(*pkg) << "\n";
            refused.push_back(name);
        }
    }
    // As brew does, a name whose install would pour a disabled dependency
    // is refused too (or warned about, under --force, for a formula); a
    // disabled dependency already installed at the index version is not
    // poured, so it is not mentioned.
    size_t needs_disabled = 0;
    if (pname == "homebrew") {
        std::erase_if(names, [&](const std::string& name) {
            const bool refuse =
                refuse_for_disabled_dependencies(config, provider, idx, name, force);
            needs_disabled += refuse;
            return refuse;
        });
    }
    if (names.empty() && !requested.empty()) {
        throw_if_refused(refused.size(), needs_disabled);
        return;
    }

    // A package already in the manifest moves to the provider's version as a
    // unit: if the new version cannot be linked, it stays where it was.
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
    throw_if_refused(refused.size(), needs_disabled);
}

void install_from_source(const Config& config, const PackageIndex& idx,
                         const std::vector<std::string>& names, const MaterialiseOptions& link_opts,
                         const SourcePlanRunner& runner, bool force) {
    auto active = active_env_path(config.den_home);
    // A disabled formula is refused before anything is planned or built:
    // building it from source is no way around homebrew-core disabling it.
    // -s builds even an installed formula, so an installed one is refused
    // too. --force builds it anyway, with a warning.
    if (auto refused = refuse_disabled_packages(idx, names, force); !refused.empty())
        throw UserError(refused_count_message(refused.size()));
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
    // A disabled dependency the plan would pour or build (build-only ones
    // included) refuses every target that needs it, before anything runs.
    {
        std::set<std::string> blocked;
        for (const auto& target : names) {
            std::set<std::string> seen;
            std::deque<std::string> todo{target};
            while (!todo.empty()) {
                auto step_name = std::move(todo.front());
                todo.pop_front();
                if (!seen.insert(step_name).second)
                    continue;
                const auto* step = plan.find(step_name);
                if (!step)
                    continue;
                todo.insert(todo.end(), step->needs.begin(), step->needs.end());
                if (step_name == target || step->action == SourceStepAction::Installed ||
                    std::find(names.begin(), names.end(), step_name) != names.end())
                    continue;
                const auto* dep = idx.find(step_name);
                if (!dep || !dep->disabled)
                    continue;
                const bool bypass = force_bypasses(*dep, force);
                std::cerr << (bypass ? "warning: " : "error: ")
                          << disabled_dependency_message(target, *dep) << "\n";
                if (!bypass)
                    blocked.insert(target);
            }
        }
        throw_if_refused(0, blocked.size());
    }
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
                      const std::vector<std::string>& names, bool dry_run, bool force) {
    auto active = active_env_path(config.den_home);
    // Only this provider's own bucket is compared against `idx`. A package
    // another provider manages (cowsay 6.1 from pip) is not the index's
    // package of the same name (homebrew cowsay 3.8.4), so the flattened
    // cross-provider view would misreport it as index-older, or plan a
    // homebrew "upgrade" over it. This matches the daemon's maintenance
    // cycle, which reads only the homebrew bucket.
    const auto installed_scheme = [&config](const std::string& name, const std::string& version) {
        return installed_version_scheme(config.store / keg_name(name) / version);
    };

    const auto pins = read_pins(config.den_home);
    std::vector<UpgradeCandidate> candidates;
    std::vector<std::string> explicitly_pinned;
    // Named packages the index marks disabled that the run would upgrade:
    // refused, as brew does.
    std::vector<std::string> refused_disabled;
    // Index older than installed: an upgrade never downgrades (that takes an
    // explicit `den use <name> <version>`), so these are reported and skipped.
    std::vector<UpgradeCandidate> older;
    // Index version lower but version_scheme higher (VersionDelta::Held):
    // held with a reason, never upgraded, likewise only `den use` moves it.
    std::vector<std::pair<UpgradeCandidate, std::string>> held_lower;
    // Casks with a newer index version, left out of a bare upgrade: swapping a
    // live .app bundle is only done when the cask is named.
    std::vector<UpgradeCandidate> manual_casks;

    if (names.empty()) {
        // The same selection `den outdated`, `den doctor` and the daemon use
        // (outdated_plan -> compute_pending): casks and disabled formulae are
        // skipped, index-older and held packages reported, never applied.
        const auto plan = outdated_plan(config, idx, provider.provider_name());
        for (const auto& p : plan.pending)
            candidates.push_back({p.name, p.installed, p.available});
        for (const auto& o : plan.index_older)
            older.push_back({o.name, o.installed, o.available});
        for (const auto& h : plan.held)
            held_lower.push_back({{h.name, h.installed, h.available}, h.reason});
        for (const auto& c : plan.casks)
            manual_casks.push_back({c.name, c.installed, c.available});
    } else {
        std::map<std::string, std::string> resolved;
        {
            auto per = resolve_per_provider(config.den_home, active);
            if (auto it = per.find(std::string(provider.provider_name())); it != per.end())
                resolved = std::move(it->second);
        }
        // A name given explicitly goes through the same select_upgrade
        // comparison, but a cask named here is upgraded: naming it is the
        // manual upgrade the bare form leaves to the user. A disabled
        // formula or cask is refused when the index version is newer (the
        // upgrade would install it); otherwise there is nothing to apply,
        // so it is reported as usual, with a warning that it is disabled.
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
            const auto sel = select_upgrade(name, it->second, pkg, installed_scheme);
            if (sel.skip == UpgradeSkip::NotInIndex) {
                std::cerr << "warning: '" << name << "' not in index\n";
                continue;
            }
            const auto delta = sel.delta;
            if (pkg->disabled) {
                if (delta == VersionDelta::Newer && !force_bypasses(*pkg, force)) {
                    std::cerr << "error: " << disabled_package_message(*pkg) << "\n";
                    refused_disabled.push_back(name);
                    continue;
                }
                if (delta == VersionDelta::Same) {
                    warn_disabled_up_to_date(*pkg, name, it->second);
                    continue;
                }
                std::cerr << "warning: " << disabled_package_message(*pkg) << "\n";
            }
            if (delta == VersionDelta::Newer) {
                candidates.push_back({name, it->second, pkg->version});
            } else if (delta == VersionDelta::Older) {
                older.push_back({name, it->second, pkg->version});
            } else if (delta == VersionDelta::Held) {
                held_lower.push_back(
                    {{name, it->second, pkg->version},
                     version_hold_reason(it->second, pkg->version, pkg->version_scheme)});
            } else {
                std::cout << name << " is already up to date (" << it->second << ").\n";
            }
        }
    }

    // A candidate whose new version would pour a disabled dependency is
    // refused, as brew does (under --force a formula dependency only warns);
    // the other candidates proceed.
    size_t needs_disabled = 0;
    std::erase_if(candidates, [&](const UpgradeCandidate& c) {
        const bool refuse = refuse_for_disabled_dependencies(config, provider, idx, c.name, force);
        needs_disabled += refuse;
        return refuse;
    });

    if (!older.empty()) {
        std::cout << "Not downgrading " << older.size()
                  << " package(s) (index older than installed):\n";
        for (const auto& o : older) {
            std::cout << "  " << o.name << " " << o.installed << " (index has " << o.available
                      << "; to downgrade: den use " << o.name << " " << o.available << ")\n";
        }
    }

    if (!held_lower.empty()) {
        std::cout << "Holding " << held_lower.size()
                  << " package(s) (index version below installed):\n";
        for (const auto& [h, reason] : held_lower) {
            std::cout << "  " << h.name << " " << h.installed << " (" << reason
                      << "; to downgrade: den use " << h.name << " " << h.available << ")\n";
        }
    }

    if (!manual_casks.empty()) {
        std::cout << "Not upgrading " << manual_casks.size()
                  << " cask(s) (upgrade a cask by naming it: den upgrade <name>):\n";
        for (const auto& c : manual_casks)
            std::cout << "  " << c.name << " " << c.installed << " (" << c.available
                      << " available)\n";
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

    // --dry-run: report the plan and stop before anything with side effects
    // (the unattended trust screen writes the trust ledger).
    if (dry_run) {
        if (upgrades.empty()) {
            if (older.empty() && held_lower.empty() && manual_casks.empty() &&
                explicitly_pinned.empty() && refused_disabled.empty() && needs_disabled == 0)
                std::cout << "All packages are up to date.\n";
            else
                std::cout << "Nothing to upgrade.\n";
        } else {
            std::cout << "Would upgrade " << upgrades.size() << " package(s):\n";
            for (const auto& u : upgrades)
                std::cout << "  " << u.name << " " << u.installed << " -> " << u.available << "\n";
        }
        std::cout << "Dry run: nothing changed.\n";
        if (!explicitly_pinned.empty())
            throw UserError(std::to_string(explicitly_pinned.size()) +
                            " requested package(s) are pinned");
        throw_if_refused(refused_disabled.size(), needs_disabled);
        return;
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
    size_t refused_dependents = 0;
    auto fail_if_held = [&] {
        if (!explicitly_pinned.empty())
            throw UserError(std::to_string(explicitly_pinned.size()) +
                            " requested package(s) are pinned");
        throw_if_refused(refused_disabled.size(), needs_disabled);
        if (refused_dependents)
            throw UserError(std::to_string(refused_dependents) +
                            " dependent(s) are disabled upstream or need a dependency that is");
        if (!trust_held.empty())
            throw UserError(std::to_string(trust_held.size()) +
                            " package(s) held back by the unattended trust policy; review with "
                            "`den trust review`");
    };

    if (upgrades.empty()) {
        fail_if_held();
        if (older.empty() && held_lower.empty() && manual_casks.empty())
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
    // A revision-bumped dependent that is disabled upstream, or would pour a
    // disabled dependency, stays put and fails the run (brew reports such a
    // dependent and upgrades the rest); --force lets a formula through.
    opts.refuse_dependent = [&](const VersionChange& c) -> std::optional<std::string> {
        std::optional<std::string> why;
        for (const auto& pour :
             disabled_pours(config, provider, idx, c.name, /*include_self=*/true)) {
            if (force_bypasses(*pour.pkg, force))
                std::cerr << "warning: " << pour.message << "\n";
            else if (!why)
                why = pour.message;
        }
        return why;
    };
    auto res = apply_version_changes(config, provider, &idx, changes, opts);
    if (!res.refused_dependents.empty()) {
        std::cout << "Not upgrading " << res.refused_dependents.size()
                  << " dependent(s) (disabled upstream):\n";
        for (const auto& [d, why] : res.refused_dependents) {
            std::cout << "  " << d.name << " " << d.from << " (" << d.to << " available)\n";
            std::cerr << "error: " << why << "\n";
        }
        refused_dependents = res.refused_dependents.size();
    }
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

void upgrade_all_packages(const Config& config, const ProviderRegistry& registry,
                          const PackageIndex& idx, bool dry_run, bool force) {
    const auto manifest = read_manifest(config.den_home, active_env_path(config.den_home));
    for (const auto& [provider_name, pkgs] : manifest.packages) {
        if (!pkgs.empty() && !registry.find(provider_name)) {
            std::cerr << "warning: manifest holds entries under provider '" << provider_name
                      << "' but no provider with that name is registered; skipping them\n";
        }
    }
    PackageProvider* provider = registry.find("homebrew");
    if (!provider)
        provider = registry.default_provider();
    if (!provider)
        throw InternalError("no package provider registered");
    upgrade_packages(config, *provider, idx, {}, dry_run, force);
}

} // namespace den
