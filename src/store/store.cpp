// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "store.h"

#include <spdlog/spdlog.h>

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
    if (it == rel.end()) return std::nullopt;
    std::string name = it->string();
    ++it;
    if (it == rel.end()) return std::nullopt;
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

} // namespace den
