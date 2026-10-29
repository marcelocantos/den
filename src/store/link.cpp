// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "link.h"

#include "../build/relocate.h"
#include "../core/error.h"
#include "store.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <regex>

namespace den {

namespace {

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
    // Versioned formulae like python@3.12 use dots after @.
    static const std::regex pattern(R"([a-zA-Z0-9_][a-zA-Z0-9_.+\-]*(@[a-zA-Z0-9][a-zA-Z0-9.]*)?)");
    return std::regex_match(name, pattern);
}

uint32_t link_package(const fs::path& pkg_path, const fs::path& env_dir, const std::string& name,
                      const std::vector<std::string>& subdirs) {
    if (!is_valid_package_name(name)) {
        throw UserError("invalid package name: " + name);
    }

    if (!fs::exists(pkg_path) || !fs::is_directory(pkg_path)) {
        throw UserError("package path does not exist: " + pkg_path.string());
    }

    uint32_t count = 0;

    for (const auto& dir : subdirs) {
        auto src = pkg_path / dir;
        auto dst = env_dir / dir;
        count += link_tree(src, dst);
    }

    // Create opt/<name> symlink inside the env.
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

    // <prefix>/opt/<name> is Homebrew's shared, single-version link.
    // Relocated text files (node shebangs, wrapper scripts) reference it, so
    // den creates it when missing or dangling, but never repoints a link that
    // resolves to a keg Homebrew owns (see ensure_prefix_opt_link). Bottle Mach-O
    // load commands rely on it only where den owns it; elsewhere (and always
    // in a prefix shared with Homebrew) they are pinned to the exact
    // dependency keg (bind_opt_reference). Only kegs that sit in a Homebrew-style
    // <prefix>/Cellar/<name>/<version> layout get a prefix link here; other
    // providers' package roots are left alone.
    {
        auto cellar = pkg_path.parent_path().parent_path(); // .../Cellar
        if (!cellar.parent_path().empty() && cellar.filename() == "Cellar" &&
            pkg_path.parent_path().filename() == name) {
            auto action = ensure_prefix_opt_link(cellar, name, pkg_path);
            if (action == OptLinkAction::Created || action == OptLinkAction::Repointed) {
                ++count;
            }
        }
    }

    SPDLOG_INFO("linked {} ({} symlinks)", name, count);
    return count;
}

uint32_t unlink_package(const fs::path& pkg_path, const fs::path& env_dir, const std::string& name,
                        const std::vector<std::string>& subdirs) {
    if (!is_valid_package_name(name)) {
        throw UserError("invalid package name: " + name);
    }

    uint32_t count = 0;

    for (const auto& dir : subdirs) {
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

    // <prefix>/opt/<name> tracks the keg, not any one environment: other
    // environments and other kegs' shebangs still use it after this env
    // drops the package, and the keg itself stays in the store. It is
    // removed by cleanup_kegs when the keg it points at is deleted.

    // Remove linked version record.
    auto linked_file = env_dir / ".den" / "linked" / name;
    if (fs::exists(linked_file, ec)) {
        fs::remove(linked_file, ec);
    }

    SPDLOG_INFO("unlinked {} ({} symlinks removed)", name, count);
    return count;
}

namespace {

/// Lexically resolve a symlink's target (relative targets are taken from the
/// link's directory). Empty on error.
fs::path lexical_link_target(const fs::path& link) {
    std::error_code ec;
    auto target = fs::read_symlink(link, ec);
    if (ec) {
        return {};
    }
    if (target.is_relative()) {
        target = link.parent_path() / target;
    }
    return target.lexically_normal();
}

bool same_path(const fs::path& a, const fs::path& b) {
    std::error_code ec1, ec2;
    if (fs::exists(a, ec1) && fs::exists(b, ec2)) {
        std::error_code ec;
        if (fs::equivalent(a, b, ec) && !ec) {
            return true;
        }
    }
    return a.lexically_normal() == b.lexically_normal();
}

} // namespace

OptLinkAction ensure_prefix_opt_link(const fs::path& store, const std::string& name,
                                     const fs::path& keg) {
    if (!is_valid_package_name(name) || name.find('/') != std::string::npos) {
        return OptLinkAction::Kept;
    }
    const auto prefix = store.parent_path();
    if (prefix.empty() || store.empty()) {
        return OptLinkAction::Kept;
    }
    std::error_code ec;
    if (!fs::is_directory(keg, ec) || ec) {
        return OptLinkAction::Kept;
    }

    const auto opt_dir = prefix / "opt";
    const auto link = opt_dir / name;

    // Homebrew writes opt links relative (../Cellar/<name>/<version>) so the
    // prefix can move; do the same when the keg sits under the prefix.
    auto make_target = [&]() {
        auto rel = keg.lexically_normal().lexically_relative(opt_dir.lexically_normal());
        if (!rel.empty() && rel.native().rfind("..", 0) == 0 &&
            keg.lexically_normal().native().rfind(prefix.lexically_normal().native(), 0) == 0) {
            return rel;
        }
        return keg;
    };

    auto create = [&]() {
        std::error_code cec;
        fs::create_directories(opt_dir, cec);
        fs::create_symlink(make_target(), link, cec);
        if (cec) {
            SPDLOG_WARN("failed to create prefix opt symlink {}: {}", link.string(), cec.message());
            return false;
        }
        return true;
    };

    const bool is_link = fs::is_symlink(link, ec);
    if (!is_link) {
        if (fs::exists(link, ec)) {
            // A real file or directory: not ours to replace.
            SPDLOG_DEBUG("leaving non-symlink {}", link.string());
            return OptLinkAction::Kept;
        }
        return create() ? OptLinkAction::Created : OptLinkAction::Failed;
    }

    if (!fs::exists(link, ec)) {
        // Dangling: whatever it pointed at is gone.
        fs::remove(link, ec);
        return create() ? OptLinkAction::Created : OptLinkAction::Failed;
    }

    const auto current = lexical_link_target(link);
    if (same_path(current, keg)) {
        return OptLinkAction::Unchanged;
    }

    // Repoint only den's own, older keg of this formula (an upgrade). A link
    // to a keg den didn't pour belongs to Homebrew: repointing it would swap
    // the dependency under Homebrew-installed packages (PR #64).
    std::error_code cec;
    const auto current_keg = fs::canonical(link, cec);
    if (cec || current_keg.parent_path().filename() != name || !keg_owned_by_den(current_keg)) {
        SPDLOG_DEBUG("leaving prefix opt link {} (not den-owned)", link.string());
        return OptLinkAction::Kept;
    }
    if (compare_keg_versions(keg.filename().string(), current_keg.filename().string()) <= 0) {
        return OptLinkAction::Kept;
    }
    // Swap atomically so the link never disappears under a running process.
    const auto tmp = opt_dir / ("." + name + ".den-tmp");
    fs::remove(tmp, ec);
    fs::create_symlink(make_target(), tmp, cec);
    if (!cec) {
        fs::rename(tmp, link, cec);
    }
    if (cec) {
        fs::remove(tmp, ec);
        SPDLOG_WARN("failed to repoint prefix opt symlink {}: {}", link.string(), cec.message());
        return OptLinkAction::Failed;
    }
    return OptLinkAction::Repointed;
}

bool remove_prefix_opt_link_to(const fs::path& store, const std::string& name,
                               const fs::path& keg) {
    const auto prefix = store.parent_path();
    if (prefix.empty() || name.find('/') != std::string::npos) {
        return false;
    }
    const auto link = prefix / "opt" / name;
    std::error_code ec;
    if (!fs::is_symlink(link, ec)) {
        return false;
    }
    if (lexical_link_target(link) != keg.lexically_normal()) {
        return false;
    }
    fs::remove(link, ec);
    return !ec;
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
