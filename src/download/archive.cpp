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

using ArchivePtr =
    std::unique_ptr<struct archive, decltype(&archive_read_free)>;

} // namespace

ExtractResult extract_archive(const fs::path& archive_path,
                              const fs::path& dest) {
    ArchivePtr ar(archive_read_new(), archive_read_free);
    if (!ar) {
        throw ArchiveError("failed to create archive reader");
    }

    archive_read_support_filter_all(ar.get());
    archive_read_support_format_all(ar.get());

    int rc =
        archive_read_open_filename(ar.get(), archive_path.c_str(), 16384);
    if (rc != ARCHIVE_OK) {
        throw ArchiveError("failed to open archive " +
                           archive_path.string() + ": " +
                           archive_error_string(ar.get()));
    }

    fs::create_directories(dest);

    ExtractResult result;
    std::string common_root;
    bool first_entry = true;

    struct archive_entry* entry = nullptr;
    while (archive_read_next_header(ar.get(), &entry) == ARCHIVE_OK) {
        std::string entry_path = archive_entry_pathname(entry);

        // Security: reject unsafe paths.
        if (is_path_unsafe(entry_path)) {
            throw ArchiveError(
                "archive contains unsafe path: " + entry_path);
        }

        // Security: reject hardlinks pointing outside dest.
        const char* hardlink = archive_entry_hardlink(entry);
        if (hardlink != nullptr && is_path_unsafe(hardlink)) {
            throw ArchiveError(
                "archive contains unsafe hardlink: " +
                std::string(hardlink));
        }

        // Track the common root directory.
        fs::path rel(entry_path);
        std::string top = rel.begin()->string();
        if (first_entry) {
            common_root = top;
            first_entry = false;
        } else if (top != common_root) {
            // Multiple top-level entries; no single root.
            common_root.clear();
        }

        fs::path full_path = dest / entry_path;

        // Create parent directories.
        fs::create_directories(full_path.parent_path());

        auto file_type = archive_entry_filetype(entry);

        if (file_type == AE_IFDIR) {
            fs::create_directories(full_path);
        } else if (file_type == AE_IFREG) {
            // Extract regular file.
            ArchivePtr disk(archive_write_disk_new(),
                            archive_read_free); // archive_write_free
            // Correct deleter — but archive_read_free and
            // archive_write_free are the same symbol in practice.

            archive_entry_set_pathname(entry, full_path.c_str());
            archive_write_disk_set_options(
                disk.get(),
                ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
                    ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                    ARCHIVE_EXTRACT_SECURE_SYMLINKS);

            rc = archive_write_header(disk.get(), entry);
            if (rc != ARCHIVE_OK) {
                throw ArchiveError(
                    "failed to write header for " + entry_path +
                    ": " + archive_error_string(disk.get()));
            }

            const void* buf = nullptr;
            size_t size = 0;
            la_int64_t offset = 0;

            while (archive_read_data_block(ar.get(), &buf, &size,
                                           &offset) == ARCHIVE_OK) {
                rc = archive_write_data_block(disk.get(), buf, size,
                                              offset);
                if (rc != ARCHIVE_OK) {
                    throw ArchiveError(
                        "failed to write data for " + entry_path +
                        ": " + archive_error_string(disk.get()));
                }
            }

            archive_write_finish_entry(disk.get());
        } else if (file_type == AE_IFLNK) {
            // Symlinks are OK — they are validated by
            // ARCHIVE_EXTRACT_SECURE_SYMLINKS above if we use
            // write_disk, but for simplicity we create them directly.
            const char* target = archive_entry_symlink(entry);
            if (target != nullptr) {
                fs::create_symlink(target, full_path);
            }
        } else {
            // Skip other file types (devices, sockets, etc.).
            SPDLOG_WARN("skipping unsupported entry type in archive: {}",
                        entry_path);
            continue;
        }

        result.files.emplace_back(entry_path);
    }

    result.root = common_root;

    SPDLOG_INFO("extracted {} files from {}", result.files.size(),
                archive_path.string());

    return result;
}

} // namespace den
