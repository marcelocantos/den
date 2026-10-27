// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0

// brew parity: `den uninstall X` refuses while installed packages in any
// environment need X at runtime; --ignore-dependencies overrides; build-only
// dependencies don't count.

#include <doctest.h>

#include "cli/uninstall_deps.h"
#include "env/manifest.h"
#include "index/index.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <sys/wait.h>

namespace den {
namespace test {

namespace fs = std::filesystem;

namespace {

struct DepsTmp {
    fs::path path;
    DepsTmp() {
        path = fs::temp_directory_path() /
               ("den_test_uninstall_deps_" +
                std::to_string(static_cast<size_t>(
                    std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(path);
    }
    ~DepsTmp() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    DepsTmp(const DepsTmp&) = delete;
    DepsTmp& operator=(const DepsTmp&) = delete;
};

struct DepsFx {
    DepsTmp tmp;
    Config cfg;
    PackageIndex idx;

    DepsFx() {
        cfg.den_home = tmp.path / "den";
        cfg.cache = cfg.den_home / "cache";
        cfg.store = tmp.path / "prefix" / "Cellar";
        fs::create_directories(cfg.store);
    }

    void pkg(const std::string& name, std::vector<std::string> deps = {},
             std::vector<std::string> build_deps = {}) {
        Package p;
        p.name = name;
        p.version = "1.0";
        p.dependencies = std::move(deps);
        p.build_dependencies = std::move(build_deps);
        idx.packages[name] = p;
    }

    void env(const std::string& path, const std::vector<std::string>& names,
             const std::vector<std::string>& auto_deps = {}) {
        Manifest m;
        for (const auto& n : names) {
            m.packages["homebrew"][n] = "1.0";
        }
        for (const auto& n : auto_deps) {
            m.auto_deps["homebrew"].insert(n);
        }
        write_manifest(cfg.den_home, path, m);
    }

    std::map<std::string, std::vector<RuntimeDependent>>
    check(const std::vector<std::string>& names, const std::string& active = "/") {
        return find_runtime_dependents(cfg, idx, active, names);
    }
};

struct Run {
    std::string out;
    int code;
};

Run run_den(const std::string& args, const fs::path& root) {
    const auto prefix = (root / "prefix").string();
    std::string cmd = "HOME=" + root.string() + " DEN_HOME=" + (root / "den").string() +
                      " DEN_PREFIX=" + prefix + " HOMEBREW_PREFIX=" + prefix +
                      " HOMEBREW_CELLAR=" + prefix + "/Cellar ./den " + args + " 2>&1";
    FILE* pipe = ::popen(cmd.c_str(), "r");
    REQUIRE(pipe != nullptr);
    std::string out;
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe)) {
        out += buf.data();
    }
    int raw = ::pclose(pipe);
    return {out, WIFEXITED(raw) ? WEXITSTATUS(raw) : -1};
}

} // namespace

TEST_SUITE("uninstall::dependents") {

    TEST_CASE("a runtime dependent in the same env is reported") {
        DepsFx fx;
        fx.pkg("libfoo");
        fx.pkg("app", {"libfoo"});
        fx.env("/", {"app", "libfoo"}, {"libfoo"});
        auto d = fx.check({"libfoo"});
        REQUIRE(d.count("libfoo"));
        REQUIRE(d["libfoo"].size() == 1);
        CHECK(d["libfoo"][0].name == "app");
        CHECK(d["libfoo"][0].env == "/");
        CHECK(fx.check({"app"}).empty()); // nothing needs app
    }

    TEST_CASE("transitive runtime dependents count") {
        DepsFx fx;
        fx.pkg("libfoo");
        fx.pkg("mid", {"libfoo"});
        fx.pkg("app", {"mid"});
        fx.env("/", {"app"}); // mid/libfoo missing from the manifest
        auto d = fx.check({"libfoo"});
        REQUIRE(d.count("libfoo"));
        CHECK(d["libfoo"][0].name == "app");
    }

    TEST_CASE("a dependent in another env blocks the uninstall") {
        DepsFx fx;
        fx.pkg("libfoo");
        fx.pkg("app", {"libfoo"});
        fx.env("/", {"libfoo"});
        fx.env("/work", {"app"}); // inherits libfoo from the root env
        auto d = fx.check({"libfoo"}, "/");
        REQUIRE(d.count("libfoo"));
        REQUIRE(d["libfoo"].size() == 1);
        CHECK(d["libfoo"][0].name == "app");
        CHECK(d["libfoo"][0].env == "/work");
    }

    TEST_CASE("a dependent whose env still provides the package does not block") {
        DepsFx fx;
        fx.pkg("libfoo");
        fx.pkg("app", {"libfoo"});
        fx.env("/", {"libfoo"});
        fx.env("/work", {"app", "libfoo"}); // its own libfoo stays
        CHECK(fx.check({"libfoo"}, "/").empty());
    }

    TEST_CASE("build-only dependencies don't count") {
        DepsFx fx;
        fx.pkg("cmake");
        fx.pkg("libfoo", {}, {"cmake"});
        fx.pkg("app", {"libfoo"}, {"cmake"});
        fx.env("/", {"app", "libfoo", "cmake"});
        CHECK(fx.check({"cmake"}).empty());
        // ...and are not followed for transitive runtime deps either.
        fx.pkg("tool", {}, {"app"});
        fx.env("/", {"tool", "app", "libfoo"});
        CHECK(fx.check({"app"}).empty());
    }

    TEST_CASE("packages removed together don't block each other") {
        DepsFx fx;
        fx.pkg("libfoo");
        fx.pkg("app", {"libfoo"});
        fx.env("/", {"app", "libfoo"});
        CHECK(fx.check({"libfoo", "app"}).empty());
    }

    TEST_CASE("keg receipt runtime deps are used when the index lacks the dependent") {
        DepsFx fx;
        fx.pkg("libfoo");
        fx.env("/", {"legacy", "libfoo"});
        const auto keg = fx.cfg.store / "legacy" / "1.0";
        fs::create_directories(keg);
        std::ofstream(keg / "INSTALL_RECEIPT.json")
            << R"({"runtime_dependencies":[{"full_name":"libfoo","version":"1.0"}]})";
        auto d = fx.check({"libfoo"});
        REQUIRE(d.count("libfoo"));
        CHECK(d["libfoo"][0].name == "legacy");
    }

    TEST_CASE("refusal message lists dependents and the override") {
        std::map<std::string, std::vector<RuntimeDependent>> d;
        d["libfoo"] = {{"/", "app"}, {"/work", "tool"}};
        auto msg = format_dependents_refusal(d);
        CHECK(msg.find("Refusing to uninstall libfoo") != std::string::npos);
        CHECK(msg.find("app (environment /)") != std::string::npos);
        CHECK(msg.find("tool (environment /work)") != std::string::npos);
        CHECK(msg.find("den uninstall --ignore-dependencies libfoo") != std::string::npos);
    }
}

TEST_SUITE("uninstall::dependents::cli") {

    TEST_CASE("den uninstall refuses a runtime dependency; --ignore-dependencies overrides") {
        if (!fs::exists("./den")) {
            MESSAGE("den binary not in cwd — skipping");
            return;
        }
        DepsFx fx;
        fx.pkg("libfoo");
        fx.pkg("cmake");
        fx.pkg("app", {"libfoo"}, {"cmake"});
        save_index(fx.idx, fx.cfg.cache / "index.json");
        fx.env("/", {"libfoo", "cmake"});
        fx.env("/work", {"app"});

        auto r = run_den("uninstall libfoo", fx.tmp.path);
        INFO(r.out);
        CHECK(r.code != 0);
        CHECK(r.out.find("Refusing to uninstall libfoo") != std::string::npos);
        CHECK(r.out.find("app (environment /work)") != std::string::npos);
        CHECK(read_manifest(fx.cfg.den_home, "/").packages["homebrew"].count("libfoo") == 1);

        // A build-only dependency doesn't block.
        r = run_den("uninstall cmake", fx.tmp.path);
        INFO(r.out);
        CHECK(r.code == 0);
        CHECK(read_manifest(fx.cfg.den_home, "/").packages["homebrew"].count("cmake") == 0);

        r = run_den("uninstall --ignore-dependencies libfoo", fx.tmp.path);
        INFO(r.out);
        CHECK(r.code == 0);
        CHECK(read_manifest(fx.cfg.den_home, "/").packages["homebrew"].count("libfoo") == 0);
    }
}

} // namespace test
} // namespace den
