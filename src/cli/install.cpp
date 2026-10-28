// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "install.h"

#include "../activity/activity.h"
#include "../cask/cask.h"
#include "../core/error.h"
#include "../core/version.h"
#include "../daemon/daemon.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../store/pins.h"
#include "../store/store.h"

#include <iostream>
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

void install_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names, const MaterialiseOptions& link_opts) {
    auto active = active_env_path(config.den_home);
    auto pname = std::string(provider.provider_name());

    for (const auto& name : names) {
        auto result = provider.install(config, name, "");

        with_manifest(config.den_home, active, [&](Manifest& m) {
            auto& bucket = m.packages[pname];
            bucket[name] = result.resolved_version;
            auto& auto_bucket = m.auto_deps[pname];
            for (const auto& dep : result.auto_deps) {
                if (!bucket.count(dep)) {
                    if (const auto* dep_pkg = idx.find(dep)) {
                        bucket[dep] = dep_pkg->version;
                    }
                }
                auto_bucket.insert(dep);
            }
        });

        std::cout << "==> Installed " << name << " " << result.resolved_version << "\n";
    }

    std::cout << "==> Materialising environment...\n";
    auto links = materialise(config.den_home, config.store, active, &idx, link_opts);
    if (report_link_count(links, cask_only(config, names)))
        std::cout << "  " << links << " symlinks\n";
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
            switch (version_delta(version, pkg->version)) {
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
            const auto delta = pkg->version == it->second ? VersionDelta::Same
                                                          : version_delta(it->second, pkg->version);
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
    const auto& upgrades = split.upgrade;
    if (!split.held.empty()) {
        std::cout << "Not upgrading " << split.held.size() << " pinned package(s):\n";
        for (const auto& h : split.held) {
            std::cout << "  " << h.name << " " << h.installed << " (pinned; " << h.available
                      << " available)\n";
        }
    }

    if (upgrades.empty()) {
        if (explicitly_pinned.empty()) {
            if (older.empty())
                std::cout << "All packages are up to date.\n";
            else
                std::cout << "Nothing to upgrade.\n";
            return;
        }
        throw UserError(std::to_string(explicitly_pinned.size()) +
                        " requested package(s) are pinned");
    }

    std::cout << upgrades.size() << " package(s) to upgrade:\n";
    for (const auto& u : upgrades) {
        std::cout << "  " << u.name << " " << u.installed << " -> " << u.available << "\n";
    }

    std::vector<std::string> upgraded_names;
    for (const auto& u : upgrades) {
        provider.install(config, u.name, u.available);
        upgraded_names.push_back(u.name);
    }

    auto pname = std::string(provider.provider_name());
    with_manifest(config.den_home, active, [&](Manifest& m) {
        auto& bucket = m.packages[pname];
        for (const auto& u : upgrades) {
            bucket[u.name] = u.available;
        }
    });

    {
        auto ts = now_secs();
        std::vector<ActivityEntry> entries;
        entries.reserve(upgrades.size());
        for (const auto& u : upgrades) {
            entries.push_back({ts, u.name, u.installed, u.available, "manual"});
        }
        record_activity(config.den_home, entries);
    }

    std::cout << "==> Materialising environment...\n";
    auto links = materialise(config.den_home, config.store, active, &idx);
    if (report_link_count(links, cask_only(config, upgraded_names)))
        std::cout << "  " << links << " symlinks\n";
    std::cout << "==> " << upgrades.size() << " package(s) upgraded.\n";
    if (!explicitly_pinned.empty()) {
        throw UserError(std::to_string(explicitly_pinned.size()) +
                        " requested package(s) are pinned");
    }
}

} // namespace den
