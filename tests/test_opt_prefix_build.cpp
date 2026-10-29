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
#include "store/store.h"

#include <cstdlib>
#include <fstream>
#include <map>
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

#ifdef __APPLE__
// git's macOS resources in miniature: two Perl modules (tar.gz with a
// top-level directory, staged with a block) and the html and man pages
// (flat tar.xz, installed straight into a directory).
struct ResourceFixture {
    fs::path sasl, smtp, html, man;
};

ResourceFixture pack_resources(const fs::path& work) {
    const auto src = work / "res-src";
    write(src / "Authen-SASL-2.1700" / "lib" / "Authen" / "SASL.pm", "package Authen::SASL;\n1;\n");
    write(src / "Net-SMTP-SSL-1.04" / "lib" / "Net" / "SMTP" / "SSL.pm",
          "package Net::SMTP::SSL;\n1;\n");
    write(src / "html" / "git.html", "<html>git</html>\n");
    write(src / "html" / "RelNotes" / "1.0.txt", "notes\n");
    write(src / "man" / "man1" / "git.1", ".TH GIT 1\n");
    write(src / "man" / "man5" / "gitattributes.5", ".TH GITATTRIBUTES 5\n");
    ResourceFixture r{work / "Authen-SASL-2.1700.tar.gz", work / "Net-SMTP-SSL-1.04.tar.gz",
                      work / "git-htmldocs-1.0.tar.xz", work / "git-manpages-1.0.tar.xz"};
    REQUIRE(sh("tar -czf " + sh_quote(r.sasl.string()) + " -C " + sh_quote(src.string()) +
               " Authen-SASL-2.1700") == 0);
    REQUIRE(sh("tar -czf " + sh_quote(r.smtp.string()) + " -C " + sh_quote(src.string()) +
               " Net-SMTP-SSL-1.04") == 0);
    REQUIRE(sh("tar -cJf " + sh_quote(r.html.string()) + " -C " +
               sh_quote((src / "html").string()) + " .") == 0);
    REQUIRE(sh("tar -cJf " + sh_quote(r.man.string()) + " -C " + sh_quote((src / "man").string()) +
               " .") == 0);
    return r;
}

std::string resource_block(const std::string& name, const fs::path& archive, const std::string& sha,
                           const std::string& version = "") {
    return "  resource \"" + name + "\" do\n    url \"file://" + archive.string() + "\"\n" +
           (version.empty() ? "" : "    version \"" + version + "\"\n") + "    sha256 \"" + sha +
           "\"\n  end\n\n";
}

