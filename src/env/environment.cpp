// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "environment.h"

#include "../core/error.h"
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

} // namespace

uint32_t materialise(const fs::path& den_home, const fs::path& store, const std::string& env_path,
                     const PackageIndex* idx) {
    auto resolved = resolve(den_home, env_path);
    auto manifest = read_manifest(den_home, env_path);
    auto dir = env_dir(den_home, env_path);

    fs::create_directories(dir);

    // Detect versioned formula families (e.g. python@3.11 and python@3.12).
    // For each family with multiple members, only link the one that's an
    // explicit install (not auto-dep). If multiple are explicit, link the
    // one with the highest version string.
    std::set<std::string> skip_versioned;
    {
        std::map<std::string, std::vector<std::string>> families;
        for (const auto& [name, _] : resolved) {
            if (name.find('@') != std::string::npos) {
                families[base_name(name)].push_back(name);
            }
        }
        for (const auto& [base, members] : families) {
            if (members.size() <= 1)
                continue;

            // Prefer explicit installs over auto-deps. Versioned families
            // are a Homebrew concept (python@3.11 vs python@3.12); look up
            // explicitness in the Homebrew auto_deps bucket.
            const auto& homebrew_auto = manifest.auto_deps["homebrew"];
            std::vector<std::string> explicit_members;
            for (const auto& m : members) {
                if (!homebrew_auto.count(m)) {
                    explicit_members.push_back(m);
                }
            }

            // From the candidates, pick the highest version string.
            const auto& candidates = explicit_members.empty() ? members : explicit_members;
            auto winner = *std::max_element(candidates.begin(), candidates.end());

            for (const auto& m : members) {
                if (m != winner) {
                    skip_versioned.insert(m);
                    SPDLOG_INFO("{}  skipped (versioned family '{}', linking {} instead)", m, base,
                                winner);
                }
            }
        }
    }

    uint32_t total = 0;

    for (const auto& [name, version] : resolved) {
        auto pkg_path = package_path(store, name, version);
        if (!fs::exists(pkg_path)) {
            SPDLOG_WARN("package {}@{} not in store, skipping", name, version);
            continue;
        }

        // Skip versioned family members that lost the dedup election.
        // They stay in the store but don't get linked into the environment.
        if (skip_versioned.count(name)) {
            // Still create opt/ link.
            auto opt_dir = dir / "opt";
            fs::create_directories(opt_dir);
            auto opt_link = opt_dir / name;
            std::error_code ec;
            if (fs::is_symlink(opt_link, ec))
                fs::remove(opt_link, ec);
            fs::create_symlink(pkg_path, opt_link, ec);
            if (!ec)
                ++total;
            record_linked_version(dir, name, version);
            continue;
        }

        // Skip keg-only packages (they shadow system tools).
        // Still create opt/<name> so dependencies can find them.
        if (idx) {
            const auto* pkg_info = idx->find(name);
            if (pkg_info && pkg_info->keg_only) {
                auto opt_dir = dir / "opt";
                fs::create_directories(opt_dir);
                auto opt_link = opt_dir / name;
                std::error_code ec;
                if (fs::is_symlink(opt_link, ec)) {
                    fs::remove(opt_link, ec);
                }
                fs::create_symlink(pkg_path, opt_link, ec);
                if (!ec) {
                    ++total;
                    SPDLOG_INFO("{}@{} is keg-only, linked opt/ only", name, version);
                }
                record_linked_version(dir, name, version);
                continue;
            }
        }

        // Check if already linked at the correct version.
        auto current = linked_version(dir, name);
        if (current && *current == version) {
            SPDLOG_INFO("{}@{} already linked, skipping", name, version);
            continue;
        }

        // Unlink previous version if present.
        if (current) {
            auto old_pkg = package_path(store, name, *current);
            unlink_package(old_pkg, dir, name);
        }

        total += link_package(pkg_path, dir, name);
        record_linked_version(dir, name, version);
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
