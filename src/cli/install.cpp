// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "install.h"

#include "../core/error.h"
#include "../download/archive.h"
#include "../download/fetch.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../platform/platform.h"
#include "../store/store.h"

#include <spdlog/spdlog.h>

#include <iostream>
#include <set>
#include <vector>

namespace den {

namespace {

/// Resolve the install order for a package and all its transitive
/// dependencies using post-order DFS. Packages already installed in
/// the store are skipped.
std::vector<const Package*> resolve_install_order(const PackageIndex& idx, const fs::path& store,
                                                  const std::string& name) {
    std::vector<const Package*> order;
    std::set<std::string> visited;
    std::set<std::string> in_stack; // cycle detection

    struct Resolver {
        const PackageIndex& idx;
        const fs::path& store;
        std::vector<const Package*>& order;
        std::set<std::string>& visited;
        std::set<std::string>& in_stack;

        void visit(const std::string& n) {
            if (visited.count(n))
                return;
            if (in_stack.count(n)) {
                SPDLOG_WARN("dependency cycle detected at '{}'", n);
                return;
            }

            auto* pkg = idx.find(n);
            if (!pkg) {
                SPDLOG_WARN("dependency '{}' not found in index, skipping", n);
                return;
            }

            in_stack.insert(n);

            // Visit dependencies first (post-order).
            for (const auto& dep : pkg->dependencies) {
                // Skip if already installed.
                if (!is_installed(store, dep, "")) {
                    // We don't know the version yet for dep check — visit anyway.
                    visit(dep);
                }
            }

            in_stack.erase(n);
            visited.insert(n);
            order.push_back(pkg);
        }
    };

    Resolver r{idx, store, order, visited, in_stack};
    r.visit(name);
    return order;
}

/// Construct the GHCR repo path for a formula name.
/// E.g. "ffmpeg" -> "homebrew/core/ffmpeg"
std::string ghcr_repo_path(const std::string& name) {
    // Handle names with @ (e.g. python@3.12 -> python/3.12)
    std::string path = name;
    auto at = path.find('@');
    if (at != std::string::npos) {
        path[at] = '/';
    }
    return "homebrew/core/" + path;
}

/// Install a single package from the index into the store.
/// Returns true if installed, false if already present.
bool install_one(const Config& config, const Package& pkg) {
    if (is_installed(config.store, pkg.name, pkg.version)) {
        SPDLOG_DEBUG("{} {} already installed", pkg.name, pkg.version);
        return false;
    }

    // Find the best archive for this platform.
    std::vector<std::string> available_tags;
    for (const auto& [tag, _] : pkg.archives)
        available_tags.push_back(tag);

    auto best = best_archive_tag(config.arch, config.macos_version, available_tags);
    if (!best) {
        throw UserError("no archive available for " + pkg.name + " " + pkg.version +
                         " on this platform");
    }

    const auto& archive = pkg.archives.at(*best);
    std::cout << "==> Downloading " << pkg.name << " " << pkg.version << "\n";

    // Fetch the archive (content-addressed cache).
    auto cache_dir = config.cache / "archives";
    fs::create_directories(cache_dir);

    fs::path archive_path;
    if (archive.url.find("ghcr.io") != std::string::npos || archive.url.empty()) {
        // GHCR blob fetch.
        auto repo = ghcr_repo_path(pkg.name);
        archive_path = fetch_ghcr_blob(repo, archive.sha256, cache_dir);
    } else {
        archive_path = fetch_archive(archive.url, archive.sha256, cache_dir);
    }

    // Extract into a temporary directory first, then move contents
    // to the store. Homebrew bottles have a <name>/<version>/ prefix
    // inside the archive that we need to strip.
    auto dest = package_path(config.store, pkg.name, pkg.version);
    auto tmp_dir = config.cache / ("tmp-extract-" + pkg.name + "-" + pkg.version);
    std::error_code rm_ec;
    fs::remove_all(tmp_dir, rm_ec); // Clean any leftover from previous failed install.
    fs::create_directories(tmp_dir);

    std::cout << "==> Extracting " << pkg.name << " " << pkg.version << "\n";
    auto result = extract_archive(archive_path, tmp_dir);
    SPDLOG_INFO("extracted {} files", result.files.size());

    // Find the inner directory (bottles use <name>/<version>/ structure).
    auto inner = tmp_dir / pkg.name / pkg.version;
    if (!fs::is_directory(inner)) {
        // Fallback: try the archive root directly.
        inner = tmp_dir / result.root;
    }

    // Move contents to the store path.
    if (fs::exists(dest)) {
        fs::remove_all(dest);
    }
    fs::create_directories(dest.parent_path());
    fs::rename(inner, dest);

    // Clean up temp.
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);

