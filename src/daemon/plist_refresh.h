// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Bringing this den home's installed daemon LaunchAgent PATH up to date with
// den's own layout. One implementation, used by `den self-update` (before it
// restarts the daemon) and by the daemon itself at startup, so an upgrade
// from a den that predates the refresh is right in one step.

#include "../core/config.h"
#include "launchd_plist.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <string>

namespace den {

/// What refresh_daemon_plist did, and the PATH den's layout gives the
/// plist's daemon.
struct DaemonPlistRefresh {
    PlistPathRefresh outcome = PlistPathRefresh::LeftAlone;
    /// The plist belongs to this den home (and, when this process runs with
    /// DEN_PREFIX, says which prefix it maintains), so `path` applies to it.
    bool ours = false;
    /// launchd_path() for the plist's den home and prefix; empty unless ours.
    std::string path;
};

/// Bring the installed daemon plist `plist`'s PATH up to date with den's own
/// layout (launchd_path), for a plist that belongs to `cfg`'s den home. The
/// prefix is the plist's DEN_PREFIX when it sets one, else cfg.prefix. A plist
/// for another den home, or one installed without DEN_PREFIX while this
/// process runs with DEN_PREFIX set, is left untouched. Hand-edited plists are
/// reported, not rewritten (refresh_launchd_plist_path). The previous plist is
/// saved to <den_home>/dev.den.daemon.plist.bak. A missing plist is left alone.
DaemonPlistRefresh refresh_daemon_plist(const Config& cfg, const std::filesystem::path& plist,
                                        std::ostream& out);

/// refresh_daemon_plist(...).outcome.
PlistPathRefresh refresh_daemon_plist_path(const Config& cfg, const std::filesystem::path& plist,
                                           std::ostream& out);

/// What the daemon's startup refresh needs from the host, injectable for
/// tests.
struct DaemonStartupPlistEnv {
    /// The installed plist (~/Library/LaunchAgents/dev.den.daemon.plist), if
    /// any (macOS only).
    std::optional<std::filesystem::path> plist;
    /// True when this process is the daemon's own launchd job, so its
    /// environment came from that plist.
    bool inside_launchd_job = false;
    /// Appends one line to daemon.log.
    std::function<void(const std::string&)> log;
    /// Sets this process's PATH.
    std::function<void(const std::string&)> set_path;
};

/// The daemon's startup step: refresh_daemon_plist on env.plist, logging one
/// line to daemon.log when it rewrote the plist or left a stale hand-edited
/// one alone (nothing when up to date or not ours). When the plist is ours and
/// now current and this process is its launchd job, this run switches to the
/// corrected PATH in-process (launchd started it with the plist's old PATH).
/// Never throws: a failure is logged and the daemon carries on.
PlistPathRefresh refresh_daemon_plist_at_startup(const Config& cfg,
                                                 const DaemonStartupPlistEnv& env);

/// refresh_daemon_plist_at_startup wired to the real host: the current user's
/// LaunchAgents plist, XPC_SERVICE_NAME, daemon.log and setenv("PATH").
/// A no-op off macOS.
void refresh_daemon_plist_at_startup(const Config& cfg);

} // namespace den
