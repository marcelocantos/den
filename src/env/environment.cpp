// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "environment.h"

#include "../core/config.h"
#include "../core/error.h"
#include "../provider/package_provider.h"
#include "../provider/registry.h"
#include "../store/link.h"
#include "../store/store.h"
#include "manifest.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <vector>

namespace den {

fs::path env_dir(const fs::path& den_home, const std::string& env_path) {
    return den_home / "envs" / env_slug(env_path);
}

namespace {

/// Extract the base name from a versioned formula name.
/// "python@3.12" -> "python", "ffmpeg" -> "ffmpeg"
std::string base_name(const std::string& name) {
    auto at = name.find('@');
    return at != std::string::npos ? name.substr(0, at) : name;
}

/// Map a provider's `binary_paths()` output to the subdir names the link
/// layer uses. Each returned path's filename becomes the env subdir.
std::vector<std::string> subdirs_from(const std::vector<fs::path>& paths) {
    std::vector<std::string> result;
    result.reserve(paths.size());
    for (const auto& p : paths) {
        result.push_back(p.filename().string());
    }
    return result;
}

/// Versioned-family dedup is a Homebrew concept: when both python@3.11 and
/// python@3.12 appear in the env, only one of them gets linked (preferring
/// explicit installs, breaking ties by highest version string). Returns
/// the set of names that should be skipped for full linking and get only
/// an opt/ symlink.
std::set<std::string>
compute_versioned_skips(const std::map<std::string, std::string>& homebrew_resolved,
                        const Manifest& manifest) {
    std::set<std::string> skip;

    std::map<std::string, std::vector<std::string>> families;
    for (const auto& [name, _] : homebrew_resolved) {
        if (name.find('@') != std::string::npos) {
            families[base_name(name)].push_back(name);
        }
    }

    const auto& homebrew_auto = manifest.auto_deps.count("homebrew")
                                    ? manifest.auto_deps.at("homebrew")
                                    : std::set<std::string>{};

    for (const auto& [base, members] : families) {
        if (members.size() <= 1) {
            continue;
        }

        std::vector<std::string> explicit_members;
        for (const auto& m : members) {
            if (!homebrew_auto.count(m)) {
                explicit_members.push_back(m);
            }
        }

        const auto& candidates = explicit_members.empty() ? members : explicit_members;
        auto winner = *std::max_element(candidates.begin(), candidates.end());

        for (const auto& m : members) {
            if (m != winner) {
                skip.insert(m);
                SPDLOG_INFO("{}  skipped (versioned family '{}', linking {} instead)", m, base,
                            winner);
            }
        }
    }
    return skip;
}

/// Create just the `opt/<name>` symlink for a package that's resolved but
/// not fully linked (versioned-family loser or keg-only). Returns 1 on
/// success, 0 otherwise.
uint32_t link_opt_only(const fs::path& pkg_path, const fs::path& env_dir, const std::string& name,
                       const std::string& version) {
    auto opt_dir = env_dir / "opt";
    fs::create_directories(opt_dir);
    auto opt_link = opt_dir / name;

    std::error_code ec;
    if (fs::is_symlink(opt_link, ec)) {
        fs::remove(opt_link, ec);
    }
    fs::create_symlink(pkg_path, opt_link, ec);
    record_linked_version(env_dir, name, version);
    return ec ? 0 : 1;
}

} // namespace

uint32_t materialise(const fs::path& den_home, const fs::path& store, const std::string& env_path,
                     const PackageIndex* idx) {
    // The materialiser asks each provider for its package layout (root +
    // subdirs to surface) and dispatches the link layer accordingly. The
    // result is provider-agnostic — a Homebrew keg, a pip-installed
    // Python package, and a Cargo crate all flow through the same loop.
    auto per_provider = resolve_per_provider(den_home, env_path);
    auto manifest = read_manifest(den_home, env_path);
    auto dir = env_dir(den_home, env_path);
    fs::create_directories(dir);

    auto registry = make_default_registry(idx);

    // The link layer is path-agnostic — it just needs den_home/store to
    // be available on the synthetic Config we pass to provider methods.
    Config cfg;
    cfg.den_home = den_home;
    cfg.store = store;

    // Versioned-family dedup (Homebrew-only).
    std::set<std::string> skip_versioned;
    auto homebrew_it = per_provider.find("homebrew");
    if (homebrew_it != per_provider.end()) {
        skip_versioned = compute_versioned_skips(homebrew_it->second, manifest);
    }

    uint32_t total = 0;

    for (const auto& [provider_name, pkgs] : per_provider) {
        auto* provider = registry.find(provider_name);
        if (!provider) {
            SPDLOG_WARN("env has packages under provider '{}' but no such provider is "
                        "registered — skipping",
                        provider_name);
            continue;
        }

        for (const auto& [name, version] : pkgs) {
            auto pkg_path = provider->package_root(cfg, name, version);
            if (!fs::exists(pkg_path)) {
                SPDLOG_WARN("package {}@{} not in store, skipping", name, version);
                continue;
            }

            InstalledPackage installed{name, version, pkg_path};
            auto exposed = provider->binary_paths(cfg, installed);
            auto subdirs = subdirs_from(exposed);

            // Versioned-family loser: only opt/ link.
            if (skip_versioned.count(name)) {
                total += link_opt_only(pkg_path, dir, name, version);
                continue;
            }

            // Keg-only (Homebrew, index-driven): only opt/ link.
            if (provider_name == "homebrew" && idx) {
                if (const auto* pkg_info = idx->find(name); pkg_info && pkg_info->keg_only) {
                    auto added = link_opt_only(pkg_path, dir, name, version);
                    if (added) {
                        SPDLOG_INFO("{}@{} is keg-only, linked opt/ only", name, version);
                    }
                    total += added;
                    continue;
                }
            }

            // Already linked at the requested version → nothing to do.
            auto current = linked_version(dir, name);
            if (current && *current == version) {
                SPDLOG_INFO("{}@{} already linked, skipping", name, version);
                continue;
            }

            // Replace older linked version, if any.
            if (current) {
                auto old_pkg = provider->package_root(cfg, name, *current);
                unlink_package(old_pkg, dir, name, subdirs);
            }

            total += link_package(pkg_path, dir, name, subdirs);
            record_linked_version(dir, name, version);
        }
    }

    // Stale-link cleanup: packages that were previously linked but are no
    // longer in the manifest (e.g. after `den uninstall`) must be removed.
    // Build the set of names currently in the manifest, then scan the
    // linked-version records for any name that's no longer present.
    std::set<std::string> manifest_names;
    for (const auto& [pn, pkgs] : per_provider) {
        for (const auto& [name, _] : pkgs) {
            manifest_names.insert(name);
        }
    }

    auto linked_dir = dir / ".den" / "linked";
    std::error_code scan_ec;
    if (fs::is_directory(linked_dir, scan_ec)) {
        for (const auto& entry : fs::directory_iterator(linked_dir, scan_ec)) {
            auto pkg_name = entry.path().filename().string();
            if (manifest_names.count(pkg_name)) {
                continue; // still in manifest, nothing to do
            }
            auto stale_version = linked_version(dir, pkg_name);
            if (!stale_version) {
                continue;
            }
            // Find which registered provider owns this package by checking
            // which one produced an on-disk path for the recorded version.
            // In practice this is almost always homebrew; we try all
            // registered providers so the logic is provider-agnostic.
            bool unlinked = false;
            for (auto* prov : registry.all()) {
                if (!prov) {
                    continue;
                }
                auto pkg_path = prov->package_root(cfg, pkg_name, *stale_version);
                if (!fs::exists(pkg_path)) {
                    continue;
                }
                InstalledPackage stale{pkg_name, *stale_version, pkg_path};
                auto exposed = prov->binary_paths(cfg, stale);
                auto subdirs = subdirs_from(exposed);
                unlink_package(pkg_path, dir, pkg_name, subdirs);
                unlinked = true;
                SPDLOG_INFO("unlinked stale package '{}' ({})", pkg_name, *stale_version);
                break;
            }
            if (!unlinked) {
                // Package is no longer on disk; the keg is gone so we can't
                // call unlink_package (it would enumerate the keg's contents
                // to know what to remove).  Instead, scan the env's subdirs
                // for symlinks whose target sits under the Cellar/<pkg_name>/
                // prefix and remove those — they're now-dangling pointers
                // into the deleted keg.
                auto pkg_prefix = (store / pkg_name).string();
                for (const auto& subdir : {"bin", "opt", "lib", "include", "share"}) {
                    auto sub = dir / subdir;
                    std::error_code dir_ec;
                    if (!fs::is_directory(sub, dir_ec)) {
                        continue;
                    }
                    for (const auto& link_entry : fs::directory_iterator(sub, dir_ec)) {
                        std::error_code sym_ec;
                        if (!fs::is_symlink(link_entry, sym_ec)) {
                            continue;
                        }
                        auto target = fs::read_symlink(link_entry, sym_ec);
                        if (sym_ec) {
                            continue;
                        }
                        if (target.string().starts_with(pkg_prefix)) {
                            std::error_code rm_link_ec;
                            fs::remove(link_entry.path(), rm_link_ec);
                            SPDLOG_INFO("removed dangling symlink {} -> {}",
                                        link_entry.path().string(), target.string());
                        }
                    }
                }
                std::error_code rm_ec;
                fs::remove(entry.path(), rm_ec);
                SPDLOG_INFO("cleared stale linked-version record for '{}' (store entry gone)",
                            pkg_name);
            }
        }
    }

    SPDLOG_INFO("materialised env '{}' ({} symlinks)", env_path, total);
    return total;
}

std::string active_env_path(const fs::path& den_home) {
    auto path = den_home / "active_env";
    std::ifstream in(path);
    if (!in) {
        return "/";
    }
    std::string env_path;
    std::getline(in, env_path);
    if (env_path.empty()) {
        return "/";
    }
    return env_path;
}

void set_active_env(const fs::path& den_home, const std::string& env_path) {
    auto path = den_home / "active_env";

    // Atomic write.
    auto tmp_path = path.string() + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            throw InternalError("failed to write active_env: " + tmp_path);
        }
        out << env_path << '\n';
        out.flush();
        if (!out) {
            throw InternalError("failed to flush active_env: " + tmp_path);
        }
    }

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        throw InternalError("failed to rename active_env: " + std::string(std::strerror(errno)));
    }

    SPDLOG_INFO("active environment set to '{}'", env_path);
}

} // namespace den
