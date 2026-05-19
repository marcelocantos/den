// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "homebrew_provider.h"

#include "../build/relocate.h"
#include "../core/config.h"
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
#include <string>
#include <vector>

namespace den {

namespace {

std::vector<const Package*> resolve_install_order(const PackageIndex& idx, const fs::path& store,
                                                  const std::string& name) {
    std::vector<const Package*> order;
    std::set<std::string> visited;
    std::set<std::string> in_stack;

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
            for (const auto& dep : pkg->dependencies) {
                if (!is_installed(store, dep, "")) {
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

// "ffmpeg" → "homebrew/core/ffmpeg"; "python@3.12" → "homebrew/core/python/3.12"
std::string ghcr_repo_path(const std::string& name) {
    std::string path = name;
    auto at = path.find('@');
    if (at != std::string::npos) {
        path[at] = '/';
    }
    return "homebrew/core/" + path;
}

bool install_one(const Config& config, const Package& pkg) {
    if (is_installed(config.store, pkg.name, pkg.version)) {
        SPDLOG_DEBUG("{} {} already installed", pkg.name, pkg.version);
        return false;
    }

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

    auto cache_dir = config.cache / "archives";
    fs::create_directories(cache_dir);

    fs::path archive_path;
    if (archive.url.find("ghcr.io") != std::string::npos || archive.url.empty()) {
        auto repo = ghcr_repo_path(pkg.name);
        archive_path = fetch_ghcr_blob(repo, archive.sha256, cache_dir);
    } else {
        archive_path = fetch_archive(archive.url, archive.sha256, cache_dir);
    }

    auto dest = package_path(config.store, pkg.name, pkg.version);
    auto tmp_dir = config.cache / ("tmp-extract-" + pkg.name + "-" + pkg.version);
    std::error_code rm_ec;
    fs::remove_all(tmp_dir, rm_ec);
    fs::create_directories(tmp_dir);

    std::cout << "==> Extracting " << pkg.name << " " << pkg.version << "\n";
    auto result = extract_archive(archive_path, tmp_dir);
    SPDLOG_INFO("extracted {} files", result.files.size());

    auto inner = tmp_dir / pkg.name / pkg.version;
    if (!fs::is_directory(inner)) {
        inner = tmp_dir / result.root;
    }

    if (fs::exists(dest)) {
        fs::remove_all(dest);
    }
    fs::create_directories(dest.parent_path());
    fs::rename(inner, dest);

    if (archive.relocation != RelocationType::Portable) {
        std::cout << "==> Relocating " << pkg.name << "\n";
        relocate_bottle(dest, pkg.name, pkg.version, config.store);
    }

    std::error_code ec;
    fs::remove_all(tmp_dir, ec);

    return true;
}

} // namespace

HomebrewProvider::HomebrewProvider(const PackageIndex* idx) : idx_(idx) {}

std::string_view HomebrewProvider::provider_name() const {
    return "homebrew";
}

bool HomebrewProvider::accepts(std::string_view /*name*/) const {
    // Default fallback provider — claims any name.
    return true;
}

InstallResult HomebrewProvider::install(const Config& config, std::string_view name,
                                        std::string_view /*version_hint*/) {
    if (!idx_) {
        throw UserError("HomebrewProvider::install requires a package index");
    }

    std::string name_str(name);
    auto* pkg = idx_->find(name_str);
    if (!pkg) {
        throw UserError("package '" + name_str + "' not found in index. Run `den update` first.");
    }

    // Forward conflict check: does this package conflict with anything already
    // present? We use the active env's manifest as the authoritative "what's
    // installed" view, because the on-disk store is a shared Cellar (a package
    // may sit there without being part of any env). Conflicts are scoped to
    // the Homebrew block — only Homebrew formulae declare conflicts_with, and
    // colliding names across providers are a separate concern.
    auto active = active_env_path(config.den_home);
    auto manifest = read_manifest(config.den_home, active);
    const auto& homebrew_pkgs = manifest.packages["homebrew"];

    for (const auto& conflict : pkg->conflicts_with) {
        if (homebrew_pkgs.count(conflict)) {
            throw UserError("cannot install '" + name_str + "': conflicts with '" + conflict +
                            "' (already installed). Uninstall '" + conflict + "' first.");
        }
    }
    // Reverse conflict check.
    for (const auto& [installed_name, _] : homebrew_pkgs) {
        const auto* installed_pkg = idx_->find(installed_name);
        if (!installed_pkg)
            continue;
        for (const auto& conflict : installed_pkg->conflicts_with) {
            if (conflict == name_str) {
                throw UserError("cannot install '" + name_str + "': conflicts with '" +
                                installed_name + "' (already installed). Uninstall '" +
                                installed_name + "' first.");
            }
        }
    }

    auto install_order = resolve_install_order(*idx_, config.store, name_str);

    InstallResult result;
    result.resolved_version = pkg->version;
    for (const auto* p : install_order) {
        install_one(config, *p);
        if (p->name != name_str) {
            result.auto_deps.push_back(p->name);
        }
    }

    return result;
}

void HomebrewProvider::uninstall(const Config& /*config*/, std::string_view /*name*/) {
    // Homebrew's shared Cellar is intentionally retained for fast reinstall —
    // the CLI removes the package from the active env's manifest and re-
    // materialises; the keg stays on disk and is reclaimed by `den cleanup`.
    // So uninstall is a no-op at the provider level.
}

std::vector<InstalledPackage> HomebrewProvider::list_installed(const Config& config) const {
    return ::den::list_installed(config.store);
}

fs::path HomebrewProvider::package_root(const Config& config, std::string_view name,
                                        std::string_view version) const {
    // Cellar layout: <store>/<name>/<version>/
    return config.store / std::string(name) / std::string(version);
}

std::vector<fs::path> HomebrewProvider::binary_paths(const Config& /*config*/,
                                                     const InstalledPackage& pkg) const {
    // Homebrew kegs expose five standard subdirectories — the materialiser
    // symlinks the contents of each into the env's matching subdir.
    std::vector<fs::path> paths;
    for (const auto& sub : {"bin", "sbin", "lib", "include", "share"}) {
        auto p = pkg.path / sub;
        if (fs::is_directory(p)) {
            paths.push_back(p);
        }
    }
    return paths;
}

} // namespace den
