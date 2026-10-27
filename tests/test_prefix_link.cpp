// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// The root environment is linked into the prefix itself, the way `brew link`
// does. Regression: automake's aclocal computes its macro dir from the prefix
// it was relocated to (<prefix>/share/aclocal). With the root env linked into
// envs/ROOT, that dir held only automake's own keg contents' links if anything
// and never the macros other packages ship, so `aclocal` could not find them.

#include <doctest.h>

#include "cli/install.h"
#include "cli/shell.h"
#include "core/config.h"
#include "core/error.h"
#include "doctor/doctor.h"
#include "env/environment.h"
#include "env/manifest.h"
#include "provider/homebrew_provider.h"
#include "provider/stub_provider.h"
#include "store/prefix_link.h"
#include "store/store.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace den {
namespace test {

namespace fs = std::filesystem;

namespace {

struct PrefixFx {
    fs::path root;
    fs::path prefix;
    fs::path store;
    fs::path home;

    explicit PrefixFx(const std::string& prefix_name = "prefix") {
        root = fs::temp_directory_path() /
               ("den_prefix_link_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        prefix = root / prefix_name;
        store = prefix / "Cellar";
        home = root / "den";
        fs::create_directories(store);
        fs::create_directories(home / "envs");
    }
    ~PrefixFx() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }

    static void write(const fs::path& p, const std::string& body, bool exec = false) {
        fs::create_directories(p.parent_path());
        std::ofstream(p) << body;
        if (exec) {
            fs::permissions(p, fs::perms::owner_all | fs::perms::group_read |
                                   fs::perms::group_exec | fs::perms::others_read |
                                   fs::perms::others_exec);
        }
    }

    // A keg in the Cellar. den_owned writes den's receipt (den poured it);
    // otherwise it stands in for a Homebrew keg.
    fs::path keg(const std::string& name, const std::string& version, bool den_owned = true) {
        auto k = store / name / version;
        fs::create_directories(k);
        if (den_owned) {
            mark_keg_owned(k, name, version);
        }
        return k;
    }

    void want(const std::string& name, const std::string& version, const std::string& env = "/") {
        auto m = read_manifest(home, env);
        m.packages["homebrew"][name] = version;
        write_manifest(home, env, m);
    }
    void drop(const std::string& name, const std::string& env = "/") {
        auto m = read_manifest(home, env);
        m.packages["homebrew"].erase(name);
        write_manifest(home, env, m);
    }

    // Brew-style relative link, as `brew link` would leave it.
    void brew_link(const fs::path& link, const fs::path& target) {
        fs::create_directories(link.parent_path());
        fs::create_symlink(fs::relative(target, link.parent_path()), link);
    }

