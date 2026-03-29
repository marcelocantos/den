// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "daemon.h"
#include "../core/error.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <csignal>
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
            if (d.contains("auto_download") && d["auto_download"].is_boolean())
                s.auto_download = d["auto_download"].get<bool>();
            if (d.contains("auto_upgrade") && d["auto_upgrade"].is_boolean())
                s.auto_upgrade = d["auto_upgrade"].get<bool>();
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

std::string launchd_plist(const fs::path& den_binary, const fs::path& den_home) {
    std::string bin = xml_escape(den_binary.string());
    std::string home = xml_escape(den_home.string());

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
        <string>--run</string>
        <string>--den-home</string>
        <string>)"
      << home << R"(</string>
    </array>

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
                state.pending.push_back(std::move(pu));
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
        pending.push_back(
            {{"name", p.name}, {"installed", p.installed}, {"available", p.available}});
    }
    j["pending"] = pending;

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

void run_daemon(const Config& config) {
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
    struct sigaction sa{};
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    ::sigaction(SIGTERM, &sa, nullptr);

    SPDLOG_INFO("daemon: started (pid {})", ::getpid());
    log_daemon(den_home, "daemon started (pid " + std::to_string(::getpid()) + ")");

    // ---- Main loop ---------------------------------------------------------
    while (!g_terminate) {
        DaemonSettings settings = read_daemon_settings(den_home);
        uint64_t interval =
            settings.interval_secs > 0 ? settings.interval_secs : DEFAULT_INTERVAL_SECS;

        SPDLOG_DEBUG("daemon: sleeping {}s", interval);

        // Sleep in 1-second ticks so we wake quickly on SIGTERM.
        for (uint64_t i = 0; i < interval && !g_terminate; ++i) {
            ::sleep(1);
        }
        if (g_terminate)
            break;

        // Re-read settings after the sleep (they may have changed).
        settings = read_daemon_settings(den_home);

        SPDLOG_INFO("daemon: running maintenance cycle");
        log_daemon(den_home, "maintenance cycle start");

        DaemonState state = read_daemon_state(den_home);
        state.errors.clear();
        state.last_check = now_secs();

        // TODO: Refresh package index (requires index module).
        SPDLOG_DEBUG("daemon: would refresh index here");

        // TODO: Check for outdated packages (requires keg + index modules).
        // Populate state.pending with discovered upgrades.
        SPDLOG_DEBUG("daemon: would check outdated packages here");

        // Pre-download bottles for pending upgrades.
        if (settings.auto_download && !state.pending.empty()) {
            SPDLOG_INFO("daemon: pre-downloading {} pending upgrade(s)", state.pending.size());
            log_daemon(den_home,
                       "pre-downloading " + std::to_string(state.pending.size()) + " upgrade(s)");
            // TODO: trigger bottle download for each pending upgrade.
        }

        // Apply upgrades if auto_upgrade is enabled and we are in the window.
        if (settings.auto_upgrade && !state.pending.empty()) {
            bool in_window = settings.upgrade_window.empty()
                                 ? true
                                 : in_maintenance_window(settings.upgrade_window);
            if (in_window) {
                SPDLOG_INFO("daemon: auto-upgrading {} package(s)", state.pending.size());
                log_daemon(den_home, "auto-upgrading " + std::to_string(state.pending.size()) +
                                         " package(s)");
                // TODO: apply upgrades, clear state.pending on success.
                state.last_upgrade = now_secs();
            } else {
                SPDLOG_DEBUG("daemon: outside maintenance window '{}', skipping upgrade",
                             settings.upgrade_window);
            }
        }

        write_daemon_state(den_home, state);
        log_daemon(den_home, "maintenance cycle complete");
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
