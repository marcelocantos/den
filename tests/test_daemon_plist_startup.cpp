// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// The daemon refreshes its own LaunchAgent PATH at startup, with the same
// rules as `den self-update` (daemon/plist_refresh.h), and switches to the
// corrected PATH for that run. An upgrade from a den that never refreshed the
// plist is then right in one step. Every startup ends with one daemon.log line
// giving the PATH the daemon actually runs with.

#include "../src/daemon/plist_refresh.h"

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace den {
namespace test_daemon_plist_startup {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_plist_startup_XXXXXX").string();
        char* result = ::mkdtemp(tmpl.data());
        if (!result)
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        path = result;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

bool has(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// A plist an older den wrote; `env_extra` goes into EnvironmentVariables,
// `extra` after it.
std::string old_plist(const fs::path& den_home, const std::string& path,
                      const std::string& env_extra = {}, const std::string& extra = {}) {
    return R"(<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>dev.den.daemon</string>
    <key>ProgramArguments</key>
    <array>
        <string>/x/den</string>
        <string>daemon</string>
        <string>run</string>
    </array>
    <key>EnvironmentVariables</key>
    <dict>
        <key>DEN_HOME</key>
        <string>)" +
           den_home.string() + "</string>\n" + env_extra + R"(        <key>PATH</key>
        <string>)" +
           path + R"(</string>
    </dict>
)" + extra +
           R"(    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
</dict>
</plist>
)";
}

const std::string kOldPath = "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin";

bool is_path_line(const std::string& line) {
    return line.rfind("daemon PATH:", 0) == 0 || line.rfind("daemon PATH (", 0) == 0;
}

// A daemon started by launchd from `plist`, recording what it logs and the
// PATH it switches to. `path` is its environment's PATH: launchd's (the
// plist's old one) until set_path changes it.
struct Startup {
    TempDir t;
    Config cfg;
    fs::path plist;
    fs::path bak;
    std::vector<std::string> log;
    std::vector<std::string> set_path;
    std::optional<std::string> path = kOldPath;
    DaemonStartupPlistEnv env;

    Startup() {
        cfg.den_home = t.path / ".den";
        cfg.prefix = cfg.den_home / "prefix";
        cfg.prefix_source = PrefixSource::DenHome;
        plist = t.path / "LaunchAgents" / "dev.den.daemon.plist";
        bak = cfg.den_home / "dev.den.daemon.plist.bak";
        env.plist = plist;
        env.inside_launchd_job = true;
        env.log = [this](const std::string& line) { log.push_back(line); };
        env.set_path = [this](const std::string& p) {
            set_path.push_back(p);
            path = p;
        };
        env.get_path = [this]() { return path; };
    }

    // What startup logged about the plist, without its closing PATH line.
    std::vector<std::string> plist_log() const {
        std::vector<std::string> out;
        for (const auto& line : log) {
            if (!is_path_line(line)) {
                out.push_back(line);
            }
        }
        return out;
    }

    // The closing PATH line: the last line logged, and the only PATH line.
    std::string path_line() const {
        int n = 0;
        for (const auto& line : log) {
            n += is_path_line(line) ? 1 : 0;
        }
        REQUIRE(n == 1);
        REQUIRE(!log.empty());
        REQUIRE(is_path_line(log.back()));
        return log.back();
    }

    std::string want() const { return launchd_path(cfg.den_home, cfg.prefix); }

    PlistPathRefresh run() { return refresh_daemon_plist_at_startup(cfg, env); }
};

