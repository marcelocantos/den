// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "daemon.h"
#include "../activity/activity.h"
#include "../core/error.h"
#include "../core/version.h"
#include "../env/environment.h"
#include "../env/manifest.h"
#include "../index/index.h"
#include "../provider/homebrew_provider.h"
#include "../provider/registry.h"
#include "../store/pins.h"
#include "../store/store.h"
#include "upgrade_pass.h"

#include <algorithm>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <unistd.h>

namespace den {

// ---- Constants -------------------------------------------------------------

static constexpr uint64_t DEFAULT_INTERVAL_SECS = 21600; // 6 hours

// ---- Internal state --------------------------------------------------------

namespace {

// Volatile flag set by the SIGTERM handler.
volatile sig_atomic_t g_terminate = 0;

void sigterm_handler(int /*sig*/) {
    g_terminate = 1;
}

// Paths.
fs::path pid_path(const fs::path& den_home) {
    return den_home / "daemon.pid";
}

fs::path state_path(const fs::path& den_home) {
    return den_home / "daemon_state.json";
}

fs::path log_path(const fs::path& den_home) {
    return den_home / "daemon.log";
}

// XML-escape a string for interpolation into plist XML attribute values or
// character data (escapes &, <, >, ", ').
std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        case '\'':
            out += "&apos;";
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}

// Read daemon settings from den_home/config.json (falls back to defaults).
struct DaemonSettings {
    bool auto_download = true;
    bool auto_upgrade = false;
    std::string upgrade_window; // "HH:MM-HH:MM" or empty
    uint64_t interval_secs = DEFAULT_INTERVAL_SECS;
};

DaemonSettings read_daemon_settings(const fs::path& den_home) {
    DaemonSettings s;
    fs::path cfg = den_home / "config.json";
    try {
        std::ifstream f(cfg);
        if (!f.is_open())
            return s;
        nlohmann::json j;
        f >> j;
        if (j.contains("daemon")) {
            const auto& d = j["daemon"];
            auto as_bool = [](const nlohmann::json& v, bool* out) {
                if (v.is_boolean()) {
                    *out = v.get<bool>();
                    return true;
                }
                if (v.is_string()) {
                    if (v.get<std::string>() == "true") {
                        *out = true;
                        return true;
                    }
                    if (v.get<std::string>() == "false") {
                        *out = false;
                        return true;
                    }
                }
                return false;
            };
            if (d.contains("auto_download"))
                as_bool(d["auto_download"], &s.auto_download);
            if (d.contains("auto_upgrade"))
                as_bool(d["auto_upgrade"], &s.auto_upgrade);
            if (d.contains("upgrade_window") && d["upgrade_window"].is_string())
                s.upgrade_window = d["upgrade_window"].get<std::string>();
            if (d.contains("interval_secs") && d["interval_secs"].is_number_unsigned())
                s.interval_secs = d["interval_secs"].get<uint64_t>();
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("daemon: failed to read settings from {}: {}", cfg.string(), e.what());
    }
    return s;
}

} // anonymous namespace

// ---- Utilities -------------------------------------------------------------

uint64_t now_secs() {
    return static_cast<uint64_t>(std::time(nullptr));
}

bool in_maintenance_window(const std::string& window) {
    if (window.empty())
        return true;

    // Parse "HH:MM-HH:MM".
    int start_h = -1, start_m = -1, end_h = -1, end_m = -1;
    char sep1 = 0, sep2 = 0, sep3 = 0;
    std::istringstream ss(window);
    ss >> start_h >> sep1 >> start_m >> sep2 >> end_h >> sep3 >> end_m;
    if (ss.fail() || sep1 != ':' || sep2 != '-' || sep3 != ':') {
        SPDLOG_WARN("daemon: invalid maintenance window '{}', treating as always-open", window);
        return true;
    }
    if (start_h < 0 || start_h > 23 || start_m < 0 || start_m > 59 || end_h < 0 || end_h > 23 ||
        end_m < 0 || end_m > 59) {
        SPDLOG_WARN("daemon: out-of-range maintenance window '{}', treating as always-open",
                    window);
        return true;
    }

    std::time_t t = std::time(nullptr);
    std::tm* lt = std::localtime(&t);
    if (!lt)
        return true;

    int now_mins = lt->tm_hour * 60 + lt->tm_min;
    int start_mins = start_h * 60 + start_m;
    int end_mins = end_h * 60 + end_m;

    if (start_mins <= end_mins) {
        return now_mins >= start_mins && now_mins < end_mins;
    } else {
        // Window wraps midnight.
        return now_mins >= start_mins || now_mins < end_mins;
    }
}

std::string launchd_plist(const fs::path& den_binary, const fs::path& den_home,
                          const fs::path& prefix) {
    std::string bin = xml_escape(den_binary.string());
    std::string home = xml_escape(den_home.string());
    // Carry an explicit DEN_PREFIX into the agent so the daemon maintains
    // the same store as the shell that installed it (store isolation).
    std::string prefix_env;
    if (const char* dp = std::getenv("DEN_PREFIX"); dp && *dp) {
        prefix_env =
            "        <key>DEN_PREFIX</key>\n        <string>" + xml_escape(dp) + "</string>\n";
    }

    std::string path = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin";
    if (!prefix.empty()) {
        const auto prefix_bin = (prefix / "bin").lexically_normal().string();
        if (prefix_bin != "/opt/homebrew/bin" && prefix_bin != "/usr/local/bin") {
            path = prefix_bin + ":" + path;
        }
    }

    std::ostringstream o;
    o << R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
    "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>dev.den.daemon</string>

    <key>ProgramArguments</key>
    <array>
        <string>)"
      << bin << R"(</string>
        <string>daemon</string>
        <string>run</string>
    </array>

