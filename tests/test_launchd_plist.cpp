// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// #157: the daemon LaunchAgent's PATH comes from den's own layout
// (<prefix>/bin, <den_home>/bin, system dirs), never a hardcoded Homebrew
// bin. Installed plists written by older dens carry /opt/homebrew/bin; they
// are rewritten by `den daemon install`, refreshed by `den self-update` and
// flagged by `den doctor`.

#include "../src/daemon/launchd_plist.h"
#include "../src/doctor/doctor.h"
#include "../src/selfupdate/daemon_restart.h"

#include <doctest.h>

#include <cerrno>
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
namespace test_launchd_plist {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_launchd_plist_XXXXXX").string();
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

struct ScopedEnv {
    std::string name;
    std::optional<std::string> old;
    ScopedEnv(std::string n, const std::optional<std::string>& value) : name(std::move(n)) {
        if (const char* v = std::getenv(name.c_str()))
            old = v;
        if (value)
            ::setenv(name.c_str(), value->c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
    ~ScopedEnv() {
        if (old)
            ::setenv(name.c_str(), old->c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
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

// The plist an older den wrote: Homebrew's bin hardcoded into PATH.
std::string legacy_plist(const fs::path& den_home, const std::string& path,
                         const std::string& extra = {}) {
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
           den_home.string() + R"(</string>
        <key>PATH</key>
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

TEST_SUITE("launchd_plist") {

    TEST_CASE("launchd_path is den's prefix bin, den home bin, then system dirs") {
        CHECK(launchd_path("/u/.den", "/u/.den/prefix") ==
              "/u/.den/prefix/bin:/u/.den/bin:/usr/bin:/bin:/usr/sbin:/sbin");
        // DEN_PREFIX-style custom prefix.
        CHECK(launchd_path("/u/.den", "/scratch/store/") ==
              "/scratch/store/bin:/u/.den/bin:/usr/bin:/bin:/usr/sbin:/sbin");
        // No brew on the machine and no prefix known: no Homebrew dirs at all.
        CHECK(launchd_path("/u/.den", "") == "/u/.den/bin:/usr/bin:/bin:/usr/sbin:/sbin");
    }

    TEST_CASE("Homebrew's bin is on the daemon PATH only when it is den's prefix") {
        for (const char* prefix : {"/u/.den/prefix", "/scratch/store", ""}) {
            const auto plist = launchd_plist("/x/den", "/u/.den", prefix);
            CHECK_FALSE(has(plist, "/opt/homebrew"));
            CHECK_FALSE(has(plist, "/usr/local/bin"));
        }
        CHECK(launchd_path("/u/.den", "/opt/homebrew") ==
              "/opt/homebrew/bin:/u/.den/bin:/usr/bin:/bin:/usr/sbin:/sbin");
        CHECK(launchd_path("/u/.den", "/usr/local") ==
              "/usr/local/bin:/u/.den/bin:/usr/bin:/bin:/usr/sbin:/sbin");
    }

    TEST_CASE("plist values round-trip through XML escaping") {
        const auto plist = launchd_plist("/x/den", "/u/a&b", "/p/<q>");
        CHECK(plist_env_value(plist, "DEN_HOME") == std::optional<std::string>("/u/a&b"));
        CHECK(plist_path_value(plist) == launchd_path("/u/a&b", "/p/<q>"));
        auto rewritten = plist_with_path_value(plist, "/a&b/bin:/usr/bin");
        REQUIRE(rewritten);
        CHECK(has(*rewritten, "<string>/a&amp;b/bin:/usr/bin</string>"));
        CHECK(plist_path_value(*rewritten) == "/a&b/bin:/usr/bin");
        CHECK_FALSE(plist_with_path_value("<plist/>", "/x"));
        CHECK(plist_unrecognised_keys(plist).empty());
        CHECK(plist_unrecognised_keys(legacy_plist(
                  "/u/.den", kOldPath, "    <key>Nice</key>\n    <integer>5</integer>\n")) ==
              std::vector<std::string>{"Nice"});
    }

    TEST_CASE("stale Homebrew dirs exclude den's own prefix bin") {
        CHECK(stale_homebrew_path_dirs(kOldPath, "/u/.den/prefix", "/opt/homebrew") ==
              std::vector<std::string>{"/opt/homebrew/bin", "/usr/local/bin"});
        CHECK(stale_homebrew_path_dirs(kOldPath, "/opt/homebrew", "/opt/homebrew") ==
              std::vector<std::string>{"/usr/local/bin"});
        CHECK(stale_homebrew_path_dirs("/brew/sbin:/usr/bin", "/u/.den/prefix", "/brew") ==
              std::vector<std::string>{"/brew/sbin"});
        CHECK(stale_homebrew_path_dirs(launchd_path("/u/.den", "/u/.den/prefix"), "/u/.den/prefix",
                                       "/opt/homebrew")
                  .empty());
    }

    TEST_CASE("refresh rewrites a stale PATH atomically, keeping a backup") {
        TempDir t;
        const auto den_home = t.path / ".den";
        const auto plist = t.path / "dev.den.daemon.plist";
        const auto backup = den_home / "dev.den.daemon.plist.bak";
        const auto old = legacy_plist(den_home, kOldPath);
        put(plist, old);
        const auto want = launchd_path(den_home, den_home / "prefix");
        std::ostringstream out;
        CHECK(refresh_launchd_plist_path(plist, want, backup, out) == PlistPathRefresh::Rewritten);
        CHECK(plist_path_value(slurp(plist)) == want);
        CHECK(slurp(backup) == old);
        // Only the PATH value changed.
        CHECK(slurp(plist) == *plist_with_path_value(old, want));
        CHECK(has(out.str(), "was " + kOldPath));
        CHECK(has(out.str(), "now " + want));
        CHECK_FALSE(fs::exists(plist.string() + ".tmp"));

        std::ostringstream again;
        CHECK(refresh_launchd_plist_path(plist, want, backup, again) == PlistPathRefresh::UpToDate);
        CHECK(again.str().empty());
    }

    TEST_CASE("refresh leaves a hand-edited plist alone and reports it") {
        TempDir t;
        const auto plist = t.path / "dev.den.daemon.plist";
        const auto backup = t.path / "bak";
        const auto old = legacy_plist(t.path / ".den", kOldPath,
                                      "    <key>Nice</key>\n    <integer>5</integer>\n");
        put(plist, old);
        std::ostringstream out;
        CHECK(refresh_launchd_plist_path(plist, "/p/bin:/usr/bin", backup, out) ==
              PlistPathRefresh::LeftAlone);
        CHECK(slurp(plist) == old);
        CHECK_FALSE(fs::exists(backup));
        CHECK(has(out.str(), "Nice"));
        CHECK(has(out.str(), "den daemon install"));
    }

    TEST_CASE("daemon install writes the generated plist, backing up a different one") {
        TempDir t;
        const auto den_home = t.path / ".den";
        const auto plist = t.path / "LaunchAgents" / "dev.den.daemon.plist";
        const auto backup = den_home / "dev.den.daemon.plist.bak";
        const auto generated = launchd_plist("/x/den", den_home, den_home / "prefix");

        std::ostringstream fresh;
        CHECK(write_daemon_plist(plist, generated, backup, fresh));
        CHECK(slurp(plist) == generated);
        CHECK_FALSE(fs::exists(backup));

        std::ostringstream same;
        CHECK_FALSE(write_daemon_plist(plist, generated, backup, same));

        const auto old =
            legacy_plist(den_home, kOldPath, "    <key>Nice</key>\n    <integer>5</integer>\n");
        put(plist, old);
        std::ostringstream out;
        CHECK(write_daemon_plist(plist, generated, backup, out));
        CHECK(slurp(plist) == generated);
        CHECK(slurp(backup) == old);
        CHECK(has(out.str(), "Daemon PATH: " + kOldPath + " -> " + plist_path_value(generated)));
        CHECK(has(out.str(), "drops keys den does not write (Nice)"));
    }

    TEST_CASE("self-update refreshes this den home's plist from its own prefix") {
        TempDir t;
        Config cfg;
        cfg.den_home = t.path / ".den";
        cfg.prefix = cfg.den_home / "prefix";
        cfg.prefix_source = PrefixSource::DenHome;
        const auto plist = t.path / "dev.den.daemon.plist";

        put(plist, legacy_plist(cfg.den_home, kOldPath));
        std::ostringstream out;
        CHECK(refresh_daemon_plist_path(cfg, plist, out) == PlistPathRefresh::Rewritten);
        CHECK(plist_path_value(slurp(plist)) == launchd_path(cfg.den_home, cfg.prefix));
        CHECK(fs::exists(cfg.den_home / "dev.den.daemon.plist.bak"));

        // A plist that pins DEN_PREFIX keeps that prefix.
        auto pinned = legacy_plist(cfg.den_home, kOldPath);
        pinned.replace(pinned.find("        <key>PATH</key>"), 0,
                       "        <key>DEN_PREFIX</key>\n        <string>/scratch/store</string>\n");
        put(plist, pinned);
        std::ostringstream out2;
        CHECK(refresh_daemon_plist_path(cfg, plist, out2) == PlistPathRefresh::Rewritten);
        CHECK(plist_path_value(slurp(plist)) == launchd_path(cfg.den_home, "/scratch/store"));

        // Another den home's daemon is not ours to touch.
        const auto other = legacy_plist(t.path / "other", kOldPath);
        put(plist, other);
        std::ostringstream out3;
        CHECK(refresh_daemon_plist_path(cfg, plist, out3) == PlistPathRefresh::LeftAlone);
        CHECK(slurp(plist) == other);

        // Running with DEN_PREFIX, but the daemon was installed without it:
        // our prefix says nothing about the daemon's.
        cfg.prefix_source = PrefixSource::EnvDenPrefix;
        const auto unpinned = legacy_plist(cfg.den_home, kOldPath);
        put(plist, unpinned);
        std::ostringstream out4;
        CHECK(refresh_daemon_plist_path(cfg, plist, out4) == PlistPathRefresh::LeftAlone);
        CHECK(slurp(plist) == unpinned);
    }

    TEST_CASE("self-update refreshes the plist before restarting, even with no daemon running") {
        for (bool running : {false, true}) {
            std::vector<std::string> calls;
            DaemonRestartEnv e;
            e.daemon_running = running;
            e.launchd_plist = fs::path("/Users/x/Library/LaunchAgents/dev.den.daemon.plist");
            e.launchd_domain = "gui/501";
            e.refresh_plist = [&](std::ostream&) { calls.push_back("refresh"); };
            e.restart_launchd = [&](std::ostream&) {
                calls.push_back("restart");
                return LaunchdRestartResult::Restarted;
            };
            e.stop = [&] { calls.push_back("stop"); };
            std::ostringstream out;
            restart_daemon_after_update(e, out);
            if (running)
                CHECK(calls == std::vector<std::string>{"refresh", "restart"});
            else
                CHECK(calls == std::vector<std::string>{"refresh"});
        }
    }

    TEST_CASE("a failing refresh is reported and the restart still happens") {
        std::vector<std::string> calls;
        DaemonRestartEnv e;
        e.daemon_running = true;
        e.launchd_plist = fs::path("/p.plist");
        e.refresh_plist = [](std::ostream&) { throw std::runtime_error("read-only"); };
        e.restart_launchd = [&](std::ostream&) {
            calls.push_back("restart");
            return LaunchdRestartResult::Restarted;
        };
        std::ostringstream out;
        CHECK(restart_daemon_after_update(e, out) == DaemonRestartOutcome::Relaunched);
        CHECK(calls == std::vector<std::string>{"restart"});
        CHECK(has(out.str(), "Could not refresh the den daemon plist's PATH (read-only)"));
    }

    TEST_CASE("doctor warns when the installed daemon PATH has Homebrew's bin") {
        TempDir t;
        Config c;
        c.den_home = t.path / "home";
        c.prefix = t.path / "pfx";
        c.store = c.prefix / "Cellar";
        c.cache = c.den_home / "cache";
        c.homebrew_prefix = "/opt/homebrew";
        fs::create_directories(c.den_home / "manifests" / "ROOT");
        const auto plist = t.path / "Library" / "LaunchAgents" / "dev.den.daemon.plist";
        ScopedEnv home("HOME", t.path.string());

        auto stale_warnings = [&] {
            int n = 0;
            for (const auto& f : doctor(c))
                if (has(f.message, "den daemon LaunchAgent PATH includes Homebrew's"))
                    ++n;
            return n;
        };

        put(plist, legacy_plist(c.den_home, kOldPath));
        CHECK(stale_warnings() == 1);
        const auto problem =
            stale_daemon_path_problem(plist, slurp(plist), c.prefix, c.homebrew_prefix);
        CHECK(has(problem, "/opt/homebrew/bin, /usr/local/bin"));
        CHECK(has(problem, "den daemon install"));

        put(plist, launchd_plist("/x/den", c.den_home, c.prefix));
        CHECK(stale_warnings() == 0);

        // Another den home's daemon is not this den's problem.
        put(plist, legacy_plist(t.path / "other", kOldPath));
        CHECK(stale_warnings() == 0);
    }

} // TEST_SUITE

} // namespace test_launchd_plist
} // namespace den
