// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace den {

namespace fs = std::filesystem;

struct InstalledPackage {
    std::string name;
    std::string version;
    fs::path path;
};

/// A keg (one installed version) enriched with Cellar-inspection metadata
/// for `den list --cellar`.
struct KegInfo {
    std::string name;
    std::string version;
    fs::path path;
    uint64_t disk_bytes = 0;           // recursive on-disk size of the keg
    std::vector<std::string> env_refs; // environment paths referencing this keg, sorted
    bool orphaned() const { return env_refs.empty(); }
};

/// Recursively sum the on-disk size of a keg directory. Follows no symlinks
/// (counts the link entry, not its target) and tolerates transient I/O errors.
uint64_t keg_disk_usage(const fs::path& keg);

/// Inspect every keg in the Cellar: disk usage, the environments that
/// reference it, and whether it is orphaned (referenced by no environment).
/// References are derived from the environment manifests under den_home.
/// The result is sorted by (name, version) for deterministic output.
std::vector<KegInfo> inspect_cellar(const fs::path& store, const fs::path& den_home);

/// Return the path for a specific package version in the store.
/// Layout: store/<name>/<version>/
fs::path package_path(const fs::path& store, const std::string& name, const std::string& version);

/// Check whether a package version is installed in the store.
bool is_installed(const fs::path& store, const std::string& name, const std::string& version);

/// List all installed packages by scanning the store directory.
std::vector<InstalledPackage> list_installed(const fs::path& store);

/// Identify which installed package owns a given file path.
/// Resolves symlinks, then matches the real path against the store layout.
/// Returns nullopt if the file doesn't belong to any installed package.
std::optional<InstalledPackage> which_package(const fs::path& store, const fs::path& file);

// ---------------------------------------------------------------------------
// Ownership receipts
//
// den and Homebrew share one Cellar. A keg is den's to delete only when den
// itself poured or built it. That fact is a receipt written into the keg at
// install time — never inferred from "no manifest references this directory".
// Homebrew does not write this file. Kegs poured before receipts existed have
// none either, and cleanup leaves them in place.
// ---------------------------------------------------------------------------

/// Filename of the receipt den writes at the root of a keg it poured or built.
inline constexpr std::string_view kDenReceiptFilename = "DEN_RECEIPT.json";

/// Record that den poured or built the keg at `keg` (a `store/<name>/<version>`
/// directory). No-op, with a warning, if that directory is missing. Safe to
/// call only for a keg den just created — never for one found already present.
void mark_keg_owned(const fs::path& keg, const std::string& name, const std::string& version);

/// True only when `keg` contains a den receipt naming this exact keg.
/// Missing, unreadable, symlinked, malformed, or mismatched receipts are not
/// ownership. Callers must treat false as "do not delete".
bool keg_owned_by_den(const fs::path& keg);

/// Result of `cleanup_kegs`.
struct CleanupReport {
    /// Den-owned kegs no environment references. Deleted unless dry-run.
    std::vector<InstalledPackage> removed;
    /// Unreferenced kegs left in place because den did not install them.
    uint32_t kept_unowned = 0;
    /// The archive cache existed and was cleared (or would be, on a dry-run).
    bool cache_cleared = false;
};

/// Remove den-owned kegs that no environment manifest references, and clear
/// `<cache>/archives`. Kegs without a den receipt — Homebrew installs, and
/// anything poured before receipts existed — are never removed.
///
/// When `dry_run` is true, nothing is deleted. `removed` still lists the kegs
/// that would be removed, and `cache_cleared` reports that the archive cache
/// would be cleared.
CleanupReport cleanup_kegs(const fs::path& store, const fs::path& den_home, const fs::path& cache,
                           bool dry_run);

} // namespace den