    <key>EnvironmentVariables</key>
    <dict>
        <key>DEN_HOME</key>
        <string>)"
      << home << R"(</string>
)" << prefix_env
      << R"(        <key>PATH</key>
        <string>)"
      << xml_escape(path) << R"(</string>
    </dict>

    <key>RunAtLoad</key>
    <true/>

    <key>KeepAlive</key>
    <true/>

    <key>StandardOutPath</key>
    <string>)"
      << home << R"(/daemon.log</string>

    <key>StandardErrorPath</key>
    <string>)"
      << home << R"(/daemon.log</string>
</dict>
</plist>
)";
    return o.str();
}

void log_daemon(const fs::path& den_home, const std::string& msg) {
    std::time_t t = std::time(nullptr);
    std::tm* lt = std::localtime(&t);
    char buf[32] = {};
    if (lt)
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", lt);

    std::ofstream f(log_path(den_home), std::ios::app);
    if (f) {
        f << '[' << buf << "] " << msg << '\n';
    }
}

// ---- State I/O -------------------------------------------------------------

DaemonState read_daemon_state(const fs::path& den_home) {
    DaemonState state;
    fs::path path = state_path(den_home);

    try {
        std::ifstream f(path);
        if (!f.is_open())
            return state;
        nlohmann::json j;
        f >> j;

        if (j.contains("last_check") && j["last_check"].is_number_unsigned())
            state.last_check = j["last_check"].get<uint64_t>();

        if (j.contains("last_upgrade") && j["last_upgrade"].is_number_unsigned())
            state.last_upgrade = j["last_upgrade"].get<uint64_t>();

        if (j.contains("errors") && j["errors"].is_array()) {
            for (const auto& e : j["errors"]) {
                if (e.is_string())
                    state.errors.push_back(e.get<std::string>());
            }
        }

        if (j.contains("pending") && j["pending"].is_array()) {
            for (const auto& p : j["pending"]) {
                if (!p.is_object())
                    continue;
                PendingUpgrade pu;
                if (p.contains("name") && p["name"].is_string())
                    pu.name = p["name"].get<std::string>();
                if (p.contains("installed") && p["installed"].is_string())
                    pu.installed = p["installed"].get<std::string>();
                if (p.contains("available") && p["available"].is_string())
                    pu.available = p["available"].get<std::string>();
                if (p.contains("installed_scheme") && p["installed_scheme"].is_number_integer())
                    pu.installed_scheme = p["installed_scheme"].get<int>();
                if (p.contains("available_scheme") && p["available_scheme"].is_number_integer())
                    pu.available_scheme = p["available_scheme"].get<int>();
                state.pending.push_back(std::move(pu));
            }
        }

        if (j.contains("index_older") && j["index_older"].is_array()) {
            for (const auto& p : j["index_older"]) {
                if (!p.is_object())
                    continue;
                PendingUpgrade pu;
                if (p.contains("name") && p["name"].is_string())
                    pu.name = p["name"].get<std::string>();
                if (p.contains("installed") && p["installed"].is_string())
                    pu.installed = p["installed"].get<std::string>();
                if (p.contains("available") && p["available"].is_string())
                    pu.available = p["available"].get<std::string>();
                if (p.contains("installed_scheme") && p["installed_scheme"].is_number_integer())
                    pu.installed_scheme = p["installed_scheme"].get<int>();
                if (p.contains("available_scheme") && p["available_scheme"].is_number_integer())
                    pu.available_scheme = p["available_scheme"].get<int>();
                state.index_older.push_back(std::move(pu));
            }
        }

        if (j.contains("deferred") && j["deferred"].is_array()) {
            for (const auto& d : j["deferred"]) {
                if (!d.is_object())
                    continue;
                DeferredUpgrade du;
                if (d.contains("name") && d["name"].is_string())
                    du.name = d["name"].get<std::string>();
                if (d.contains("installed") && d["installed"].is_string())
                    du.installed = d["installed"].get<std::string>();
                if (d.contains("available") && d["available"].is_string())
                    du.available = d["available"].get<std::string>();
                if (d.contains("reason") && d["reason"].is_string())
                    du.reason = d["reason"].get<std::string>();
                state.deferred.push_back(std::move(du));
            }
        }
    } catch (const std::exception& e) {
        SPDLOG_WARN("daemon: failed to read state from {}: {}", path.string(), e.what());
    }
    return state;
}

