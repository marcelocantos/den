// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Pour-level regression: a Portable (:any_skip_relocation) bottle must still
// have its @@HOMEBREW_*@@ text placeholders expanded, including
// @@HOMEBREW_JAVA@@ / @@HOMEBREW_PERL@@ resolved from the package's deps.
// Previously HomebrewProvider skipped relocation for Portable bottles, so
// automake, gradle, apktool, yarn, ... were poured with literal placeholders.

#include "build/relocate.h"
#include "core/config.h"
#include "download/sha256.h"
#include "index/package.h"
#include "provider/exec.h"
#include "provider/homebrew_provider.h"

#include <doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace den;

namespace {

std::string slurp(const fs::path& f) {
    std::ifstream in(f);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

Package portable_formula(const std::string& name, const std::string& version,
                         const std::string& url, const std::string& sha,
                         std::vector<std::string> deps) {
    Package p;
    p.name = name;
    p.version = version;
    p.dependencies = std::move(deps);
    Package::Archive ar;
    ar.url = url;
    ar.sha256 = sha;
    ar.relocation = RelocationType::Portable;
    p.archives["default"] = std::move(ar);
    return p;
}

} // namespace

TEST_SUITE("relocate::portable_pour") {

    TEST_CASE("Portable bottle pour expands PREFIX/CELLAR/JAVA/PERL text placeholders") {
        auto tmp = fs::temp_directory_path() / ("den-portable-pour-" + std::to_string(::getpid()));
        fs::remove_all(tmp);
        auto prefix = tmp / "prefix";
        auto store = prefix / "Cellar";
        auto stage = tmp / "stage";
        fs::create_directories(stage / "probe" / "1.0" / "bin");
        fs::create_directories(stage / "probe" / "1.0" / "share" / "probe");
        {
            std::ofstream out(stage / "probe" / "1.0" / "bin" / "probe");
            out << "#!/bin/bash\n"
                << "export JAVA_HOME=\"${JAVA_HOME:-@@HOMEBREW_JAVA@@}\"\n"
                << "exec \"@@HOMEBREW_CELLAR@@/probe/1.0/share/probe/run\" \"$@\"\n";
        }
        {
            std::ofstream out(stage / "probe" / "1.0" / "share" / "probe" / "Config.pm");
            out << "#!@@HOMEBREW_PERL@@\nour $prefix = '@@HOMEBREW_PREFIX@@';\n";
        }
        auto tarball = tmp / "probe-1.0.portable.bottle.tar.gz";
        auto tar = run_tool({"tar", "-czf", tarball.string(), "-C", stage.string(), "probe"});
        REQUIRE(tar.exit_code == 0);
        const auto sha = hash_file(tarball);

        // The openjdk@21 dependency is already poured, so only probe is fetched.
        fs::create_directories(store / "openjdk@21" / "21.0.1" / "bin");
        { std::ofstream(store / "openjdk@21" / "21.0.1" / "bin" / "java") << "x"; }

        PackageIndex idx;
        idx.packages["probe"] = portable_formula(
            "probe", "1.0", "https://bottles.invalid/probe-1.0.tar.gz", sha, {"openjdk@21"});
        idx.packages["openjdk@21"] = portable_formula(
            "openjdk@21", "21.0.1", "file:///nonexistent.tar.gz", std::string(64, 'e'), {});

        Config cfg;
        cfg.den_home = tmp / "den";
        cfg.cache = cfg.den_home / "cache";
        cfg.store = store;
        cfg.homebrew_cellar = store;
        // Seed the content-addressed archive cache so the pour never touches
        // the network (fetch_archive returns a verified cache hit).
        fs::create_directories(cfg.cache / "archives");
        fs::copy_file(tarball, cfg.cache / "archives" / (sha + ".tar.gz"));

        HomebrewProvider provider(&idx);
        provider.install(cfg, "probe", "");

        auto keg = store / "probe" / "1.0";
        auto script = slurp(keg / "bin" / "probe");
        auto pm = slurp(keg / "share" / "probe" / "Config.pm");
        CHECK(script.find("@@HOMEBREW_") == std::string::npos);
        CHECK(pm.find("@@HOMEBREW_") == std::string::npos);
        CHECK(script.find(homebrew_java_path(prefix.string(), {"openjdk@21"})) !=
              std::string::npos);
        CHECK(script.find("/opt/openjdk@21/") != std::string::npos);
        CHECK(script.find((keg / "share" / "probe" / "run").string()) != std::string::npos);
        CHECK(pm.rfind("#!" + homebrew_perl_path(prefix.string(), "probe", {"openjdk@21"}) + "\n",
                       0) == 0);
        CHECK(pm.find("'" + prefix.string() + "'") != std::string::npos);

        fs::remove_all(tmp);
    }
}
