// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <filesystem>
#include <optional>
#include <string>

namespace den {

struct UpdateInfo {
    std::string latest_version; // version the binary reports, e.g. "0.5.0" (no v, no -rc.N)
    std::string download_url;   // tarball URL for this platform
    std::string checksum_url;   // .sha256 URL
    std::string tag;            // release tag, e.g. "v0.5.0" or "v0.5.0-rc.1"
};

/// Normalise a user-supplied release version to its tag: "0.15.0",
/// "v0.15.0", "0.15.0-rc.1" and "v0.15.0-rc.1" are accepted (the latter two
/// map to "v0.15.0-rc.1"). Throws UserError on anything else.
std::string normalize_release_tag(const std::string& version);

/// The version in a release's asset names (and printed by `den --version`):
/// the tag minus its leading "v" and any "-rc.N" suffix. Release candidates
/// ship the GA-named assets, e.g. tag v0.15.0-rc.1 carries
/// den-0.15.0-<os>-<arch>.tar.gz.
std::string asset_version_for_tag(const std::string& tag);

/// Platform suffix used in release tarball names, e.g. "darwin-aarch64".
std::string platform_suffix(const Config& cfg);

/// Build an UpdateInfo from a GitHub release JSON document (as returned by
/// /releases/latest or /releases/tags/<tag>). Returns nullopt when the JSON
/// is malformed or the release has no tarball for `suffix`.
std::optional<UpdateInfo> update_info_from_release_json(const std::string& json,
                                                        const std::string& suffix);

/// Fetch a specific release (GA or RC) by version or tag, via
/// /releases/tags/<tag>. Throws UserError when the version is malformed, the
/// release cannot be fetched, or it has no binary for this platform.
UpdateInfo fetch_release_for_version(const Config& cfg, const std::string& version);

/// The command that upgrades a Homebrew-installed den.
inline constexpr const char* kBrewUpgradeCommand = "brew upgrade marcelocantos/tap/den";

/// If `exe` (the path den was run from) belongs to a Homebrew keg, returns the
/// keg path that shows it; otherwise nullopt. A keg is detected when `exe` or
/// its realpath lies under a `.../Cellar/den/<version>/` directory, or under a
/// formula opt path `<P>/opt/den/` where `<P>/Cellar/den` exists (so a plain
/// /opt/den install is not mistaken for one). Paths are injected so tests can
/// exercise this without a real Homebrew install.
std::optional<std::filesystem::path> brew_keg_for_exe(const std::filesystem::path& exe);

/// Throws UserError telling the user to run kBrewUpgradeCommand when `exe` is
/// in a Homebrew keg (see brew_keg_for_exe); returns normally otherwise.
/// `den self-update` calls this before any network access or file change, so
/// brew stays the only thing that replaces a brew-installed den.
void refuse_self_update_if_brew_managed(const std::filesystem::path& exe);

/// The path the running den was started from (unresolved where the OS
/// reports it that way), or nullopt if it cannot be determined.
std::optional<std::filesystem::path> running_exe_path();

/// Fetch the latest available release version from GitHub Releases.
/// Returns empty string if unable to determine (network error, API oddity, etc.).
/// Unlike check_for_update, this always returns the version — it does NOT compare
/// against DEN_VERSION, making it suitable for --check / --dry-run queries.
std::string resolve_latest_version();

/// Check GitHub Releases for a newer version of den.
/// Returns nullopt if already up to date or unable to determine.
std::optional<UpdateInfo> check_for_update(const Config& cfg);

/// Download and install a new den binary, replacing the current one.
/// The running binary is atomically replaced via rename.
void apply_update(const UpdateInfo& info, const Config& cfg);

/// Create a fresh, private (0700) temporary directory under the system temp
/// dir with mkdtemp(3), named `<stem>.XXXXXX`. Throws UserError on failure.
std::filesystem::path make_private_temp_dir(const std::string& stem);

/// Install a verified update tarball: extract it with den's hardened
/// libarchive extractor into a fresh private temp dir, then atomically
/// replace `exe` with the tarball's `den` (a rename onto a fresh inode, never
/// an in-place write, so a running daemon's mapped binary is untouched) and
/// refresh the trust replica.
/// The temp dir is always removed. Throws UserError on failure.
void install_update_tarball(const std::filesystem::path& tarball, const std::filesystem::path& exe);

/// After extracting an update tarball into `extracted_root`, install the
/// tarball's share/den/known_hashes.json as <exe_dir>/../share/den/
/// known_hashes.json (🎯T42), creating it if missing. If the tarball has no
/// replica, an installed copy is left as is.
void refresh_packaged_replica(const std::filesystem::path& extracted_root,
                              const std::filesystem::path& exe);

/// Keep install.sh's installed-tag record (<exe_dir>/../share/den/
/// installed-tag) in step with a binary self-update just put at `exe`:
/// rewrite it as `<tag>\nsha256 <hex of exe>\n`, so install.sh still knows
/// the exact release (`den --version` omits -rc.N). A missing record is not
/// created (den was not installed by install.sh). Best effort: a failure is
/// logged, and install.sh then sees the sha mismatch and treats the record
/// as stale rather than trusting it.
void update_installed_tag_record(const std::filesystem::path& exe, const std::string& tag);

} // namespace den
