// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "supervisor.h"
#include "plist_import.h"

#include "../core/error.h"
#include "../store/store.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <sys/file.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;

namespace den {

namespace {

// ---- Constants ------------------------------------------------------------

constexpr uint32_t START_POLL_TIMEOUT_MS = 2000;
constexpr uint32_t START_POLL_INTERVAL_MS = 10;
constexpr uint32_t STOP_POLL_INTERVAL_MS = 50;
constexpr uint32_t MONITOR_POLL_INTERVAL_MS = 50;
constexpr uint64_t LONG_RUN_RESET_SECS = 30;
constexpr uint32_t BACKOFF_INITIAL_MS = 1000;
constexpr uint32_t BACKOFF_CAP_MS = 60000;

// ---- Signals --------------------------------------------------------------

volatile sig_atomic_t g_stop = 0;

void supervisor_sigterm(int /*sig*/) {
    g_stop = 1;
}

// ---- Time -----------------------------------------------------------------

uint64_t now_secs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

void sleep_ms(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Sleep but bail out early if g_stop is set. Returns true if interrupted.
bool sleep_with_interrupt(uint32_t ms) {
    uint32_t step = 50;
    for (uint32_t i = 0; i < ms; i += step) {
        if (g_stop)
            return true;
        sleep_ms(std::min(step, ms - i));
    }
    return g_stop;
}

// ---- PID liveness ---------------------------------------------------------

bool pid_alive(int pid) {
    if (pid <= 0)
        return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    return errno == EPERM;
}

// ---- File helpers ---------------------------------------------------------

void write_atomic(const fs::path& path, const std::string& content) {
    fs::path tmp = path.string() + ".tmp";
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    {
        std::ofstream f(tmp);
        f << content;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        SPDLOG_WARN("supervisor: failed to rename {} -> {}: {}", tmp.string(), path.string(),
                    ec.message());
    }
}

std::optional<int> read_pid_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f.is_open())
        return std::nullopt;
    int pid = 0;
    f >> pid;
    if (pid <= 0)
        return std::nullopt;
    return pid;
}

// ---- Paths ----------------------------------------------------------------

fs::path spec_path(const Config& cfg, const std::string& name) {
    return service_dir(cfg, name) / "spec.json";
}

fs::path sup_pid_path(const Config& cfg, const std::string& name) {
    return service_dir(cfg, name) / "sup.pid";
}

fs::path svc_pid_path(const Config& cfg, const std::string& name) {
    return service_dir(cfg, name) / "pid";
}

fs::path state_path(const Config& cfg, const std::string& name) {
    return service_dir(cfg, name) / "state.json";
}

// ---- State (runtime stats persisted by the supervisor) --------------------

struct ServiceRuntimeState {
    std::optional<uint64_t> start_time;
    std::optional<int> last_exit;
    uint32_t restart_count = 0;
};

ServiceRuntimeState read_runtime_state(const Config& cfg, const std::string& name) {
    ServiceRuntimeState s;
    std::ifstream f(state_path(cfg, name));
    if (!f.is_open())
        return s;
    try {
        nlohmann::json j;
        f >> j;
        if (j.contains("start_time") && j["start_time"].is_number_unsigned())
            s.start_time = j["start_time"].get<uint64_t>();
        if (j.contains("last_exit") && j["last_exit"].is_number_integer())
            s.last_exit = j["last_exit"].get<int>();
        if (j.contains("restart_count") && j["restart_count"].is_number_unsigned())
            s.restart_count = j["restart_count"].get<uint32_t>();
    } catch (const std::exception& e) {
        SPDLOG_WARN("supervisor: parse state failed for {}: {}", name, e.what());
    }
    return s;
}

void write_runtime_state(const Config& cfg, const std::string& name, const ServiceRuntimeState& s) {
    nlohmann::json j = nlohmann::json::object();
    if (s.start_time)
        j["start_time"] = *s.start_time;
    if (s.last_exit)
        j["last_exit"] = *s.last_exit;
    j["restart_count"] = s.restart_count;
    write_atomic(state_path(cfg, name), j.dump(2) + "\n");
}

// ---- Service discovery via plist ------------------------------------------

std::optional<ServiceSpec> import_from_homebrew_plist(const Config& cfg, const std::string& name) {
#ifndef __APPLE__
    (void)cfg;
    (void)name;
    return std::nullopt;
#else
    auto installed = list_installed(cfg.store);
    for (const auto& pkg : installed) {
        if (pkg.name != name)
            continue;
        for (const auto& candidate :
             {pkg.path / (std::string("homebrew.mxcl.") + pkg.name + ".plist"),
              pkg.path / "share" / (pkg.name + ".plist")}) {
            if (!fs::exists(candidate))
                continue;
            auto spec = parse_homebrew_plist(candidate, name);
            if (spec) {
                SPDLOG_DEBUG("supervisor: imported spec for {} from {}", name, candidate.string());
                return spec;
            }
        }
    }
    return std::nullopt;
#endif
}

} // namespace