    return true;
}

} // namespace

void install_packages(const Config& config, const PackageIndex& idx,
                      const std::vector<std::string>& names) {
    auto active = active_env_path(config.den_home);

    for (const auto& name : names) {
        auto* pkg = idx.find(name);
        if (!pkg) {
            throw UserError("package '" + name + "' not found in index. Run `den update` first.");
        }

        // Resolve full dependency tree.
        auto install_order = resolve_install_order(idx, config.store, name);

        // Install deps + target.
        for (const auto* p : install_order) {
            install_one(config, *p);
        }

        // Update manifest.
        with_manifest(config.den_home, active, [&](Manifest& m) {
            m.packages[pkg->name] = pkg->version;

            // Mark transitive deps as auto.
            for (const auto* p : install_order) {
                if (p->name != name) {
                    if (!m.packages.count(p->name)) {
                        m.packages[p->name] = p->version;
                    }
                    m.auto_deps.insert(p->name);
                }
            }
        });

        std::cout << "==> Installed " << name << " " << pkg->version << "\n";
    }

    // Re-materialise.
    std::cout << "==> Materialising environment...\n";
    auto links = materialise(config.den_home, config.store, active);
    std::cout << "  " << links << " symlinks\n";
}

void uninstall_packages(const Config& config, const std::vector<std::string>& names) {
    auto active = active_env_path(config.den_home);

    with_manifest(config.den_home, active, [&](Manifest& m) {
        for (const auto& name : names) {
            if (!m.packages.count(name)) {
                std::cerr << "warning: '" << name << "' is not installed in environment '" << active
                          << "'\n";
                continue;
            }
            m.packages.erase(name);
            m.auto_deps.erase(name);
            std::cout << "Removed '" << name << "' from environment '" << active << "'.\n";
        }
    });

    // Re-materialise.
    auto links = materialise(config.den_home, config.store, active);
    std::cout << "Materialised: " << links << " symlinks.\n";
}

void upgrade_packages(const Config& config, const PackageIndex& idx,
                      const std::vector<std::string>& names) {
    auto active = active_env_path(config.den_home);
    auto resolved = resolve(config.den_home, active);

    // Find packages that need upgrading.
    struct Upgrade {
        std::string name;
        std::string installed;
        std::string available;
    };
    std::vector<Upgrade> upgrades;

    if (names.empty()) {
        // Upgrade all.
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

    // Install new versions.
    for (const auto& u : upgrades) {
        auto* pkg = idx.find(u.name);
        if (!pkg)
            continue;

        auto install_order = resolve_install_order(idx, config.store, u.name);
        for (const auto* p : install_order) {
            install_one(config, *p);
        }
    }

    // Update manifest.
    with_manifest(config.den_home, active, [&](Manifest& m) {
        for (const auto& u : upgrades) {
            m.packages[u.name] = u.available;
        }
    });

    // Re-materialise.
    std::cout << "==> Materialising environment...\n";
    auto links = materialise(config.den_home, config.store, active);
    std::cout << "  " << links << " symlinks\n";
    std::cout << "==> " << upgrades.size() << " package(s) upgraded.\n";
}

} // namespace den