// The resource half of git's install method, unchanged in shape.
std::string resource_formula(const std::string& klass, const fs::path& archive,
                             const ResourceFixture& r, const std::string& smtp_sha) {
    return "class " + klass +
           " < Formula\n"
           "  desc \"den resource staging fixture\"\n"
           "  homepage \"https://example.invalid/den-fixture\"\n"
           "  url \"file://" +
           archive.string() + "\"\n  version \"1.0\"\n  sha256 \"" + hash_file(archive) + "\"\n\n" +
           resource_block("Authen::SASL", r.sasl, hash_file(r.sasl)) +
           resource_block("html", r.html, hash_file(r.html), "1.0") +
           resource_block("man", r.man, hash_file(r.man), "1.0") +
           resource_block("Net::SMTP::SSL", r.smtp, smtp_sha) +
           "  def install\n"
           "    odie \"html resource needs to be updated\" if build.stable? && version != "
           "resource(\"html\").version\n"
           "    odie \"man resource needs to be updated\" if build.stable? && version != "
           "resource(\"man\").version\n"
           "    system \"./configure\", \"--prefix=#{prefix}\", \"--sysconfdir=#{etc}\"\n"
           "    system \"make\", \"install\"\n"
           "    man.install resource(\"man\")\n"
           "    (share/\"doc/git-doc\").install resource(\"html\")\n"
           "    resource(\"Net::SMTP::SSL\").stage do\n"
           "      (share/\"perl5\").install \"lib/Net\"\n"
           "    end\n"
           "    resource(\"Authen::SASL\").stage do\n"
           "      (share/\"perl5\").install \"lib/Authen\"\n"
           "    end\n"
           "  end\n"
           "end\n";
}
#endif

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
        SUBCASE("a stale link to the keg being built is checked by target only") {
            const auto new_keg = prefix / "Cellar" / "foo" / "1.0";
            fs::create_directories(opt.parent_path());
            fs::create_directory_symlink("../Cellar/foo/1.0", opt); // dangling
            OptPathGuard guard(opt, new_keg);
            write(new_keg / "bin" / "foo", "1.0"); // den fills its own keg
            CHECK_NOTHROW(guard.check());
            fs::remove(opt);
            fs::create_directory_symlink("../Cellar/foo/0.9", opt);
            CHECK_THROWS_AS(guard.check(), UserError);
        }
        SUBCASE("a link to another keg is still listed") {
            fs::create_directories(opt.parent_path());
            fs::create_directory_symlink("../Cellar/foo/0.9", opt);
            OptPathGuard guard(opt, prefix / "Cellar" / "foo" / "1.0");
            write(opt / "bin" / "bar", "1.0");
            CHECK_THROWS_AS(guard.check(), UserError);
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

    TEST_CASE(
        "a fixed-cellar formula whose build ignores DESTDIR fails, leaving no opt path or keg") {
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
        // Its bottles bake the prefix, so den must not retry into a staging
        // keg and relocate.
        Package::Archive fixed;
        fixed.relocation = RelocationType::HardcodedPrefix;
        pkg.archives["arm64_sequoia"] = fixed;
        pkg.archives["x86_64_linux"] = fixed;
        idx.packages[name] = pkg;
        REQUIRE(bakes_fixed_prefix(idx, name));

        CHECK_THROWS_WITH_AS(build_from_source(cfg, idx, name, "1.0"),
                             doctest::Contains("ignored DESTDIR"), UserError);
        CHECK_FALSE(fs::exists(fs::symlink_status(cfg.prefix / "opt" / name)));
        CHECK_FALSE(fs::exists(cfg.store / name / "1.0"));
    }

    TEST_CASE("an install step that ignores DESTDIR retries into a staging keg (tree's Makefile)") {
        // tree 2.3.2's Makefile in miniature: DESTDIR is its bin directory
        // and MANDIR an absolute path, so DESTDIR staging puts the binary
        // loose in the stage and the man page in the live opt path.
        TempTree t;
        const auto cfg = make_config(t.root);
        fs::create_directories(cfg.store);
        const std::string name = "den-fixture-treelike";

        const auto src = t.root / "src" / "treelike-1.0";
        write(src / "Makefile", "PREFIX=/usr/local\n"
                                "DESTDIR=${PREFIX}/bin\n"
                                "MANDIR=${PREFIX}/man\n"
                                "all: treelike\n"
                                "treelike:\n"
                                "\techo '#!/bin/sh' > treelike\n"
                                "\techo 'echo treelike' >> treelike\n"
                                "install: treelike\n"
                                "\tinstall -d $(DESTDIR)\n"
                                "\tinstall -m 755 treelike $(DESTDIR)/treelike\n"
                                "\tinstall -d $(MANDIR)/man1\n"
                                "\techo '.TH TREELIKE 1' > $(MANDIR)/man1/treelike.1\n"
                                "\tinstall -d $(PREFIX)/share/treelike\n"
                                "\techo 'prefix=$(PREFIX)' > $(PREFIX)/share/treelike/paths.txt\n"
                                "\tln -s $(PREFIX)/bin/treelike $(PREFIX)/share/treelike/link\n");
        const auto archive = t.root / "treelike-1.0.tar.xz";
        REQUIRE(sh("tar -cJf " + sh_quote(archive.string()) + " -C " +
                   sh_quote((t.root / "src").string()) + " treelike-1.0") == 0);
        const auto formula = t.root / (name + ".rb");
        write(formula, "class DenFixtureTreelike < Formula\n"
                       "  desc \"ignores DESTDIR, as tree does\"\n"
                       "  homepage \"https://example.invalid/treelike\"\n"
                       "  url \"file://" +
                           archive.string() + "\"\n  sha256 \"" + hash_file(archive) +
                           "\"\n\n  def install\n"
                           "    system \"make\", \"install\", \"PREFIX=#{prefix}\", "
                           "\"MANDIR=#{man}\"\n"
                           "  end\nend\n");

        PackageIndex idx;
        Package pkg;
        pkg.name = name;
        pkg.version = "1.0";
        pkg.ruby_source_path = formula.string();
        // tree's bottles are :any_skip_relocation.
        Package::Archive any;
        any.relocation = RelocationType::Portable;
        pkg.archives["arm64_sequoia"] = any;
        pkg.archives["x86_64_linux"] = any;
        idx.packages[name] = pkg;
        REQUIRE_FALSE(bakes_fixed_prefix(idx, name));

        const auto opt = cfg.prefix / "opt" / name;
        const auto keg = cfg.store / name / "1.0";
        CHECK(build_from_source(cfg, idx, name, "1.0") == keg);

        CHECK(fs::is_regular_file(keg / "bin" / "treelike"));
        CHECK((fs::status(keg / "bin" / "treelike").permissions() & fs::perms::owner_exec) !=
              fs::perms::none);
        CHECK(fs::is_regular_file(keg / "share" / "man" / "man1" / "treelike.1"));
        // Text naming the staging keg was relocated to the opt path.
        CHECK(slurp(keg / "share" / "treelike" / "paths.txt") == "prefix=" + opt.string() + "\n");
        CHECK(fs::read_symlink(keg / "share" / "treelike" / "link") == opt / "bin" / "treelike");
        CHECK(files_mentioning(keg, ".destdir").empty());
        // Nothing loose in the stage, and the live opt path was never made.
        CHECK_FALSE(fs::exists(fs::symlink_status(opt)));
        CHECK_FALSE(fs::exists(cfg.cache / "build" / (name + "-1.0.destdir")));
    }

    TEST_CASE("with_staged_keg retargets whole opt paths only") {
        const fs::path opt = "/p/opt/tree";
        const fs::path sk = "/h/cache/build/tree-1.destdir/p/opt/tree";
        CHECK(with_staged_keg("make install PREFIX=/p/opt/tree MANDIR=/p/opt/tree/share/man", opt,
                              sk) ==
              "make install PREFIX=" + sk.string() + " MANDIR=" + sk.string() + "/share/man");
        CHECK(with_staged_keg("make install PREFIX='/p/opt/tree'", opt, sk) ==
              "make install PREFIX='" + sk.string() + "'");
        CHECK(with_staged_keg("make install PREFIX=/p/opt/tree-extra", opt, sk) ==
              "make install PREFIX=/p/opt/tree-extra");
    }

    TEST_CASE("relocate_staged_keg rewrites text and refuses binaries") {
        TempTree t;
        const auto sk = t.root / "stage" / "p" / "opt" / "x";
        const fs::path opt = "/p/opt/x";
        write(sk / "share" / "x.pc",
              "prefix=" + sk.string() + "\nlibdir=" + sk.string() + "/lib\n");
        fs::create_symlink(sk / "share" / "x.pc", sk / "share" / "link");
        relocate_staged_keg(sk, opt);
        CHECK(slurp(sk / "share" / "x.pc") == "prefix=/p/opt/x\nlibdir=/p/opt/x/lib\n");
        CHECK(fs::read_symlink(sk / "share" / "link") == opt / "share" / "x.pc");

        write(sk / "bin" / "x", std::string("\x7f"
                                            "ELF\0\0",
                                            6) +
                                    sk.string());
        CHECK_THROWS_WITH_AS(relocate_staged_keg(sk, opt), doctest::Contains("bin/x"), UserError);
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

#ifdef __APPLE__
    TEST_CASE("formula resources stage from den's cache, as git's macOS resources do") {
        TempTree t;
        const auto cfg = make_config(t.root);
        fs::create_directories(cfg.store);
        const std::string name = "den-fixture-res";
        const auto archive = pack_fixture(t.root, "J", "den-fixture-1.0.tar.xz");
        const auto res = pack_resources(t.root);
        const auto formula = t.root / (name + ".rb");
        write(formula, resource_formula("DenFixtureRes", archive, res, hash_file(res.smtp)));

        PackageIndex idx;
        Package pkg;
        pkg.name = name;
        pkg.version = "1.0";
        pkg.ruby_source_path = formula.string();
        idx.packages[name] = pkg;

        const auto keg = cfg.store / name / "1.0";
        REQUIRE(build_from_source(cfg, idx, name, "1.0") == keg);

        // Block staging runs inside the module's top-level directory.
        CHECK(slurp(keg / "share" / "perl5" / "Authen" / "SASL.pm") ==
              "package Authen::SASL;\n1;\n");
        CHECK(slurp(keg / "share" / "perl5" / "Net" / "SMTP" / "SSL.pm") ==
              "package Net::SMTP::SSL;\n1;\n");
        // Target staging moves a flat archive's contents into the directory.
        CHECK(slurp(keg / "share" / "man" / "man1" / "git.1") == ".TH GIT 1\n");
        CHECK(slurp(keg / "share" / "man" / "man5" / "gitattributes.5") == ".TH GITATTRIBUTES 5\n");
        CHECK(slurp(keg / "share" / "doc" / "git-doc" / "git.html") == "<html>git</html>\n");
        CHECK(slurp(keg / "share" / "doc" / "git-doc" / "RelNotes" / "1.0.txt") == "notes\n");
        // The main build still ran.
        CHECK(fs::is_regular_file(keg / "bin" / "den-fixture"));

        // Each resource was cached under den's source cache, with its archive
        // suffix, and its staging directory is gone.
        const auto sources = cfg.cache / "sources";
        CHECK(hash_file(sources / (name + "-1.0--Authen-SASL.tar.gz")) == hash_file(res.sasl));
        CHECK(hash_file(sources / (name + "-1.0--Net-SMTP-SSL.tar.gz")) == hash_file(res.smtp));
        CHECK(hash_file(sources / (name + "-1.0--html.tar.xz")) == hash_file(res.html));
        CHECK(hash_file(sources / (name + "-1.0--man.tar.xz")) == hash_file(res.man));
        CHECK_FALSE(fs::exists(cfg.cache / "build" / (name + "-1.0.resources")));

        // The receipt lists all four, verified.
        const auto receipt = read_source_build_receipt(keg);
        REQUIRE(receipt.has_value());
        REQUIRE(receipt->resources.size() == 4);
        std::map<std::string, std::string> got;
        for (const auto& in : receipt->resources)
            got[in.name] = in.sha256;
        CHECK(got["Authen::SASL"] == hash_file(res.sasl));
        CHECK(got["Net::SMTP::SSL"] == hash_file(res.smtp));
        CHECK(got["html"] == hash_file(res.html));
        CHECK(got["man"] == hash_file(res.man));
    }

    TEST_CASE("a resource that does not match its sha256 fails the build before it starts") {
        TempTree t;
        const auto cfg = make_config(t.root);
        fs::create_directories(cfg.store);
        const std::string name = "den-fixture-badres";
        const auto archive = pack_fixture(t.root, "J", "den-fixture-1.0.tar.xz");
        const auto res = pack_resources(t.root);
        const auto formula = t.root / (name + ".rb");
        write(formula, resource_formula("DenFixtureBadres", archive, res, std::string(64, 'a')));

        PackageIndex idx;
        Package pkg;
        pkg.name = name;
        pkg.version = "1.0";
        pkg.ruby_source_path = formula.string();
        idx.packages[name] = pkg;

        CHECK_THROWS_WITH_AS(
            build_from_source(cfg, idx, name, "1.0"),
            doctest::Contains("SHA256 mismatch for " + name + " resource Net::SMTP::SSL"),
            UserError);
        CHECK_FALSE(fs::exists(cfg.store / name / "1.0"));
        CHECK_FALSE(fs::exists(cfg.cache / "sources" / (name + "-1.0--Net-SMTP-SSL.tar.gz")));
    }
#endif
}

} // namespace den