void write_daemon_state(const fs::path& den_home, const DaemonState& state) {
    nlohmann::json j;
    if (state.last_check)
        j["last_check"] = *state.last_check;
    if (state.last_upgrade)
        j["last_upgrade"] = *state.last_upgrade;

    j["errors"] = state.errors;

    nlohmann::json pending = nlohmann::json::array();
    for (const auto& p : state.pending) {
        pending.push_back({{"name", p.name},
                           {"installed", p.installed},
                           {"available", p.available},
                           {"installed_scheme", p.installed_scheme},
                           {"available_scheme", p.available_scheme}});
    }
    j["pending"] = pending;

    nlohmann::json older = nlohmann::json::array();
    for (const auto& p : state.index_older) {
        older.push_back({{"name", p.name},
                         {"installed", p.installed},
                         {"available", p.available},
                         {"installed_scheme", p.installed_scheme},
                         {"available_scheme", p.available_scheme}});
    }
    j["index_older"] = older;

    nlohmann::json deferred = nlohmann::json::array();
    for (const auto& d : state.deferred) {
        deferred.push_back({{"name", d.name},
                            {"installed", d.installed},
                            {"available", d.available},
                            {"reason", d.reason}});
    }
    j["deferred"] = deferred;

    // Atomic write: write to .tmp then rename.
    fs::path tmp = state_path(den_home).string() + ".tmp";
    try {
        fs::create_directories(den_home);
        {
            std::ofstream f(tmp);
            if (!f)
                throw UserError("daemon: cannot open " + tmp.string() + " for writing");
            f << j.dump(2) << '\n';
        }
        fs::rename(tmp, state_path(den_home));
    } catch (const std::exception& e) {
        SPDLOG_ERROR("daemon: failed to write state: {}", e.what());
        std::error_code ec;
        fs::remove(tmp, ec);
    }
}

