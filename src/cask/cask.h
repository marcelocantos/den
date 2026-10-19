// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"
#include "../index/package.h"
#include "../provider/package_provider.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Filename of the receipt den writes at the root of a cask version directory
/// it staged. Same spirit as DEN_RECEIPT.json for Cellar kegs: den deletes or
/// overwrites an app/binary only when a receipt den wrote names that path.
inline constexpr std::string_view kDenCaskReceiptFilename = "DEN_CASK_RECEIPT.json";

/// One binary den linked from a cask install.
struct CaskBinaryLink {
    fs::path link;   // Path of the symlink den created (usually env bin/).
    fs::path target; // Absolute path inside the den caskroom stage.
};

/// Parsed receipt for a den-installed cask version.
struct CaskReceipt {
    std::string token;
    std::string version;
    std::vector<fs::path> apps; // Absolute .app paths den placed.
    std::vector<CaskBinaryLink> binaries;
    std::vector<std::string> pkgs; // pkgutil package IDs den installed, if any.
    bool quarantine_cleared = false;
    /// The cask's `uninstall` stanza at install time, so uninstall/upgrade
    /// do not depend on the current index. Includes unsupported directive
    /// names (recorded, never silently dropped).
    CaskUninstall uninstall;
};

/// den-managed cask staging root: <den_home>/caskroom
fs::path den_caskroom(const Config& config);

/// Version directory: <caskroom>/<token>/<version>
fs::path cask_version_path(const Config& config, const std::string& token,
                           const std::string& version);

/// Directory where .app bundles are installed. Override with DEN_APPDIR
/// (tests use a temp dir so /Applications is never touched).
fs::path cask_appdir(const Config& config);

/// True when `app_path` is listed in a den cask receipt under den's caskroom.
/// Missing/malformed receipts never count as ownership.
bool app_owned_by_den(const Config& config, const fs::path& app_path);

/// Read the receipt for a staged cask version, if present and valid.
std::optional<CaskReceipt> read_cask_receipt(const fs::path& version_dir);

/// Install a cask-type package (ArtifactType::App or a binary-only cask
/// indexed from cask.json). Stages under den's caskroom, places apps into
/// the appdir, links binaries into the active env bin, and records a receipt.
///
/// Never writes Homebrew's Caskroom. Never overwrites an existing app unless
/// den's own receipt owns it. Clears com.apple.quarantine on placed artifacts
/// (documented Homebrew-equivalent behaviour).
///
/// On non-Apple platforms throws UserError ("casks are not supported on …").
InstallResult install_cask(const Config& config, const Package& pkg);

/// Uninstall a den-installed cask: run the receipt's `uninstall` stanza
/// (launchctl, quit, signal, pkgutil; then delete, trash, rmdir after the
/// app is gone), remove only apps/binaries named in the receipt, then drop
/// the stage directory. An upgrade runs the stop directives (launchctl,
/// quit, signal) right before the swap. Refuses to delete apps not owned
/// by den. No-op (with a message path via exception) if nothing is installed.
void uninstall_cask(const Config& config, std::string_view token);

/// List casks den itself installed (from the caskroom receipts), newest
/// version per token.
std::vector<InstalledPackage> list_den_casks(const Config& config);

/// True if den has any staged version of this cask token.
bool is_cask_installed(const Config& config, std::string_view token);

/// Version string of the installed cask, or empty if none.
std::string installed_cask_version(const Config& config, std::string_view token);

} // namespace den