// ---- Public API: paths ----------------------------------------------------

fs::path services_root(const Config& config) {
    return config.den_home / "services";
}

fs::path service_dir(const Config& config, const std::string& name) {
    return services_root(config) / name;
}

fs::path service_log_path(const Config& config, const std::string& name) {
    return service_dir(config, name) / "log";
}

// ---- Public API: restart policy strings -----------------------------------

std::string restart_policy_to_string(RestartPolicy p) {
    switch (p) {
    case RestartPolicy::Always:
        return "always";
    case RestartPolicy::OnFailure:
        return "on-failure";
    case RestartPolicy::Never:
        return "never";
    }
    return "never";
}

std::optional<RestartPolicy> restart_policy_from_string(const std::string& s) {
    if (s == "always")
        return RestartPolicy::Always;
    if (s == "on-failure" || s == "on_failure" || s == "onfailure")
        return RestartPolicy::OnFailure;
    if (s == "never")
        return RestartPolicy::Never;
    return std::nullopt;
}

// ---- Public API: spec I/O -------------------------------------------------

std::optional<ServiceSpec> load_spec(const Config& config, const std::string& name) {
    std::ifstream f(spec_path(config, name));
    if (!f.is_open())
        return std::nullopt;
    try {
        nlohmann::json j;
        f >> j;
        ServiceSpec spec;
        spec.name = name;
        if (j.contains("name") && j["name"].is_string())
            spec.name = j["name"].get<std::string>();
        if (j.contains("argv") && j["argv"].is_array()) {
            for (const auto& v : j["argv"]) {
                if (v.is_string())
                    spec.argv.push_back(v.get<std::string>());
            }
        }
        if (j.contains("env") && j["env"].is_object()) {
            for (auto it = j["env"].begin(); it != j["env"].end(); ++it) {
                if (it.value().is_string())
                    spec.env.emplace_back(it.key(), it.value().get<std::string>());
            }
        }
        if (j.contains("working_dir") && j["working_dir"].is_string())
            spec.working_dir = j["working_dir"].get<std::string>();
        if (j.contains("restart") && j["restart"].is_string()) {
            if (auto p = restart_policy_from_string(j["restart"].get<std::string>()))
                spec.restart = *p;
        }
        if (j.contains("depends_on") && j["depends_on"].is_array()) {
            for (const auto& v : j["depends_on"]) {
                if (v.is_string())
                    spec.depends_on.push_back(v.get<std::string>());
            }
        }
        if (spec.argv.empty())
            return std::nullopt;
        return spec;
    } catch (const std::exception& e) {
        SPDLOG_WARN("supervisor: failed to parse spec for {}: {}", name, e.what());
        return std::nullopt;
    }
}

void save_spec(const Config& config, const ServiceSpec& spec) {
    nlohmann::json j;
    j["name"] = spec.name;
    j["argv"] = spec.argv;
    nlohmann::json env = nlohmann::json::object();
    for (const auto& [k, v] : spec.env)
        env[k] = v;
    j["env"] = env;
    j["working_dir"] = spec.working_dir;
    j["restart"] = restart_policy_to_string(spec.restart);
    j["depends_on"] = spec.depends_on;

    std::error_code ec;
    fs::create_directories(service_dir(config, spec.name), ec);
    write_atomic(spec_path(config, spec.name), j.dump(2) + "\n");
}

std::optional<ServiceSpec> resolve_service_spec(const Config& config, const std::string& name) {
    if (auto s = load_spec(config, name))
        return s;
    if (auto s = import_from_homebrew_plist(config, name)) {
        s->name = name;
        save_spec(config, *s);
        return s;
    }
    return std::nullopt;
}

// ---- Public API: status ---------------------------------------------------

std::optional<ServiceStatus> service_status(const Config& config, const std::string& name) {
    auto dir = service_dir(config, name);
    if (!fs::exists(dir))
        return std::nullopt;

    ServiceStatus st;
    st.name = name;

    if (auto p = read_pid_file(sup_pid_path(config, name))) {
        st.sup_pid = *p;
        st.running = pid_alive(*p);
    }
    if (auto p = read_pid_file(svc_pid_path(config, name))) {
        if (pid_alive(*p))
            st.svc_pid = *p;
    }

    auto rs = read_runtime_state(config, name);
    st.start_time = rs.start_time;
    st.last_exit = rs.last_exit;
    st.restart_count = rs.restart_count;

    if (auto spec = load_spec(config, name))
        st.restart = spec->restart;

    return st;
}