// ---- Lifecycle -------------------------------------------------------------

// ---- Maintenance cycle (🎯T13) ---------------------------------------------

std::vector<PendingUpgrade> compute_pending(const std::map<std::string, std::string>& installed,
                                            const PackageIndex& idx,
                                            std::vector<PendingUpgrade>* index_older,
                                            const InstalledSchemeFn& installed_scheme) {
    std::vector<PendingUpgrade> out;
    if (index_older)
        index_older->clear();
    for (const auto& [name, version] : installed) {
        const Package* pkg = idx.find(name);
        if (!pkg || pkg->version.empty() || pkg->version == version)
            continue;
        if (pkg->artifact_type == ArtifactType::App || !pkg->cask_artifacts.empty())
            continue; // cask upgrades stay manual
        if (pkg->disabled)
            continue;
        const int have_scheme = installed_scheme ? installed_scheme(name, version) : 0;
        const PendingUpgrade pu{name, version, pkg->version, have_scheme, pkg->version_scheme};
        switch (version_delta(version, have_scheme, pkg->version, pkg->version_scheme)) {
        case VersionDelta::Newer:
            // Includes a version_scheme bump to a lower version (openssl@3
            // 3.6.x -> 3.5.9 LTS, scheme 0 -> 1): brew treats it as an upgrade.
            out.push_back(pu);
            break;
        case VersionDelta::Older:
            // Never downgrade from an upgrade path: the index can lag what is
            // installed (a stale cache or mirror, or an index that predates a
            // version_scheme bump). Report it.
            if (index_older)
                index_older->push_back(pu);
            break;
        case VersionDelta::Same:
            break;
        }
    }
    return out; // std::map iteration → already sorted by name
}

std::string index_older_message(const PendingUpgrade& p) {
    return p.name + " " + p.installed + ": index older than installed (index has " + p.available +
           "); not downgrading. To downgrade explicitly: den use " + p.name + " " + p.available;
}

