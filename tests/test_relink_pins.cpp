// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den update` relinks kegs poured before bottles bound their dependencies
// through <prefix>/opt (relink_pinned_kegs). The fixtures are real binaries:
// `app` loads libdepa by its versioned keg path, as an old bottle did, and
// prints the version it loaded.

#include <doctest.h>

#ifdef __APPLE__

#include "build/linkage_scan.h"
#include "build/relink_pins.h"
#include "core/config.h"
#include "store/store.h"

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <utility>

namespace fs = std::filesystem;
using namespace den;

namespace {

struct RelinkTmp {
    fs::path path;
    RelinkTmp() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("den-relink-pins-" + std::to_string(rd()));
        fs::create_directories(path);
        path = fs::canonical(path);
    }
    ~RelinkTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    RelinkTmp(const RelinkTmp&) = delete;
    RelinkTmp& operator=(const RelinkTmp&) = delete;
};

Config relink_config(const fs::path& root) {
    Config c;
    c.den_home = root / "den";
    c.prefix = root / "prefix";
    c.homebrew_prefix = root / "brew"; // den has its own prefix
    c.store = c.prefix / "Cellar";
    c.homebrew_cellar = c.homebrew_prefix / "Cellar";
    c.homebrew_caskroom = c.homebrew_prefix / "Caskroom";
    c.homebrew_taps = c.homebrew_prefix / "Library" / "Taps";
    c.cache = c.den_home / "cache";
    c.appdir = root / "Applications";
    c.arch = Arch::Arm64;
    fs::create_directories(c.den_home);
    fs::create_directories(c.store);
    return c;
}

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << text;
}

std::string quote(const fs::path& p) {
    std::string out = "'";
    for (char c : p.string())
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

std::pair<int, std::string> sh(const std::string& cmd) {
    std::string out;
    FILE* p = ::popen((cmd + " 2>&1").c_str(), "r");
    REQUIRE(p != nullptr);
    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), buf.size(), p))
        out += buf.data();
    const int status = ::pclose(p);
    return {WIFEXITED(status) ? WEXITSTATUS(status) : 128, out};
}

void compile(const std::string& args) {
    auto [rc, out] = sh("cc " + args);
    INFO("cc " << args << "\n" << out);
    REQUIRE(rc == 0);
}

// depa <version>, identified by its versioned keg path (an old bottle's pin).
fs::path make_depa(const Config& c, const std::string& version, bool owned = true) {
    const auto keg = c.store / "depa" / version;
    const auto lib = keg / "lib" / "libdepa.1.dylib";
    write(keg / "src.c", "const char* depa(void) { return \"depa " + version + "\"; }\n");
    fs::create_directories(lib.parent_path());
    compile("-dynamiclib -install_name " + quote(lib) + " -o " + quote(lib) + " " +
            quote(keg / "src.c"));
    fs::remove(keg / "src.c");
    if (owned)
        mark_keg_owned(keg, "depa", version);
    return keg;
}

// app 1.0 linked against depa <version>'s versioned keg, with an rpath to it.
fs::path make_app(const Config& c, const std::string& depa_version) {
    const auto keg = c.store / "app" / "1.0";
    const auto deplib = c.store / "depa" / depa_version / "lib";
    write(keg / "main.c", "#include <stdio.h>\nconst char* depa(void);\n"
                          "int main(void) { puts(depa()); return 0; }\n");
    fs::create_directories(keg / "bin");
    compile("-o " + quote(keg / "bin" / "app") + " " + quote(keg / "main.c") + " " +
            quote(deplib / "libdepa.1.dylib") + " -Wl,-rpath," + deplib.string());
    fs::remove(keg / "main.c");
    fs::permissions(keg / "bin" / "app",
                    fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read |
                        fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);
    mark_keg_owned(keg, "app", "1.0");
    return keg;
}

void opt_link(const Config& c, const std::string& version) {
    const auto link = c.prefix / "opt" / "depa";
    fs::create_directories(link.parent_path());
    std::error_code ec;
    fs::remove(link, ec);
    fs::create_directory_symlink(fs::path("..") / "Cellar" / "depa" / version, link);
}

std::string app_says(const fs::path& app) {
    auto [rc, out] = sh(quote(app / "bin" / "app"));
    INFO(out);
    REQUIRE(rc == 0);
    return out.substr(0, out.find('\n'));
}

std::string loads(const fs::path& app) {
    return sh("otool -L " + quote(app / "bin" / "app") + "; otool -l " +
              quote(app / "bin" / "app") + " | grep -A2 LC_RPATH")
        .second;
}

} // namespace

