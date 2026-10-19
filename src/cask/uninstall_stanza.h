// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// 🎯T12 — Homebrew cask `uninstall` stanzas (Mac-first slice).
//
// Supported directives: quit, signal, launchctl, pkgutil, delete, trash,
// rmdir. Everything else (script, early_script, kext, login_item, …) is
// recorded as unsupported and reported loudly, never silently skipped.
// zap and uninstall_preflight/postflight are out of scope.
//
// Safety rules:
//   - Only the exact paths/labels/ids named in the stanza are acted on.
//   - Paths: `~` / `~/…` expand to $HOME; anything relative, containing
//     `..`, globs, `$`, or naming a protected system directory is refused.
//   - Nothing is ever escalated: operations that need root are reported as
//     "needs admin" with the exact command to run.

#include "../index/package.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace den {

namespace fs = std::filesystem;

/// Side effects the stanza runner needs from the OS. Injected so tests can
/// observe calls and simulate apps that ignore quit.
class CaskSystem {
  public:
    virtual ~CaskSystem() = default;
    virtual bool app_running(const std::string& bundle_id) = 0;
    /// Ask the app to quit gracefully (Apple Event). False + `err` on failure.
    virtual bool request_quit(const std::string& bundle_id, std::string& err) = 0;
    virtual std::vector<int> app_pids(const std::string& bundle_id) = 0;
    virtual bool send_signal(int pid, int sig) = 0;
    /// Is `label` loaded in the user (gui) domain, or the system domain?
    virtual bool launchctl_loaded(const std::string& label, bool system_domain) = 0;
    virtual bool launchctl_remove(const std::string& label, bool system_domain,
                                  std::string& err) = 0;
    virtual std::vector<std::string> pkgutil_match(const std::string& pattern) = 0;
    enum class ForgetResult { Ok, NeedsAdmin, Failed };
    virtual ForgetResult pkgutil_forget(const std::string& id, std::string& err) = 0;
    virtual bool is_root() = 0;
    virtual void sleep_ms(int ms) = 0;
};

/// The real macOS implementation (lsappinfo, osascript, launchctl, pkgutil).
CaskSystem& real_cask_system();

/// Process-wide system used by install_cask/uninstall_cask. Tests swap in a
/// fake; pass nullptr to restore the real one.
CaskSystem& cask_system();
void set_cask_system(CaskSystem* sys);

enum class StanzaPhase {
    /// Before an upgrade swap: launchctl (unload only, plists kept), quit,
    /// signal. Destructive directives are not run on upgrade because the
    /// swap's rollback could not restore what they remove.
    Stop,
    /// Full uninstall, before den removes the app: launchctl, quit, signal,
    /// pkgutil.
    PreRemove,
    /// Full uninstall, after den removes the app: delete, trash, rmdir.
    PostRemove,
};

struct StanzaOptions {
    fs::path home;      // `~` expansion
    fs::path trash_dir; // where `trash` moves things (default ~/.Trash)
    fs::path appdir;    // DEN_APPDIR; /Applications/... is rebased here when different
    int quit_timeout_ms = 10000;
    int signal_wait_ms = 3000;
    std::vector<fs::path> user_agent_dirs; // default ~/Library/LaunchAgents
    std::vector<fs::path>
        system_agent_dirs; // default /Library/LaunchAgents, /Library/LaunchDaemons
};

/// Options from the environment: HOME, DEN_APPDIR (via appdir),
/// DEN_CASK_QUIT_TIMEOUT (seconds).
StanzaOptions default_stanza_options(const fs::path& appdir);

struct StanzaReport {
    std::vector<std::string> actions;     // what was done
    std::vector<std::string> warnings;    // refused / unsupported / timed out
    std::vector<std::string> needs_admin; // exact commands the user must run
};

/// Run the directives of `phase`. Never throws for a single directive's
/// failure; everything is reported.
StanzaReport run_uninstall_stanza(const CaskUninstall& stanza, StanzaPhase phase, CaskSystem& sys,
                                  const StanzaOptions& opts);

/// Resolve a stanza path safely. Returns nullopt (and sets `why`) when the
/// path must not be touched.
std::optional<fs::path> resolve_stanza_path(const std::string& raw, const StanzaOptions& opts,
                                            std::string& why);

bool valid_bundle_id(const std::string& s);
bool valid_launchd_label(const std::string& s);
/// Signal name (TERM, SIGTERM, KILL, …) or number → signal value, or -1.
int parse_signal(const std::string& s);

} // namespace den
