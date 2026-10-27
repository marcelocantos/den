// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "homebrew_provider.h"

#include "../build/relocate.h"
#include "../cask/cask.h"
#include "../core/config.h"
#include "../core/error.h"
#include "../download/archive.h"
#include "../download/fetch.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../index/dep_resolve.h"
#include "../platform/platform.h"
#include "../store/link.h"
#include "../store/store.h"
#include "../trust/trust_model.h"

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

} // namespace

fs::path find_bottle_keg_dir(const fs::path& extract_root, const std::string& name,
                             const std::string& version) {
    std::error_code ec;
    const fs::path base = extract_root / name;
    if (fs::is_directory(base / version, ec)) {
        return base / version;
    }
    fs::path match;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        if (!e.is_directory()) {
            continue;
        }
        const auto d = e.path().filename().string();
        if (d == version || d.starts_with(version + "_")) {
            if (!match.empty()) {
                return {}; // ambiguous
            }
            match = e.path();
        }
    }
    return match;
}

namespace {

bool install_one(const Config& config, const Package& pkg) {
    // Cask / app packages take a completely different path from Cellar bottles.
    // Dispatch before any bottle-tag / GHCR logic so "default" archives and
    // DMG/ZIP/PKG artifacts are handled by the cask installer.
    if (pkg.artifact_type == ArtifactType::App || !pkg.cask_artifacts.empty()) {
#ifndef __APPLE__
        throw UserError("casks are not supported on Linux (macOS only)");
#else
        install_cask(config, pkg);
        return true;
#endif
    }

    if (is_installed(config.store, pkg.name, pkg.version)) {
        SPDLOG_DEBUG("{} {} already installed", pkg.name, pkg.version);
        // A keg poured before den maintained opt links (or by a Homebrew
        // that has since lost the link) still needs <prefix>/opt/<name>.
        ensure_prefix_opt_link(config.store, pkg.name,
                               package_path(config.store, pkg.name, pkg.version));
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

    // 🎯T42/T64: independent trust cross-check BEFORE we download or extract.
    //
    // Source A is the hash Homebrew's API gave us (archive.sha256, baked into
    // the index). Source B is den's replica snapshot, served from a SEPARATE
    // origin (raw.githubusercontent.com). An attacker must compromise BOTH to
    // swap a bottle: they would need a matching SHA256 in Homebrew's API *and*
    // in den's GitHub replica (which is diff-verified against GHCR in CI).
    //
    // We consult the bundled local snapshot here. fetch from the live CDN is
    // intentionally avoided on the hot install path (network cost, offline
    // installs); the replica-verify CI pipeline keeps the bundled snapshot
    // fresh and GHCR-verified.
    {
        MaybeHash homebrew_hash = archive.sha256.empty() ? std::nullopt : MaybeHash(archive.sha256);
        MaybeHash replica_hash;
        try {
            auto replica = load_local_replica(config);
            replica_hash = replica_lookup(replica, pkg.name, pkg.upstream_version(), *best);
        } catch (const std::exception& e) {
            // A corrupt replica snapshot leaves the replica source unreachable
            // (nullopt) rather than aborting — the cross-check then degrades to
            // single-source with a warning.
            SPDLOG_WARN("trust replica unavailable for {} {}: {}", pkg.name, pkg.version, e.what());
        }

        TrustCheckResult trust = cross_check_hashes(homebrew_hash, replica_hash);
        switch (trust.decision) {
        case TrustDecision::Proceed:
            SPDLOG_INFO("trust: {} {} cross-checked against replica — {}", pkg.name, pkg.version,
                        trust.message);
            break;
        case TrustDecision::WarnProceed:
            SPDLOG_WARN("trust: {} {} — {} (degraded trust)", pkg.name, pkg.version, trust.message);
            break;
        case TrustDecision::Refuse:
            throw UserError("refusing to install '" + pkg.name + " " + pkg.version +
                            "': independent trust check failed — " + trust.message);
        }
    }

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
    // tmp_dir stands in for the Cellar (the keg unpacks as
    // <name>/<version>/ and is renamed into $HOMEBREW_CELLAR), so bottle
    // symlinks into $HOMEBREW_PREFIX/etc, /opt, ... are legitimate.
    ExtractOptions xopts;
    xopts.symlink_scope = SymlinkScope::BottlePrefix;
    auto result = extract_archive(archive_path, tmp_dir, xopts);
    SPDLOG_INFO("extracted {} files", result.files.size());

    auto inner = tmp_dir / pkg.name / pkg.version;
    if (!fs::is_directory(inner)) {
        // Revisioned/rebuilt bottles unpack as <name>/<version>_<rev>/ (e.g.
        // libgit2/1.9.7_2/). Taking <name>/ as the keg nested the real keg one
        // level down, so <keg>/lib didn't exist and dependents failed to load.
        inner = find_bottle_keg_dir(tmp_dir, pkg.name, pkg.version);
        if (inner.empty()) {
            inner = tmp_dir / result.root;
        } else if (inner.filename() != pkg.version) {
            // The bottle was built as <version>_<rev> but the index entry
            // has no revision (an index.json written before den tracked
            // revisions). Pouring it under the bare version would break
            // every dependent, whose load commands name the revisioned
            // directory, so refuse and ask for a fresh index.
            const auto bottle_dir = inner.filename().string();
            std::error_code tec;
            fs::remove_all(tmp_dir, tec);
            throw UserError("bottle for " + pkg.name + " unpacks as " + pkg.name + "/" +
                            bottle_dir + " but the index says " + pkg.version +
                            "; the package index is out of date. Run `den update` and retry.");
        }
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

    // Every bottle gets the text pass: Homebrew expands @@HOMEBREW_*@@ in
    // scripts even for :any_skip_relocation (Portable) bottles and only skips
    // dynamic-linkage rewriting for them (automake, gradle, yarn, ...).
    {
        RelocationOptions reloc;
        reloc.dependencies = pkg.dependencies;
        reloc.skip_linkage = archive.relocation == RelocationType::Portable;
        if (!reloc.skip_linkage) {
            std::cout << "==> Relocating " << pkg.name << "\n";
        }
        try {
            relocate_bottle(dest, pkg.name, pkg.version, config.store, reloc);
        } catch (...) {
            // Never leave a keg that can't load: a later install would see it
            // as present and skip re-pouring.
            std::error_code rec;
            fs::remove_all(dest, rec);
            fs::remove_all(tmp_dir, rec);
            throw;
        }
    }

    std::error_code ec;
    fs::remove_all(tmp_dir, ec);

    // Stamp the keg den just poured. The early return above skips kegs that
    // were already in the shared Cellar, so a Homebrew install is never
    // marked as ours and `den cleanup` will not delete it.
    mark_keg_owned(dest, pkg.name, pkg.version);

    // Text files relocated to <prefix>/opt/<name> (e.g. node shebangs) need
    // the shared opt link; create it, or move it along from an older den keg.
    ensure_prefix_opt_link(config.store, pkg.name, dest);

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
        const bool is_cask = p->artifact_type == ArtifactType::App || !p->cask_artifacts.empty();
        if (is_cask) {
            install_one(config, *p);
        } else if (::den::is_installed(config.store, p->name, p->version)) {
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

void HomebrewProvider::uninstall(const Config& config, std::string_view name) {
    // Casks den installed live under den's caskroom (not the shared Cellar).
    // Remove their apps/binaries via receipt-scoped uninstall.
    std::string token(name);
    if (is_cask_installed(config, token)) {
        uninstall_cask(config, token);
        return;
    }
    // The shared Cellar is retained for fast reinstall. The CLI drops the
    // package from the active env manifest and rematerialises; the keg stays
    // on disk until `den cleanup`, which removes it only if den poured it and
    // no environment still references that version. Uninstall is a no-op here.
}

std::vector<InstalledPackage> HomebrewProvider::list_installed(const Config& config) const {
    auto pkgs = ::den::list_installed(config.store);
    for (auto& c : list_den_casks(config)) {
        pkgs.push_back(std::move(c));
    }
    return pkgs;
}

fs::path HomebrewProvider::package_root(const Config& config, std::string_view name,
                                        std::string_view version) const {
    // Casks live under den's caskroom, not the shared Cellar.
    std::string token(name);
    if (token.starts_with("cask/")) {
        token = token.substr(5);
    }
    auto cask_path = cask_version_path(config, token, std::string(version));
    if (fs::exists(cask_path)) {
        return cask_path;
    }
    // Cellar layout: <store>/<name>/<version>/
    return config.store / std::string(name) / std::string(version);
}

std::vector<fs::path> HomebrewProvider::binary_paths(const Config& /*config*/,
                                                     const InstalledPackage& pkg) const {
    // Cask stages do not use the Cellar bin/lib layout. Binaries are linked
    // at install time into the env bin; materialise should not re-link the
    // whole stage tree. Returning no subdirs makes materialise a no-op for
    // the package body while still recording the linked version.
    if (pkg.path.string().find("/caskroom/") != std::string::npos) {
        return {};
    }
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

std::optional<fs::path> prefetch_bottle(const Config& config, const Package& pkg) {
    if (pkg.artifact_type == ArtifactType::App || !pkg.cask_artifacts.empty())
        return std::nullopt;
    std::vector<std::string> tags;
    for (const auto& [tag, _] : pkg.archives)
        tags.push_back(tag);
    auto best = best_archive_tag(config.arch, config.macos_version, tags);
    if (!best)
        return std::nullopt;
    const auto& archive = pkg.archives.at(*best);
    auto cache_dir = config.cache / "archives";
    fs::create_directories(cache_dir);
    if (archive.url.find("ghcr.io") != std::string::npos || archive.url.empty())
        return fetch_ghcr_blob(ghcr_repo_path(pkg.name), archive.sha256, cache_dir);
    return fetch_archive(archive.url, archive.sha256, cache_dir);
}

} // namespace den
