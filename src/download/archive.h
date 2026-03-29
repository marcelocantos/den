// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Result of extracting an archive.
struct ExtractResult {
    std::vector<fs::path> files; // All extracted file paths (relative to dest).
    fs::path root;               // Common root directory inside the archive.
};

/// Extract a tar.gz (or other libarchive-supported) archive into dest.
///
/// Security: rejects absolute paths, path traversal (.. components),
/// and hardlinks pointing outside the destination.
/// Throws ArchiveError on failure.
ExtractResult extract_archive(const fs::path& archive, const fs::path& dest);

} // namespace den
