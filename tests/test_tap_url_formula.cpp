// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Native install of tap formulae that ship a prebuilt download instead of a
// bottle (src/tap/url_formula.h). Everything here is offline: archives are
// built locally and served from file:// URLs.

#include "../src/core/error.h"
#include "../src/download/sha256.h"
#include "../src/tap/tap.h"
#include "../src/tap/url_formula.h"

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace den {
namespace tap_url_formula_test {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_tapurl_XXXXXX").string();
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

const TapPlatform kMacArm{true, true};
const TapPlatform kMacIntel{true, false};
const TapPlatform kLinuxArm{false, true};
const TapPlatform kLinuxIntel{false, false};

std::vector<InstallStep> steps_of(const std::string& body, const std::string& name = "tool") {
    std::vector<std::string> lines;
    std::istringstream in(body);
    for (std::string l; std::getline(in, l);)
        lines.push_back(l);
    std::string why;
    auto steps = parse_install_steps(lines, name, why);
    INFO(why);
    REQUIRE(steps.has_value());
    return *steps;
}

std::string refusal_of(const std::string& line) {
    std::string why;
    auto steps = parse_install_steps({line}, "tool", why);
    CHECK_FALSE(steps.has_value());
    return why;
}

// A staged download plus the keg it installs into.
struct Stage {
    TempDir tmp;
    fs::path stage = tmp.path / "stage";
    fs::path keg = tmp.path / "Cellar" / "tool" / "1.0";
    InstallPaths paths{keg, {}, tmp.path, "tool", "1.0"};
    Stage() {
        fs::create_directories(stage);
        fs::create_directories(keg);
    }
    void apply(const std::string& body) { apply_install_steps(steps_of(body), stage, paths); }
};

// GoReleaser/Homebrew Releaser shape used by the marcelocantos tap.
const char* kReleaserFormula = R"RB(# typed: false
class Tool < Formula
  desc "A tool"
  homepage "https://example.com"
  url "https://example.com/archive/refs/tags/v1.2.3.tar.gz"
  sha256 "source0000000000000000000000000000000000000000000000000000000000"
  version "1.2.3"
  license "Apache-2.0"

  depends_on "go" => :build
  depends_on "gh"
  depends_on "marcelocantos/tap/blurter"

  on_macos do
    on_arm do
      url "https://example.com/tool-darwin-arm64.tar.gz"
      sha256 "macarm"
    end
    on_intel do
      url "https://example.com/tool-darwin-amd64.tar.gz"
      sha256 "macintel"
    end
    depends_on "terminal-notifier"
  end

  on_linux do
    on_arm do
      url "https://example.com/tool-linux-arm64.tar.gz"
      sha256 "linuxarm"
    end
    on_intel do
      url "https://example.com/tool-linux-amd64.tar.gz"
      sha256 "linuxintel"
    end
  end

  service do
    run [opt_bin/"tool", "serve"]
  end

  def caveats
    <<~EOS
      if you see this
      end
    EOS
  end

  def install
    bin.install "tool"
  end

  test do
    system "#{bin}/tool", "--version"
  end
end
)RB";

// process-compose shape: Hardware::CPU conditionals with a `def install` in
// each branch and no top-level url.
const char* kConditionalFormula = R"RB(class ProcessCompose < Formula
  version "1.40.0"
  on_macos do
    if Hardware::CPU.intel?
      url "https://example.com/pc_darwin_amd64.tar.gz"
      sha256 "macintel"
      def install
        bin.install "process-compose-intel" => "process-compose"
      end
    end
    if Hardware::CPU.arm?
      url "https://example.com/pc_darwin_arm64.tar.gz"
      sha256 "macarm"
      def install
        bin.install "process-compose"
      end
    end
  end
  on_linux do
    if Hardware::CPU.intel? and Hardware::CPU.is_64_bit?
      url "https://example.com/pc_linux_amd64.tar.gz"
    end
    if Hardware::CPU.arm?
      if Hardware::CPU.is_64_bit?
        url "https://example.com/pc_linux_arm64.tar.gz"
        sha256 "linuxarm"
        def install
          bin.install "process-compose"
        end
      else
        url "https://example.com/pc_linux_arm.tar.gz"
        sha256 "linuxarm32"
      end
    end
  end
end
)RB";