    // An automake-shaped keg: aclocal prints the macro dir it was relocated
    // to, and ships its own macros there.
    void automake() {
        auto k = keg("automake", "1.17");
        write(k / "bin" / "aclocal",
              "#!/bin/sh\n"
              "if [ \"$1\" = \"--print-ac-dir\" ]; then echo '" +
                  (prefix / "share" / "aclocal").string() + "'; fi\n",
              true);
        write(k / "share" / "aclocal" / "automake.m4", "dnl automake\n");
        write(k / "share" / "automake-1.17" / "am" / "header.am", "# am\n");
        want("automake", "1.17");
    }
};

std::string run(const fs::path& exe, const std::string& args) {
    std::string out;
    auto cmd = "'" + exe.string() + "' " + args;
    FILE* p = popen(cmd.c_str(), "r");
    REQUIRE(p != nullptr);
    char buf[512];
    while (fgets(buf, sizeof buf, p)) {
        out += buf;
    }
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

bool exists_no_follow(const fs::path& p) {
    std::error_code ec;
    return fs::symlink_status(p, ec).type() != fs::file_type::not_found;
}

} // namespace

TEST_CASE("prefix link: aclocal --print-ac-dir finds macros from other packages") {
    PrefixFx fx;
    fx.automake();
    auto libtool = fx.keg("libtool", "2.5.4");
    PrefixFx::write(libtool / "share" / "aclocal" / "libtool.m4", "dnl libtool\n");
    fx.want("libtool", "2.5.4");

    materialise(fx.home, fx.store, "/");

    auto aclocal = fx.prefix / "bin" / "aclocal";
    REQUIRE(fs::is_symlink(aclocal));
    auto acdir = fs::path(run(aclocal, "--print-ac-dir"));
    CHECK(acdir == fx.prefix / "share" / "aclocal");
    CHECK(fs::exists(acdir / "automake.m4"));
    CHECK(fs::exists(acdir / "libtool.m4"));
    CHECK(fs::exists(fx.prefix / "share" / "automake-1.17" / "am" / "header.am"));
    // Links into the Cellar are relative, as brew writes them.
    CHECK(fs::read_symlink(aclocal).is_relative());

    // envs/ROOT is a compat shim onto the prefix.
    auto shim = env_dir(fx.home, "/") / "bin";
    REQUIRE(fs::is_symlink(shim));
    CHECK(fs::equivalent(shim, fx.prefix / "bin"));
    CHECK(fs::exists(shim / "aclocal"));
}

TEST_CASE("prefix link: shares a brew prefix without touching brew's links") {
    // Migrate-style layout: den adopted an existing Homebrew prefix that
    // already has brew kegs linked into it.
    PrefixFx fx("homebrew");
    auto m4 = fx.keg("m4", "1.4.19", /*den_owned=*/false);
    PrefixFx::write(m4 / "bin" / "m4", "#!/bin/sh\n", true);
    fx.brew_link(fx.prefix / "bin" / "m4", m4 / "bin" / "m4");
    // brew links a dir wholesale when one keg owns it.
    auto brew_libtool = fx.keg("libtool", "2.5.4", /*den_owned=*/false);
    PrefixFx::write(brew_libtool / "share" / "aclocal" / "libtool.m4", "dnl libtool\n");
    fx.brew_link(fx.prefix / "share" / "aclocal", brew_libtool / "share" / "aclocal");
    auto m4_link = fs::read_symlink(fx.prefix / "bin" / "m4");

    fx.automake();
    materialise(fx.home, fx.store, "/");

    auto acdir = fs::path(run(fx.prefix / "bin" / "aclocal", "--print-ac-dir"));
    CHECK(fs::exists(acdir / "automake.m4"));
    CHECK(fs::exists(acdir / "libtool.m4"));
    // The shared dir was split into per-file links, brew-style.
    CHECK_FALSE(fs::is_symlink(fx.prefix / "share" / "aclocal"));
    CHECK(fs::read_symlink(fx.prefix / "bin" / "m4") == m4_link);

    // Unlinking automake leaves brew's links alone.
    fx.drop("automake");
    materialise(fx.home, fx.store, "/");
    CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "aclocal"));
    CHECK_FALSE(exists_no_follow(fx.prefix / "share" / "aclocal" / "automake.m4"));
    CHECK(fs::exists(fx.prefix / "share" / "aclocal" / "libtool.m4"));
    CHECK(fs::read_symlink(fx.prefix / "bin" / "m4") == m4_link);
}

TEST_CASE("prefix link: refuses to clobber foreign links and files") {
    PrefixFx fx;
    auto brew_foo = fx.keg("brewfoo", "1.0", /*den_owned=*/false);
    PrefixFx::write(brew_foo / "bin" / "foo", "#!/bin/sh\n", true);
    fx.brew_link(fx.prefix / "bin" / "foo", brew_foo / "bin" / "foo");
    PrefixFx::write(fx.prefix / "bin" / "bar", "user's own script\n", true);
    auto brew_target = fs::read_symlink(fx.prefix / "bin" / "foo");

    auto foo = fx.keg("foo", "2.0");
    PrefixFx::write(foo / "bin" / "foo", "#!/bin/sh\n", true);
    PrefixFx::write(foo / "bin" / "bar", "#!/bin/sh\n", true);
    PrefixFx::write(foo / "bin" / "foo-extra", "#!/bin/sh\n", true);
    fx.want("foo", "2.0");

    std::string msg;
    try {
        materialise(fx.home, fx.store, "/");
    } catch (const UserError& e) {
        msg = e.what();
    }
    REQUIRE_FALSE(msg.empty());
    CHECK(msg.find((fx.prefix / "bin" / "foo").string()) != std::string::npos);
    CHECK(msg.find((fx.prefix / "bin" / "bar").string()) != std::string::npos);
    CHECK(msg.find("--overwrite") != std::string::npos);
    // Nothing of foo was linked, and the foreign objects are untouched.
    CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "foo-extra"));
    CHECK(fs::read_symlink(fx.prefix / "bin" / "foo") == brew_target);
    CHECK_FALSE(fs::is_symlink(fx.prefix / "bin" / "bar"));

    SUBCASE("--overwrite replaces them") {
        MaterialiseOptions opts;
        opts.overwrite.insert("foo");
        materialise(fx.home, fx.store, "/", nullptr, opts);
        CHECK(fs::equivalent(fx.prefix / "bin" / "foo", foo / "bin" / "foo"));
        CHECK(fs::equivalent(fx.prefix / "bin" / "bar", foo / "bin" / "bar"));
        CHECK(fs::equivalent(fx.prefix / "bin" / "foo-extra", foo / "bin" / "foo-extra"));

        SUBCASE("unlink removes only den's own links") {
            // Something else takes over one of den's links afterwards.
            fs::remove(fx.prefix / "bin" / "foo-extra");
            fx.brew_link(fx.prefix / "bin" / "foo-extra", brew_foo / "bin" / "foo");
            fx.drop("foo");
            materialise(fx.home, fx.store, "/");
            CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "foo"));
            CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "bar"));
            REQUIRE(fs::is_symlink(fx.prefix / "bin" / "foo-extra"));
            CHECK(fs::equivalent(fx.prefix / "bin" / "foo-extra", brew_foo / "bin" / "foo"));
            CHECK(fs::is_directory(fx.prefix / "bin"));
        }
    }
}

