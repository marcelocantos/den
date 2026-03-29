// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "link.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <array>
#include <fstream>
#include <regex>

namespace den {

namespace {

constexpr std::array<const char*, 5> kLinkDirs = {
    "bin", "sbin", "lib", "include", "share",
};

/// Recursively create symlinks from src_dir into dst_dir.
/// For files: creates a symlink in dst_dir pointing to the file in src_dir.
/// For directories: creates the directory in dst_dir and recurses.
uint32_t link_tree(const fs::path& src_dir, const fs::path& dst_dir) {
    uint32_t count = 0;

    if (!fs::exists(src_dir) || !fs::is_directory(src_dir)) {
        return count;
    }

    fs::create_directories(dst_dir);

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(src_dir, ec)) {
        auto dest = dst_dir / entry.path().filename();

        if (entry.is_directory()) {
            count += link_tree(entry.path(), dest);
        } else {
            if (fs::exists(dest, ec)) {
                SPDLOG_WARN("skipping existing file: {}", dest.string());
                continue;
            }
            fs::create_symlink(entry.path(), dest, ec);
            if (ec) {
                SPDLOG_WARN("failed to symlink {} -> {}: {}", entry.path().string(), dest.string(),
                            ec.message());
            } else {
                ++count;
            }
        }
    }

    return count;
}

/// Remove symlinks in dst_dir that point into src_dir. Clean up empty
/// directories left behind.
uint32_t unlink_tree(const fs::path& src_dir, const fs::path& dst_dir) {
    uint32_t count = 0;

    if (!fs::exists(dst_dir) || !fs::is_directory(dst_dir)) {
        return count;
    }

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dst_dir, ec)) {
        auto path = entry.path();

        if (entry.is_directory()) {
            count += unlink_tree(src_dir / path.filename(), path);
            // Remove directory if now empty.
            if (fs::is_empty(path, ec) && !ec) {
                fs::remove(path, ec);
            }
        } else if (entry.is_symlink()) {
            auto target = fs::read_symlink(path, ec);
            if (!ec) {
                // Check if this symlink points into src_dir.
                auto target_canon = fs::weakly_canonical(target, ec);
                auto src_canon = fs::weakly_canonical(src_dir, ec);
                if (!ec && target_canon.string().starts_with(src_canon.string())) {
                    fs::remove(path, ec);
                    if (!ec) {
                        ++count;
                    }
                }
            }
        }
    }

    return count;
}

} // namespace

bool is_valid_package_name(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    // Reject path traversal.
    if (name.find("..") != std::string::npos) {
        return false;
    }
    static const std::regex pattern(R"([a-zA-Z0-9_][a-zA-Z0-9_.+\-]*(@[a-zA-Z0-9]+)?)");
    return std::regex_match(name, pattern);
}

uint32_t link_package(const fs::path& pkg_path, const fs::path& env_dir, const std::string& name) {
    if (!is_valid_package_name(name)) {
        throw UserError("invalid package name: " + name);
    }

    if (!fs::exists(pkg_path) || !fs::is_directory(pkg_path)) {
        throw UserError("package path does not exist: " + pkg_path.string());
    }

    uint32_t count = 0;

    for (const auto& dir : kLinkDirs) {
        auto src = pkg_path / dir;
        auto dst = env_dir / dir;
        count += link_tree(src, dst);
    }

    // Create opt/<name> symlink.
    auto opt_dir = env_dir / "opt";
    fs::create_directories(opt_dir);
    auto opt_link = opt_dir / name;
    std::error_code ec;
    if (fs::exists(opt_link, ec)) {
        fs::remove(opt_link, ec);
    }
    fs::create_symlink(pkg_path, opt_link, ec);
    if (ec) {
        SPDLOG_WARN("failed to create opt symlink: {}", ec.message());
    } else {
        ++count;
    }

    SPDLOG_INFO("linked {} ({} symlinks)", name, count);
    return count;
}

uint32_t unlink_package(const fs::path& pkg_path, const fs::path& env_dir,
                        const std::string& name) {
    if (!is_valid_package_name(name)) {
        throw UserError("invalid package name: " + name);
    }

    uint32_t count = 0;

    for (const auto& dir : kLinkDirs) {
        auto src = pkg_path / dir;
        auto dst = env_dir / dir;
        count += unlink_tree(src, dst);
    }

    // Remove opt/<name> symlink.
    auto opt_link = env_dir / "opt" / name;
    std::error_code ec;
    if (fs::is_symlink(opt_link, ec)) {
        fs::remove(opt_link, ec);
        if (!ec) {
            ++count;
        }
    }

    // Remove linked version record.
    auto linked_file = env_dir / ".den" / "linked" / name;
    if (fs::exists(linked_file, ec)) {
        fs::remove(linked_file, ec);
    }

    SPDLOG_INFO("unlinked {} ({} symlinks removed)", name, count);
    return count;
}

void record_linked_version(const fs::path& env_dir, const std::string& name,
                           const std::string& version) {
    if (!is_valid_package_name(name)) {
        throw UserError("invalid package name: " + name);
    }

    auto linked_dir = env_dir / ".den" / "linked";
    fs::create_directories(linked_dir);

    auto path = linked_dir / name;
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw InternalError("failed to write linked version: " + path.string());
    }
    out << version << '\n';
}

std::optional<std::string> linked_version(const fs::path& env_dir, const std::string& name) {
    auto path = env_dir / ".den" / "linked" / name;
    std::ifstream in(path);
    if (!in) {
        return std::nullopt;
    }
    std::string version;
    std::getline(in, version);
    if (version.empty()) {
        return std::nullopt;
    }
    return version;
}

} // namespace den