std::vector<ServiceStatus> list_services(const Config& config) {
    std::vector<ServiceStatus> out;
    auto root = services_root(config);
    if (!fs::exists(root))
        return out;
    for (auto& e : fs::directory_iterator(root)) {
        if (!e.is_directory())
            continue;
        auto name = e.path().filename().string();
        if (auto st = service_status(config, name))
            out.push_back(std::move(*st));
    }
    std::sort(out.begin(), out.end(),
              [](const ServiceStatus& a, const ServiceStatus& b) { return a.name < b.name; });
    return out;
}

// ---- Supervisor inner loop (runs in the orphaned grandchild) --------------

int run_supervisor_loop(const Config& config, const ServiceSpec& spec) {
    auto dir = service_dir(config, spec.name);
    std::error_code ec;
    fs::create_directories(dir, ec);

    // Open log file; redirect stdin from /dev/null and stdout+stderr to log.
    auto log = dir / "log";
    int log_fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd >= 0) {
        ::dup2(log_fd, STDOUT_FILENO);
        ::dup2(log_fd, STDERR_FILENO);
        ::close(log_fd);
    }
    int devnull = ::open("/dev/null", O_RDONLY);
    if (devnull >= 0) {
        ::dup2(devnull, STDIN_FILENO);
        ::close(devnull);
    }

    // Open sup.pid with flock for liveness signal. Held until exit.
    auto sup_path = sup_pid_path(config, spec.name);
    int sup_fd = ::open(sup_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (sup_fd < 0) {
        SPDLOG_ERROR("supervisor: cannot open sup.pid {}: {}", sup_path.string(),
                     std::strerror(errno));
        return 1;
    }
    if (::flock(sup_fd, LOCK_EX | LOCK_NB) != 0) {
        SPDLOG_ERROR("supervisor: another supervisor already holds {}", sup_path.string());
        ::close(sup_fd);
        return 1;
    }
    std::string pid_str = std::to_string(::getpid()) + "\n";
    (void)::write(sup_fd, pid_str.c_str(), pid_str.size());

    // chdir: working_dir if set, else /.
    if (!spec.working_dir.empty()) {
        if (::chdir(spec.working_dir.c_str()) != 0)
            SPDLOG_WARN("supervisor: chdir({}) failed: {}", spec.working_dir, std::strerror(errno));
    } else {
        (void)::chdir("/");
    }

    // SIGTERM handler.
    struct sigaction sa{};
    sa.sa_handler = supervisor_sigterm;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGINT, &sa, nullptr);

    ServiceRuntimeState state = read_runtime_state(config, spec.name);
    state.start_time = now_secs();
    state.restart_count = 0;
    state.last_exit.reset();
    write_runtime_state(config, spec.name, state);

    uint32_t backoff_ms = BACKOFF_INITIAL_MS;
    int exit_code = 0;

    while (!g_stop) {
        // ---- Spawn service child ----
        pid_t svc = ::fork();
        if (svc < 0) {
            SPDLOG_ERROR("supervisor: fork failed for {}: {}", spec.name, std::strerror(errno));
            if (sleep_with_interrupt(backoff_ms))
                break;
            continue;
        }
        if (svc == 0) {
            // Child: reset signal handlers, apply env, exec.
            ::signal(SIGTERM, SIG_DFL);
            ::signal(SIGINT, SIG_DFL);
            for (const auto& [k, v] : spec.env)
                ::setenv(k.c_str(), v.c_str(), 1);
            std::vector<char*> argv;
            argv.reserve(spec.argv.size() + 1);
            for (const auto& a : spec.argv)
                argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            ::execvp(argv[0], argv.data());
            std::fprintf(stderr, "supervisor: exec %s failed: %s\n", argv[0], std::strerror(errno));
            ::_exit(127);
        }

        // Parent (supervisor): record service PID.
        write_atomic(svc_pid_path(config, spec.name), std::to_string(svc) + "\n");
        uint64_t svc_started = now_secs();
        SPDLOG_DEBUG("supervisor: {} service pid {} (policy {})", spec.name, svc,
                     restart_policy_to_string(spec.restart));

        // Wait for the service to exit, or for g_stop.
        int status = 0;
        bool reaped = false;
        while (!reaped) {
            if (g_stop) {
                // Forward TERM to service; wait up to 10s; then KILL.
                ::kill(svc, SIGTERM);
                for (int i = 0; i < 100; ++i) {
                    pid_t r = ::waitpid(svc, &status, WNOHANG);
                    if (r == svc) {
                        reaped = true;
                        break;
                    }
                    sleep_ms(100);
                }
                if (!reaped) {
                    ::kill(svc, SIGKILL);
                    ::waitpid(svc, &status, 0);
                    reaped = true;
                }
                break;
            }
            pid_t r = ::waitpid(svc, &status, WNOHANG);
            if (r == svc) {
                reaped = true;
                break;
            }
            if (r < 0 && errno != EINTR) {
                SPDLOG_WARN("supervisor: waitpid failed for {}: {}", spec.name,
                            std::strerror(errno));
                reaped = true;
                break;
            }
            sleep_ms(MONITOR_POLL_INTERVAL_MS);
        }

        // Service has exited (or we're stopping). Clear service PID.
        std::error_code rmec;
        fs::remove(svc_pid_path(config, spec.name), rmec);

        int last_exit = -1;
        if (WIFEXITED(status))
            last_exit = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            last_exit = 128 + WTERMSIG(status);
        state.last_exit = last_exit;
        write_runtime_state(config, spec.name, state);

        if (g_stop)
            break;

        // Reset backoff if the service ran a long time.
        if (now_secs() - svc_started > LONG_RUN_RESET_SECS)
            backoff_ms = BACKOFF_INITIAL_MS;

        bool should_restart = false;
        switch (spec.restart) {
        case RestartPolicy::Always:
            should_restart = true;
            break;
        case RestartPolicy::OnFailure:
            should_restart = (last_exit != 0);
            break;
        case RestartPolicy::Never:
            should_restart = false;
            break;
        }

        if (!should_restart)
            break;

        ++state.restart_count;
        write_runtime_state(config, spec.name, state);
        if (sleep_with_interrupt(backoff_ms))
            break;
        backoff_ms = std::min(backoff_ms * 2, BACKOFF_CAP_MS);
    }

    // Cleanup.
    std::error_code rmec;
    fs::remove(svc_pid_path(config, spec.name), rmec);
    ::flock(sup_fd, LOCK_UN);
    ::close(sup_fd);
    fs::remove(sup_path, rmec);
    return exit_code;
}