TEST_SUITE("daemon_plist_startup") {

    TEST_CASE("startup rewrites a stale plist, saves a .bak and logs one line") {
        Startup s;
        const auto old = old_plist(s.cfg.den_home, kOldPath);
        put(s.plist, old);

        CHECK(s.run() == PlistPathRefresh::Rewritten);
        CHECK(plist_path_value(slurp(s.plist)) == s.want());
        REQUIRE(fs::exists(s.bak));
        CHECK(slurp(s.bak) == old);
        REQUIRE(s.plist_log().size() == 1);
        CHECK(has(s.plist_log()[0], "daemon plist PATH refreshed"));
        CHECK(has(s.plist_log()[0], kOldPath));
        CHECK(has(s.plist_log()[0], s.want()));
        CHECK(has(s.plist_log()[0], s.bak.string()));
        CHECK(!has(s.plist_log()[0], "\n"));
    }

    TEST_CASE("startup switches this run to the corrected PATH") {
        Startup s;
        put(s.plist, old_plist(s.cfg.den_home, kOldPath));
        s.run();
        CHECK(s.set_path == std::vector<std::string>{s.want()});
    }

    TEST_CASE("startup honours the plist's own DEN_PREFIX") {
        Startup s;
        put(s.plist,
            old_plist(s.cfg.den_home, kOldPath,
                      "        <key>DEN_PREFIX</key>\n        <string>/scratch/store</string>\n"));
        CHECK(s.run() == PlistPathRefresh::Rewritten);
        const auto want = launchd_path(s.cfg.den_home, "/scratch/store");
        CHECK(plist_path_value(slurp(s.plist)) == want);
        CHECK(s.set_path == std::vector<std::string>{want});
    }

    TEST_CASE("startup leaves another den home's plist alone") {
        Startup s;
        const auto other = old_plist(s.t.path / "other", kOldPath);
        put(s.plist, other);
        CHECK(s.run() == PlistPathRefresh::LeftAlone);
        CHECK(slurp(s.plist) == other);
        CHECK(!fs::exists(s.bak));
        CHECK(s.plist_log().empty());
        CHECK(s.set_path.empty());
    }

    TEST_CASE("startup leaves an unpinned plist alone when running with DEN_PREFIX") {
        Startup s;
        s.cfg.prefix_source = PrefixSource::EnvDenPrefix;
        const auto unpinned = old_plist(s.cfg.den_home, kOldPath);
        put(s.plist, unpinned);
        CHECK(s.run() == PlistPathRefresh::LeftAlone);
        CHECK(slurp(s.plist) == unpinned);
        CHECK(!fs::exists(s.bak));
        CHECK(s.plist_log().empty());
        CHECK(s.set_path.empty());
    }

    TEST_CASE("startup reports a stale hand-edited plist but does not rewrite it") {
        Startup s;
        const auto edited = old_plist(s.cfg.den_home, kOldPath, {},
                                      "    <key>Nice</key>\n    <integer>5</integer>\n");
        put(s.plist, edited);
        CHECK(s.run() == PlistPathRefresh::LeftAlone);
        CHECK(slurp(s.plist) == edited);
        CHECK(!fs::exists(s.bak));
        CHECK(s.set_path.empty()); // the plist is still stale: keep launchd's PATH
        REQUIRE(s.plist_log().size() == 1);
        CHECK(has(s.plist_log()[0], "Nice"));
        CHECK(has(s.plist_log()[0], "den daemon install"));
        CHECK(!has(s.plist_log()[0], "\n"));
    }

    TEST_CASE("startup is idempotent when the plist is already current") {
        Startup s;
        put(s.plist, old_plist(s.cfg.den_home, kOldPath));
        REQUIRE(s.run() == PlistPathRefresh::Rewritten);
        const auto after = slurp(s.plist);
        fs::remove(s.bak);
        s.log.clear();
        s.set_path.clear();

        CHECK(s.run() == PlistPathRefresh::UpToDate);
        CHECK(slurp(s.plist) == after);
        CHECK(!fs::exists(s.bak));
        CHECK(s.plist_log().empty());
        // Already-current plist: this run's PATH still comes from den's layout.
        CHECK(s.set_path == std::vector<std::string>{s.want()});
    }

    TEST_CASE("a daemon run by hand keeps its shell's PATH") {
        Startup s;
        s.env.inside_launchd_job = false;
        put(s.plist, old_plist(s.cfg.den_home, kOldPath));
        CHECK(s.run() == PlistPathRefresh::Rewritten);
        CHECK(plist_path_value(slurp(s.plist)) == s.want());
        CHECK(s.set_path.empty());
    }

    TEST_CASE("startup with no installed plist does nothing") {
        Startup s;
        s.env.plist.reset();
        CHECK(s.run() == PlistPathRefresh::LeftAlone);
        s.env.plist = s.plist; // configured but absent
        CHECK(s.run() == PlistPathRefresh::LeftAlone);
        CHECK(s.plist_log().empty());
        CHECK(s.set_path.empty());
    }

    TEST_CASE("self-update and startup share one refresh") {
        // Same plist, same outcome and same PATH from both entry points.
        Startup s;
        put(s.plist, old_plist(s.cfg.den_home, kOldPath));
        std::ostringstream out;
        CHECK(refresh_daemon_plist_path(s.cfg, s.plist, out) == PlistPathRefresh::Rewritten);
        const auto by_self_update = slurp(s.plist);
        put(s.plist, old_plist(s.cfg.den_home, kOldPath));
        CHECK(s.run() == PlistPathRefresh::Rewritten);
        CHECK(slurp(s.plist) == by_self_update);
    }

    TEST_CASE("startup logs the PATH it corrected in-process") {
        Startup s;
        put(s.plist, old_plist(s.cfg.den_home, kOldPath));
        CHECK(s.run() == PlistPathRefresh::Rewritten);
        REQUIRE(s.log.size() == 2);
        CHECK(has(s.log[0], "daemon plist PATH refreshed"));
        CHECK(s.path_line() == "daemon PATH (corrected in-process): " + s.want() +
                                   " (launchd gave " + kOldPath + ")");
    }

    TEST_CASE("startup corrects a stale launchd PATH even when the plist is already current") {
        // An older den rewrote the plist and restarted the job, but launchd
        // kept the job's old environment: nothing to rewrite, PATH corrected.
        Startup s;
        put(s.plist, old_plist(s.cfg.den_home, s.want()));
        CHECK(s.run() == PlistPathRefresh::UpToDate);
        REQUIRE(s.log.size() == 1);
        CHECK(s.path_line() == "daemon PATH (corrected in-process): " + s.want() +
                                   " (launchd gave " + kOldPath + ")");
    }

    TEST_CASE("startup logs an unchanged PATH when launchd's was already right") {
        Startup s;
        s.path = s.want();
        put(s.plist, old_plist(s.cfg.den_home, s.want()));
        CHECK(s.run() == PlistPathRefresh::UpToDate);
        REQUIRE(s.log.size() == 1);
        CHECK(s.path_line() == "daemon PATH: " + s.want());
        CHECK(s.path == s.want());
    }

    TEST_CASE("startup logs launchd's PATH when it leaves the plist alone") {
        Startup s;
        put(s.plist, old_plist(s.cfg.den_home, kOldPath, {},
                               "    <key>Nice</key>\n    <integer>5</integer>\n"));
        CHECK(s.run() == PlistPathRefresh::LeftAlone);
        CHECK(s.path_line() == "daemon PATH: " + kOldPath);

        Startup other;
        put(other.plist, old_plist(other.t.path / "other", kOldPath));
        CHECK(other.run() == PlistPathRefresh::LeftAlone);
        REQUIRE(other.log.size() == 1);
        CHECK(other.path_line() == "daemon PATH: " + kOldPath);

        Startup none;
        none.env.plist.reset();
        CHECK(none.run() == PlistPathRefresh::LeftAlone);
        REQUIRE(none.log.size() == 1);
        CHECK(none.path_line() == "daemon PATH: " + kOldPath);
    }

    TEST_CASE("a daemon run by hand logs its shell's PATH unchanged") {
        Startup s;
        s.env.inside_launchd_job = false;
        s.path = "/shell/bin:/usr/bin:/bin";
        put(s.plist, old_plist(s.cfg.den_home, kOldPath));
        CHECK(s.run() == PlistPathRefresh::Rewritten);
        CHECK(plist_path_value(slurp(s.plist)) == s.want());
        CHECK(s.path == "/shell/bin:/usr/bin:/bin");
        CHECK(s.path_line() == "daemon PATH: /shell/bin:/usr/bin:/bin");
    }

    TEST_CASE("a subprocess spawned after startup sees the corrected PATH") {
        // The real setenv/getenv, as the daemon wires them, so what pours
        // spawn inherits is what the PATH line says.
        Startup s;
        const char* saved = std::getenv("PATH");
        const std::optional<std::string> restore =
            saved ? std::optional<std::string>(saved) : std::nullopt;
        ::setenv("PATH", kOldPath.c_str(), 1);
        s.env.set_path = [](const std::string& p) { ::setenv("PATH", p.c_str(), 1); };
        s.env.get_path = []() -> std::optional<std::string> {
            const char* p = std::getenv("PATH");
            return p ? std::optional<std::string>(p) : std::nullopt;
        };
        put(s.plist, old_plist(s.cfg.den_home, kOldPath));
        s.run();

        std::string child;
        if (FILE* f = ::popen("printf %s \"$PATH\"", "r")) {
            std::array<char, 512> buf{};
            for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), f)) > 0;) {
                child.append(buf.data(), n);
            }
            ::pclose(f);
        }
        if (restore) {
            ::setenv("PATH", restore->c_str(), 1);
        } else {
            ::unsetenv("PATH");
        }
        CHECK(child == s.want());
        CHECK(s.path_line() == "daemon PATH (corrected in-process): " + s.want() +
                                   " (launchd gave " + kOldPath + ")");
    }
}

} // namespace test_daemon_plist_startup
} // namespace den
