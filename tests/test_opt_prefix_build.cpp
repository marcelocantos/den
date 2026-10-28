// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Opt-prefix source builds (docs/source-builds.md): builds are configured for
// <prefix>/opt/<name>, installed into Cellar/<name>/<version> through DESTDIR,
// and fetch sources under their real archive suffix.

#include <doctest.h>

#include "build/opt_prefix_build.h"
#include "build/source_build.h"
#include "core/config.h"
#include "core/error.h"
#include "download/archive.h"
#include "download/sha256.h"
#include "index/package.h"

#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace den {

namespace {

struct TempTree {
    fs::path root;
    TempTree() {
        std::random_device rd;
        root = fs::temp_directory_path() / ("den-optprefix-" + std::to_string(rd()));
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
    return std::system(cmd.c_str());
}

// A tiny autotools-style project: ./configure bakes --prefix and
// --sysconfdir into the Makefile, and `make install` writes a script that
// prints them. Its Makefile assigns DESTDIR itself (as OpenSSL's does), so
// staging must override it on the make command line.
void write_fixture_project(const fs::path& dir) {
    write(dir / "configure", "#!/bin/sh\n"
                             "prefix=/usr/local\n"
                             "sysconfdir=\n"
                             "for a in \"$@\"; do\n"
                             "  case \"$a\" in\n"
                             "    --prefix=*) prefix=\"${a#--prefix=}\" ;;\n"
                             "    --sysconfdir=*) sysconfdir=\"${a#--sysconfdir=}\" ;;\n"
                             "  esac\n"
                             "done\n"
                             "[ -n \"$sysconfdir\" ] || sysconfdir=\"$prefix/etc\"\n"
                             "sed -e \"s|@prefix@|$prefix|g\" -e \"s|@sysconfdir@|$sysconfdir|g\" "
                             "Makefile.in > Makefile\n");
    fs::permissions(dir / "configure", fs::perms::owner_all | fs::perms::group_read |
                                           fs::perms::group_exec | fs::perms::others_read |
                                           fs::perms::others_exec);
    write(dir / "Makefile.in",
          "prefix = @prefix@\n"
          "sysconfdir = @sysconfdir@\n"
          "DESTDIR =\n"
          "all: den-fixture\n"
          "den-fixture: den-fixture.in\n"
          "\tsed -e 's|[@]prefix[@]|$(prefix)|g' -e 's|[@]sysconfdir[@]|$(sysconfdir)|g' "
          "den-fixture.in > den-fixture\n"
          "install: den-fixture\n"
          "\tmkdir -p $(DESTDIR)$(prefix)/bin $(DESTDIR)$(prefix)/share/den-fixture "
          "$(DESTDIR)$(sysconfdir)\n"
          "\tcp den-fixture $(DESTDIR)$(prefix)/bin/den-fixture\n"
          "\tchmod 755 $(DESTDIR)$(prefix)/bin/den-fixture\n"
          "\tcp den-fixture $(DESTDIR)$(prefix)/bin/remove-me\n"
          "\techo 'prefix=$(prefix)' > $(DESTDIR)$(prefix)/share/den-fixture/paths.txt\n"
          "\techo 'conf' > $(DESTDIR)$(sysconfdir)/den-fixture.conf\n");
    write(dir / "den-fixture.in", "#!/bin/sh\necho 'prefix=@prefix@ sysconfdir=@sysconfdir@'\n");
}

// Pack the fixture project as <out> with tar's `flag` (J = xz, j = bzip2).
fs::path pack_fixture(const fs::path& work, const std::string& flag, const std::string& file) {
    const auto src = work / "src";
    fs::remove_all(src);
    write_fixture_project(src / "den-fixture-1.0");
    const auto out = work / file;
    REQUIRE(sh("tar -c" + flag + "f " + sh_quote(out.string()) + " -C " + sh_quote(src.string()) +
               " den-fixture-1.0") == 0);
    return out;
}

std::string fixture_formula(const std::string& klass, const fs::path& archive) {
    return "class " + klass +
           " < Formula\n"
           "  desc \"den opt-prefix build fixture\"\n"
           "  homepage \"https://example.invalid/den-fixture\"\n"
           "  url \"file://" +
           archive.string() +
           "\"\n"
           "  sha256 \"" +
           hash_file(archive) +
           "\"\n"
           "\n"
           "  def install\n"
           "    system \"./configure\", \"--prefix=#{prefix}\", \"--sysconfdir=#{etc}\"\n"
           "    system \"make\"\n"
           "    system \"make\", \"install\"\n"
           "    rm bin/\"remove-me\"\n"
           "    pkgshare.mkpath\n"
           "    (pkgshare/\"ruby-step.txt\").write \"ok\\n\"\n"
           "  end\n"
           "end\n";
}

Config make_config(const fs::path& root) {
    Config c;
    c.den_home = root / "home";
    c.cache = c.den_home / "cache";
    c.set_prefix(root / "prefix", PrefixSource::EnvDenPrefix);
    return c;
}

// Every regular file under `dir` that mentions `needle`.
std::vector<std::string> files_mentioning(const fs::path& dir, const std::string& needle) {
    std::vector<std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (e.is_regular_file() && !e.is_symlink() &&
            slurp(e.path()).find(needle) != std::string::npos)
            out.push_back(e.path().string());
    }
    return out;
}

// Build `name` from a fixture archive through build_from_source, and check
// the opt-prefix contract.
void build_and_check(const Config& cfg, const fs::path& work, const std::string& name,
                     const std::string& klass, const fs::path& archive, const std::string& suffix) {
    const auto formula = work / (name + ".rb");
    write(formula, fixture_formula(klass, archive));

    PackageIndex idx;
    Package pkg;
    pkg.name = name;
    pkg.version = "1.0";
    pkg.ruby_source_path = formula.string();
    idx.packages[name] = pkg;

    const auto opt = cfg.prefix / "opt" / name;
    const auto keg = cfg.store / name / "1.0";

    auto built = build_from_source(cfg, idx, name, "1.0");
    CHECK(built == keg);

    // The keg is installed versioned.
    REQUIRE(fs::is_regular_file(keg / "bin" / "den-fixture"));
    CHECK(fs::is_regular_file(keg / "share" / "den-fixture" / "paths.txt"));

    // Baked paths are opt-based, never the versioned keg.
    const auto script = slurp(keg / "bin" / "den-fixture");
    INFO("script: " << script);
    CHECK(script.find("prefix=" + opt.string() + " ") != std::string::npos);
    CHECK(slurp(keg / "share" / "den-fixture" / "paths.txt") == "prefix=" + opt.string() + "\n");
    CHECK(files_mentioning(keg, keg.string()).empty());

    // Nothing staged was left behind and the build never wrote the opt path.
    CHECK_FALSE(fs::exists(cfg.cache / "build" / (name + "-1.0.destdir")));

    // The source was cached under its real archive suffix.
    CHECK(fs::exists(cfg.cache / "sources" / (name + "-1.0" + suffix)));
    CHECK_FALSE(fs::exists(cfg.cache / "sources" / (name + "-1.0.tar.gz")));

#ifdef __APPLE__
    // macOS builds through the bundled Ruby (den_build.rb): the formula's
    // Ruby steps after `make install` see the merged keg, and etc is the
    // shared <prefix>/etc.
    CHECK_FALSE(fs::exists(keg / "bin" / "remove-me"));
    CHECK(slurp(keg / "share" / name / "ruby-step.txt") == "ok\n");
    CHECK(script.find("sysconfdir=" + (cfg.prefix / "etc").string() + "'") != std::string::npos);
    CHECK(slurp(cfg.prefix / "etc" / "den-fixture.conf") == "conf\n");
#else
    // Linux builds through the native fallback (generic autotools).
    CHECK(script.find("sysconfdir=" + (opt / "etc").string() + "'") != std::string::npos);
    CHECK(fs::is_regular_file(keg / "etc" / "den-fixture.conf"));
#endif
}

} // namespace