// ---- Public API: start ----------------------------------------------------

bool start_service(const Config& config, const ServiceSpec& spec) {
    std::error_code ec;
    fs::create_directories(service_dir(config, spec.name), ec);

    // Refuse to start a duplicate.
    if (auto pid = read_pid_file(sup_pid_path(config, spec.name))) {
        if (pid_alive(*pid)) {
            SPDLOG_WARN("services: '{}' is already running (sup pid {})", spec.name, *pid);
            return false;
        }
    }
    // Remove stale pid files.
    fs::remove(sup_pid_path(config, spec.name), ec);
    fs::remove(svc_pid_path(config, spec.name), ec);

    // Persist spec.
    save_spec(config, spec);

    // ---- Double-fork dance ----
    pid_t mid = ::fork();
    if (mid < 0) {
        SPDLOG_ERROR("services: fork failed: {}", std::strerror(errno));
        return false;
    }
    if (mid == 0) {
        // Intermediate child.
        if (::setsid() < 0)
            ::_exit(1);
        pid_t grand = ::fork();
        if (grand < 0)
            ::_exit(1);
        if (grand > 0)
            ::_exit(0);
        // Grandchild: become the supervisor.
        int rc = run_supervisor_loop(config, spec);
        ::_exit(rc);
    }

    // Parent: reap the intermediate child.
    int status = 0;
    ::waitpid(mid, &status, 0);

    // Poll for sup.pid to appear.
    auto sup_path = sup_pid_path(config, spec.name);
    for (uint32_t waited = 0; waited < START_POLL_TIMEOUT_MS; waited += START_POLL_INTERVAL_MS) {
        if (auto pid = read_pid_file(sup_path)) {
            if (pid_alive(*pid))
                return true;
        }
        sleep_ms(START_POLL_INTERVAL_MS);
    }
    SPDLOG_ERROR("services: supervisor for '{}' did not signal startup", spec.name);
    return false;
}

// ---- Public API: stop -----------------------------------------------------

