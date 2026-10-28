// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Restarting a launchd agent (dev.den.daemon) onto a freshly swapped binary.
//
// STOPGAP. den is signed ad hoc, so each release has a different code
// identity. If launchd merely respawns the job (KeepAlive, kickstart), it
// checks the new binary against the launch constraint it recorded for the old
// one and can refuse the spawn (`last exit reason = OS_REASON_CODESIGNING`).
// On macOS 27 we observed that `launchctl bootout gui/<uid>/<label>` followed
// by `launchctl bootstrap gui/<uid> <plist>` drops that recorded constraint,
// so even an ad-hoc swap restarts cleanly. This depends on observed macOS
// behaviour, not on a documented guarantee. The fallback below tolerates
// exactly one codesigning spawn failure before giving up. The real fix is to
// sign den with a stable team identity.
//
// Environment: launchd builds the service environment from the plist's
// EnvironmentVariables, launchd's defaults and the gui domain, not from the
// caller. As insurance, launchctl itself is always run as
// `env -i HOME=… USER=… PATH=/usr/bin:/bin:/usr/sbin:/sbin launchctl …` so
// nothing from the caller's session (SSH_AUTH_SOCK, SSH_*, TERM, shell
// variables) can reach the service.

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <sys/types.h>

namespace den {

/// PATH given to launchctl (and to the detached restart helper).
inline constexpr const char* kScrubbedPath = "/usr/bin:/bin:/usr/sbin:/sbin";
/// How long to wait for the restarted job to report a new PID.
inline constexpr std::chrono::seconds kRestartConfirmTimeout{15};

struct ScrubbedEnv {
    std::string home;
    std::string user;
};

/// HOME and USER for the scrubbed environment: from the passwd entry for the
/// current uid, falling back to $HOME / $USER.
ScrubbedEnv scrubbed_env_for_current_user();

/// argv that runs `tool args...` under `env -i HOME=… USER=… PATH=…`.
std::vector<std::string> scrubbed_argv(const ScrubbedEnv& env, const std::string& tool,
                                       const std::vector<std::string>& args);

struct SpawnResult {
    bool spawned = false;
    int exit_code = -1;
    std::string output; ///< combined stdout + stderr
};

/// Spawn `argv` (argv[0] an absolute path) with an EMPTY environment
/// (argv is expected to come from scrubbed_argv), wait, and capture output.
SpawnResult run_scrubbed(const std::vector<std::string>& argv);

/// What `launchctl print gui/<uid>/<label>` says about a job.
struct LaunchdJobState {
    bool loaded = false;
    std::optional<pid_t> pid;
    std::optional<int> runs;
    std::string last_exit_reason; ///< e.g. "OS_REASON_CODESIGNING"; may be empty
    bool codesign_failure() const;
};

/// Parse `launchctl print` output.
LaunchdJobState parse_launchctl_print(bool loaded, const std::string& text);

/// Host operations, injectable for tests.
struct LaunchdOps {
    std::string domain; ///< "gui/<uid>"
    std::string label;  ///< "dev.den.daemon"
    std::filesystem::path plist;
    /// Runs launchctl with the given args (scrubbed env); returns the result.
    std::function<SpawnResult(const std::vector<std::string>&)> launchctl;
    std::function<void(std::chrono::milliseconds)> sleep;
    std::function<std::chrono::steady_clock::time_point()> now;
    std::chrono::seconds confirm_timeout = kRestartConfirmTimeout;
};

/// The launchctl to run: the first executable `launchctl` on the caller's
/// PATH (so a test harness can substitute a stub), else /bin/launchctl.
/// Only the tool's location comes from the caller; its environment never does.
std::string resolve_launchctl();

/// LaunchdOps wired to `launchctl_path` (default resolve_launchctl()) run
/// via run_scrubbed.
LaunchdOps real_launchd_ops(const std::string& label, const std::filesystem::path& plist,
                            std::string launchctl_path = {});

enum class LaunchdRestartResult {
    Restarted,                   ///< new PID, no codesign failure seen
    RestartedAfterCodesignRetry, ///< new PID after exactly one codesign spawn failure
    Failed,
};

/// bootout gui/<uid>/<label>, bootstrap gui/<uid> <plist>, then wait up to
/// confirm_timeout for a PID different from `old_pid`. One codesigning spawn
/// failure is tolerated (the job is kickstarted once); a second fails.
/// Progress and diagnostics go to `out`.
LaunchdRestartResult restart_launchd_job(const LaunchdOps& ops, std::optional<pid_t> old_pid,
                                         std::ostream& out);

/// Body of the detached helper's /bin/sh script (bootout, bootstrap with a
/// short retry, then poll for a running PID, logging to stdout).
std::string detached_restart_script(const std::string& launchctl_path, const ScrubbedEnv& env,
                                    const std::string& domain, const std::string& label,
                                    const std::filesystem::path& plist);

/// Run the restart from a helper that outlives the caller: double fork, the
/// grandchild calls setsid() (new session and process group, so neither
/// launchd's bootout of the caller's job nor a signal to its process group
/// reaches it) and execs /bin/sh with an empty environment plus
/// HOME/USER/PATH, logging to `log`. Returns the helper's PID, or nullopt if
/// it could not be started.
std::optional<pid_t> spawn_detached_restart(const std::string& script,
                                            const std::filesystem::path& log,
                                            const ScrubbedEnv& env);

/// True when this process runs inside the launchd job `label` (launchd sets
/// XPC_SERVICE_NAME for a job's processes and their children), i.e. a
/// bootout of that job would kill us mid-restart.
bool running_inside_launchd_job(const std::string& label);

} // namespace den
