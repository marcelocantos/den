// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "install.h"

#include "../activity/activity.h"
#include "../daemon/daemon.h"
#include "../env/environment.h"
#include "../env/manifest.h"

#include <iostream>
#include <vector>

namespace den {

void install_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names) {
    auto active = active_env_path(config.den_home);

    for (const auto& name : names) {
        auto result = provider.install(config, name, "");

        with_manifest(config.den_home, active, [&](Manifest& m) {
            m.packages[name] = result.resolved_version;
            for (const auto& dep : result.auto_deps) {
                if (!m.packages.count(dep)) {
                    if (const auto* dep_pkg = idx.find(dep)) {
                        m.packages[dep] = dep_pkg->version;
                    }
                }
                m.auto_deps.insert(dep);
            }
        });

        std::cout << "==> Installed " << name << " " << result.resolved_version << "\n";
    }

    std::cout << "==> Materialising environment...\n";
    auto links = materialise(config.den_home, config.store, active, &idx);
    std::cout << "  " << links << " symlinks\n";
}

void uninstall_packages(const Config& config, PackageProvider& provider,
                        const std::vector<std::string>& names) {
    auto active = active_env_path(config.den_home);

    with_manifest(config.den_home, active, [&](Manifest& m) {
        for (const auto& name : names) {
            if (!m.packages.count(name)) {
                std::cerr << "warning: '" << name << "' is not installed in environment '" << active
                          << "'\n";
                continue;
            }
            provider.uninstall(config, name);
            m.packages.erase(name);
            m.auto_deps.erase(name);
            std::cout << "Removed '" << name << "' from environment '" << active << "'.\n";
        }
    });

    auto links = materialise(config.den_home, config.store, active);
    std::cout << "Materialised: " << links << " symlinks.\n";
}

void upgrade_packages(const Config& config, PackageProvider& provider, const PackageIndex& idx,
                      const std::vector<std::string>& names) {
    auto active = active_env_path(config.den_home);
    auto resolved = resolve(config.den_home, active);

    struct Upgrade {
        std::string name;
        std::string installed;
        std::string available;
    };
    std::vector<Upgrade> upgrades;

    if (names.empty()) {
        for (const auto& [name, version] : resolved) {
            auto* pkg = idx.find(name);
            if (pkg && pkg->version != version) {
                upgrades.push_back({name, version, pkg->version});
            }
        }
    } else {
        for (const auto& name : names) {
            auto it = resolved.find(name);
            if (it == resolved.end()) {
                std::cerr << "warning: '" << name << "' is not installed\n";
                continue;
            }
            auto* pkg = idx.find(name);
            if (!pkg) {
                std::cerr << "warning: '" << name << "' not in index\n";
                continue;
            }
            if (pkg->version != it->second) {
                upgrades.push_back({name, it->second, pkg->version});
            } else {
                std::cout << name << " is already up to date (" << it->second << ").\n";
            }
        }
    }

    if (upgrades.empty()) {
        std::cout << "All packages are up to date.\n";
        return;
    }

    std::cout << upgrades.size() << " package(s) to upgrade:\n";
    for (const auto& u : upgrades) {
        std::cout << "  " << u.name << " " << u.installed << " -> " << u.available << "\n";
    }

    for (const auto& u : upgrades) {
        provider.install(config, u.name, u.available);
    }

    with_manifest(config.den_home, active, [&](Manifest& m) {
        for (const auto& u : upgrades) {
            m.packages[u.name] = u.available;
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
    std::cout << "  " << links << " symlinks\n";
    std::cout << "==> " << upgrades.size() << " package(s) upgraded.\n";
}

} // namespace den
