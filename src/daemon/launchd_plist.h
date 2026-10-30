// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// den's user daemon LaunchAgent (~/Library/LaunchAgents/dev.den.daemon.plist):
// generating it, reading values back out of an installed one, and bringing an
// installed one's PATH up to date.

#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// The PATH den's daemon runs with, built from den's own layout:
/// `<prefix>/bin` (when `prefix` is non-empty), `<den_home>/bin`, then
/// /usr/bin:/bin:/usr/sbin:/sbin. Nothing about Homebrew is hardcoded:
/// /opt/homebrew/bin or /usr/local/bin is on it only when it is den's own
/// prefix bin.
std::string launchd_path(const fs::path& den_home, const fs::path& prefix);

/// Generate a launchd plist XML string for the daemon service.
/// Label: dev.den.daemon. RunAtLoad and KeepAlive are both true.
/// Interpolated strings are XML-escaped. PATH is launchd_path(den_home,
/// prefix); an explicit DEN_PREFIX in this process's environment is carried
/// into the agent.
std::string launchd_plist(const fs::path& den_binary, const fs::path& den_home,
                          const fs::path& prefix = {});

/// The value of the EnvironmentVariables entry `key` in `plist`
/// (XML-unescaped), or nullopt when the plist has no such key.
std::optional<std::string> plist_env_value(const std::string& plist, const std::string& key);

/// The PATH value in `plist` (XML-unescaped), or an empty string when it has
/// none.
std::string plist_path_value(const std::string& plist);

/// `plist` with its PATH value replaced by `path` (XML-escaped), or nullopt
/// when the plist has no PATH entry.
std::optional<std::string> plist_with_path_value(const std::string& plist, const std::string& path);

/// Keys in `plist` that launchd_plist() never writes (in the order they
/// appear), i.e. ones added by hand.
std::vector<std::string> plist_unrecognised_keys(const std::string& plist);

/// Homebrew bin dirs on `path` that are not den's prefix bin: `<homebrew_prefix>/bin`
/// and `/sbin`, /opt/homebrew/bin and /usr/local/bin, minus `<prefix>/bin`
/// and `<prefix>/sbin`. In the order they appear on `path`.
std::vector<std::string> stale_homebrew_path_dirs(const std::string& path, const fs::path& prefix,
                                                  const fs::path& homebrew_prefix);

/// `den doctor`'s warning for an installed daemon plist (`plist_file`, text
/// `plist`) whose PATH carries a Homebrew bin dir that isn't den's prefix
/// bin, or an empty string when it carries none. `prefix` is den's prefix
/// (the plist's own DEN_PREFIX wins when it sets one).
std::string stale_daemon_path_problem(const fs::path& plist_file, const std::string& plist,
                                      const fs::path& prefix, const fs::path& homebrew_prefix);

enum class PlistPathRefresh {
    UpToDate,  ///< PATH already matched; the file was not touched.
    Rewritten, ///< PATH rewritten in place (atomically, after a backup).
    LeftAlone, ///< PATH differs, but the plist has hand-added keys (or no
               ///< PATH entry), so it was reported and not rewritten.
};

/// Bring the installed plist `plist_file`'s PATH to `path`. When it differs
/// and the plist holds only keys den writes, save the current text to
/// `backup`, then replace just the PATH value atomically. When the plist
/// has hand-added keys, leave it alone and report the difference instead.
/// Reports what it did on `out` (nothing when up to date).
PlistPathRefresh refresh_launchd_plist_path(const fs::path& plist_file, const std::string& path,
                                            const fs::path& backup, std::ostream& out);

/// What `den daemon install` does with the plist file: write `generated` to
/// `plist_file` atomically. When a different plist is already there, save it
/// to `backup` first and report the PATH change and any hand-added keys the
/// regenerated plist drops. Returns true when the file content changed.
bool write_daemon_plist(const fs::path& plist_file, const std::string& generated,
                        const fs::path& backup, std::ostream& out);

} // namespace den