TEST_CASE("prefix link: replaces links to den-managed kegs and keeps config files") {
    PrefixFx fx;
    auto a = fx.keg("tool", "1.0");
    PrefixFx::write(a / "bin" / "tool", "#!/bin/sh\necho 1\n", true);
    PrefixFx::write(a / "etc" / "tool.conf", "default\n");
    PrefixFx::write(fx.prefix / "etc" / "tool.conf", "edited by user\n");
    fx.want("tool", "1.0");
    materialise(fx.home, fx.store, "/");
    CHECK(fs::equivalent(fx.prefix / "bin" / "tool", a / "bin" / "tool"));
    // An existing config file is kept, as brew does.
    CHECK_FALSE(fs::is_symlink(fx.prefix / "etc" / "tool.conf"));

    // Upgrade: the link into the old den keg is replaced.
    auto b = fx.keg("tool", "2.0");
    PrefixFx::write(b / "bin" / "tool", "#!/bin/sh\necho 2\n", true);
    fx.want("tool", "2.0");
    materialise(fx.home, fx.store, "/");
    CHECK(fs::equivalent(fx.prefix / "bin" / "tool", b / "bin" / "tool"));
    CHECK(run(fx.prefix / "bin" / "tool", "") == "2");
}

TEST_CASE("prefix link: migrates an envs/ROOT install") {
    PrefixFx fx;
    auto k = fx.keg("tool", "1.0");
    PrefixFx::write(k / "bin" / "tool", "#!/bin/sh\n", true);
    PrefixFx::write(k / "share" / "man" / "man1" / "tool.1", ".TH tool\n");
    fx.want("tool", "1.0");
    auto cask_bin = fx.root / "Caskroom" / "democtl" / "1.0" / "democtl";
    PrefixFx::write(cask_bin, "#!/bin/sh\n", true);

    // The old layout, as an earlier den left it.
    auto env = env_dir(fx.home, "/");
    fs::create_directories(env / "bin");
    fs::create_directories(env / "share" / "man" / "man1");
    fs::create_directories(env / "opt");
    fs::create_symlink(k / "bin" / "tool", env / "bin" / "tool");
    fs::create_symlink(k / "share" / "man" / "man1" / "tool.1",
                       env / "share" / "man" / "man1" / "tool.1");
    fs::create_symlink(k, env / "opt" / "tool");
    fs::create_symlink(cask_bin, env / "bin" / "democtl");
    PrefixFx::write(env / "share" / "notes.txt", "mine\n");
    PrefixFx::write(env / ".den" / "linked" / "tool", "1.0");

    // A named env keeps its own dirs.
    fx.want("tool", "1.0", "/work");
    materialise(fx.home, fx.store, "/work");
    auto work = env_dir(fx.home, "/work");
    REQUIRE(fs::is_symlink(work / "bin" / "tool"));

    REQUIRE(root_env_needs_migration(fx.home));
    auto report = migrate_root_env_to_prefix(fx.home, fx.store);
    CHECK(report.migrated);
    CHECK(report.conflicts.empty());
    CHECK(report.moved == 1);
    CHECK(report.removed >= 2);
    REQUIRE(report.preserved.size() == 1);
    CHECK(fs::exists(report.preserved[0] / "notes.txt"));

    CHECK(fs::equivalent(fx.prefix / "bin" / "tool", k / "bin" / "tool"));
    CHECK(fs::exists(fx.prefix / "share" / "man" / "man1" / "tool.1"));
    CHECK(fs::equivalent(fx.prefix / "bin" / "democtl", cask_bin));
    CHECK(fs::is_symlink(env / "bin"));
    CHECK(fs::exists(env / "bin" / "democtl"));
    CHECK_FALSE(fs::exists(env / ".den" / "linked"));
    CHECK_FALSE(root_env_needs_migration(fx.home));

    // Idempotent.
    auto again = migrate_root_env_to_prefix(fx.home, fx.store);
    CHECK_FALSE(again.migrated);
    CHECK(fs::equivalent(fx.prefix / "bin" / "tool", k / "bin" / "tool"));

    // The named env was not touched.
    CHECK_FALSE(fs::is_symlink(work / "bin"));
    CHECK(fs::equivalent(work / "bin" / "tool", k / "bin" / "tool"));
}

