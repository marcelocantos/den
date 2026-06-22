// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "store.h"

#include "../env/manifest.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <map>
#include <set>

namespace den {

fs::path package_path(const fs::path& store, const std::string& name, const std::string& version) {
    return store / name / version;
}

bool is_installed(const fs::path& store, const std::string& name, const std::string& version) {
    auto p = package_path(store, name, version);
    return fs::exists(p) && fs::is_directory(p);
}

std::vector<InstalledPackage> list_installed(const fs::path& store) {
    std::vector<InstalledPackage> result;

    if (!fs::exists(store) || !fs::is_directory(store)) {
        return result;
    }

    std::error_code ec;
    for (const auto& name_entry : fs::directory_iterator(store, ec)) {
        if (!name_entry.is_directory()) {
            continue;
        }
        auto name = name_entry.path().filename().string();
        for (const auto& ver_entry : fs::directory_iterator(name_entry.path(), ec)) {
            if (!ver_entry.is_directory()) {
                continue;
            }
            auto version = ver_entry.path().filename().string();
            result.push_back(InstalledPackage{
                .name = name,
                .version = version,
                .path = ver_entry.path(),
            });
        }
    }

    if (ec) {
        SPDLOG_WARN("error scanning store: {}", ec.message());
    }

    return result;
}

std::optional<InstalledPackage> which_package(const fs::path& store, const fs::path& file) {
    std::error_code ec;

    // Resolve symlinks to get the real path in the store.
    auto real = fs::canonical(file, ec);
    if (ec) {
        return std::nullopt;
    }

    // Check if the real path is under the store directory.
    auto store_canonical = fs::canonical(store, ec);
    if (ec) {
        return std::nullopt;
    }

    auto rel = real.lexically_relative(store_canonical);
    if (rel.empty() || *rel.begin() == "..") {
        return std::nullopt;
    }

    // Store layout: <name>/<version>/...
    // Extract the first two path components.
    auto it = rel.begin();
    if (it == rel.end())
        return std::nullopt;
    std::string name = it->string();
    ++it;
    if (it == rel.end())
        return std::nullopt;
    std::string version = it->string();

    auto pkg_path = store_canonical / name / version;
    if (!fs::is_directory(pkg_path, ec)) {
        return std::nullopt;
    }

    return InstalledPackage{
        .name = name,
        .version = version,
        .path = pkg_path,
    };
}

uint64_t keg_disk_usage(const fs::path& keg) {
    std::error_code ec;
    if (!fs::exists(keg, ec) || !fs::is_directory(keg, ec)) {
        return 0;
    }

    uint64_t total = 0;
    // Do not follow symlinks: count the link entry itself, never its target —
    // a keg may symlink into shared resources we must not double-count.
    for (auto it = fs::recursive_directory_iterator(
             keg, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            SPDLOG_DEBUG("keg_disk_usage: iteration error under {}: {}", keg.string(),
                         ec.message());
            ec.clear();
            continue;
        }
        std::error_code stat_ec;
        if (it->is_symlink(stat_ec) || stat_ec) {
            continue;
        }
        if (it->is_regular_file(stat_ec) && !stat_ec) {
            auto sz = it->file_size(stat_ec);
            if (!stat_ec) {
                total += sz;
            }
        }
    }
    return total;
}

std::vector<KegInfo> inspect_cellar(const fs::path& store, const fs::path& den_home) {
    // Build a map of "name@version" -> sorted set of referencing env paths by
    // resolving every environment manifest.
    std::map<std::string, std::set<std::string>> refs;
    for (const auto& env_path : list_all(den_home)) {
        auto resolved = resolve(den_home, env_path);
        for (const auto& [name, version] : resolved) {
            refs[name + "@" + version].insert(env_path);
        }
    }

    std::vector<KegInfo> result;
    for (const auto& pkg : list_installed(store)) {
        KegInfo info;
        info.name = pkg.name;
        info.version = pkg.version;
        info.path = pkg.path;
        info.disk_bytes = keg_disk_usage(pkg.path);
        auto it = refs.find(pkg.name + "@" + pkg.version);
        if (it != refs.end()) {
            info.env_refs.assign(it->second.begin(), it->second.end());
        }
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(), [](const KegInfo& a, const KegInfo& b) {
        if (a.name != b.name)
            return a.name < b.name;
        return a.version < b.version;
    });

    return result;
}

} // namespace den
