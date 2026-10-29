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
/// The Cellar directory name for a manifest/index name. A tap-qualified
/// formula ("user/repo/name") lives in Cellar/<name>, as Homebrew lays it
/// out; every other name is returned unchanged.
std::string keg_name(std::string_view name);

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

/// What den records about a keg it built from source (docs/source-builds.md,
/// "Receipt"): the verified inputs, where the recipe came from, the
/// toolchain, and the opt path the build baked in.
struct SourceBuildReceipt {
    struct Input {
        std::string name; // resource name; empty for the main source and patches
        std::string url;
        std::string sha256;
    };
    Input source;                 // the stable tarball
    std::vector<Input> resources; // formula `resource` blocks
    std::vector<Input> patches;   // external patches
    std::string formula_tap;      // "homebrew/core", or "<user>/<repo>" for a tap
    std::string formula_commit;   // the tap commit the .rb was fetched at; empty if unpinned
    std::string formula_path;     // "Formula/t/tree.rb", or the tap's local file
    std::string toolchain_sdk;    // macOS SDK path; empty elsewhere or if unknown
    std::string toolchain_clang;  // first line of the compiler's --version
    std::string opt_prefix_baked; // <prefix>/opt/<name>
    std::string fallback;         // "native" when den's recipe parser built it after a Ruby
                                  // build failed; empty when the Ruby build succeeded
    std::string fallback_reason;  // why the Ruby build failed, when fallback is set
};

/// Record that den poured or built the keg at `keg` (a `store/<name>/<version>`
/// directory). No-op, with a warning, if that directory is missing. Safe to
/// call only for a keg den just created — never for one found already present.
/// `version_scheme` is the formula's Homebrew version_scheme at pour time,
/// recorded so upgrade ordering can compare (version_scheme, version).
/// A source build passes `source`, recorded as "built_from_source": true plus
/// "source", "formula", "toolchain" and "opt_prefix_baked".
void mark_keg_owned(const fs::path& keg, const std::string& name, const std::string& version,
                    int version_scheme = 0, const SourceBuildReceipt* source = nullptr);

/// The source-build fields of the den receipt at `keg`, or nullopt when the
/// keg has no den receipt or was not built from source.
std::optional<SourceBuildReceipt> read_source_build_receipt(const fs::path& keg);

/// The Homebrew version_scheme of the installed keg at `keg`: the
/// "version_scheme" den recorded in DEN_RECEIPT.json, else Homebrew's
/// INSTALL_RECEIPT.json `source.versions.version_scheme`, else 0 (absent,
/// unreadable, or malformed receipts all mean 0, brew's default).
int installed_version_scheme(const fs::path& keg);

/// True only when `keg` contains a den receipt naming this exact keg.
/// Missing, unreadable, symlinked, malformed, or mismatched receipts are not
/// ownership. Callers must treat false as "do not delete".
bool keg_owned_by_den(const fs::path& keg);

/// An unreferenced den keg that cleanup keeps because a surviving keg's
/// Mach-O file still names it by its versioned path (`<store>/<name>/<ver>/…`)
/// in an LC_ID_DYLIB, dylib load or LC_RPATH command. Removing it would make
/// dyld fail to load that file.
struct KeptReference {
    InstalledPackage keg;  ///< the keg kept
    InstalledPackage by;   ///< a surviving keg that names it
    fs::path file;         ///< the Mach-O file in `by`
    std::string load_path; ///< the path as written in the load command
};

/// Result of `cleanup_kegs`.
struct CleanupReport {
    /// Den-owned kegs no environment references. Deleted unless dry-run.
    std::vector<InstalledPackage> removed;
    /// Unreferenced den-owned kegs kept because a surviving keg still loads
    /// them by versioned path (one entry per kept keg, sorted).
    std::vector<KeptReference> kept_referenced;
    /// Unreferenced kegs left in place because den did not install them.
    uint32_t kept_unowned = 0;
    /// The archive cache existed and was cleared (or would be, on a dry-run).
    bool cache_cleared = false;
};

/// Remove den-owned kegs that no environment manifest references, and clear
/// `<cache>/archives`. Kegs without a den receipt — Homebrew installs, and
/// anything poured before receipts existed — are never removed.
///
/// Manifests are not the only references: a bottle poured with its load
/// commands pinned to `<store>/<dep>/<version>/…` keeps loading that exact
/// version after `<dep>` is upgraded. So before removing anything, the Mach-O
/// files of every keg that stays are scanned, and any candidate keg they name
/// by versioned path (id, dylib load or rpath) is kept and reported in
/// `kept_referenced`, along with whatever it names in turn.
///
/// When `dry_run` is true, nothing is deleted. `removed` still lists the kegs
/// that would be removed, and `cache_cleared` reports that the archive cache
/// would be cleared.
CleanupReport cleanup_kegs(const fs::path& store, const fs::path& den_home, const fs::path& cache,
                           bool dry_run);

/// The kegs in `candidates` that the Mach-O files of `survivors` name by a
/// versioned path under `store` (LC_ID_DYLIB, any dylib load, or LC_RPATH),
/// closed transitively over the kegs found. A keg naming itself does not
/// count. Reads load commands directly, so it behaves the same on any host.
std::vector<KeptReference> macho_pinned_kegs(const fs::path& store,
                                             const std::vector<InstalledPackage>& survivors,
                                             const std::vector<InstalledPackage>& candidates);

/// Outcome of release_prefix_opt_link.
enum class OptReleaseAction {
    NotOwned,  // no link, a dangling link, or a link to a keg den does not own: left alone
    Kept,      // an environment still references the keg the link points at
    HandedOff, // repointed at another installed, den-owned, still-referenced version
    Removed,   // no other such version: the link was removed
    Failed,    // filesystem error (logged)
};

/// Called after `den uninstall` drops `name` from an environment. If
/// `<store.parent>/opt/<name>` points at a keg den poured (keg_owned_by_den)
/// whose version no environment under `den_home` still references, hand the
/// link to the highest other version of `name` in `store` that den owns and an
/// environment still references, or remove it when there is none. The keg
/// itself stays until `den cleanup`. A link den does not own (Homebrew's, a
/// real file, or a dangling link) is never touched. `handed_to` receives the
/// new version on HandedOff.
OptReleaseAction release_prefix_opt_link(const fs::path& store, const fs::path& den_home,
                                         const std::string& name, std::string* handed_to = nullptr);

} // namespace den
