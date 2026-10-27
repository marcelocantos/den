// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den self-update` must not replace a den that Homebrew installed: it
// refuses and points at `brew upgrade marcelocantos/tap/den`, without network
// access or touching any file. Detection takes the exe path as input, so these
// tests use scratch Homebrew-shaped layouts, never a real install.

#include "../src/core/error.h"
#include "../src/selfupdate/selfupdate.h"

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace den {
namespace test_selfupdate_brew {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_subrew_XXXXXX").string();
        if (!::mkdtemp(tmpl.data()))
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        path = fs::canonical(tmpl);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

// A scratch Homebrew prefix: <p>/Cellar/den/0.14.0/bin/den, <p>/opt/den ->
// ../Cellar/den/0.14.0, <p>/bin/den -> ../Cellar/den/0.14.0/bin/den.
struct FakeBrew {
    TempDir tmp;
    fs::path prefix, keg, keg_exe;
    explicit FakeBrew(const fs::path& exe_src = {}) {
        prefix = tmp.path / "brew";
        keg = prefix / "Cellar" / "den" / "0.14.0";
        keg_exe = keg / "bin" / "den";
        fs::create_directories(keg / "bin");
        if (exe_src.empty()) {
            std::FILE* f = std::fopen(keg_exe.c_str(), "w");
            REQUIRE(f);
            std::fclose(f);
        } else {
            fs::copy_file(exe_src, keg_exe);
        }
        fs::permissions(keg_exe, static_cast<fs::perms>(0755));
        fs::create_directories(prefix / "opt");
        fs::create_directory_symlink("../Cellar/den/0.14.0", prefix / "opt" / "den");
        fs::create_directories(prefix / "bin");
        fs::create_symlink("../Cellar/den/0.14.0/bin/den", prefix / "bin" / "den");
    }
};

static std::string run(const std::string& cmd, int* status) {
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen failed for: " + cmd);
    std::string out;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr)
        out += buf.data();
    *status = ::pclose(pipe);
    return out;
}

TEST_SUITE("selfupdate::brew_keg") {

    TEST_CASE("an exe inside Cellar/den/<version> is a brew keg") {
        FakeBrew b;
        auto k = brew_keg_for_exe(b.keg_exe);
        REQUIRE(k);
        CHECK(*k == b.keg);
        // Detection is lexical for the keg path, so it holds even before the
        // keg exists on disk.
        CHECK(brew_keg_for_exe("/somewhere/Cellar/den/1.2.3/bin/den").has_value());
        CHECK(brew_keg_for_exe("/home/linuxbrew/.linuxbrew/Cellar/den/1.2.3/bin/den"));
    }

    TEST_CASE("an exe under the formula's opt path is a brew keg") {
        FakeBrew b;
        auto opt_exe = b.prefix / "opt" / "den" / "bin" / "den";
        auto k = brew_keg_for_exe(opt_exe);
        REQUIRE(k);
        // The unresolved opt path is recognised in its own right...
        CHECK(*k == b.prefix / "opt" / "den");
        // ...and a bin/ symlink resolves into the keg.
        auto k2 = brew_keg_for_exe(b.prefix / "bin" / "den");
        REQUIRE(k2);
        CHECK(*k2 == b.keg);
    }

    TEST_CASE("an opt path is only a keg when the prefix has Cellar/den") {
        TempDir t;
        fs::create_directories(t.path / "opt" / "den" / "bin");
        CHECK_FALSE(brew_keg_for_exe(t.path / "opt" / "den" / "bin" / "den"));
        CHECK_FALSE(brew_keg_for_exe("/opt/den/bin/den"));
    }

    TEST_CASE("a normal (non-brew) install is still allowed") {
        TempDir t;
        fs::create_directories(t.path / ".den" / "bin");
        auto exe = t.path / ".den" / "bin" / "den";
        CHECK_FALSE(brew_keg_for_exe(exe));
        CHECK_NOTHROW(refuse_self_update_if_brew_managed(exe));
        CHECK_FALSE(brew_keg_for_exe("/usr/local/bin/den"));
        CHECK_FALSE(brew_keg_for_exe("/home/me/.local/bin/den"));
        // "den" elsewhere in the path is not enough.
        CHECK_FALSE(brew_keg_for_exe("/x/Cellar/denx/1.0/bin/den"));
        CHECK_FALSE(brew_keg_for_exe("/x/den/Cellar/bin/den"));
    }

    TEST_CASE("refusal names the brew upgrade command") {
        FakeBrew b;
        try {
            refuse_self_update_if_brew_managed(b.keg_exe);
            FAIL("expected UserError");
        } catch (const UserError& e) {
            std::string msg = e.what();
            CHECK(msg.find("brew upgrade marcelocantos/tap/den") != std::string::npos);
            CHECK(msg.find(b.keg.string()) != std::string::npos);
        }
    }

    TEST_CASE("den self-update run from a brew keg refuses and changes nothing") {
        // Copy the freshly built den into a scratch keg and run it from there
        // (via the keg path and via the opt path).
        FakeBrew b(fs::absolute("den"));
        const auto before = fs::last_write_time(b.keg_exe);
        const auto size = fs::file_size(b.keg_exe);
        for (const auto& exe : {b.keg_exe, b.prefix / "opt" / "den" / "bin" / "den"}) {
            CAPTURE(exe);
            int status = 0;
            // No network: point any accidental fetch at a dead proxy.
            const auto out =
                run("DEN_HOME=" + (b.tmp.path / "home").string() +
                        " DEN_PREFIX=" + (b.tmp.path / "prefix").string() +
                        " HTTPS_PROXY=http://127.0.0.1:9 https_proxy=http://127.0.0.1:9 " +
                        exe.string() + " self-update",
                    &status);
            CAPTURE(out);
            REQUIRE(WIFEXITED(status));
            CHECK(WEXITSTATUS(status) != 0);
            CHECK(out.find("brew upgrade marcelocantos/tap/den") != std::string::npos);
            CHECK(out.find("Downloading") == std::string::npos);
        }
        CHECK(fs::last_write_time(b.keg_exe) == before);
        CHECK(fs::file_size(b.keg_exe) == size);
        // Nothing new appeared next to the binary (no temp/staged update).
        size_t n = 0;
        for ([[maybe_unused]] const auto& e : fs::directory_iterator(b.keg / "bin"))
            ++n;
        CHECK(n == 1);
    }

} // TEST_SUITE

} // namespace test_selfupdate_brew
} // namespace den