bool stop_service(const Config& config, const std::string& name, uint32_t timeout_secs) {
    auto sup_path = sup_pid_path(config, name);
    auto sup = read_pid_file(sup_path);
    if (!sup || !pid_alive(*sup)) {
        // Already stopped. Clean stale pid files defensively.
        std::error_code ec;
        fs::remove(sup_path, ec);
        fs::remove(svc_pid_path(config, name), ec);
        return true;
    }

    if (::kill(static_cast<pid_t>(*sup), SIGTERM) != 0) {
        SPDLOG_WARN("services: SIGTERM to supervisor of '{}' failed: {}", name,
                    std::strerror(errno));
    }

    uint32_t waited_ms = 0;
    uint32_t timeout_ms = timeout_secs * 1000;
    while (waited_ms < timeout_ms) {
        if (!pid_alive(*sup))
            return true;
        sleep_ms(STOP_POLL_INTERVAL_MS);
        waited_ms += STOP_POLL_INTERVAL_MS;
    }

    // Escalate.
    SPDLOG_WARN("services: supervisor of '{}' did not stop within {}s, sending SIGKILL", name,
                timeout_secs);
    ::kill(static_cast<pid_t>(*sup), SIGKILL);
    sleep_ms(200);
    std::error_code ec;
    fs::remove(sup_path, ec);
    fs::remove(svc_pid_path(config, name), ec);
    return !pid_alive(*sup);
}

// Topologically order `names` so that dependents come BEFORE their
// dependencies (suitable for stopping). Cycles fall back to input order.
// Topologically order `names` so that dependents come BEFORE their
// dependencies (suitable for stopping). Edges: A → B when A depends_on B,
// meaning A must stop before B. Cycles fall back to input order.
std::vector<std::string> topo_stop_order(const Config& config,
                                         const std::vector<std::string>& names) {
    auto index_of = [&](const std::string& n) -> int {
        for (size_t i = 0; i < names.size(); ++i)
            if (names[i] == n)
                return static_cast<int>(i);
        return -1;
    };

    std::vector<std::vector<int>> edges(names.size());
    for (size_t i = 0; i < names.size(); ++i) {
        auto spec = load_spec(config, names[i]);
        if (!spec)
            continue;
        for (const auto& dep : spec->depends_on) {
            int j = index_of(dep);
            if (j >= 0)
                edges[i].push_back(j);
        }
    }

    std::vector<int> indeg(names.size(), 0);
    for (size_t i = 0; i < names.size(); ++i)
        for (int j : edges[i])
            ++indeg[j];

    std::vector<int> q;
    for (size_t i = 0; i < names.size(); ++i)
        if (indeg[i] == 0)
            q.push_back(static_cast<int>(i));

    std::vector<std::string> order;
    while (!q.empty()) {
        int i = q.back();
        q.pop_back();
        order.push_back(names[i]);
        for (int j : edges[i]) {
            if (--indeg[j] == 0)
                q.push_back(j);
        }
    }

    // Cycle / leftover: append in input order.
    if (order.size() < names.size()) {
        for (size_t i = 0; i < names.size(); ++i) {
            if (std::find(order.begin(), order.end(), names[i]) == order.end())
                order.push_back(names[i]);
        }
    }
    return order;
}

std::vector<std::string> stop_services_ordered(const Config& config,
                                               const std::vector<std::string>& names,
                                               uint32_t timeout_secs) {
    std::vector<std::string> stopped;
    auto order = topo_stop_order(config, names);
    for (const auto& name : order) {
        if (stop_service(config, name, timeout_secs))
            stopped.push_back(name);
    }
    return stopped;
}

// ---- Public API: logs -----------------------------------------------------

void cat_service_log(const Config& config, const std::string& name) {
    std::ifstream f(service_log_path(config, name));
    if (!f.is_open())
        return;
    std::cout << f.rdbuf();
}

void follow_service_log(const Config& config, const std::string& name) {
    auto path = service_log_path(config, name);
    std::ifstream f(path);
    if (!f.is_open()) {
        SPDLOG_ERROR("services: no log file for '{}' at {}", name, path.string());
        return;
    }
    std::cout << f.rdbuf();
    std::cout.flush();

    // Install interrupt handler so SIGINT/SIGTERM bail out cleanly.
    g_stop = 0;
    struct sigaction sa{};
    sa.sa_handler = supervisor_sigterm;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);

    f.clear();
    while (!g_stop) {
        if (!f) {
            f.clear();
            sleep_ms(200);
            continue;
        }
        std::string line;
        while (std::getline(f, line)) {
            std::cout << line << '\n';
        }
        std::cout.flush();
        sleep_ms(200);
    }
}

} // namespace den