MaintenanceReport run_maintenance_cycle(const fs::path& cellar, const fs::path& den_home,
                                        const MaintenanceSettings& settings, DaemonState& state,
                                        const MaintenanceHooks& hooks) {
    MaintenanceReport report;
    state.errors.clear();
    state.last_check = now_secs();

    PackageIndex idx;
    try {
        idx = hooks.refresh_index();
        report.index_refreshed = true;
    } catch (const std::exception& e) {
        state.errors.push_back(std::string("index refresh: ") + e.what());
        log_daemon(den_home, std::string("index refresh failed: ") + e.what());
        report.pending = state.pending.size();
        return report; // keep the previous pending list; retry next cycle
    }

    try {
        state.pending =
            compute_pending(hooks.installed(), idx, &state.index_older, hooks.installed_scheme);
    } catch (const std::exception& e) {
        state.errors.push_back(std::string("outdated check: ") + e.what());
        report.pending = state.pending.size();
        return report;
    }
    // A deferral only makes sense while its package is still pending.
    std::erase_if(state.deferred, [&](const DeferredUpgrade& d) {
        return std::none_of(state.pending.begin(), state.pending.end(),
                            [&](const PendingUpgrade& p) { return p.name == d.name; });
    });
    report.pending = state.pending.size();
    report.index_older = state.index_older.size();
    log_daemon(den_home, std::to_string(report.pending) + " package(s) outdated");
    for (const auto& o : state.index_older)
        log_daemon(den_home, index_older_message(o));

    if (settings.auto_download && hooks.prefetch) {
        // A pinned package is never upgraded (the upgrade pass and `den
        // upgrade` both hold it), so pre-downloading its bottle only wastes
        // bandwidth and cache space. Pins are read like the upgrade pass
        // reads them: an unreadable pins.json fails closed and skips every
        // pre-download rather than treating everything as unpinned.
        PinMap pins;
        bool pins_ok = true;
        try {
            pins = read_pins(den_home);
        } catch (const std::exception& e) {
            pins_ok = false;
            state.errors.push_back(
                std::string("download: pins unreadable, skipping pre-download: ") + e.what());
            log_daemon(den_home,
                       std::string("pins unreadable, skipping pre-download: ") + e.what());
        }
        std::size_t wanted = 0;
        for (const auto& pu : state.pending) {
            if (!pins_ok || pins.count(pu.name)) {
                ++report.skipped_pinned;
                continue;
            }
            ++wanted;
            try {
                hooks.prefetch(pu);
                ++report.downloaded;
            } catch (const std::exception& e) {
                state.errors.push_back("download " + pu.name + ": " + e.what());
            }
        }
        if (!state.pending.empty()) {
            std::string msg = "pre-downloaded " + std::to_string(report.downloaded) + "/" +
                              std::to_string(wanted) + " upgrade(s)";
            if (report.skipped_pinned > 0)
                msg += ", skipped " + std::to_string(report.skipped_pinned) + " pinned";
            log_daemon(den_home, msg);
        }
    }

    if (settings.auto_upgrade && !state.pending.empty()) {
        bool in_window = settings.upgrade_window.empty() ||
                         (hooks.in_window ? hooks.in_window(settings.upgrade_window)
                                          : in_maintenance_window(settings.upgrade_window));
        if (in_window) {
            UpgradePassResult res = run_upgrade_pass(cellar, den_home, state, hooks.apply);
            report.applied = res.applied.size();
            report.deferred = res.deferred.size();
            log_daemon(den_home, "upgrade pass: applied " + std::to_string(report.applied) +
                                     ", deferred " + std::to_string(report.deferred));
        } else {
            log_daemon(den_home,
                       "outside upgrade window '" + settings.upgrade_window + "', not upgrading");
        }
    }
    return report;
}

namespace {

// Real hooks for the running daemon. Only den-managed homebrew packages in
// the active env are considered — Homebrew's own kegs and shared opt links
// are never touched (pours bind to exact kegs, see relocate.cpp / #64).
MaintenanceHooks make_live_hooks(const Config& config, PackageIndex& idx, bool& applied_any) {
    MaintenanceHooks h;
    h.refresh_index = [&config, &idx] {
        idx = fetch_and_transform(config);
        save_index(idx, config.cache / "index.json");
        return idx;
    };
    h.installed = [&config] {
        auto per = resolve_per_provider(config.den_home, active_env_path(config.den_home));
        auto it = per.find("homebrew");
        return it == per.end() ? std::map<std::string, std::string>{} : it->second;
    };
    h.installed_scheme = [&config](const std::string& name, const std::string& version) {
        return installed_version_scheme(config.store / keg_name(name) / version);
    };
    h.prefetch = [&config, &idx](const PendingUpgrade& pu) {
        if (const Package* pkg = idx.find(pu.name))
            prefetch_bottle(config, *pkg);
    };
    h.apply = [&config, &idx, &applied_any](const PendingUpgrade& pu) {
        auto registry = make_default_registry(&idx);
        auto* provider = registry.find("homebrew");
        if (!provider)
            throw UserError("homebrew provider not registered");
        provider->install(config, pu.name, pu.available);
        with_manifest(config.den_home, active_env_path(config.den_home),
                      [&](Manifest& m) { m.packages["homebrew"][pu.name] = pu.available; });
        applied_any = true;
    };
    return h;
}

} // namespace

