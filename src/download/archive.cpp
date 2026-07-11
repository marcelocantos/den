// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "archive.h"

#include "../core/error.h"

#include <spdlog/spdlog.h>

#include <archive.h>
#include <archive_entry.h>

#include <memory>

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

using ArchivePtr = std::unique_ptr<struct archive, decltype(&archive_read_free)>;

} // namespace

ExtractResult extract_archive(const fs::path& archive_path, const fs::path& dest) {
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

    fs::create_directories(dest);

    // Use a single write_disk extractor for the whole archive.
    // Do NOT use ARCHIVE_EXTRACT_SECURE_SYMLINKS — Homebrew bottles
    // contain internal symlinks (e.g. .brew/ directory) that are safe.
    // Our own is_path_unsafe check handles traversal attacks.
    ArchivePtr disk(archive_write_disk_new(), archive_read_free);
    archive_write_disk_set_options(disk.get(), ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                                                   ARCHIVE_EXTRACT_SECURE_NODOTDOT);

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

        // Security: reject hardlinks pointing outside dest.
        const char* hardlink = archive_entry_hardlink(entry);
        if (hardlink != nullptr && is_path_unsafe(hardlink)) {
            throw ArchiveError("archive contains unsafe hardlink: " + std::string(hardlink));
        }

        // Security: reject symlink targets that escape dest. Absolute targets
        // or `..` components let a later regular-file entry write through the
        // link outside dest (libarchive does not O_NOFOLLOW parent dirs).
        // Homebrew bottles use relative internal symlinks (e.g. .brew/) which
        // pass this check without needing ARCHIVE_EXTRACT_SECURE_SYMLINKS.
        const char* symlink_target = archive_entry_symlink(entry);
        if (symlink_target != nullptr && is_path_unsafe(symlink_target)) {
            throw ArchiveError("archive contains unsafe symlink target: " +
                               std::string(symlink_target));
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

    result.root = common_root;

    SPDLOG_INFO("extracted {} files from {}", result.files.size(), archive_path.string());

    return result;
}

} // namespace den