TEST_CASE("prefix link: named envs stay in envs/<slug>") {
    PrefixFx fx;
    auto k = fx.keg("only", "1.0");
    PrefixFx::write(k / "bin" / "only", "#!/bin/sh\n", true);
    fx.want("only", "1.0", "/proj");
    materialise(fx.home, fx.store, "/proj");
    CHECK(fs::is_symlink(env_dir(fx.home, "/proj") / "bin" / "only"));
    CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "only"));
}

TEST_CASE("prefix link: shell-env puts <prefix>/bin and sbin first") {
    PrefixFx fx;
    Config cfg;
    cfg.den_home = fx.home;
    cfg.store = fx.store;
    materialise(fx.home, fx.store, "/");

    std::ostringstream out;
    auto* old = std::cout.rdbuf(out.rdbuf());
    try {
        print_env_switch(cfg, "ROOT");
    } catch (...) {
        std::cout.rdbuf(old);
        throw;
    }
    std::cout.rdbuf(old);
    auto s = out.str();
    auto bin = s.find((fx.prefix / "bin").string());
    auto sbin = s.find((fx.prefix / "sbin").string());
    auto den_bin = s.find((fx.home / "bin").string());
    REQUIRE(bin != std::string::npos);
    REQUIRE(sbin != std::string::npos);
    CHECK(bin < sbin);
    if (den_bin != std::string::npos) {
        CHECK(sbin < den_bin);
    }
    CHECK(s.find((env_dir(fx.home, "/") / "bin").string()) == std::string::npos);
}

TEST_CASE("prefix link: uninstalling a root Cellar keg clears its links, records and opt link") {
    PrefixFx fx;
    Config cfg;
    cfg.den_home = fx.home;
    cfg.store = fx.store;
    auto k = fx.keg("tool", "1.0");
    PrefixFx::write(k / "bin" / "tool", "#!/bin/sh\n", true);
    fx.want("tool", "1.0");
    materialise(fx.home, fx.store, "/");
    REQUIRE(fs::is_symlink(fx.prefix / "bin" / "tool"));
    REQUIRE(fs::is_symlink(fx.prefix / "opt" / "tool"));
    // The Cellar opt link is the shared, relative one #84's release logic expects.
    CHECK(fs::read_symlink(fx.prefix / "opt" / "tool") == fs::path("../Cellar/tool/1.0"));
    const auto ctx = make_prefix_link_context(fx.home, fx.store);

    HomebrewProvider brew(nullptr);
    uninstall_packages(cfg, brew, {"tool"});
    CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "tool"));
    CHECK_FALSE(exists_no_follow(fx.prefix / "opt" / "tool"));
    CHECK_FALSE(prefix_linked_version(ctx, "tool"));
    CHECK(prefix_linked_names(ctx).empty());

    // Re-materialising does not bring anything back.
    materialise(fx.home, fx.store, "/");
    CHECK_FALSE(exists_no_follow(fx.prefix / "opt" / "tool"));
    CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "tool"));
}

