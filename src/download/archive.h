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

/// Where an archive's symlinks may point.
enum class SymlinkScope {
    /// Every symlink must resolve inside dest. The default for casks, source
    /// tarballs and Ruby bundles.
    Dest,

    /// dest stands in for a Homebrew Cellar: kegs unpack as
    /// dest/<name>/<version>/ and are later moved to
    /// $HOMEBREW_CELLAR/<name>/<version>/. Symlinks may additionally reach
    /// one level above dest (the virtual $HOMEBREW_PREFIX), but only into the
    /// standard prefix directories (see kBottlePrefixDirs). Real bottles do
    /// this: python@3.15 and certifi link cacert.pem to
    /// $HOMEBREW_PREFIX/etc/ca-certificates/cert.pem, llvm links etc/clang to
    /// $HOMEBREW_PREFIX/etc/clang, and powershell/fastlane link into
    /// $HOMEBREW_PREFIX/opt/<dep>.
    BottlePrefix,
};

/// Prefix-level directories a bottle symlink may target under
/// SymlinkScope::BottlePrefix.
inline constexpr const char* kBottlePrefixDirs[] = {
    "bin", "etc", "include", "lib", "opt", "sbin", "share", "var", "Frameworks",
};

struct ExtractOptions {
    SymlinkScope symlink_scope = SymlinkScope::Dest;
};

/// Extract a tar.gz (or other libarchive-supported) archive into dest.
///
/// Security:
/// - rejects absolute entry paths and `..` components;
/// - rejects hardlinks whose target is unsafe or passes through a symlink;
/// - rejects any entry whose path passes through, or would replace, a
///   symlink (so nothing is ever written through a link, whether the link
///   came from this archive or was already on disk);
/// - rejects absolute symlink targets, and relative targets that, after
///   following any symlinks already extracted, resolve outside the scope
///   allowed by opts.symlink_scope. Every symlink is re-checked once the
///   whole archive is down, so a link created later cannot redirect an
///   earlier one out of scope.
/// Throws ArchiveError on failure.
ExtractResult extract_archive(const fs::path& archive, const fs::path& dest,
                              const ExtractOptions& opts = {});

} // namespace den
