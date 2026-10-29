// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Source-build sandbox (docs/source-builds.md, "Build sandbox"): on macOS,
// configure, build and install commands run under a sandbox-exec profile that
// denies writes to den's prefix and $DEN_HOME except the build directory, the
// staging DESTDIR and the cache temp directory.

#include <doctest.h>

#include "build/build_sandbox.h"
#include "build/source_build.h"
#include "core/config.h"
#include "core/error.h"
#include "download/sha256.h"
#include "index/package.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace den {

namespace {

struct TempTree {
    fs::path root;
    TempTree() {
        std::random_device rd;
        root = fs::temp_directory_path() / ("den-sandbox-" + std::to_string(rd()));
        fs::create_directories(root);
        root = fs::canonical(root);
    }
    ~TempTree() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

void write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string sh_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s)
        out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
}

int sh(const std::string& cmd) {
    int rc = std::system(cmd.c_str());
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

Config make_config(const fs::path& root) {
    Config c;
    c.den_home = root / "home";
    c.cache = c.den_home / "cache";
    c.set_prefix(root / "prefix", PrefixSource::EnvDenPrefix);
    return c;
}

// Sets an environment variable for one scope, restoring it afterwards.
struct ScopedEnv {
    std::string name;
    std::optional<std::string> old;
    ScopedEnv(std::string n, const std::string& value) : name(std::move(n)) {
        if (const char* v = std::getenv(name.c_str()))
            old = v;
        ::setenv(name.c_str(), value.c_str(), 1);
    }
    ~ScopedEnv() {
        if (old)
            ::setenv(name.c_str(), old->c_str(), 1);
        else
            ::unsetenv(name.c_str());
    }
};

// The real $HOME, as build_sandbox.cpp reads it.
fs::path home() {
    const char* h = std::getenv("HOME");
    REQUIRE(h != nullptr);
    return h;
}

bool contains(const std::vector<fs::path>& v, const fs::path& p) {
    for (const auto& e : v)
        if (e == p)
            return true;
    return false;
}

// Every entry under `dir` (symlinks not followed) with its type, permissions,
// modification time and contents (or link target): equal snapshots mean the
// tree is byte-identical.
std::string snapshot(const fs::path& dir) {
    std::vector<std::string> lines;
    for (auto it = fs::recursive_directory_iterator(dir); it != fs::recursive_directory_iterator();
         ++it) {
        const auto& p = it->path();
        const auto st = fs::symlink_status(p);
        std::string line = fs::relative(p, dir).string() + " " +
                           std::to_string(static_cast<int>(st.type())) + " " +
                           std::to_string(static_cast<unsigned>(st.permissions())) + " ";
        if (fs::is_symlink(st)) {
            line += "-> " + fs::read_symlink(p).string();
        } else {
            line += std::to_string(
                static_cast<long long>(fs::last_write_time(p).time_since_epoch().count()));
            if (fs::is_regular_file(st))
                line += " " + slurp(p);
        }
        lines.push_back(line);
    }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const auto& l : lines)
        out += l + "\n";
    return out;
}

// A fixture formula whose source is a single Makefile; `make` then
// `make install PREFIX=<opt>` (den stages the install through DESTDIR).
PackageIndex fixture(const fs::path& root, const std::string& name, const std::string& klass,
                     const std::string& makefile) {
    const auto src = root / "src" / (name + "-1.0");
    write(src / "Makefile", makefile);
    const auto archive = root / (name + "-1.0.tar.xz");
    REQUIRE(sh("tar -cJf " + sh_quote(archive.string()) + " -C " +
               sh_quote((root / "src").string()) + " " + name + "-1.0") == 0);
    const auto formula = root / (name + ".rb");
    write(formula, "class " + klass +
                       " < Formula\n"
                       "  desc \"den build sandbox fixture\"\n"
                       "  homepage \"https://example.invalid/" +
                       name + "\"\n  url \"file://" + archive.string() + "\"\n  sha256 \"" +
                       hash_file(archive) +
                       "\"\n\n  def install\n"
                       "    system \"make\"\n"
                       "    system \"make\", \"install\", \"PREFIX=#{prefix}\"\n"
                       "  end\nend\n");
    PackageIndex idx;
    Package pkg;
    pkg.name = name;
    pkg.version = "1.0";
    pkg.ruby_source_path = formula.string();
    idx.packages[name] = pkg;
    return idx;
}

} // namespace

