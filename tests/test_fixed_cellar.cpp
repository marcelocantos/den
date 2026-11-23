// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// Fixed-cellar bottles and Mach-O header space, following Homebrew:
//  - :any / :any_skip_relocation bottles pour into any Cellar;
//  - a bottle whose `cellar` is a fixed path (/opt/homebrew/Cellar,
//    /usr/local/Cellar, /home/linuxbrew/.linuxbrew/Cellar) pours only into
//    that Cellar, so den builds it from source instead when it can
//    (build/source_plan.h) and otherwise refuses it up front, naming the
//    formula, never leaving a keg behind;
//  - running out of Mach-O header space while relocating is a hard error and
//    the partly relocated keg is rolled back.

#include "build/relocate.h"
#include "build/source_plan.h"
#include "core/config.h"
#include "core/error.h"
#include "doctor/doctor.h"
#include "download/sha256.h"
#include "index/bottle_cellar.h"
#include "index/index.h"
#include "index/package.h"
#include "provider/exec.h"
#include "provider/homebrew_provider.h"

#include <doctest.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace den;
using json = nlohmann::json;

namespace {

// Brew API formula.json shape: per-file `cellar` overrides bottle.stable.cellar.
std::string fixture_formula_json() {
    json f = json::array({json{
        {"name", "fixedprobe"},
        {"versions", {{"stable", "2.0"}}},
        {"urls", {{"stable", {{"url", "https://example.invalid/fixedprobe-2.0.tar.gz"}}}}},
        {"bottle",
         {{"stable",
           {{"cellar", ":any"},
            {"files",
             {{"arm64_sequoia",
               {{"cellar", "/opt/homebrew/Cellar"}, {"url", "u1"}, {"sha256", "s1"}}},
              {"sonoma", {{"cellar", "/usr/local/Cellar"}, {"url", "u2"}, {"sha256", "s2"}}},
              {"x86_64_linux",
               {{"cellar", "/home/linuxbrew/.linuxbrew/Cellar"}, {"url", "u3"}, {"sha256", "s3"}}},
              {"arm64_sonoma", {{"url", "u4"}, {"sha256", "s4"}}},
              {"arm64_ventura",
               {{"cellar", ":any_skip_relocation"}, {"url", "u5"}, {"sha256", "s5"}}}}}}}}}}});
    return f.dump();
}

Package::Archive fixed_archive(const std::string& cellar, const std::string& url = "u",
                               const std::string& sha = std::string(64, 'f')) {
    Package::Archive ar;
    ar.url = url;
    ar.sha256 = sha;
    ar.relocation = RelocationType::HardcodedPrefix;
    ar.cellar = cellar;
    return ar;
}

Config tmp_config(const fs::path& tmp, const fs::path& store) {
    Config cfg;
    cfg.den_home = tmp / "den";
    cfg.cache = cfg.den_home / "cache";
    cfg.prefix = store.parent_path();
    cfg.store = store;
    cfg.homebrew_cellar = store;
    fs::create_directories(cfg.cache / "archives");
    fs::create_directories(store);
    return cfg;
}

fs::path fresh_tmp(const std::string& tag) {
    auto tmp = fs::temp_directory_path() / ("den-" + tag + "-" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    return tmp;
}

} // namespace

TEST_SUITE("bottle::fixed_cellar") {

    TEST_CASE("index records each bottle's fixed cellar and leaves relocatable ones empty") {
        auto idx = transform_homebrew(fixture_formula_json(), "[]");
        const Package* p = idx.find("fixedprobe");
        REQUIRE(p != nullptr);
        CHECK(p->archives.at("arm64_sequoia").relocation == RelocationType::HardcodedPrefix);
        CHECK(p->archives.at("arm64_sequoia").cellar == "/opt/homebrew/Cellar");
        CHECK(p->archives.at("sonoma").cellar == "/usr/local/Cellar");
        CHECK(p->archives.at("x86_64_linux").cellar == "/home/linuxbrew/.linuxbrew/Cellar");
        CHECK(p->archives.at("arm64_sonoma").relocation == RelocationType::NeedsSubstitution);
        CHECK(p->archives.at("arm64_sonoma").cellar.empty());
        CHECK(p->archives.at("arm64_ventura").relocation == RelocationType::Portable);
        CHECK(p->archives.at("arm64_ventura").cellar.empty());

        // The fixed cellar survives a save/load round trip of the JSON index.
        auto tmp = fresh_tmp("fixed-cellar-index");
        save_index(idx, tmp / "index.json");
        auto back = load_index_json(tmp / "index.json");
        CHECK(back.find("fixedprobe")->archives.at("arm64_sequoia").cellar ==
              "/opt/homebrew/Cellar");
        CHECK(back.find("fixedprobe")->archives.at("arm64_sonoma").cellar.empty());
        fs::remove_all(tmp);
    }

    TEST_CASE("required cellar: relocatable bottles pour anywhere, fixed ones only into theirs") {
        Package::Archive any;
        any.relocation = RelocationType::NeedsSubstitution;
        Package::Archive skip;
        skip.relocation = RelocationType::Portable;
        CHECK(required_bottle_cellar("arm64_sequoia", any).empty());
        CHECK(required_bottle_cellar("arm64_sequoia", skip).empty());
        CHECK(bottle_pours_into("arm64_sequoia", any, "/Users/x/.den/prefix/Cellar"));
        CHECK(bottle_pours_into("arm64_sequoia", skip, "/Users/x/.den/prefix/Cellar"));

        auto fixed = fixed_archive("/opt/homebrew/Cellar");
        CHECK(required_bottle_cellar("arm64_sequoia", fixed) == "/opt/homebrew/Cellar");
        CHECK_FALSE(bottle_pours_into("arm64_sequoia", fixed, "/Users/x/.den/prefix/Cellar"));
        CHECK(bottle_pours_into("arm64_sequoia", fixed, "/opt/homebrew/Cellar"));
        CHECK(bottle_pours_into("arm64_sequoia", fixed, "/opt/homebrew/Cellar/"));

        // Index entries from before den recorded the path fall back to the
        // standard Cellar for the tag.
        auto legacy = fixed_archive("");
        CHECK(required_bottle_cellar("arm64_sequoia", legacy) == "/opt/homebrew/Cellar");
        CHECK(required_bottle_cellar("sonoma", legacy) == "/usr/local/Cellar");
        CHECK(required_bottle_cellar("x86_64_linux", legacy) ==
              "/home/linuxbrew/.linuxbrew/Cellar");
        CHECK(required_bottle_cellar("arm64_linux", legacy) == "/home/linuxbrew/.linuxbrew/Cellar");
    }

    TEST_CASE("install builds a fixed-cellar keg for another Cellar from source, else refuses") {
        auto tmp = fresh_tmp("fixed-cellar-refuse");
        auto cfg = tmp_config(tmp, tmp / "prefix" / "Cellar");

        PackageIndex idx;
        Package p;
        p.name = "fixedprobe";
        p.version = "2.0";
        p.source_url = "https://example.invalid/fixedprobe-2.0.tar.gz";
        p.archives["default"] =
            fixed_archive("/opt/homebrew/Cellar", "https://bottles.invalid/fixedprobe.tar.gz");
        idx.packages["fixedprobe"] = p;
        Package q = p;
        q.name = "nosource";
        q.source_url.reset();
        idx.packages["nosource"] = q;

        std::vector<std::string> built;
        SourcePlanRunner runner;
        runner.pour = [](const Config&, const Package& pkg) {
            FAIL("unexpected pour of " << pkg.name);
        };
        runner.build = [&](const Config& c, const PackageIndex&, const std::string& n,
                           const std::string& v) {
            built.push_back(n);
            fs::create_directories(c.store / n / v);
            return c.store / n / v;
        };
        set_source_plan_runner_for_testing(&runner);

        HomebrewProvider provider(&idx);
        std::string msg;
        try {
            provider.install(cfg, "fixedprobe", "");
        } catch (const UserError& e) {
            msg = e.what();
        }
        CHECK_MESSAGE(msg.empty(), msg);
        CHECK(built == std::vector<std::string>{"fixedprobe"});
        // The bottle it can't pour was never fetched.
        CHECK(fs::is_empty(cfg.cache / "archives"));
        CHECK_FALSE(prefetch_bottle(cfg, p).has_value());

        std::string msg2;
        try {
            provider.install(cfg, "nosource", "");
        } catch (const UserError& e) {
            msg2 = e.what();
        }
        set_source_plan_runner_for_testing(nullptr);
        CHECK(msg2.find("nosource 2.0") != std::string::npos);
        CHECK(msg2.find("/opt/homebrew/Cellar") != std::string::npos);
        CHECK(msg2.find(cfg.store.string()) != std::string::npos);
        CHECK(msg2.find("No source build is available for nosource") != std::string::npos);
        // Refused before download: no keg, no archive fetched, no opt link.
        CHECK_FALSE(fs::exists(cfg.store / "nosource"));
        CHECK(fs::is_empty(cfg.cache / "archives"));
        CHECK_FALSE(fs::exists(cfg.prefix / "opt" / "nosource"));
        CHECK(built.size() == 1);
        fs::remove_all(tmp);
    }

    TEST_CASE("a fixed-cellar bottle pours when den's Cellar is the one it was built for") {
        auto tmp = fresh_tmp("fixed-cellar-match");
        auto store = tmp / "prefix" / "Cellar";
        auto stage = tmp / "stage";
        fs::create_directories(stage / "fixedprobe" / "2.0" / "bin");
        std::ofstream(stage / "fixedprobe" / "2.0" / "bin" / "fixedprobe")
            << "#!/bin/sh\necho ok\n";
        auto tarball = tmp / "fixedprobe.tar.gz";
        REQUIRE(run_tool({"tar", "-czf", tarball.string(), "-C", stage.string(), "fixedprobe"})
                    .exit_code == 0);
        const auto sha = hash_file(tarball);

        auto cfg = tmp_config(tmp, store);
        fs::copy_file(tarball, cfg.cache / "archives" / (sha + ".tar.gz"));
        PackageIndex idx;
        Package p;
        p.name = "fixedprobe";
        p.version = "2.0";
        p.archives["default"] =
            fixed_archive(store.string(), "https://bottles.invalid/fixedprobe.tar.gz", sha);
        idx.packages["fixedprobe"] = p;

        HomebrewProvider provider(&idx);
        provider.install(cfg, "fixedprobe", "");
        CHECK(fs::exists(store / "fixedprobe" / "2.0" / "bin" / "fixedprobe"));
        fs::remove_all(tmp);
    }

    TEST_CASE("doctor lists kegs whose bottle was built for a different fixed Cellar") {
        auto tmp = fresh_tmp("fixed-cellar-doctor");
        auto store = tmp / "Cellar";
        fs::create_directories(store / "fixedprobe" / "2.0");
        fs::create_directories(store / "anyprobe" / "1.0");
        fs::create_directories(store / "unindexed" / "1.0");

        PackageIndex idx;
        Package f;
        f.name = "fixedprobe";
        f.version = "2.0";
        f.archives["all"] = fixed_archive("/opt/homebrew/Cellar");
        idx.packages["fixedprobe"] = f;
        Package a;
        a.name = "anyprobe";
        a.version = "1.0";
        Package::Archive any;
        any.relocation = RelocationType::NeedsSubstitution;
        a.archives["all"] = any;
        idx.packages["anyprobe"] = a;

        auto kegs = kegs_with_mismatched_cellar(store, idx, Arch::Arm64, std::nullopt);
        REQUIRE(kegs.size() == 1);
        CHECK(kegs[0] == "fixedprobe/2.0 (bottle Cellar /opt/homebrew/Cellar)");

        // In the Cellar the bottle was built for, nothing is mismatched.
        idx.packages["fixedprobe"].archives["all"].cellar = store.string();
        CHECK(kegs_with_mismatched_cellar(store, idx, Arch::Arm64, std::nullopt).empty());
        fs::remove_all(tmp);
    }

    TEST_CASE("install_name_tool header-space failures are recognised") {
        CHECK(install_name_tool_out_of_header_space(
            "error: install_name_tool: changing install names or rpaths can't be redone for: "
            "/x/libfoo.dylib (for architecture arm64) because larger updated load commands do "
            "not fit (the program must be relinked, and you may need to use -headerpad or "
            "-headerpad_max_install_names)"));
        CHECK_FALSE(install_name_tool_out_of_header_space("error: no such file"));
    }

#ifdef __APPLE__
    TEST_CASE("running out of Mach-O header space is a hard error and the keg is rolled back") {
        auto tmp = fresh_tmp("headerpad");
        // A long Cellar: every rewritten rpath grows by ~(len - 19) bytes.
        auto store = tmp / std::string(200, 'p') / std::string(200, 'q') / "Cellar";
        auto stage = tmp / "stage";
        auto lib = stage / "padprobe" / "1.0" / "lib";
        fs::create_directories(lib);
        {
            std::ofstream(tmp / "pad.c") << "int padprobe(void) { return 42; }\n";
        }
        std::vector<std::string> cc = {"clang",
                                       "-dynamiclib",
                                       "-o",
                                       (lib / "libpadprobe.dylib").string(),
                                       (tmp / "pad.c").string(),
                                       "-Wl,-headerpad,0",
                                       "-install_name",
                                       "@@HOMEBREW_CELLAR@@/padprobe/1.0/lib/libpadprobe.dylib"};
        // Enough placeholder rpaths that the relocated load commands can't
        // fit in a 16 KiB __TEXT page, though the originals easily do.
        for (int i = 0; i < 80; ++i)
            cc.push_back("-Wl,-rpath,@@HOMEBREW_CELLAR@@/padprobe/1.0/lib/r" + std::to_string(i));
        auto built = run_tool(cc);
        REQUIRE_MESSAGE(built.exit_code == 0, built.output);

        auto tarball = tmp / "padprobe.tar.gz";
        REQUIRE(run_tool({"tar", "-czf", tarball.string(), "-C", stage.string(), "padprobe"})
                    .exit_code == 0);
        const auto sha = hash_file(tarball);

        auto cfg = tmp_config(tmp, store);
        fs::copy_file(tarball, cfg.cache / "archives" / (sha + ".tar.gz"));
        PackageIndex idx;
        Package p;
        p.name = "padprobe";
        p.version = "1.0";
        Package::Archive ar;
        ar.url = "https://bottles.invalid/padprobe.tar.gz";
        ar.sha256 = sha;
        ar.relocation = RelocationType::NeedsSubstitution; // :any
        p.archives["default"] = ar;
        idx.packages["padprobe"] = p;

        HomebrewProvider provider(&idx);
        std::string msg;
        try {
            provider.install(cfg, "padprobe", "");
        } catch (const UserError& e) {
            msg = e.what();
        }
        REQUIRE_FALSE(msg.empty());
        CHECK(msg.find("header space") != std::string::npos);
        CHECK(msg.find("libpadprobe.dylib") != std::string::npos);
        // Never a half-relocated keg: the keg and opt link are gone.
        CHECK_FALSE(fs::exists(store / "padprobe" / "1.0"));
        CHECK_FALSE(fs::exists(cfg.prefix / "opt" / "padprobe"));
        fs::remove_all(tmp);
    }

#endif
}