void run_daemon(const Config& config, bool once) {
    const fs::path& den_home = config.den_home;
    fs::create_directories(den_home);

    // ---- PID file + exclusive flock ----------------------------------------
    fs::path pid_file = pid_path(den_home);
    int pid_fd = ::open(pid_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (pid_fd < 0) {
        throw UserError("daemon: cannot open PID file " + pid_file.string());
    }
    if (::flock(pid_fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(pid_fd);
        throw UserError("daemon: another instance is already running (flock failed)");
    }

    // Write PID.
    std::string pid_str = std::to_string(::getpid()) + "\n";
    (void)::write(pid_fd, pid_str.c_str(), pid_str.size());
    // Keep pid_fd open (holds the lock) until we exit.

    // ---- SIGTERM handler ---------------------------------------------------
    struct sigaction sa {};
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);

    SPDLOG_INFO("daemon: started (pid {})", ::getpid());
    log_daemon(den_home, std::string("daemon started (pid ") + std::to_string(::getpid()) +
                             ", den " + DEN_VERSION + ")");

    // ---- Main loop ---------------------------------------------------------
    while (!g_terminate) {
        DaemonSettings settings = read_daemon_settings(den_home);
        uint64_t interval =
            settings.interval_secs > 0 ? settings.interval_secs : DEFAULT_INTERVAL_SECS;

        SPDLOG_DEBUG("daemon: sleeping {}s", interval);

        // Sleep in 1-second ticks so we wake quickly on SIGTERM.
        for (uint64_t i = 0; !once && i < interval && !g_terminate; ++i) {
            ::sleep(1);
        }
        if (g_terminate)
            break;

        // Re-read settings after the sleep (they may have changed).
        settings = read_daemon_settings(den_home);

        SPDLOG_INFO("daemon: running maintenance cycle");
        log_daemon(den_home, "maintenance cycle start");

        DaemonState state = read_daemon_state(den_home);
        MaintenanceSettings ms{settings.auto_download, settings.auto_upgrade,
                               settings.upgrade_window};
        PackageIndex idx;
        bool applied_any = false;
        MaintenanceReport rep = run_maintenance_cycle(config.store, den_home, ms, state,
                                                      make_live_hooks(config, idx, applied_any));
        if (applied_any) {
            try {
                materialise(den_home, config.store, active_env_path(den_home), &idx);
            } catch (const std::exception& e) {
                state.errors.push_back(std::string("materialise: ") + e.what());
            }
        }
        SPDLOG_INFO("daemon: cycle done (pending {}, downloaded {}, skipped pinned {}, applied {}, "
                    "deferred {})",
                    rep.pending, rep.downloaded, rep.skipped_pinned, rep.applied, rep.deferred);

        write_daemon_state(den_home, state);
        log_daemon(den_home, "maintenance cycle complete");
        if (once)
            break;
    }

    SPDLOG_INFO("daemon: shutting down");
    log_daemon(den_home, "daemon shutting down");

    // Release lock and clean up PID file.
    ::flock(pid_fd, LOCK_UN);
    ::close(pid_fd);
    std::error_code ec;
    fs::remove(pid_file, ec);
}

void stop_daemon(const Config& config) {
    fs::path pid_file = pid_path(config.den_home);

    std::ifstream f(pid_file);
    if (!f.is_open()) {
        throw UserError("daemon: PID file not found — is the daemon running?");
    }

    pid_t pid = 0;
    f >> pid;
    if (pid <= 0) {
        throw UserError("daemon: invalid PID in " + pid_file.string());
    }

    if (::kill(pid, SIGTERM) != 0) {
        throw UserError("daemon: failed to signal process " + std::to_string(pid) +
                        " — it may have already exited");
    }
    SPDLOG_INFO("daemon: sent SIGTERM to pid {}", pid);
}

bool is_daemon_running(const Config& config) {
    fs::path pid_file = pid_path(config.den_home);

    int fd = ::open(pid_file.c_str(), O_WRONLY);
    if (fd < 0)
        return false; // No PID file → not running.

    // If we can acquire an exclusive lock, the daemon is not holding one.
    bool locked = (::flock(fd, LOCK_EX | LOCK_NB) == 0);
    if (locked)
        ::flock(fd, LOCK_UN);
    ::close(fd);

    return !locked; // Daemon is running iff we could NOT get the lock.
}

} // namespace den