TEST_SUITE("opt_prefix_build") {

    TEST_CASE("source_archive_suffix keeps the URL's archive type") {
        CHECK(source_archive_suffix("https://x.org/a-1.0.tar.gz") == ".tar.gz");
        CHECK(source_archive_suffix("https://x.org/a-1.0.tar.xz") == ".tar.xz");
        CHECK(source_archive_suffix("https://x.org/a-1.0.tar.bz2") == ".tar.bz2");
        CHECK(source_archive_suffix("https://x.org/a-1.0.TAR.XZ") == ".tar.xz");
        CHECK(source_archive_suffix("https://x.org/a-1.0.tar.zst") == ".tar.zst");
        CHECK(source_archive_suffix("https://x.org/a-1.0.tgz") == ".tgz");
        CHECK(source_archive_suffix("https://x.org/a-1.0.zip") == ".zip");
        CHECK(source_archive_suffix("https://x.org/a-1.0.tar.xz?raw=true#frag") == ".tar.xz");
        CHECK(source_archive_suffix("file:///tmp/a-1.0.tar.bz2") == ".tar.bz2");
        // No recognisable suffix: a neutral name, never a false ".tar.gz".
        CHECK(source_archive_suffix("https://x.org/download/a-1.2.3") == ".src");
        CHECK(source_archive_suffix("https://x.org/archive/") == ".src");
        CHECK(source_archive_path("/c", "git", "2.51.0", "https://k.org/git-2.51.0.tar.xz") ==
              fs::path("/c/sources/git-2.51.0.tar.xz"));
    }

    TEST_CASE("install steps are recognised and staged through DESTDIR") {
        CHECK(is_install_step("make install"));
        CHECK(is_install_step("make -j1 install_sw"));
        CHECK(is_install_step("gmake install-strip"));
        CHECK(is_install_step("V=1 make install"));
        CHECK(is_install_step("./configure --prefix=/p && make && make install"));
        CHECK(is_install_step("cmake --install build"));
        CHECK(is_install_step("cmake --build build --target install"));
        CHECK(is_install_step("ninja -C build install"));
        CHECK(is_install_step("meson install -C build"));
        CHECK_FALSE(is_install_step("make"));
        CHECK_FALSE(is_install_step("make check"));
        CHECK_FALSE(is_install_step("./configure --prefix=/p"));
        CHECK_FALSE(is_install_step("cmake --build build"));
        CHECK_FALSE(is_install_step("install -m 644 foo /p"));

        CHECK(with_destdir("make install", "/s") == "make install DESTDIR='/s'");
        CHECK(with_destdir("cmake --install build", "/s") == "DESTDIR='/s' cmake --install build");
        CHECK(with_destdir("./configure --prefix=/p && make && make install", "/s") ==
              "./configure --prefix=/p && make && make install DESTDIR='/s'");
        CHECK(with_destdir("make", "/s") == "make");
    }

    TEST_CASE("merge_destdir_stage moves the staged opt tree into the keg") {
        TempTree t;
        const auto prefix = t.root / "prefix";
        const auto opt = prefix / "opt" / "foo";
        const auto keg = prefix / "Cellar" / "foo" / "1.0";
        const auto stage = t.root / "stage";
        const auto staged = stage / opt.relative_path();

        write(keg / "bin" / "old", "old");
        write(keg / "lib" / "same", "before");
        write(staged / "bin" / "foo", "foo");
        write(staged / "lib" / "same", "after");
        fs::create_directory_symlink("lib", staged / "lib64");
        write(stage / (prefix / "etc" / "foo.conf").relative_path(), "staged conf");
        write(stage / (prefix / "etc" / "new.conf").relative_path(), "new conf");
        write(prefix / "etc" / "foo.conf", "user conf");
        write(stage / (prefix / "var" / "foo" / "db").relative_path(), "db");

        merge_destdir_stage(stage, opt, keg, prefix);

        CHECK(slurp(keg / "bin" / "old") == "old");
        CHECK(slurp(keg / "bin" / "foo") == "foo");
        CHECK(slurp(keg / "lib" / "same") == "after");
        CHECK(fs::is_symlink(keg / "lib64"));
        CHECK(slurp(prefix / "etc" / "foo.conf") == "user conf"); // never clobbered
        CHECK(slurp(prefix / "etc" / "new.conf") == "new conf");
        CHECK(slurp(prefix / "var" / "foo" / "db") == "db");
        CHECK_FALSE(fs::exists(stage));
        CHECK_FALSE(fs::exists(opt));

        // A missing stage is a no-op.
        CHECK_NOTHROW(merge_destdir_stage(stage, opt, keg, prefix));
    }

    TEST_CASE("merge_destdir_stage refuses files staged outside the keg") {
        TempTree t;
        const auto prefix = t.root / "prefix";
        const auto opt = prefix / "opt" / "foo";
        const auto keg = prefix / "Cellar" / "foo" / "1.0";
        const auto stage = t.root / "stage";
        write(stage / opt.relative_path() / "bin" / "foo", "foo");
        write(stage / "usr" / "local" / "bin" / "stray", "stray");
        write(stage / (prefix / "share" / "stray").relative_path(), "stray");

        try {
            merge_destdir_stage(stage, opt, keg, prefix);
            FAIL("expected UserError");
        } catch (const UserError& e) {
            const std::string msg = e.what();
            CHECK(msg.find("/usr/local/bin/stray") != std::string::npos);
            CHECK(msg.find((prefix / "share" / "stray").string()) != std::string::npos);
        }
        CHECK_FALSE(fs::exists(stage));
        CHECK_FALSE(fs::exists(keg / "bin" / "foo"));
        CHECK_FALSE(fs::exists(prefix / "share" / "stray"));
    }

    TEST_CASE("OptPathGuard catches a build writing the live opt path") {
        TempTree t;
        const auto prefix = t.root / "prefix";
        const auto opt = prefix / "opt" / "foo";
        const auto old_keg = prefix / "Cellar" / "foo" / "0.9";
        write(old_keg / "bin" / "foo", "0.9");

        SUBCASE("absent opt path created by the build is removed") {
            OptPathGuard guard(opt);
            write(opt / "bin" / "foo", "1.0");
            CHECK_THROWS_AS(guard.check(), UserError);
            CHECK_FALSE(fs::exists(fs::symlink_status(opt)));
        }
        SUBCASE("an untouched opt link passes") {
            fs::create_directories(opt.parent_path());
            fs::create_directory_symlink("../Cellar/foo/0.9", opt);
            OptPathGuard guard(opt);
            CHECK_NOTHROW(guard.check());
        }
        SUBCASE("writing through the opt link into the older keg fails") {
            fs::create_directories(opt.parent_path());
            fs::create_directory_symlink("../Cellar/foo/0.9", opt);
            OptPathGuard guard(opt);
            write(opt / "bin" / "bar", "1.0");
            CHECK_THROWS_AS(guard.check(), UserError);
        }
        SUBCASE("repointing the opt link fails") {
            fs::create_directories(opt.parent_path());
            fs::create_directory_symlink("../Cellar/foo/0.9", opt);
            OptPathGuard guard(opt);
            fs::remove(opt);
            fs::create_directory_symlink("../Cellar/foo/1.0", opt);
            CHECK_THROWS_AS(guard.check(), UserError);
        }
        SUBCASE("absent and still absent passes") {
            OptPathGuard guard(opt);
            CHECK_NOTHROW(guard.check());
        }
    }

    TEST_CASE("extract_archive unpacks .tar.xz and .tar.bz2 sources") {
        TempTree t;
        for (const auto& [flag, file] : {std::pair<std::string, std::string>{"J", "f.tar.xz"},
                                         std::pair<std::string, std::string>{"j", "f.tar.bz2"}}) {
            CAPTURE(file);
            const auto archive = pack_fixture(t.root, flag, file);
            const auto out = t.root / ("out-" + file);
            fs::create_directories(out);
            auto res = extract_archive(archive, out);
            CHECK(res.root == fs::path("den-fixture-1.0"));
            CHECK(fs::is_regular_file(out / "den-fixture-1.0" / "configure"));
            CHECK(slurp(out / "den-fixture-1.0" / "den-fixture.in").find("@prefix@") !=
                  std::string::npos);
        }
    }

    TEST_CASE("a build that ignores DESTDIR fails and leaves no opt path or keg") {
        TempTree t;
        const auto cfg = make_config(t.root);
        fs::create_directories(cfg.store);
        const std::string name = "den-fixture-nodestdir";

        const auto src = t.root / "src" / "nodestdir-1.0";
        write(src / "Makefile", "install:\n\tmkdir -p $(PREFIX)/bin\n"
                                "\techo hi > $(PREFIX)/bin/nodestdir\n");
        const auto archive = t.root / "nodestdir-1.0.tar.xz";
        REQUIRE(sh("tar -cJf " + sh_quote(archive.string()) + " -C " +
                   sh_quote((t.root / "src").string()) + " nodestdir-1.0") == 0);
        const auto formula = t.root / (name + ".rb");
        write(formula, "class DenFixtureNodestdir < Formula\n"
                       "  desc \"ignores DESTDIR\"\n"
                       "  homepage \"https://example.invalid/nodestdir\"\n"
                       "  url \"file://" +
                           archive.string() + "\"\n  sha256 \"" + hash_file(archive) +
                           "\"\n\n  def install\n"
                           "    system \"make\", \"install\", \"PREFIX=#{prefix}\"\n"
                           "  end\nend\n");

        PackageIndex idx;
        Package pkg;
        pkg.name = name;
        pkg.version = "1.0";
        pkg.ruby_source_path = formula.string();
        idx.packages[name] = pkg;

        CHECK_THROWS_WITH_AS(build_from_source(cfg, idx, name, "1.0"),
                             doctest::Contains("ignored DESTDIR"), UserError);
        CHECK_FALSE(fs::exists(fs::symlink_status(cfg.prefix / "opt" / name)));
        CHECK_FALSE(fs::exists(cfg.store / name / "1.0"));
    }

    TEST_CASE("fixture formula builds opt-based into a versioned keg (.tar.xz, .tar.bz2)") {
        TempTree t;
        const auto cfg = make_config(t.root);
        fs::create_directories(cfg.store);

        SUBCASE("tar.xz, no opt link yet") {
            const auto archive = pack_fixture(t.root, "J", "den-fixture-1.0.tar.xz");
            build_and_check(cfg, t.root, "den-fixture-xz", "DenFixtureXz", archive, ".tar.xz");
            CHECK_FALSE(fs::exists(fs::symlink_status(cfg.prefix / "opt" / "den-fixture-xz")));
        }
        SUBCASE("tar.bz2, opt link still on an older keg") {
            const auto old_keg = cfg.store / "den-fixture-bz2" / "0.9";
            write(old_keg / "bin" / "den-fixture", "#!/bin/sh\necho old\n");
            const auto opt = cfg.prefix / "opt" / "den-fixture-bz2";
            fs::create_directories(opt.parent_path());
            fs::create_directory_symlink("../Cellar/den-fixture-bz2/0.9", opt);

            const auto archive = pack_fixture(t.root, "j", "den-fixture-1.0.tar.bz2");
            build_and_check(cfg, t.root, "den-fixture-bz2", "DenFixtureBz2", archive, ".tar.bz2");

            // The live link and the keg it names were not touched.
            CHECK(fs::read_symlink(opt) == fs::path("../Cellar/den-fixture-bz2/0.9"));
            CHECK(slurp(old_keg / "bin" / "den-fixture") == "#!/bin/sh\necho old\n");
            CHECK_FALSE(fs::exists(old_keg / "share"));
        }
    }
}

} // namespace den
