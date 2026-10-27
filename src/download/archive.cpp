// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "archive.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <archive.h>
#include <archive_entry.h>

#include <deque>
#include <memory>
#include <utility>
#include <vector>

namespace den {

namespace {

/// Check whether a path component is ".." or an absolute path.
bool is_path_unsafe(const std::string& entry_path) {
    if (entry_path.empty()) {
        return true;
    }

    // Reject absolute paths.
    if (entry_path[0] == '/') {
        return true;
    }

    // Reject path traversal components.
    fs::path p(entry_path);
    for (const auto& component : p) {
        if (component == "..") {
            return true;
        }
    }

    return false;
}

/// True when `p` exists as a symlink (without following it).
bool is_symlink_nofollow(const fs::path& p) {
    std::error_code ec;
    return fs::symlink_status(p, ec).type() == fs::file_type::symlink;
}

/// Reject `rel` (already checked by is_path_unsafe) if any of its components
/// under `root` is an existing symlink. With `include_last` false the final
/// component is not checked (callers decide separately what to do with it).
void reject_symlink_traversal(const fs::path& root, const fs::path& rel, bool include_last,
                              const std::string& what) {
    std::vector<fs::path> comps(rel.begin(), rel.end());
    while (!comps.empty() && comps.back().empty()) {
        comps.pop_back(); // trailing slash on directory entries
    }
    const size_t n = include_last ? comps.size() : (comps.empty() ? 0 : comps.size() - 1);
    fs::path cur = root;
    for (size_t i = 0; i < n; ++i) {
        if (comps[i].empty() || comps[i] == ".") {
            continue;
        }
        cur /= comps[i];
        if (is_symlink_nofollow(cur)) {
            throw ArchiveError("archive " + what + " passes through a symlink: " + rel.string());
        }
    }
}

bool is_bottle_prefix_dir(const std::string& name) {
    for (const char* d : kBottlePrefixDirs) {
        if (name == d) {
            return true;
        }
    }
    return false;
}

/// Resolve the symlink at dest/`link_rel` to target `target` the way the
/// filesystem will, following any symlinks already present under dest.
///
/// The result is a list of components relative to dest. It may start with a
/// single ".." meaning "the directory containing dest" (the virtual
/// $HOMEBREW_PREFIX for bottles). Returns false when the target is absolute,
/// climbs more than one level above dest, or loops.
bool resolve_link_target(const fs::path& dest, const fs::path& link_rel, const std::string& target,
                         std::vector<std::string>& out) {
    if (target.empty() || target[0] == '/') {
        return false;
    }
    std::vector<std::string> cur;
    for (const auto& c : link_rel.parent_path()) {
        if (!c.empty() && c != ".") {
            cur.push_back(c.string());
        }
    }
    std::deque<std::string> pending;
    for (const auto& c : fs::path(target)) {
        pending.push_back(c.string());
    }
    int budget = 40; // like SYMLOOP_MAX
    while (!pending.empty()) {
        std::string c = std::move(pending.front());
        pending.pop_front();
        if (c.empty() || c == ".") {
            continue;
        }
        if (c == "..") {
            if (!cur.empty() && cur.front() == "..") {
                // Already outside dest. Refuse to climb any further, and
                // refuse `..` after descending into the prefix: on the real
                // filesystem $HOMEBREW_PREFIX/opt/<dep> etc. are themselves
                // symlinks, so lexical `..` there would not mean what it
                // says.
                return false;
            }
            if (cur.empty()) {
                cur.push_back(".."); // the directory containing dest
            } else {
                cur.pop_back();
            }
            continue;
        }
        cur.push_back(c);
        if (cur.front() == "..") {
            continue; // outside dest: nothing of ours to follow
        }
        fs::path on_disk = dest;
        for (const auto& x : cur) {
            on_disk /= x;
        }
        if (!is_symlink_nofollow(on_disk)) {
            continue;
        }
        if (--budget <= 0) {
            return false;
        }
        std::error_code ec;
        auto next = fs::read_symlink(on_disk, ec);
        if (ec || next.empty() || next.is_absolute()) {
            return false;
        }
        cur.pop_back();
        std::vector<std::string> comps;
        for (const auto& x : next) {
            comps.push_back(x.string());
        }
        pending.insert(pending.begin(), comps.begin(), comps.end());
    }
    out = std::move(cur);
    return true;
}

void check_symlink_target(const fs::path& dest, const fs::path& link_rel, const std::string& target,
                          SymlinkScope scope) {
    std::vector<std::string> resolved;
    bool ok = resolve_link_target(dest, link_rel, target, resolved);
    if (ok && !resolved.empty() && resolved.front() == "..") {
        // Outside dest: only bottles may do this, and only into a standard
        // prefix directory ($HOMEBREW_PREFIX/etc/..., $HOMEBREW_PREFIX/opt/...).
        ok = scope == SymlinkScope::BottlePrefix && resolved.size() >= 2 &&
             is_bottle_prefix_dir(resolved[1]);
    }
    if (!ok) {
        throw ArchiveError("archive symlink escapes dest: " + link_rel.string() + " -> " + target);
    }
}

using ArchivePtr = std::unique_ptr<struct archive, decltype(&archive_read_free)>;

} // namespace

ExtractResult extract_archive(const fs::path& archive_path, const fs::path& dest_in,
                              const ExtractOptions& opts) {
    ArchivePtr ar(archive_read_new(), archive_read_free);
    if (!ar) {
        throw ArchiveError("failed to create archive reader");
    }

    archive_read_support_filter_all(ar.get());
    archive_read_support_format_all(ar.get());

    int rc = archive_read_open_filename(ar.get(), archive_path.c_str(), 16384);
    if (rc != ARCHIVE_OK) {
        throw ArchiveError("failed to open archive " + archive_path.string() + ": " +
                           archive_error_string(ar.get()));
    }

    fs::create_directories(dest_in);
    // Canonicalise dest so no symlink sits above it (macOS /tmp and
    // /var/folders are symlinks). That lets libarchive's SECURE_SYMLINKS
    // check the full rewritten path without false positives.
    const fs::path dest = fs::canonical(dest_in);

    // Use a single write_disk extractor for the whole archive.
    // SECURE_NODOTDOT rejects .. pathnames; SECURE_SYMLINKS refuses to write
    // through a symlink on disk. Both back up the explicit checks below,
    // which give clearer errors and also police symlink *targets* (libarchive
    // creates symlinks verbatim).
    ArchivePtr disk(archive_write_disk_new(), archive_read_free);
    archive_write_disk_set_options(disk.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                                   ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                                   ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    std::vector<std::pair<fs::path, std::string>> symlinks;

    ExtractResult result;
    std::string common_root;
    bool first_entry = true;

    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(ar.get(), &entry) == ARCHIVE_OK) {
        std::string entry_path = archive_entry_pathname(entry);

        // Security: reject unsafe paths.
        if (is_path_unsafe(entry_path)) {
            throw ArchiveError("archive contains unsafe path: " + entry_path);
        }

        // Security: never write through, or replace, a symlink. A tar can
        // carry `a -> /etc` followed by `a/passwd`, or a directory entry `a/`
        // after the link (libarchive would chmod/utime the link's target).
        reject_symlink_traversal(dest, entry_path, /*include_last=*/true, "entry");

        // Security: reject hardlinks pointing outside dest or through a
        // symlink (a hardlink to `a/passwd` with `a -> /etc` would alias an
        // outside file inside the keg).
        const char* hardlink = archive_entry_hardlink(entry);
        if (hardlink != nullptr) {
            if (is_path_unsafe(hardlink)) {
                throw ArchiveError("archive contains unsafe hardlink: " + std::string(hardlink));
            }
            reject_symlink_traversal(dest, hardlink, /*include_last=*/true, "hardlink target");
        }

        // Security: a symlink target must resolve inside dest (or, for
        // bottles, into a standard $HOMEBREW_PREFIX directory one level up).
        // Absolute targets always fail. Resolution follows symlinks already
        // extracted, so `x -> ../etc` then `y -> x/../..` is caught.
        const char* symlink_target = archive_entry_symlink(entry);
        if (symlink_target != nullptr) {
            check_symlink_target(dest, entry_path, symlink_target, opts.symlink_scope);
            symlinks.emplace_back(entry_path, symlink_target);
        }

        // Track the common root directory.
        fs::path rel(entry_path);
        std::string top = rel.begin()->string();
        if (first_entry) {
            common_root = top;
            first_entry = false;
        } else if (top != common_root) {
            common_root.clear();
        }

        // Rewrite the entry path to be under dest.
        fs::path full_path = dest / entry_path;
        archive_entry_set_pathname(entry, full_path.c_str());

        // Also rewrite hardlink targets to be under dest.
        if (hardlink) {
            auto full_hl = dest / std::string(hardlink);
            archive_entry_set_hardlink(entry, full_hl.c_str());
        }

        rc = archive_write_header(disk.get(), entry);
        if (rc != ARCHIVE_OK) {
            SPDLOG_WARN("extract header warning for {}: {}", entry_path,
                        archive_error_string(disk.get()));
            if (rc == ARCHIVE_FATAL) {
                throw ArchiveError("failed to write header for " + entry_path + ": " +
                                   archive_error_string(disk.get()));
            }
        }

        // Copy data blocks for regular files.
        if (archive_entry_size(entry) > 0) {
            const void* buf = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;

            while (archive_read_data_block(ar.get(), &buf, &size, &offset) == ARCHIVE_OK) {
                archive_write_data_block(disk.get(), buf, size, offset);
            }
        }

        archive_write_finish_entry(disk.get());
        result.files.emplace_back(entry_path);
    }

    // A symlink extracted later can redirect an earlier one (`a -> b/../..`
    // checked while b was a plain path, then `b -> ../opt`). Re-check every
    // link against the final tree.
    for (const auto& [link, target] : symlinks) {
        check_symlink_target(dest, link, target, opts.symlink_scope);
    }

    result.root = common_root;

    SPDLOG_INFO("extracted {} files from {}", result.files.size(), archive_path.string());

    return result;
}

} // namespace den
