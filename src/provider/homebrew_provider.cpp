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
#include "../index/dep_resolve.h"
#include "../platform/platform.h"
#include "../store/store.h"

#include <spdlog/spdlog.h>

#include <iostream>
#include <set>
#include <string>
#include <vector>

namespace den {

namespace {

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
    try {
        fs::create_directories(dest.parent_path());
    } catch (const fs::filesystem_error& e) {
        throw UserError("cannot create Cellar directory '" + dest.parent_path().string() +
                        "': " + e.code().message() + "\nCheck that '" +
                        dest.parent_path().parent_path().string() +
                        "' exists and is writable by the current user.");
    }
    try {
        fs::rename(inner, dest);
    } catch (const fs::filesystem_error& e) {
        throw UserError("cannot install '" + pkg.name + "' to '" + dest.string() +
                        "': " + e.code().message());
    }

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

    // Resolve the full install order via the SAT solver (🎯T63). It enforces
    // version constraints, conflicts, and stability preferences across the
    // transitive dependency closure — there is no greedy fallback path.
    std::vector<std::string> installed_names;
    installed_names.reserve(homebrew_pkgs.size());
    for (const auto& [installed_name, _] : homebrew_pkgs)
        installed_names.push_back(installed_name);

    auto resolution = resolve_request(*idx_, {name_str}, installed_names);
    if (!resolution.satisfiable) {
        std::string detail;
        for (const auto& e : resolution.explanation) {
            if (e.kind == sat::ExplainEntry::Kind::Conflict) {
                detail = e.message;
                break;
            }
        }
        throw UserError("cannot resolve dependencies for '" + name_str + "'" +
                        (detail.empty() ? "" : ": " + detail));
    }

    // Surface stability warnings (🎯T30) to the user before installing.
    for (const auto& w : resolution.warnings)
        SPDLOG_WARN("{}", w);

    InstallResult result;
    result.resolved_version = pkg->version;
    for (const auto* p : resolution.install_order) {
        if (::den::is_installed(config.store, p->name, p->version)) {
            // Already present in the shared Cellar — skip the download/extract.
            SPDLOG_DEBUG("{} {} already installed", p->name, p->version);
        } else {
            install_one(config, *p);
        }
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
