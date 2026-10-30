// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

#include "plist_refresh.h"

#include "../selfupdate/launchd_restart.h"
#include "daemon.h"

#include <cstdlib>
#include <exception>
#include <fstream>
#include <sstream>
#include <system_error>

namespace den {

namespace fs = std::filesystem;

namespace {

std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// A multi-line report as one daemon.log line.
std::string one_line(const std::string& text) {
    std::string line;
    std::istringstream in(text);
    for (std::string part; std::getline(in, part);) {
        const auto b = part.find_first_not_of(" \t");
        if (b == std::string::npos) {
            continue;
        }
        if (!line.empty()) {
            line += "; ";
        }
        line += part.substr(b);
    }
    return line;
}

} // namespace

DaemonPlistRefresh refresh_daemon_plist(const Config& cfg, const fs::path& plist,
                                        std::ostream& out) {
    DaemonPlistRefresh r;
    const auto text = read_file(plist);
    if (!text) {
        return r;
    }
    const auto home = plist_env_value(*text, "DEN_HOME");
    if (!home || fs::path(*home).lexically_normal() != cfg.den_home.lexically_normal()) {
        return r; // another den home's daemon
    }
    fs::path prefix = cfg.prefix;
    if (auto dp = plist_env_value(*text, "DEN_PREFIX"); dp && !dp->empty()) {
        prefix = *dp;
    } else if (cfg.prefix_source == PrefixSource::EnvDenPrefix) {
        // The daemon was installed without DEN_PREFIX; this process's
        // DEN_PREFIX says nothing about which prefix it maintains.
        return r;
    }
    r.ours = true;
    r.path = launchd_path(cfg.den_home, prefix);
    r.outcome =
        refresh_launchd_plist_path(plist, r.path, cfg.den_home / "dev.den.daemon.plist.bak", out);
    return r;
}

PlistPathRefresh refresh_daemon_plist_path(const Config& cfg, const fs::path& plist,
                                           std::ostream& out) {
    return refresh_daemon_plist(cfg, plist, out).outcome;
}

PlistPathRefresh refresh_daemon_plist_at_startup(const Config& cfg,
                                                 const DaemonStartupPlistEnv& env) {
    if (!env.plist) {
        return PlistPathRefresh::LeftAlone;
    }
    const auto log = [&env](const std::string& line) {
        if (env.log) {
            env.log(line);
        }
    };
    DaemonPlistRefresh r;
    std::ostringstream report;
    try {
        r = refresh_daemon_plist(cfg, *env.plist, report);
    } catch (const std::exception& e) {
        log(std::string("could not refresh the daemon plist's PATH (") + e.what() +
            "); run `den daemon install` to regenerate it");
        return PlistPathRefresh::LeftAlone;
    }
    if (!r.ours) {
        return r.outcome;
    }
    if (r.outcome == PlistPathRefresh::Rewritten) {
        log("daemon plist PATH refreshed: " + one_line(report.str()));
    } else if (r.outcome == PlistPathRefresh::LeftAlone) {
        log("daemon plist PATH is stale: " + one_line(report.str()));
        return r.outcome; // still stale: keep the PATH launchd gave us
    }
    if (env.inside_launchd_job && env.set_path) {
        env.set_path(r.path);
    }
    return r.outcome;
}

void refresh_daemon_plist_at_startup(const Config& cfg) {
#ifdef __APPLE__
    static const std::string kLabel = "dev.den.daemon";
    DaemonStartupPlistEnv env;
    const auto senv = scrubbed_env_for_current_user();
    if (!senv.home.empty()) {
        auto plist = fs::path(senv.home) / "Library" / "LaunchAgents" / (kLabel + ".plist");
        std::error_code ec;
        if (fs::is_regular_file(plist, ec)) {
            env.plist = plist;
        }
    }
    env.inside_launchd_job = running_inside_launchd_job(kLabel);
    env.log = [&cfg](const std::string& line) { log_daemon(cfg.den_home, line); };
    env.set_path = [](const std::string& path) { ::setenv("PATH", path.c_str(), 1); };
    refresh_daemon_plist_at_startup(cfg, env);
#else
    (void)cfg;
#endif
}

} // namespace den
