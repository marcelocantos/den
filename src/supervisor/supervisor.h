// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "../core/config.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace den {

namespace fs = std::filesystem;

enum class RestartPolicy {
    Never,
    Always,
    OnFailure,
};

/// Parse / emit a RestartPolicy as the string used in spec.json.
std::string restart_policy_to_string(RestartPolicy p);
std::optional<RestartPolicy> restart_policy_from_string(const std::string& s);

struct ServiceSpec {
    std::string name;
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env; // extra env (in addition to inherited)
    std::string working_dir;                              // empty = inherit
    RestartPolicy restart = RestartPolicy::Never;
    std::vector<std::string> depends_on; // names of services this depends on
};

struct ServiceStatus {
    std::string name;
    std::optional<int> sup_pid;
    std::optional<int> svc_pid;
    std::optional<uint64_t> start_time; // unix secs
    std::optional<int> last_exit;
    uint32_t restart_count = 0;
    RestartPolicy restart = RestartPolicy::Never;
    bool running = false; // true iff sup_pid is alive
};

// ---- Paths -----------------------------------------------------------------

/// `<den_home>/services/`
fs::path services_root(const Config& config);

/// `<den_home>/services/<name>/`
fs::path service_dir(const Config& config, const std::string& name);

// ---- Spec I/O --------------------------------------------------------------

/// Load a spec.json for the named service. Returns nullopt if missing.
std::optional<ServiceSpec> load_spec(const Config& config, const std::string& name);

/// Persist a spec to `<service_dir>/spec.json` (atomic write).
void save_spec(const Config& config, const ServiceSpec& spec);

/// Resolve a service spec for the given name. Order of resolution:
///   1. `<service_dir>/spec.json` if present
///   2. A Homebrew-style `homebrew.mxcl.<name>.plist` discovered in the
///      installed packages — parsed via `plist_import.h` and saved to
///      spec.json for subsequent runs.
/// Returns nullopt if neither source yields a usable spec.
std::optional<ServiceSpec> resolve_service_spec(const Config& config, const std::string& name);

// ---- Lifecycle -------------------------------------------------------------

/// Start a service. Spawns a supervisor process that survives den's exit
/// (double-fork + setsid) and writes its PID to `<service_dir>/sup.pid`.
/// Returns true if the supervisor signalled successful startup (sup.pid
/// observed within a few seconds).
bool start_service(const Config& config, const ServiceSpec& spec);

/// Stop a service. Sends SIGTERM to the supervisor, then SIGKILL after
/// `timeout_secs` if it has not exited. Returns true if the supervisor's
/// PID file is gone (or supervisor not running) when we return.
bool stop_service(const Config& config, const std::string& name, uint32_t timeout_secs = 10);

/// Stop several services in dependency-aware order: dependents are stopped
/// before their dependencies (computed via each service's spec's
/// `depends_on` list). Unknown services are skipped with a warning.
/// Returns the names that stopped cleanly.
std::vector<std::string> stop_services_ordered(const Config& config,
                                               const std::vector<std::string>& names,
                                               uint32_t timeout_secs = 10);

/// Snapshot status for a single service.
std::optional<ServiceStatus> service_status(const Config& config, const std::string& name);

/// Snapshot status for every service that has a directory under services_root.
std::vector<ServiceStatus> list_services(const Config& config);

// ---- Logs ------------------------------------------------------------------

/// Path to the captured log file (`<service_dir>/log`).
fs::path service_log_path(const Config& config, const std::string& name);

/// Cat the existing log to stdout.
void cat_service_log(const Config& config, const std::string& name);

/// Follow the log: write existing contents, then poll for new bytes until
/// SIGINT / SIGTERM. Returns when interrupted.
void follow_service_log(const Config& config, const std::string& name);

// ---- Test seam -------------------------------------------------------------

/// Run the supervisor main loop directly in the current process (no fork).
/// Used by tests and by the spawned grandchild. Returns the process exit
/// code (0 on normal completion).
int run_supervisor_loop(const Config& config, const ServiceSpec& spec);

} // namespace den