TEST_CASE("prefix link: opt hand-off from the root env is stable across re-materialise") {
    PrefixFx fx;
    Config cfg;
    cfg.den_home = fx.home;
    cfg.store = fx.store;
    auto v1 = fx.keg("tool", "1.0");
    PrefixFx::write(v1 / "bin" / "tool", "#!/bin/sh\n", true);
    auto v2 = fx.keg("tool", "2.0");
    PrefixFx::write(v2 / "bin" / "tool", "#!/bin/sh\n", true);
    // Root uses the newer keg, so opt/tool points there; /work keeps 1.0.
    fx.want("tool", "2.0");
    fx.want("tool", "1.0", "/work");
    materialise(fx.home, fx.store, "/work");
    materialise(fx.home, fx.store, "/");
    REQUIRE(fs::equivalent(fx.prefix / "opt" / "tool", v2));

    HomebrewProvider brew(nullptr);
    uninstall_packages(cfg, brew, {"tool"});
    REQUIRE(fs::is_symlink(fx.prefix / "opt" / "tool"));
    CHECK(fs::read_symlink(fx.prefix / "opt" / "tool") == fs::path("../Cellar/tool/1.0"));
    CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "tool"));

    // Neither env's materialise flips it back (ensure_prefix_opt_link only
    // repoints to a newer keg; the root's ledger has no claim on opt/tool).
    materialise(fx.home, fx.store, "/");
    materialise(fx.home, fx.store, "/work");
    CHECK(fs::equivalent(fx.prefix / "opt" / "tool", v1));
    CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "tool"));
}

TEST_CASE(
    "prefix link: uninstalling a non-Cellar root package leaves no stale opt link or record") {
    PrefixFx fx;
    Config cfg;
    cfg.den_home = fx.home;
    cfg.store = fx.store;
    StubProvider stub;
    stub.install(cfg, "demo", "");
    auto m = read_manifest(fx.home, "/");
    m.packages["stub"]["demo"] = "0.1.0";
    write_manifest(fx.home, "/", m);
    materialise(fx.home, fx.store, "/");

    const auto root = stub.package_root(cfg, "demo", "0.1.0");
    const auto opt = fx.prefix / "opt" / "demo";
    REQUIRE(fs::is_symlink(opt));
    CHECK(fs::read_symlink(opt) == root); // absolute: not a Cellar keg
    REQUIRE(fs::is_symlink(fx.prefix / "bin" / "demo"));
    const auto ctx = make_prefix_link_context(fx.home, fx.store);
    REQUIRE(prefix_linked_version(ctx, "demo"));

    uninstall_packages(cfg, stub, {"demo"});
    CHECK_FALSE(exists_no_follow(opt));
    CHECK_FALSE(exists_no_follow(fx.prefix / "bin" / "demo"));
    CHECK_FALSE(prefix_linked_version(ctx, "demo"));
    CHECK(prefix_linked_names(ctx).empty());
}

TEST_CASE("prefix link: a foreign opt link for a non-Cellar package name is left alone") {
    PrefixFx fx;
    Config cfg;
    cfg.den_home = fx.home;
    cfg.store = fx.store;
    auto brew_demo = fx.keg("demo", "9.9", /*den_owned=*/false);
    fx.brew_link(fx.prefix / "opt" / "demo", brew_demo);
    const auto brew_target = fs::read_symlink(fx.prefix / "opt" / "demo");

    StubProvider stub;
    stub.install(cfg, "demo", "");
    auto m = read_manifest(fx.home, "/");
    m.packages["stub"]["demo"] = "0.1.0";
    write_manifest(fx.home, "/", m);
    materialise(fx.home, fx.store, "/");
    CHECK(fs::read_symlink(fx.prefix / "opt" / "demo") == brew_target);

    uninstall_packages(cfg, stub, {"demo"});
    CHECK(fs::read_symlink(fx.prefix / "opt" / "demo") == brew_target);
}

TEST_CASE("doctor: PATH order for the root env") {
    const fs::path prefix = "/opt/homebrew";
    const fs::path den_bin = "/Users/u/.den/bin";
    CHECK(root_path_order_problem("/opt/homebrew/bin:/opt/homebrew/sbin:/usr/bin", prefix, den_bin)
              .empty());
    CHECK(root_path_order_problem("/Users/u/.den/bin:/opt/homebrew/bin:/opt/homebrew/sbin:/usr/bin",
                                  prefix, den_bin)
              .empty());
    CHECK_FALSE(
        root_path_order_problem("/usr/bin:/opt/homebrew/bin:/opt/homebrew/sbin", prefix, den_bin)
            .empty());
    CHECK_FALSE(
        root_path_order_problem("/opt/homebrew/sbin:/opt/homebrew/bin", prefix, den_bin).empty());
    CHECK_FALSE(
        root_path_order_problem("/Users/u/.den/envs/ROOT/bin:/usr/bin", prefix, den_bin).empty());
}

} // namespace test
} // namespace den