TEST_SUITE("tap::url_formula") {

    TEST_CASE("picks the url and sha256 for each platform from on_* blocks") {
        struct Want {
            TapPlatform p;
            const char* url;
            const char* sha;
        };
        for (const auto& w : {Want{kMacArm, "tool-darwin-arm64", "macarm"},
                              Want{kMacIntel, "tool-darwin-amd64", "macintel"},
                              Want{kLinuxArm, "tool-linux-arm64", "linuxarm"},
                              Want{kLinuxIntel, "tool-linux-amd64", "linuxintel"}}) {
            auto f = parse_tap_formula(kReleaserFormula, w.p);
            REQUIRE(f.url_install.has_value());
            CHECK(f.url_install->url == std::string("https://example.com/") + w.url + ".tar.gz");
            CHECK(f.url_install->sha256 == w.sha);
            CHECK(f.url_install->refusal.empty());
            CHECK(f.version == "1.2.3");
            // service/caveats/test blocks don't leak into the install recipe.
            REQUIRE(f.url_install->install_lines.size() == 1);
            CHECK(f.url_install->install_lines[0] == "bin.install \"tool\"");
        }
    }

    TEST_CASE("runtime depends_on for this platform only, build deps skipped") {
        auto mac = parse_tap_formula(kReleaserFormula, kMacArm);
        CHECK(mac.dependencies ==
              std::vector<std::string>{"gh", "marcelocantos/tap/blurter", "terminal-notifier"});
        auto linux_ = parse_tap_formula(kReleaserFormula, kLinuxIntel);
        CHECK(linux_.dependencies == std::vector<std::string>{"gh", "marcelocantos/tap/blurter"});
    }

    TEST_CASE("Hardware::CPU conditionals select url, sha256 and def install") {
        auto arm = parse_tap_formula(kConditionalFormula, kMacArm);
        REQUIRE(arm.url_install.has_value());
        CHECK(arm.url_install->url == "https://example.com/pc_darwin_arm64.tar.gz");
        CHECK(arm.url_install->sha256 == "macarm");
        CHECK(arm.url_install->install_lines ==
              std::vector<std::string>{"bin.install \"process-compose\""});

        auto intel = parse_tap_formula(kConditionalFormula, kMacIntel);
        REQUIRE(intel.url_install.has_value());
        CHECK(intel.url_install->sha256 == "macintel");
        CHECK(intel.url_install->install_lines ==
              std::vector<std::string>{
                  "bin.install \"process-compose-intel\" => \"process-compose\""});

        auto larm = parse_tap_formula(kConditionalFormula, kLinuxArm);
        REQUIRE(larm.url_install.has_value());
        CHECK(larm.url_install->url == "https://example.com/pc_linux_arm64.tar.gz");
        CHECK(larm.url_install->sha256 == "linuxarm");
    }

    TEST_CASE("a platform url without sha256 is refused") {
        // linux/intel's branch condition is a compound den doesn't evaluate,
        // so no url applies there; a url with no sha256 must be refused.
        const char* src = R"RB(class T < Formula
  on_macos do
    url "https://example.com/t.tar.gz"
  end
  def install
    bin.install "t"
  end
end
)RB";
        auto f = parse_tap_formula(src, kMacArm);
        REQUIRE(f.url_install.has_value());
        CHECK(f.url_install->refusal.find("no sha256") != std::string::npos);
    }

    // ── one test per whitelisted install step ──────────────────────────────

    TEST_CASE("step: bin.install") {
        Stage s;
        write(s.stage / "tool", "#!/bin/sh\necho tool\n");
        s.apply("bin.install \"tool\"");
        CHECK(slurp(s.keg / "bin/tool") == "#!/bin/sh\necho tool\n");
    }

    TEST_CASE("step: bin.install with rename") {
        Stage s;
        write(s.stage / "tool-darwin-arm64", "x");
        write(s.stage / "helper", "y");
        s.apply("bin.install \"tool-darwin-arm64\" => \"tool\", \"helper\"");
        CHECK(slurp(s.keg / "bin/tool") == "x");
        CHECK(slurp(s.keg / "bin/helper") == "y");
        CHECK_FALSE(fs::exists(s.keg / "bin/tool-darwin-arm64"));
    }

    TEST_CASE("step: libexec.install Dir[\"*\"]") {
        Stage s;
        write(s.stage / "ytt", "main");
        write(s.stage / "lib/mod.py", "mod");
        s.apply("libexec.install Dir[\"*\"]");
        CHECK(slurp(s.keg / "libexec/ytt") == "main");
        CHECK(slurp(s.keg / "libexec/lib/mod.py") == "mod");
    }

    TEST_CASE("step: install_symlink") {
        Stage s;
        write(s.stage / "ytt", "main");
        s.apply("libexec.install Dir[\"*\"]\nbin.install_symlink libexec/\"ytt\"");
        REQUIRE(fs::is_symlink(s.keg / "bin/ytt"));
        CHECK(fs::read_symlink(s.keg / "bin/ytt") == fs::path("../libexec/ytt"));
        CHECK(slurp(s.keg / "bin/ytt") == "main");

        Stage r;
        write(r.stage / "ytt", "main");
        r.apply("libexec.install \"ytt\"\nbin.install_symlink libexec/\"ytt\" => \"ytt2\"");
        CHECK(slurp(r.keg / "bin/ytt2") == "main");
    }

    TEST_CASE("step: (share/\"x\").install") {
        Stage s;
        write(s.stage / "sidecar/run.py", "py");
        s.apply("(share/\"claudia\").install \"sidecar\"");
        CHECK(slurp(s.keg / "share/claudia/sidecar/run.py") == "py");
    }

    TEST_CASE("step: shell completions") {
        Stage s;
        write(s.stage / "completions/cv.bash", "b");
        write(s.stage / "completions/cv.zsh", "z");
        write(s.stage / "completions/cv.fish", "f");
        s.apply("bash_completion.install \"completions/cv.bash\" => \"cv\"\n"
                "zsh_completion.install \"completions/cv.zsh\" => \"_cv\"\n"
                "fish_completion.install \"completions/cv.fish\"");
        CHECK(slurp(s.keg / "etc/bash_completion.d/cv") == "b");
        CHECK(slurp(s.keg / "share/zsh/site-functions/_cv") == "z");
        CHECK(slurp(s.keg / "share/fish/vendor_completions.d/cv.fish") == "f");
    }

    TEST_CASE("step: man1.install") {
        Stage s;
        write(s.stage / "doc/tool.1", "man");
        s.apply("man1.install \"doc/tool.1\"");
        CHECK(slurp(s.keg / "share/man/man1/tool.1") == "man");
    }

    TEST_CASE("step: install guarded by if File.exist?") {
        Stage s;
        write(s.stage / "libexec/present/a", "a");
        s.apply("(libexec/\"x\").install \"libexec/present\" if File.exist?(\"libexec/present\")\n"
                "(libexec/\"x\").install \"libexec/absent\" if File.exist?(\"libexec/absent\")");
        CHECK(slurp(s.keg / "libexec/x/present/a") == "a");
        CHECK_FALSE(fs::exists(s.keg / "libexec/x/absent"));
    }

    TEST_CASE("step: wrapper script via write + chmod (vellum)") {
        Stage s;
        write(s.stage / "vellum", "bin");
        // As the statement scanner hands it over: the heredoc body follows
        // the opening line, already dedented.
        const std::string body = "bin.install \"vellum\" => \"vellum-bin\"\n";
        auto steps = steps_of(body);
        std::string why;
        auto more = parse_install_steps({"(bin/\"vellum\").write <<~SH\n#!/bin/sh\n"
                                         "export PATH=\"#{HOMEBREW_PREFIX}/bin:$PATH\"\n"
                                         "exec \"#{opt_bin}/vellum-bin\" \"$@\"\n",
                                         "(bin/\"vellum\").chmod 0755"},
                                        "vellum", why);
        INFO(why);
        REQUIRE(more.has_value());
        steps.insert(steps.end(), more->begin(), more->end());
        InstallPaths paths{s.keg, {}, s.tmp.path, "vellum", "1.0"};
        apply_install_steps(steps, s.stage, paths);
        const auto prefix = s.tmp.path.string();
        CHECK(slurp(s.keg / "bin/vellum") == "#!/bin/sh\nexport PATH=\"" + prefix +
                                                 "/bin:$PATH\"\nexec \"" + prefix +
                                                 "/opt/vellum/bin/vellum-bin\" \"$@\"\n");
        auto perms = fs::status(s.keg / "bin/vellum").permissions();
        CHECK((perms & fs::perms::all) == static_cast<fs::perms>(0755));
        CHECK(slurp(s.keg / "bin/vellum-bin") == "bin");
    }

    // ── clean refusals ────────────────────────────────────────────────────

    TEST_CASE("refuses anything outside the whitelist, naming the step") {
        CHECK(refusal_of("system \"make\", \"install\"") ==
              "unsupported install step: `system \"make\", \"install\"`");
        CHECK(refusal_of("mkdir \"build\"").find("mkdir") != std::string::npos);
        CHECK(refusal_of("bin.install Dir[\"bin/*\"], \"extra\"").starts_with("unsupported"));
        CHECK(refusal_of("bin.install \"../escape\"").starts_with("unsupported"));
        CHECK(refusal_of("bin.install \"#{buildpath}/x\"").starts_with("unsupported"));
        CHECK(
            refusal_of("(etc/\"x\").install_symlink \"/usr/bin/true\"").starts_with("unsupported"));
        CHECK(refusal_of("inreplace \"x\", \"a\", \"b\"").starts_with("unsupported"));
    }

    TEST_CASE("write refuses interpolations it cannot resolve") {
        Stage s;
        std::string why;
        auto steps =
            parse_install_steps({"(bin/\"t\").write <<~SH\n#{Formula[\"x\"].bin}\n"}, "tool", why);
        REQUIRE(steps.has_value());
        CHECK_THROWS_AS(apply_install_steps(*steps, s.stage, s.paths), UserError);
    }

    TEST_CASE("missing source file is a clear error") {
        Stage s;
        try {
            s.apply("bin.install \"nope\"");
            FAIL("expected UserError");
        } catch (const UserError& e) {
            CHECK(std::string(e.what()).find("'nope'") != std::string::npos);
        }
    }

    TEST_CASE("user/repo/name resolves to the bare name, and wins over a shadowed core formula") {
        PackageIndex idx;
        Package core;
        core.name = "tool";
        core.version = "9.9";
        core.ruby_source_path = "Formula/t/tool.rb";
        Package tap;
        tap.name = "test/urltap/tool";
        tap.version = "1.0";
        tap.ruby_source_path = "/taps/test/homebrew-urltap/Formula/tool.rb";
        idx.packages["tool"] = core;
        idx.packages["test/urltap/tool"] = tap;

        CHECK(resolve_tap_install_name(idx, "ripgrep") == "ripgrep");
        CHECK(resolve_tap_install_name(idx, "test/urltap/tool") == "tool");
        REQUIRE(idx.find("tool") != nullptr);
        CHECK(idx.find("tool")->version == "1.0");
        CHECK(idx.find("tool")->name == "tool");
        CHECK(idx.find("tool")->ruby_source_path == tap.ruby_source_path);
    }

    // ── end to end through the CLI (offline, file:// download) ──────────────

    struct Env {
        TempDir tmp;
        fs::path home = tmp.path / "home";
        fs::path prefix = home / "prefix"; // den's own prefix and Cellar
        fs::path brew = home / "brew";     // a Homebrew den must not touch
        std::string den(const std::string& args) const {
            return run("DEN_HOME=" + home.string() + " DEN_PREFIX=" + prefix.string() +
                       " HOMEBREW_PREFIX=" + brew.string() +
                       " HOMEBREW_CELLAR=" + (brew / "Cellar").string() + " ./den " + args);
        }
        // A tap with one formula whose download is a local tarball.
        void make_tap(const std::string& install_body, const std::string& sha_override = "") {
            auto src = tmp.path / "src" / "tool-1.0";
            write(src / "tool", "#!/bin/sh\necho tool 1.0\n");
            fs::permissions(src / "tool", static_cast<fs::perms>(0755));
            auto tarball = tmp.path / "tool-1.0.tar.gz";
            run("tar -czf " + tarball.string() + " -C " + (tmp.path / "src").string() +
                " tool-1.0");
            auto sha = sha_override.empty() ? hash_file(tarball) : sha_override;
            const auto url = "file://" + tarball.string();
            write(tmp.path / "tap/Formula/tool.rb",
                  "class Tool < Formula\n  version \"1.0\"\n  on_macos do\n    url \"" + url +
                      "\"\n    sha256 \"" + sha + "\"\n  end\n  on_linux do\n    url \"" + url +
                      "\"\n    sha256 \"" + sha + "\"\n  end\n  def install\n    " + install_body +
                      "\n  end\nend\n");
            den("tap add test/urltap --url " + (tmp.path / "tap").string());
        }
    };

    TEST_CASE("e2e: user/repo/name installs into the bare keg with a receipt and opt link") {
        Env env;
        env.make_tap("bin.install \"tool\"");
        auto out = env.den("install test/urltap/tool");
        INFO(out);
        auto keg = env.prefix / "Cellar/tool/1.0";
        CHECK(slurp(keg / "bin/tool") == "#!/bin/sh\necho tool 1.0\n");
        CHECK(fs::is_regular_file(keg / "DEN_RECEIPT.json"));
        CHECK(fs::is_symlink(env.prefix / "opt/tool"));
        CHECK_FALSE(fs::exists(env.prefix / "Cellar/test"));
        CHECK_FALSE(fs::exists(env.brew));
    }

    TEST_CASE("e2e: sha256 mismatch is refused and leaves no keg") {
        Env env;
        env.make_tap("bin.install \"tool\"", std::string(64, '0'));
        auto out = env.den("install tool");
        INFO(out);
        CHECK(out.find("SHA256 mismatch") != std::string::npos);
        CHECK_FALSE(fs::exists(env.prefix / "Cellar/tool"));
    }

    TEST_CASE("e2e: unsupported install step is refused cleanly") {
        Env env;
        env.make_tap("system \"make\", \"install\"");
        auto out = env.den("install tool");
        INFO(out);
        CHECK(out.find("cannot install tool 1.0: unsupported install step: `system \"make\", "
                       "\"install\"`") != std::string::npos);
        CHECK_FALSE(fs::exists(env.prefix / "Cellar/tool"));
    }

    TEST_CASE("e2e: -s accepts user/repo/name") {
        Env env;
        env.make_tap("bin.install \"tool\"");
        auto out = env.den("install -s test/urltap/tool");
        INFO(out);
        CHECK(out.find("invalid package name") == std::string::npos);
    }
}

} // namespace tap_url_formula_test
} // namespace den
