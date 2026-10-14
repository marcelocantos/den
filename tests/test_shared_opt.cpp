// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Regression: den must not clobber or depend on Homebrew's shared
// <prefix>/opt links. Found on den-test-mac 2026-10-14: `den install bat`
// poured libgit2 1.9.7_2 nested under Cellar/libgit2/1.9.7/1.9.7_2 and
// repointed /opt/homebrew/opt/libgit2 at it, breaking Homebrew's bat; and
// `den install pv` left `@@HOMEBREW_PREFIX@@/opt/gettext/…` unrelocated
// because otool was unusable, so pv failed at load time.

#include <doctest.h>

#include "build/relocate.h"
#include "provider/homebrew_provider.h"
#include "store/link.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

namespace {
struct OptTmp {
    fs::path path;
    OptTmp() {
        path = fs::temp_directory_path() /
               ("den_test_opt_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }
    ~OptTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};
void touch(const fs::path& p) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << "x";
}
} // namespace

TEST_SUITE("shared_opt") {

    TEST_CASE("compare_keg_versions orders Homebrew versions") {
        CHECK(compare_keg_versions("1.9.10", "1.9.7") > 0);
        CHECK(compare_keg_versions("1.9.7_2", "1.9.7") > 0);
        CHECK(compare_keg_versions("1.8.4", "1.9.4") < 0);
        CHECK(compare_keg_versions("2024-09-24", "2026-03-19") < 0);
        CHECK(compare_keg_versions("1.0", "1.0") == 0);
    }

    TEST_CASE("select_dep_keg picks the highest real keg and skips nested-only kegs") {
        OptTmp t;
        const auto cellar = t.path / "Cellar";
        touch(cellar / "libgit2" / "1.8.4" / "lib" / "libgit2.1.8.dylib");
        touch(cellar / "libgit2" / "1.9.4" / "lib" / "libgit2.1.9.dylib");
        // Malformed keg from the old pour bug: only a nested version dir.
        touch(cellar / "libgit2" / "1.9.7" / "1.9.7_2" / "lib" / "libgit2.1.9.dylib");
        CHECK(select_dep_keg(cellar, "libgit2") == cellar / "libgit2" / "1.9.4");
        CHECK(select_dep_keg(cellar, "missing").empty());
    }

    TEST_CASE("pin_opt_reference binds opt references to exact kegs") {
        OptTmp t;
        const auto cellar = t.path / "Cellar";
        touch(cellar / "gettext" / "1.0" / "lib" / "libintl.8.dylib");
        const auto self = cellar / "pv" / "1.12.0";
        fs::create_directories(self / "bin");
        CHECK(pin_opt_reference("@@HOMEBREW_PREFIX@@/opt/gettext/lib/libintl.8.dylib", cellar, "pv",
                                self) ==
              (cellar / "gettext" / "1.0").string() + "/lib/libintl.8.dylib");
        CHECK(pin_opt_reference("@@HOMEBREW_PREFIX@@/opt/pv/lib/libx.dylib", cellar, "pv", self) ==
              self.string() + "/lib/libx.dylib");
        CHECK(pin_opt_reference("/usr/lib/libSystem.B.dylib", cellar, "pv", self) ==
              "/usr/lib/libSystem.B.dylib");
        CHECK(pin_opt_reference("@@HOMEBREW_PREFIX@@/opt/nope/lib/a.dylib", cellar, "pv", self) ==
              "@@HOMEBREW_PREFIX@@/opt/nope/lib/a.dylib");
        CHECK(pin_opt_reference("@@HOMEBREW_CELLAR@@/pv/1.12.0/lib/a.dylib", cellar, "pv", self) ==
              "@@HOMEBREW_CELLAR@@/pv/1.12.0/lib/a.dylib");
    }

    TEST_CASE("find_bottle_keg_dir handles revisioned bottles") {
        OptTmp t;
        fs::create_directories(t.path / "libgit2" / "1.9.7_2" / "lib");
        CHECK(find_bottle_keg_dir(t.path, "libgit2", "1.9.7") == t.path / "libgit2" / "1.9.7_2");
        fs::create_directories(t.path / "jq" / "1.8.2" / "bin");
        CHECK(find_bottle_keg_dir(t.path, "jq", "1.8.2") == t.path / "jq" / "1.8.2");
        fs::create_directories(t.path / "amb" / "1.0_1");
        fs::create_directories(t.path / "amb" / "1.0_2");
        CHECK(find_bottle_keg_dir(t.path, "amb", "1.0").empty());
        // "1.9.70" must not match version "1.9.7".
        fs::create_directories(t.path / "x" / "1.9.70");
        CHECK(find_bottle_keg_dir(t.path, "x", "1.9.7").empty());
    }

#ifdef __APPLE__
    TEST_CASE("link_package never creates or repoints the shared prefix opt link on macOS") {
        OptTmp t;
        const auto prefix = t.path / "prefix";
        const auto cellar = prefix / "Cellar";
        touch(cellar / "libgit2" / "1.8.4" / "lib" / "libgit2.1.8.dylib");
        const auto newkeg = cellar / "libgit2" / "1.9.7";
        touch(newkeg / "lib" / "libgit2.1.9.dylib");
        fs::create_directories(prefix / "opt");
        // Homebrew's link points at the version its packages were built for.
        fs::create_symlink(cellar / "libgit2" / "1.8.4", prefix / "opt" / "libgit2");
        const auto env = t.path / "env";
        link_package(newkeg, env, "libgit2", {"lib"});
        CHECK(fs::read_symlink(prefix / "opt" / "libgit2") == cellar / "libgit2" / "1.8.4");
        // The env's own opt link does point at den's keg.
        CHECK(fs::read_symlink(env / "opt" / "libgit2") == newkeg);

        // Absent shared link stays absent.
        const auto pv = cellar / "pv" / "1.12.0";
        touch(pv / "bin" / "pv");
        link_package(pv, env, "pv", {"bin"});
        CHECK_FALSE(fs::exists(prefix / "opt" / "pv"));
        CHECK_FALSE(fs::is_symlink(prefix / "opt" / "pv"));
    }
#endif
}

TEST_CASE("looks_like_macho_header distinguishes fat Mach-O from Java class files") {
    const unsigned char thin64[] = {0xCF, 0xFA, 0xED, 0xFE, 0x0C, 0x00, 0x00, 0x01};
    const unsigned char fat2[] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x02};
    const unsigned char java8[] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34};
    const unsigned char java_minor[] = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x03, 0x00, 0x2D};
    const unsigned char text[] = {'#', '!', '/', 'b', 'i', 'n', '/', 's'};
    CHECK(looks_like_macho_header(thin64, sizeof thin64));
    CHECK(looks_like_macho_header(fat2, sizeof fat2));
    CHECK_FALSE(looks_like_macho_header(java8, sizeof java8));
    CHECK_FALSE(looks_like_macho_header(java_minor, sizeof java_minor));
    CHECK_FALSE(looks_like_macho_header(text, sizeof text));
    CHECK_FALSE(looks_like_macho_header(fat2, 4));
}

} // namespace test
} // namespace den
