// Tap installs record "user/repo/name" in the manifest, so reinstalling or
// upgrading from the manifest picks the tap formula even when a core formula
// has the same bare name. Kegs still live in Cellar/<name>.

#include "../src/download/sha256.h"
#include "../src/env/environment.h"
#include "../src/env/manifest.h"
#include "../src/index/index.h"
#include "../src/index/package.h"

#include <doctest.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace den {
namespace tap_qualified_test {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "den_tapq_XXXXXX").string();
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

// An isolated den (own DEN_HOME and prefix) whose index holds core formulae
// named like the tap's, with no bottles: if den ever resolves the core one,
// the install fails with "no archive available".
struct Env {
    TempDir tmp;
    fs::path home = tmp.path / "home";
    fs::path prefix = home / "prefix";
    fs::path brew = home / "brew";
    fs::path tap = tmp.path / "tap";

    std::string den(const std::string& args) const {
        return run("DEN_HOME=" + home.string() + " DEN_PREFIX=" + prefix.string() +
                   " HOMEBREW_PREFIX=" + brew.string() +
                   " HOMEBREW_CELLAR=" + (brew / "Cellar").string() + " ./den " + args);
    }

    void core_index(const std::vector<std::string>& names) const {
        PackageIndex idx;
        for (const auto& n : names) {
            Package p;
            p.name = n;
            p.version = "9.9";
            p.ruby_source_path = "Formula/" + n.substr(0, 1) + "/" + n + ".rb";
            idx.packages[n] = p;
        }
        save_index(idx, home / "cache" / "index.json");
    }

    // A url formula `name` whose tarball holds bin/<name> printing its version.
    std::string formula(const std::string& name, const std::string& version,
                        const std::vector<std::string>& deps = {}) const {
        auto src = tmp.path / ("src-" + name + "-" + version) / (name + "-" + version);
        write(src / name, "#!/bin/sh\necho " + name + " " + version + "\n");
        fs::permissions(src / name, static_cast<fs::perms>(0755));
        auto tarball = tmp.path / (name + "-" + version + ".tar.gz");
        run("tar -czf " + tarball.string() + " -C " + src.parent_path().string() + " " +
            src.filename().string());
        std::string cls = name;
        cls[0] = static_cast<char>(std::toupper(cls[0]));
        std::string rb = "class " + cls + " < Formula\n  version \"" + version + "\"\n";
        for (const auto& d : deps)
            rb += "  depends_on \"" + d + "\"\n";
        rb += "  url \"file://" + tarball.string() + "\"\n  sha256 \"" + hash_file(tarball) +
              "\"\n  def install\n    bin.install \"" + name + "\"\n  end\nend\n";
        return rb;
    }

    void add_tap(const std::vector<std::pair<std::string, std::string>>& formulae) {
        for (const auto& [name, rb] : formulae)
            write(tap / "Formula" / (name + ".rb"), rb);
        den("tap add test/urltap --url " + tap.string());
    }

    Manifest manifest() const { return read_manifest(home, "/"); }
};

TEST_SUITE("tap::qualified_manifest") {
    TEST_CASE("manifest round-trip keeps the tap formula when core shadows its name") {
        Env env;
        env.core_index({"tool"});
        env.add_tap({{"tool", env.formula("tool", "1.0")}});

        auto out = env.den("install test/urltap/tool");
        INFO(out);
        auto m = env.manifest();
        REQUIRE(m.packages["homebrew"].count("test/urltap/tool") == 1);
        CHECK(m.packages["homebrew"]["test/urltap/tool"] == "1.0");
        CHECK(m.packages["homebrew"].count("tool") == 0);
        CHECK(slurp(env.prefix / "Cellar/tool/1.0/bin/tool") == "#!/bin/sh\necho tool 1.0\n");
        CHECK(fs::is_symlink(env.prefix / "opt/tool"));
        CHECK_FALSE(fs::exists(env.prefix / "Cellar/test"));
        // Restore on a fresh store: reinstall every manifest entry by name.
        fs::remove_all(env.prefix);
        for (const auto& [name, _] : m.packages["homebrew"]) {
            auto again = env.den("install " + name);
            INFO(again);
            CHECK(again.find("no archive available") == std::string::npos);
        }
        CHECK(slurp(env.prefix / "Cellar/tool/1.0/bin/tool") == "#!/bin/sh\necho tool 1.0\n");

        // Upgrade from the manifest follows the tap, not core's 9.9.
        write(env.home / "taps/test/urltap/Formula/tool.rb", env.formula("tool", "1.1"));
        auto up = env.den("upgrade");
        INFO(up);
        CHECK(up.find("no archive available") == std::string::npos);
        CHECK(slurp(env.prefix / "Cellar/tool/1.1/bin/tool") == "#!/bin/sh\necho tool 1.1\n");
        CHECK_FALSE(fs::exists(env.prefix / "Cellar/tool/9.9"));
        CHECK(env.manifest().packages["homebrew"]["test/urltap/tool"] == "1.1");
    }

    TEST_CASE("bare-name manifest entries from before still materialise") {
        Env env;
        env.add_tap({{"tool", env.formula("tool", "1.0")}});
        auto out = env.den("install tool");
        INFO(out);
        auto m = env.manifest();
        CHECK(m.packages["homebrew"].count("tool") == 1);
        CHECK(slurp(env.prefix / "Cellar/tool/1.0/bin/tool") == "#!/bin/sh\necho tool 1.0\n");
        CHECK(fs::is_symlink(env.prefix / "opt/tool"));
    }

    TEST_CASE("a tap dependency whose name core shadows resolves to the tap formula") {
        Env env;
        env.core_index({"helper"});
        env.add_tap({{"tool", env.formula("tool", "1.0", {"test/homebrew-urltap/helper"})},
                     {"helper", env.formula("helper", "2.0")}});

        auto out = env.den("install test/urltap/tool");
        INFO(out);
        CHECK(out.find("no archive available") == std::string::npos);
        CHECK(slurp(env.prefix / "Cellar/helper/2.0/bin/helper") == "#!/bin/sh\necho helper 2.0\n");
        CHECK(slurp(env.prefix / "Cellar/tool/1.0/bin/tool") == "#!/bin/sh\necho tool 1.0\n");
        CHECK_FALSE(fs::exists(env.prefix / "Cellar/helper/9.9"));
        auto m = env.manifest();
        CHECK(m.auto_deps["homebrew"].count("test/urltap/helper") == 1);
        CHECK(m.packages["homebrew"]["test/urltap/helper"] == "2.0");
    }
}

} // namespace tap_qualified_test
} // namespace den