TEST_CASE("relink_pinned_kegs: binds a pinned keg through opt, once") {
    RelinkTmp t;
    auto c = relink_config(t.path);
    make_depa(c, "1.0");
    opt_link(c, "1.0");
    const auto app = make_app(c, "1.0");
    REQUIRE(keg_pinned_loads(app, c.store, "app").size() == 2); // load + rpath
    REQUIRE(app_says(app) == "depa 1.0");

    auto r = relink_pinned_kegs(c);
    INFO(loads(app));
    REQUIRE(r.failed.empty());
    REQUIRE(r.left_pinned.empty());
    REQUIRE(r.relinked.size() == 1);
    CHECK(r.relinked[0] == "app 1.0 (bin/app: depa 1.0)");
    CHECK(r.files == 1);
    CHECK(keg_pinned_loads(app, c.store, "app").empty());
    const auto l = loads(app);
    CHECK(l.find((c.prefix / "opt" / "depa" / "lib" / "libdepa.1.dylib").string()) !=
          std::string::npos);
    CHECK(l.find((c.prefix / "opt" / "depa" / "lib").string()) != std::string::npos);
    CHECK(app_says(app) == "depa 1.0");
    CHECK(sh("codesign -v " + quote(app / "bin" / "app")).first == 0);
    // Mode survives the rename over the original.
    CHECK((fs::status(app / "bin" / "app").permissions() & fs::perms::owner_write) ==
          fs::perms::none);
    // No scratch left behind.
    for (const auto& e : fs::directory_iterator(app / "bin"))
        CHECK(e.path().filename() == "app");

    // Idempotent: nothing left to do.
    auto again = relink_pinned_kegs(c);
    CHECK(again.relinked.empty());
    CHECK(again.left_pinned.empty());
    CHECK(again.failed.empty());
    CHECK(again.files == 0);

    // And the point of it: a depa upgrade now reaches app.
    make_depa(c, "2.0");
    opt_link(c, "2.0");
    CHECK(app_says(app) == "depa 2.0");
}

TEST_CASE("relink_pinned_kegs: leaves a pin when opt names another version") {
    RelinkTmp t;
    auto c = relink_config(t.path);
    make_depa(c, "1.0");
    make_depa(c, "2.0");
    opt_link(c, "2.0");
    const auto app = make_app(c, "1.0");
    const auto before = loads(app);

    auto r = relink_pinned_kegs(c);
    REQUIRE(r.relinked.empty());
    REQUIRE(r.failed.empty());
    REQUIRE(r.left_pinned.size() == 1);
    CHECK(r.left_pinned[0].find("app 1.0 -> depa 1.0: ") == 0);
    CHECK(r.left_pinned[0].find("names 2.0, not 1.0") != std::string::npos);
    CHECK(loads(app) == before);
    CHECK(app_says(app) == "depa 1.0");
}

TEST_CASE("relink_pinned_kegs: leaves a pin when den did not pour the opt keg") {
    RelinkTmp t;
    auto c = relink_config(t.path);
    make_depa(c, "1.0", /*owned=*/false);
    opt_link(c, "1.0");
    const auto app = make_app(c, "1.0");
    const auto before = loads(app);

    auto r = relink_pinned_kegs(c);
    REQUIRE(r.relinked.empty());
    REQUIRE(r.left_pinned.size() == 1);
    CHECK(r.left_pinned[0].find("was not poured by den") != std::string::npos);
    CHECK(loads(app) == before);
    CHECK(app_says(app) == "depa 1.0");

    // A missing opt link is reported the same way.
    fs::remove(c.prefix / "opt" / "depa");
    r = relink_pinned_kegs(c);
    REQUIRE(r.left_pinned.size() == 1);
    CHECK(r.left_pinned[0].find(" is missing") != std::string::npos);
}

TEST_CASE("relink_pinned_kegs: header-space failure leaves the file unchanged") {
    RelinkTmp t;
    auto c = relink_config(t.path);
    make_depa(c, "1.0");
    opt_link(c, "1.0");
    const auto app = make_app(c, "1.0");
    const auto bin = app / "bin" / "app";
    const auto before = sh("shasum " + quote(bin)).second;

    // opt paths are always shorter than the versioned keg paths they
    // replace, so real header-space failures need a stand-in tool: one that
    // scribbles on the file it was given, then fails as install_name_tool does.
    const auto tool = t.path / "fake-install-name-tool";
    write(tool, "#!/bin/sh\nfor f; do :; done\nprintf garbage >> \"$f\"\n"
                "echo 'error: changing install names or rpaths can't be redone for: x "
                "(for architecture arm64) because larger updated load commands do not fit "
                "(the program must be relinked, and you may need to use -headerpad)' >&2\n"
                "exit 1\n");
    fs::permissions(tool, fs::perms::owner_all);

    auto r = relink_pinned_kegs(c, tool.string());
    REQUIRE(r.relinked.empty());
    REQUIRE(r.failed.size() == 1);
    CHECK(r.failed[0] == "app 1.0: bin/app: the opt paths do not fit in its Mach-O header");
    CHECK(r.files == 0);
    CHECK(sh("shasum " + quote(bin)).second == before);
    CHECK(app_says(app) == "depa 1.0");
    CHECK(keg_pinned_loads(app, c.store, "app").size() == 2);
    for (const auto& e : fs::directory_iterator(app / "bin"))
        CHECK(e.path().filename() == "app");
}

TEST_CASE("relink_pinned_kegs: skipped while den shares Homebrew's prefix") {
    RelinkTmp t;
    auto c = relink_config(t.path);
    c.homebrew_prefix = t.path / "brew";
    c.homebrew_cellar = c.homebrew_prefix / "Cellar";
    c.store = c.homebrew_cellar;
    c.prefix = c.homebrew_prefix;
    fs::create_directories(c.store);
    REQUIRE(c.shares_homebrew_cellar());
    make_depa(c, "1.0");
    opt_link(c, "1.0");
    const auto app = make_app(c, "1.0");
    auto r = relink_pinned_kegs(c);
    CHECK(r.relinked.empty());
    CHECK(r.left_pinned.empty());
    CHECK(keg_pinned_loads(app, c.store, "app").size() == 2);
}

#endif // __APPLE__
