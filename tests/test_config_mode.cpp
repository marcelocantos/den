// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// config.json is a sensitive file: den writes it 0600 (atomically, via a temp
// file that already has the final mode), and `den doctor` agrees on that mode
// and reports any other mode in octal.

#include "../src/doctor/doctor.h"
#include "../src/settings/settings.h"

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace den {
namespace test_config_mode {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_cfgmode_XXXXXX").string();
        if (!::mkdtemp(tmpl.data()))
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        path = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// Restores the process umask on scope exit.
struct UmaskGuard {
    mode_t old;
    explicit UmaskGuard(mode_t m) : old(::umask(m)) {}
    ~UmaskGuard() { ::umask(old); }
};

static unsigned file_mode(const fs::path& p) {
    struct stat st {};
    REQUIRE(::stat(p.c_str(), &st) == 0);
    return st.st_mode & 07777u;
}

// Run ./den (cwd is the build dir under ctest) with an isolated DEN_HOME.
static std::string run_den(const std::string& args, const std::string& den_home) {
    const std::string prefix = den_home + "/brew";
    std::string cmd = "DEN_HOME=" + den_home + " DEN_PREFIX=" + prefix +
                      " HOMEBREW_PREFIX=" + prefix + " HOMEBREW_CELLAR=" + prefix +
                      "/Cellar ./den " + args + " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen failed for: " + cmd);
    std::string out;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
        out += buf.data();
    ::pclose(pipe);
    return out;
}

TEST_SUITE("settings::config_mode") {

    TEST_CASE("config.json is written 0600 regardless of umask") {
        for (mode_t um : {mode_t{022}, mode_t{0}, mode_t{002}}) {
            CAPTURE(um);
            TempDir tmp;
            UmaskGuard g(um);
            set_setting(tmp.path, "daemon.auto_download", "false");
            CHECK(file_mode(tmp.path / "config.json") == 0600u);
            CHECK_FALSE(fs::exists(tmp.path / "config.json.tmp"));
        }
    }

    TEST_CASE("rewriting a 0644 config.json tightens it to 0600") {
        TempDir tmp;
        set_setting(tmp.path, "daemon.auto_download", "false");
        fs::permissions(tmp.path / "config.json", static_cast<fs::perms>(0644));
        // A stale, world-readable temp file must not leak its mode either.
        {
            FILE* f = std::fopen((tmp.path / "config.json.tmp").c_str(), "w");
            REQUIRE(f);
            std::fclose(f);
        }
        fs::permissions(tmp.path / "config.json.tmp", static_cast<fs::perms>(0666));
        set_setting(tmp.path, "daemon.auto_download", "true");
        CHECK(file_mode(tmp.path / "config.json") == 0600u);
        CHECK(get_setting(tmp.path, "daemon.auto_download") == "true");
    }

    TEST_CASE("writer and doctor agree on the mode") {
        CHECK(kConfigFileMode == 0600u);
        CHECK(sensitive_file_mode_problem("/x/config.json", kConfigFileMode).empty());
    }

    TEST_CASE("doctor reports a permissive mode in octal") {
        CHECK(format_file_mode(0644) == "0644");
        CHECK(format_file_mode(0600) == "0600");
        CHECK(format_file_mode(0755) == "0755");
        auto msg = sensitive_file_mode_problem("/x/config.json", 0644);
        REQUIRE_FALSE(msg.empty());
        CHECK(msg.find("mode 0644") != std::string::npos);
        CHECK(msg.find("expected 0600") != std::string::npos);
        CHECK(msg.find("420") == std::string::npos); // not decimal
        CHECK_FALSE(sensitive_file_mode_problem("/x/a.pem", 0640).empty());
        CHECK_FALSE(sensitive_file_mode_problem("/x/a.key", 0604).empty());
        CHECK(sensitive_file_mode_problem("/x/a.key", 0400).empty());
    }

    TEST_CASE("den doctor passes the permission check on a freshly written config") {
        TempDir tmp;
        const auto set_out = run_den("set daemon.auto_download false", tmp.path.string());
        CAPTURE(set_out);
        REQUIRE(fs::exists(tmp.path / "config.json"));
        CHECK(file_mode(tmp.path / "config.json") == 0600u);
        const auto out = run_den("doctor", tmp.path.string());
        CAPTURE(out);
        CHECK(out.find("overly permissive") == std::string::npos);
    }

    TEST_CASE("den doctor flags a 0644 config.json and shows 0644") {
        TempDir tmp;
        run_den("set daemon.auto_download false", tmp.path.string());
        REQUIRE(fs::exists(tmp.path / "config.json"));
        fs::permissions(tmp.path / "config.json", static_cast<fs::perms>(0644));
        const auto out = run_den("doctor", tmp.path.string());
        CAPTURE(out);
        CHECK(out.find("overly permissive mode 0644 (expected 0600)") != std::string::npos);
        CHECK(out.find("mode 420") == std::string::npos);
    }

} // TEST_SUITE

} // namespace test_config_mode
} // namespace den
