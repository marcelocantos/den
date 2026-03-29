// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "environment.h"

#include "../core/error.h"
#include "../store/link.h"
#include "../store/store.h"
#include "manifest.h"

#include <spdlog/spdlog.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace den {

fs::path env_dir(const fs::path& den_home, const std::string& env_path) {
    return den_home / "envs" / env_slug(env_path);
}

uint32_t materialise(const fs::path& den_home, const fs::path& store,
                     const std::string& env_path) {
    auto resolved = resolve(den_home, env_path);
    auto dir = env_dir(den_home, env_path);

    fs::create_directories(dir);

    uint32_t total = 0;

    for (const auto& [name, version] : resolved) {
        auto pkg = package_path(store, name, version);
        if (!fs::exists(pkg)) {
            SPDLOG_WARN("package {}@{} not in store, skipping", name, version);
            continue;
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

        total += link_package(pkg, dir, name);
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
        throw InternalError("failed to rename active_env: " +
                            std::string(std::strerror(errno)));
    }

    SPDLOG_INFO("active environment set to '{}'", env_path);
}

} // namespace den
