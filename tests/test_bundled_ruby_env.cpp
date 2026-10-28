// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// `den install -s` runs den_build.rb under the bundled Ruby. The bundle's
// homebrew_env.txt snapshot was captured on the machine that built the
// bundle, so it hardcodes /opt/homebrew (prefix, Cellar, library, bootsnap
// gem, brew file) and that machine's home. These tests pin that the build
// environment comes from den's configuration instead — in the default layout
// (<den_home>/prefix) and the migrate layout (den's prefix is Homebrew's) —
// and that -s builds of a tap formula and a core-shaped formula complete
// through the bundled Ruby in a scratch prefix.

#include "../src/build/source_build.h"
#include "../src/download/sha256.h"
#include "../src/index/package.h"
#include "../src/ruby/bundle.h"
#include "../src/ruby/ruby_env.h"

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace bundled_ruby_env_test {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        // Under /tmp, not $TMPDIR: the bundled Ruby is relocated in place and
        // its new path must be shorter than the original prefix.
        std::string tmpl = "/tmp/den_rbenv_XXXXXX";
        char* result = ::mkdtemp(tmpl.data());
        if (!result)
            throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
        path = fs::canonical(result);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string run(const std::string& cmd) {
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen failed: " + cmd);
    std::string out;
    std::array<char, 1024> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        out += buf.data();
    ::pclose(pipe);
    return out;
}

// The shape of the real snapshot: brew's machine paths plus constants.
const char* kSnapshot = "HOMEBREW_PREFIX=/opt/homebrew\n"
                        "HOMEBREW_CELLAR=/opt/homebrew/Cellar\n"
                        "HOMEBREW_REPOSITORY=/opt/homebrew\n"
                        "HOMEBREW_DEFAULT_PREFIX=/opt/homebrew\n"
                        "HOMEBREW_LIBRARY=/opt/homebrew/Library\n"
                        "HOMEBREW_BOOTSNAP_GEM_PATH=/opt/homebrew/Library/Homebrew/vendor/"
                        "portable-ruby/current/lib/ruby/gems/4.0.0/gems/bootsnap-1.23.0/lib/"
                        "bootsnap\n"
                        "HOMEBREW_BREWED_CURL_PATH=/opt/homebrew/opt/curl/bin/curl\n"
                        "HOMEBREW_BREW_FILE=/opt/homebrew/bin/brew\n"
                        "HOMEBREW_GIT_CONFIG_FILE=/opt/homebrew/.git/config\n"
                        "HOMEBREW_CACHE=/Users/someone/Library/Caches/Homebrew\n"
                        "HOMEBREW_DEFAULT_CACHE=/Users/someone/Library/Caches/Homebrew\n"
                        "HOMEBREW_LOGS=/Users/someone/Library/Logs/Homebrew\n"
                        "HOMEBREW_PATH=/opt/homebrew/bin:/usr/bin:/bin\n"
                        "HOMEBREW_PYENV_ROOT=/Users/someone/.pyenv\n"
                        "HOMEBREW_USER_CONFIG_HOME=/Users/someone/.homebrew\n"
                        "HOMEBREW_HELP_MESSAGE=Example usage:\n"
                        "  brew search TEXT|/REGEX/\n"
                        "HOMEBREW_VERSION=5.1.1\n"
                        "HOMEBREW_USER_AGENT=Homebrew/5.1.1 (Macintosh; arm64 Mac OS X 26.3.1)\n"
                        "HOMEBREW_API_DEFAULT_DOMAIN=https://formulae.brew.sh/api\n"
                        "HOMEBREW_PROCESSOR=arm64\n"
                        "# comment\n"
                        "\n";

EnvMap snapshot() {
    std::istringstream in(kSnapshot);
    return parse_env_snapshot(in);
}

Config make_config(const fs::path& den_home, const fs::path& prefix,
                   const fs::path& homebrew_prefix) {
    Config c;
    c.den_home = den_home;
    c.cache = den_home / "cache";
    c.set_prefix(prefix, PrefixSource::EnvDenPrefix);
    c.homebrew_prefix = homebrew_prefix;
    c.homebrew_cellar = homebrew_prefix / "Cellar";
    return c;
}

void check_no_opt_homebrew(const EnvMap& env) {
    for (const auto& [k, v] : env) {
        INFO(k << "=" << v);
        CHECK(v.find("/opt/homebrew") == std::string::npos);
    }
}

// Every HOMEBREW_* global.rb (and friends) reads with ENV.fetch.
void check_required_keys(const EnvMap& env) {
    for (const char* k :
         {"HOMEBREW_PREFIX", "HOMEBREW_CELLAR", "HOMEBREW_REPOSITORY", "HOMEBREW_LIBRARY",
          "HOMEBREW_CACHE", "HOMEBREW_DEFAULT_CACHE", "HOMEBREW_LOGS", "HOMEBREW_DEFAULT_LOGS",
          "HOMEBREW_TEMP", "HOMEBREW_DEFAULT_TEMP", "HOMEBREW_BREW_FILE",
          "HOMEBREW_ORIGINAL_BREW_FILE", "HOMEBREW_BREWED_CURL_PATH", "HOMEBREW_PATH",
          "HOMEBREW_USER_CONFIG_HOME", "HOMEBREW_GENERIC_DEFAULT_PREFIX",
          "HOMEBREW_GENERIC_DEFAULT_REPOSITORY"}) {
        INFO(k);
        CHECK(env.count(k) == 1);
    }
}

bool has_entry(const std::string& path, const std::string& entry) {
    std::istringstream in(path);
    for (std::string e; std::getline(in, e, ':');)
        if (e == entry)
            return true;
    return false;
}

TEST_SUITE("build::bundled_ruby_env") {

    TEST_CASE("snapshot parsing skips comments, blanks and continuation lines") {
        auto s = snapshot();
        CHECK(s.at("HOMEBREW_PREFIX") == "/opt/homebrew");
        CHECK(s.at("HOMEBREW_HELP_MESSAGE") == "Example usage:");
        CHECK(s.at("HOMEBREW_VERSION") == "5.1.1");
        for (const auto& [k, v] : s)
            CHECK(k.rfind("HOMEBREW_", 0) == 0);
    }

    TEST_CASE("default layout: env uses den's prefix and Cellar, no /opt/homebrew") {
        auto den_home = fs::path("/tmp/x/.den");
        auto c = make_config(den_home, den_home / "prefix", "/opt/homebrew");
        auto ruby = den_home / "ruby";
        auto env = bundled_ruby_env(c, ruby, snapshot(), "/opt/homebrew/bin:/usr/bin:/bin");

        check_no_opt_homebrew(env);
        check_required_keys(env);
        CHECK(env.at("HOMEBREW_PREFIX") == (den_home / "prefix").string());
        CHECK(env.at("HOMEBREW_CELLAR") == (den_home / "prefix" / "Cellar").string());
        CHECK(env.at("HOMEBREW_REPOSITORY") == (den_home / "prefix").string());
        CHECK(env.at("HOMEBREW_LIBRARY") == (ruby / "homebrew").string());
        CHECK(env.at("HOMEBREW_CACHE") == (den_home / "cache" / "homebrew").string());
        CHECK(env.at("HOMEBREW_BREW_FILE") == "/dev/null");
        CHECK(env.at("HOMEBREW_PATH") == "/usr/bin:/bin");
        CHECK(env.at("PATH") == "/usr/bin:/bin");
        // The bootsnap gem lives inside a live Homebrew: never load it.
        CHECK(env.count("HOMEBREW_BOOTSNAP_GEM_PATH") == 0);
        CHECK(env.at("HOMEBREW_NO_BOOTSNAP") == "1");
        CHECK(env.count("HOMEBREW_PYENV_ROOT") == 0);
        // The snapshot's home directory never leaks either.
        for (const auto& [k, v] : env)
            CHECK(v.find("/Users/someone") == std::string::npos);
        // Non-path constants survive.
        CHECK(env.at("HOMEBREW_VERSION") == "5.1.1");
        CHECK(env.at("HOMEBREW_API_DEFAULT_DOMAIN") == "https://formulae.brew.sh/api");
        CHECK(env.at("HOMEBREW_USER_AGENT").find("Homebrew/5.1.1") == 0);
        CHECK(env.at("HOMEBREW_HELP_MESSAGE") == "Example usage:");
        CHECK(env.at("DEN_RUBY_ENV") == "1");
    }

    TEST_CASE("migrate layout in a scratch prefix: env is the scratch Homebrew prefix") {
        auto hb = fs::path("/tmp/scratch/hbprefix");
        auto c = make_config("/tmp/scratch/den", hb, hb);
        auto env = bundled_ruby_env(c, "/tmp/scratch/den/ruby", snapshot(),
                                    "/opt/homebrew/bin:" + (hb / "bin").string() + ":/usr/bin");
        check_no_opt_homebrew(env);
        check_required_keys(env);
        CHECK(env.at("HOMEBREW_PREFIX") == hb.string());
        CHECK(env.at("HOMEBREW_CELLAR") == (hb / "Cellar").string());
        CHECK(env.at("HOMEBREW_CASKROOM") == (hb / "Caskroom").string());
        CHECK(has_entry(env.at("PATH"), (hb / "bin").string()));
        CHECK_FALSE(has_entry(env.at("PATH"), "/opt/homebrew/bin"));
    }

    TEST_CASE("migrate layout on a real /opt/homebrew keeps its PATH entries") {
        auto c = make_config("/tmp/u/.den", "/opt/homebrew", "/opt/homebrew");
        auto env =
            bundled_ruby_env(c, "/tmp/u/.den/ruby", snapshot(), "/opt/homebrew/bin:/usr/bin");
        CHECK(env.at("HOMEBREW_PREFIX") == "/opt/homebrew");
        CHECK(env.at("HOMEBREW_CELLAR") == "/opt/homebrew/Cellar");
        CHECK(has_entry(env.at("PATH"), "/opt/homebrew/bin"));
        // Still den's bundled library and caches, not brew's.
        CHECK(env.at("HOMEBREW_LIBRARY") == "/tmp/u/.den/ruby/homebrew");
        CHECK(env.count("HOMEBREW_BOOTSNAP_GEM_PATH") == 0);
    }

    TEST_CASE("build PATH drops a foreign HOMEBREW_PREFIX but keeps den's prefix") {
        auto c = make_config("/tmp/d", "/tmp/d/prefix", "/tmp/elsewhere/brew");
        auto p = sanitize_build_path(
            c, "/tmp/elsewhere/brew/bin:/home/linuxbrew/.linuxbrew/bin:/tmp/d/prefix/bin:"
               "/opt/homebrew/sbin:/usr/bin::/opt/homebrewx/bin");
        CHECK(p == "/tmp/d/prefix/bin:/usr/bin:/opt/homebrewx/bin");
    }

    TEST_CASE("brew ruby is used only when brew belongs to den's prefix") {
        TempDir t;
        auto prefix = t.path / "brewprefix";
        auto real = prefix / "Library" / "Homebrew" / "bin" / "brew";
        write(real, "#!/bin/sh\nexit 0\n");
        fs::permissions(real, static_cast<fs::perms>(0755));
        fs::create_directories(prefix / "bin");
        fs::create_symlink(real, prefix / "bin" / "brew");

        auto found = find_brew_on_path("/nonexistent:" + (prefix / "bin").string() + ":/usr/bin");
        CHECK(found == prefix / "bin" / "brew");
        CHECK(brew_serves_prefix(found, prefix));
        CHECK_FALSE(brew_serves_prefix(found, t.path / "den" / "prefix"));
        CHECK_FALSE(brew_serves_prefix({}, prefix));
        CHECK(find_brew_on_path("/nonexistent").empty());
    }

#ifdef __APPLE__
    TEST_CASE("the real bundle snapshot yields no /opt/homebrew for a scratch prefix") {
        TempDir t;
        auto den_home = t.path / "den";
        auto ruby = ensure_ruby_bundle(den_home);
        auto real = read_env_snapshot(ruby / "homebrew_env.txt");
        REQUIRE_FALSE(real.empty());
        CHECK(real.at("HOMEBREW_PREFIX") == "/opt/homebrew"); // the bug's source

        for (bool migrate : {false, true}) {
            INFO("migrate=" << migrate);
            auto prefix = migrate ? t.path / "hb" : den_home / "prefix";
            auto c = make_config(den_home, prefix, t.path / "hb");
            auto env = bundled_ruby_env(c, ruby, real, "/opt/homebrew/bin:/usr/bin:/bin");
            check_no_opt_homebrew(env);
            check_required_keys(env);
            CHECK(env.at("HOMEBREW_CELLAR") == (prefix / "Cellar").string());

            auto cmd = bundled_ruby_command(c, ruby, den_home / "cache/formulas/x.rb", "x", "1.0");
            // PATH comes from the test process and may name /opt/homebrew;
            // the generated command never does.
            CHECK(cmd.find("/opt/homebrew") == std::string::npos);
            CHECK(cmd.find("'1.0' 'x'") != std::string::npos);
        }
    }

    TEST_CASE("ensure_ruby_bundle replaces a stale den_build.rb with the compiled-in one") {
        TempDir t;
        auto ruby = ensure_ruby_bundle(t.path / "den");
        CHECK(slurp(ruby / "den_build.rb") == std::string(embedded_den_build_rb()));
        CHECK(std::string(embedded_den_build_rb()).find("DEN_RUBY_ENV") != std::string::npos);
        write(ruby / "den_build.rb", "# stale\n");
        ensure_ruby_bundle(t.path / "den"); // fast path (already unpacked)
        CHECK(slurp(ruby / "den_build.rb") == std::string(embedded_den_build_rb()));
        CHECK_FALSE(refresh_den_build_script(ruby));
    }

    // ── -s builds through the bundled Ruby, offline (file:// sources) ───────

    // A source tarball whose Makefile installs a script into
    // $(DESTDIR)$(PREFIX)/bin (source builds stage through DESTDIR; see
    // docs/source-builds.md).
    fs::path make_source(const fs::path& dir, const std::string& name, const std::string& version) {
        auto src = dir / "src" / (name + "-" + version);
        write(src / name, "#!/bin/sh\necho " + name + " " + version + "\n");
        const std::string bin = "$(DESTDIR)$(PREFIX)/bin";
        write(src / "Makefile", "install:\n\tmkdir -p " + bin + "\n\tcp " + name + " " + bin + "/" +
                                    name + "\n\tchmod 755 " + bin + "/" + name + "\n");
        auto tarball = dir / (name + "-" + version + ".tar.gz");
        run("tar -czf " + tarball.string() + " -C " + (dir / "src").string() + " " + name + "-" +
            version);
        return tarball;
    }

    // Uses Formula DSL the text parser refuses (string interpolation into a
    // system call with a computed argument), so success proves the bundled
    // Ruby did the build rather than the parser fallback.
    std::string formula(const std::string& klass, const std::string& version,
                        const fs::path& tarball) {
        return "class " + klass + " < Formula\n  desc \"fixture\"\n  url \"file://" +
               tarball.string() + "\"\n  version \"" + version + "\"\n  sha256 \"" +
               hash_file(tarball) +
               "\"\n\n  def install\n    args = [\"PREFIX=#{prefix}\"].map(&:to_s)\n"
               "    system \"make\", \"install\", *args\n"
               "    (share/\"fixture\").mkpath\n"
               "    (share/\"fixture/env\").write ENV.fetch(\"HOMEBREW_PREFIX\") + \"\\n\" "
               "+ ENV.fetch(\"HOMEBREW_CELLAR\") + \"\\n\" + ENV.fetch(\"HOMEBREW_LIBRARY\")\n"
               "  end\nend\n";
    }

    struct Scratch {
        TempDir tmp;
        fs::path home = tmp.path / "home";
        fs::path brew = tmp.path / "hb"; // scratch HOMEBREW_PREFIX
        bool migrate;
        explicit Scratch(bool m) : migrate(m) {}
        fs::path prefix() const { return migrate ? brew : home / "prefix"; }
        std::string den(const std::string& args) const {
            return run("env -u DEN_PREFIX -u HOMEBREW_REPOSITORY -u HOMEBREW_CASKROOM"
                       " PATH=/usr/bin:/bin:/usr/sbin:/sbin HOME=" +
                       tmp.path.string() + " DEN_HOME=" + home.string() +
                       " DEN_PREFIX=" + prefix().string() + " HOMEBREW_PREFIX=" + brew.string() +
                       " HOMEBREW_CELLAR=" + (brew / "Cellar").string() + " ./den " + args);
        }
    };

    TEST_CASE("e2e: -s builds a local tap formula through the bundled Ruby") {
        for (bool migrate : {false, true}) {
            INFO("migrate=" << migrate);
            Scratch s(migrate);
            auto tarball = make_source(s.tmp.path, "rbtool", "1.2");
            write(s.tmp.path / "tap/Formula/rbtool.rb", formula("Rbtool", "1.2", tarball));
            auto tap = s.den("tap add test/rbtap --url " + (s.tmp.path / "tap").string());
            INFO(tap);
            auto out = s.den("install -s test/rbtap/rbtool");
            INFO(out);
            CHECK(out.find("bundled ruby build failed") == std::string::npos);
            CHECK(out.find("==> Built rbtool 1.2") != std::string::npos);
            auto keg = s.prefix() / "Cellar/rbtool/1.2";
            CHECK(run((keg / "bin/rbtool").string()) == "rbtool 1.2\n");
            // The formula saw den's prefix, Cellar and bundled library.
            CHECK(slurp(keg / "share/fixture/env") == s.prefix().string() + "\n" +
                                                          (s.prefix() / "Cellar").string() + "\n" +
                                                          (s.home / "ruby" / "homebrew").string());
            CHECK(fs::is_regular_file(keg / "DEN_RECEIPT.json"));
        }
    }

    // Same fixture, but with the formula's sha256 replaced.
    std::string formula_with_sha(const std::string& klass, const std::string& version,
                                 const fs::path& tarball, const std::string& sha) {
        auto text = formula(klass, version, tarball);
        auto real = hash_file(tarball);
        auto at = text.find(real);
        REQUIRE(at != std::string::npos);
        return text.replace(at, real.size(), sha);
    }

    TEST_CASE("e2e: -s refuses a source tarball whose sha256 does not match") {
        Scratch s(false);
        auto tarball = make_source(s.tmp.path, "rbtool", "1.2");
        const std::string wrong(64, 'a');
        write(s.tmp.path / "tap/Formula/rbtool.rb",
              formula_with_sha("Rbtool", "1.2", tarball, wrong));
        auto tap = s.den("tap add test/rbtap --url " + (s.tmp.path / "tap").string());
        INFO(tap);
        auto out = s.den("install -s test/rbtap/rbtool");
        INFO(out);
        // The bundled Ruby refused it (its message names both digests)...
        CHECK(out.find("SHA256 mismatch for rbtool source tarball") != std::string::npos);
        CHECK(out.find("expected: " + wrong) != std::string::npos);
        CHECK(out.find("actual:   " + hash_file(tarball)) != std::string::npos);
        CHECK(out.find("==> Built rbtool") == std::string::npos);
        CHECK(out.find("==> Building\n") == std::string::npos);
        // Reported once: the mismatch is final, so no parser fallback
        // re-downloads the tarball and fails the same check again.
        const std::string needle = "SHA256 mismatch for rbtool";
        size_t hits = 0;
        for (auto p = out.find(needle); p != std::string::npos; p = out.find(needle, p + 1))
            ++hits;
        CHECK(hits == 1);
        CHECK(out.find("bundled ruby build failed") == std::string::npos);
        // ...and nothing was built, installed or kept in the source cache.
        CHECK_FALSE(fs::exists(s.prefix() / "Cellar/rbtool"));
        CHECK_FALSE(fs::exists(s.prefix() / "bin/rbtool"));
        auto sources = s.home / "cache/sources";
        CHECK_FALSE(fs::exists(sources / "rbtool-1.2.tar.gz"));
        CHECK_FALSE(fs::exists(sources / "rbtool-1.2.tar.gz.download"));
    }

    TEST_CASE("e2e: -s replaces a cached source tarball that no longer matches") {
        Scratch s(false);
        auto tarball = make_source(s.tmp.path, "rbtool", "1.2");
        write(s.tmp.path / "tap/Formula/rbtool.rb", formula("Rbtool", "1.2", tarball));
        auto cached = s.home / "cache/sources/rbtool-1.2.tar.gz";
        write(cached, "not the real tarball\n");
        auto tap = s.den("tap add test/rbtap --url " + (s.tmp.path / "tap").string());
        INFO(tap);
        auto out = s.den("install -s test/rbtap/rbtool");
        INFO(out);
        CHECK(out.find("bundled ruby build failed") == std::string::npos);
        CHECK(out.find("==> Built rbtool 1.2") != std::string::npos);
        CHECK(run((s.prefix() / "Cellar/rbtool/1.2/bin/rbtool").string()) == "rbtool 1.2\n");
        REQUIRE(fs::exists(cached));
        CHECK(hash_file(cached) == hash_file(tarball));
    }

    TEST_CASE("core-shaped formula: build_from_source in a scratch prefix via bundled Ruby") {
        TempDir t;
        auto den_home = t.path / "den";
        auto c = make_config(den_home, den_home / "prefix", t.path / "hb");
        auto tarball = make_source(t.path, "rbcore", "3.4.5");
        write(t.path / "rbcore.rb", formula("Rbcore", "3.4.5", tarball));
        PackageIndex idx;
        Package pkg;
        pkg.name = "rbcore";
        pkg.version = "3.4.5";
        pkg.ruby_source_path = (t.path / "rbcore.rb").string();
        idx.packages["rbcore"] = pkg;

        auto keg = build_from_source(c, idx, "rbcore", "3.4.5");
        CHECK(keg == den_home / "prefix/Cellar/rbcore/3.4.5");
        CHECK(slurp(keg / "bin/rbcore") == "#!/bin/sh\necho rbcore 3.4.5\n");
        CHECK(slurp(keg / "share/fixture/env") == (den_home / "prefix").string() + "\n" +
                                                      (den_home / "prefix/Cellar").string() + "\n" +
                                                      (den_home / "ruby/homebrew").string());
        CHECK_FALSE(fs::exists(t.path / "hb"));
    }
#endif
}

} // namespace bundled_ruby_env_test
} // namespace den