TEST_SUITE("build_sandbox") {

    TEST_CASE("seatbelt_quote escapes quotes and backslashes") {
        CHECK(seatbelt_quote("/a/b") == "\"/a/b\"");
        CHECK(seatbelt_quote("a\"b\\c") == "\"a\\\"b\\\\c\"");
        CHECK(seatbelt_quote("it's (x)") == "\"it's (x)\"");
    }

    TEST_CASE("the sandbox denies the prefix and DEN_HOME except this build's dirs") {
        TempTree t;
        // Seatbelt matches real paths: everything resolves through symlinks.
        fs::create_directories(t.root / "real");
        fs::create_directory_symlink(t.root / "real", t.root / "link");
        const auto cfg = make_config(t.root / "link");
        const auto sb = build_sandbox_for(cfg, "foo", "1.2");

        const auto real = t.root / "real";
        CHECK(contains(sb.deny_write, real / "prefix"));
        CHECK(contains(sb.deny_write, real / "prefix" / "Cellar"));
        CHECK(contains(sb.deny_write, real / "home"));
        CHECK(sb.allow_write.size() == 4);
        CHECK(contains(sb.allow_write, real / "home" / "cache" / "build" / "foo-1.2"));
        CHECK(contains(sb.allow_write, real / "home" / "cache" / "build" / "foo-1.2.destdir"));
        CHECK(contains(sb.allow_write, real / "home" / "cache" / "tmp" / "foo-1.2"));
        CHECK(sb.temp_dir == real / "home" / "cache" / "tmp" / "foo-1.2");
    }

    TEST_CASE("the profile denies first, then carves out the allowed dirs") {
        BuildSandbox sb;
        sb.deny_write = {"/p", "/h"};
        sb.allow_write = {"/h/cache/build/x-1", "/h/cache/tmp/x-1"};
        const auto prof = seatbelt_profile(sb);
        CHECK(prof.starts_with("(version 1)\n(debug deny)\n(allow default)\n"));
        const auto deny = prof.find("(deny file-write* (subpath \"/p\") (subpath \"/h\"))");
        const auto allow = prof.find(
            "(allow file-write* (subpath \"/h/cache/build/x-1\") (subpath \"/h/cache/tmp/x-1\"))");
        REQUIRE(deny != std::string::npos);
        REQUIRE(allow != std::string::npos);
        CHECK(deny < allow); // later rules win
        CHECK(prof.find("(deny file-write-setugid)") > allow);
    }

    TEST_CASE("the sandbox also denies Homebrew's prefixes, /usr/local and ~/Library") {
        TempTree t;
        auto cfg = make_config(t.root);
        {
            // The real roots (only inspected here, never written).
            const auto sb = build_sandbox_for(cfg, "foo", "1.0");
            const auto roots = system_deny_roots(cfg);
            for (const auto* r : {"/opt/homebrew", "/usr/local"}) {
                CAPTURE(r);
                const auto real = fs::weakly_canonical(r);
                CHECK(contains(roots, real));
                CHECK(contains(sb.deny_write, real));
            }
            CHECK(contains(roots, fs::weakly_canonical(home() / "Library")));
            CHECK(contains(sb.allow_write, fs::weakly_canonical(home() / "Library" / "Developer")));
        }

        // Test hook: the fixed roots rebased under a scratch root, plus a
        // detected Homebrew prefix that differs from both defaults.
        const auto sys = t.root / "sys";
        ScopedEnv env("DEN_BUILD_SANDBOX_SYSTEM_ROOT", sys.string());
        cfg.homebrew_prefix = t.root / "brew";
        cfg.homebrew_cellar = t.root / "brew-cellar";
        const auto roots = system_deny_roots(cfg);
        CHECK(contains(roots, sys / "opt" / "homebrew"));
        CHECK(contains(roots, sys / "opt" / "homebrew" / "Cellar"));
        CHECK(contains(roots, sys / "usr" / "local"));
        CHECK(contains(roots, sys / home().relative_path() / "Library"));
        CHECK(contains(roots, t.root / "brew"));
        CHECK(contains(roots, t.root / "brew-cellar"));
        CHECK_FALSE(contains(roots, fs::path("/opt/homebrew")));
        CHECK_FALSE(contains(roots, fs::path("/usr/local")));

        const auto sb = build_sandbox_for(cfg, "foo", "1.0");
        for (const auto& r : roots)
            CHECK(contains(sb.deny_write, r));
        CHECK(contains(sb.allow_write, sys / home().relative_path() / "Library" / "Developer"));

        // A default detected prefix is not listed twice.
        cfg.homebrew_prefix = sys / "opt" / "homebrew";
        cfg.homebrew_cellar = sys / "opt" / "homebrew" / "Cellar";
        CHECK(system_deny_roots(cfg).size() == 4);
    }

    TEST_CASE("sandboxed_shell_command wraps commands only where supported") {
        BuildSandbox sb;
        sb.deny_write = {"/p"};
        const auto wrapped = sandboxed_shell_command(sb, "make install");
#ifdef __APPLE__
        REQUIRE(build_sandbox_supported());
        CHECK_FALSE(inside_macos_sandbox());
        CHECK_NOTHROW(require_build_sandbox());
        CHECK(wrapped.starts_with("/usr/bin/sandbox-exec -p '(version 1)"));
        CHECK(wrapped.ends_with(" /bin/sh -c 'make install'"));
#else
        // Linux: no sandbox yet (documented gap).
        CHECK_FALSE(build_sandbox_supported());
        CHECK(wrapped == "make install");
#endif
    }

#ifdef __APPLE__
    TEST_CASE("sandbox-exec allows this build's dirs and denies the rest of den's trees") {
        TempTree t;
        const auto cfg = make_config(t.root);
        const auto sb = build_sandbox_for(cfg, "foo", "1.0");
        for (const auto& d : sb.allow_write)
            fs::create_directories(d);
        const auto old_keg = cfg.store / "foo" / "0.9";
        write(old_keg / "bin" / "foo", "0.9\n");
        fs::create_directories(cfg.prefix / "opt");
        fs::create_directory_symlink("../Cellar/foo/0.9", cfg.prefix / "opt" / "foo");
        const auto build = sb.allow_write[0];
        fs::create_directory_symlink(cfg.prefix / "opt" / "foo", build / "through");
        const auto before = snapshot(old_keg);

        const auto run = [&](const std::string& cmd) {
            return sh(sandboxed_shell_command(sb, cmd) + " 2>/dev/null");
        };
        const auto q = [](const fs::path& p) { return sh_quote(p.string()); };

        // Allowed: the build dir, the staging DESTDIR, the cache temp, and
        // anything outside den's trees.
        for (const auto& d : sb.allow_write) {
            CAPTURE(d);
            CHECK(run("echo ok > " + q(d / "f") + " && mkdir -p " + q(d / "a/b") + " && rm " +
                      q(d / "f")) == 0);
        }
        CHECK(run("echo ok > " + q(t.root / "outside")) == 0);
        CHECK(slurp(t.root / "outside") == "ok\n");

        // Denied: DEN_HOME, the prefix, the live opt path and, through the
        // opt link, the older keg -- creating, overwriting, removing,
        // renaming and chmod alike.
        CHECK(run("echo x > " + q(cfg.den_home / "stray")) != 0);
        CHECK(run("echo x > " + q(cfg.cache / "stray")) != 0);
        CHECK(run("mkdir -p " + q(cfg.prefix / "bin")) != 0);
        CHECK(run("echo x > " + q(cfg.prefix / "opt" / "foo" / "bin" / "foo")) != 0);
        CHECK(run("echo x > " + q(cfg.prefix / "opt" / "foo" / "bin" / "new")) != 0);
        CHECK(run("echo x > " + q(build / "through" / "bin" / "foo")) != 0);
        CHECK(run("rm " + q(old_keg / "bin" / "foo")) != 0);
        CHECK(run("mv " + q(old_keg / "bin" / "foo") + " " + q(build / "stolen")) != 0);
        CHECK(run("chmod 777 " + q(old_keg / "bin" / "foo")) != 0);
        CHECK(run("rm " + q(cfg.prefix / "opt" / "foo")) != 0);

        CHECK_FALSE(fs::exists(cfg.den_home / "stray"));
        CHECK_FALSE(fs::exists(cfg.cache / "stray"));
        CHECK_FALSE(fs::exists(cfg.prefix / "bin"));
        CHECK(fs::read_symlink(cfg.prefix / "opt" / "foo") == fs::path("../Cellar/foo/0.9"));
        CHECK(snapshot(old_keg) == before);
    }
#endif

    TEST_CASE("an install that ignores DESTDIR cannot write through the opt link") {
        TempTree t;
        const auto cfg = make_config(t.root);
        const std::string name = "den-sandbox-through";
        // The live opt link still names the older keg.
        const auto old_keg = cfg.store / name / "0.9";
        write(old_keg / "bin" / "tool", "#!/bin/sh\necho 0.9\n");
        write(old_keg / "lib" / "libtool.txt", "0.9 lib\n");
        fs::create_directory_symlink("libtool.txt", old_keg / "lib" / "alias.txt");
        const auto opt = cfg.prefix / "opt" / name;
        fs::create_directories(opt.parent_path());
        fs::create_directory_symlink("../Cellar/" + name + "/0.9", opt);
        const auto before = snapshot(old_keg);

        // Overwrites a file in, and adds one to, $(PREFIX) -- the opt path.
        const auto idx = fixture(t.root, name, "DenSandboxThrough",
                                 "all:\n"
                                 "install:\n"
                                 "\tmkdir -p $(PREFIX)/bin\n"
                                 "\techo 1.0 > $(PREFIX)/bin/tool\n"
                                 "\techo 1.0 > $(PREFIX)/bin/extra\n"
                                 "\trm -f $(PREFIX)/lib/libtool.txt\n");

        CHECK_THROWS_AS(build_from_source(cfg, idx, name, "1.0"), UserError);
        CHECK(fs::read_symlink(opt) == fs::path("../Cellar/" + name + "/0.9"));
        CHECK_FALSE(fs::exists(cfg.store / name / "1.0"));
#ifdef __APPLE__
        // The sandbox denied every write: the older keg is byte-identical.
        CHECK(snapshot(old_keg) == before);
#else
        // Linux has no sandbox (documented gap): OptPathGuard fails the
        // build, but the write-through already happened.
        CHECK(snapshot(old_keg) != before);
#endif
    }

    TEST_CASE("a normal build succeeds inside the sandbox") {
        TempTree t;
        const auto cfg = make_config(t.root);
        fs::create_directories(cfg.store);
        const std::string name = "den-sandbox-ok";
        const auto probe = cfg.den_home / "sandbox-probe";
        // The build step records whether it could write $DEN_HOME; the
        // install honours DESTDIR.
        const auto idx = fixture(t.root, name, "DenSandboxOk",
                                 "all:\n"
                                 "\tif (echo x > " +
                                     probe.string() +
                                     ") 2>/dev/null; then echo open; else echo denied; fi "
                                     "> probe.txt\n"
                                     "\techo built > out.txt\n"
                                     "install:\n"
                                     "\tmkdir -p $(DESTDIR)$(PREFIX)/share\n"
                                     "\tcp out.txt probe.txt $(DESTDIR)$(PREFIX)/share/\n");

        const auto keg = build_from_source(cfg, idx, name, "1.0");
        CHECK(keg == cfg.store / name / "1.0");
        CHECK(slurp(keg / "share" / "out.txt") == "built\n");
        CHECK_FALSE(fs::exists(cfg.cache / "tmp" / (name + "-1.0")));
#ifdef __APPLE__
        CHECK(slurp(keg / "share" / "probe.txt") == "denied\n");
        CHECK_FALSE(fs::exists(probe));
#else
        CHECK(slurp(keg / "share" / "probe.txt") == "open\n");
#endif
    }

    TEST_CASE("build_cache_env puts every tool cache under the cache temp dir") {
        BuildSandbox sb;
        sb.temp_dir = "/h/cache/tmp/foo-1.2";
        const auto env = build_cache_env(sb);
        CHECK(env.size() == 6);
        CHECK(env.at("GOCACHE") == "/h/cache/tmp/foo-1.2/go-build");
        CHECK(env.at("GOMODCACHE") == "/h/cache/tmp/foo-1.2/go/pkg/mod");
        CHECK(env.at("GOPATH") == "/h/cache/tmp/foo-1.2/go");
        CHECK(env.at("PIP_CACHE_DIR") == "/h/cache/tmp/foo-1.2/pip");
        CHECK(env.at("npm_config_cache") == "/h/cache/tmp/foo-1.2/npm");
        CHECK(env.at("CARGO_HOME") == "/h/cache/tmp/foo-1.2/cargo");
    }

    TEST_CASE("build commands get tool caches they can write in the cache temp dir") {
        TempTree t;
        const auto cfg = make_config(t.root);
        fs::create_directories(cfg.store);
        const std::string name = "den-sandbox-caches";
        // Each cache must be writable under the sandbox; the build records
        // where they pointed.
        const auto idx =
            fixture(t.root, name, "DenSandboxCaches",
                    "VARS = GOCACHE GOMODCACHE GOPATH PIP_CACHE_DIR npm_config_cache CARGO_HOME\n"
                    "all:\n"
                    "\tfor v in $(VARS); do d=$$(printenv $$v); "
                    "test -n \"$$d\" && mkdir -p \"$$d\" && echo x > \"$$d/probe\" && "
                    "echo \"$$v=$$d\" >> caches.txt || exit 1; done\n"
                    "install:\n"
                    "\tmkdir -p $(DESTDIR)$(PREFIX)/share\n"
                    "\tcp caches.txt $(DESTDIR)$(PREFIX)/share/\n");

        const auto keg = build_from_source(cfg, idx, name, "1.0");
        const auto tmp = fs::weakly_canonical(cfg.cache / "tmp" / (name + "-1.0"));
        CHECK(slurp(keg / "share" / "caches.txt") ==
              "GOCACHE=" + (tmp / "go-build").string() + "\n" + "GOMODCACHE=" +
                  (tmp / "go" / "pkg" / "mod").string() + "\n" + "GOPATH=" + (tmp / "go").string() +
                  "\n" + "PIP_CACHE_DIR=" + (tmp / "pip").string() + "\n" + "npm_config_cache=" +
                  (tmp / "npm").string() + "\n" + "CARGO_HOME=" + (tmp / "cargo").string() + "\n");
        // The caches went with the build's temp directory.
        CHECK_FALSE(fs::exists(tmp));
    }

#ifdef __APPLE__
    TEST_CASE("a build step writing outside the allowed dirs fails the build") {
        TempTree t;
        const auto cfg = make_config(t.root);
        fs::create_directories(cfg.store);
        const std::string name = "den-sandbox-stray";
        const auto stray = cfg.den_home / "stray.txt";
        const auto idx = fixture(t.root, name, "DenSandboxStray",
                                 "all:\n"
                                 "\techo x > " +
                                     stray.string() +
                                     "\n"
                                     "install:\n"
                                     "\tmkdir -p $(DESTDIR)$(PREFIX)/share\n"
                                     "\techo ok > $(DESTDIR)$(PREFIX)/share/ok.txt\n");

        CHECK_THROWS_WITH_AS(build_from_source(cfg, idx, name, "1.0"),
                             doctest::Contains("outside the build directory"), UserError);
        CHECK_FALSE(fs::exists(stray));
        CHECK_FALSE(fs::exists(cfg.store / name / "1.0"));
    }

    TEST_CASE("sandbox-exec denies the system roots and allows ~/Library/Developer") {
        TempTree t;
        const auto sys = t.root / "sys";
        ScopedEnv env("DEN_BUILD_SANDBOX_SYSTEM_ROOT", sys.string());
        auto cfg = make_config(t.root);
        cfg.homebrew_prefix = t.root / "brew";
        cfg.homebrew_cellar = t.root / "brew" / "Cellar";
        const auto roots = system_deny_roots(cfg);
        for (const auto& r : roots)
            fs::create_directories(r);
        const auto sb = build_sandbox_for(cfg, "foo", "1.0");
        const auto run = [&](const std::string& cmd) {
            return sh(sandboxed_shell_command(sb, cmd) + " 2>/dev/null");
        };
        for (const auto& r : roots) {
            CAPTURE(r);
            CHECK(run("echo x > " + sh_quote((r / "stray").string())) != 0);
            CHECK(run("mkdir -p " + sh_quote((r / "bin").string())) != 0);
            CHECK_FALSE(fs::exists(r / "stray"));
            CHECK_FALSE(fs::exists(r / "bin"));
        }
        const auto dev = sys / home().relative_path() / "Library" / "Developer";
        CHECK(run("mkdir -p " + sh_quote((dev / "Xcode").string())) == 0);
        CHECK(fs::is_directory(dev / "Xcode"));
    }

    TEST_CASE("a build step writing into any denied system root fails the build") {
        TempTree t;
        const auto sys = t.root / "sys";
        ScopedEnv env("DEN_BUILD_SANDBOX_SYSTEM_ROOT", sys.string());
        auto cfg = make_config(t.root);
        cfg.homebrew_prefix = t.root / "brew";
        cfg.homebrew_cellar = t.root / "brew" / "Cellar";
        fs::create_directories(cfg.store);
        const std::vector<fs::path> targets = {
            sys / "opt" / "homebrew" / "bin" / "stray",
            sys / "opt" / "homebrew" / "Cellar" / "stray" / "1.0" / "stray",
            sys / "usr" / "local" / "bin" / "stray",
            sys / home().relative_path() / "Library" / "stray",
            t.root / "brew" / "bin" / "stray",
            t.root / "brew" / "Cellar" / "stray",
        };
        for (const auto& r : system_deny_roots(cfg))
            fs::create_directories(r);
        int i = 0;
        for (const auto& target : targets) {
            CAPTURE(target);
            const std::string name = "den-sandbox-root" + std::to_string(i);
            const auto idx =
                fixture(t.root / std::to_string(i++), name, "DenSandboxRoot",
                        "all:\n"
                        "\tmkdir -p " +
                            target.parent_path().string() + " && echo x > " + target.string() +
                            "\n"
                            "install:\n"
                            "\tmkdir -p $(DESTDIR)$(PREFIX)/share\n"
                            "\techo ok > $(DESTDIR)$(PREFIX)/share/ok.txt\n");
            CHECK_THROWS_WITH_AS(build_from_source(cfg, idx, name, "1.0"),
                                 doctest::Contains("outside the build directory"), UserError);
            CHECK_FALSE(fs::exists(target));
            CHECK_FALSE(fs::exists(cfg.store / name / "1.0"));
        }

        // And a normal build still succeeds with every root denied.
        const auto idx = fixture(t.root / "ok", "den-sandbox-roots-ok", "DenSandboxRootsOk",
                                 "all:\n"
                                 "\techo built > out.txt\n"
                                 "install:\n"
                                 "\tmkdir -p $(DESTDIR)$(PREFIX)/share\n"
                                 "\tcp out.txt $(DESTDIR)$(PREFIX)/share/\n");
        const auto keg = build_from_source(cfg, idx, "den-sandbox-roots-ok", "1.0");
        CHECK(slurp(keg / "share" / "out.txt") == "built\n");
    }
#endif
}

} // namespace den
